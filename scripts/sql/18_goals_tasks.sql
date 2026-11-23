-- =====================================================================
-- v1.5.1-T001 GOAL-TASKS：课规 + 本周课规任务卡
-- 兼容变更（MINOR）：新增 user_goals；扩展 user_weekly_goal
-- 执行前置：先备份数据库（backups/ 目录）
-- 本脚本可重复执行：所有结构判断均查 information_schema
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 1) 课规表（数量不设上限，同一时间仅 1 个 is_current=1） ==========
CREATE TABLE IF NOT EXISTS `user_goals` (
  `id`                 INT NOT NULL AUTO_INCREMENT,
  `user_id`            INT NOT NULL,
  `name`               VARCHAR(64) NOT NULL DEFAULT '' COMMENT '课规名称',
  `raw_text`           TEXT NULL COMMENT '自由文本原文',
  `goal_type`          VARCHAR(32) NOT NULL DEFAULT '' COMMENT '首选指标 metric_key',
  `target_desc`        VARCHAR(255) NOT NULL DEFAULT '' COMMENT '目标描述',
  `deadline`           DATE NOT NULL COMMENT '有效期至（当前周期）',
  `previous_deadline`  DATE NULL COMMENT '上一次到期日（用于展示已到期 N 天）',
  `auto_renewed_at`    DATETIME NULL COMMENT '最近一次自动延长有效期时间',
  `daily_minutes`      INT NOT NULL DEFAULT 30 COMMENT '每天可用分钟',
  `daily_new_words`    INT NOT NULL DEFAULT 10 COMMENT '每天学习新词个数',
  `wordlist_id`        INT NOT NULL DEFAULT 0 COMMENT '绑定词表 ID，0=未绑定',
  `is_current`         TINYINT NOT NULL DEFAULT 0 COMMENT '1=正在执行（每用户仅一行）',
  `parse_source`       VARCHAR(16) NOT NULL DEFAULT 'manual' COMMENT 'template/manual',
  `status`             VARCHAR(16) NOT NULL DEFAULT 'active' COMMENT 'active',
  `start_date`         DATE NOT NULL COMMENT '开始日期',
  `activate_week`      DATE NULL COMMENT '生效周的周一，可指向下周',
  `end_date`           DATE NULL COMMENT '截止日期（与 deadline 同步）',
  `created_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_user_status` (`user_id`, `status`),
  KEY `idx_user_current` (`user_id`, `is_current`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='用户课规';

-- ========== 2) 本周课规任务卡：主键改为 (user_id, week_key, goal_id, metric_key) ==========
DROP PROCEDURE IF EXISTS `__goal_tasks_migrate`;
DELIMITER $$
CREATE PROCEDURE `__goal_tasks_migrate`()
BEGIN
  -- 2.1 补列（逐列判断，兼容中断后重跑）
  IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='user_weekly_goal' AND COLUMN_NAME='week_key') THEN
    ALTER TABLE `user_weekly_goal`
      ADD COLUMN `week_key` DATE NULL COMMENT '自然周周一' AFTER `year_week`;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='user_weekly_goal' AND COLUMN_NAME='goal_id') THEN
    ALTER TABLE `user_weekly_goal`
      ADD COLUMN `goal_id` INT NOT NULL DEFAULT 0 COMMENT '所属课规 ID；0=旧数据' AFTER `week_key`;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='user_weekly_goal' AND COLUMN_NAME='metric_key') THEN
    ALTER TABLE `user_weekly_goal`
      ADD COLUMN `metric_key` VARCHAR(32) NOT NULL DEFAULT '' COMMENT '指标 metric_key' AFTER `goal_id`;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='user_weekly_goal' AND COLUMN_NAME='plan_rank') THEN
    ALTER TABLE `user_weekly_goal`
      ADD COLUMN `plan_rank` INT NOT NULL DEFAULT 1 COMMENT '本周参与顺序（用于奖励衰减）' AFTER `achieved`;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='user_weekly_goal' AND COLUMN_NAME='joined_at') THEN
    ALTER TABLE `user_weekly_goal`
      ADD COLUMN `joined_at` DATETIME NULL COMMENT '本周开始参与时间' AFTER `plan_rank`;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='user_weekly_goal' AND COLUMN_NAME='effective_target') THEN
    ALTER TABLE `user_weekly_goal`
      ADD COLUMN `effective_target` INT NOT NULL DEFAULT 0 COMMENT '门槛快照（判定值）' AFTER `joined_at`;
  END IF;
  IF NOT EXISTS (SELECT 1 FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='user_weekly_goal' AND COLUMN_NAME='claimed') THEN
    ALTER TABLE `user_weekly_goal`
      ADD COLUMN `claimed` TINYINT NOT NULL DEFAULT 0 COMMENT '每项 5 金桂是否已领' AFTER `effective_target`;
  END IF;

  -- 2.2 存量回填
  UPDATE `user_weekly_goal` SET `metric_key`=`goal_type` WHERE `metric_key`='';
  UPDATE `user_weekly_goal`
    SET `week_key`=STR_TO_DATE(CONCAT(`year_week` DIV 100,' ',`year_week` MOD 100,' Monday'),'%x %v %W')
    WHERE `week_key` IS NULL;
  UPDATE `user_weekly_goal` SET `effective_target`=`target` WHERE `effective_target`=0;
  UPDATE `user_weekly_goal` SET `joined_at`=`created_at` WHERE `joined_at` IS NULL;
  -- 旧数据视为历史已发奖励
  UPDATE `user_weekly_goal` SET `claimed`=1 WHERE `achieved`=1 AND `claimed`=0;

  -- 2.3 week_key 收紧为 NOT NULL
  ALTER TABLE `user_weekly_goal`
    MODIFY COLUMN `week_key` DATE NOT NULL COMMENT '自然周周一';

  -- 2.4 主键迁移（仅当主键尚未包含 week_key）
  IF NOT EXISTS (SELECT 1 FROM information_schema.STATISTICS
                 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='user_weekly_goal'
                   AND INDEX_NAME='PRIMARY' AND COLUMN_NAME='week_key') THEN
    ALTER TABLE `user_weekly_goal`
      DROP PRIMARY KEY,
      ADD PRIMARY KEY (`user_id`, `week_key`, `goal_id`, `metric_key`);
  END IF;
END$$
DELIMITER ;
CALL `__goal_tasks_migrate`();
DROP PROCEDURE IF EXISTS `__goal_tasks_migrate`;
