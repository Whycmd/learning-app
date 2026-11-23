// 职责：AI 讲解（v1.5.0-T006）
// 链路：归一化 → L1 命中直接返回 → 未命中建 L2 任务 → 队列 runner 调 AI → 落 explanations
// 依赖：Db / AiClient / AiQueueService / KnowledgeService / prompts.h
#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ai_client.h"
#include "ai_queue_service.h"
#include "db.h"
#include "json.h"
#include "knowledge_service.h"
#include "prompts.h"

namespace eng {

class ExplainService
{
public:
  struct View {
    int64_t id=0;
    int userId=0;
    std::string inputText, normalizedKey, inputType, identity, depth, status, summary, contentMd, resultJson, error, createdAt;
    int64_t taskId=0, l1KpId=0;
    int position=0, etaSec=0, queueLevel=0;
  };

  struct Submit {
    bool ok=false;
    int httpStatus=200;
    std::string error;
    int64_t explainId=0, taskId=0;
    int queueLevel=0;
    bool l1Hit=false;
    int position=0, etaSec=0;
    std::string summary, resultJson;
  };

  ExplainService(Db& db, AiClient& ai, AiQueueService& queue, const std::string& root)
      : db_(db), ai_(ai), queue_(queue), root_(root) { loadShareConfig(); loadQuotaConfig(); }

  // ===== 公共入口 =====

  // 新建讲解；uid<=0 视为游客（guestId 必填）
  Submit submit(int uid, const std::string& guestId, const std::string& rawText,
                const std::string& depth, int queueLevel, bool skipL1 = false)
  {
    Submit r;
    std::string text = trim(rawText);
    if (text.empty()) { r.httpStatus = 400; r.error = "请输入要讲解的内容"; return r; }
    std::string type = detectType(text);
    if (!hasEnglish(text)) { r.httpStatus = 400; r.error = "目前只支持英文单词、短语、句子或段落"; return r; }
    size_t cap = (type == "word" || type == "phrase") ? 500 : 2000;
    if (text.size() > cap) {
      r.httpStatus = 400;
      r.error = (cap == 500 ? "单词/短语最多 500 个字符" : "句子/段落最多 2000 个字符");
      return r;
    }
    std::string d = normalizeDepth(depth);
    std::string identity = prompts::getLearnerIdentity(db_, uid);
    std::string key = clampKey(KnowledgeService::normalizeKey(text));

    // 1) L1：已固化词条（同 normalized_key + 同身份），命中同步秒回，不占额度
    if (!skipL1) {
      View l1;
      if (findL1(uid, key, identity, l1)) {
        int64_t eid = insertRow(uid, guestId, text, key, type, identity, d, "done", l1.l1KpId,
                                l1.summary, l1.contentMd, l1.resultJson);
        if (eid <= 0) { r.httpStatus = 500; r.error = "写入讲解记录失败"; return r; }
        r.ok = true; r.explainId = eid; r.l1Hit = true; r.summary = l1.summary; r.resultJson = l1.resultJson;
        return r;
      }
    }

    // 2) 限额（仅 L2 计入）
    if (uid > 0) {
      int level = userLevel(uid, "explain", cfgUserCalls_, cfgUserTokens_);
      if (level > 0) queueLevel = std::max(queueLevel, level);
    } else {
      std::string err;
      if (!guestBump(guestId, "explain", guestExplain_, err)) {
        r.httpStatus = 401;
        r.error = "游客今日讲解次数已用完，登录后可继续使用";
        return r;
      }
    }

    // 3) L2：建任务
    int64_t eid = insertRow(uid, guestId, text, key, type, identity, d, "queued", 0, "", "", "");
    if (eid <= 0) { r.httpStatus = 500; r.error = "写入讲解记录失败"; return r; }
    std::vector<AiQueueService::NewItem> items;
    items.push_back(AiQueueService::NewItem{eid, text.substr(0, 200)});
    std::string payload = "{\"explain_id\":" + std::to_string(eid) + ",\"depth\":" + jstr(d) + "}";
    auto c = queue_.create(uid, "explain", "explain", items, payload, queueLevel);
    if (!c.ok) {
      db_.exec("UPDATE explanations SET status='failed', error_msg=" + q(c.error) +
               ", finished_at=NOW() WHERE id=" + std::to_string(eid));
      r.httpStatus = c.httpStatus ? c.httpStatus : 500;
      r.error = c.error.empty() ? "创建 AI 任务失败" : c.error;
      return r;
    }
    db_.exec("UPDATE explanations SET task_id=" + std::to_string(c.taskId) + " WHERE id=" + std::to_string(eid));
    if (uid > 0) bumpUser(uid, "explain");
    AiTaskView tv;
    if (queue_.get(c.taskId, uid, tv)) { r.position = tv.position; r.etaSec = tv.etaSec; }
    r.ok = true; r.explainId = eid; r.taskId = c.taskId; r.queueLevel = queueLevel;
    return r;
  }

  // 重新生成：跳过 L1，保留旧记录
  Submit regenerate(int64_t explainId, int uid, const std::string& guestId, const std::string& depth, int queueLevel)
  {
    View v;
    if (!get(explainId, uid, guestId, v)) { Submit r; r.httpStatus = 404; r.error = "讲解不存在"; return r; }
    return submit(uid, guestId, v.inputText, depth.empty() ? v.depth : depth, queueLevel, true);
  }

  bool get(int64_t explainId, int uid, const std::string& guestId, View& out)
  {
    auto rows = db_.query(
        "SELECT id,user_id,IFNULL(guest_id,'') AS guest_id,input_text,normalized_key,input_type,"
        "identity_snapshot,depth,status,summary,content_md,result_json,error_msg,created_at,task_id,"
        "IFNULL(l1_kp_id,0) AS l1_kp_id FROM explanations WHERE id=" +
        std::to_string(explainId));
    if (rows.empty()) return false;
    auto& row = rows[0];
    int owner = std::stoi(row.at("user_id"));
    if (owner > 0) {
      if (owner != uid) return false;
    } else if (guestId.empty() || row.at("guest_id") != guestId) {
      return false;
    }
    fillView(row, out);
    if (out.status == "queued" && out.taskId > 0) {
      AiTaskView tv;
      if (queue_.get(out.taskId, uid, tv)) {
        out.position = tv.position; out.etaSec = tv.etaSec; out.queueLevel = tv.queueLevel;
        if (tv.status == "done" && out.status == "queued") {
          // 任务已完成但行未刷新（runner 异常中断）→ 以任务为准重读
          auto again = db_.query(
              "SELECT id,user_id,input_text,normalized_key,input_type,identity_snapshot,depth,status,summary,"
              "content_md,result_json,error_msg,created_at,task_id,IFNULL(l1_kp_id,0) AS l1_kp_id FROM explanations WHERE id=" +
              std::to_string(explainId));
          if (!again.empty()) fillView(again[0], out);
        }
      }
    }
    return true;
  }

  std::vector<View> history(int uid, int limit, int offset)
  {
    std::vector<View> out;
    if (uid <= 0) return out;
    auto rows = db_.query(
        "SELECT id,user_id,input_text,normalized_key,input_type,identity_snapshot,depth,status,summary,"
        "content_md,result_json,error_msg,created_at,task_id,IFNULL(l1_kp_id,0) AS l1_kp_id FROM explanations "
        "WHERE user_id=" + std::to_string(uid) + " ORDER BY id DESC LIMIT " + std::to_string(limit) +
        " OFFSET " + std::to_string(offset));
    for (auto& row : rows) { View v; fillView(row, v); out.push_back(v); }
    return out;
  }

  // 队列 runner：kind=explain
  bool runTask(int uid, int64_t taskId)
  {
    auto items = db_.query("SELECT id,ref_id FROM ai_task_items WHERE task_id=" + std::to_string(taskId) +
                           " AND status='pending' ORDER BY item_index, id");
    bool allOk = true;
    for (auto& it : items) {
      int64_t itemId = std::stoll(it.at("id"));
      int64_t eid = it.count("ref_id") && !it.at("ref_id").empty() ? std::stoll(it.at("ref_id")) : 0;
      db_.exec("UPDATE ai_task_items SET status='running', started_at=NOW() WHERE id=" + std::to_string(itemId));
      auto rows = db_.query("SELECT input_text,input_type,depth,identity_snapshot FROM explanations WHERE id=" +
                            std::to_string(eid));
      if (rows.empty()) { failItem(itemId, eid, "讲解记录不存在"); allOk = false; continue; }
      std::string text = rows[0].at("input_text");
      std::string type = rows[0].at("input_type");
      std::string depth = rows[0].at("depth");
      std::string identity = rows[0].at("identity_snapshot");

      std::string user = prompts::buildTemplate(prompts::EXPLAIN_USER,
          {{"{TEXT}", text}, {"{TYPE}", type}, {"{DEPTH}", depth},
           {"{DEPTH_RULE}", depthRule(depth)}, {"{LEARNER_IDENTITY}", identity}});
      std::vector<std::map<std::string, std::string>> msgs = {
          {{"role", "system"}, {"content", prompts::EXPLAIN_SYSTEM}},
          {{"role", "user"}, {"content", user}}};
      auto resp = ai_.postChat(msgs, true, 2048, 0.4, "explain");
      if (!resp.ok) { failItem(itemId, eid, resp.body.empty() ? "AI 请求失败" : resp.body.substr(0, 180)); allOk = false; continue; }

      JsonValue j = parseJson(unwrapJson(aiText(resp)));
      std::string summary = j.getString("summary");
      std::string md = mdText(j, "markdown_content");
      if (summary.empty() && md.empty()) { failItem(itemId, eid, "AI 返回内容无法解析"); allOk = false; continue; }
      std::string resultJson = "{\"source\":\"l2\",\"summary\":" + jstr(summary) +
                               ",\"markdown_content\":" + jstr(md) + "}";

      db_.exec("UPDATE explanations SET status='done', summary=" + q(summary) + ", content_md=" + q(md) +
               ", result_json=" + q(resultJson) + ", finished_at=NOW() WHERE id=" + std::to_string(eid));
      db_.exec("UPDATE ai_task_items SET status='done', finished_at=NOW() WHERE id=" + std::to_string(itemId));
      maybeIngest(uid, text, type, identity, summary, md, false);
    }
    return allOk;
  }

  // 手动「保存为词条」（小众身份）
  struct SaveResult { bool ok=false; int httpStatus=200; std::string error; int64_t kpId=0; };
  SaveResult saveKp(int64_t explainId, int uid)
  {
    SaveResult r;
    if (uid <= 0) { r.httpStatus = 401; r.error = "请先登录"; return r; }
    View v;
    if (!get(explainId, uid, "", v)) { r.httpStatus = 404; r.error = "讲解不存在"; return r; }
    if (v.status != "done") { r.httpStatus = 409; r.error = "讲解尚未完成"; return r; }
    r.kpId = maybeIngest(uid, v.inputText, v.inputType, v.identity, v.summary, v.contentMd, true);
    if (r.kpId <= 0) {
      auto rows = db_.query("SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid) +
                            " AND normalized_key=" + q(clampKey(KnowledgeService::normalizeKey(v.inputText))) + " LIMIT 1");
      if (!rows.empty()) r.kpId = std::stoll(rows[0].at("id"));
    }
    if (r.kpId <= 0) { r.httpStatus = 500; r.error = "保存词条失败"; return r; }
    r.ok = true;
    return r;
  }

  // ===== v1.5.0-T008：学习时追问 =====

  struct FollowupView {
    int64_t id=0, explainId=0, taskId=0, mergedKpId=0;
    int merged=0, position=0, etaSec=0;
    std::string question, answerMd, status, createdAt;
  };

  struct FollowupSubmit {
    bool ok=false;
    int httpStatus=200;
    std::string error;
    int64_t id=0, taskId=0;
    int position=0, etaSec=0;
  };

  // 发起追问：仅精讲完成后可问；游客也可（校验归属）
  FollowupSubmit submitFollowup(int64_t explainId, int uid, const std::string& guestId,
                                const std::string& rawQuestion, int queueLevel)
  {
    FollowupSubmit r;
    std::string question = trim(rawQuestion);
    if (question.empty()) { r.httpStatus = 400; r.error = "请输入追问内容"; return r; }
    if (question.size() > 500) { r.httpStatus = 400; r.error = "追问最多 500 个字符"; return r; }
    View v;
    if (!get(explainId, uid, guestId, v)) { r.httpStatus = 404; r.error = "讲解不存在"; return r; }
    if (v.status != "done") { r.httpStatus = 409; r.error = "精讲尚未生成，暂不能追问"; return r; }
    std::string gid = uid > 0 ? "NULL" : q(guestId);
    auto w = db_.exec(
        "INSERT INTO explanation_followups (explain_id,user_id,guest_id,question,status) VALUES (" +
        std::to_string(explainId) + "," + std::to_string(uid) + "," + gid + "," + q(question) + ",'queued')");
    int64_t fid = w.last_insert_id;
    if (fid <= 0) { r.httpStatus = 500; r.error = "写入追问失败"; return r; }
    std::vector<AiQueueService::NewItem> items;
    items.push_back(AiQueueService::NewItem{fid, question.substr(0, 200)});
    std::string payload = "{\"explain_id\":" + std::to_string(explainId) +
                          ",\"followup_id\":" + std::to_string(fid) + "}";
    auto c = queue_.create(uid, "followup", "followup", items, payload, queueLevel);
    if (!c.ok) {
      db_.exec("UPDATE explanation_followups SET status='failed' WHERE id=" + std::to_string(fid));
      r.httpStatus = c.httpStatus ? c.httpStatus : 500;
      r.error = c.error.empty() ? "创建 AI 任务失败" : c.error;
      return r;
    }
    db_.exec("UPDATE explanation_followups SET task_id=" + std::to_string(c.taskId) +
             " WHERE id=" + std::to_string(fid));
    AiTaskView tv;
    if (queue_.get(c.taskId, uid, tv)) { r.position = tv.position; r.etaSec = tv.etaSec; }
    r.ok = true; r.id = fid; r.taskId = c.taskId;
    return r;
  }

  bool listFollowups(int64_t explainId, int uid, const std::string& guestId,
                     std::vector<FollowupView>& out)
  {
    View v;
    if (!get(explainId, uid, guestId, v)) return false;
    auto rows = db_.query(
        "SELECT id,explain_id,IFNULL(question,'') AS question,IFNULL(answer_md,'') AS answer_md,status,"
        "IFNULL(task_id,0) AS task_id,merged,IFNULL(merged_kp_id,0) AS merged_kp_id,"
        "DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s') AS created_at FROM explanation_followups "
        "WHERE explain_id=" + std::to_string(explainId) + " ORDER BY id");
    for (auto& row : rows) {
      FollowupView f;
      f.id = std::stoll(row.at("id"));
      f.explainId = std::stoll(row.at("explain_id"));
      f.question = row.at("question");
      f.answerMd = row.at("answer_md");
      f.status = row.at("status");
      f.taskId = row.at("task_id").empty() ? 0 : std::stoll(row.at("task_id"));
      f.merged = std::stoi(row.at("merged"));
      f.mergedKpId = row.at("merged_kp_id").empty() ? 0 : std::stoll(row.at("merged_kp_id"));
      f.createdAt = row.at("created_at");
      if (f.status == "queued" && f.taskId > 0) {
        AiTaskView tv;
        if (queue_.get(f.taskId, uid, tv)) { f.position = tv.position; f.etaSec = tv.etaSec; }
      }
      out.push_back(f);
    }
    return true;
  }

  struct MergeResult { bool ok=false; int httpStatus=200; bool already=false; std::string error; int64_t kpId=0; };

  // 用户同意后并入公共精讲：目标词条转 public，正文追加「## 补充：{问题}」
  MergeResult mergeFollowup(int64_t explainId, int64_t fid, int uid)
  {
    MergeResult r;
    if (uid <= 0) { r.httpStatus = 401; r.error = "请先登录"; return r; }
    View v;
    if (!get(explainId, uid, "", v)) { r.httpStatus = 404; r.error = "讲解不存在"; return r; }
    auto fRows = db_.query(
        "SELECT id,IFNULL(question,'') AS question,IFNULL(answer_md,'') AS answer_md,status,merged,"
        "IFNULL(merged_kp_id,0) AS merged_kp_id FROM explanation_followups WHERE id=" +
        std::to_string(fid) + " AND explain_id=" + std::to_string(explainId));
    if (fRows.empty()) { r.httpStatus = 404; r.error = "追问不存在"; return r; }
    if (fRows[0].at("merged") == "1") {
      r.ok = true; r.already = true;
      r.kpId = fRows[0].at("merged_kp_id").empty() ? 0 : std::stoll(fRows[0].at("merged_kp_id"));
      return r;
    }
    if (fRows[0].at("status") != "done" || fRows[0].at("answer_md").empty()) {
      r.httpStatus = 409; r.error = "追问尚未完成"; return r;
    }
    std::string append = "\n\n## 补充：" + fRows[0].at("question") + "\n\n" + fRows[0].at("answer_md") + "\n";

    // 目标词条：本次讲解的 L1 词条优先，否则按 key + 身份找本人词条
    int64_t kpId = v.l1KpId;
    if (kpId > 0) {
      auto own = db_.query("SELECT id FROM knowledge_points WHERE id=" + std::to_string(kpId) +
                           " AND user_id=" + std::to_string(uid));
      if (own.empty()) kpId = 0;
    }
    if (kpId <= 0) {
      auto rows = db_.query(
          "SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid) +
          " AND normalized_key=" + q(v.normalizedKey) + " AND identity_snapshot=" + q(v.identity) +
          " ORDER BY (is_solidified=1) DESC, id ASC LIMIT 1");
      if (!rows.empty()) kpId = std::stoll(rows[0].at("id"));
    }
    if (kpId <= 0) {
      std::string key = v.normalizedKey.empty()
          ? clampKey(KnowledgeService::normalizeKey(v.inputText)) : v.normalizedKey;
      std::string stdContent = v.summary.empty() ? v.inputText.substr(0, 200) : v.summary;
      bool solidified = !v.contentMd.empty();
      auto w = db_.exec(
          "INSERT INTO knowledge_points (user_id,user_input_content,standard_content,normalized_key,"
          "identity_snapshot,is_solidified,visibility,origin,solidify_time) VALUES (" +
          std::to_string(uid) + "," + q(v.inputText) + "," + q(stdContent) + "," + q(key) + "," +
          q(v.identity) + "," + (solidified ? "1" : "0") + ",'public'," + q(shareOrigin_) + "," +
          (solidified ? "NOW()" : "NULL") + ")");
      kpId = w.last_insert_id;
      if (kpId > 0 && solidified) {
        db_.exec("INSERT INTO kp_extras (knowledge_id,extra_type,content,display_order) VALUES (" +
                 std::to_string(kpId) + ",'markdown_content'," + q(v.contentMd) + ",0)");
      }
    }
    if (kpId <= 0) { r.httpStatus = 500; r.error = "创建词条失败"; return r; }

    bool ok = db_.transaction([&]() -> bool {
      auto mark = db_.exec("UPDATE explanation_followups SET merged=1, merged_kp_id=" +
                           std::to_string(kpId) + " WHERE id=" + std::to_string(fid) + " AND merged=0");
      if (mark.affected_rows == 0) return true;  // 并发重复点按：幂等
      db_.exec("UPDATE knowledge_points SET visibility='public' WHERE id=" + std::to_string(kpId) +
               " AND user_id=" + std::to_string(uid));
      auto ex = db_.query("SELECT id FROM kp_extras WHERE knowledge_id=" + std::to_string(kpId) +
                          " AND extra_type='markdown_content' LIMIT 1");
      if (ex.empty()) {
        db_.exec("INSERT INTO kp_extras (knowledge_id,extra_type,content,display_order) VALUES (" +
                 std::to_string(kpId) + ",'markdown_content'," + q(append) + ",0)");
      } else {
        db_.exec("UPDATE kp_extras SET content=CONCAT(content," + q(append) + ") WHERE id=" + ex[0].at("id"));
      }
      db_.exec("UPDATE knowledge_points SET is_solidified=1 WHERE id=" + std::to_string(kpId));
      return true;
    });
    if (!ok) { r.httpStatus = 500; r.error = "并入失败"; return r; }
    r.ok = true; r.kpId = kpId;
    return r;
  }

  // 队列 runner：kind=followup
  bool runFollowupTask(int uid, int64_t taskId)
  {
    auto items = db_.query("SELECT id,ref_id FROM ai_task_items WHERE task_id=" + std::to_string(taskId) +
                           " AND status='pending' ORDER BY item_index, id");
    bool allOk = true;
    for (auto& it : items) {
      int64_t itemId = std::stoll(it.at("id"));
      int64_t fid = it.count("ref_id") && !it.at("ref_id").empty() ? std::stoll(it.at("ref_id")) : 0;
      db_.exec("UPDATE ai_task_items SET status='running', started_at=NOW() WHERE id=" + std::to_string(itemId));
      auto rows = db_.query(
          "SELECT f.question,e.input_text,e.identity_snapshot,e.content_md FROM explanation_followups f "
          "JOIN explanations e ON e.id=f.explain_id WHERE f.id=" + std::to_string(fid));
      if (rows.empty()) { failFollowupItem(itemId, fid, "追问记录不存在"); allOk = false; continue; }
      std::string user = prompts::buildTemplate(prompts::FOLLOWUP_USER,
          {{"{TEXT}", rows[0].at("input_text")},
           {"{LEARNER_IDENTITY}", rows[0].at("identity_snapshot")},
           {"{EXPLAIN_MD}", rows[0].at("content_md")},
           {"{QUESTION}", rows[0].at("question")}});
      std::vector<std::map<std::string, std::string>> msgs = {
          {{"role", "system"}, {"content", prompts::FOLLOWUP_SYSTEM}},
          {{"role", "user"}, {"content", user}}};
      auto resp = ai_.postChat(msgs, true, 1200, 0.4, "explain_followup");
      if (!resp.ok) {
        failFollowupItem(itemId, fid, resp.body.empty() ? "AI 请求失败" : resp.body.substr(0, 180));
        allOk = false; continue;
      }
      JsonValue j = parseJson(unwrapJson(aiText(resp)));
      std::string answer = mdText(j, "answer_md");
      if (answer.empty()) { failFollowupItem(itemId, fid, "AI 返回内容无法解析"); allOk = false; continue; }
      db_.exec("UPDATE explanation_followups SET status='done', answer_md=" + q(answer) +
               ", finished_at=NOW() WHERE id=" + std::to_string(fid));
      db_.exec("UPDATE ai_task_items SET status='done', finished_at=NOW() WHERE id=" + std::to_string(itemId));
    }
    return allOk;
  }

  void failFollowupItem(int64_t itemId, int64_t fid, const std::string& err)
  {
    db_.exec("UPDATE ai_task_items SET status='failed', error=" + q(err) + ", finished_at=NOW() WHERE id=" +
             std::to_string(itemId));
    db_.exec("UPDATE explanation_followups SET status='failed' WHERE id=" + std::to_string(fid));
  }

  // ===== 限额 =====

  bool guestBump(const std::string& guestId, const std::string& feature, int limit, std::string& err)
  {
    if (guestId.empty()) { err = "missing guest id"; return false; }
    auto w = db_.exec(
        "INSERT INTO guest_trials (guest_id,feature,day,used) VALUES (" + q(guestId) + "," + q(feature) +
        ",CURDATE(),1) ON DUPLICATE KEY UPDATE used=IF(used<" + std::to_string(limit) + ",used+1,used)");
    return w.affected_rows > 0;
  }

  int userLevel(int uid, const std::string& kind, int callLimit, long long tokenLimit)
  {
    auto rows = db_.query("SELECT calls,tokens FROM ai_usage_daily WHERE user_id=" + std::to_string(uid) +
                          " AND day=CURDATE() AND kind=" + q(kind));
    if (rows.empty()) return 0;
    int calls = std::stoi(rows[0].at("calls"));
    long long toks = rows[0].count("tokens") ? std::stoll(rows[0].at("tokens")) : 0;
    if (calls >= callLimit || (tokenLimit > 0 && toks >= tokenLimit)) return 1;
    return 0;
  }

  void bumpUser(int uid, const std::string& kind)
  {
    if (uid <= 0) return;
    db_.exec("INSERT INTO ai_usage_daily (user_id,day,kind,calls,tokens) VALUES (" + std::to_string(uid) +
             ",CURDATE()," + q(kind) + ",1,0) ON DUPLICATE KEY UPDATE calls=calls+1");
  }

  int cfgGuestExplain() const { return guestExplain_; }
  int cfgGuestExercise() const { return guestExercise_; }
  int cfgIdleHintItems() const { return idleHintItems_; }
  int cfgIdleHintEta() const { return idleHintEta_; }

private:
  Db& db_;
  AiClient& ai_;
  AiQueueService& queue_;
  std::string root_;

  // config/knowledge_share.json
  bool autoIngest_ = true;
  std::vector<std::string> shareIdentities_;
  std::vector<std::string> shareTypes_;
  std::string shareOrigin_ = "explain";
  // config/quota_rules.json
  int guestExplain_ = 10;
  int guestExercise_ = 1;
  int userCalls_ = 15;
  long long userTokens_ = 500000;
  int idleHintItems_ = 5;
  int idleHintEta_ = 60;

  int cfgUserCalls_ = 15;
  long long cfgUserTokens_ = 500000;

  static std::string q(const std::string& s) { return "'" + Db::escape(s) + "'"; }
  static std::string jstr(const std::string& s) { return JsonValue::makeString(s).dump(); }

  // 精讲正文可能是字符串，也可能是「每行一个元素」的字符串数组（KP_MARKDOWN_RULES 允许）
  static std::string mdText(const JsonValue& j, const std::string& field)
  {
    auto* v = j.find(field);
    if (!v) return "";
    if (v->isString()) return v->asString();
    if (!v->isArray()) return "";
    std::string out;
    for (auto& e : v->asArray()) {
      std::string line = e.isString() ? e.asString() : "";
      if (trim(line).empty()) continue;
      if (!out.empty()) out += "\n";
      out += line;
    }
    return out;
  }
  // 真实 provider 放在 body，Mock 放在 extractedJson
  static std::string aiText(const AiResponse& r) { return r.extractedJson.empty() ? r.body : r.extractedJson; }

  static std::string trim(const std::string& s)
  {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  }
  static bool hasEnglish(const std::string& s)
  {
    int letters = 0;
    for (unsigned char c : s) if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letters++;
    return letters >= 2;
  }
  // normalized_key 列为 varchar(191)，按 UTF-8 边界截断，避免超长输入写库失败
  static std::string clampKey(std::string k, size_t maxLen = 191)
  {
    if (k.size() <= maxLen) return k;
    size_t n = maxLen;
    while (n > 0 && ((unsigned char)k[n] & 0xC0) == 0x80) n--;
    k.resize(n);
    return k;
  }
  static std::string detectType(const std::string& s)
  {
    if (s.find('\n') != std::string::npos && s.size() > 60) return "article";
    size_t spaces = (size_t)std::count(s.begin(), s.end(), ' ');
    if (spaces == 0) return "word";
    if (s.size() <= 80) return "phrase";
    return "sentence";
  }
  static std::string normalizeDepth(const std::string& d)
  {
    if (d == "brief" || d == "standard" || d == "deep") return d;
    return "standard";
  }
  static std::string depthRule(const std::string& d)
  {
    if (d == "brief") return "只讲核心释义与一个典型例句，保持精简";
    if (d == "deep") return "补充词源/近义辨析/易错点，并给多个例句";
    return "核心释义、用法要点与一个典型例句";
  }
  // 去掉 ```json 包裹，截取首个 { 到末个 }
  static std::string unwrapJson(const std::string& s)
  {
    size_t b = s.find('{'), e = s.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e < b) return s;
    return s.substr(b, e - b + 1);
  }

  void loadShareConfig()
  {
    std::ifstream f(root_ + "/config/knowledge_share.json");
    if (!f) { shareIdentities_ = {"高中生", "初中生", "大学生", "雅思", "托福", "考研"}; shareTypes_ = {"word", "phrase", "sentence"}; return; }
    std::stringstream ss; ss << f.rdbuf();
    JsonValue j = parseJson(ss.str());
    autoIngest_ = j.getBool("auto_ingest", true);
    shareOrigin_ = j.getString("origin", "explain");
    if (auto* v = j.find("common_identities")) if (v->isArray()) for (auto& x : v->asArray()) if (x.isString()) shareIdentities_.push_back(x.asString());
    if (auto* v = j.find("ingest_types")) if (v->isArray()) for (auto& x : v->asArray()) if (x.isString()) shareTypes_.push_back(x.asString());
  }

  void loadQuotaConfig()
  {
    std::ifstream f(root_ + "/config/quota_rules.json");
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    JsonValue j = parseJson(ss.str());
    if (auto* g = j.find("guest")) {
      guestExplain_ = (int)g->getInt("explain", guestExplain_);
      guestExercise_ = (int)g->getInt("custom_exercise", guestExercise_);
    }
    if (auto* u = j.find("user")) {
      userCalls_ = (int)u->getInt("explain_plus_exercise", userCalls_);
      userTokens_ = u->getInt("tokens_per_day", userTokens_);
      cfgUserCalls_ = userCalls_;
      cfgUserTokens_ = userTokens_;
    }
    if (auto* qq = j.find("queue")) {
      idleHintItems_ = (int)qq->getInt("idle_hint_min_items", idleHintItems_);
      idleHintEta_ = (int)qq->getInt("idle_hint_min_eta_sec", idleHintEta_);
    }
  }

  int64_t insertRow(int uid, const std::string& guestId, const std::string& text, const std::string& key,
                    const std::string& type, const std::string& identity, const std::string& depth,
                    const std::string& status, int64_t l1KpId, const std::string& summary,
                    const std::string& md, const std::string& resultJson)
  {
    std::string gid = uid > 0 ? "NULL" : q(guestId);
    std::string finished = status == "done" ? "NOW()" : "NULL";
    auto w = db_.exec(
        "INSERT INTO explanations (user_id,guest_id,input_text,normalized_key,input_type,identity_snapshot,"
        "depth,status,l1_kp_id,summary,content_md,result_json,finished_at) VALUES (" +
        std::to_string(uid) + "," + gid + "," + q(text) + "," + q(key) + "," + q(type) + "," + q(identity) +
        "," + q(depth) + "," + q(status) + "," + (l1KpId > 0 ? std::to_string(l1KpId) : std::string("NULL")) +
        "," + q(summary) + "," + q(md) + "," + q(resultJson) + "," + finished + ")");
    return w.last_insert_id;
  }

  void fillView(const std::map<std::string, std::string>& row, View& v)
  {
    v.id = std::stoll(row.at("id"));
    v.userId = std::stoi(row.at("user_id"));
    v.inputText = row.at("input_text");
    v.normalizedKey = row.at("normalized_key");
    v.inputType = row.at("input_type");
    v.identity = row.at("identity_snapshot");
    v.depth = row.at("depth");
    v.status = row.at("status");
    v.summary = row.count("summary") ? row.at("summary") : "";
    v.contentMd = row.count("content_md") ? row.at("content_md") : "";
    v.resultJson = row.count("result_json") ? row.at("result_json") : "";
    v.error = row.count("error_msg") ? row.at("error_msg") : "";
    v.createdAt = row.at("created_at");
    v.taskId = row.count("task_id") && !row.at("task_id").empty() ? std::stoll(row.at("task_id")) : 0;
    v.l1KpId = row.count("l1_kp_id") && !row.at("l1_kp_id").empty() ? std::stoll(row.at("l1_kp_id")) : 0;
  }

  // L1：可复用精讲词条（本人词条 + 他人 public），同 key + 同身份，不暴露作者与备注
  bool findL1(int uid, const std::string& key, const std::string& identity, View& out)
  {
    auto rows = db_.query(
        "SELECT id, IFNULL(standard_content,'') AS standard_content FROM knowledge_points "
        "WHERE normalized_key=" + q(key) + " AND identity_snapshot=" + q(identity) +
        " AND is_solidified=1 AND (user_id=" + std::to_string(uid) + " OR visibility='public')"
        " ORDER BY (user_id=" + std::to_string(uid) + ") DESC, id ASC LIMIT 1");
    if (rows.empty()) return false;
    int64_t kpId = std::stoll(rows[0].at("id"));
    std::string summary = rows[0].at("standard_content");
    std::string md;
    auto extras = db_.query(
        "SELECT extra_type, content FROM kp_extras WHERE knowledge_id=" + std::to_string(kpId) +
        " AND extra_type='markdown_content' AND content<>'' ORDER BY id LIMIT 1");
    if (!extras.empty()) md = extras[0].at("content");
    if (md.empty()) return false;  // 无精讲正文不可复用
    out.l1KpId = kpId;
    out.summary = summary;
    out.contentMd = md;
    out.resultJson = "{\"source\":\"l1\",\"kp_id\":" + std::to_string(kpId) +
                     ",\"summary\":" + jstr(summary) + ",\"markdown_content\":" + jstr(md) + "}";
    return true;
  }

  // 自动/手动入池：只写知识条，不写 user_learning
  // v1.5.0-T008：仅「首次自动入池 + 常见身份」可写 public；手动保存一律 private
  int64_t maybeIngest(int uid, const std::string& text, const std::string& type,
                      const std::string& identity, const std::string& summary,
                      const std::string& md, bool force)
  {
    if (uid <= 0) return 0;
    bool autoPub = false;
    if (!force) {
      if (!autoIngest_) return 0;
      if (!contains(shareIdentities_, identity)) return 0;
      if (!contains(shareTypes_, type)) return 0;
      autoPub = true;
    }
    std::string key = clampKey(KnowledgeService::normalizeKey(text));
    std::string standard = summary.empty() ? text.substr(0, 200) : summary;
    bool solidified = !md.empty();
    auto w = db_.exec(
        "INSERT IGNORE INTO knowledge_points (user_id,user_input_content,standard_content,normalized_key,"
        "identity_snapshot,is_solidified,visibility,origin,solidify_time) VALUES (" +
        std::to_string(uid) + "," + q(text) + "," + q(standard) + "," + q(key) + "," + q(identity) +
        "," + (solidified ? "1" : "0") + "," + (autoPub ? "'public'" : "'private'") + "," + q(shareOrigin_) +
        "," + (solidified ? "NOW()" : "NULL") + ")");
    if (w.affected_rows != 1 || w.last_insert_id <= 0) return 0;
    int64_t kpId = w.last_insert_id;
    if (solidified) {
      db_.exec("INSERT INTO kp_extras (knowledge_id,extra_type,content,display_order) VALUES (" +
               std::to_string(kpId) + ",'markdown_content'," + q(md) + ",0)");
    }
    return kpId;
  }

  void failItem(int64_t itemId, int64_t explainId, const std::string& err)
  {
    db_.exec("UPDATE ai_task_items SET status='failed', error=" + q(err) + ", finished_at=NOW() WHERE id=" +
             std::to_string(itemId));
    db_.exec("UPDATE explanations SET status='failed', error_msg=" + q(err.substr(0, 240)) +
             ", finished_at=NOW() WHERE id=" + std::to_string(explainId));
  }

  static bool contains(const std::vector<std::string>& v, const std::string& s)
  {
    return std::find(v.begin(), v.end(), s) != v.end();
  }
};

}  // namespace eng
