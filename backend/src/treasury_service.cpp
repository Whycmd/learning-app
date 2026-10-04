// 点金圣殿 服务层实现（v1.3.0-T005）
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

int TreasuryService::getIncomeAmount(const std::string& code) const
{
  const JsonValue* inc = rewardsCfg_.find("income");
  if (!inc || !inc->isObject()) return 0;
  const JsonValue* item = inc->find(code);
  if (!item || !item->isObject()) return 0;
  return (int)item->getInt("amount", 0);
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
  int month = t->tm_mon + 1;
  int day = t->tm_mday;
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
  int nextThreshold = 0;
  for (auto& t : titles->asArray())
  {
    std::string tc = t.getString("code");
    int th = (int)t.getInt("threshold", 0);
    if (th <= 0) continue;
    if (solidifyCount >= th && tc > currentTitle)
    {
      nextTitle = tc;
      nextThreshold = th;
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

void TreasuryService::checkGoalStreakSeals(int uid)
{
  const JsonValue* seals = rewardsCfg_.find("seals");
  if (!seals || !seals->isArray()) return;

  // 统计已达成周数
  auto rows = db_.query(
    "SELECT COUNT(*) AS c FROM user_weekly_goal "
    "WHERE user_id=" + std::to_string(uid) + " AND achieved=1");
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

JsonValue TreasuryService::getWeeklyGoal(int uid)
{
  int yw = currentYearWeek();

  // 查找或创建本周课规
  auto rows = db_.query(
    "SELECT goal_type, target, progress, achieved FROM user_weekly_goal "
    "WHERE user_id=" + std::to_string(uid) + " AND year_week=" + std::to_string(yw)
    + " LIMIT 1");

  if (rows.empty())
  {
    // 首次默认「复习 30 条」
    int target = 30;
    db_.exec(
      "INSERT INTO user_weekly_goal (user_id, year_week, goal_type, target, progress) "
      "VALUES (" + std::to_string(uid) + "," + std::to_string(yw)
      + ",'review_items'," + std::to_string(target) + ",0)");
    JsonObject o;
    o["year_week"] = JsonValue::makeInt(yw);
    o["goal_type"] = JsonValue::makeString("review_items");
    o["target"] = JsonValue::makeInt(target);
    o["progress"] = JsonValue::makeInt(0);
    o["achieved"] = JsonValue::makeBool(false);
    return JsonValue::makeObject(o);
  }

  JsonObject o;
  o["year_week"] = JsonValue::makeInt(yw);
  o["goal_type"] = JsonValue::makeString(rows[0]["goal_type"]);
  o["target"] = JsonValue::makeInt(std::stoi(rows[0]["target"]));
  o["progress"] = JsonValue::makeInt(std::stoi(rows[0]["progress"]));
  o["achieved"] = JsonValue::makeBool(rows[0]["achieved"] == "1");
  return JsonValue::makeObject(o);
}

bool TreasuryService::setWeeklyGoal(int uid, const std::string& goal_type, int target)
{
  if (!isValidGoalType(goal_type)) return false;
  int yw = currentYearWeek();
  // 更新现有记录或插入
  db_.exec(
    "INSERT INTO user_weekly_goal (user_id, year_week, goal_type, target, progress, achieved) "
    "VALUES (" + std::to_string(uid) + "," + std::to_string(yw)
    + ",'" + Db::escape(goal_type) + "'," + std::to_string(target) + ",0,0)"
    " ON DUPLICATE KEY UPDATE goal_type='" + Db::escape(goal_type)
    + "',target=" + std::to_string(target) + ",progress=0,achieved=0");
  return true;
}

bool TreasuryService::isValidGoalType(const std::string& goal_type) const
{
  const JsonValue* wg = rewardsCfg_.find("weekly_goal");
  if (!wg || !wg->isObject()) return false;
  const JsonValue* types = wg->find("goal_types");
  if (!types || !types->isArray()) return false;
  for (auto& t : types->asArray())
  {
    if (t.isString() && t.asString() == goal_type) return true;
  }
  return false;
}

void TreasuryService::advanceWeeklyProgress(int uid, const std::string& goal_type, int amount)
{
  int yw = currentYearWeek();
  auto rows = db_.query(
    "SELECT progress, target, achieved FROM user_weekly_goal "
    "WHERE user_id=" + std::to_string(uid) + " AND year_week=" + std::to_string(yw)
    + " AND goal_type='" + Db::escape(goal_type) + "' LIMIT 1");

  if (rows.empty()) return;

  int progress = std::stoi(rows[0]["progress"]) + amount;
  int target = std::stoi(rows[0]["target"]);
  bool achieved = rows[0]["achieved"] == "1";

  if (!achieved && progress >= target)
  {
    // 达成！+30 金桂
    db_.exec(
      "UPDATE user_weekly_goal SET progress=" + std::to_string(progress)
      + ",achieved=1 WHERE user_id=" + std::to_string(uid) + " AND year_week="
      + std::to_string(yw) + " AND goal_type='" + Db::escape(goal_type) + "'");

    apply(30, "goal_done", 0);

    // 颁发首次达成印章（若未拥有）
    auto sealCheck = db_.query(
      "SELECT 1 FROM user_seal WHERE user_id=" + std::to_string(uid)
      + " AND seal_code='goal_done' LIMIT 1");
    if (sealCheck.empty())
    {
      db_.exec(
        "INSERT INTO user_seal (user_id, seal_code, earned_at) "
        "VALUES (" + std::to_string(uid) + ",'goal_done',NOW())");
    }

    // 检查里程碑
    checkGoalStreakSeals(uid);

    std::cerr << "[treasury] weekly goal achieved by user " << uid << std::endl;
  }
  else
  {
    db_.exec(
      "UPDATE user_weekly_goal SET progress=" + std::to_string(progress)
      + " WHERE user_id=" + std::to_string(uid) + " AND year_week="
      + std::to_string(yw) + " AND goal_type='" + Db::escape(goal_type) + "'");
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
