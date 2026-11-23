-- =====================================================================
-- v1.5.1-T005 HOME-NAV：AI 导航会话（仅登录用户写入）
-- 兼容变更（MINOR）：新增 ai_nav_messages 一表
-- 执行前置：先备份数据库（backups/ 目录）
-- 本脚本可重复执行：CREATE TABLE IF NOT EXISTS
-- =====================================================================

SET NAMES utf8mb4;

-- ========== AI 导航消息：单会话按用户保留 7 天，读取时惰性清理 ==========
CREATE TABLE IF NOT EXISTS `ai_nav_messages` (
  `id`               BIGINT NOT NULL AUTO_INCREMENT,
  `user_id`          INT NOT NULL,
  `role`             VARCHAR(16) NOT NULL COMMENT 'user/assistant',
  `content`          TEXT NOT NULL COMMENT '消息正文',
  `action_json`      VARCHAR(500) NOT NULL DEFAULT '' COMMENT '动作锚点 {id,label,route}',
  `proposal_json`    TEXT COMMENT '课规提案字段（仅 assistant 提案消息）',
  `proposal_status`  VARCHAR(16) NOT NULL DEFAULT 'none' COMMENT 'none/proposed/confirmed/discarded/expired',
  `created_at`       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_user_time` (`user_id`, `created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AI 导航会话消息';
