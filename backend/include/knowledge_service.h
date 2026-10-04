// 知识点业务服务层（阶段二）
// 职责：录入、查重、AI 固化拓展、掌握程度判定、Word 导出
#pragma once
#include "db.h"
#include "ai_client.h"
#include "auth.h"
#include "json.h"
#include "prompts.h"
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <sys/wait.h>

namespace eng {

struct KnowledgePoint {
  int64_t id=0;
  int user_id=1;
  std::string user_input_content;
  std::string standard_content;
  int difficulty=0;
  std::string master_require;
  std::string remark;
  int is_solidified=0;
  std::string create_time;
  std::string solidify_time;
};

class KnowledgeService {
  Db& db_;
  AiClient& ai_;
  std::string wordTmpDir_;
public:
  KnowledgeService(Db& db, AiClient& ai, const std::string& wordTmp)
    : db_(db), ai_(ai), wordTmpDir_(wordTmp) {}

  // 原子任务1：知识点录入（单条，未固化不查重）
  struct CreateResult { int64_t id=0; bool ok=false; std::string error; bool duplicate=false; };
  CreateResult create(const std::string& userInput, const std::string& remark="") {
    CreateResult r;
    if (userInput.empty()) { r.error = "user_input_content empty"; return r; }
    // 未固化不查重；已固化语义查重
    // 简化：仅检查已固化知识点中是否存在相同 user_input 或 standard
    // PRD §3.5: 未固化不查重，允许重复录入
    std::ostringstream sql;
    sql << "INSERT INTO knowledge_points (user_id, user_input_content, remark, is_solidified, create_time) "
        << "VALUES (" << RequestContext::currentUserId() << ", '" << Db::escape(userInput) << "', "
        << (remark.empty() ? "NULL" : "'" + Db::escape(remark) + "'") << ", "
        << "0, NOW())";
    try {
      auto wr = db_.exec(sql.str());
      r.id = wr.last_insert_id;
      r.ok = true;
      return r;
    } catch (const std::exception& e) {
      r.error = e.what();
      return r;
    }
  }

  // 列表（分页）
  std::vector<Row> list(int page=1, int size=20, bool onlyUnsolidified=false) {
    std::ostringstream sql;
    sql << "SELECT id, user_input_content, standard_content, difficulty, master_require, remark, "
           "is_solidified, DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time, "
           "DATE_FORMAT(solidify_time, '%Y-%m-%d %H:%i:%s') AS solidify_time "
           "FROM knowledge_points WHERE user_id=" << RequestContext::currentUserId();
    if (onlyUnsolidified) sql << " AND is_solidified=0";
    sql << " ORDER BY id DESC LIMIT " << ((page-1)*size) << ", " << size;
    return db_.query(sql.str());
  }

  // 详情（含子表）
  JsonValue detail(int64_t id) {
    std::ostringstream sql;
    sql << "SELECT id, user_input_content, standard_content, difficulty, master_require, remark, "
           "is_solidified, DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time, "
           "DATE_FORMAT(solidify_time, '%Y-%m-%d %H:%i:%s') AS solidify_time "
           "FROM knowledge_points WHERE id=" << id
           << " AND user_id=" << RequestContext::currentUserId();
    auto rows = db_.query(sql.str());
    if (rows.empty()) return JsonValue::makeNull();
    auto& r = rows[0];
    JsonObject obj;
    obj["id"] = JsonValue::makeInt(std::stoll(r["id"]));
    obj["user_input_content"] = JsonValue::makeString(r["user_input_content"]);
    obj["standard_content"] = JsonValue::makeString(r["standard_content"]);
    obj["difficulty"] = r["difficulty"].empty() ? JsonValue::makeNull() : JsonValue::makeInt(std::stoi(r["difficulty"]));
    obj["master_require"] = r["master_require"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["master_require"]);
    obj["remark"] = r["remark"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["remark"]);
    obj["is_solidified"] = JsonValue::makeInt(std::stoi(r["is_solidified"]));
    obj["create_time"] = JsonValue::makeString(r["create_time"]);
    obj["solidify_time"] = r["solidify_time"].empty() ? JsonValue::makeNull() : JsonValue::makeString(r["solidify_time"]);

    // 子表
    auto loadArr = [&](const std::string& table, const std::vector<std::string>& cols) {
      std::ostringstream s;
      s << "SELECT ";
      for (size_t i = 0; i < cols.size(); i++) { if (i) s << ","; s << cols[i]; }
      s << " FROM " << table << " WHERE knowledge_id=" << id << " ORDER BY display_order, id";
      auto rs = db_.query(s.str());
      JsonArray a;
      for (auto& row : rs) {
        JsonObject o;
        for (auto& c : cols) o[c] = JsonValue::makeString(row[c]);
        a.push_back(JsonValue::makeObject(o));
      }
      return JsonValue::makeArray(a);
    };

    obj["definitions"] = loadArr("kp_definitions", {"id","pos","meaning","example"});
    obj["collocations"] = loadArr("kp_collocations", {"id","collocation","example"});
    obj["synonyms"] = loadArr("kp_synonyms", {"id","synonym","usage_diff"});
    obj["antonyms"] = loadArr("kp_antonyms", {"id","antonym"});
    obj["extras"] = loadArr("kp_extras", {"id","extra_type","content"});
    return JsonValue::makeObject(obj);
  }

  // 原子任务2：AI 一键批量固化（仅未固化知识点）
  struct SolidifyResult {
    int total=0, success=0, failed=0;
    std::vector<std::string> failedReasons;
    std::vector<std::string> wordPaths;
  };
  SolidifyResult solidifyAll() {
    SolidifyResult r;
    auto unsolidified = db_.query("SELECT id, user_input_content, remark FROM knowledge_points WHERE is_solidified=0 AND user_id="
                                  + std::to_string(RequestContext::currentUserId()) + " ORDER BY id");
    r.total = unsolidified.size();
    for (auto& row : unsolidified) {
      int64_t id = std::stoll(row["id"]);
      std::string err;
      bool ok = solidifyOne(id, row["user_input_content"], row["remark"], err);
      if (ok) {
        r.success++;
        // Word 导出
        std::string wp = exportWord(id);
        if (!wp.empty()) r.wordPaths.push_back(wp);
      } else {
        r.failed++;
        r.failedReasons.push_back("kp#" + std::to_string(id) + ": " + err);
      }
    }
    return r;
  }

  // 跨用户缓存判定结果（solidifyOne / solidifyBatch 共用）
  enum class CacheHit { Miss, Merged, Copied };

  // 全局查重：相同 standard_content 的已固化词条（不限 user_id）
  // Copied：他人固化内容已复制到当前 kp；Merged：自己已有副本，当前 kp 已删除合并
  // Miss：无缓存可用（复制失败也按 Miss 处理，回退到 AI 流程）
  CacheHit tryCrossUserCache(int64_t id, const std::string& userInput,
                             const std::string& remark, std::string& err)
  {
    // 同时查 standard_content 和 user_input_content（AI 可能润色 standard_content，
    // 但 user_input_content 是用户原始输入，更稳定）
    auto dup = db_.query(
      "SELECT id, user_id FROM knowledge_points WHERE "
      "(standard_content='" + Db::escape(userInput) + "' "
      " OR user_input_content='" + Db::escape(userInput) + "') "
      "AND is_solidified=1 AND id<>" + std::to_string(id) + " LIMIT 1"
    );
    if (dup.empty()) return CacheHit::Miss;

    int64_t dupId = std::stoll(dup[0]["id"]);
    int dupOwner = std::stoi(dup[0]["user_id"]);
    if (dupOwner == RequestContext::currentUserId())
    {
      db_.exec("DELETE FROM knowledge_points WHERE id=" + std::to_string(id));
      err = "merged into kp#" + dup[0]["id"];
      return CacheHit::Merged;
    }
    std::string cacheErr;
    if (copySolidifyContent(dupId, id, cacheErr))
    {
      recordCrossUserCache(id, userInput, remark, dupId);
      return CacheHit::Copied;
    }
    // 复制失败：回退 AI（不阻断用户）
    return CacheHit::Miss;
  }

  // 单条 AI 固化
  bool solidifyOne(int64_t id, const std::string& userInput, const std::string& remark, std::string& err) {
    // 跨用户查重：相同 standard_content 的已固化词条（不限 user_id）
    CacheHit hit = tryCrossUserCache(id, userInput, remark, err);
    if (hit == CacheHit::Merged) return true;
    if (hit == CacheHit::Copied) return true;
    // Miss：继续 AI 流程

    // AI 响应缓存检查：ai_tasks 表中是否有 done 状态、相同输入的任务
    std::string cacheKey = userInput + "||" + remark;
    auto cached = db_.query(
      "SELECT result FROM ai_tasks WHERE task_type='solidify_kp' AND status='done' "
      "AND ref_id=" + std::to_string(id) + " AND payload='" + Db::escape(cacheKey) + "' LIMIT 1"
    );

    // 记录 AI 任务（pending）
    int64_t taskId = 0;
    try {
      auto wr = db_.exec(
        "INSERT INTO ai_tasks (task_type, ref_id, status, payload) VALUES ('solidify_kp', "
        + std::to_string(id) + ", 'pending', '" + Db::escape(cacheKey) + "')"
      );
      taskId = wr.last_insert_id;
    } catch (...) {}

    // 断网降级检查
    if (ai_.isDegraded()) {
      err = "AI gateway unreachable (degraded)";
      if (taskId) db_.exec("UPDATE ai_tasks SET status='failed', error_msg='" + Db::escape(err) + "' WHERE id=" + std::to_string(taskId));
      return false;
    }

    // 构造 AI 提示：使用 prompts.h 模板（注释与提示词内容统一在 prompts.h 维护）
    std::string remarkLine;
    if (!remark.empty()) {
      remarkLine = prompts::replace(prompts::KP_REMARK_RULE, "{REMARK}", remark);
    }
    std::string userPrompt = prompts::buildTemplate(
      prompts::KP_SOLIDIFY_ONE_USER,
      {
        {"{KP_SIX_SECTIONS}", prompts::KP_SIX_SECTIONS},
        {"{KP_RULES}",       prompts::KP_RULES},
        {"{REMARK_LINE}",    remarkLine},
        {"{KP_JSON_SCHEMA}", prompts::KP_JSON_SCHEMA},
        {"{KP_JSON_SCHEMA}", prompts::KP_JSON_SCHEMA},
        {"{LEARNER_IDENTITY}", prompts::getLearnerIdentity(db_, RequestContext::currentUserId())},
        {"{USER_INPUT}",     userInput}
      }
    );
    std::vector<std::map<std::string,std::string>> msgs = {
      {{"role","system"},{"content",prompts::KP_SOLIDIFY_SYSTEM}},
      {{"role","user"},{"content",userPrompt}}
    };
    auto resp = ai_.postChat(msgs, true, 2048, 0.3);
    if (!resp.ok) {
      err = "AI call failed: " + resp.body;
      if (taskId) db_.exec("UPDATE ai_tasks SET status='failed', error_msg='" + Db::escape(err) + "', retry_times=retry_times+1 WHERE id=" + std::to_string(taskId));
      return false;
    }
    JsonValue json;
    try {
      if (!resp.extractedJson.empty()) json = parseJson(resp.extractedJson);
      else { err = "AI returned no JSON"; if (taskId) db_.exec("UPDATE ai_tasks SET status='failed', error_msg='" + Db::escape(err) + "', retry_times=retry_times+1 WHERE id=" + std::to_string(taskId)); return false; }
    } catch (const std::exception& e) {
      err = std::string("JSON parse failed: ") + e.what();
      if (taskId) db_.exec("UPDATE ai_tasks SET status='failed', error_msg='" + Db::escape(err) + "', retry_times=retry_times+1 WHERE id=" + std::to_string(taskId));
      return false;
    }
    if (!json.isObject()) { err = "AI JSON not object"; if (taskId) db_.exec("UPDATE ai_tasks SET status='failed', error_msg='" + Db::escape(err) + "', retry_times=retry_times+1 WHERE id=" + std::to_string(taskId)); return false; }

    bool ok = applySolidify(id, userInput, json, err);
    if (ok && taskId) {
      // 成功：存 result 用于缓存 + 改 done
      try {
        std::string resultStr = json.dump();
        db_.exec("UPDATE ai_tasks SET status='done', result='" + Db::escape(resultStr) + "' WHERE id=" + std::to_string(taskId));
      } catch (...) {
        db_.exec("UPDATE ai_tasks SET status='done' WHERE id=" + std::to_string(taskId));
      }
    } else if (!ok && taskId) {
      db_.exec("UPDATE ai_tasks SET status='failed', error_msg='" + Db::escape(err) + "', retry_times=retry_times+1 WHERE id=" + std::to_string(taskId));
    }
    return ok;
  }

  // 跨用户复用：将 src 已固化知识点完整复制到 dst（dst 的 id/user_id 保持不变）
  bool copySolidifyContent(int64_t srcId, int64_t dstId, std::string& err)
  {
    auto src = db_.query("SELECT id FROM knowledge_points WHERE id=" + std::to_string(srcId)
                         + " AND is_solidified=1 LIMIT 1");
    if (src.empty()) { err = "source not found"; return false; }
    bool ok = db_.transaction([&]() -> bool
    {
      // 主表：复制 standard/difficulty/master，dst 标记已固化
      db_.exec(
        "UPDATE knowledge_points dst JOIN knowledge_points src ON src.id=" + std::to_string(srcId)
        + " SET dst.standard_content=src.standard_content, dst.difficulty=src.difficulty, "
          "dst.master_require=src.master_require, dst.is_solidified=1, dst.solidify_time=NOW() "
        + " WHERE dst.id=" + std::to_string(dstId)
      );
      // 清 dst 旧子表
      db_.exec("DELETE FROM kp_definitions WHERE knowledge_id=" + std::to_string(dstId));
      db_.exec("DELETE FROM kp_collocations WHERE knowledge_id=" + std::to_string(dstId));
      db_.exec("DELETE FROM kp_synonyms WHERE knowledge_id=" + std::to_string(dstId));
      db_.exec("DELETE FROM kp_antonyms WHERE knowledge_id=" + std::to_string(dstId));
      db_.exec("DELETE FROM kp_extras WHERE knowledge_id=" + std::to_string(dstId));
      // 从 src 复制全部子表
      db_.exec("INSERT INTO kp_definitions (knowledge_id, pos, meaning, example, display_order) "
               "SELECT " + std::to_string(dstId) + ", pos, meaning, example, display_order "
               "FROM kp_definitions WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_collocations (knowledge_id, collocation, example, display_order) "
               "SELECT " + std::to_string(dstId) + ", collocation, example, display_order "
               "FROM kp_collocations WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_synonyms (knowledge_id, synonym, usage_diff, display_order) "
               "SELECT " + std::to_string(dstId) + ", synonym, usage_diff, display_order "
               "FROM kp_synonyms WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_antonyms (knowledge_id, antonym, display_order) "
               "SELECT " + std::to_string(dstId) + ", antonym, display_order "
               "FROM kp_antonyms WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_extras (knowledge_id, extra_type, content, display_order) "
               "SELECT " + std::to_string(dstId) + ", extra_type, content, display_order "
               "FROM kp_extras WHERE knowledge_id=" + std::to_string(srcId));
      return true;
    });
    if (!ok) err = "transaction failed";
    return ok;
  }

  // 记录跨用户复用任务（ai_tasks，标记来源，便于追溯）
  void recordCrossUserCache(int64_t kpId, const std::string& userInput,
                            const std::string& remark, int64_t srcId)
  {
    std::string payload = userInput + "||" + remark;
    try
    {
      db_.exec(
        "INSERT INTO ai_tasks (task_type, ref_id, status, payload, result) VALUES ('solidify_kp', "
        + std::to_string(kpId) + ", 'done', '" + Db::escape(payload)
        + "', '{\"source\":\"cross-user-cache\",\"source_kp\":" + std::to_string(srcId) + "}')"
      );
    }
    catch (...) {}
  }

  // 公共入库逻辑（solidifyOne 和 solidifyBatch 共用）
  bool applySolidify(int64_t id, const std::string& userInput, const JsonValue& json, std::string& err) {
    std::string stdContent = json.getString("standard_content", userInput);
    if (stdContent.empty() || stdContent.size() > 100) stdContent = userInput;
    // 同时查 standard_content 和 user_input_content（AI 可能润色 standard_content，
    // 但 user_input_content 是用户原始输入，更稳定）
    auto dup = db_.query(
      "SELECT id, user_id FROM knowledge_points WHERE "
      "(standard_content='" + Db::escape(stdContent) + "' "
      " OR user_input_content='" + Db::escape(userInput) + "') "
      "AND is_solidified=1 AND id<>" + std::to_string(id) + " LIMIT 1"
    );
    if (!dup.empty() && std::stoi(dup[0]["user_id"]) == RequestContext::currentUserId())
    {
      // 自己已有同名固化副本：合并
      db_.exec("DELETE FROM knowledge_points WHERE id=" + std::to_string(id));
      err = "merged into kp#" + dup[0]["id"];
      return true;
    }
    // 命中他人固化：不合并、不删除，继续用本次 AI 数据写入自己的知识点

    // 掌握程度判定（代码按难度自动判定，不信任AI返回值）
    int difficulty = (int)json.getInt("difficulty", 3);
    if (difficulty < 1) difficulty = 1;
    if (difficulty > 5) difficulty = 5;
    std::string master = (difficulty >= 5) ? "会读" : "会写";

    // 事务：更新主表 + 写子表
    bool txOk = db_.transaction([&]() -> bool {
      // 更新主表
      std::ostringstream up;
      up << "UPDATE knowledge_points SET standard_content='" << Db::escape(stdContent)
         << "', difficulty=" << difficulty
         << ", master_require='" << Db::escape(master)
         << "', is_solidified=1, solidify_time=NOW() WHERE id=" << id;
      if (db_.exec(up.str()).affected_rows == 0 && db_.query("SELECT id FROM knowledge_points WHERE id=" + std::to_string(id)).empty()) return false;

      // 清旧子表（兼容重固化）
      db_.exec("DELETE FROM kp_definitions WHERE knowledge_id=" + std::to_string(id));
      db_.exec("DELETE FROM kp_collocations WHERE knowledge_id=" + std::to_string(id));
      db_.exec("DELETE FROM kp_synonyms WHERE knowledge_id=" + std::to_string(id));
      db_.exec("DELETE FROM kp_antonyms WHERE knowledge_id=" + std::to_string(id));
      db_.exec("DELETE FROM kp_extras WHERE knowledge_id=" + std::to_string(id));

      int order = 0;
      // 释义
      auto* defs = json.find("definitions");
      if (defs && defs->isArray()) {
        for (auto& d : defs->asArray()) {
          std::ostringstream s;
          s << "INSERT INTO kp_definitions (knowledge_id, pos, meaning, example, display_order) VALUES ("
            << id << ", "
            << "'" << Db::escape(d.getString("pos")) << "', "
            << "'" << Db::escape(d.getString("meaning")) << "', "
            << "'" << Db::escape(d.getString("example")) << "', "
            << (order++) << ")";
          db_.exec(s.str());
        }
      }
      order = 0;
      auto* cols = json.find("collocations");
      if (cols && cols->isArray()) {
        for (auto& c : cols->asArray()) {
          std::ostringstream s;
          s << "INSERT INTO kp_collocations (knowledge_id, collocation, example, display_order) VALUES ("
            << id << ", "
            << "'" << Db::escape(c.getString("collocation")) << "', "
            << "'" << Db::escape(c.getString("example")) << "', "
            << (order++) << ")";
          db_.exec(s.str());
        }
      }
      order = 0;
      auto* syns = json.find("synonyms");
      if (syns && syns->isArray()) {
        for (auto& s : syns->asArray()) {
          std::ostringstream sql;
          sql << "INSERT INTO kp_synonyms (knowledge_id, synonym, display_order) VALUES ("
              << id << ", '" << Db::escape(s.asString()) << "', " << (order++) << ")";
          db_.exec(sql.str());
        }
      }
      order = 0;
      auto* ants = json.find("antonyms");
      if (ants && ants->isArray()) {
        for (auto& a : ants->asArray()) {
          std::ostringstream sql;
          sql << "INSERT INTO kp_antonyms (knowledge_id, antonym, display_order) VALUES ("
              << id << ", '" << Db::escape(a.asString()) << "', " << (order++) << ")";
          db_.exec(sql.str());
        }
      }
      // 扩展（音标/词根/同源词汇/典故俗语/同义词辨析/短语解析/重难点/频率/语域/文化/同类词语）
      int exOrder = 0;
      // 识别 AI 对不适用项返回的“无此内容”空话（应略过而非入库）
      auto isEmptySaying = [](const std::string& raw) -> bool {
        std::string s;
        for (char c : raw) {
          unsigned char uc = (unsigned char)c;
          if (std::isalnum(uc)) s += (char)std::tolower(uc);
        }
        if (s.size() > 60) return false;
        if (s == "none" || s == "na" || s == "n/a" || s == "无" || s == "暂无" ||
            s == "没有" || s == "不适用" || s == "noneed" || s == "无相关内容") return true;
        if (s.rfind("thereisno", 0) == 0 || s.rfind("thereareno", 0) == 0 ||
            s.rfind("thereisnot", 0) == 0 || s.rfind("therearenot", 0) == 0) return true;
        // 精确针对各结构板块的“无内容”说法
        static const std::vector<std::string> noPats = {
          "nospecific", "nophraseanalysis", "noallusions", "nocollocations",
          "nosynonyms", "noantonyms", "nowordfamily", "norelatedwords",
          "noadditional", "nofurther", "noother", "noextra", "nodirect",
          "notapplicable", "notneeded"
        };
        for (const auto& p : noPats) if (s.rfind(p, 0) == 0) return true;
        return false;
      };
      auto addExtra = [&](const std::string& type, const std::string& content) {
        if (!content.empty() && !isEmptySaying(content)) {
          db_.exec("INSERT INTO kp_extras (knowledge_id, extra_type, content, display_order) VALUES (" + std::to_string(id) + ", '" + type + "', '" + Db::escape(content) + "', " + std::to_string(exOrder++) + ")");
        }
      };
      // 兼容 AI 把字段返回为字符串或字符串数组
      auto flexStr = [&](const std::string& key) -> std::string {
        auto* v = json.find(key);
        if (!v) return "";
        if (v->isString()) return v->asString();
        if (v->isArray()) {
          std::string out;
          for (auto& e : v->asArray()) {
            std::string item = e.isString() ? e.asString() : e.dump();
            if (item.empty()) continue;
            if (!out.empty()) out += "\n";
            out += item;
          }
          return out;
        }
        return "";
      };
      // 顺序对应六板块：音标 → 词根词源 → 同源词汇 → 典故俗语 → 辨析/短语/重难点 → 频率/语域/文化/同类
      addExtra("phonetic", json.getString("phonetic"));
      addExtra("root", json.getString("root_analysis"));
      addExtra("word_family", flexStr("word_family"));
      addExtra("allusions", flexStr("allusions"));
      addExtra("synonym_analysis", json.getString("synonym_analysis"));
      addExtra("phrase_analysis", json.getString("phrase_analysis"));
      addExtra("key_points", json.getString("key_points"));
      addExtra("frequency", json.getString("frequency_note"));
      addExtra("register", json.getString("register_note"));
      addExtra("cultural", json.getString("cultural_note"));
      addExtra("related_words", flexStr("related_words"));
      return true;
    });
    if (!txOk) { err = "transaction failed"; return false; }
    return true;
  }

  // 原子任务2.5：导出知识点 Word（调 Node.js 子进程生成 .docx）
  std::string exportWord(int64_t kpId) {
    auto detailJson = detail(kpId);
    if (detailJson.isNull()) return "";
    std::string jsonStr = detailJson.dump();
    // 调 Node.js 子进程：node scripts/gen_word_kp.js <kpId> <jsonPath>
    // 写临时 JSON 到 word_tmp
    std::string tmpPath = wordTmpDir_ + "/kp_" + std::to_string(kpId) + "_" + std::to_string(::time(nullptr)) + ".json";
    std::ofstream f(tmpPath);
    f << jsonStr;
    f.close();

    std::string scriptPath = "/Users/junwang/Desktop/王浩源/英语/英语学习应用/scripts/gen_word_kp.js";
    std::string cmd = "/Users/junwang/.local/bin/node " + scriptPath + " " + std::to_string(kpId) + " " + tmpPath + " " + wordTmpDir_;
    int rc = std::system(cmd.c_str());
    if (rc != 0) return "";
    // 返回生成的 docx 路径
    std::string outPath = wordTmpDir_ + "/knowledge_point_" + std::to_string(kpId) + ".docx";
    if (::access(outPath.c_str(), F_OK) != 0) return "";
    return outPath;
  }

  // 批量 AI 固化（一次AI调用处理多个知识点，减少对接次数）
  struct BatchResult { int success=0; int failed=0; std::string error; };
  BatchResult solidifyBatch(const std::vector<int64_t>& ids) {
    BatchResult r;
    if (ids.empty()) { r.error = "no ids"; return r; }

    // 收集未固化知识点
    struct KpInfo { int64_t id; std::string input; std::string remark; };
    std::vector<KpInfo> kps;
    for (auto id : ids) {
      auto rows = db_.query("SELECT user_input_content, remark FROM knowledge_points WHERE id=" + std::to_string(id) + " AND is_solidified=0");
      if (!rows.empty()) kps.push_back({id, rows[0]["user_input_content"], rows[0]["remark"]});
    }
    if (kps.empty()) { r.error = "no unsolidified knowledge points"; return r; }

    // 先做跨用户缓存分流：命中的本地复制（零AI），未命中的才进入批量AI调用
    std::vector<KpInfo> needAi;
    for (auto& kp : kps)
    {
      std::string cacheErr;
      CacheHit hit = tryCrossUserCache(kp.id, kp.input, kp.remark, cacheErr);
      if (hit == CacheHit::Copied || hit == CacheHit::Merged) r.success++;
      else needAi.push_back(kp);
    }
    if (needAi.empty()) return r;
    kps = std::move(needAi);

    // 构造批量提示词：使用 prompts.h 模板（注释与提示词内容统一在 prompts.h 维护）
    std::ostringstream wordList;
    int idx = 0;
    for (auto& kp : kps) {
      wordList << (++idx) << ". "" << kp.input << """;
      if (!kp.remark.empty()) wordList << "（备注:" << kp.remark << "）";
      wordList << "\n";
    }
    std::string userPrompt = prompts::buildTemplate(
      prompts::KP_SOLIDIFY_BATCH_USER,
      {
        {"{KP_SIX_SECTIONS}", prompts::KP_SIX_SECTIONS},
        {"{KP_RULES}",       prompts::KP_RULES},
        {"{KP_JSON_SCHEMA}", prompts::KP_JSON_SCHEMA},
        {"{WORD_LIST}",      wordList.str()},
        {"{LEARNER_IDENTITY}", prompts::getLearnerIdentity(db_, RequestContext::currentUserId())},
      }
    );
    std::vector<std::map<std::string,std::string>> msgs = {
      {{"role","system"},{"content",prompts::KP_SOLIDIFY_BATCH_SYSTEM}},
      {{"role","user"},{"content",userPrompt}}
    };
    int maxTokens = 1024 * std::min((int)kps.size(), 8);
    auto resp = ai_.postChat(msgs, true, maxTokens, 0.3);
    if (!resp.ok) { r.error = "AI batch call failed: " + resp.body; return r; }

    JsonValue json;
    try {
      if (!resp.extractedJson.empty()) json = parseJson(resp.extractedJson);
      else { r.error = "AI returned no JSON"; return r; }
    } catch (const std::exception& e) {
      r.error = std::string("JSON parse failed: ") + e.what();
      return r;
    }

    if (json.isArray()) {
      auto& arr = json.asArray();
      for (size_t i = 0; i < kps.size() && i < arr.size(); i++) {
        std::string subErr;
        if (applySolidify(kps[i].id, kps[i].input, arr[i], subErr)) r.success++;
        else r.failed++;
      }
    } else if (json.isObject() && kps.size() == 1) {
      std::string subErr;
      if (applySolidify(kps[0].id, kps[0].input, json, subErr)) r.success++;
      else r.failed++;
    } else {
      r.error = "AI JSON not array";
    }
    return r;
  }

  // 批量导出知识点拓展 Word（多知识点合并为一个文档）
  std::string exportBatchWord(const std::vector<int64_t>& ids) {
    if (ids.empty()) return "";
    // 收集所有知识点详情，构造 JSON 数组
    std::string jsonStr = "[";
    bool first = true;
    for (auto id : ids) {
      auto detailJson = detail(id);
      if (detailJson.isNull()) continue;
      if (!first) jsonStr += ",";
      jsonStr += detailJson.dump();
      first = false;
    }
    jsonStr += "]";
    if (first) return ""; // 没有有效数据

    std::string tmpPath = wordTmpDir_ + "/kp_batch_" + std::to_string(::time(nullptr)) + ".json";
    std::ofstream f(tmpPath);
    f << jsonStr;
    f.close();

    std::string scriptPath = "/Users/junwang/Desktop/王浩源/英语/英语学习应用/scripts/gen_word_kp.js";
    std::string cmd = "/Users/junwang/.local/bin/node " + scriptPath + " batch " + tmpPath + " " + wordTmpDir_;
    int rc = std::system(cmd.c_str());
    if (rc != 0) return "";
    std::string outPath = wordTmpDir_ + "/knowledge_points_batch.docx";
    if (::access(outPath.c_str(), F_OK) != 0) return "";
    return outPath;
  }

  // 原子任务4：人工编辑标准词条（查重兜底，允许合并/拆分）
  bool updateStandardContent(int64_t id, const std::string& newStandard) {
    std::ostringstream sql;
    sql << "UPDATE knowledge_points SET standard_content='" << Db::escape(newStandard)
        << "' WHERE id=" << id << " AND is_solidified=1";
    return db_.exec(sql.str()).affected_rows > 0;
  }

  // 合并两个知识点（srcId 的子表数据复制到 dstId，src 软删除）
  bool merge(int64_t srcId, int64_t dstId) {
    if (srcId == dstId) return false;
    return db_.transaction([&]() -> bool {
      // 复制子表数据
      db_.exec("INSERT INTO kp_definitions (knowledge_id, pos, meaning, example, display_order) SELECT " + std::to_string(dstId) + ", pos, meaning, example, display_order FROM kp_definitions WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_collocations (knowledge_id, collocation, example, display_order) SELECT " + std::to_string(dstId) + ", collocation, example, display_order FROM kp_collocations WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_synonyms (knowledge_id, synonym, usage_diff, display_order) SELECT " + std::to_string(dstId) + ", synonym, usage_diff, display_order FROM kp_synonyms WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_antonyms (knowledge_id, antonym, display_order) SELECT " + std::to_string(dstId) + ", antonym, display_order FROM kp_antonyms WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_extras (knowledge_id, extra_type, content, display_order) SELECT " + std::to_string(dstId) + ", extra_type, content, display_order FROM kp_extras WHERE knowledge_id=" + std::to_string(srcId));
      // 删除源
      db_.exec("DELETE FROM kp_definitions WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("DELETE FROM kp_collocations WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("DELETE FROM kp_synonyms WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("DELETE FROM kp_antonyms WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("DELETE FROM kp_extras WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("DELETE FROM knowledge_points WHERE id=" + std::to_string(srcId));
      return true;
    });
  }

  // 拆分知识点：复制一份为新知识点（用户后续编辑）
  int64_t split(int64_t srcId) {
    auto d = detail(srcId);
    if (d.isNull()) return 0;
    auto src = db_.query("SELECT user_input_content, standard_content, difficulty, master_require, remark FROM knowledge_points WHERE id=" + std::to_string(srcId));
    if (src.empty()) return 0;
    std::ostringstream sql;
    sql << "INSERT INTO knowledge_points (user_id, user_input_content, standard_content, difficulty, master_require, remark, is_solidified, create_time, solidify_time) "
        << "SELECT 1, user_input_content, standard_content, difficulty, master_require, remark, is_solidified, NOW(), solidify_time FROM knowledge_points WHERE id=" << srcId;
    auto wr = db_.exec(sql.str());
    int64_t newId = wr.last_insert_id;
    if (newId > 0) {
      db_.exec("INSERT INTO kp_definitions (knowledge_id, pos, meaning, example, display_order) SELECT " + std::to_string(newId) + ", pos, meaning, example, display_order FROM kp_definitions WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_collocations (knowledge_id, collocation, example, display_order) SELECT " + std::to_string(newId) + ", collocation, example, display_order FROM kp_collocations WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_synonyms (knowledge_id, synonym, usage_diff, display_order) SELECT " + std::to_string(newId) + ", synonym, usage_diff, display_order FROM kp_synonyms WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_antonyms (knowledge_id, antonym, display_order) SELECT " + std::to_string(newId) + ", antonym, display_order FROM kp_antonyms WHERE knowledge_id=" + std::to_string(srcId));
      db_.exec("INSERT INTO kp_extras (knowledge_id, extra_type, content, display_order) SELECT " + std::to_string(newId) + ", extra_type, content, display_order FROM kp_extras WHERE knowledge_id=" + std::to_string(srcId));
    }
    return newId;
  }
};

} // namespace eng
