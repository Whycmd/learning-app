-- =====================================================================
-- v1.5.0-T004：界面模式（功能模式 / 计划模式）
-- 执行前置：先备份数据库（backups/ 目录）
--
-- 迁移顺序：01 → 02 → 03 → 04 → 05 → ... → 12 → 13（本文件）
-- 本次为兼容变更（users 加列），存量用户默认进入功能模式，无需回填。
-- =====================================================================

-- users 表加列（MySQL 8.0 不支持 ADD COLUMN IF NOT EXISTS）
-- 用存储过程判断列是否存在后再加列，保证脚本可重复执行
DROP PROCEDURE IF EXISTS `__add_ui_mode_col`;
DELIMITER $$
CREATE PROCEDURE `__add_ui_mode_col`()
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'users'
      AND COLUMN_NAME = 'ui_mode'
  ) THEN
    ALTER TABLE `users`
      ADD COLUMN `ui_mode` VARCHAR(16) NOT NULL DEFAULT 'function'
      COMMENT '界面模式：function=功能模式，plan=计划模式'
      AFTER `learner_identity`;
  END IF;
END$$
DELIMITER ;
CALL `__add_ui_mode_col`();
DROP PROCEDURE IF EXISTS `__add_ui_mode_col`;
