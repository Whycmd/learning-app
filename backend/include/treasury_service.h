// 点金圣殿 服务层（v1.3.0-T005；v1.5.1-T001 扩充课规任务卡）
// 职责：金桂账户管理、朱砂印颁发、称谓升级、课规与本周任务卡
// 配置：config/rewards.json（金额/门槛参数）、config/metrics.json（可判定指标注册表）
#pragma once

#include "db.h"
#include "json.h"
#include <string>
#include <tuple>
#include <vector>

namespace eng {

class TreasuryService
{
  Db& db_;
  std::string rewardsJson_;      // 原始 JSON 字符串
  JsonValue rewardsCfg_;         // 解析后的配置
  std::string configPath_;       // 配置文件路径（相对于项目根目录）
  std::string metricsPath_;      // 指标注册表路径
  JsonValue metricsCfg_;         // 指标注册表

  // 内部：加载配置
  bool loadConfig();
  bool loadMetrics();

  // 内部：获取收入金额
  int getIncomeAmount(const std::string& code) const;

  // 内部：检查今日是否已入账（quiz_done/quiz_perfect/review_done 同日仅首笔）
  bool hasEarnedToday(const std::string& code, int uid);

  // 内部：计算 token 扣费
  int calcTokenCost(int promptTokens, int completionTokens) const;

  // 内部：写入流水 + 更新余额（事务内幂等）
  bool applyInternal(int delta, const std::string& code, int ref_id);

  // 内部：获取/创建用户账户
  bool ensureAccount(int uid);

  // 内部：L5 词条里程碑检查
  void checkMasterySeals(int uid);

  // 内部：固化次数称谓检查
  void checkTitleUpgrade(int uid);

  // 内部：课规里程碑检查（整周三项全部达成）
  void checkGoalStreakSeals(int uid);

  // 内部：获取当前自然周的 year_week
  static int currentYearWeek();

  // ===== v1.5.1-T001：任务卡与门槛 =====

  // 本周周一（offsetWeeks=0 本周，1 下周）
  std::string weekMonday(int offsetWeeks = 0);

  // 指标配置
  const JsonValue* metricConfig(const std::string& metricKey) const;
  bool isRegisteredMetric(const std::string& metricKey) const;

  // 计划衰减后的金额（rank 从 1 开始；低于 1 金桂不再发放）
  int decayAmount(int base, int planRank) const;

  // 本周某计划已入账的奖励合计（用于封顶）
  int goalWeekEarned(int uid, const std::string& weekKey, int goalId) const;
  int weekRewardEarned(int uid, const std::string& weekKey) const;

  // 某计划本周是否已发全完成奖励
  bool hasCompleteReward(int uid, int goalId, const std::string& weekKey);

  // 新词指标按学情当周新建数只读同步（仅同步到归属计划）
  void syncNewWordProgress(int uid, const std::string& weekKey, int goalId);

  // D28：一个学习事件只归属一个计划（正在执行优先，否则本周参与最早）
  int resolveMetricPlan(int uid, const std::string& weekKey, const std::string& metricKey);

  // 本周计划排名归一：正在执行为第 1 位，其余按进入本周的先后排
  void normalizePlanRanks(int uid, const std::string& weekKey);

  // 颁发「课规有成」与周里程碑
  void awardGoalDoneSeals(int uid);

public:
  TreasuryService(Db& db, const std::string& configPath = "config/rewards.json",
                  const std::string& metricsPath = "config/metrics.json")
    : db_(db), configPath_(configPath), metricsPath_(metricsPath)
  {
    loadConfig();
    loadMetrics();
  }

  // ========== 核心入账 ==========

  // 统一入账入口：delta>0 入账，delta<0 扣费；幂等保证不重入
  // 返回 true=成功，false=失败（余额不足等）
  bool apply(int delta, const std::string& code, int ref_id = 0);

  // ========== 查询 ==========

  // 返回 (balance, total_earned, total_spent)
  std::tuple<int, int64_t, int64_t> getBalance(int uid);

  // 今日累计入账金桂（不含扣费）
  int getTodayEarned(int uid);

  // v1.5.1-T002：今日某原因入账笔数（用于重练日上限）
  int countTodayByReason(int uid, const std::string& code);

  // v1.5.1-T002：重练奖励金额与日上限（config/rewards.json retry 段）
  int getRetryAmount() const;
  int getRetryMaxPerDay() const;

  // 最近 N 条流水
  JsonArray getTxns(int uid, int limit = 10);

  // 7 枚印章状态（含未解锁的占位）
  JsonArray getSeals(int uid);

  // 当前称谓 code（如 "scholar_2"）
  std::string getCurrentTitle(int uid);

  // 固化总次数（供首页展示）
  int getSolidifyCount(int uid);

  // L5 已掌握词条数
  int getMasteryCount(int uid);

  // ========== 本周课规（兼容旧接口） ==========

  // 获取/创建本周课规（兼容字段，取自任务卡首个指标）
  JsonValue getWeeklyGoal(int uid);

  // 修改本周课规（兼容旧接口；等价于改当前计划单指标目标）
  bool setWeeklyGoal(int uid, const std::string& goal_type, int target);

  // 验证 goal_type 是否合法（已注册指标）
  bool isValidGoalType(const std::string& goal_type) const;

  // 累加课规进度（由外部真实学习事件触发；只计入归属计划，不重复计数）
  void advanceWeeklyProgress(int uid, const std::string& goal_type, int amount);

  // ========== v1.5.1-T001：任务卡 ==========

  // 本周参与过的计划及任务（读取时幂等生成当前计划任务）
  JsonArray getWeeklyPlans(int uid);

  // 本周奖励汇总（含封顶）
  JsonValue getWeekReward(int uid);

  // 幂等生成某计划本周任务
  void ensureWeeklyTasks(int uid, int goalId);

  // 修改单条任务目标值；返回门槛判定值，失败置 err
  bool setWeeklyTaskTarget(int uid, int goalId, const std::string& metricKey, int target,
                           int& effectiveTarget, std::string& err);

  // 保存下周任务目标（下周一自动生效，本周不受影响）
  bool saveNextWeekTargets(int uid, int goalId, const JsonArray& tasks, std::string& err);

  // D23 动态门槛
  int computeMetricFloor(int uid, const std::string& metricKey);

  // D29 自动推荐目标（近 4 周中位数，不足 2 周用默认值）
  int computeSuggestion(int uid, const std::string& metricKey);
  JsonArray getSuggestions(int uid);

  // 领取奖励：trigger=panel 领未领每项；trigger=complete 补发全部并加全完成奖励
  int claimWeeklyRewards(int uid, const std::string& trigger, int goalId = 0);

  // 切换正在执行的课规（登记本周参与顺序）
  void switchCurrentGoal(int uid, int goalId);

  // 已注册指标清单
  JsonArray getMetrics();

  // ========== 批量检查（定时或事件触发） ==========

  // 检查并颁发 L5 里程碑印章
  void checkAndAwardMasterySeals(int uid);

  // 检查并升级称谓（固化次数达标 +100 金桂）
  void checkAndAwardTitle(int uid);

  // 检查并颁发课规里程碑印章（3/10/50 周）
  void checkAndAwardGoalStreakSeals(int uid);
};

} // namespace eng
