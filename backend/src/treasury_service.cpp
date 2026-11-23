// 点金圣殿 服务层实现（v1.3.0-T005；v1.5.1-T001 任务卡）
#include "treasury_service.h"
#include "auth.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iostream>

namespace eng {

// ========== 内部辅助 ==========

bool TreasuryService::loadConfig()
{
  std::ifstream f(configPath_);
  if (!f)
  {
    std::cerr << "[treasury] cannot open config: " << configPath_ << std::endl;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  rewardsJson_ = ss.str();
  try {
    rewardsCfg_ = parseJson(rewardsJson_);
  } catch (...) {
    std::cerr << "[treasury] invalid config JSON" << std::endl;
    return false;
  }
  return !rewardsCfg_.isNull();
}

bool TreasuryService::loadMetrics()
{
  std::ifstream f(metricsPath_);
  if (!f)
  {
    std::cerr << "[treasury] cannot open metrics: " << metricsPath_ << std::endl;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  try {
    metricsCfg_ = parseJson(ss.str());
  } catch (...) {
    std::cerr << "[treasury] invalid metrics JSON" << std::endl;
    return false;
  }
  return !metricsCfg_.isNull();
}

int TreasuryService::getIncomeAmount(const std::string& code) const
{
  const JsonValue* inc = rewardsCfg_.find("income");
  if (!inc || !inc->isObject()) return 0;
  const JsonValue* item = inc->find(code);
  if (!item || !item->isObject()) return 0;
  return (int)item->getInt("amount", 0);
}

// [CONFIG] goal_tasks 参数读取
static int goalTaskCfgInt(const JsonValue& cfg, const std::string& key, int def)
{
  const JsonValue* gt = cfg.find("goal_tasks");
  return (gt && gt->isObject()) ? (int)gt->getInt(key, def) : def;
}

static double goalTaskCfgDouble(const JsonValue& cfg, const std::string& key, double def)
{
  const JsonValue* gt = cfg.find("goal_tasks");
  if (!gt || !gt->isObject()) return def;
  const JsonValue* v = gt->find(key);
  return v ? v->asDouble() : def;
}

bool TreasuryService::hasEarnedToday(const std::string& code, int uid)
{
  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM user_coin_txn "
    "WHERE user_id=" + std::to_string(uid)
    + " AND reason_code='" + Db::escape(code)
    + "' AND DATE(created_at)=CURDATE()");
  return !rows.empty() && std::stoi(rows[0]["c"]) > 0;
}

int TreasuryService::calcTokenCost(int promptTokens, int completionTokens) const
{
  const JsonValue* exp = rewardsCfg_.find("expense");
  if (!exp || !exp->isObject()) return 0;
  int divisor = (int)exp->getInt("base_divisor", 2000);
  int cap = (int)exp->getInt("base_cap", 6);
  if (divisor <= 0) divisor = 2000;
  int cost = (int)((promptTokens + completionTokens) / divisor);
  if (cost > cap) cost = cap;
  return cost;
}

bool TreasuryService::applyInternal(int delta, const std::string& code, int ref_id)
{
  return db_.transaction([&]() -> bool {
    // 幂等检查
    auto existing = db_.query(
      "SELECT id FROM user_coin_txn "
      "WHERE user_id=" + std::to_string(RequestContext::currentUserId())
      + " AND reason_code='" + Db::escape(code)
      + "' AND ref_id=" + std::to_string(ref_id)
      + " AND DATE(created_at)=CURDATE() LIMIT 1");
    if (!existing.empty()) return true; // 已入账，幂等跳过

    int uid = RequestContext::currentUserId();

    // 确保账户存在
    auto acc = db_.query(
      "SELECT id, balance, total_earned, total_spent FROM user_coin WHERE user_id="
      + std::to_string(uid) + " LIMIT 1");
    if (acc.empty())
    {
      db_.exec("INSERT INTO user_coin (user_id, balance, total_earned, total_spent) "
               "VALUES (" + std::to_string(uid) + ",0,0,0)");
    }

    // 读取当前余额
    auto rows = db_.query(
      "SELECT balance, total_earned, total_spent FROM user_coin WHERE user_id="
      + std::to_string(uid) + " LIMIT 1");
    int balance = rows.empty() ? 0 : std::stoi(rows[0]["balance"]);
    int64_t total_earned = rows.empty() ? 0 : std::stoll(rows[0]["total_earned"]);
    int64_t total_spent = rows.empty() ? 0 : std::stoll(rows[0]["total_spent"]);

    // 扣费前检查余额
    if (delta < 0 && balance + delta < 0)
    {
      std::cerr << "[treasury] insufficient balance: " << balance << " < " << -delta << std::endl;
      return false;
    }

    balance += delta;
    if (delta > 0) total_earned += delta;
    else total_spent += -delta;

    // 写流水
    db_.exec(
      "INSERT INTO user_coin_txn (user_id, delta, reason_code, ref_id, balance_after) "
      "VALUES (" + std::to_string(uid) + "," + std::to_string(delta)
      + ",'" + Db::escape(code) + "'," + std::to_string(ref_id)
      + "," + std::to_string(balance) + ")");

    // 更新余额
    db_.exec(
      "UPDATE user_coin SET balance=" + std::to_string(balance)
      + ",total_earned=" + std::to_string(total_earned)
      + ",total_spent=" + std::to_string(total_spent)
      + ",updated_at=NOW() WHERE user_id=" + std::to_string(uid));

    return true;
  });
}

bool TreasuryService::ensureAccount(int uid)
{
  auto rows = db_.query(
    "SELECT id FROM user_coin WHERE user_id=" + std::to_string(uid) + " LIMIT 1");
  if (!rows.empty()) return true;
  db_.exec("INSERT INTO user_coin (user_id, balance, total_earned, total_spent) "
           "VALUES (" + std::to_string(uid) + ",0,0,0)");
  return true;
}

int TreasuryService::currentYearWeek()
{
  // ISO 周年×100+周号
  time_t now = time(nullptr);
  struct tm* t = gmtime(&now);
  int year = t->tm_year + 1900;
  // 计算 ISO 周
  int dayOfYear = t->tm_yday + 1;
  int dow = t->tm_wday == 0 ? 7 : t->tm_wday; // Monday=1
  int week = (dayOfYear - dow + 10) / 7;
  if (week < 1)
  {
    // 属于上一年的最后一周
    year -= 1;
    week = 52;
  }
  else if (week > 52)
  {
    // 属于下一年的第一周
    year += 1;
    week = 1;
  }
  return year * 100 + week;
}

// ========== v1.5.1-T001：任务卡辅助 ==========

std::string TreasuryService::weekMonday(int offsetWeeks)
{
  std::string off = offsetWeeks == 0 ? "0" : std::to_string(offsetWeeks);
  auto rows = db_.query(
    "SELECT DATE_FORMAT(DATE_ADD(DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) DAY),"
    " INTERVAL " + off + " WEEK),'%Y-%m-%d') AS wk");
  return rows.empty() ? std::string() : rows[0]["wk"];
}

const JsonValue* TreasuryService::metricConfig(const std::string& metricKey) const
{
  const JsonValue* arr = metricsCfg_.find("metrics");
  if (!arr || !arr->isArray()) return nullptr;
  for (auto& m : arr->asArray())
  {
    if (m.getString("metric_key") == metricKey) return &m;
  }
  return nullptr;
}

bool TreasuryService::isRegisteredMetric(const std::string& metricKey) const
{
  return metricConfig(metricKey) != nullptr;
}

int TreasuryService::decayAmount(int base, int planRank) const
{
  double k = goalTaskCfgDouble(rewardsCfg_, "multi_plan_decay", 0.5);
  if (planRank < 1) planRank = 1;
  double v = base * std::pow(k, planRank - 1);
  if (v < 1.0) return 0;
  return (int)std::llround(v);
}

int TreasuryService::goalWeekEarned(int uid, const std::string& weekKey, int goalId) const
{
  auto rows = db_.query(
    "SELECT COALESCE(SUM(delta),0) AS s FROM user_coin_txn WHERE user_id="
    + std::to_string(uid) + " AND reason_code IN ('goal_task_item','goal_week_complete')"
    + " AND FLOOR(ref_id/10)=" + std::to_string(goalId)
    + " AND created_at >= '" + Db::escape(weekKey) + "'"
    + " AND created_at < DATE_ADD('" + Db::escape(weekKey) + "', INTERVAL 7 DAY)");
  return rows.empty() ? 0 : std::stoi(rows[0]["s"]);
}

int TreasuryService::weekRewardEarned(int uid, const std::string& weekKey) const
{
  auto rows = db_.query(
    "SELECT COALESCE(SUM(delta),0) AS s FROM user_coin_txn WHERE user_id="
    + std::to_string(uid) + " AND reason_code IN ('goal_task_item','goal_week_complete')"
    + " AND created_at >= '" + Db::escape(weekKey) + "'"
    + " AND created_at < DATE_ADD('" + Db::escape(weekKey) + "', INTERVAL 7 DAY)");
  return rows.empty() ? 0 : std::stoi(rows[0]["s"]);
}

bool TreasuryService::hasCompleteReward(int uid, int goalId, const std::string& weekKey)
{
  auto rows = db_.query(
    "SELECT id FROM user_coin_txn WHERE user_id=" + std::to_string(uid)
    + " AND reason_code='goal_week_complete' AND ref_id=" + std::to_string(goalId * 10 + 9)
    + " AND created_at >= '" + Db::escape(weekKey) + "'"
    + " AND created_at < DATE_ADD('" + Db::escape(weekKey) + "', INTERVAL 7 DAY) LIMIT 1");
  return !rows.empty();
}

void TreasuryService::syncNewWordProgress(int uid, const std::string& weekKey, int goalId)
{
  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM user_learning WHERE user_id=" + std::to_string(uid)
    + " AND create_time >= '" + Db::escape(weekKey) + "'"
    + " AND create_time < DATE_ADD('" + Db::escape(weekKey) + "', INTERVAL 7 DAY)");
  int live = rows.empty() ? 0 : std::stoi(rows[0]["c"]);
  auto task = db_.query(
    "SELECT progress, effective_target FROM user_weekly_goal WHERE user_id="
    + std::to_string(uid) + " AND week_key='" + Db::escape(weekKey) + "' AND goal_id="
    + std::to_string(goalId) + " AND metric_key='new_words' LIMIT 1");
  if (task.empty()) return;
  int progress = std::stoi(task[0]["progress"]);
  if (live > progress)
  {
    int eff = std::stoi(task[0]["effective_target"]);
    int achieved = live >= eff ? 1 : 0;
    db_.exec(
      "UPDATE user_weekly_goal SET progress=" + std::to_string(live)
      + ",achieved=" + std::to_string(achieved) + " WHERE user_id=" + std::to_string(uid)
      + " AND week_key='" + Db::escape(weekKey) + "' AND goal_id=" + std::to_string(goalId)
      + " AND metric_key='new_words'");
  }
}

// D28：一个真实学习事件只归属一个计划，避免多计划重复计数
int TreasuryService::resolveMetricPlan(int uid, const std::string& weekKey,
                                       const std::string& metricKey)
{
  auto gs = db_.query(
    "SELECT id FROM user_goals WHERE user_id=" + std::to_string(uid)
    + " AND status='active' AND (activate_week IS NULL OR activate_week<='" + Db::escape(weekKey) + "')"
    " ORDER BY is_current DESC, id ASC");
  if (gs.empty()) return 0;
  for (auto& g : gs) ensureWeeklyTasks(uid, std::stoi(g["id"]));

  // 优先「正在执行」的计划
  auto cur = db_.query(
    "SELECT g.goal_id AS goal_id FROM user_weekly_goal g"
    " JOIN user_goals ug ON ug.id=g.goal_id AND ug.user_id=g.user_id"
    " WHERE g.user_id=" + std::to_string(uid) + " AND g.week_key='" + Db::escape(weekKey)
    + "' AND g.metric_key='" + Db::escape(metricKey) + "' AND g.goal_id>0"
    " AND ug.is_current=1 AND ug.status='active' LIMIT 1");
  if (!cur.empty()) return std::stoi(cur[0]["goal_id"]);

  // 主计划不存在或没有该指标任务 → 取本周参与时间最早的计划
  auto any = db_.query(
    "SELECT g.goal_id AS goal_id, MIN(g.joined_at) AS joined FROM user_weekly_goal g"
    " JOIN user_goals ug ON ug.id=g.goal_id AND ug.user_id=g.user_id"
    " WHERE g.user_id=" + std::to_string(uid) + " AND g.week_key='" + Db::escape(weekKey)
    + "' AND g.metric_key='" + Db::escape(metricKey) + "' AND g.goal_id>0"
    " AND ug.status='active' GROUP BY g.goal_id"
    " ORDER BY joined ASC, g.goal_id ASC LIMIT 1");
  return any.empty() ? 0 : std::stoi(any[0]["goal_id"]);
}

void TreasuryService::ensureWeeklyTasks(int uid, int goalId)
{
  if (goalId <= 0) return;
  auto g = db_.query(
    "SELECT id, name, is_current, DATE_FORMAT(activate_week,'%Y-%m-%d') AS activate_week"
    " FROM user_goals WHERE id=" + std::to_string(goalId) + " AND user_id="
    + std::to_string(uid) + " AND status='active' LIMIT 1");
  if (g.empty()) return;
  std::string wk = weekMonday();
  // 未来生效的计划本周不产生任务
  if (!g[0]["activate_week"].empty() && g[0]["activate_week"] > wk) return;

  auto existing = db_.query(
    "SELECT COUNT(*) AS c FROM user_weekly_goal WHERE user_id=" + std::to_string(uid)
    + " AND week_key='" + Db::escape(wk) + "' AND goal_id=" + std::to_string(goalId));
  int count = existing.empty() ? 0 : std::stoi(existing[0]["c"]);
  if (count > 0) return;

  // D25：所有已生效课规都生成本周任务；正在执行固定第 1 位，其余按进入顺序递补
  int rank = 1;
  if (g[0]["is_current"] != "1")
  {
    auto rankRows = db_.query(
      "SELECT COALESCE(MAX(plan_rank),0) AS r FROM user_weekly_goal WHERE user_id="
      + std::to_string(uid) + " AND week_key='" + Db::escape(wk) + "' AND goal_id>0");
    rank = (rankRows.empty() ? 0 : std::stoi(rankRows[0]["r"])) + 1;
  }

  auto ywRows = db_.query(
    "SELECT YEARWEEK('" + Db::escape(wk) + "',3) AS yw");
  std::string yw = ywRows.empty() ? "0" : ywRows[0]["yw"];

  JsonArray metrics;
  const JsonValue* def = metricsCfg_.find("default_tasks");
  if (def && def->isArray())
  {
    for (auto& m : def->asArray()) if (m.isString()) metrics.push_back(m);
  }
  if (metrics.empty())
  {
    metrics.push_back(JsonValue::makeString("review_items"));
    metrics.push_back(JsonValue::makeString("new_words"));
    metrics.push_back(JsonValue::makeString("quiz_count"));
  }

  for (auto& mv : metrics)
  {
    std::string key = mv.asString();
    const JsonValue* mc = metricConfig(key);
    if (!mc) continue; // 未登记指标不计分
    int target = (int)mc->getInt("default_target", 1);
    if (target < 1) target = 1;
    int floor = computeMetricFloor(uid, key);
    int eff = std::max(target, floor);
    db_.exec(
      "INSERT IGNORE INTO user_weekly_goal (user_id, year_week, week_key, goal_id, metric_key,"
      " goal_type, target, progress, achieved, plan_rank, joined_at, effective_target, claimed)"
      " VALUES (" + std::to_string(uid) + "," + yw + ",'" + Db::escape(wk) + "',"
      + std::to_string(goalId) + ",'" + Db::escape(key) + "','" + Db::escape(key) + "',"
      + std::to_string(target) + ",0,0," + std::to_string(rank) + ",NOW(),"
      + std::to_string(eff) + ",0)");
  }
}

void TreasuryService::normalizePlanRanks(int uid, const std::string& weekKey)
{
  auto rows = db_.query(
    "SELECT g.goal_id AS goal_id, MIN(g.joined_at) AS joined, MAX(ug.is_current) AS is_current"
    " FROM user_weekly_goal g JOIN user_goals ug ON ug.id=g.goal_id AND ug.user_id=g.user_id"
    " WHERE g.user_id=" + std::to_string(uid) + " AND g.week_key='" + Db::escape(weekKey) + "'"
    " AND g.goal_id>0 GROUP BY g.goal_id");
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
  {
    bool ca = a.at("is_current") == "1", cb = b.at("is_current") == "1";
    if (ca != cb) return ca;
    if (a.at("joined") != b.at("joined")) return a.at("joined") < b.at("joined");
    return std::stoi(a.at("goal_id")) < std::stoi(b.at("goal_id"));
  });
  int rank = 1;
  for (auto& r : rows)
  {
    db_.exec(
      "UPDATE user_weekly_goal SET plan_rank=" + std::to_string(rank)
      + " WHERE user_id=" + std::to_string(uid) + " AND week_key='" + Db::escape(weekKey)
      + "' AND goal_id=" + r["goal_id"]);
    rank++;
  }
}

int TreasuryService::computeMetricFloor(int uid, const std::string& metricKey)
{
  const JsonValue* mc = metricConfig(metricKey);
  if (!mc) return 0;
  int defTarget = (int)mc->getInt("default_target", 1);
  int absMin = (int)mc->getInt("abs_min", 1);
  int absMax = (int)mc->getInt("abs_max", 100000);
  double ratio = goalTaskCfgDouble(rewardsCfg_, "threshold_ratio", 0.6);
  int lookback = goalTaskCfgInt(rewardsCfg_, "lookback_weeks", 4);

  std::string wk = weekMonday();
  // D25：门槛按「同一周单计划最高进度」统计，避免多计划重复计数抬高门槛
  auto rows = db_.query(
    "SELECT week_key, MAX(p) AS p, MAX(t) AS t FROM ("
    " SELECT week_key, goal_id, SUM(progress) AS p, SUM(effective_target) AS t"
    " FROM user_weekly_goal WHERE user_id=" + std::to_string(uid)
    + " AND metric_key='" + Db::escape(metricKey) + "'"
    + " AND goal_id>0 AND week_key < '" + Db::escape(wk) + "'"
    + " AND week_key >= DATE_SUB('" + Db::escape(wk) + "', INTERVAL " + std::to_string(lookback) + " WEEK)"
    + " GROUP BY week_key, goal_id) x GROUP BY week_key ORDER BY week_key");

  auto clamp = [&](int v) {
    if (v < absMin) v = absMin;
    if (v > absMax) v = absMax;
    return v;
  };

  if ((int)rows.size() < 2) return clamp(defTarget);

  std::vector<int> vals;
  for (auto& r : rows) vals.push_back(std::stoi(r["p"]));
  std::sort(vals.begin(), vals.end());
  double median;
  size_t n = vals.size();
  if (n % 2 == 1) median = vals[n / 2];
  else median = (vals[n / 2 - 1] + vals[n / 2]) / 2.0;

  int floorV = clamp((int)std::llround(ratio * median));

  // 连续 3 周超额（完成率 > up_ratio）→ 上调；连续 2 周完成率 < down_rate → 下调
  double upRatio = goalTaskCfgDouble(rewardsCfg_, "up_ratio", 2.0);
  double upFactor = goalTaskCfgDouble(rewardsCfg_, "up_factor", 1.2);
  double downRate = goalTaskCfgDouble(rewardsCfg_, "down_rate", 0.5);
  double downFactor = goalTaskCfgDouble(rewardsCfg_, "down_factor", 0.8);

  int upStreak = 0, downStreak = 0;
  for (auto it = rows.rbegin(); it != rows.rend(); ++it)
  {
    int p = std::stoi((*it)["p"]);
    int t = std::stoi((*it)["t"]);
    double rate = t > 0 ? (double)p / t : 0.0;
    if (rate > upRatio) upStreak++;
    else break;
  }
  for (auto it = rows.rbegin(); it != rows.rend(); ++it)
  {
    int p = std::stoi((*it)["p"]);
    int t = std::stoi((*it)["t"]);
    double rate = t > 0 ? (double)p / t : 0.0;
    if (rate < downRate) downStreak++;
    else break;
    if (downStreak >= 2) break;
  }
  if (upStreak >= 3) floorV = (int)std::llround(floorV * upFactor);
  else if (downStreak >= 2) floorV = (int)std::llround(floorV * downFactor);
  return clamp(floorV);
}

// D29：自动推荐目标（近 4 周单计划最高进度的中位数；历史不足 2 周用默认值）
int TreasuryService::computeSuggestion(int uid, const std::string& metricKey)
{
  const JsonValue* mc = metricConfig(metricKey);
  if (!mc) return 0;
  int defTarget = (int)mc->getInt("default_target", 1);
  int absMin = (int)mc->getInt("abs_min", 1);
  int absMax = (int)mc->getInt("abs_max", 100000);
  int lookback = goalTaskCfgInt(rewardsCfg_, "lookback_weeks", 4);

  auto clamp = [&](int v) {
    if (v < absMin) v = absMin;
    if (v > absMax) v = absMax;
    return v;
  };

  std::string wk = weekMonday();
  auto rows = db_.query(
    "SELECT week_key, MAX(p) AS p FROM ("
    " SELECT week_key, goal_id, SUM(progress) AS p FROM user_weekly_goal"
    " WHERE user_id=" + std::to_string(uid)
    + " AND metric_key='" + Db::escape(metricKey) + "'"
    + " AND goal_id>0 AND week_key < '" + Db::escape(wk) + "'"
    + " AND week_key >= DATE_SUB('" + Db::escape(wk) + "', INTERVAL " + std::to_string(lookback) + " WEEK)"
    + " GROUP BY week_key, goal_id) x GROUP BY week_key ORDER BY week_key");

  if ((int)rows.size() < 2) return clamp(defTarget);

  std::vector<int> vals;
  for (auto& r : rows) vals.push_back(std::stoi(r["p"]));
  std::sort(vals.begin(), vals.end());
  size_t n = vals.size();
  double median = (n % 2 == 1) ? vals[n / 2] : (vals[n / 2 - 1] + vals[n / 2]) / 2.0;
  return clamp((int)std::llround(median));
}

JsonArray TreasuryService::getSuggestions(int uid)
{
  JsonArray arr;
  const JsonValue* ms = metricsCfg_.find("metrics");
  if (!ms || !ms->isArray()) return arr;
  for (auto& m : ms->asArray())
  {
    std::string key = m.getString("metric_key");
    JsonObject o;
    o["metric_key"] = JsonValue::makeString(key);
    o["name"] = JsonValue::makeString(m.getString("name"));
    o["unit"] = JsonValue::makeString(m.getString("unit"));
    o["default_target"] = JsonValue::makeInt(m.getInt("default_target", 1));
    o["suggested"] = JsonValue::makeInt(computeSuggestion(uid, key));
    arr.push_back(JsonValue::makeObject(o));
  }
  return arr;
}

// ========== L5 里程碑检查 ==========

void TreasuryService::checkMasterySeals(int uid)
{
  const JsonValue* seals = rewardsCfg_.find("seals");
  if (!seals || !seals->isArray()) return;

  // 查询当前 L5 掌握数
  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM user_learning "
    "WHERE user_id=" + std::to_string(uid) + " AND proficiency_level>=5");
  int masteryCount = rows.empty() ? 0 : std::stoi(rows[0]["c"]);

  for (auto& seal : seals->asArray())
  {
    std::string code = seal.getString("code");
    if (code.find("mastery_") != 0) continue; // 只处理掌握成就

    int threshold = (int)seal.getInt("threshold", 0);
    if (threshold <= 0) continue;

    // 已获得则跳过
    auto existing = db_.query(
      "SELECT 1 FROM user_seal WHERE user_id=" + std::to_string(uid)
      + " AND seal_code='" + Db::escape(code) + "' LIMIT 1");
    if (!existing.empty()) continue;

    if (masteryCount >= threshold)
    {
      // 颁发印章
      db_.exec(
        "INSERT INTO user_seal (user_id, seal_code, earned_at) "
        "VALUES (" + std::to_string(uid) + ",'" + Db::escape(code) + "',NOW())");
      std::cerr << "[treasury] awarded seal: " << code << " to user " << uid << std::endl;
    }
  }
}

// ========== 称谓升级检查 ==========

void TreasuryService::checkTitleUpgrade(int uid)
{
  const JsonValue* titles = rewardsCfg_.find("titles");
  if (!titles || !titles->isArray()) return;

  // 查询当前固化次数
  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM knowledge_points "
    "WHERE user_id=" + std::to_string(uid) + " AND is_solidified=1");
  int solidifyCount = rows.empty() ? 0 : std::stoi(rows[0]["c"]);

  // 当前称谓
  auto titleRows = db_.query(
    "SELECT title_code FROM user_title WHERE user_id=" + std::to_string(uid) + " LIMIT 1");
  std::string currentTitle = titleRows.empty() ? "" : titleRows[0]["title_code"];

  // 找下一个可升级称谓
  std::string nextTitle;
  for (auto& t : titles->asArray())
  {
    std::string tc = t.getString("code");
    int th = (int)t.getInt("threshold", 0);
    if (th <= 0) continue;
    if (solidifyCount >= th && tc > currentTitle)
    {
      nextTitle = tc;
    }
  }

  if (!nextTitle.empty())
  {
    // 升级称谓
    db_.exec(
      "INSERT INTO user_title (user_id, title_code, upgrade_at) "
      "VALUES (" + std::to_string(uid) + ",'" + Db::escape(nextTitle) + "',NOW())"
      " ON DUPLICATE KEY UPDATE upgrade_at=NOW()");

    // +100 金桂（幂等由 apply 保证）
    apply(100, "title_up", 0);

    std::cerr << "[treasury] title upgraded to " << nextTitle << " for user " << uid << std::endl;
  }
}

// ========== 课规里程碑检查 ==========

// 里程碑口径：整周 3 项全部达成的周数
void TreasuryService::checkGoalStreakSeals(int uid)
{
  const JsonValue* seals = rewardsCfg_.find("seals");
  if (!seals || !seals->isArray()) return;

  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM ("
    " SELECT week_key FROM user_weekly_goal WHERE user_id=" + std::to_string(uid)
    + " AND goal_id>0 GROUP BY week_key"
    " HAVING SUM(achieved)=COUNT(*) AND COUNT(*)>=3) t");
  int achievedWeeks = rows.empty() ? 0 : std::stoi(rows[0]["c"]);

  for (auto& seal : seals->asArray())
  {
    std::string code = seal.getString("code");
    if (code.find("streak_") != 0) continue;

    int threshold = (int)seal.getInt("threshold", 0);
    if (threshold <= 0) continue;

    // 已获得则跳过
    auto existing = db_.query(
      "SELECT 1 FROM user_seal WHERE user_id=" + std::to_string(uid)
      + " AND seal_code='" + Db::escape(code) + "' LIMIT 1");
    if (!existing.empty()) continue;

    if (achievedWeeks >= threshold)
    {
      db_.exec(
        "INSERT INTO user_seal (user_id, seal_code, earned_at) "
        "VALUES (" + std::to_string(uid) + ",'" + Db::escape(code) + "',NOW())");

      // +10 金桂里程碑加成
      apply(10, "streak_bonus", 0);

      std::cerr << "[treasury] awarded goal streak seal: " << code << " to user " << uid << std::endl;
    }
  }
}

void TreasuryService::awardGoalDoneSeals(int uid)
{
  auto sealCheck = db_.query(
    "SELECT 1 FROM user_seal WHERE user_id=" + std::to_string(uid)
    + " AND seal_code='goal_done' LIMIT 1");
  if (sealCheck.empty())
  {
    db_.exec(
      "INSERT INTO user_seal (user_id, seal_code, earned_at) "
      "VALUES (" + std::to_string(uid) + ",'goal_done',NOW())");
  }
  checkGoalStreakSeals(uid);
}

// ========== 公开接口 ==========

bool TreasuryService::apply(int delta, const std::string& code, int ref_id)
{
  if (delta == 0) return true;
  return applyInternal(delta, code, ref_id);
}

std::tuple<int, int64_t, int64_t> TreasuryService::getBalance(int uid)
{
  auto rows = db_.query(
    "SELECT balance, total_earned, total_spent FROM user_coin WHERE user_id="
    + std::to_string(uid) + " LIMIT 1");
  if (rows.empty()) return std::make_tuple(0, 0LL, 0LL);
  return std::make_tuple(
    std::stoi(rows[0]["balance"]),
    std::stoll(rows[0]["total_earned"]),
    std::stoll(rows[0]["total_spent"]));
}

int TreasuryService::getTodayEarned(int uid)
{
  auto rows = db_.query(
    "SELECT COALESCE(SUM(delta), 0) AS total FROM user_coin_txn "
    "WHERE user_id=" + std::to_string(uid)
    + " AND delta > 0 AND DATE(created_at)=CURDATE()");
  return rows.empty() ? 0 : std::stoi(rows[0]["total"]);
}

// v1.5.1-T002：今日某原因入账笔数（重练日上限）
int TreasuryService::countTodayByReason(int uid, const std::string& code)
{
  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM user_coin_txn "
    "WHERE user_id=" + std::to_string(uid)
    + " AND reason_code='" + Db::escape(code) + "'"
    " AND delta > 0 AND DATE(created_at)=CURDATE()");
  return rows.empty() ? 0 : std::stoi(rows[0]["c"]);
}

int TreasuryService::getRetryAmount() const
{
  const JsonValue* r = rewardsCfg_.find("retry");
  return (r && r->isObject()) ? (int)r->getInt("amount", 5) : 5;
}

int TreasuryService::getRetryMaxPerDay() const
{
  const JsonValue* r = rewardsCfg_.find("retry");
  return (r && r->isObject()) ? (int)r->getInt("max_per_day", 10) : 10;
}

JsonArray TreasuryService::getTxns(int uid, int limit)
{
  auto rows = db_.query(
    "SELECT delta, reason_code, ref_id, balance_after, DATE_FORMAT(created_at,'%Y-%m-%d %H:%i') AS created_at "
    "FROM user_coin_txn WHERE user_id=" + std::to_string(uid)
    + " ORDER BY id DESC LIMIT " + std::to_string(limit));

  JsonArray arr;
  for (auto& row : rows)
  {
    JsonObject o;
    o["delta"] = JsonValue::makeInt(std::stoi(row["delta"]));
    o["reason_code"] = JsonValue::makeString(row["reason_code"]);
    o["ref_id"] = JsonValue::makeInt(row["ref_id"].empty() ? 0 : std::stoi(row["ref_id"]));
    o["balance_after"] = JsonValue::makeInt(std::stoi(row["balance_after"]));
    o["created_at"] = JsonValue::makeString(row["created_at"]);
    arr.push_back(JsonValue::makeObject(o));
  }
  return arr;
}

JsonArray TreasuryService::getSeals(int uid)
{
  const JsonValue* seals = rewardsCfg_.find("seals");
  if (!seals || !seals->isArray()) return JsonArray{};

  // 查询已获得的印章
  auto earnedRows = db_.query(
    "SELECT seal_code, earned_at FROM user_seal WHERE user_id=" + std::to_string(uid));
  std::map<std::string, std::string> earnedMap;
  for (auto& row : earnedRows)
    earnedMap[row["seal_code"]] = row["earned_at"];

  JsonArray arr;
  for (auto& seal : seals->asArray())
  {
    std::string code = seal.getString("code");
    JsonObject o;
    o["code"] = JsonValue::makeString(code);
    o["label_cn"] = JsonValue::makeString(seal.getString("label_cn"));
    o["label_sub"] = JsonValue::makeString(seal.getString("label_sub"));
    o["earned"] = JsonValue::makeBool(earnedMap.count(code) > 0);
    if (earnedMap.count(code))
      o["earned_at"] = JsonValue::makeString(earnedMap[code]);
    arr.push_back(JsonValue::makeObject(o));
  }
  return arr;
}

std::string TreasuryService::getCurrentTitle(int uid)
{
  auto rows = db_.query(
    "SELECT title_code FROM user_title WHERE user_id=" + std::to_string(uid) + " LIMIT 1");
  return rows.empty() ? "" : rows[0]["title_code"];
}

int TreasuryService::getSolidifyCount(int uid)
{
  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM knowledge_points WHERE user_id="
    + std::to_string(uid) + " AND is_solidified=1");
  return rows.empty() ? 0 : std::stoi(rows[0]["c"]);
}

int TreasuryService::getMasteryCount(int uid)
{
  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM user_learning WHERE user_id="
    + std::to_string(uid) + " AND proficiency_level>=5");
  return rows.empty() ? 0 : std::stoi(rows[0]["c"]);
}

JsonArray TreasuryService::getMetrics()
{
  JsonArray arr;
  const JsonValue* ms = metricsCfg_.find("metrics");
  if (!ms || !ms->isArray()) return arr;
  for (auto& m : ms->asArray())
  {
    JsonObject o;
    o["metric_key"] = JsonValue::makeString(m.getString("metric_key"));
    o["name"] = JsonValue::makeString(m.getString("name"));
    o["unit"] = JsonValue::makeString(m.getString("unit"));
    o["default_target"] = JsonValue::makeInt(m.getInt("default_target", 1));
    o["abs_min"] = JsonValue::makeInt(m.getInt("abs_min", 1));
    o["abs_max"] = JsonValue::makeInt(m.getInt("abs_max", 100000));
    arr.push_back(JsonValue::makeObject(o));
  }
  return arr;
}

// 读取时幂等生成各计划任务 + 汇总本周参与计划
JsonArray TreasuryService::getWeeklyPlans(int uid)
{
  std::string wk = weekMonday();
  // D25：所有已生效课规都生成本周任务并记录进度；正在执行优先登记（第 1 位）
  auto gs = db_.query(
    "SELECT id FROM user_goals WHERE user_id=" + std::to_string(uid)
    + " AND status='active' AND (activate_week IS NULL OR activate_week<='" + Db::escape(wk) + "')"
    " ORDER BY is_current DESC, id ASC");
  for (auto& g : gs) ensureWeeklyTasks(uid, std::stoi(g["id"]));
  normalizePlanRanks(uid, wk);

  // D28：新词只同步到归属计划，避免多计划重复记录
  int nwPlan = resolveMetricPlan(uid, wk, "new_words");
  if (nwPlan > 0) syncNewWordProgress(uid, wk, nwPlan);

  for (auto& g : gs)
  {
    int gid = std::stoi(g["id"]);
    // 若三项均已达成则自动结算（幂等）
    auto done = db_.query(
      "SELECT COUNT(*) AS c, SUM(achieved) AS a FROM user_weekly_goal WHERE user_id="
      + std::to_string(uid) + " AND week_key='" + Db::escape(wk) + "' AND goal_id="
      + std::to_string(gid));
    if (!done.empty() && std::stoi(done[0]["c"]) >= 3
        && std::stoi(done[0]["a"].empty() ? "0" : done[0]["a"]) == std::stoi(done[0]["c"]))
    {
      claimWeeklyRewards(uid, "complete", gid);
    }
  }

  auto plans = db_.query(
    "SELECT g.goal_id AS goal_id, MIN(g.plan_rank) AS plan_rank,"
    " DATE_FORMAT(MIN(g.joined_at),'%Y-%m-%d %H:%i') AS joined_at,"
    " MAX(ug.name) AS name, MAX(ug.is_current) AS is_current,"
    " DATE_FORMAT(MAX(ug.deadline),'%Y-%m-%d') AS deadline"
    " FROM user_weekly_goal g JOIN user_goals ug ON ug.id=g.goal_id AND ug.user_id=g.user_id"
    " WHERE g.user_id=" + std::to_string(uid) + " AND g.week_key='" + Db::escape(wk) + "'"
    " AND g.goal_id>0 AND ug.status='active' GROUP BY g.goal_id ORDER BY plan_rank");

  double k = goalTaskCfgDouble(rewardsCfg_, "multi_plan_decay", 0.5);
  double challengeRatio = goalTaskCfgDouble(rewardsCfg_, "challenge_ratio", 1.5);
  int itemBase = goalTaskCfgInt(rewardsCfg_, "item", 5);
  int completeBase = goalTaskCfgInt(rewardsCfg_, "complete", 15);

  JsonArray arr;
  for (auto& p : plans)
  {
    int goalId = std::stoi(p["goal_id"]);
    int rank = std::stoi(p["plan_rank"]);
    auto tasks = db_.query(
      "SELECT metric_key, target, progress, achieved, effective_target, claimed"
      " FROM user_weekly_goal WHERE user_id=" + std::to_string(uid) + " AND week_key='"
      + Db::escape(wk) + "' AND goal_id=" + std::to_string(goalId)
      + " ORDER BY FIELD(metric_key,'review_items','new_words','quiz_count'), metric_key");

    JsonArray ta;
    int achievedCount = 0;
    int claimable = 0;
    for (auto& t : tasks)
    {
      std::string key = t["metric_key"];
      const JsonValue* mc = metricConfig(key);
      bool achieved = t["achieved"] == "1";
      bool claimed = t["claimed"] == "1";
      int baseTarget = std::stoi(t["target"]);
      int progress = std::stoi(t["progress"]);
      // D29：挑战目标 = ceil(基础目标 × 1.5)，只记录、不发额外金币
      int challengeTarget = (int)std::ceil(baseTarget * challengeRatio);
      if (challengeTarget <= baseTarget) challengeTarget = baseTarget + 1;
      if (achieved) achievedCount++;
      if (achieved && !claimed) claimable += decayAmount(itemBase, rank);
      JsonObject o;
      o["metric_key"] = JsonValue::makeString(key);
      o["metric_name"] = JsonValue::makeString(mc ? mc->getString("name", key) : key);
      o["unit"] = JsonValue::makeString(mc ? mc->getString("unit") : "");
      o["target"] = JsonValue::makeInt(baseTarget);
      o["effective_target"] = JsonValue::makeInt(std::stoi(t["effective_target"]));
      o["progress"] = JsonValue::makeInt(progress);
      o["achieved"] = JsonValue::makeBool(achieved);
      o["claimed"] = JsonValue::makeBool(claimed);
      o["challenge_target"] = JsonValue::makeInt(challengeTarget);
      o["challenge_reached"] = JsonValue::makeBool(progress >= challengeTarget);
      ta.push_back(JsonValue::makeObject(o));
    }

    bool allDone = (int)tasks.size() >= 3 && achievedCount == (int)tasks.size();
    if (allDone && !hasCompleteReward(uid, goalId, wk))
      claimable += decayAmount(completeBase, rank);

    JsonObject reward;
    reward["item"] = JsonValue::makeInt(decayAmount(itemBase, rank));
    reward["complete"] = JsonValue::makeInt(decayAmount(completeBase, rank));
    reward["earned"] = JsonValue::makeInt(goalWeekEarned(uid, wk, goalId));
    reward["claimable"] = JsonValue::makeInt(claimable);

    JsonObject o;
    o["goal_id"] = JsonValue::makeInt(goalId);
    o["plan_name"] = JsonValue::makeString(p["name"].empty() ? "课规" : p["name"]);
    o["week_key"] = JsonValue::makeString(wk);
    o["plan_rank"] = JsonValue::makeInt(rank);
    o["decay"] = JsonValue::makeDouble(std::pow(k, rank - 1));
    o["is_current"] = JsonValue::makeBool(p["is_current"] == "1");
    o["deadline"] = JsonValue::makeString(p["deadline"]);
    o["joined_at"] = JsonValue::makeString(p["joined_at"]);
    o["all_done"] = JsonValue::makeBool(allDone);
    o["tasks"] = JsonValue::makeArray(ta);
    o["reward"] = JsonValue::makeObject(reward);
    arr.push_back(JsonValue::makeObject(o));
  }
  return arr;
}

JsonValue TreasuryService::getWeekReward(int uid)
{
  std::string wk = weekMonday();
  int cap = goalTaskCfgInt(rewardsCfg_, "week_cap", 60);
  JsonArray plans = getWeeklyPlans(uid);
  int claimable = 0;
  for (auto& p : plans)
  {
    const JsonValue* r = p.find("reward");
    if (r && r->isObject()) claimable += (int)r->getInt("claimable", 0);
  }
  int earned = weekRewardEarned(uid, wk);
  JsonObject o;
  o["week_key"] = JsonValue::makeString(wk);
  o["cap"] = JsonValue::makeInt(cap);
  o["earned"] = JsonValue::makeInt(earned);
  o["claimable"] = JsonValue::makeInt(claimable);
  o["item_base"] = JsonValue::makeInt(goalTaskCfgInt(rewardsCfg_, "item", 5));
  o["complete_base"] = JsonValue::makeInt(goalTaskCfgInt(rewardsCfg_, "complete", 15));
  return JsonValue::makeObject(o);
}

void TreasuryService::switchCurrentGoal(int uid, int goalId)
{
  std::string u = std::to_string(uid);
  auto rows = db_.query(
    "SELECT id FROM user_goals WHERE id=" + std::to_string(goalId) + " AND user_id=" + u
    + " AND status='active' LIMIT 1");
  if (rows.empty()) return;
  db_.transaction([&]() -> bool
  {
    db_.exec("UPDATE user_goals SET is_current=0 WHERE user_id=" + u);
    db_.exec("UPDATE user_goals SET is_current=1 WHERE id=" + std::to_string(goalId)
             + " AND user_id=" + u);
    return true;
  });
  // 新计划从切换时刻起计本周任务（登记 plan_rank），并刷新本周排名
  ensureWeeklyTasks(uid, goalId);
  normalizePlanRanks(uid, weekMonday());
}

bool TreasuryService::setWeeklyTaskTarget(int uid, int goalId, const std::string& metricKey,
                                          int target, int& effectiveTarget, std::string& err)
{
  if (!isRegisteredMetric(metricKey)) { err = "未登记的指标不可计分"; return false; }
  if (target < 1) { err = "目标数量需大于 0"; return false; }
  std::string wk = weekMonday();
  int floor = computeMetricFloor(uid, metricKey);
  int eff = std::max(target, floor);

  auto rows = db_.query(
    "SELECT metric_key FROM user_weekly_goal WHERE user_id=" + std::to_string(uid)
    + " AND week_key='" + Db::escape(wk) + "' AND goal_id=" + std::to_string(goalId)
    + " AND metric_key='" + Db::escape(metricKey) + "' LIMIT 1");
  if (rows.empty())
  {
    // 该计划本周尚未包含此指标：补一行（不重置已有进度）
    auto g = db_.query(
      "SELECT id FROM user_goals WHERE id=" + std::to_string(goalId) + " AND user_id="
      + std::to_string(uid) + " LIMIT 1");
    if (g.empty()) { err = "课规不存在"; return false; }
    auto rankRows = db_.query(
      "SELECT COALESCE(MAX(plan_rank),0) AS r FROM user_weekly_goal WHERE user_id="
      + std::to_string(uid) + " AND week_key='" + Db::escape(wk) + "' AND goal_id="
      + std::to_string(goalId));
    int rank = rankRows.empty() ? 1 : std::max(1, std::stoi(rankRows[0]["r"]));
    auto ywRows = db_.query("SELECT YEARWEEK('" + Db::escape(wk) + "',3) AS yw");
    std::string yw = ywRows.empty() ? "0" : ywRows[0]["yw"];
    db_.exec(
      "INSERT IGNORE INTO user_weekly_goal (user_id, year_week, week_key, goal_id, metric_key,"
      " goal_type, target, progress, achieved, plan_rank, joined_at, effective_target, claimed)"
      " VALUES (" + std::to_string(uid) + "," + yw + ",'" + Db::escape(wk) + "',"
      + std::to_string(goalId) + ",'" + Db::escape(metricKey) + "','" + Db::escape(metricKey)
      + "'," + std::to_string(target) + ",0,0," + std::to_string(rank) + ",NOW(),"
      + std::to_string(eff) + ",0)");
  }
  else
  {
    db_.exec(
      "UPDATE user_weekly_goal SET target=" + std::to_string(target)
      + ",effective_target=" + std::to_string(eff)
      + " WHERE user_id=" + std::to_string(uid) + " AND week_key='" + Db::escape(wk)
      + "' AND goal_id=" + std::to_string(goalId) + " AND metric_key='" + Db::escape(metricKey) + "'");
  }
  effectiveTarget = eff;
  return true;
}

bool TreasuryService::saveNextWeekTargets(int uid, int goalId, const JsonArray& tasks, std::string& err)
{
  if (tasks.empty()) { err = "缺少下周任务"; return false; }
  auto g = db_.query(
    "SELECT id FROM user_goals WHERE id=" + std::to_string(goalId) + " AND user_id="
    + std::to_string(uid) + " AND status='active' LIMIT 1");
  if (g.empty()) { err = "课规不存在"; return false; }

  std::string wk = weekMonday(1);
  auto ywRows = db_.query("SELECT YEARWEEK('" + Db::escape(wk) + "',3) AS yw");
  std::string yw = ywRows.empty() ? "0" : ywRows[0]["yw"];

  for (auto& t : tasks)
  {
    std::string key = t.getString("metric_key");
    int target = (int)t.getInt("target", 0);
    if (!isRegisteredMetric(key)) { err = "未登记的指标不可计分"; return false; }
    if (target < 1) { err = "目标数量需大于 0"; return false; }
    int eff = std::max(target, computeMetricFloor(uid, key));
    db_.exec(
      "INSERT INTO user_weekly_goal (user_id, year_week, week_key, goal_id, metric_key, goal_type,"
      " target, progress, achieved, plan_rank, joined_at, effective_target, claimed)"
      " VALUES (" + std::to_string(uid) + "," + yw + ",'" + Db::escape(wk) + "',"
      + std::to_string(goalId) + ",'" + Db::escape(key) + "','" + Db::escape(key) + "',"
      + std::to_string(target) + ",0,0,1,NOW()," + std::to_string(eff) + ",0)"
      " ON DUPLICATE KEY UPDATE target=VALUES(target), effective_target=VALUES(effective_target)");
  }
  return true;
}

int TreasuryService::claimWeeklyRewards(int uid, const std::string& trigger, int goalId)
{
  std::string wk = weekMonday();
  int cap = goalTaskCfgInt(rewardsCfg_, "week_cap", 60);
  int itemBase = goalTaskCfgInt(rewardsCfg_, "item", 5);
  int completeBase = goalTaskCfgInt(rewardsCfg_, "complete", 15);
  auto metricIndex = [&](const std::string& key) -> int
  {
    const JsonValue* ms = metricsCfg_.find("metrics");
    if (ms && ms->isArray())
    {
      int i = 1;
      for (auto& m : ms->asArray())
      {
        if (m.getString("metric_key") == key) return i;
        i++;
      }
    }
    return 8;
  };

  std::string planFilter = goalId > 0 ? (" AND g.goal_id=" + std::to_string(goalId)) : "";
  auto plans = db_.query(
    "SELECT g.goal_id AS goal_id, MIN(g.plan_rank) AS plan_rank FROM user_weekly_goal g"
    " JOIN user_goals ug ON ug.id=g.goal_id AND ug.user_id=g.user_id"
    " WHERE g.user_id=" + std::to_string(uid) + " AND g.week_key='" + Db::escape(wk)
    + "' AND g.goal_id>0 AND ug.status='active'"
    + planFilter + " GROUP BY g.goal_id");

  int credited = 0;
  for (auto& p : plans)
  {
    int gid = std::stoi(p["goal_id"]);
    int rank = std::stoi(p["plan_rank"]);

    // 每项 5：先置 claimed=1 再入账，并发/重复调用只入账一次
    auto tasks = db_.query(
      "SELECT metric_key FROM user_weekly_goal WHERE user_id=" + std::to_string(uid)
      + " AND week_key='" + Db::escape(wk) + "' AND goal_id=" + std::to_string(gid)
      + " AND achieved=1 AND claimed=0");
    for (auto& t : tasks)
    {
      std::string key = t["metric_key"];
      auto wr = db_.exec(
        "UPDATE user_weekly_goal SET claimed=1 WHERE user_id=" + std::to_string(uid)
        + " AND week_key='" + Db::escape(wk) + "' AND goal_id=" + std::to_string(gid)
        + " AND metric_key='" + Db::escape(key) + "' AND claimed=0");
      if (wr.affected_rows <= 0) continue;
      int amount = decayAmount(itemBase, rank);
      if (amount <= 0) continue;
      int used = weekRewardEarned(uid, wk);
      if (used >= cap) continue;
      if (used + amount > cap) amount = cap - used;
      if (apply(amount, "goal_task_item", gid * 10 + metricIndex(key))) credited += amount;
    }

    if (trigger != "complete") continue;

    // 三项全部完成 → 补发未领 + 15 + 印章（幂等）
    auto sum = db_.query(
      "SELECT COUNT(*) AS c, SUM(achieved) AS a FROM user_weekly_goal WHERE user_id="
      + std::to_string(uid) + " AND week_key='" + Db::escape(wk) + "' AND goal_id="
      + std::to_string(gid));
    if (sum.empty() || std::stoi(sum[0]["c"]) < 3
        || std::stoi(sum[0]["a"] != "" ? sum[0]["a"] : "0") != std::stoi(sum[0]["c"])) continue;

    if (!hasCompleteReward(uid, gid, wk))
    {
      int amount = decayAmount(completeBase, rank);
      int used = weekRewardEarned(uid, wk);
      if (amount > 0 && used < cap)
      {
        if (used + amount > cap) amount = cap - used;
        if (apply(amount, "goal_week_complete", gid * 10 + 9)) credited += amount;
      }
    }
    awardGoalDoneSeals(uid);
  }
  return credited;
}

// ========== 旧接口兼容 ==========

JsonValue TreasuryService::getWeeklyGoal(int uid)
{
  JsonArray plans = getWeeklyPlans(uid);
  if (!plans.empty())
  {
    JsonObject o;
    const JsonValue& p = plans[0];
    o["year_week"] = JsonValue::makeInt(currentYearWeek());
    o["week_key"] = JsonValue::makeString(p.getString("week_key"));
    o["goal_id"] = JsonValue::makeInt(p.getInt("goal_id", 0));
    const JsonValue* tasks = p.find("tasks");
    if (tasks && tasks->isArray() && !tasks->asArray().empty())
    {
      const JsonValue& t = tasks->asArray()[0];
      o["goal_type"] = JsonValue::makeString(t.getString("metric_key"));
      o["target"] = JsonValue::makeInt(t.getInt("target", 0));
      o["progress"] = JsonValue::makeInt(t.getInt("progress", 0));
      o["achieved"] = JsonValue::makeBool(p.getBool("all_done", false));
    }
    return JsonValue::makeObject(o);
  }
  // 无课规：返回占位（不落库），保持旧响应结构
  JsonObject o;
  o["year_week"] = JsonValue::makeInt(currentYearWeek());
  o["week_key"] = JsonValue::makeString(weekMonday());
  o["goal_id"] = JsonValue::makeInt(0);
  o["goal_type"] = JsonValue::makeString("review_items");
  o["target"] = JsonValue::makeInt(30);
  o["progress"] = JsonValue::makeInt(0);
  o["achieved"] = JsonValue::makeBool(false);
  return JsonValue::makeObject(o);
}

bool TreasuryService::setWeeklyGoal(int uid, const std::string& goal_type, int target)
{
  if (!isValidGoalType(goal_type) || target < 1) return false;

  auto cur = db_.query(
    "SELECT id FROM user_goals WHERE user_id=" + std::to_string(uid)
    + " AND is_current=1 AND status='active' LIMIT 1");
  int gid = cur.empty() ? 0 : std::stoi(cur[0]["id"]);
  if (gid == 0)
  {
    // 兼容旧接口：无课规时建立一个默认课规（后续可改/删）
    std::string wk = weekMonday();
    auto wr = db_.exec(
      "INSERT INTO user_goals (user_id, name, goal_type, target_desc, deadline, daily_minutes,"
      " daily_new_words, is_current, parse_source, status, start_date, activate_week, end_date)"
      " VALUES (" + std::to_string(uid) + ",'每周课规','" + Db::escape(goal_type)
      + "','每周课规',DATE_ADD(CURDATE(), INTERVAL 7 DAY),20,10,1,'template','active',"
      "CURDATE(),'" + Db::escape(wk) + "',DATE_ADD(CURDATE(), INTERVAL 7 DAY))");
    gid = (int)wr.last_insert_id;
    if (gid <= 0) return false;
  }
  ensureWeeklyTasks(uid, gid);
  int eff = 0;
  std::string err;
  return setWeeklyTaskTarget(uid, gid, goal_type, target, eff, err);
}

bool TreasuryService::isValidGoalType(const std::string& goal_type) const
{
  return isRegisteredMetric(goal_type);
}

void TreasuryService::advanceWeeklyProgress(int uid, const std::string& goal_type, int amount)
{
  if (!isRegisteredMetric(goal_type) || amount <= 0) return;
  std::string wk = weekMonday();
  // D28：一个真实学习事件只计入一个计划，不重复计数
  int gid = resolveMetricPlan(uid, wk, goal_type);
  if (gid <= 0) return; // 无生效课规：不计进度

  auto rows = db_.query(
    "SELECT progress, effective_target, achieved FROM user_weekly_goal WHERE user_id="
    + std::to_string(uid) + " AND week_key='" + Db::escape(wk) + "' AND goal_id="
    + std::to_string(gid) + " AND metric_key='" + Db::escape(goal_type) + "' LIMIT 1");
  if (rows.empty()) return;
  int progress = std::stoi(rows[0]["progress"]) + amount;
  int eff = std::stoi(rows[0]["effective_target"]);
  bool wasAchieved = rows[0]["achieved"] == "1";
  bool achieved = wasAchieved || progress >= eff;

  db_.exec(
    "UPDATE user_weekly_goal SET progress=" + std::to_string(progress)
    + ",achieved=" + std::to_string(achieved ? 1 : 0)
    + " WHERE user_id=" + std::to_string(uid) + " AND week_key='" + Db::escape(wk)
    + "' AND goal_id=" + std::to_string(gid) + " AND metric_key='" + Db::escape(goal_type) + "'");
  if (wasAchieved || !achieved) return;

  std::cerr << "[treasury] task achieved: " << goal_type << " plan=" << gid
            << " user=" << uid << std::endl;
  // 三项全部完成 → 自动补发未领 + 15 + 印章（无需打开面板）
  auto sum = db_.query(
    "SELECT COUNT(*) AS c, SUM(achieved) AS a FROM user_weekly_goal WHERE user_id="
    + std::to_string(uid) + " AND week_key='" + Db::escape(wk) + "' AND goal_id="
    + std::to_string(gid));
  if (!sum.empty() && std::stoi(sum[0]["c"]) >= 3
      && std::stoi(sum[0]["a"].empty() ? "0" : sum[0]["a"]) == std::stoi(sum[0]["c"]))
  {
    claimWeeklyRewards(uid, "complete", gid);
  }
}

void TreasuryService::checkAndAwardMasterySeals(int uid)
{
  checkMasterySeals(uid);
}

void TreasuryService::checkAndAwardTitle(int uid)
{
  checkTitleUpgrade(uid);
}

void TreasuryService::checkAndAwardGoalStreakSeals(int uid)
{
  checkGoalStreakSeals(uid);
}

} // namespace eng
