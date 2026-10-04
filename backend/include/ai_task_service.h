// AI 任务调度服务（v1.2.0-T002）
// 职责：ai_tasks 表 CRUD + 重试驱动
// 注意：任务的实际执行由 KnowledgeService 的 solidify 链路完成，本 Service 只负责记录状态
#pragma once
#include "db.h"
#include <string>
#include <vector>
#include <map>
#include <sstream>

namespace eng {

class AiTaskService {
  Db& db_;
public:
  AiTaskService(Db& db) : db_(db) {}

  // 记录任务（固化前调用）
  struct CreateResult { int64_t id=0; bool ok=false; };
  CreateResult create(const std::string& taskType, int64_t refId, const std::string& payload="") {
    CreateResult r;
    std::ostringstream sql;
    sql << "INSERT INTO ai_tasks (task_type, ref_id, status, payload) VALUES ('"
        << Db::escape(taskType) << "', " << refId << ", 'pending', "
        << (payload.empty() ? "NULL" : "'" + Db::escape(payload) + "'") << ")";
    try {
      auto wr = db_.exec(sql.str());
      r.id = wr.last_insert_id;
      r.ok = true;
    } catch (...) {}
    return r;
  }

  // 更新任务状态
  bool updateStatus(int64_t taskId, const std::string& status, const std::string& result="", const std::string& errorMsg="") {
    std::ostringstream sql;
    sql << "UPDATE ai_tasks SET status='" << Db::escape(status) << "'";
    if (!result.empty()) sql << ", result='" << Db::escape(result) << "'";
    if (!errorMsg.empty()) sql << ", error_msg='" << Db::escape(errorMsg) << "'";
    if (status == "running" || status == "failed") sql << ", retry_times=retry_times+1";
    sql << " WHERE id=" << taskId;
    try { db_.exec(sql.str()); return true; } catch (...) { return false; }
  }

  // 列表（按状态过滤）
  struct TaskInfo {
    int64_t id=0; std::string task_type; int64_t ref_id=0; std::string status;
    int retry_times=0; std::string error_msg; std::string create_time;
  };
  std::vector<TaskInfo> list(const std::string& status="") {
    std::ostringstream sql;
    sql << "SELECT id, task_type, ref_id, status, retry_times, error_msg, "
           "DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time "
           "FROM ai_tasks";
    if (!status.empty()) sql << " WHERE status='" << Db::escape(status) << "'";
    sql << " ORDER BY id DESC LIMIT 200";
    auto rows = db_.query(sql.str());
    std::vector<TaskInfo> out;
    for (auto& r : rows) {
      TaskInfo t;
      t.id = std::stoll(r["id"]);
      t.task_type = r["task_type"];
      t.ref_id = std::stoll(r["ref_id"]);
      t.status = r["status"];
      t.retry_times = std::stoi(r["retry_times"]);
      t.error_msg = r["error_msg"];
      t.create_time = r["create_time"];
      out.push_back(t);
    }
    return out;
  }

  // 统计
  struct Stats { int pending=0; int running=0; int failed=0; int done=0; };
  Stats stats() {
    Stats s;
    try {
      auto rows = db_.query(
        "SELECT status, COUNT(*) AS c FROM ai_tasks GROUP BY status"
      );
      for (auto& r : rows) {
        int c = std::stoi(r["c"]);
        if (r["status"] == "pending") s.pending = c;
        else if (r["status"] == "running") s.running = c;
        else if (r["status"] == "failed") s.failed = c;
        else if (r["status"] == "done") s.done = c;
      }
    } catch (...) {}
    return s;
  }

  // 删除
  bool remove(int64_t id) {
    try { db_.exec("DELETE FROM ai_tasks WHERE id=" + std::to_string(id)); return true; }
    catch (...) { return false; }
  }

  // 获取 pending/failed 任务供重试
  std::vector<int64_t> findRetryable(const std::string& taskType="solidify_kp") {
    auto rows = db_.query(
      "SELECT id FROM ai_tasks WHERE task_type='" + Db::escape(taskType) + "' "
      "AND (status='pending' OR status='failed') AND retry_times < 5 "
      "ORDER BY id ASC LIMIT 50"
    );
    std::vector<int64_t> ids;
    for (auto& r : rows) ids.push_back(std::stoll(r["id"]));
    return ids;
  }
};

} // namespace eng
