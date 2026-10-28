// v1.4.0-T012：异步批量固化任务（持久化 + 断点续跑 + 词条级渐进展示）
// [BUSINESS] 状态机与任务编排；[FUNCTIONAL] 分片大小；[CONFIG] 并发数来自 config/ai.json
#pragma once

#include "db.h"
#include "auth.h"
#include "json.h"
#include "knowledge_service.h"

#include <atomic>
#include <algorithm>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace eng {

struct SolidifyJobView {
  std::string id;
  int total = 0;
  int done = 0;
  int failed = 0;
  int progress = 0;
  std::string status;
  std::string error;
  std::string createTime;
  std::string updateTime;
  std::vector<int64_t> completedIds;
  struct FailedItem { int64_t knowledgeId = 0; std::string input; std::string error; };
  std::vector<FailedItem> failedItems;
};

class SolidifyJobService {
  Db& db_;
  KnowledgeService& kp_;
  int concurrency_;
  std::function<void(int)> onComplete_;
  std::mutex mu_;
  std::set<std::string> running_;
  static constexpr int CHUNK_SIZE = 3; // [FUNCTIONAL] 每片词数

public:
  SolidifyJobService(Db& db, KnowledgeService& kp, int concurrency,
                     std::function<void(int)> onComplete)
    : db_(db), kp_(kp), concurrency_(concurrency < 1 ? 2 : (concurrency > 16 ? 16 : concurrency)),
      onComplete_(std::move(onComplete)) {}

  // 服务重启：遗留任务标记中断，明细 running 回退 pending；由用户手动“继续”
  void recoverOnStartup()
  {
    try
    {
      db_.exec("UPDATE solidify_jobs SET status='interrupted' WHERE status IN ('pending','running')");
      db_.exec("UPDATE solidify_job_items SET status='pending' WHERE status='running'");
      cleanupExpired();
    }
    catch (...) {}
  }

  // 30 天任务保留（每次创建任务时顺带清理，避免依赖外部定时器）
  void cleanupExpired()
  {
    try
    {
      db_.exec("DELETE i FROM solidify_job_items i JOIN solidify_jobs j ON i.job_id=j.id "
               "WHERE j.status IN ('done','partial','failed','interrupted') "
               "AND j.update_time < DATE_SUB(NOW(), INTERVAL 30 DAY)");
      db_.exec("DELETE FROM solidify_jobs WHERE status IN ('done','partial','failed','interrupted') "
               "AND update_time < DATE_SUB(NOW(), INTERVAL 30 DAY)");
    }
    catch (...) {}
  }

  struct CreateResult { std::string jobId; int total = 0; bool reused = false; std::string status; };

  // 已有运行中任务则复用；否则快照当前未固化词建立新任务
  CreateResult createOrReuse(int uid, const std::vector<int64_t>& ids)
  {
    auto existing = db_.query(
      "SELECT id, total, status FROM solidify_jobs WHERE user_id=" + std::to_string(uid)
      + " AND status IN ('pending','running') ORDER BY id DESC LIMIT 1");
    if (!existing.empty())
      return {existing[0]["id"], std::stoi(existing[0]["total"]), true, existing[0]["status"]};

    std::string jobId = Auth::randomHex(12);
    int total = 0;
    db_.exec("INSERT INTO solidify_jobs (id, user_id, total, status) VALUES ('"
             + Db::escape(jobId) + "', " + std::to_string(uid) + ", " + std::to_string(ids.size())
             + ", 'pending')");
    for (int64_t id : ids)
    {
      auto kp = db_.query("SELECT id FROM knowledge_points WHERE id=" + std::to_string(id)
                          + " AND user_id=" + std::to_string(uid) + " AND is_solidified=0");
      if (kp.empty()) continue;
      db_.exec("INSERT INTO solidify_job_items (job_id, knowledge_id) VALUES ('"
               + Db::escape(jobId) + "', " + std::to_string(id) + ")");
      total++;
    }
    if (total == 0)
    {
      db_.exec("UPDATE solidify_jobs SET status='done' WHERE id='" + Db::escape(jobId) + "'");
      return {jobId, 0, false, "done"};
    }
    db_.exec("UPDATE solidify_jobs SET total=" + std::to_string(total) + " WHERE id='" + Db::escape(jobId) + "'");
    cleanupExpired();
    bool started = start(jobId, uid);
    return {jobId, total, false, started ? "running" : "pending"};
  }

  // 从断点继续：failed → pending，done/skipped 不重跑
  struct ResumeResult { bool ok = false; int code = 400; std::string error; std::string status; };
  ResumeResult resume(const std::string& jobId, int uid)
  {
    auto rows = db_.query("SELECT status FROM solidify_jobs WHERE id='" + Db::escape(jobId)
                          + "' AND user_id=" + std::to_string(uid));
    if (rows.empty()) return {false, 404, "job not found", ""};
    std::string st = rows[0]["status"];
    if (st == "running" || st == "pending") return {true, 200, "", st};
    if (st != "interrupted" && st != "partial" && st != "failed")
      return {false, 409, "job cannot resume: " + st, st};

    db_.exec("UPDATE solidify_job_items SET status='pending', error_msg=NULL "
             "WHERE job_id='" + Db::escape(jobId) + "' AND status='failed'");
    db_.exec("UPDATE solidify_jobs SET status='pending', failed=0, error=NULL "
             "WHERE id='" + Db::escape(jobId) + "' AND user_id=" + std::to_string(uid));
    bool started = start(jobId, uid);
    return {true, 200, started ? "" : "failed to start", "running"};
  }

  bool get(const std::string& jobId, int uid, SolidifyJobView& out)
  {
    auto rows = db_.query(
      "SELECT id, total, done, failed, status, error, "
      "DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time, "
      "DATE_FORMAT(update_time, '%Y-%m-%d %H:%i:%s') AS update_time "
      "FROM solidify_jobs WHERE id='" + Db::escape(jobId) + "' AND user_id=" + std::to_string(uid));
    if (rows.empty()) return false;
    fillView(rows[0], out);
    auto doneIds = db_.query(
      "SELECT knowledge_id FROM solidify_job_items WHERE job_id='" + Db::escape(jobId)
      + "' AND status IN ('done','skipped') ORDER BY id");
    for (auto& r : doneIds) out.completedIds.push_back(std::stoll(r["knowledge_id"]));
    auto failed = db_.query(
      "SELECT i.knowledge_id, i.error_msg, kp.user_input_content FROM solidify_job_items i "
      "LEFT JOIN knowledge_points kp ON kp.id=i.knowledge_id "
      "WHERE i.job_id='" + Db::escape(jobId) + "' AND i.status='failed' ORDER BY i.id");
    for (auto& r : failed)
    {
      SolidifyJobView::FailedItem f;
      f.knowledgeId = std::stoll(r["knowledge_id"]);
      f.input = r["user_input_content"];
      f.error = r["error_msg"];
      out.failedItems.push_back(f);
    }
    return true;
  }

  std::vector<SolidifyJobView> list(int uid, int limit, int offset)
  {
    if (limit < 1) limit = 20;
    if (limit > 100) limit = 100;
    if (offset < 0) offset = 0;
    auto rows = db_.query(
      "SELECT id, total, done, failed, status, error, "
      "DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time, "
      "DATE_FORMAT(update_time, '%Y-%m-%d %H:%i:%s') AS update_time "
      "FROM solidify_jobs WHERE user_id=" + std::to_string(uid)
      + " ORDER BY create_time DESC, id DESC LIMIT " + std::to_string(offset) + ", " + std::to_string(limit));
    std::vector<SolidifyJobView> out;
    for (auto& r : rows) { SolidifyJobView v; fillView(r, v); out.push_back(v); }
    return out;
  }

private:
  bool start(const std::string& jobId, int uid)
  {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (running_.count(jobId)) return true;
      running_.insert(jobId);
    }
    try
    {
      auto wr = db_.exec("UPDATE solidify_jobs SET status='running', error=NULL WHERE id='"
                         + Db::escape(jobId) + "' AND user_id=" + std::to_string(uid)
                         + " AND status IN ('pending','interrupted','partial','failed')");
      if (wr.affected_rows == 0)
      {
        std::lock_guard<std::mutex> lk(mu_);
        running_.erase(jobId);
        return false;
      }
    }
    catch (...)
    {
      std::lock_guard<std::mutex> lk(mu_);
      running_.erase(jobId);
      return false;
    }
    std::thread([this, jobId, uid] { runJob(jobId, uid); }).detach();
    return true;
  }

  void runJob(std::string jobId, int uid)
  {
    RequestContext::userId = uid;
    auto rows = db_.query("SELECT knowledge_id FROM solidify_job_items WHERE job_id='"
                          + Db::escape(jobId) + "' AND status='pending' ORDER BY id");
    std::vector<int64_t> ids;
    for (auto& r : rows) ids.push_back(std::stoll(r["knowledge_id"]));

    std::atomic<size_t> cursor{0};
    int workers = concurrency_;
    if (workers > (int)ids.size() && !ids.empty()) workers = (int)ids.size();
    std::vector<std::thread> threads;
    for (int w = 0; w < workers; w++)
    {
      threads.emplace_back([this, &jobId, &ids, &cursor, uid] {
        RequestContext::userId = uid;
        while (true)
        {
          size_t begin = cursor.fetch_add(CHUNK_SIZE);
          if (begin >= ids.size()) break;
          size_t end = std::min(ids.size(), begin + CHUNK_SIZE);
          std::vector<int64_t> chunk(ids.begin() + begin, ids.begin() + end);
          processChunk(jobId, uid, chunk);
        }
      });
    }
    for (auto& t : threads) t.join();

    // 汇总最终状态
    auto cnt = db_.query(
      "SELECT COUNT(*) AS total, "
      "SUM(status IN ('done','skipped')) AS done, SUM(status='failed') AS failed "
      "FROM solidify_job_items WHERE job_id='" + Db::escape(jobId) + "'");
    int total = 0, done = 0, failed = 0;
    if (!cnt.empty())
    {
      total = cnt[0]["total"].empty() ? 0 : std::stoi(cnt[0]["total"]);
      done = cnt[0]["done"].empty() ? 0 : std::stoi(cnt[0]["done"]);
      failed = cnt[0]["failed"].empty() ? 0 : std::stoi(cnt[0]["failed"]);
    }
    std::string status = (failed == 0 && done >= total) ? "done" : (done > 0 ? "partial" : "failed");
    db_.exec("UPDATE solidify_jobs SET done=" + std::to_string(done) + ", failed="
             + std::to_string(failed) + ", status='" + status + "' WHERE id='" + Db::escape(jobId) + "'");
    if (done > 0 && onComplete_)
    {
      try { RequestContext::userId = uid; onComplete_(uid); } catch (...) {}
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      running_.erase(jobId);
    }
  }

  void processChunk(const std::string& jobId, int uid, const std::vector<int64_t>& chunk)
  {
    std::ostringstream in;
    for (size_t i = 0; i < chunk.size(); i++) { if (i) in << ","; in << chunk[i]; }
    db_.exec("UPDATE solidify_job_items SET status='running' WHERE job_id='" + Db::escape(jobId)
             + "' AND status='pending' AND knowledge_id IN (" + in.str() + ")");

    // 已固化（手工固化/其他任务完成）→ 跳过，不重复调用 AI
    std::set<int64_t> already;
    auto sol = db_.query("SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid)
                         + " AND is_solidified=1 AND id IN (" + in.str() + ")");
    for (auto& r : sol) already.insert(std::stoll(r["id"]));
    std::vector<int64_t> need;
    for (int64_t id : chunk)
    {
      if (already.count(id)) markItem(jobId, id, "skipped", "");
      else need.push_back(id);
    }
    refreshCounts(jobId);
    if (need.empty()) return;

    std::set<int64_t> reported;
    try
    {
      kp_.solidifyBatch(need, [&](const KnowledgeService::BatchItemResult& item) {
        reported.insert(item.id);
        markItem(jobId, item.id, item.ok ? (item.skipped ? "skipped" : "done") : "failed", item.error);
        refreshCounts(jobId);
      });
    }
    catch (const std::exception& e)
    {
      for (int64_t id : need)
      {
        if (reported.count(id)) continue;
        markItem(jobId, id, "failed", e.what());
      }
    }
    for (int64_t id : need)
    {
      if (!reported.count(id)) markItem(jobId, id, "failed", "no result");
    }
    refreshCounts(jobId);
  }

  void markItem(const std::string& jobId, int64_t id, const std::string& status, const std::string& err)
  {
    std::string sql = "UPDATE solidify_job_items SET status='" + status + "', error_msg="
                      + (err.empty() ? std::string("NULL") : "'" + Db::escape(err) + "'")
                      + " WHERE job_id='" + Db::escape(jobId) + "' AND knowledge_id=" + std::to_string(id);
    try { db_.exec(sql); } catch (...) {}
  }

  void refreshCounts(const std::string& jobId)
  {
    try
    {
      db_.exec("UPDATE solidify_jobs SET done=(SELECT COUNT(*) FROM solidify_job_items "
               "WHERE job_id='" + Db::escape(jobId) + "' AND status IN ('done','skipped')), "
               "failed=(SELECT COUNT(*) FROM solidify_job_items WHERE job_id='"
               + Db::escape(jobId) + "' AND status='failed') WHERE id='" + Db::escape(jobId) + "'");
    }
    catch (...) {}
  }

  static void fillView(Row r, SolidifyJobView& v)
  {
    v.id = r["id"];
    v.total = r["total"].empty() ? 0 : std::stoi(r["total"]);
    v.done = r["done"].empty() ? 0 : std::stoi(r["done"]);
    v.failed = r["failed"].empty() ? 0 : std::stoi(r["failed"]);
    v.status = r["status"];
    v.error = r["error"];
    v.createTime = r["create_time"];
    v.updateTime = r["update_time"];
    int processed = v.done + v.failed;
    v.progress = v.total > 0 ? (processed * 100 / v.total) : 100;
  }
};

} // namespace eng
