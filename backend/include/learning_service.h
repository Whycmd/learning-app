// 学情服务层（阶段三）
// 职责：0-5级熟练度体系、双模式遗忘曲线、人工选题流程
// 遗忘曲线公式（向下取整）：
//   按周模式：next = today + floor(2 * (1 + 2^proficiency)) 天
//   按日模式：next = today + floor(0.4 * (1 + 3^proficiency)) 天
// 选题后熟练度调整：按日模式 +1，按周模式 +2
// 错题降权：错误一次 proficiency -1
#pragma once
#include "db.h"
#include "json.h"
#include "auth.h"
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <ctime>
#include <sstream>
#include <algorithm>

namespace eng {

class LearningService {
  Db& db_;

  static int userId() { return RequestContext::currentUserId(); }
public:
  LearningService(Db& db) : db_(db) {}

  // ---- 熟练度等级定义 ----
  // 0 未掌握 / 1 生疏 / 2 理解 / 3 熟练 / 4 精通 / 5 满级终止
  static std::string proficiencyName(int level) {
    switch (level) {
      case 0: return "未掌握";
      case 1: return "生疏";
      case 2: return "理解";
      case 3: return "熟练";
      case 4: return "精通";
      case 5: return "满级";
      default: return (level >= 5) ? "满级" : "未知";
    }
  }

  // 获取或创建学情记录
  // 固化知识点在首次进入复习流程时自动创建 user_learning 记录
  JsonValue getOrCreate(int knowledgeId) {
    // 检查知识点是否已固化
    auto kpRows = db_.query(
      "SELECT id, is_solidified, standard_content FROM knowledge_points WHERE id=" + std::to_string(knowledgeId)
    );
    if (kpRows.empty()) return JsonValue::makeNull();
    if (kpRows[0]["is_solidified"] != "1") return JsonValue::makeNull();

    auto rows = db_.query(
      "SELECT id, knowledge_id, proficiency_level, review_mode, "
      "review_times, error_times, "
      "DATE_FORMAT(next_review_time, '%Y-%m-%d %H:%i:%s') AS next_review_time, "
      "DATE_FORMAT(last_review_time, '%Y-%m-%d %H:%i:%s') AS last_review_time, "
      "DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time, "
      "DATE_FORMAT(update_time, '%Y-%m-%d %H:%i:%s') AS update_time "
      "FROM user_learning WHERE user_id=" + std::to_string(userId()) +
      " AND knowledge_id=" + std::to_string(knowledgeId)
    );
    if (rows.empty()) {
      // 自动创建：初始 proficiency=0, mode=daily, next_review=NOW()
      db_.exec(
        "INSERT INTO user_learning (user_id, knowledge_id, proficiency_level, review_mode, "
        "review_times, error_times, next_review_time, last_review_time, create_time) "
        "VALUES (" + std::to_string(userId()) + ", " + std::to_string(knowledgeId) +
        ", 0, 'daily', 0, 0, NOW(), NULL, NOW())"
      );
      rows = db_.query(
        "SELECT id, knowledge_id, proficiency_level, review_mode, "
        "review_times, error_times, "
        "DATE_FORMAT(next_review_time, '%Y-%m-%d %H:%i:%s') AS next_review_time, "
        "DATE_FORMAT(last_review_time, '%Y-%m-%d %H:%i:%s') AS last_review_time, "
        "DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time, "
        "DATE_FORMAT(update_time, '%Y-%m-%d %H:%i:%s') AS update_time "
        "FROM user_learning WHERE user_id=" + std::to_string(userId()) +
        " AND knowledge_id=" + std::to_string(knowledgeId)
      );
    }
    if (rows.empty()) return JsonValue::makeNull();
    return rowToJson(rows[0]);
  }

  // 原子任务1：答题自动更新熟练度
  // isCorrect=true: 不变（选题时统一调整）
  // isCorrect=false: proficiency -1
  struct AnswerResult { bool ok=false; int newProficiency=0; std::string error; };
  AnswerResult onAnswerResult(int knowledgeId, bool isCorrect) {
    AnswerResult r;
    auto lr = getOrCreate(knowledgeId);
    if (lr.isNull()) { r.error = "knowledge point not found or not solidified"; return r; }
    int curLevel = lr.getInt("proficiency_level");
    int reviewTimes = lr.getInt("review_times");
    int errorTimes = lr.getInt("error_times");
    int newLevel = curLevel;
    if (!isCorrect) {
      newLevel = curLevel - 1;
      if (newLevel < 0) newLevel = 0;
    }
    int newErrorTimes = isCorrect ? errorTimes : errorTimes + 1;
    int newReviewTimes = reviewTimes + 1;

    db_.exec(
      "UPDATE user_learning SET proficiency_level=" + std::to_string(newLevel) +
      ", error_times=" + std::to_string(newErrorTimes) +
      ", review_times=" + std::to_string(newReviewTimes) +
      ", last_review_time=NOW() WHERE user_id=" + std::to_string(userId()) +
      " AND knowledge_id=" + std::to_string(knowledgeId)
    );
    r.ok = true;
    r.newProficiency = newLevel;
    return r;
  }

  // 原子任务1：手动修改熟练度（前端覆盖）
  bool setProficiency(int knowledgeId, int level) {
    if (level < 0 || level > 5) return false;
    auto lr = getOrCreate(knowledgeId);
    if (lr.isNull()) return false;
    return db_.exec(
      "UPDATE user_learning SET proficiency_level=" + std::to_string(level) +
      " WHERE user_id=" + std::to_string(userId()) +
      " AND knowledge_id=" + std::to_string(knowledgeId)
    ).affected_rows > 0;
  }

  // 设置复习模式
  bool setReviewMode(int knowledgeId, const std::string& mode) {
    if (mode != "daily" && mode != "weekly") return false;
    auto lr = getOrCreate(knowledgeId);
    if (lr.isNull()) return false;
    return db_.exec(
      "UPDATE user_learning SET review_mode='" + Db::escape(mode) + "'"
      " WHERE user_id=" + std::to_string(userId()) +
      " AND knowledge_id=" + std::to_string(knowledgeId)
    ).affected_rows > 0;
  }

  // 原子任务2：遗忘曲线计算
  // 按周：floor(2 * (1 + 2^level)) 天
  // 按日：floor(0.4 * (1 + 3^level)) 天
  static int calcIntervalDays(int proficiency, const std::string& mode) {
    double raw;
    if (mode == "weekly") {
      raw = 2.0 * (1.0 + std::pow(2.0, (double)proficiency));
    } else {
      raw = 0.4 * (1.0 + std::pow(3.0, (double)proficiency));
    }
    return (int)std::floor(raw);
  }

  // 计算下次复习时间（基于当前熟练度）
  std::string calcNextReviewTime(int proficiency, const std::string& mode) {
    int days = calcIntervalDays(proficiency, mode);
    // 使用 DATE_ADD(NOW(), INTERVAL n DAY)
    std::ostringstream s;
    s << "DATE_ADD(NOW(), INTERVAL " << days << " DAY)";
    return s.str();
  }

  // 原子任务2：完成复习后更新复习时间
  // 调用时机：用户选题完成后，统一调整熟练度 + 重算下次复习时间
  struct ReviewCompleteResult {
    bool ok=false;
    int newProficiency=0;
    std::string nextReviewTime;
    std::string error;
  };
  ReviewCompleteResult completeReview(int knowledgeId) {
    ReviewCompleteResult r;
    auto lr = getOrCreate(knowledgeId);
    if (lr.isNull()) { r.error = "knowledge point not found"; return r; }
    int curLevel = lr.getInt("proficiency_level");
    std::string mode = lr.getString("review_mode");

    // 满级终止复习：proficiency >= 5 时 next_review_time 设为 NULL
    if (curLevel >= 5) {
      db_.exec(
        "UPDATE user_learning SET next_review_time=NULL, last_review_time=NOW(), "
        "review_times=review_times+1 WHERE user_id=" + std::to_string(userId()) +
        " AND knowledge_id=" + std::to_string(knowledgeId)
      );
      r.ok = true;
      r.newProficiency = curLevel;
      r.nextReviewTime = "NULL (terminated)";
      return r;
    }

    std::string nextExpr = calcNextReviewTime(curLevel, mode);
    db_.exec(
      "UPDATE user_learning SET next_review_time=" + nextExpr +
      ", last_review_time=NOW(), review_times=review_times+1 WHERE user_id=" +
      std::to_string(userId()) + " AND knowledge_id=" + std::to_string(knowledgeId)
    );
    auto rows = db_.query(
      "SELECT proficiency_level, DATE_FORMAT(next_review_time, '%Y-%m-%d %H:%i:%s') AS nrt "
      "FROM user_learning WHERE user_id=" + std::to_string(userId()) +
      " AND knowledge_id=" + std::to_string(knowledgeId)
    );
    if (!rows.empty()) {
      r.ok = true;
      r.newProficiency = std::stoi(rows[0]["proficiency_level"].empty() ? "0" : rows[0]["proficiency_level"]);
      r.nextReviewTime = rows[0]["nrt"].empty() ? "NULL" : rows[0]["nrt"];
    }
    return r;
  }

  // 原子任务3：人工选题流程
  // 系统自动筛选：next_review_time <= today, proficiency < 5 -> 候选池
  // 排序优先级：未掌握(0) > 生疏(1) > 理解(2) > 熟练(3) > 精通(4)，满级(5)不进入
  JsonValue getCandidatePool() {
    std::ostringstream sql;
    sql << "SELECT ul.id AS learning_id, ul.knowledge_id, ul.proficiency_level, "
           "ul.review_mode, ul.review_times, ul.error_times, "
           "DATE_FORMAT(ul.next_review_time, '%Y-%m-%d %H:%i:%s') AS next_review_time, "
           "DATE_FORMAT(ul.last_review_time, '%Y-%m-%d %H:%i:%s') AS last_review_time, "
           "kp.standard_content, kp.user_input_content, kp.difficulty, kp.master_require, kp.remark "
           "FROM user_learning ul "
           "JOIN knowledge_points kp ON ul.knowledge_id=kp.id "
           "WHERE ul.user_id=" << userId() <<
           " AND ul.proficiency_level < 5 "
           " AND ul.is_active=1 "
           " AND (ul.next_review_time IS NULL OR ul.next_review_time <= NOW()) "
           " ORDER BY ul.proficiency_level ASC, ul.next_review_time ASC";
    auto rows = db_.query(sql.str());
    JsonArray a;
    for (auto& row : rows) {
      JsonObject o;
      o["learning_id"] = JsonValue::makeInt(std::stoll(row["learning_id"].empty() ? "0" : row["learning_id"]));
      o["knowledge_id"] = JsonValue::makeInt(std::stoll(row["knowledge_id"].empty() ? "0" : row["knowledge_id"]));
      o["proficiency_level"] = JsonValue::makeInt(std::stoi(row["proficiency_level"].empty() ? "0" : row["proficiency_level"]));
      o["proficiency_name"] = JsonValue::makeString(proficiencyName(std::stoi(row["proficiency_level"].empty() ? "0" : row["proficiency_level"])));
      o["review_mode"] = JsonValue::makeString(row["review_mode"]);
      o["review_times"] = JsonValue::makeInt(std::stoi(row["review_times"].empty() ? "0" : row["review_times"]));
      o["error_times"] = JsonValue::makeInt(std::stoi(row["error_times"].empty() ? "0" : row["error_times"]));
      o["next_review_time"] = row["next_review_time"].empty() ? JsonValue::makeNull() : JsonValue::makeString(row["next_review_time"]);
      o["last_review_time"] = row["last_review_time"].empty() ? JsonValue::makeNull() : JsonValue::makeString(row["last_review_time"]);
      o["standard_content"] = row["standard_content"].empty() ? JsonValue::makeNull() : JsonValue::makeString(row["standard_content"]);
      o["user_input_content"] = JsonValue::makeString(row["user_input_content"]);
      o["difficulty"] = row["difficulty"].empty() ? JsonValue::makeNull() : JsonValue::makeInt(std::stoi(row["difficulty"]));
      o["master_require"] = row["master_require"].empty() ? JsonValue::makeNull() : JsonValue::makeString(row["master_require"]);
      o["remark"] = row["remark"].empty() ? JsonValue::makeNull() : JsonValue::makeString(row["remark"]);
      a.push_back(JsonValue::makeObject(o));
    }
    JsonObject result;
    result["data"] = JsonValue::makeArray(a);
    result["total"] = JsonValue::makeInt((int)a.size());
    return JsonValue::makeObject(result);
  }

  // 原子任务3：选题完成后调整熟练度
  // 按日模式：+1；按周模式：+2
  // 然后重算下次复习时间
  struct SelectResult {
    bool ok=false;
    std::vector<int> updatedIds;
    std::map<int, std::pair<int, std::string>> details; // id -> (newLevel, nextReviewTime)
    std::string error;
  };
  SelectResult onQuestionSelected(const std::vector<int>& knowledgeIds) {
    SelectResult r;
    if (knowledgeIds.empty()) { r.error = "no knowledge points selected"; return r; }
    for (int kpId : knowledgeIds) {
      auto lr = getOrCreate(kpId);
      if (lr.isNull()) continue;
      int curLevel = lr.getInt("proficiency_level");
      std::string mode = lr.getString("review_mode");
      int increment = (mode == "weekly") ? 2 : 1;
      int newLevel = curLevel + increment;
      if (newLevel > 5) newLevel = 5; // 满级封顶

      if (newLevel >= 5) {
        // 满级终止
        db_.exec(
          "UPDATE user_learning SET proficiency_level=5, next_review_time=NULL, "
          "last_review_time=NOW(), review_times=review_times+1 "
          "WHERE user_id=" + std::to_string(userId()) + " AND knowledge_id=" + std::to_string(kpId)
        );
        r.details[kpId] = {5, "NULL (terminated)"};
      } else {
        std::string nextExpr = calcNextReviewTime(newLevel, mode);
        db_.exec(
          "UPDATE user_learning SET proficiency_level=" + std::to_string(newLevel) +
          ", next_review_time=" + nextExpr +
          ", last_review_time=NOW(), review_times=review_times+1 "
          "WHERE user_id=" + std::to_string(userId()) + " AND knowledge_id=" + std::to_string(kpId)
        );
        auto rows = db_.query(
          "SELECT DATE_FORMAT(next_review_time, '%Y-%m-%d %H:%i:%s') AS nrt "
          "FROM user_learning WHERE user_id=" + std::to_string(userId()) +
          " AND knowledge_id=" + std::to_string(kpId)
        );
        std::string nrt = (!rows.empty() && !rows[0]["nrt"].empty()) ? rows[0]["nrt"] : "NULL";
        r.details[kpId] = {newLevel, nrt};
      }
      r.updatedIds.push_back(kpId);
    }
    r.ok = !r.updatedIds.empty();
    return r;
  }

  // 学情统计概览（v1.3.0-T108 扩充：熟练度加权 / 掌握数 / 累计复习错题）
  JsonValue getStats() {
    auto rows = db_.query(
      "SELECT proficiency_level, COUNT(*) AS cnt FROM user_learning "
      "WHERE user_id=" + std::to_string(userId()) + " GROUP BY proficiency_level ORDER BY proficiency_level"
    );
    JsonArray dist;
    int total = 0, mastered = 0, weighted = 0;
    for (auto& row : rows) {
      int lv = std::stoi(row["proficiency_level"].empty() ? "0" : row["proficiency_level"]);
      int cnt = std::stoi(row["cnt"].empty() ? "0" : row["cnt"]);
      total += cnt;
      if (lv >= 5) mastered += cnt;
      weighted += lv * cnt;
      JsonObject o;
      o["level"] = JsonValue::makeInt(lv);
      o["name"] = JsonValue::makeString(proficiencyName(lv));
      o["count"] = JsonValue::makeInt(cnt);
      dist.push_back(JsonValue::makeObject(o));
    }

    auto dueRows = db_.query(
      "SELECT COUNT(*) AS cnt FROM user_learning WHERE user_id=" + std::to_string(userId()) +
      " AND proficiency_level < 5 AND is_active=1 AND next_review_time <= NOW()"
    );
    int dueCount = dueRows.empty() ? 0 : std::stoi(dueRows[0]["cnt"].empty() ? "0" : dueRows[0]["cnt"]);

    auto aggRows = db_.query(
      "SELECT SUM(review_times) AS rt, SUM(error_times) AS et, COUNT(*) AS tc "
      "FROM user_learning WHERE user_id=" + std::to_string(userId())
    );
    int rt = aggRows.empty() ? 0 : std::stoi(aggRows[0]["rt"].empty() ? "0" : aggRows[0]["rt"]);
    int et = aggRows.empty() ? 0 : std::stoi(aggRows[0]["et"].empty() ? "0" : aggRows[0]["et"]);
    int tc = aggRows.empty() ? 0 : std::stoi(aggRows[0]["tc"].empty() ? "0" : aggRows[0]["tc"]);
    total = tc;

    double avg = total ? (double)weighted / total : 0.0;

    JsonObject result;
    result["total"] = JsonValue::makeInt(total);
    result["due_for_review"] = JsonValue::makeInt(dueCount);
    result["mastered_count"] = JsonValue::makeInt(mastered);
    result["avg_proficiency"] = JsonValue::makeDouble(avg);
    result["review_times_sum"] = JsonValue::makeInt(rt);
    result["error_times_sum"] = JsonValue::makeInt(et);
    result["distribution"] = JsonValue::makeArray(dist);
    return JsonValue::makeObject(result);
  }

  // v1.3.0-T108：最近 N 天每日复习次数（基于 last_review_time）
  // 返回 {start, end, days: [{date, count}] }
  JsonValue getReviewCalendar(int days = 90) {
    auto rows = db_.query(
      "SELECT DATE(last_review_time) AS d, COUNT(*) AS c FROM user_learning "
      "WHERE user_id=" + std::to_string(userId()) +
      " AND last_review_time IS NOT NULL AND last_review_time >= DATE_SUB(CURDATE(), INTERVAL " +
      std::to_string(days - 1) + " DAY) "
      "GROUP BY DATE(last_review_time) ORDER BY d ASC"
    );
    JsonArray arr;
    for (auto& row : rows) {
      JsonObject o;
      o["date"] = JsonValue::makeString(row["d"]);
      o["count"] = JsonValue::makeInt(std::stoi(row["c"].empty() ? "0" : row["c"]));
      arr.push_back(JsonValue::makeObject(o));
    }
    // 计算连续打卡天数（从今天往回数）
    int streak = 0;
    time_t now = time(nullptr);
    for (int i = 0; i < days; i++) {
      char buf[16];
      tm t; localtime_r(&now, &t);
      strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
      bool hit = false;
      for (auto& r : rows) if (r["d"] == buf) { hit = true; break; }
      if (!hit && i == 0) { /* 今天没复习，从昨天算 */ }
      else if (!hit) break;
      else streak++;
      now -= 86400;
    }
    JsonObject result;
    result["days"] = JsonValue::makeArray(arr);
    result["streak"] = JsonValue::makeInt(streak);
    result["window_days"] = JsonValue::makeInt(days);
    return JsonValue::makeObject(result);
  }

private:
  static JsonValue rowToJson(const Row& row) {
    JsonObject o;
    for (auto& kv : row) {
      if (kv.first == "id" || kv.first == "knowledge_id" || kv.first == "proficiency_level"
          || kv.first == "review_times" || kv.first == "error_times") {
        o[kv.first] = kv.second.empty() ? JsonValue::makeInt(0) : JsonValue::makeInt(std::stoll(kv.second));
      } else {
        o[kv.first] = kv.second.empty() ? JsonValue::makeNull() : JsonValue::makeString(kv.second);
      }
    }
    return JsonValue::makeObject(o);
  }
};

} // namespace eng
