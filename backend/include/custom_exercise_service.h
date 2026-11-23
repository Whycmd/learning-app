// 职责：定制练习（v1.5.0-T006）
// 链路：组题条 → 24h 同参复用 → 取词（词表索引 / 在学词 / 主题）→ L2 任务 → AI 出题
//       客观题本地秒批，主观题 AI 批改失败降级参考答案；全程不写 knowledge_points / user_learning
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "ai_client.h"
#include "ai_queue_service.h"
#include "article_excerpt.h"
#include "db.h"
#include "exercise_sanitize.h"
#include "json.h"
#include "prompts.h"

namespace eng {

class CustomExerciseService
{
public:
  struct Submit {
    bool ok=false;
    int httpStatus=200;
    std::string error;
    int64_t id=0, taskId=0;
    bool reused=false;
    int queueLevel=0, position=0, etaSec=0;
  };

  struct View {
    int64_t id=0;
    int userId=0;
    std::string status, specJson, difficulty, style, exerciseJson, gradeJson, error, createdAt;
    int itemCount=0, queueLevel=0, position=0, etaSec=0;
    int64_t taskId=0;
  };

  struct GradeResult {
    bool ok=false;
    int httpStatus=200;
    std::string error, gradeJson;
    int score=0;
  };

  CustomExerciseService(Db& db, AiClient& ai, AiQueueService& queue, const std::string& root,
                        const std::string& wordTmp)
      : db_(db), ai_(ai), queue_(queue), root_(root), wordTmpDir_(wordTmp) { loadQuotaConfig(); }

  Submit submit(int uid, const std::string& guestId, const JsonValue& spec, int queueLevel)
  {
    Submit r;
    if (!spec.isObject()) { r.httpStatus = 400; r.error = "组题条格式不正确"; return r; }
    // 1) 题量与题型
    int total = 0, objCount = 0;
    std::string countsNote;
    std::string typesMode = spec.getString("types_mode", "counts");
    bool autoTypes = (typesMode == "auto");
    if (autoTypes) {
      total = (int)spec.getInt("total", 10);
      if (total <= 0 || total > 100) { r.httpStatus = 400; r.error = "总题数需在 1-100"; return r; }
      countsNote = "不限题型，共 " + std::to_string(total) + " 题";
      objCount = total;
    } else if (auto* c = spec.find("counts")) {
      if (c->isObject()) {
        for (auto& kv : c->asObject()) {
          std::string t = kv.first;
          int n = (int)kv.second.asInt();
          if (!isKnownType(t)) { r.httpStatus = 400; r.error = "未知题型：" + t; return r; }
          if (n < 0 || n > 100) { r.httpStatus = 400; r.error = "单题型数量需在 0-100"; return r; }
          total += n;
          if (isObjective(t)) objCount += n;
          if (n > 0) countsNote += t + ":" + std::to_string(n) + " ";
        }
      }
    }
    int cap = uid > 0 ? 100 : 30;
    if (total <= 0) { r.httpStatus = 400; r.error = "请至少选择 1 道题"; return r; }
    if (total > cap) { r.httpStatus = 400; r.error = "题量超出上限（游客 30 / 登录 100）"; return r; }
    std::string difficulty = spec.getString("difficulty", "basic");
    if (difficulty != "basic" && difficulty != "advanced" && difficulty != "challenge") difficulty = "basic";
    std::string style = spec.getString("style", "exam");
    if (style != "exam" && style != "situational" && style != "story" && style != "academic") style = "exam";
    std::string extra = spec.getString("extra_requirements", "");
    if (extra.size() > 200) extra = extra.substr(0, 200);

    // 2) 组题条规范化 + 哈希
    std::string specJson = spec.dump();
    JsonValue canon = parseJson(specJson);
    std::string canonical = canon.dump();
    std::string hash = fnvHex(canonical);

    // 3) 24h 同参复用（仅登录用户）
    if (uid > 0) {
      auto rows = db_.query(
          "SELECT id,task_id FROM custom_exercises WHERE user_id=" + std::to_string(uid) +
          " AND spec_hash=" + q(hash) + " AND status='done'"
          " AND created_at > DATE_SUB(NOW(), INTERVAL 24 HOUR) ORDER BY id DESC LIMIT 1");
      if (!rows.empty()) {
        r.ok = true; r.reused = true;
        r.id = std::stoll(rows[0].at("id"));
        r.taskId = rows[0].count("task_id") && !rows[0].at("task_id").empty() ? std::stoll(rows[0].at("task_id")) : 0;
        return r;
      }
    }

    // 4) 限额
    if (uid > 0) {
      auto rows = db_.query("SELECT calls FROM ai_usage_daily WHERE user_id=" + std::to_string(uid) +
                            " AND day=CURDATE() AND kind='custom_exercise'");
      int calls = rows.empty() ? 0 : std::stoi(rows[0].at("calls"));
      if (calls >= userCallLimit_) queueLevel = std::max(queueLevel, 1);
    } else {
      if (guestId.empty()) { r.httpStatus = 400; r.error = "缺少游客标识"; return r; }
      auto w = db_.exec("INSERT INTO guest_trials (guest_id,feature,day,used) VALUES (" + q(guestId) +
                        ",'custom_exercise',CURDATE(),1) ON DUPLICATE KEY UPDATE used=IF(used<" +
                        std::to_string(guestExerciseLimit_) + ",used+1,used)");
      if (w.affected_rows <= 0) {
        r.httpStatus = 401;
        r.error = "游客今日定制练习次数已用完，登录后可继续使用";
        return r;
      }
    }

    // 5) 落库 + 建任务
    std::string gid = uid > 0 ? "NULL" : q(guestId);
    auto w = db_.exec(
        "INSERT INTO custom_exercises (user_id,guest_id,spec_json,spec_hash,status,item_count,difficulty,style) VALUES (" +
        std::to_string(uid) + "," + gid + "," + q(specJson) + "," + q(hash) + ",'queued'," +
        std::to_string(total) + "," + q(difficulty) + "," + q(style) + ")");
    int64_t id = w.last_insert_id;
    if (id <= 0) { r.httpStatus = 500; r.error = "创建练习失败"; return r; }

    std::vector<AiQueueService::NewItem> items;
    items.push_back(AiQueueService::NewItem{id, "custom_exercise"});
    std::string payload = "{\"exercise_id\":" + std::to_string(id) + ",\"spec\":" + q(payloadSafe(specJson)) + "}";
    auto c = queue_.create(uid, "custom_exercise", "custom_exercise", items, payload, queueLevel);
    if (!c.ok) {
      db_.exec("UPDATE custom_exercises SET status='failed', error_msg=" + q(c.error) + " WHERE id=" + std::to_string(id));
      r.httpStatus = c.httpStatus ? c.httpStatus : 500;
      r.error = c.error.empty() ? "创建 AI 任务失败" : c.error;
      return r;
    }
    db_.exec("UPDATE custom_exercises SET task_id=" + std::to_string(c.taskId) + " WHERE id=" + std::to_string(id));
    if (uid > 0) bumpUser(uid);
    AiTaskView tv;
    if (queue_.get(c.taskId, uid, tv)) { r.position = tv.position; r.etaSec = tv.etaSec; }
    r.ok = true; r.id = id; r.taskId = c.taskId; r.queueLevel = queueLevel;
    (void)objCount; (void)countsNote; (void)extra;
    return r;
  }

  bool get(int64_t id, int uid, const std::string& guestId, View& out)
  {
    auto rows = db_.query(
        "SELECT id,user_id,status,spec_json,difficulty,style,exercise_json,grade_json,error_msg,created_at,"
        "item_count,task_id FROM custom_exercises WHERE id=" + std::to_string(id));
    if (rows.empty()) return false;
    auto& row = rows[0];
    int owner = std::stoi(row.at("user_id"));
    if (uid > 0) {
      if (owner != uid) return false;
    } else {
      auto g = db_.query("SELECT guest_id FROM custom_exercises WHERE id=" + std::to_string(id));
      if (g.empty() || g[0].count("guest_id") == 0 || g[0].at("guest_id") != guestId) return false;
    }
    out.id = (int64_t)std::stoll(row.at("id"));
    out.userId = owner;
    out.status = row.at("status");
    out.specJson = row.at("spec_json");
    out.difficulty = row.at("difficulty");
    out.style = row.at("style");
    out.exerciseJson = row.count("exercise_json") ? row.at("exercise_json") : "";
    out.gradeJson = row.count("grade_json") ? row.at("grade_json") : "";
    out.error = row.count("error_msg") ? row.at("error_msg") : "";
    out.createdAt = row.at("created_at");
    out.itemCount = std::stoi(row.at("item_count"));
    out.taskId = row.count("task_id") && !row.at("task_id").empty() ? std::stoll(row.at("task_id")) : 0;
    if (out.status == "queued" && out.taskId > 0) {
      AiTaskView tv;
      if (queue_.get(out.taskId, uid, tv)) {
        out.position = tv.position; out.etaSec = tv.etaSec; out.queueLevel = tv.queueLevel;
        if (tv.status == "done") {
          auto again = db_.query("SELECT status,exercise_json FROM custom_exercises WHERE id=" + std::to_string(id));
          if (!again.empty()) {
            out.status = again[0].at("status");
            out.exerciseJson = again[0].count("exercise_json") ? again[0].at("exercise_json") : "";
          }
        } else if (tv.status == "failed" || tv.status == "canceled") {
          // 任务失败/取消时回写，避免前端无限轮询「AI 出题中」
          std::string em = tv.error.empty() ? "AI 生成失败，可重试" : tv.error;
          db_.exec("UPDATE custom_exercises SET status='failed', error_msg=" + q(em.substr(0, 240)) +
                   " WHERE id=" + std::to_string(id) + " AND status IN ('queued','running')");
          out.status = "failed";
          out.error = em;
        }
      }
    }
    return true;
  }

  // v1.5.0-T009 D08/D12：定制练习按需导出空白卷。
  // latestOnly=true 时（游客当次导出）仅允许该游客最新一条 done，否则 403「只能导出当前结果」。
  struct ExportResult {
    bool ok=false;
    int httpStatus=200;
    std::string error, path;
  };

  ExportResult exportWord(int64_t id, int uid, const std::string& guestId, bool latestOnly)
  {
    ExportResult r;
    View v;
    if (!get(id, uid, guestId, v)) { r.httpStatus = 404; r.error = "练习不存在"; return r; }
    if (v.status != "done") { r.httpStatus = 409; r.error = "练习尚未生成完成"; return r; }
    if (latestOnly && v.userId <= 0) {
      auto rows = db_.query("SELECT MAX(id) AS m FROM custom_exercises WHERE guest_id='" +
                            Db::escape(guestId) + "' AND status='done'");
      int64_t latestId = 0;
      if (!rows.empty() && !rows[0].at("m").empty()) {
        try { latestId = std::stoll(rows[0].at("m")); } catch (...) {}
      }
      if (latestId != id) { r.httpStatus = 403; r.error = "只能导出当前结果"; return r; }
    }
    JsonValue ex = parseJson(v.exerciseJson);
    const JsonValue* items = ex.find("items");
    if (!items || !items->isArray() || items->asArray().empty()) {
      r.httpStatus = 500; r.error = "练习数据异常"; return r;
    }

    JsonObject payload;
    payload["kind"] = JsonValue::makeString("custom");
    payload["id"] = JsonValue::makeInt(id);
    payload["title"] = JsonValue::makeString("定制练习卷");
    payload["difficulty"] = JsonValue::makeString(v.difficulty);
    payload["style"] = JsonValue::makeString(v.style);
    if (!v.specJson.empty()) {
      try { payload["spec"] = parseJson(v.specJson); } catch (...) {}
    }
    payload["exercises"] = *items;

    std::string tmpPath = wordTmpDir_ + "/custom_paper_" + std::to_string(id) + "_" +
                          std::to_string((int64_t)::time(nullptr)) + ".json";
    {
      std::ofstream f(tmpPath);
      if (!f) { r.httpStatus = 500; r.error = "导出临时文件写入失败"; return r; }
      f << JsonValue::makeObject(payload).dump();
    }
    const char* nodeEnv = std::getenv("NODE_BIN");
    std::string nodeBin = nodeEnv ? nodeEnv : "node";
    std::string cmd = nodeBin + " " + root_ + "/scripts/gen_word_exercise.js " +
                      std::to_string(id) + " " + tmpPath + " " + wordTmpDir_;
    int rc = std::system(cmd.c_str());
    if (rc != 0) { r.httpStatus = 500; r.error = "导出服务暂不可用"; return r; }

    std::string outPath = wordTmpDir_ + "/custom_exercise_" + std::to_string(id) + ".docx";
    if (::access(outPath.c_str(), F_OK) != 0) {
      r.httpStatus = 500; r.error = "导出服务暂不可用"; return r;
    }
    r.ok = true;
    r.path = outPath;
    return r;
  }

  std::vector<View> history(int uid, int limit, int offset)
  {
    std::vector<View> out;
    if (uid <= 0) return out;
    auto rows = db_.query(
        "SELECT c.id,c.user_id,c.status,c.spec_json,c.difficulty,c.style,c.error_msg,c.created_at,c.item_count,c.task_id,"
        "SUBSTRING(IFNULL(c.grade_json,''),1,400) AS grade_json,t.status AS task_status,t.error_msg AS task_error "
        "FROM custom_exercises c LEFT JOIN ai_tasks t ON t.id=c.task_id WHERE c.user_id=" +
        std::to_string(uid) + " ORDER BY c.id DESC LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset));
    for (auto& row : rows) {
      View v;
      v.id = (int64_t)std::stoll(row.at("id"));
      v.userId = std::stoi(row.at("user_id"));
      v.status = row.at("status");
      v.specJson = row.at("spec_json");
      v.difficulty = row.at("difficulty");
      v.style = row.at("style");
      v.gradeJson = row.count("grade_json") ? row.at("grade_json") : "";
      v.error = row.count("error_msg") ? row.at("error_msg") : "";
      v.createdAt = row.at("created_at");
      v.itemCount = std::stoi(row.at("item_count"));
      v.taskId = row.count("task_id") && !row.at("task_id").empty() ? std::stoll(row.at("task_id")) : 0;
      if (v.status == "queued" && v.taskId > 0) {
        std::string ts = row.count("task_status") ? row.at("task_status") : "";
        if (ts == "failed" || ts == "canceled") {
          std::string em = row.count("task_error") && !row.at("task_error").empty()
                               ? row.at("task_error") : "AI 生成失败，可重试";
          db_.exec("UPDATE custom_exercises SET status='failed', error_msg=" + q(em.substr(0, 240)) +
                   " WHERE id=" + std::to_string(v.id) + " AND status IN ('queued','running')");
          v.status = "failed";
          v.error = em;
        }
      }
      out.push_back(v);
    }
    return out;
  }

  // 队列 runner：kind=custom_exercise
  bool runTask(int uid, int64_t taskId)
  {
    auto items = db_.query("SELECT id,ref_id FROM ai_task_items WHERE task_id=" + std::to_string(taskId) +
                           " AND status='pending' ORDER BY item_index, id");
    bool allOk = true;
    for (auto& it : items) {
      int64_t itemId = std::stoll(it.at("id"));
      int64_t eid = it.count("ref_id") && !it.at("ref_id").empty() ? std::stoll(it.at("ref_id")) : 0;
      db_.exec("UPDATE ai_task_items SET status='running', started_at=NOW() WHERE id=" + std::to_string(itemId));
      auto rows = db_.query("SELECT user_id,spec_json,item_count FROM custom_exercises WHERE id=" + std::to_string(eid));
      if (rows.empty()) { failItem(itemId, eid, "练习记录不存在"); allOk = false; continue; }
      int owner = std::stoi(rows[0].at("user_id"));
      JsonValue spec = parseJson(rows[0].at("spec_json"));
      int total = std::stoi(rows[0].at("item_count"));
      std::string pool = poolFor(owner, spec);
      std::string identity = prompts::getLearnerIdentity(db_, owner);
      std::string typesMode = spec.getString("types_mode", "counts");
      std::string typeModeRule = (typesMode == "auto")
          ? "题型不限：请按考试常见题型自行搭配，至少包含 2 种题型，总题数按组题要求执行。"
          : "严格按组题要求中的题型与数量出题，不得擅自增加、减少或替换题型。";
      std::string articleExcerpt = pickArticleExcerpt(db_, owner);
      if (articleExcerpt.empty()) articleExcerpt = "（站内暂无可用文章节选，请按下方兜底规则自选语篇。）";

      // 长语篇题型预留名额：主请求只出其余题型，避免主请求占满题量
      int longReserve = 0;
      JsonValue mainSpec = spec;
      if (typesMode != "auto") {
        auto* c = spec.find("counts");
        if (c && c->isObject()) {
          longReserve = (int)c->getInt("long_reading_cloze", 0) + (int)c->getInt("full_cloze", 0);
          if (longReserve > 0) {
            JsonObject c2 = c->asObject();
            c2.erase("long_reading_cloze");
            c2.erase("full_cloze");
            JsonObject s2 = spec.asObject();
            s2["counts"] = JsonValue::makeObject(c2);
            mainSpec = JsonValue::makeObject(s2);
          }
        }
      }
      int mainTarget = total - longReserve;
      if (mainTarget < 0) mainTarget = 0;

      JsonArray gotItems;
      bool mainFailed = false;
      if (mainTarget > 0) {
        std::string specText = specTextOf(mainSpec);
        std::string user = prompts::buildTemplate(prompts::CUSTOM_EXERCISE_USER,
            {{"{SPEC}", specText}, {"{TYPE_MODE_RULE}", typeModeRule}, {"{POOL}", pool},
             {"{LEARNER_IDENTITY}", identity}, {"{ARTICLE_EXCERPT}", articleExcerpt}});
        std::vector<std::map<std::string, std::string>> msgs = {
            {{"role", "system"}, {"content", prompts::CUSTOM_EXERCISE_SYSTEM}},
            {{"role", "user"}, {"content", user}}};
        auto resp = ai_.postChat(msgs, true, 4096, 0.6, "custom_exercise");
        if (!resp.ok) {
          mainFailed = true;
        } else {
          try {
            JsonValue j = parseJson(unwrapJson(aiText(resp)));
            const JsonValue* arr = j.find("items");
            if (!arr || !arr->isArray() || arr->asArray().empty()) {
              mainFailed = true;
            } else {
              JsonValue norm = normalizeItems(arr->asArray(), mainTarget);
              gotItems = norm.find("items")->asArray();
              // 短题无效丢弃后按缺失题型/数量补生成 1 轮
              if ((int)gotItems.size() < mainTarget) {
                std::string missing = missingSpecText(mainSpec, gotItems, mainTarget);
                if (!missing.empty()) {
                  std::string user2 = prompts::buildTemplate(prompts::CUSTOM_EXERCISE_USER,
                      {{"{SPEC}", missing}, {"{TYPE_MODE_RULE}", typeModeRule}, {"{POOL}", pool},
                       {"{LEARNER_IDENTITY}", identity}, {"{ARTICLE_EXCERPT}", articleExcerpt}});
                  std::vector<std::map<std::string, std::string>> msgs2 = {
                      {{"role", "system"}, {"content", prompts::CUSTOM_EXERCISE_SYSTEM}},
                      {{"role", "user"}, {"content", user2}}};
                  auto resp2 = ai_.postChat(msgs2, true, 4096, 0.6, "custom_exercise");
                  if (resp2.ok) {
                    try {
                      JsonValue j2 = parseJson(unwrapJson(aiText(resp2)));
                      const JsonValue* arr2 = j2.find("items");
                      if (arr2 && arr2->isArray()) {
                        JsonValue extra = normalizeItems(arr2->asArray(), mainTarget - (int)gotItems.size());
                        for (auto& x : extra.find("items")->asArray()) {
                          if ((int)gotItems.size() >= mainTarget) break;
                          gotItems.push_back(x);
                        }
                      }
                    } catch (...) {}
                  }
                }
              }
            }
          } catch (...) { mainFailed = true; }
        }
      }
      // 主请求若仍混入长语篇题型，剔除后由专用请求重出，避免占用短题名额
      if (longReserve > 0) {
        JsonArray keep;
        for (auto& x : gotItems) {
          std::string t = x.getString("type");
          if (t != "long_reading_cloze" && t != "full_cloze") keep.push_back(x);
        }
        gotItems = keep;
      }
      if (mainFailed && longReserve == 0) {
        failItem(itemId, eid, "AI 返回题目为空");
        allOk = false;
        continue;
      }

      // 长语篇题型单独补生成：主请求容易忽略或截断，按缺失数量逐题专用生成
      if (typesMode != "auto") {
        auto* counts = spec.find("counts");
        if (counts && counts->isObject()) {
          for (const std::string& lt : {"long_reading_cloze", "full_cloze"}) {
            int want = (int)counts->getInt(lt, 0);
            if (want <= 0) continue;
            int have = 0;
            for (auto& x : gotItems) if (x.getString("type") == lt) have++;
            for (int k = have; k < want && (int)gotItems.size() < total; ++k) {
              JsonValue item = generateLongItem(lt, pool, identity, articleExcerpt);
              if (item.isObject()) gotItems.push_back(item);
              else break;
            }
          }
        }
      }
      JsonObject rootObj;
      rootObj["items"] = JsonValue::makeArray(gotItems);
      if ((int)gotItems.size() < total) {
        rootObj["notice"] = JsonValue::makeString("部分题目生成失败，已保留有效题目，可重新生成。");
      }
      JsonValue result = JsonValue::makeObject(rootObj);
      std::string exerciseJson = result.dump();
      db_.exec("UPDATE custom_exercises SET status='done', exercise_json=" + q(exerciseJson) +
               ", item_count=" + std::to_string((int)result.find("items")->asArray().size()) +
               ", finished_at=NOW() WHERE id=" + std::to_string(eid));
      db_.exec("UPDATE ai_task_items SET status='done', finished_at=NOW() WHERE id=" + std::to_string(itemId));
    }
    return allOk;
  }

  // 提交作答并批改
  GradeResult grade(int64_t id, int uid, const std::string& guestId, const JsonValue& answers)
  {
    GradeResult r;
    View v;
    if (!get(id, uid, guestId, v)) { r.httpStatus = 404; r.error = "练习不存在"; return r; }
    if (v.status != "done") { r.httpStatus = 409; r.error = "练习尚未生成完成"; return r; }
    JsonValue ex = parseJson(v.exerciseJson);
    const JsonValue* items = ex.find("items");
    if (!items || !items->isArray()) { r.httpStatus = 500; r.error = "练习数据异常"; return r; }
    std::map<int, std::string> ansMap;
    if (answers.isObject()) {
      if (auto* a = answers.find("answers")) {
        if (a->isArray()) {
          for (auto& x : a->asArray()) {
            if (!x.isObject()) continue;
            int idx = (int)x.getInt("index", -1);
            if (idx < 0) continue;
            const JsonValue* av = x.find("answer");
            if (!av) continue;
            ansMap[idx] = av->isArray() ? av->dump() : av->asString();
          }
        }
      }
    }
    std::string identity = prompts::getLearnerIdentity(db_, uid);
    std::string out = "{\"items\":[";
    int sum = 0, n = 0, correct = 0;
    bool first = true;
    int idx = 0;
    for (auto& it : items->asArray()) {
      std::string type = it.getString("type");
      std::string stem = it.getString("stem");
      std::string ref = it.getString("answer");
      std::string got = ansMap.count(idx) ? ansMap[idx] : "";
      int score = 0;
      bool okItem = false;
      std::string feedback, reference = ref;
      if (type == "long_reading_cloze" || type == "full_cloze") {
        std::vector<std::string> refVals, refNos;
        if (type == "long_reading_cloze") {
          const JsonValue* bl = it.find("blanks");
          if (bl && bl->isArray()) {
            for (auto& b : bl->asArray()) {
              if (!b.isObject()) continue;
              refVals.push_back(exsan::trim(b.getString("answer")));
              refNos.push_back(std::to_string((int)b.getInt("no", (int)refVals.size())));
            }
          }
        } else {
          const JsonValue* qs = it.find("questions");
          if (qs && qs->isArray()) {
            for (auto& q : qs->asArray()) {
              if (!q.isObject()) continue;
              refVals.push_back(exsan::trim(q.getString("answer")));
              refNos.push_back(std::to_string((int)q.getInt("no", (int)refVals.size())));
            }
          }
        }
        if (!refVals.empty()) {
          std::vector<std::string> gotVals;
          JsonValue gotJson;
          try { gotJson = parseJson(got); } catch (...) {}
          if (gotJson.isArray()) {
            for (auto& g : gotJson.asArray()) if (g.isString()) gotVals.push_back(g.asString());
          }
          int totalBlanks = (int)refVals.size();
          int correctBlanks = 0;
          std::string wrongNos, allRefs;
          int pos = 0;
          for (auto& blankAns : refVals) {
            std::string g = pos < (int)gotVals.size() ? gotVals[pos] : "";
            if (sameAnswer(type == "full_cloze" ? "cloze_choice" : "word_form_fill", g, blankAns)) correctBlanks++;
            else {
              if (!wrongNos.empty()) wrongNos += "、";
              wrongNos += refNos[pos];
            }
            allRefs += (allRefs.empty() ? "" : "  ") + refNos[pos] + ". " + blankAns;
            pos++;
          }
          score = totalBlanks > 0 ? correctBlanks * 100 / totalBlanks : 0;
          okItem = correctBlanks == totalBlanks;
          feedback = okItem ? "全部填对" : ("第 " + wrongNos + " 空错误；正确答案：" + allRefs);
          reference = allRefs;
        } else {
          feedback = "练习数据缺少空位答案，请重新生成";
        }
      } else if (isObjective(type)) {
        okItem = sameAnswer(type, got, ref);
        score = okItem ? 100 : 0;
        feedback = okItem ? "回答正确" : "回答错误，正确答案：" + ref;
      } else {
        std::string user = prompts::buildTemplate(prompts::CUSTOM_EXERCISE_GRADE_USER,
            {{"{TYPE}", type}, {"{STEM}", stem}, {"{REFERENCE}", ref}, {"{ANSWER}", got}, {"{LEARNER_IDENTITY}", identity}});
        std::vector<std::map<std::string, std::string>> msgs = {
            {{"role", "system"}, {"content", prompts::CUSTOM_EXERCISE_GRADE_SYSTEM}},
            {{"role", "user"}, {"content", user}}};
        auto resp = ai_.postChat(msgs, true, 512, 0.2, "custom_exercise_grade");
        if (resp.ok) {
          JsonValue g = parseJson(unwrapJson(aiText(resp)));
          score = (int)g.getInt("score", 0);
          okItem = g.getBool("correct", score >= 60);
          feedback = g.getString("feedback", "");
          reference = g.getString("reference", ref);
        } else {
          score = 0;
          feedback = "AI 批改暂不可用，请对照参考答案自行核对";
        }
      }
      if (okItem) correct++;
      sum += score; n++;
      if (!first) out += ",";
      first = false;
      out += "{\"index\":" + std::to_string(idx) + ",\"type\":" + jstr(type) +
             ",\"correct\":" + (okItem ? "true" : "false") + ",\"score\":" + std::to_string(score) +
             ",\"feedback\":" + jstr(feedback) + ",\"reference\":" + jstr(reference) + "}";
      idx++;
    }
    out += "],\"score\":" + std::to_string(n ? sum / n : 0) + ",\"correct\":" + std::to_string(correct) +
           ",\"total\":" + std::to_string(n) + "}";
    db_.exec("UPDATE custom_exercises SET grade_json=" + q(out) + " WHERE id=" + std::to_string(id));
    r.ok = true;
    r.gradeJson = out;
    r.score = n ? sum / n : 0;
    return r;
  }

  static bool isKnownType(const std::string& t)
  {
    return exsan::isKnownType(t);
  }
  static bool isObjective(const std::string& t)
  {
    return exsan::isObjective(t);
  }

private:
  Db& db_;
  AiClient& ai_;
  AiQueueService& queue_;
  std::string root_;
  std::string wordTmpDir_;
  int guestExerciseLimit_ = 1;
  int userCallLimit_ = 15;

  static std::string q(const std::string& s) { return "'" + Db::escape(s) + "'"; }
  static std::string jstr(const std::string& s) { return JsonValue::makeString(s).dump(); }
  static std::string payloadSafe(const std::string& s) { return s; }
  // 真实 provider 放在 body，Mock 放在 extractedJson
  static std::string aiText(const AiResponse& r) { return r.extractedJson.empty() ? r.body : r.extractedJson; }

  void loadQuotaConfig()
  {
    std::ifstream f(root_ + "/config/quota_rules.json");
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    JsonValue j = parseJson(ss.str());
    if (auto* g = j.find("guest")) guestExerciseLimit_ = (int)g->getInt("custom_exercise", guestExerciseLimit_);
    if (auto* u = j.find("user")) userCallLimit_ = (int)u->getInt("explain_plus_exercise", userCallLimit_);
  }

  void bumpUser(int uid)
  {
    db_.exec("INSERT INTO ai_usage_daily (user_id,day,kind,calls,tokens) VALUES (" + std::to_string(uid) +
             ",CURDATE(),'custom_exercise',1,0) ON DUPLICATE KEY UPDATE calls=calls+1");
  }

  static std::string fnvHex(const std::string& s)
  {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
  }

  // 组题条转自然语言，明确告知 AI
  static std::string specTextOf(const JsonValue& spec)
  {
    std::string out;
    std::string src = "主题自选";
    if (auto* s = spec.find("source")) {
      std::string type = s->getString("type", "theme");
      if (type == "theme") src = "主题：" + s->getString("theme", "通用");
      else if (type == "wordlist") src = "我的词表（id=" + std::to_string(s->getInt("id", 0)) + "）";
      else if (type == "range") src = "教材范围包（id=" + std::to_string(s->getInt("id", 0)) + "）";
      else if (type == "wrong") src = "我的错题";
      else if (type == "studying") src = "我正在学的词";
    }
    out += "取词依据：" + src + "；";
    out += "难度：" + spec.getString("difficulty", "basic") + "；";
    out += "出题风格：" + spec.getString("style", "exam") + "；";
    if (auto* c = spec.find("counts")) {
      if (c->isObject()) {
        out += "题型数量：";
        for (auto& kv : c->asObject()) {
          int n = (int)kv.second.asInt();
          if (n > 0) out += kv.first + "×" + std::to_string(n) + " ";
        }
        out += "；";
      }
    }
    std::string extra = spec.getString("extra_requirements", "");
    if (!extra.empty()) out += "额外事项（用户原文）：" + extra + "；";
    if (spec.getString("types_mode", "counts") == "auto") {
      out += "题型：不限，由你按考试常见题型搭配；";
      out += "总题数：" + std::to_string((int)spec.getInt("total", 10)) + "；";
    }
    return out;
  }

  // 取词池：显式 words > 词表索引 > 在学/错题 > 空（由 AI 按主题自选）
  std::string poolFor(int uid, const JsonValue& spec)
  {
    std::vector<std::string> words;
    if (auto* w = spec.find("words")) {
      if (w->isArray()) {
        for (auto& x : w->asArray()) {
          if (x.isString()) words.push_back(x.asString());
          if (words.size() >= 40) break;
        }
      }
    }
    std::string type;
    int64_t sid = 0;
    if (auto* s = spec.find("source")) { type = s->getString("type"); sid = s->getInt("id", 0); }
    if (words.empty() && (type == "wordlist" || type == "range" || type == "mylist") && sid > 0) {
      std::string where = "wi.wordlist_id=" + std::to_string(sid);
      if (type == "range") {
        auto sc = db_.query("SELECT IFNULL(scope_key,'') AS k FROM wordlists WHERE id=" + std::to_string(sid));
        if (!sc.empty() && !sc[0].at("k").empty()) {
          where = "w.scope_key=" + q(sc[0].at("k"));
        }
      }
      // MySQL 8 中 DISTINCT 下不能按未选出列排序，改为取原始行后内存去重
      auto rows = db_.query(
          "SELECT wi.word FROM wordlist_items wi JOIN wordlists w ON w.id=wi.wordlist_id WHERE " +
          where + " ORDER BY wi.order_no, wi.id LIMIT 240");
      std::set<std::string> seen;
      for (auto& row : rows) {
        if (seen.insert(row.at("word")).second) words.push_back(row.at("word"));
        if (words.size() >= 60) break;
      }
    }
    if (words.empty() && (type == "studying" || type == "wrong") && uid > 0) {
      auto rows = db_.query(
          "SELECT user_input_content AS w FROM knowledge_points WHERE user_id=" + std::to_string(uid) +
          " AND is_solidified=1 ORDER BY id DESC LIMIT 40");
      for (auto& row : rows) words.push_back(row.at("w"));
    }
    std::string out;
    for (size_t i = 0; i < words.size(); i++) { if (i) out += ", "; out += words[i]; }
    return out;
  }

  static std::string unwrapJson(const std::string& s)
  {
    size_t b = s.find('{'), e = s.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e < b) return s;
    return s.substr(b, e - b + 1);
  }

  // 题型收敛：未知/无效题丢弃；选项前缀、空位标记、缺题面兜底统一在 exercise_sanitize.h 处理
  static JsonValue normalizeItems(const JsonArray& arr, int cap)
  {
    return exsan::normalizeItems(arr, cap);
  }

  // 补生成请求：只描述缺失题型与数量
  static std::string missingSpecText(const JsonValue& spec, const JsonArray& got, int total)
  {
    int missing = total - (int)got.size();
    if (missing <= 0) return "";
    if (spec.getString("types_mode", "counts") == "auto") {
      return "补充生成 " + std::to_string(missing) + " 道题，题型与已有题目尽量不同，至少包含 2 种题型。";
    }
    std::map<std::string, int> want, have;
    if (auto* c = spec.find("counts")) {
      if (c->isObject()) for (auto& kv : c->asObject()) want[kv.first] = (int)kv.second.asInt();
    }
    for (auto& it : got) have[it.getString("type")]++;
    std::string out;
    for (auto& kv : want) {
      // 长语篇题型走专用请求，避免与短题混排被模型忽略
      if (kv.first == "long_reading_cloze" || kv.first == "full_cloze") continue;
      int need = kv.second - have[kv.first];
      if (need > 0) out += kv.first + ":" + std::to_string(need) + " ";
    }
    if (out.empty()) return "";
    return "请补充以下题型数量（只出这些题，不要重复已有题目）：" + out;
  }

  // 长语篇题只取文章标题作主题，避免模型照抄短节选导致空数不足
  static std::string topicHint(const std::string& excerpt)
  {
    size_t p = excerpt.find("文章标题：");
    if (p == std::string::npos) return "";
    size_t e = excerpt.find('\n', p);
    return excerpt.substr(p, e == std::string::npos ? std::string::npos : e - p);
  }

  // 长语篇题型专用生成：单独 AI 请求，最多尝试 3 次
  JsonValue generateLongItem(const std::string& type, const std::string& pool,
                             const std::string& identity, const std::string& articleExcerpt)
  {
    bool full = (type == "full_cloze");
    const std::string& sys = full ? prompts::FULL_CLOZE_SYSTEM : prompts::LONG_READING_SYSTEM;
    const std::string& tpl = full ? prompts::FULL_CLOZE_USER : prompts::LONG_READING_USER;
    std::string topic = topicHint(articleExcerpt);
    if (topic.empty()) topic = "英语学习常见主题";
    std::string user = prompts::buildTemplate(tpl,
        {{"{POOL}", pool.empty() ? "自选常用词汇" : pool},
         {"{LEARNER_IDENTITY}", identity},
         {"{ARTICLE_EXCERPT}", topic}});
    for (int attempt = 0; attempt < 3; ++attempt) {
      std::vector<std::map<std::string, std::string>> msgs = {
          {{"role", "system"}, {"content", sys}},
          {{"role", "user"}, {"content", user}}};
      auto resp = ai_.postChat(msgs, true, 4096, 0.4, "custom_exercise");
      if (!resp.ok) continue;
      try {
        JsonValue j = parseJson(unwrapJson(aiText(resp)));
        const JsonValue* arr = j.find("items");
        if (arr && arr->isArray()) {
          for (auto& ex : arr->asArray()) {
            JsonValue n = exsan::normalizeItem(ex);
            if (n.isObject() && n.getString("type") == type && longItemUsable(n)) return n;
          }
        }
      } catch (...) {}
    }
    return JsonValue::makeNull();
  }

  // 空位之间必须有足够的上下文词，避免 [[1]] [[2]] 这类连排空
  // numbered=true 时按 [[n]] 扫描（长篇阅读保留标记），否则按 ______ 扫描（完整完形已替换）
  static bool blanksSpread(const std::string& p, size_t expect, bool numbered)
  {
    std::vector<std::pair<size_t, size_t>> pos;
    if (numbered) {
      size_t k = 0;
      while ((k = p.find("[[", k)) != std::string::npos) {
        size_t e = p.find("]]", k + 2);
        if (e == std::string::npos) break;
        pos.push_back({k, e + 2});
        k = e + 2;
      }
    } else {
      size_t k = 0;
      while ((k = p.find("______", k)) != std::string::npos) {
        pos.push_back({k, k + 6});
        k += 6;
      }
    }
    if (pos.size() != expect) return false;
    for (size_t i = 1; i < pos.size(); ++i) {
      std::string gap = p.substr(pos[i - 1].second, pos[i].first - pos[i - 1].second);
      bool hasLetter = false;
      for (char c : gap) if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) { hasLetter = true; break; }
      if (gap.size() < 15 || !hasLetter) return false;
    }
    return true;
  }

  // 长语篇题可用性检查：空位连排或选项组重复的题直接丢弃重试
  static bool longItemUsable(const JsonValue& n)
  {
    std::string type = n.getString("type");
    std::string p = n.getString("passage");
    if (type == "full_cloze") {
      const JsonValue* qs = n.find("questions");
      if (!qs || !qs->isArray() || qs->asArray().size() < 4) return false;
      if (!blanksSpread(p, qs->asArray().size(), false)) return false;
      // 允许个别空复用词库选项，但四空选项组完全相同视为无效题
      std::set<std::string> sets;
      for (auto& q : qs->asArray()) {
        const JsonValue* ops = q.find("options");
        if (!ops || !ops->isArray() || ops->asArray().size() != 4) return false;
        std::vector<std::string> v;
        for (auto& o : ops->asArray()) v.push_back(o.asString());
        std::sort(v.begin(), v.end());
        std::string key;
        for (auto& s : v) key += s + "|";
        sets.insert(key);
      }
      return sets.size() >= 2;
    }
    if (type == "long_reading_cloze") {
      const JsonValue* bl = n.find("blanks");
      if (!bl || !bl->isArray() || bl->asArray().size() < 4) return false;
      return blanksSpread(p, bl->asArray().size(), true);
    }
    return true;
  }

  static std::string firstLetter(const std::string& s)
  {
    for (char c : s) {
      if (c >= 'a' && c <= 'z') return std::string(1, (char)(c - 'a' + 'A'));
      if (c >= 'A' && c <= 'Z') return std::string(1, c);
    }
    return "";
  }

  static std::string norm(const std::string& s)
  {
    std::string out;
    for (char c : s) {
      if (c == '"' || c == '\'' || c == '.' || c == ',') continue;
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { if (!out.empty() && out.back() != ' ') out += ' '; continue; }
      out += (char)std::tolower((unsigned char)c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
  }

  static bool sameAnswer(const std::string& type, const std::string& got, const std::string& ref)
  {
    if (got.empty()) return false;
    if (type == "word_match" || type == "cloze_choice") {
      if (norm(got) == norm(ref)) return true;
      std::string gl = firstLetter(got), rl = firstLetter(ref);
      return !gl.empty() && gl == rl && norm(got).size() <= 3;
    }
    std::string g = norm(got);
    // 参考答案支持 "a / b | c" 多解（仅 ASCII 分隔符，避免多字节字符字面量）
    std::string cur;
    std::vector<std::string> cands;
    for (char c : ref) {
      if (c == '/' || c == '|' || c == ';' || c == ',') { if (!cur.empty()) cands.push_back(norm(cur)); cur.clear(); }
      else cur += c;
    }
    if (!cur.empty()) cands.push_back(norm(cur));
    for (auto& x : cands) if (x == g) return true;
    return false;
  }

  void failItem(int64_t itemId, int64_t exerciseId, const std::string& err)
  {
    db_.exec("UPDATE ai_task_items SET status='failed', error=" + q(err) + ", finished_at=NOW() WHERE id=" +
             std::to_string(itemId));
    db_.exec("UPDATE custom_exercises SET status='failed', error_msg=" + q(err.substr(0, 240)) +
             " WHERE id=" + std::to_string(exerciseId));
  }
};

}  // namespace eng
