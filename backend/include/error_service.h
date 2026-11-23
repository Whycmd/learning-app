// 错题归因与重练队列服务（v1.5.1-T002 ERROR-RETRY）
// 职责：规则归因 → AI 异步兜底 → 错题本聚合 → 重练队列与奖励
// 配置：config/error_rules.json、config/rewards.json（retry 段）
#pragma once

#include "db.h"
#include "json.h"
#include "auth.h"
#include "prompts.h"
#include "ai_client.h"
#include "learning_service.h"
#include "treasury_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace eng {

class ErrorService
{
public:
  struct WrongInput {
    int userId = 0;
    int knowledgeId = 0;
    std::string sourceType = "exercise";  // exercise/quiz/retry
    std::string sourceId;
    int questionNo = 0;
    std::string questionType;
    std::string userAnswer;
    std::string correctAnswer;
    JsonValue options;                    // 选择题选项（可选）
  };

  struct Attribution {
    long long id = 0;
    std::string errorType = "unknown";
    std::string reason;
    std::string source = "fallback";
    bool pending = false;
    bool created = false;
  };

  ErrorService(Db& db, AiClient& ai, LearningService& learn, TreasuryService& treasury,
               const std::string& configPath = "config/error_rules.json")
    : db_(db), ai_(ai), learn_(learn), treasury_(treasury)
  {
    loadConfig(configPath);
  }

  // ========== 写入：答错归因 + 入队 ==========
  Attribution recordWrong(const WrongInput& in)
  {
    Attribution a;
    if (in.userId <= 0 || in.knowledgeId <= 0) return a;
    if (trim(in.userAnswer).empty()) return a;  // D5：未作答不计错题

    std::string sourceId = trim(in.sourceId);
    if (sourceId.empty()) {
      // 无批次定位时生成唯一值，避免唯一键把多次单题批改合并
      static std::atomic<long long> seq{0};
      sourceId = "adhoc-" + std::to_string(in.userId) + "-" +
                 std::to_string((long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count()) +
                 "-" + std::to_string(++seq);
    }

    std::string word = lookupWord(in.knowledgeId);

    // 1) 规则归因
    std::string errorType, reason;
    attributeByRules(in, word, errorType, reason);
    bool needAi = (errorType == "unknown") && aiEnabled_;

    std::string typeEsc = Db::escape(errorType);
    std::string srcType = Db::escape(in.sourceType.empty() ? "exercise" : in.sourceType);
    std::string srcIdEsc = Db::escape(sourceId);
    std::string qTypeEsc = Db::escape(in.questionType);
    std::string uaEsc = Db::escape(in.userAnswer);
    std::string caEsc = Db::escape(in.correctAnswer);
    std::string reasonEsc = Db::escape(reason);

    // 2) 去重写入（同题重复批改只更新）
    auto wr = db_.exec(
      "INSERT INTO error_attributions "
      "(user_id,knowledge_id,source_type,source_id,question_no,question_type,"
      "user_answer,correct_answer,error_type,rule_reason,ai_reason,attribution_source) VALUES ("
      + std::to_string(in.userId) + "," + std::to_string(in.knowledgeId) + ",'" + srcType + "','" + srcIdEsc + "',"
      + std::to_string(in.questionNo) + ",'" + qTypeEsc + "','" + uaEsc + "','" + caEsc + "','"
      + typeEsc + "','" + reasonEsc + "','','" + (needAi ? "fallback" : "rule") + "') "
      "ON DUPLICATE KEY UPDATE knowledge_id=VALUES(knowledge_id),question_type=VALUES(question_type),"
      "user_answer=VALUES(user_answer),correct_answer=VALUES(correct_answer),"
      "error_type=VALUES(error_type),rule_reason=VALUES(rule_reason),ai_reason='',"
      "attribution_source=VALUES(attribution_source),updated_at=NOW()");

    bool created = (wr.affected_rows == 1);
    long long aid = wr.last_insert_id;
    if (aid <= 0) {
      auto rows = db_.query(
        "SELECT id FROM error_attributions WHERE user_id=" + std::to_string(in.userId) +
        " AND source_type='" + srcType + "' AND source_id='" + srcIdEsc + "'"
        " AND question_no=" + std::to_string(in.questionNo) + " LIMIT 1");
      if (!rows.empty()) aid = std::stoll(rows[0]["id"]);
    }

    // 3) 入队/重置
    db_.exec(
      "INSERT INTO retry_items (user_id,knowledge_id,attribution_id,retry_count,consecutive_correct,"
      "next_retry_time,archived_at,status) VALUES ("
      + std::to_string(in.userId) + "," + std::to_string(in.knowledgeId) + "," + std::to_string(aid)
      + ",0,0,NOW(),NULL,'queued') "
      "ON DUPLICATE KEY UPDATE attribution_id=VALUES(attribution_id),consecutive_correct=0,"
      "next_retry_time=NOW(),archived_at=NULL,status='queued',updated_at=NOW()");

    a.id = aid;
    a.errorType = errorType;
    a.reason = reason;
    a.source = needAi ? "fallback" : "rule";
    a.pending = needAi;
    a.created = created;

    // 4) AI 兜底（异步，不阻塞答题）
    if (created && needAi && aid > 0) {
      scheduleAi(aid, in.userId, word, in.questionType, in.userAnswer, in.correctAnswer);
    }
    return a;
  }

  // ========== 查询：单条归因（AI 回填轮询） ==========
  JsonValue getAttribution(int uid, long long id)
  {
    auto rows = db_.query(
      "SELECT id,error_type,rule_reason,ai_reason,attribution_source,"
      "DATE_FORMAT(created_at,'%Y-%m-%d %H:%i') AS created_at "
      "FROM error_attributions WHERE id=" + std::to_string(id) +
      " AND user_id=" + std::to_string(uid) + " LIMIT 1");
    if (rows.empty()) return JsonValue::makeNull();
    auto& r = rows[0];
    JsonObject o;
    o["id"] = JsonValue::makeInt(std::stoll(r["id"]));
    o["error_type"] = JsonValue::makeString(r["error_type"]);
    o["error_label"] = JsonValue::makeString(errorLabel(r["error_type"]));
    o["reason"] = JsonValue::makeString(r["ai_reason"].empty() ? r["rule_reason"] : r["ai_reason"]);
    o["source"] = JsonValue::makeString(r["attribution_source"]);
    o["pending"] = JsonValue::makeBool(r["attribution_source"] == "fallback");
    o["created_at"] = JsonValue::makeString(r["created_at"]);
    return JsonValue::makeObject(o);
  }

  // ========== 修改归因 ==========
  bool updateAttributionType(int uid, long long id, const std::string& type)
  {
    if (!isValidType(type)) return false;
    auto wr = db_.exec(
      "UPDATE error_attributions SET error_type='" + Db::escape(type) +
      "',attribution_source='user',updated_at=NOW() WHERE id=" + std::to_string(id) +
      " AND user_id=" + std::to_string(uid));
    return wr.affected_rows > 0;
  }

  // ========== 错题本聚合 ==========
  JsonValue summary(int uid)
  {
    lazyArchive(uid);
    auto totalRow = db_.query(
      "SELECT COUNT(*) AS c FROM retry_items WHERE user_id=" + std::to_string(uid) +
      " AND status<>'archived'");
    auto archRow = db_.query(
      "SELECT COUNT(*) AS c FROM retry_items WHERE user_id=" + std::to_string(uid) +
      " AND status='archived'");
    auto dueRow = db_.query(
      "SELECT COUNT(*) AS c FROM retry_items WHERE user_id=" + std::to_string(uid) +
      " AND status='queued' AND next_retry_time<=NOW()");

    JsonObject byType;
    auto typeRows = db_.query(
      "SELECT x.error_type, COUNT(*) AS c FROM ("
      "  SELECT ea.knowledge_id,"
      "    (SELECT a2.error_type FROM error_attributions a2 WHERE a2.user_id=ea.user_id"
      "      AND a2.knowledge_id=ea.knowledge_id ORDER BY a2.id DESC LIMIT 1) AS error_type"
      "  FROM error_attributions ea WHERE ea.user_id=" + std::to_string(uid) +
      "  GROUP BY ea.knowledge_id"
      ") x JOIN retry_items ri ON ri.user_id=" + std::to_string(uid) +
      " AND ri.knowledge_id=x.knowledge_id AND ri.status<>'archived'"
      " GROUP BY x.error_type");
    for (auto& r : typeRows) byType[r["error_type"]] = JsonValue::makeInt(std::stoll(r["c"]));

    auto expRows = db_.query(
      "SELECT COALESCE(SUM(item_count),0) AS c FROM wordlists WHERE user_id=" +
      std::to_string(uid) + " AND title='错题导出'");

    JsonObject o;
    o["archived_count"] = JsonValue::makeInt(archRow.empty() ? 0 : std::stoll(archRow[0]["c"]));
    o["total_count"] = JsonValue::makeInt(
      (totalRow.empty() ? 0 : std::stoll(totalRow[0]["c"])) +
      (archRow.empty() ? 0 : std::stoll(archRow[0]["c"])));
    o["export_count"] = JsonValue::makeInt(expRows.empty() ? 0 : std::stoll(expRows[0]["c"]));
    // due_count 展示今日应重练，total_count 为未归档总数
    o["due_count"] = JsonValue::makeInt(dueRow.empty() ? 0 : std::stoll(dueRow[0]["c"]));
    o["by_type"] = JsonValue::makeObject(byType);
    return JsonValue::makeObject(o);
  }

  JsonValue wrongbookWords(int uid, const std::string& errorType, const std::string& sourceType,
                           int days, const std::string& keyword, bool includeArchived,
                           int page, int size)
  {
    lazyArchive(uid);
    if (page < 1) page = 1;
    if (size < 1 || size > 100) size = 20;

    std::string where = " WHERE 1=1";
    if (!errorType.empty()) {
      where += " AND (SELECT a3.error_type FROM error_attributions a3"
               " WHERE a3.user_id=" + std::to_string(uid) +
               " AND a3.knowledge_id=t.knowledge_id ORDER BY a3.id DESC LIMIT 1)='" +
               Db::escape(errorType) + "'";
    }
    if (!sourceType.empty()) {
      where += " AND EXISTS (SELECT 1 FROM error_attributions a4 WHERE a4.user_id=" +
               std::to_string(uid) + " AND a4.knowledge_id=t.knowledge_id"
               " AND a4.source_type='" + Db::escape(sourceType) + "')";
    }
    if (days > 0) {
      where += " AND t.last_error_at >= DATE_SUB(NOW(), INTERVAL " + std::to_string(days) + " DAY)";
    }
    if (!trim(keyword).empty()) {
      std::string k = Db::escape(trim(keyword));
      where += " AND (kp.standard_content LIKE '%" + k + "%' OR kp.user_input_content LIKE '%" + k + "%')";
    }
    if (!includeArchived) {
      where += " AND COALESCE(ri.status,'queued')<>'archived'";
    }

    std::string base =
      " FROM (SELECT ea.knowledge_id, COUNT(*) AS error_count, MAX(ea.created_at) AS last_error_at"
      "       FROM error_attributions ea WHERE ea.user_id=" + std::to_string(uid) +
      "       GROUP BY ea.knowledge_id) t"
      " LEFT JOIN knowledge_points kp ON kp.id=t.knowledge_id"
      " LEFT JOIN retry_items ri ON ri.user_id=" + std::to_string(uid) +
      " AND ri.knowledge_id=t.knowledge_id" + where;

    auto cntRows = db_.query("SELECT COUNT(*) AS c" + base);
    int total = cntRows.empty() ? 0 : std::stoi(cntRows[0]["c"]);

    auto rows = db_.query(
      "SELECT t.knowledge_id,t.error_count,"
      "DATE_FORMAT(t.last_error_at,'%Y-%m-%d %H:%i') AS last_error_at,"
      "(SELECT a2.error_type FROM error_attributions a2 WHERE a2.user_id=" + std::to_string(uid) +
      " AND a2.knowledge_id=t.knowledge_id ORDER BY a2.id DESC LIMIT 1) AS latest_error_type,"
      "COALESCE(NULLIF(kp.standard_content,''),kp.user_input_content,'') AS word,"
      "COALESCE((SELECT d.meaning FROM kp_definitions d WHERE d.knowledge_id=t.knowledge_id"
      " ORDER BY d.display_order LIMIT 1),'') AS meaning,"
      "ri.id AS retry_id,COALESCE(ri.status,'queued') AS retry_status,"
      "DATE_FORMAT(ri.next_retry_time,'%Y-%m-%d %H:%i') AS next_retry_time,"
      "COALESCE(ri.consecutive_correct,0) AS consecutive_correct,"
      "(ri.archived_at IS NOT NULL) AS archived" + base +
      " ORDER BY t.last_error_at DESC LIMIT " + std::to_string((page - 1) * size) + "," +
      std::to_string(size));

    JsonArray items;
    for (auto& r : rows) {
      JsonObject o;
      o["knowledge_id"] = JsonValue::makeInt(std::stoll(r["knowledge_id"]));
      o["word"] = JsonValue::makeString(r["word"]);
      o["meaning"] = JsonValue::makeString(r["meaning"]);
      o["error_count"] = JsonValue::makeInt(std::stoll(r["error_count"]));
      o["last_error_at"] = JsonValue::makeString(r["last_error_at"]);
      o["latest_error_type"] = JsonValue::makeString(r["latest_error_type"]);
      o["latest_error_label"] = JsonValue::makeString(errorLabel(r["latest_error_type"]));
      o["archived"] = JsonValue::makeBool(r["archived"] == "1");
      JsonObject roi;
      roi["id"] = JsonValue::makeInt(r["retry_id"].empty() ? 0 : std::stoll(r["retry_id"]));
      roi["status"] = JsonValue::makeString(r["retry_status"]);
      roi["next_retry_time"] = JsonValue::makeString(r["next_retry_time"]);
      roi["consecutive_correct"] = JsonValue::makeInt(std::stoll(r["consecutive_correct"]));
      o["retry"] = JsonValue::makeObject(roi);
      items.push_back(JsonValue::makeObject(o));
    }

    JsonObject out;
    out["items"] = JsonValue::makeArray(items);
    out["total"] = JsonValue::makeInt(total);
    out["page"] = JsonValue::makeInt(page);
    out["size"] = JsonValue::makeInt(size);
    return JsonValue::makeObject(out);
  }

  JsonValue wordDetail(int uid, int knowledgeId)
  {
    lazyArchive(uid);
    auto rows = db_.query(
      "SELECT id,source_type,source_id,question_no,question_type,user_answer,correct_answer,"
      "error_type,rule_reason,ai_reason,attribution_source,"
      "DATE_FORMAT(created_at,'%Y-%m-%d %H:%i') AS created_at "
      "FROM error_attributions WHERE user_id=" + std::to_string(uid) +
      " AND knowledge_id=" + std::to_string(knowledgeId) + " ORDER BY id DESC LIMIT 50");
    if (rows.empty()) return JsonValue::makeNull();

    JsonArray items;
    for (auto& r : rows) {
      JsonObject o;
      o["id"] = JsonValue::makeInt(std::stoll(r["id"]));
      o["source_type"] = JsonValue::makeString(r["source_type"]);
      o["source_id"] = JsonValue::makeString(r["source_id"]);
      o["question_no"] = JsonValue::makeInt(std::stoll(r["question_no"]));
      o["question_type"] = JsonValue::makeString(r["question_type"]);
      o["user_answer"] = JsonValue::makeString(r["user_answer"]);
      o["correct_answer"] = JsonValue::makeString(r["correct_answer"]);
      o["error_type"] = JsonValue::makeString(r["error_type"]);
      o["error_label"] = JsonValue::makeString(errorLabel(r["error_type"]));
      o["reason"] = JsonValue::makeString(r["ai_reason"].empty() ? r["rule_reason"] : r["ai_reason"]);
      o["source"] = JsonValue::makeString(r["attribution_source"]);
      o["created_at"] = JsonValue::makeString(r["created_at"]);
      items.push_back(JsonValue::makeObject(o));
    }

    auto retryRows = db_.query(
      "SELECT id,retry_count,consecutive_correct,status,"
      "DATE_FORMAT(next_retry_time,'%Y-%m-%d %H:%i') AS next_retry_time,"
      "DATE_FORMAT(archived_at,'%Y-%m-%d %H:%i') AS archived_at "
      "FROM retry_items WHERE user_id=" + std::to_string(uid) +
      " AND knowledge_id=" + std::to_string(knowledgeId) + " LIMIT 1");

    auto kpRows = db_.query(
      "SELECT COALESCE(NULLIF(standard_content,''),user_input_content,'') AS word,"
      "COALESCE((SELECT d.meaning FROM kp_definitions d WHERE d.knowledge_id=" +
      std::to_string(knowledgeId) + " ORDER BY d.display_order LIMIT 1),'') AS meaning "
      "FROM knowledge_points WHERE id=" + std::to_string(knowledgeId) + " LIMIT 1");

    JsonObject out;
    out["knowledge_id"] = JsonValue::makeInt(knowledgeId);
    out["word"] = JsonValue::makeString(kpRows.empty() ? "" : kpRows[0]["word"]);
    out["meaning"] = JsonValue::makeString(kpRows.empty() ? "" : kpRows[0]["meaning"]);
    out["items"] = JsonValue::makeArray(items);
    if (!retryRows.empty()) {
      auto& r = retryRows[0];
      JsonObject roi;
      roi["id"] = JsonValue::makeInt(std::stoll(r["id"]));
      roi["retry_count"] = JsonValue::makeInt(std::stoll(r["retry_count"]));
      roi["consecutive_correct"] = JsonValue::makeInt(std::stoll(r["consecutive_correct"]));
      roi["status"] = JsonValue::makeString(r["status"]);
      roi["next_retry_time"] = JsonValue::makeString(r["next_retry_time"]);
      roi["archived_at"] = JsonValue::makeString(r["archived_at"]);
      out["retry"] = JsonValue::makeObject(roi);
    }
    return JsonValue::makeObject(out);
  }

  // ========== 重练队列 ==========
  JsonValue retryQueue(int uid, bool dueOnly)
  {
    lazyArchive(uid);
    std::string cond = dueOnly
      ? " AND ri.status='queued' AND ri.next_retry_time<=NOW()"
      : " AND ri.status<>'archived'";
    auto rows = db_.query(
      "SELECT ri.id,ri.knowledge_id,ri.retry_count,ri.consecutive_correct,"
      "DATE_FORMAT(ri.next_retry_time,'%Y-%m-%d %H:%i') AS next_retry_time,"
      "COALESCE(NULLIF(kp.standard_content,''),kp.user_input_content,'') AS word,"
      "COALESCE((SELECT d.meaning FROM kp_definitions d WHERE d.knowledge_id=ri.knowledge_id"
      " ORDER BY d.display_order LIMIT 1),'') AS meaning "
      "FROM retry_items ri LEFT JOIN knowledge_points kp ON kp.id=ri.knowledge_id "
      "WHERE ri.user_id=" + std::to_string(uid) + cond +
      " ORDER BY ri.next_retry_time ASC, ri.id ASC LIMIT 200");

    JsonArray items;
    for (auto& r : rows) {
      JsonObject o;
      o["id"] = JsonValue::makeInt(std::stoll(r["id"]));
      o["knowledge_id"] = JsonValue::makeInt(std::stoll(r["knowledge_id"]));
      o["word"] = JsonValue::makeString(r["word"]);
      o["meaning"] = JsonValue::makeString(r["meaning"]);
      o["retry_count"] = JsonValue::makeInt(std::stoll(r["retry_count"]));
      o["consecutive_correct"] = JsonValue::makeInt(std::stoll(r["consecutive_correct"]));
      o["next_retry_time"] = JsonValue::makeString(r["next_retry_time"]);
      items.push_back(JsonValue::makeObject(o));
    }
    return JsonValue::makeArray(items);
  }

  bool advance(int uid, long long id)
  {
    // [FUNCTIONAL] 同一秒内重复提交时 next_retry_time/updated_at 无变化，affected_rows=0，
    // 需补查存在性，避免幂等请求被误判为失败（问题集合 #28）
    auto wr = db_.exec("UPDATE retry_items SET next_retry_time=NOW(),updated_at=NOW()"
                       " WHERE id=" + std::to_string(id) + " AND user_id=" + std::to_string(uid) +
                       " AND status='queued'");
    if (wr.affected_rows > 0) return true;
    auto rows = db_.query("SELECT id FROM retry_items WHERE id=" + std::to_string(id) +
                          " AND user_id=" + std::to_string(uid) +
                          " AND status='queued' LIMIT 1");
    return !rows.empty();
  }

  bool skip(int uid, long long id)
  {
    return db_.exec("UPDATE retry_items SET next_retry_time=DATE_ADD(NOW(),INTERVAL " +
                    std::to_string(skipDays_) + " DAY),updated_at=NOW()"
                    " WHERE id=" + std::to_string(id) + " AND user_id=" + std::to_string(uid) +
                    " AND status='queued'").affected_rows > 0;
  }

  // returnJson: {ok,error,archived,next_retry_time,consecutive_correct,reward:{...}}
  JsonValue complete(int uid, long long id, bool correct)
  {
    auto rows = db_.query(
      "SELECT id,knowledge_id,retry_count,consecutive_correct,status,"
      "(next_retry_time<=NOW()) AS due FROM retry_items WHERE id=" + std::to_string(id) +
      " AND user_id=" + std::to_string(uid) + " LIMIT 1");
    if (rows.empty()) return errValue("未找到该重练项");
    auto& it = rows[0];
    if (it["status"] == "archived") return errValue("该词已归档");
    if (it["due"] != "1") return errValue("未到重练时间");

    int knowledgeId = std::stoi(it["knowledge_id"]);
    int retryCount = std::stoi(it["retry_count"]);
    int cc = std::stoi(it["consecutive_correct"]);

    // 先占位，防并发重复完成
    auto claim = db_.exec(
      "UPDATE retry_items SET retry_count=retry_count+1,"
      "consecutive_correct=" + std::string(correct ? "consecutive_correct+1" : "0") +
      ",updated_at=NOW() WHERE id=" + std::to_string(id) +
      " AND user_id=" + std::to_string(uid) + " AND status='queued'"
      " AND next_retry_time<=NOW()");
    if (claim.affected_rows <= 0) return errValue("重练状态已变化，请刷新");

    int newCc = correct ? cc + 1 : 0;
    int newLevel = -1;
    int intervalDays = retryBaseDays_;
    bool archived = false;

    if (correct) {
      newLevel = learn_.adjustProficiency(knowledgeId, +1);
      if (newLevel < 0) newLevel = 0;
      learn_.completeReview(knowledgeId);
      intervalDays = std::max(retryBaseDays_, LearningService::calcIntervalDays(newLevel, "daily"));
    } else {
      learn_.onAnswerResult(knowledgeId, false);
      auto lr = learn_.getOrCreate(knowledgeId);
      if (!lr.isNull()) newLevel = (int)lr.getInt("proficiency_level");
      intervalDays = retryBaseDays_;
    }

    if (newCc >= archiveConsecutive_) {
      db_.exec("UPDATE retry_items SET status='archived',archived_at=NOW(),"
               "next_retry_time=DATE_ADD(NOW(),INTERVAL " + std::to_string(intervalDays) + " DAY),"
               "updated_at=NOW() WHERE id=" + std::to_string(id) +
               " AND user_id=" + std::to_string(uid));
      archived = true;
    } else {
      db_.exec("UPDATE retry_items SET next_retry_time=DATE_ADD(NOW(),INTERVAL " +
               std::to_string(intervalDays) + " DAY),status='queued',archived_at=NULL,updated_at=NOW()"
               " WHERE id=" + std::to_string(id) + " AND user_id=" + std::to_string(uid));
    }

    // 答错补一条归因（source=retry），不重复扣熟练度
    if (!correct) {
      WrongInput wi;
      wi.userId = uid;
      wi.knowledgeId = knowledgeId;
      wi.sourceType = "retry";
      wi.sourceId = std::to_string(id);
      wi.questionNo = retryCount + 1;
      wi.questionType = "retry";
      wi.userAnswer = "(重练答错)";
      wi.correctAnswer = lookupWord(knowledgeId);
      recordWrong(wi);
      db_.exec("UPDATE retry_items SET next_retry_time=DATE_ADD(NOW(),INTERVAL " +
               std::to_string(intervalDays) + " DAY),updated_at=NOW() WHERE id=" +
               std::to_string(id) + " AND user_id=" + std::to_string(uid));
    }

    // 奖励：完成一次 +5，日上限
    int earned = 0;
    int todayCount = treasury_.countTodayByReason(uid, "retry_done");
    int cap = treasury_.getRetryMaxPerDay();
    auto dup = db_.query(
      "SELECT id FROM user_coin_txn WHERE user_id=" + std::to_string(uid) +
      " AND reason_code='retry_done' AND ref_id=" + std::to_string(id) +
      " AND DATE(created_at)=CURDATE() LIMIT 1");
    if (todayCount < cap && dup.empty()) {
      if (treasury_.apply(treasury_.getRetryAmount(), "retry_done", (int)id)) {
        earned = treasury_.getRetryAmount();
        todayCount += 1;
      }
    }

    JsonObject reward;
    reward["amount"] = JsonValue::makeInt(earned);
    reward["count"] = JsonValue::makeInt(todayCount);
    reward["capped"] = JsonValue::makeBool(todayCount >= cap);

    JsonObject o;
    o["ok"] = JsonValue::makeBool(true);
    o["archived"] = JsonValue::makeBool(archived);
    o["consecutive_correct"] = JsonValue::makeInt(newCc);
    o["new_proficiency"] = JsonValue::makeInt(newLevel);
    o["reward"] = JsonValue::makeObject(reward);
    return JsonValue::makeObject(o);
  }

  bool archive(int uid, long long id)
  {
    return db_.exec("UPDATE retry_items SET status='archived',archived_at=NOW(),updated_at=NOW()"
                    " WHERE id=" + std::to_string(id) + " AND user_id=" + std::to_string(uid) +
                    " AND status<>'archived'").affected_rows > 0;
  }

  bool restore(int uid, long long id)
  {
    return db_.exec("UPDATE retry_items SET status='queued',archived_at=NULL,"
                    "consecutive_correct=0,next_retry_time=NOW(),updated_at=NOW()"
                    " WHERE id=" + std::to_string(id) + " AND user_id=" + std::to_string(uid) +
                    " AND status='archived'").affected_rows > 0;
  }

  static std::string errorLabel(const std::string& type)
  {
    if (type == "spelling") return "拼写";
    if (type == "meaning") return "词义";
    if (type == "collocation") return "搭配";
    if (type == "grammar") return "语法";
    return "未知";
  }

  static bool isValidType(const std::string& type)
  {
    return type == "spelling" || type == "meaning" || type == "collocation" ||
           type == "grammar" || type == "unknown";
  }

  // 供 main.cpp 判断配置
  bool aiEnabled() const { return aiEnabled_; }
  int aiTimeoutSec() const { return aiTimeoutSec_; }

private:
  Db& db_;
  AiClient& ai_;
  LearningService& learn_;
  TreasuryService& treasury_;

  int maxEdit_ = 2;
  int archiveConsecutive_ = 3;
  int archiveDays_ = 90;
  int retryBaseDays_ = 1;
  int skipDays_ = 1;
  int aiTimeoutSec_ = 15;
  int aiMaxTokens_ = 220;
  double aiTemperature_ = 0.2;
  bool aiEnabled_ = true;

  // ---------- 配置 ----------
  void loadConfig(const std::string& path)
  {
    std::ifstream f(path);
    if (!f) {
      std::cerr << "[error] config not found: " << path << " (use defaults)\n";
      return;
    }
    std::stringstream ss; ss << f.rdbuf();
    try {
      JsonValue root = parseJson(ss.str());
      maxEdit_ = (int)root.getInt("edit_distance_max", 2);
      archiveConsecutive_ = (int)root.getInt("archive_consecutive_correct", 3);
      archiveDays_ = (int)root.getInt("archive_days", 90);
      retryBaseDays_ = (int)root.getInt("retry_base_days", 1);
      skipDays_ = (int)root.getInt("skip_days", 1);
      if (auto* ai = root.find("ai"); ai && ai->isObject()) {
        aiEnabled_ = ai->getBool("enabled", true);
        aiTimeoutSec_ = (int)ai->getInt("timeout_sec", 15);
        aiMaxTokens_ = (int)ai->getInt("max_tokens", 220);
        aiTemperature_ = ai->getInt("temperature", 0) > 0 ? (double)ai->getInt("temperature", 0) : 0.2;
      }
    } catch (const std::exception& e) {
      std::cerr << "[error] parse " << path << " failed: " << e.what() << "\n";
    }
  }

  static JsonValue errValue(const std::string& msg)
  {
    JsonObject o;
    o["ok"] = JsonValue::makeBool(false);
    o["error"] = JsonValue::makeString(msg);
    return JsonValue::makeObject(o);
  }

  // ---------- 文本工具 ----------
  static std::string trim(const std::string& s)
  {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }

  static std::string normalize(const std::string& s)
  {
    std::string out;
    for (char c : trim(s)) {
      if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
      else out += c;
    }
    // 合并连续空白
    std::string t;
    bool sp = false;
    for (char c : out) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = true; continue; }
      if (sp && !t.empty()) t += ' ';
      sp = false;
      t += c;
    }
    return t;
  }

  static int editDistance(const std::string& a, const std::string& b)
  {
    if (a.empty()) return (int)b.size();
    if (b.empty()) return (int)a.size();
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); ++i) {
      cur[0] = (int)i;
      for (size_t j = 1; j <= b.size(); ++j) {
        int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
        cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
      }
      prev.swap(cur);
    }
    return prev[b.size()];
  }

  static bool sameStem(const std::string& a, const std::string& b)
  {
    if (a.empty() || b.empty()) return false;
    size_t n = std::min(a.size(), b.size());
    size_t minLen = std::min<size_t>(3, n);
    return a.compare(0, minLen, b, 0, minLen) == 0;
  }

  static std::string optionText(const std::string& ans, const JsonValue& options)
  {
    if (ans.size() != 1 || !options.isArray()) return ans;
    char c = ans[0];
    if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'F') return ans;
    int idx = c - 'A';
    auto& arr = options.asArray();
    if (idx < 0 || (size_t)idx >= arr.size()) return ans;
    const JsonValue& v = arr[idx];
    if (v.isString()) return v.asString();
    if (v.isObject()) {
      if (auto* t = v.find("text"); t && t->isString()) return t->asString();
      if (auto* t = v.find("label"); t && t->isString()) return t->asString();
    }
    return ans;
  }

  std::string lookupWord(int knowledgeId)
  {
    auto rows = db_.query(
      "SELECT COALESCE(NULLIF(standard_content,''),user_input_content,'') AS word "
      "FROM knowledge_points WHERE id=" + std::to_string(knowledgeId) + " LIMIT 1");
    return rows.empty() ? "" : rows[0]["word"];
  }

  bool hasCollocation(int knowledgeId, const std::string& answer)
  {
    std::string a = normalize(answer);
    if (a.size() < 3) return false;
    std::string like = "%" + Db::escape(a) + "%";
    auto rows = db_.query(
      "SELECT id FROM kp_collocations WHERE knowledge_id=" + std::to_string(knowledgeId) +
      " AND (collocation LIKE '" + like + "' OR '" + Db::escape(a) + "' LIKE CONCAT('%',collocation,'%'))"
      " LIMIT 1");
    return !rows.empty();
  }

  void attributeByRules(const WrongInput& in, const std::string& word,
                        std::string& type, std::string& reason)
  {
    std::string ua = optionText(in.userAnswer, in.options);
    std::string ca = optionText(in.correctAnswer, in.options);
    std::string un = normalize(ua);
    std::string cn = normalize(ca);
    int d = editDistance(un, cn);
    bool sameFirst = !un.empty() && !cn.empty() && un[0] == cn[0];
    const std::string& t = in.questionType;

    if (un.empty() || cn.empty()) { type = "unknown"; reason = "作答或答案为空，规则无法判定"; return; }

    if (t == "word_match" || t == "cloze_choice") {
      if (d > 0 && d <= maxEdit_ && sameFirst) {
        type = "spelling";
        reason = "与正确答案相差 " + std::to_string(d) + " 个字符";
      } else {
        type = "meaning";
        reason = "选项词义与正确答案不符";
      }
      return;
    }

    if (t == "word_form_fill") {
      if (sameStem(un, cn) && un != cn) { type = "grammar"; reason = "词形/时态不一致"; return; }
      if (d > 0 && d <= maxEdit_ && sameFirst) {
        type = "spelling";
        reason = "与正确答案相差 " + std::to_string(d) + " 个字符";
        return;
      }
      type = "grammar";
      reason = "词形或时态不符合语境";
      return;
    }

    if (t == "context_blank_fill") {
      if (d > 0 && d <= maxEdit_ && sameFirst) {
        type = "spelling";
        reason = "与正确答案相差 " + std::to_string(d) + " 个字符";
        return;
      }
      if (hasCollocation(in.knowledgeId, ca)) {
        type = "collocation";
        reason = "固定搭配或介词使用不符";
        return;
      }
      type = "meaning";
      reason = "填入词的词义与语境不符";
      return;
    }

    if (t == "cn_to_en_translation" || t == "sentence_imitation") {
      type = "grammar";
      reason = "句式或语法结构与参考表达不符";
      return;
    }

    type = "unknown";
    reason = "规则无法判定";
  }

  // ---------- AI 兜底 ----------
  void scheduleAi(long long attributionId, int uid, const std::string& word,
                  const std::string& qType, const std::string& userAnswer,
                  const std::string& correctAnswer)
  {
    std::thread([this, attributionId, uid, word, qType, userAnswer, correctAnswer]() {
      RequestContext::userId = uid;
      try {
        std::vector<std::map<std::string, std::string>> msgs = {
          {{"role", "system"}, {"content", prompts::ERROR_ATTRIBUTION_SYSTEM}},
          {{"role", "user"}, {"content", prompts::buildTemplate(
              prompts::ERROR_ATTRIBUTION_USER,
              {{"{WORD}", word}, {"{QUESTION_TYPE}", qType},
               {"{USER_ANSWER}", userAnswer}, {"{CORRECT_ANSWER}", correctAnswer}})}}};
        auto resp = ai_.postChat(msgs, true, aiMaxTokens_, aiTemperature_, "attribution");
        if (resp.ok) {
          std::string text = resp.extractedJson.empty() ? resp.body : resp.extractedJson;
          try {
            JsonValue j = parseJson(text);
            std::string et = j.getString("error_type");
            std::string rs = j.getString("reason");
            if (isValidType(et) && et != "unknown") {
              if (rs.size() > 200) rs.resize(200);
              db_.exec("UPDATE error_attributions SET error_type='" + Db::escape(et) +
                       "',ai_reason='" + Db::escape(rs) + "',attribution_source='ai',updated_at=NOW()"
                       " WHERE id=" + std::to_string(attributionId) +
                       " AND attribution_source='fallback'");
            }
          } catch (...) { /* 解析失败保持未知 */ }
          db_.exec("INSERT INTO ai_usage_daily (user_id,day,kind,calls,tokens) VALUES (" +
                   std::to_string(uid) + ",CURDATE(),'assist',1," +
                   std::to_string(resp.totalTokens) + ") ON DUPLICATE KEY UPDATE "
                   "calls=calls+1,tokens=tokens+VALUES(tokens)");
        }
      } catch (...) { /* 静默失败，保留 fallback */ }
      RequestContext::userId = 0;
    }).detach();
  }

  // ---------- 归档 ----------
  void lazyArchive(int uid)
  {
    db_.exec("UPDATE retry_items SET status='archived',archived_at=NOW(),updated_at=NOW()"
             " WHERE user_id=" + std::to_string(uid) +
             " AND status='queued' AND updated_at < DATE_SUB(NOW(), INTERVAL " +
             std::to_string(archiveDays_) + " DAY)");
  }
};

} // namespace eng
