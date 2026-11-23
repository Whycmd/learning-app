-- =====================================================================
-- v1.5.1-T002 ERROR-RETRY：错题归因与重练队列
-- 兼容变更（MINOR）：新增 error_attributions / retry_items 两表
-- 执行前置：先备份数据库（backups/ 目录）
-- 本脚本可重复执行：CREATE TABLE IF NOT EXISTS
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 1) 错题归因明细：每次答错一条，按题去重 ==========
CREATE TABLE IF NOT EXISTS `error_attributions` (
  `id`                 BIGINT NOT NULL AUTO_INCREMENT,
  `user_id`            INT NOT NULL,
  `knowledge_id`       INT NOT NULL COMMENT '关联知识点',
  `source_type`        VARCHAR(16) NOT NULL DEFAULT 'exercise' COMMENT 'exercise/quiz/retry',
  `source_id`          VARCHAR(64) NOT NULL DEFAULT '' COMMENT '练习/小测批次 ID；空=单次批改',
  `question_no`        INT NOT NULL DEFAULT 0 COMMENT '批次内题号',
  `question_type`      VARCHAR(32) NOT NULL DEFAULT '' COMMENT '题型',
  `user_answer`        VARCHAR(500) NOT NULL DEFAULT '' COMMENT '用户作答',
  `correct_answer`     VARCHAR(500) NOT NULL DEFAULT '' COMMENT '正确答案',
  `error_type`         VARCHAR(16) NOT NULL DEFAULT 'unknown' COMMENT 'spelling/meaning/collocation/grammar/unknown',
  `rule_reason`        VARCHAR(255) NOT NULL DEFAULT '' COMMENT '规则判定说明',
  `ai_reason`          VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'AI 补充说明',
  `attribution_source` VARCHAR(16) NOT NULL DEFAULT 'rule' COMMENT 'rule/ai/user/fallback',
  `created_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`         DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_source` (`user_id`, `source_type`, `source_id`, `question_no`),
  KEY `idx_user_time` (`user_id`, `created_at`),
  KEY `idx_user_word` (`user_id`, `knowledge_id`),
  KEY `idx_user_type` (`user_id`, `error_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='错题归因明细';

-- ========== 2) 重练队列：按词唯一，派生状态 ==========
CREATE TABLE IF NOT EXISTS `retry_items` (
  `id`                  BIGINT NOT NULL AUTO_INCREMENT,
  `user_id`             INT NOT NULL,
  `knowledge_id`        INT NOT NULL,
  `attribution_id`      BIGINT NOT NULL DEFAULT 0 COMMENT '最近一条归因',
  `retry_count`         INT NOT NULL DEFAULT 0 COMMENT '累计重练次数',
  `consecutive_correct` INT NOT NULL DEFAULT 0 COMMENT '连续答对次数',
  `next_retry_time`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '下次应重练时间',
  `archived_at`         DATETIME NULL COMMENT '归档时间',
  `status`              VARCHAR(16) NOT NULL DEFAULT 'queued' COMMENT 'queued/done/skipped/archived',
  `created_at`          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_user_word` (`user_id`, `knowledge_id`),
  KEY `idx_due` (`user_id`, `status`, `next_retry_time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='错题重练队列';
