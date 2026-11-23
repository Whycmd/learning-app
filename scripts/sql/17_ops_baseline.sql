-- =====================================================================
-- v1.5.0-T011 OPS-BASELINE · 运维基线
-- 内容：
--   1) 新建 ai_call_log（AI 调用明细：场景/模型/tokens/耗时/成败）
-- 说明：金桂流水复用 08_user_treasury.sql 的 user_coin_txn，本文件不改其结构
-- 幂等：表已存在时跳过，可重复执行
-- 执行前置：无（仅新增独立表）
-- 执行：mysql --socket=$HOME/.local/run/mysql.sock -uroot --default-character-set=utf8mb4 < scripts/sql/17_ops_baseline.sql
-- 迁移顺序：01 → ... → 16 → 17（本文件）
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 1. AI 调用明细 ==========
-- 只记录调用特征与计数，不记录 prompt / 响应正文，避免隐私与体积问题
CREATE TABLE IF NOT EXISTS `ai_call_log` (
  `id`                BIGINT NOT NULL AUTO_INCREMENT,
  `user_id`           INT NOT NULL DEFAULT 0 COMMENT '0=游客或系统任务',
  `guest_id`          VARCHAR(64) NULL COMMENT '游客标识（Cookie eng_guest_id）',
  `feature`           VARCHAR(32) NOT NULL DEFAULT 'other' COMMENT '调用场景：solidify/exercise_generate/grade/quiz_generate/explain/explain_followup/custom_exercise/custom_exercise_grade/wordlist_generate/other',
  `provider`          VARCHAR(32) NOT NULL DEFAULT '' COMMENT '提供方类型：openai_compatible/agnes/mock',
  `model`             VARCHAR(64) NOT NULL DEFAULT '' COMMENT '模型名',
  `prompt_tokens`     INT NOT NULL DEFAULT 0,
  `completion_tokens` INT NOT NULL DEFAULT 0,
  `total_tokens`      INT NOT NULL DEFAULT 0,
  `duration_ms`       INT NOT NULL DEFAULT 0 COMMENT '调用耗时（毫秒）',
  `ok`                TINYINT NOT NULL DEFAULT 0 COMMENT '1=成功 0=失败',
  `error_msg`         VARCHAR(255) NULL COMMENT '失败摘要（截断 255 字）',
  `created_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_feature_time` (`feature`, `created_at`),
  KEY `idx_user_time` (`user_id`, `created_at`),
  KEY `idx_time` (`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AI 调用明细（tokens/耗时/成败）';

SELECT 'ai_call_log' AS t, COUNT(*) AS c FROM `ai_call_log`;
