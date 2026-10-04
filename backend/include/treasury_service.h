// 点金圣殿 服务层（v1.3.0-T005）
// 职责：金桂账户管理、朱砂印颁发、称谓升级、本周课规
// 配置：读取 config/rewards.json，修改 JSON 无需改代码
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

  // 内部：加载配置
  bool loadConfig();

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

  // 内部：课规里程碑检查
  void checkGoalStreakSeals(int uid);

  // 内部：获取当前自然周的 year_week
  static int currentYearWeek();

public:
  TreasuryService(Db& db, const std::string& configPath = "config/rewards.json")
    : db_(db), configPath_(configPath)
  {
    loadConfig();
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

  // ========== 本周课规 ==========

  // 获取/创建本周课规
  JsonValue getWeeklyGoal(int uid);

  // 修改本周课规（周内随时可改）
  // 返回 false 表示 goal_type 非法
  bool setWeeklyGoal(int uid, const std::string& goal_type, int target);

  // 验证 goal_type 是否合法
  bool isValidGoalType(const std::string& goal_type) const;

  // 累加课规进度（由外部事件触发）
  void advanceWeeklyProgress(int uid, const std::string& goal_type, int amount);

  // ========== 批量检查（定时或事件触发） ==========

  // 检查并颁发 L5 里程碑印章
  void checkAndAwardMasterySeals(int uid);

  // 检查并升级称谓（固化次数达标 +100 金桂）
  void checkAndAwardTitle(int uid);

  // 检查并颁发课规里程碑印章（3/10/50 周）
  void checkAndAwardGoalStreakSeals(int uid);
};

} // namespace eng
