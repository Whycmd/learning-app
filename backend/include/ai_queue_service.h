// 通用 AI 队列（v1.5.0-T002）
// 职责：任务投递、同 lane FIFO 排队、position/eta、状态机、重试退避、超时、启动恢复
// 执行：worker 池按 kind 分派到注册的 runner；业务执行逻辑（固化/讲解等）由 runner 提供
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include "db.h"
#include "ai_provider.h"
#include "auth.h"

namespace eng {

struct AiQueueItem {
  int64_t id=0, taskId=0, refId=0;
  int index=0;
  std::string type, status, input, error;
};

struct AiTaskView {
  int64_t id=0, refId=0;
  int uid=0;
  std::string kind, lane, status, error, createdAt, updateAt, startedAt, finishedAt, modelUsed, resultRef;
  int total=0, done=0, failed=0, progress=0, retryCount=0;
  int position=0, etaSec=0;
  int queueLevel=0;  // v1.5.0-T006：0=正常队列 1..N=空闲队列
  std::vector<AiQueueItem> failedItems;
};

class AiQueueService {
public:
  using Runner = std::function<bool(int uid, int64_t taskId)>;
  using CompleteHook = std::function<void(int uid, int64_t taskId)>;

  struct NewItem { int64_t refId=0; std::string input; };
  struct CreateResult { int64_t taskId=0; int total=0; bool ok=false; int httpStatus=200; std::string error; };
  struct OpResult { bool ok=false; int httpStatus=200; std::string error; };

  AiQueueService(Db& db, int workers, const AiQueueConfig& cfg)
      : db_(db), workers_(std::max(1, workers)), cfg_(cfg) {}
  ~AiQueueService() { stop(); }

  void registerRunner(const std::string& kind, Runner fn) { runners_[kind] = std::move(fn); }
  void setCompleteHook(CompleteHook fn) { hook_ = std::move(fn); }

  void start() {
    if (!threads_.empty()) return;
    stopping_ = false;
    recoverOnStartup();
    for (int i = 0; i < workers_; ++i)
      threads_.emplace_back([this]{ workerLoop(); });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
  }

  // 启动恢复：重启后遗留 running 任务全部回 queued，running 子项回 pending（断点续跑）
  void recoverOnStartup() {
    db_.exec("UPDATE ai_task_items i JOIN ai_tasks t ON t.id=i.task_id "
             "SET i.status='pending' WHERE t.status='running' AND i.status='running'");
    db_.exec("UPDATE ai_tasks SET status='queued', started_at=NULL, next_run_at=NULL "
             "WHERE status='running'");
    cv_.notify_all();
  }

  CreateResult create(int uid, const std::string& kind, const std::string& lane,
                      const std::vector<NewItem>& items, const std::string& payloadJson = "",
                      int queueLevel = 0) {
    CreateResult r;
    if (items.empty()) { r.httpStatus = 400; r.error = "empty task"; return r; }
    if (queueSize() >= cfg_.max_size) { r.httpStatus = 429; r.error = "queue full"; return r; }
    if (queueLevel < 0) queueLevel = 0;
    if (queueLevel > cfg_.idle_max_level) queueLevel = cfg_.idle_max_level;
    int64_t firstRef = items.size() == 1 ? items[0].refId : 0;
    db_.transaction([&]() -> bool {
      auto w = db_.exec("INSERT INTO ai_tasks (task_type, ref_id, status, user_id, kind, lane, total, "
                        "payload_json, progress, retry_count, queue_level, update_time) VALUES (" +
                        q(kind) + "," + std::to_string(firstRef) + ",'queued'," + std::to_string(uid) + "," +
                        q(kind) + "," + q(lane) + "," + std::to_string((int)items.size()) + "," +
                        q(payloadJson) + ",0,0," + std::to_string(queueLevel) + ",NOW())");
      r.taskId = w.last_insert_id;
      std::string sql = "INSERT INTO ai_task_items "
                        "(task_id,item_index,item_type,ref_id,input_text,status) VALUES ";
      for (size_t i = 0; i < items.size(); ++i) {
        if (i) sql += ",";
        sql += "(" + std::to_string(r.taskId) + "," + std::to_string((int)i) + "," + q(kind) + "," +
               std::to_string(items[i].refId) + "," + q(items[i].input) + ",'pending')";
      }
      db_.exec(sql);
      return r.taskId > 0;
    });
    r.ok = r.taskId > 0;
    r.total = (int)items.size();
    if (!r.ok) { r.httpStatus = 500; r.error = "create task failed"; }
    cv_.notify_all();
    return r;
  }

  bool get(int64_t taskId, int uid, AiTaskView& out) {
    auto rows = db_.query("SELECT id,user_id,kind,lane,status,error_msg,create_time,update_time,started_at,"
                          "finished_at,model_used,total,retry_count,ref_id,result_ref,progress,queue_level FROM ai_tasks "
                          "WHERE id=" + std::to_string(taskId));
    if (rows.empty()) return false;
    const auto& row = rows[0];
    int owner = std::stoi(row.at("user_id"));
    if (uid >= 0 && owner != uid) return false;
    out = AiTaskView();
    out.id = taskId;
    out.uid = owner;
    out.kind = row.at("kind");
    out.lane = row.at("lane");
    out.status = row.at("status");
    out.error = row.at("error_msg");
    out.createdAt = row.at("create_time");
    out.updateAt = row.at("update_time");
    out.startedAt = row.at("started_at");
    out.finishedAt = row.at("finished_at");
    out.modelUsed = row.at("model_used");
    out.total = std::stoi(row.at("total"));
    out.retryCount = std::stoi(row.at("retry_count"));
    out.refId = row.at("ref_id").empty() ? 0 : std::stoll(row.at("ref_id"));
    out.resultRef = row.count("result_ref") ? row.at("result_ref") : "";
    out.queueLevel = row.count("queue_level") && !row.at("queue_level").empty()
                     ? std::stoi(row.at("queue_level")) : 0;
    fillStats(taskId, out);
    // v1.5.0-T002：ETA 随时间递减，并统一加 5%+1 秒余量，避免过早显示「即将完成」
    // 运行中：原值 = (按已耗时剩余 + 按剩余进度) / 2
    // 排队中：前方排队任务整任务估时 + 执行中任务剩余（按 worker 并发分摊）
    if (out.status == "queued") {
      out.position = positionOf(taskId, out.queueLevel, out.createdAt);
      int avg = avgCostSec(out.kind);
      int runningRemain = 0;
      auto running = db_.query(
        "SELECT TIMESTAMPDIFF(SECOND, started_at, NOW()) AS elapsed_sec, progress FROM ai_tasks "
        "WHERE queue_level=" + std::to_string(out.queueLevel) + " AND status='running'");
      for (auto& rr : running) {
        int elapsed = rr.at("elapsed_sec").empty() ? 0 : std::max(0, std::stoi(rr.at("elapsed_sec")));
        int byTime = std::max(0, avg - elapsed);
        int progress = rr.at("progress").empty() ? 0 : std::stoi(rr.at("progress"));
        int byProgress = avg * std::max(0, 100 - progress) / 100;
        runningRemain += (byTime + byProgress) / 2;
      }
      if (workers_ > 1) runningRemain = (runningRemain + workers_ - 1) / workers_;
      out.etaSec = (int)((out.position * avg + runningRemain) * 1.05 + 1);
    } else if (out.status == "running") {
      int avg = avgCostSec(out.kind);
      int elapsed = 0;
      if (!out.startedAt.empty()) {
        auto er = db_.query("SELECT TIMESTAMPDIFF(SECOND, '" + Db::escape(out.startedAt) + "', NOW()) AS e");
        if (!er.empty() && !er[0].at("e").empty()) elapsed = std::max(0, std::stoi(er[0].at("e")));
      }
      int byTime = std::max(0, avg - elapsed);
      int byProgress = avg * std::max(0, 100 - out.progress) / 100;
      out.etaSec = (int)(((byTime + byProgress) / 2) * 1.05 + 1);
    }
    // 失败项（最多 50 条，供前端展示）
    auto items = db_.query("SELECT id,task_id,item_index,item_type,ref_id,input_text,status,error "
                           "FROM ai_task_items WHERE task_id=" + std::to_string(taskId) +
                           " AND status='failed' ORDER BY item_index LIMIT 50");
    for (const auto& it : items) {
      AiQueueItem v;
      v.id = (int64_t)std::stoll(it.at("id"));
      v.taskId = taskId;
      v.index = std::stoi(it.at("item_index"));
      v.type = it.at("item_type");
      v.refId = it.at("ref_id").empty() ? 0 : std::stoll(it.at("ref_id"));
      v.input = it.at("input_text");
      v.status = it.at("status");
      v.error = it.at("error");
      out.failedItems.push_back(std::move(v));
    }
    return true;
  }

  std::vector<AiTaskView> list(int uid, const std::string& kind, int limit, int offset) {
    if (limit <= 0) limit = 20;
    if (limit > 100) limit = 100;
    std::string w = " WHERE user_id=" + std::to_string(uid);
    if (!kind.empty()) w += " AND kind=" + q(kind);
    auto rows = db_.query("SELECT id FROM ai_tasks" + w +
                          " ORDER BY create_time DESC, id DESC LIMIT " + std::to_string(limit) +
                          " OFFSET " + std::to_string(std::max(0, offset)));
    std::vector<AiTaskView> out;
    for (const auto& row : rows) {
      AiTaskView v;
      if (get((int64_t)std::stoll(row.at("id")), uid, v)) out.push_back(std::move(v));
    }
    return out;
  }

  // 手动重试/继续：失败与 pending 子项重置，任务回 queued
  OpResult retry(int64_t taskId, int uid) {
    OpResult r;
    AiTaskView v;
    if (!get(taskId, uid, v)) { r.httpStatus = 404; r.error = "task not found"; return r; }
    if (v.status == "queued" || v.status == "running") {
      r.httpStatus = 409; r.error = "task already active"; return r;
    }
    db_.exec("UPDATE ai_task_items SET status='pending', error=NULL, started_at=NULL, "
             "finished_at=NULL WHERE task_id=" + std::to_string(taskId) +
             " AND status IN ('failed','pending')");
    db_.exec("UPDATE ai_tasks SET status='queued', retry_count=0, next_run_at=NULL, "
             "finished_at=NULL, error_msg=NULL, started_at=NULL, update_time=NOW() "
             "WHERE id=" + std::to_string(taskId));
    r.ok = true;
    cv_.notify_all();
    return r;
  }

  // v1.5.0-T006：加急，转回正常队列（仅排队中有效）
  OpResult expedite(int64_t taskId, int uid) {
    OpResult r;
    AiTaskView v;
    if (!get(taskId, uid, v)) { r.httpStatus = 404; r.error = "task not found"; return r; }
    if (v.status != "queued") { r.httpStatus = 409; r.error = "task not queued"; return r; }
    if (v.queueLevel == 0) { r.ok = true; return r; }
    db_.exec("UPDATE ai_tasks SET queue_level=0, expedited_at=NOW(), update_time=NOW() WHERE id=" +
             std::to_string(taskId));
    cv_.notify_all();
    r.ok = true;
    return r;
  }

  // v1.5.0-T006：转入空闲队列（level 1..idle_max_level）
  OpResult defer(int64_t taskId, int uid, int level) {
    OpResult r;
    AiTaskView v;
    if (!get(taskId, uid, v)) { r.httpStatus = 404; r.error = "task not found"; return r; }
    if (v.status != "queued") { r.httpStatus = 409; r.error = "task not queued"; return r; }
    if (level < 1 || level > cfg_.idle_max_level) { r.httpStatus = 400; r.error = "invalid level"; return r; }
    db_.exec("UPDATE ai_tasks SET queue_level=" + std::to_string(level) + ", update_time=NOW() WHERE id=" +
             std::to_string(taskId));
    r.ok = true;
    return r;
  }

  int queueSize() {
    auto rows = db_.query("SELECT COUNT(*) AS c FROM ai_tasks WHERE status IN ('queued','running')");
    return rows.empty() ? 0 : std::stoi(rows[0].at("c"));
  }

  int avgCostSec(const std::string& kind) {
    auto rows = db_.query("SELECT AVG(TIMESTAMPDIFF(SECOND, started_at, finished_at)) AS avg_sec "
                          "FROM (SELECT started_at, finished_at FROM ai_tasks WHERE kind=" + q(kind) +
                          " AND status IN ('done','partial','failed') AND started_at IS NOT NULL "
                          "AND finished_at IS NOT NULL ORDER BY finished_at DESC LIMIT 20) t");
    if (rows.empty()) return cfg_.avg_cost_cold_start_sec;
    std::string s = rows[0].at("avg_sec");
    if (s.empty()) return cfg_.avg_cost_cold_start_sec;
    double v = std::atof(s.c_str());
    return v <= 0 ? cfg_.avg_cost_cold_start_sec : std::max(1, (int)v);
  }

  const AiQueueConfig& config() const { return cfg_; }

private:
  // 将已失败任务在退避后自动回队；返回 true 表示已安排重试
  bool maybeAutoRetry(int64_t taskId, const std::string& kind, int retryCount, int failed, int done) {
    if (failed <= 0 && done <= 0) return false;
    if (retryCount >= cfg_.retry_max) return false;
    int wait = cfg_.backoff_base_sec * (1 << std::min(3, retryCount));
    db_.exec("UPDATE ai_task_items SET status='pending', error=NULL, started_at=NULL, "
             "finished_at=NULL WHERE task_id=" + std::to_string(taskId) + " AND status='failed'");
    db_.exec("UPDATE ai_tasks SET status='queued', retry_count=" + std::to_string(retryCount + 1) +
             ", next_run_at=DATE_ADD(NOW(), INTERVAL " + std::to_string(wait) + " SECOND), "
             "started_at=NULL, finished_at=NULL, error_msg=NULL, update_time=NOW() WHERE id=" +
             std::to_string(taskId));
    return true;
  }

  // 同级队列内位置（v1.5.0-T006：按 queue_level 分组，跨级互不影响）
  int positionOf(int64_t taskId, int queueLevel, const std::string& createdAt) {
    auto rows = db_.query(
      "SELECT COUNT(*) AS c FROM ai_tasks WHERE queue_level=" + std::to_string(queueLevel) +
      " AND status='queued' AND id<>" + std::to_string(taskId) +
      " AND (create_time < '" + Db::escape(createdAt) + "' OR (create_time='"
      + Db::escape(createdAt) + "' AND id<" + std::to_string(taskId) + "))");
    return rows.empty() ? 0 : std::stoi(rows[0].at("c"));
  }

  void fillStats(int64_t taskId, AiTaskView& v) {
    auto rows = db_.query("SELECT COUNT(*) AS total, "
                          "SUM(status IN ('done','skipped')) AS done, "
                          "SUM(status='failed') AS failed FROM ai_task_items WHERE task_id=" +
                          std::to_string(taskId));
    if (!rows.empty()) {
      const auto& r = rows[0];
      v.total = r.at("total").empty() ? 0 : std::stoi(r.at("total"));
      v.done = r.at("done").empty() ? 0 : std::stoi(r.at("done"));
      v.failed = r.at("failed").empty() ? 0 : std::stoi(r.at("failed"));
    }
    v.progress = v.total > 0 ? (v.done + v.failed) * 100 / v.total : 0;
  }

  static std::string q(const std::string& s) { return "'" + Db::escape(s) + "'"; }

  void workerLoop() {
    while (true) {
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::seconds(1), [this]{ return stopping_.load(); });
        if (stopping_) return;
      }
      int64_t taskId = 0; int uid = 0; std::string kind;
      if (!claimNext(taskId, uid, kind)) continue;
      try {
        process(taskId, uid, kind);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[ai-queue] task %lld runner exception: %s\n",
                     (long long)taskId, e.what());
        finish(taskId, uid, kind, false);
      } catch (...) {
        finish(taskId, uid, kind, false);
      }
    }
  }

  bool claimNext(int64_t& taskId, int& uid, std::string& kind) {
    int64_t picked = 0;
    if (!pickClaimable(picked)) return false;
    auto rows = db_.query("SELECT user_id,kind FROM ai_tasks WHERE id=" + std::to_string(picked));
    if (rows.empty()) return false;
    taskId = picked;
    uid = std::stoi(rows[0].at("user_id"));
    kind = rows[0].at("kind");
    // 原子抢占，避免多 worker 重复执行
    auto w = db_.exec("UPDATE ai_tasks SET status='running', started_at=NOW(), update_time=NOW() "
                      "WHERE id=" + std::to_string(taskId) + " AND status='queued'");
    return w.affected_rows == 1;
  }

  // v1.5.0-T006：分级领取
  // 1) 正常队列（queue_level=0）永远优先；
  // 2) 空闲队列仅在正常队列无排队、无执行中，且空闲任务未占满 idle_max_workers 时领取；
  // 3) 空闲队列按层级从低到高，同级 FIFO。
  bool pickClaimable(int64_t& outId) {
    auto normal = db_.query(
      "SELECT id FROM ai_tasks WHERE status='queued' AND queue_level=0 "
      "AND (next_run_at IS NULL OR next_run_at <= NOW()) ORDER BY create_time, id LIMIT 1");
    if (!normal.empty()) { outId = (int64_t)std::stoll(normal[0].at("id")); return true; }
    if (cfg_.idle_max_workers <= 0) return false;
    auto busy = db_.query("SELECT COUNT(*) AS c FROM ai_tasks "
                          "WHERE status='running' AND queue_level=0");
    if (!busy.empty() && std::stoi(busy[0].at("c")) > 0) return false;
    auto idleRunning = db_.query("SELECT COUNT(*) AS c FROM ai_tasks "
                                 "WHERE status='running' AND queue_level>0");
    if (!idleRunning.empty() && std::stoi(idleRunning[0].at("c")) >= cfg_.idle_max_workers) return false;
    auto idle = db_.query(
      "SELECT id FROM ai_tasks WHERE status='queued' AND queue_level>0 "
      "AND (next_run_at IS NULL OR next_run_at <= NOW()) "
      "ORDER BY queue_level, create_time, id LIMIT 1");
    if (idle.empty()) return false;
    outId = (int64_t)std::stoll(idle[0].at("id"));
    return true;
  }

  void process(int64_t taskId, int uid, const std::string& kind) {
    // v1.5.0-T011：worker 线程恢复任务归属用户，保证 AI 调用留痕能记到正确 user_id
    int prevUid = RequestContext::userId;
    RequestContext::userId = uid;
    bool ok = false;
    auto it = runners_.find(kind);
    if (it != runners_.end() && it->second) ok = it->second(uid, taskId);
    RequestContext::userId = prevUid;
    finish(taskId, uid, kind, ok);
  }

  void finish(int64_t taskId, int uid, const std::string& kind, bool runnerOk) {
    AiTaskView v;
    if (!get(taskId, -1, v)) return;
    int retryCount = v.retryCount;
    std::string status;
    if (v.failed == 0 && v.done >= v.total) status = "done";
    else if (v.done > 0) status = "partial";
    else status = "failed";

    if (status != "done" && maybeAutoRetry(taskId, kind, retryCount, v.failed, v.done)) return;

    db_.exec("UPDATE ai_tasks SET status=" + q(status) + ", progress=" +
             std::to_string(v.total > 0 ? (v.done + v.failed) * 100 / v.total : 0) +
             ", finished_at=NOW(), update_time=NOW() WHERE id=" + std::to_string(taskId));
    if (status == "done" && hook_) {
      try { hook_(uid, taskId); } catch (...) {}
    }
    (void)runnerOk;
  }

  Db& db_;
  int workers_;
  AiQueueConfig cfg_;
  std::map<std::string, Runner> runners_;
  CompleteHook hook_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stopping_{false};
};

} // namespace eng
