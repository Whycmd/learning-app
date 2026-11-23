// v1.4.0-T012 异步批量固化任务；v1.5.0-T002 迁入通用 AI 队列（ai_tasks/ai_task_items）
// [BUSINESS] 旧 /api/kp/solidify/jobs* 接口保持兼容；新任务由 AiQueueService 调度执行
#pragma once

#include "db.h"
#include "auth.h"
#include "json.h"
#include "knowledge_service.h"
#include "ai_queue_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
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
  int queuePosition = 0;
  int etaSec = 0;
  std::vector<int64_t> completedIds;
  struct FailedItem { int64_t knowledgeId = 0; std::string input; std::string error; };
  std::vector<FailedItem> failedItems;
};

class SolidifyJobService {
  Db& db_;
  KnowledgeService& kp_;
  AiQueueService& queue_;
  std::function<void(int)> onComplete_;
  // legacy（历史 solidify_jobs 任务继续通道）
  std::mutex mu_;
  std::set<std::string> running_;
  static constexpr int CHUNK_SIZE = 3; // [FUNCTIONAL] 每片词数

public:
  SolidifyJobService(Db& db, KnowledgeService& kp, AiQueueService& queue,
                     std::function<void(int)> onComplete)
    : db_(db), kp_(kp), queue_(queue), onComplete_(std::move(onComplete)) {}

  // 服务重启：历史 solidify_jobs 遗留任务标记中断（由用户手动“继续”）；
  // 新队列的 running 任务由 AiQueueService::recoverOnStartup 自动回队
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

  // 30 天任务保留（新队列与历史表同步清理，queued/running 不动）
  void cleanupExpired()
  {
    try
    {
      db_.exec("DELETE i FROM solidify_job_items i JOIN solidify_jobs j ON i.job_id=j.id "
               "WHERE j.status IN ('done','partial','failed','interrupted') "
               "AND j.update_time < DATE_SUB(NOW(), INTERVAL 30 DAY)");
      db_.exec("DELETE FROM solidify_jobs WHERE status IN ('done','partial','failed','interrupted') "
               "AND update_time < DATE_SUB(NOW(), INTERVAL 30 DAY)");
      db_.exec("DELETE i FROM ai_task_items i JOIN ai_tasks t ON t.id=i.task_id "
               "WHERE t.status IN ('done','partial','failed') "
               "AND t.update_time < DATE_SUB(NOW(), INTERVAL 30 DAY)");
      db_.exec("DELETE FROM ai_tasks WHERE kind='solidify_kp' "
               "AND status IN ('done','partial','failed') "
               "AND update_time < DATE_SUB(NOW(), INTERVAL 30 DAY)");
    }
    catch (...) {}
  }

  struct CreateResult {
    std::string jobId;
    int total = 0;
    bool reused = false;
    std::string status;
    int code = 200;
    std::string error;
  };

  // 已有排队/执行中任务则复用；否则快照当前未固化词投递到通用队列
  CreateResult createOrReuse(int uid, const std::vector<int64_t>& ids)
  {
    auto existing = db_.query(
      "SELECT id, total, status FROM ai_tasks WHERE user_id=" + std::to_string(uid)
      + " AND kind='solidify_kp' AND status IN ('queued','running') ORDER BY id DESC LIMIT 1");
    if (!existing.empty())
    {
      std::string st = existing[0]["status"] == "queued" ? "pending" : existing[0]["status"];
      return {existing[0]["id"], std::stoi(existing[0]["total"]), true, st, 200, ""};
    }

    // 过滤：属于当前用户且未固化
    std::vector<AiQueueService::NewItem> items;
    std::vector<std::string> seen;
    for (int64_t id : ids)
    {
      auto kp = db_.query("SELECT id, user_input_content, standard_content FROM knowledge_points "
                          "WHERE id=" + std::to_string(id) + " AND user_id=" + std::to_string(uid) +
                          " AND is_solidified=0");
      if (kp.empty()) continue;
      AiQueueService::NewItem it;
      it.refId = id;
      it.input = kp[0]["user_input_content"].empty() ? kp[0]["standard_content"]
                                                     : kp[0]["user_input_content"];
      items.push_back(it);
    }

    if (items.empty())
    {
      // 与原行为一致：空任务直接 done
      auto w = db_.exec("INSERT INTO ai_tasks (task_type, status, user_id, kind, lane, total, "
                        "progress, retry_count, update_time) VALUES ('solidify_kp','done'," +
                        std::to_string(uid) + ",'solidify_kp','solidify',0,100,0,NOW())");
      return {std::to_string(w.last_insert_id), 0, false, "done", 200, ""};
    }

    auto r = queue_.create(uid, "solidify_kp", "solidify", items, "{}");
    if (!r.ok)
    {
      CreateResult cr;
      cr.code = r.httpStatus;
      cr.error = r.error;
      cr.status = "failed";
      return cr;
    }
    cleanupExpired();
    return {std::to_string(r.taskId), r.total, false, "pending", 200, ""};
  }

  // 从断点继续：failed → pending，done/skipped 不重跑
  struct ResumeResult { bool ok = false; int code = 400; std::string error; std::string status; };
  ResumeResult resume(const std::string& jobId, int uid)
  {
    int64_t taskId = 0;
    AiTaskView tv;
    if (parseNumeric(jobId, taskId) && queue_.get(taskId, uid, tv))
    {
      if (tv.status == "queued" || tv.status == "running")
        return {true, 200, "", tv.status == "queued" ? "pending" : tv.status};
      // 已完成任务不可续跑，与原 solidify_jobs 行为一致（409）
      if (tv.status == "done") return {false, 409, "job cannot resume: done", "done"};
      auto op = queue_.retry(taskId, uid);
      if (!op.ok) return {false, op.httpStatus, op.error, ""};
      return {true, 200, "", "pending"};
    }
    return resumeLegacy(jobId, uid);
  }

  bool get(const std::string& jobId, int uid, SolidifyJobView& out)
  {
    int64_t taskId = 0;
    AiTaskView tv;
    if (parseNumeric(jobId, taskId) && queue_.get(taskId, uid, tv))
    {
      out = mapTask(tv);
      auto doneIds = db_.query(
        "SELECT ref_id FROM ai_task_items WHERE task_id=" + std::to_string(taskId) +
        " AND status IN ('done','skipped') ORDER BY item_index");
      for (auto& r : doneIds) if (!r["ref_id"].empty()) out.completedIds.push_back(std::stoll(r["ref_id"]));
      auto failed = db_.query(
        "SELECT i.ref_id, i.error, kp.user_input_content FROM ai_task_items i "
        "LEFT JOIN knowledge_points kp ON kp.id=i.ref_id "
        "WHERE i.task_id=" + std::to_string(taskId) + " AND i.status='failed' ORDER BY i.item_index");
      for (auto& r : failed)
      {
        SolidifyJobView::FailedItem f;
        f.knowledgeId = r["ref_id"].empty() ? 0 : std::stoll(r["ref_id"]);
        f.input = r["user_input_content"];
        f.error = r["error"];
        out.failedItems.push_back(f);
      }
      return true;
    }
    return getLegacy(jobId, uid, out);
  }

  std::vector<SolidifyJobView> list(int uid, int limit, int offset)
  {
    if (limit < 1) limit = 20;
    if (limit > 100) limit = 100;
    if (offset < 0) offset = 0;
    std::vector<SolidifyJobView> out;
    auto tasks = queue_.list(uid, "solidify_kp", limit + offset, 0);
    for (auto& t : tasks) out.push_back(mapTask(t));
    auto legacy = listLegacy(uid, limit + offset, 0);
    out.insert(out.end(), legacy.begin(), legacy.end());
    std::sort(out.begin(), out.end(), [](const SolidifyJobView& a, const SolidifyJobView& b) {
      return a.createTime > b.createTime;
    });
    if (offset >= (int)out.size()) return {};
    if (offset > 0) out.erase(out.begin(), out.begin() + offset);
    if ((int)out.size() > limit) out.resize(limit);
    return out;
  }

  // ── 队列 runner：执行固化任务（任务内按 3 词分片串行） ──
  bool runTask(int uid, int64_t taskId)
  {
    RequestContext::userId = uid;
    auto rows = db_.query("SELECT item_index, ref_id FROM ai_task_items WHERE task_id=" +
                          std::to_string(taskId) + " AND status='pending' ORDER BY item_index");
    std::vector<std::pair<int,int64_t>> pendingRows;
    for (auto& r : rows)
      pendingRows.push_back({std::stoi(r["item_index"]),
                             r["ref_id"].empty() ? 0 : std::stoll(r["ref_id"])});
    if (pendingRows.empty()) return true;

    auto beginAt = std::chrono::steady_clock::now();
    int timeout = queue_.config().task_timeout_sec;
    for (size_t pos = 0; pos < pendingRows.size(); pos += CHUNK_SIZE)
    {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - beginAt).count();
      if (timeout > 0 && elapsed >= timeout)
      {
        db_.exec("UPDATE ai_task_items SET status='failed', error='task timeout', "
                 "finished_at=NOW() WHERE task_id=" + std::to_string(taskId) +
                 " AND status='pending'");
        return true;
      }
      std::vector<std::pair<int,int64_t>> chunk(
        pendingRows.begin() + pos,
        pendingRows.begin() + std::min(pendingRows.size(), pos + CHUNK_SIZE));
      processChunk(taskId, uid, chunk);
      refreshTaskProgress(taskId);
    }
    return true;
  }

private:
  static bool parseNumeric(const std::string& s, int64_t& out)
  {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    try { out = std::stoll(s); return true; } catch (...) { return false; }
  }

  static SolidifyJobView mapTask(const AiTaskView& t)
  {
    SolidifyJobView v;
    v.id = std::to_string(t.id);
    v.total = t.total;
    v.done = t.done;
    v.failed = t.failed;
    v.progress = t.progress;
    v.status = (t.status == "queued") ? "pending" : t.status;
    v.error = t.error;
    v.createTime = t.createdAt;
    v.updateTime = t.updateAt;
    v.queuePosition = t.position;
    v.etaSec = t.etaSec;
    return v;
  }

  void markItemByIndex(int64_t taskId, int index, const std::string& status, const std::string& err)
  {
    std::string sql = "UPDATE ai_task_items SET status='" + status + "', error="
                      + (err.empty() ? std::string("NULL") : "'" + Db::escape(err) + "'")
                      + ", finished_at=NOW() WHERE task_id=" + std::to_string(taskId)
                      + " AND item_index=" + std::to_string(index);
    try { db_.exec(sql); } catch (...) {}
  }

  void refreshTaskProgress(int64_t taskId)
  {
    try
    {
      db_.exec("UPDATE ai_tasks SET progress=(SELECT IFNULL(ROUND((SUM(status IN ('done','skipped','failed'))"
               "/NULLIF(COUNT(*),0))*100),0) FROM ai_task_items WHERE task_id=" + std::to_string(taskId) +
               ") WHERE id=" + std::to_string(taskId));
    }
    catch (...) {}
  }

  void processChunk(int64_t taskId, int uid, const std::vector<std::pair<int,int64_t>>& chunk)
  {
    std::ostringstream indexes, refs;
    for (size_t i = 0; i < chunk.size(); ++i)
    {
      if (i) { indexes << ","; refs << ","; }
      indexes << chunk[i].first;
      refs << chunk[i].second;
    }
    db_.exec("UPDATE ai_task_items SET status='running', started_at=NOW() WHERE task_id="
             + std::to_string(taskId) + " AND status='pending' AND item_index IN (" + indexes.str() + ")");

    std::set<int64_t> already;
    auto sol = db_.query("SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid)
                         + " AND is_solidified=1 AND id IN (" + refs.str() + ")");
    for (auto& r : sol) already.insert(std::stoll(r["id"]));

    std::map<int64_t,int> indexByRef;
    std::vector<int64_t> need;
    for (auto& it : chunk)
    {
      indexByRef[it.second] = it.first;
      if (already.count(it.second)) markItemByIndex(taskId, it.first, "skipped", "");
      else need.push_back(it.second);
    }
    if (need.empty()) return;

    std::set<int64_t> reported;
    try
    {
      kp_.solidifyBatch(need, [&](const KnowledgeService::BatchItemResult& item) {
        reported.insert(item.id);
        auto f = indexByRef.find(item.id);
        if (f != indexByRef.end())
          markItemByIndex(taskId, f->second,
                          item.ok ? (item.skipped ? "skipped" : "done") : "failed", item.error);
      });
    }
    catch (const std::exception& e)
    {
      for (int64_t id : need)
      {
        if (reported.count(id)) continue;
        markItemByIndex(taskId, indexByRef[id], "failed", e.what());
      }
    }
    for (int64_t id : need)
      if (!reported.count(id)) markItemByIndex(taskId, indexByRef[id], "failed", "no result");
  }

  // ── legacy：历史 solidify_jobs 任务 ──
  bool getLegacy(const std::string& jobId, int uid, SolidifyJobView& out)
  {
    auto rows = db_.query(
      "SELECT id, total, done, failed, status, error, "
      "DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time, "
      "DATE_FORMAT(update_time, '%Y-%m-%d %H:%i:%s') AS update_time "
      "FROM solidify_jobs WHERE id='" + Db::escape(jobId) + "' AND user_id=" + std::to_string(uid));
    if (rows.empty()) return false;
    fillLegacyView(rows[0], out);
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

  std::vector<SolidifyJobView> listLegacy(int uid, int limit, int offset)
  {
    auto rows = db_.query(
      "SELECT id, total, done, failed, status, error, "
      "DATE_FORMAT(create_time, '%Y-%m-%d %H:%i:%s') AS create_time, "
      "DATE_FORMAT(update_time, '%Y-%m-%d %H:%i:%s') AS update_time "
      "FROM solidify_jobs WHERE user_id=" + std::to_string(uid)
      + " ORDER BY create_time DESC, id DESC LIMIT " + std::to_string(std::max(0, offset)) + ", "
      + std::to_string(limit));
    std::vector<SolidifyJobView> out;
    for (auto& r : rows) { SolidifyJobView v; fillLegacyView(r, v); out.push_back(v); }
    return out;
  }

  ResumeResult resumeLegacy(const std::string& jobId, int uid)
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
    bool started = startLegacy(jobId, uid);
    return {true, 200, started ? "" : "failed to start", "running"};
  }

  bool startLegacy(const std::string& jobId, int uid)
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
    std::thread([this, jobId, uid] { runLegacyJob(jobId, uid); }).detach();
    return true;
  }

  void runLegacyJob(std::string jobId, int uid)
  {
    RequestContext::userId = uid;
    auto rows = db_.query("SELECT knowledge_id FROM solidify_job_items WHERE job_id='"
                          + Db::escape(jobId) + "' AND status='pending' ORDER BY id");
    std::vector<int64_t> ids;
    for (auto& r : rows) ids.push_back(std::stoll(r["knowledge_id"]));

    for (size_t i = 0; i < ids.size(); i += CHUNK_SIZE)
    {
      std::vector<int64_t> chunk(ids.begin() + i, ids.begin() + std::min(ids.size(), i + CHUNK_SIZE));
      processLegacyChunk(jobId, uid, chunk);
    }

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

  void processLegacyChunk(const std::string& jobId, int uid, const std::vector<int64_t>& chunk)
  {
    std::ostringstream in;
    for (size_t i = 0; i < chunk.size(); i++) { if (i) in << ","; in << chunk[i]; }
    db_.exec("UPDATE solidify_job_items SET status='running' WHERE job_id='" + Db::escape(jobId)
             + "' AND status='pending' AND knowledge_id IN (" + in.str() + ")");

    std::set<int64_t> already;
    auto sol = db_.query("SELECT id FROM knowledge_points WHERE user_id=" + std::to_string(uid)
                         + " AND is_solidified=1 AND id IN (" + in.str() + ")");
    for (auto& r : sol) already.insert(std::stoll(r["id"]));
    std::vector<int64_t> need;
    for (int64_t id : chunk)
    {
      if (already.count(id)) markLegacyItem(jobId, id, "skipped", "");
      else need.push_back(id);
    }
    refreshLegacyCounts(jobId);
    if (need.empty()) return;

    std::set<int64_t> reported;
    try
    {
      kp_.solidifyBatch(need, [&](const KnowledgeService::BatchItemResult& item) {
        reported.insert(item.id);
        markLegacyItem(jobId, item.id, item.ok ? (item.skipped ? "skipped" : "done") : "failed", item.error);
        refreshLegacyCounts(jobId);
      });
    }
    catch (const std::exception& e)
    {
      for (int64_t id : need)
        if (!reported.count(id)) markLegacyItem(jobId, id, "failed", e.what());
    }
    for (int64_t id : need)
      if (!reported.count(id)) markLegacyItem(jobId, id, "failed", "no result");
    refreshLegacyCounts(jobId);
  }

  void markLegacyItem(const std::string& jobId, int64_t id, const std::string& status, const std::string& err)
  {
    std::string sql = "UPDATE solidify_job_items SET status='" + status + "', error_msg="
                      + (err.empty() ? std::string("NULL") : "'" + Db::escape(err) + "'")
                      + " WHERE job_id='" + Db::escape(jobId) + "' AND knowledge_id=" + std::to_string(id);
    try { db_.exec(sql); } catch (...) {}
  }

  void refreshLegacyCounts(const std::string& jobId)
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

  static void fillLegacyView(const Row& r, SolidifyJobView& v)
  {
    v.id = r.at("id");
    v.total = r.at("total").empty() ? 0 : std::stoi(r.at("total"));
    v.done = r.at("done").empty() ? 0 : std::stoi(r.at("done"));
    v.failed = r.at("failed").empty() ? 0 : std::stoi(r.at("failed"));
    v.status = r.at("status");
    v.error = r.at("error");
    v.createTime = r.at("create_time");
    v.updateTime = r.at("update_time");
    int processed = v.done + v.failed;
    v.progress = v.total > 0 ? (processed * 100 / v.total) : 100;
  }
};

} // namespace eng
