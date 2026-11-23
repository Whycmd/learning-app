-- =====================================================================
-- v1.5.2-T001 DEV-INTAKE：开发者看板 + AI 导航需求/Bug 自动沉淀
-- 兼容变更（MINOR）：新增 dev_backlog、dev_bugs 两表；users 增列 role
-- 执行前置：先备份数据库（backups/ 目录）
-- 本脚本可重复执行：建表 IF NOT EXISTS；加列用存储过程判断（同 13 号风格）
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 需求台账：AI 导航沉淀的开发需求（编号即 #B{id}） ==========
CREATE TABLE IF NOT EXISTS `dev_backlog` (
  `id`                INT NOT NULL AUTO_INCREMENT,
  `source_message_id` BIGINT DEFAULT NULL COMMENT '触发消息的 ai_nav_messages 用户消息 id；游客 NULL',
  `user_id`           INT NOT NULL DEFAULT 0 COMMENT '0=游客',
  `title`             VARCHAR(200) NOT NULL COMMENT 'AI 短标题（原文）',
  `detail`            TEXT COMMENT '触发消息片段（≤500 字）',
  `category`          VARCHAR(12) NOT NULL DEFAULT 'enhancement' COMMENT 'feature/enhancement',
  `scope_fit`         VARCHAR(12) NOT NULL DEFAULT 'in_scope' COMMENT 'in_scope/out_of_scope',
  `confidence`        DECIMAL(3,2) NOT NULL DEFAULT 0 COMMENT '判定置信度 0~1',
  `dedupe_key`        CHAR(40) NOT NULL COMMENT 'sha1(kind|归一化标题)，精确去重',
  `occurrences`       INT NOT NULL DEFAULT 1 COMMENT '被提次数',
  `status`            VARCHAR(12) NOT NULL DEFAULT 'new' COMMENT 'new/triage/accepted/planned/shipped/rejected',
  `priority`          VARCHAR(4) DEFAULT NULL COMMENT 'P0/P1/P2',
  `admin_note`        VARCHAR(500) DEFAULT NULL,
  `created_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  `last_seen_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '最近一次被提交',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_dedupe` (`dedupe_key`),
  KEY `idx_status` (`status`),
  KEY `idx_last_seen` (`last_seen_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='开发需求台账';

-- ========== Bug 台账：AI 导航沉淀的应用异常（编号即 #G{id}） ==========
CREATE TABLE IF NOT EXISTS `dev_bugs` (
  `id`                INT NOT NULL AUTO_INCREMENT,
  `source_message_id` BIGINT DEFAULT NULL COMMENT '触发消息的 ai_nav_messages 用户消息 id；游客 NULL',
  `user_id`           INT NOT NULL DEFAULT 0 COMMENT '0=游客',
  `title`             VARCHAR(200) NOT NULL COMMENT 'AI 短标题（原文）',
  `detail`            TEXT COMMENT '触发消息片段（≤500 字）',
  `category`          VARCHAR(12) NOT NULL DEFAULT 'content' COMMENT 'ui/api/data/export/content',
  `page_path`         VARCHAR(100) NOT NULL DEFAULT '' COMMENT '提问时所在页面',
  `error_code`        VARCHAR(32) DEFAULT NULL COMMENT '本期无采集来源，留 admin 补录',
  `severity`          VARCHAR(8) NOT NULL DEFAULT 'minor' COMMENT 'blocker/major/minor',
  `confidence`        DECIMAL(3,2) NOT NULL DEFAULT 0 COMMENT '判定置信度 0~1',
  `dedupe_key`        CHAR(40) NOT NULL COMMENT 'sha1(kind|归一化标题)，精确去重',
  `occurrences`       INT NOT NULL DEFAULT 1 COMMENT '被提次数',
  `status`            VARCHAR(12) NOT NULL DEFAULT 'new' COMMENT 'new/triage/confirmed/fixing/fixed/closed/duplicate',
  `duplicate_of`      INT DEFAULT NULL COMMENT '重复指向的 Bug id（代码校验存在性，不设外键）',
  `admin_note`        VARCHAR(500) DEFAULT NULL,
  `created_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  `last_seen_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '最近一次被提交',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_dedupe` (`dedupe_key`),
  KEY `idx_status` (`status`),
  KEY `idx_last_seen` (`last_seen_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='Bug 台账';

-- ========== users 增列 role（D4：admin 由直接改库设置，无前台入口） ==========
DROP PROCEDURE IF EXISTS `__add_users_role_col`;
DELIMITER $$
CREATE PROCEDURE `__add_users_role_col`()
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'users'
      AND COLUMN_NAME = 'role'
  ) THEN
    ALTER TABLE `users`
      ADD COLUMN `role` VARCHAR(10) NOT NULL DEFAULT 'user'
      COMMENT '角色：user/admin'
      AFTER `display_name`;
  END IF;
END$$
DELIMITER ;
CALL `__add_users_role_col`();
DROP PROCEDURE IF EXISTS `__add_users_role_col`;
