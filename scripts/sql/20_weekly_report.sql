-- =====================================================================
-- v1.5.1-T003 WEEKLY-REPORT：学情周报（仅缓存 AI 文本）
-- 兼容变更（MINOR）：新增 weekly_report_ai 一表
-- 执行前置：先备份数据库（backups/ 目录）
-- 本脚本可重复执行：CREATE TABLE IF NOT EXISTS
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 周报 AI 文本缓存：按用户 + 自然周唯一 ==========
CREATE TABLE IF NOT EXISTS `weekly_report_ai` (
  `user_id`    INT NOT NULL,
  `week_start` DATE NOT NULL COMMENT '自然周周一',
  `status`     VARCHAR(16) NOT NULL DEFAULT 'none' COMMENT 'none/queued/running/done/failed',
  `task_id`    BIGINT NOT NULL DEFAULT 0 COMMENT 'ai_tasks.id',
  `ai_text`    TEXT COMMENT 'AI 生成的下周建议',
  `error_msg`  VARCHAR(255) NOT NULL DEFAULT '' COMMENT '失败原因',
  `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`user_id`, `week_start`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='学情周报 AI 文本缓存';
