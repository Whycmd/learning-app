-- =====================================================================
-- v1.5.0-T006 AI-TUTOR · AI 讲解 + 定制练习 + 空闲队列分级
-- 内容：
--   1) ai_tasks 增 queue_level / expedited_at 与领取索引（0=正常，1..N=空闲队列）
--   2) 新建 explanations（讲解历史与 L2 结果）
--   3) 新建 custom_exercises（定制练习与 24h 复用）
--   4) 新建 guest_trials（游客每日限额）
--   5) 新建 ai_usage_daily（登录用户每日 AI 次数与 tokens 记账）
-- 幂等：列/索引/表已存在时跳过，可重复执行
-- 执行前置：先执行 14_wordlib_struct.sql（知识库唯一键 uk_user_norm）
-- 执行：mysql --socket=$HOME/.local/run/mysql.sock -uroot --default-character-set=utf8mb4 < scripts/sql/15_ai_tutor.sql
-- 迁移顺序：01 → ... → 14 → 15（本文件）
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 0. 幂等辅助过程 ==========
DROP PROCEDURE IF EXISTS ait_add_column;
DELIMITER //
CREATE PROCEDURE ait_add_column(IN tbl VARCHAR(64), IN col VARCHAR(64), IN ddl TEXT)
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND COLUMN_NAME = col
  ) THEN
    SET @s = CONCAT('ALTER TABLE `', tbl, '` ADD COLUMN ', ddl);
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
  END IF;
END//
DELIMITER ;

DROP PROCEDURE IF EXISTS ait_add_index;
DELIMITER //
CREATE PROCEDURE ait_add_index(IN tbl VARCHAR(64), IN idx VARCHAR(64), IN cols TEXT)
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND INDEX_NAME = idx
  ) THEN
    SET @s = CONCAT('ALTER TABLE `', tbl, '` ADD KEY `', idx, '` (', cols, ')');
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
  END IF;
END//
DELIMITER ;

-- ========== 1. ai_tasks：队列分级 ==========
-- queue_level：0 正常队列；1..N 空闲队列（上级队列全空才领取）
CALL ait_add_column('ai_tasks', 'queue_level',
  '`queue_level` TINYINT NOT NULL DEFAULT 0 COMMENT ''0=正常队列 1..N=空闲队列'' AFTER `lane`');
CALL ait_add_column('ai_tasks', 'expedited_at',
  '`expedited_at` DATETIME NULL COMMENT ''加急回正常队列的时间''');
CALL ait_add_index('ai_tasks', 'idx_claim', '`status`, `queue_level`, `next_run_at`, `create_time`');

-- ========== 2. explanations：AI 讲解 ==========
CREATE TABLE IF NOT EXISTS `explanations` (
  `id`                BIGINT NOT NULL AUTO_INCREMENT,
  `user_id`           INT NOT NULL DEFAULT 0 COMMENT '0=游客',
  `guest_id`          VARCHAR(64) NULL COMMENT '游客标识（Cookie eng_guest_id）',
  `input_text`        TEXT NOT NULL COMMENT '原始输入',
  `normalized_key`    VARCHAR(191) NOT NULL COMMENT '归一化键，规则同 AiQueueService::normalizeKey',
  `input_type`        VARCHAR(16) NOT NULL DEFAULT 'word' COMMENT 'word/phrase/sentence/article',
  `identity_snapshot` VARCHAR(50) NOT NULL DEFAULT '' COMMENT '生成时的学习者身份',
  `depth`             VARCHAR(10) NOT NULL DEFAULT 'standard' COMMENT 'brief/standard/deep',
  `status`            VARCHAR(12) NOT NULL DEFAULT 'queued' COMMENT 'queued/running/done/failed',
  `task_id`           BIGINT NULL COMMENT 'ai_tasks.id',
  `l1_kp_id`          INT NULL COMMENT 'L1 命中的 knowledge_points.id',
  `summary`           TEXT NULL COMMENT '结论先行',
  `content_md`        MEDIUMTEXT NULL COMMENT '折叠精讲 Markdown',
  `result_json`       MEDIUMTEXT NULL COMMENT '六块结构化内容',
  `prev_result_json`  MEDIUMTEXT NULL COMMENT '重新生成前的旧结果快照',
  `tokens_used`       INT NOT NULL DEFAULT 0,
  `error_msg`         VARCHAR(255) NULL,
  `created_at`        DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `finished_at`       DATETIME NULL,
  PRIMARY KEY (`id`),
  KEY `idx_user_time` (`user_id`, `created_at`),
  KEY `idx_guest_time` (`guest_id`, `created_at`),
  KEY `idx_lookup` (`user_id`, `normalized_key`, `identity_snapshot`),
  KEY `idx_task` (`task_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AI 讲解历史与 L2 结果';

-- ========== 3. custom_exercises：定制练习 ==========
CREATE TABLE IF NOT EXISTS `custom_exercises` (
  `id`            BIGINT NOT NULL AUTO_INCREMENT,
  `user_id`       INT NOT NULL DEFAULT 0,
  `guest_id`      VARCHAR(64) NULL,
  `spec_json`     TEXT NOT NULL COMMENT '组题条快照',
  `spec_hash`     CHAR(16) NOT NULL COMMENT '规范化 FNV-1a 64 哈希',
  `task_id`       BIGINT NULL,
  `status`        VARCHAR(12) NOT NULL DEFAULT 'queued' COMMENT 'queued/running/done/failed',
  `item_count`    INT NOT NULL DEFAULT 0,
  `difficulty`    VARCHAR(12) NOT NULL DEFAULT 'basic' COMMENT 'basic/advanced/challenge',
  `style`         VARCHAR(16) NOT NULL DEFAULT 'exam' COMMENT '出题风格',
  `exercise_json` MEDIUMTEXT NULL COMMENT '题目（含答案）',
  `grade_json`    MEDIUMTEXT NULL COMMENT '客观题判分与主观题批改',
  `tokens_used`   INT NOT NULL DEFAULT 0,
  `error_msg`     VARCHAR(255) NULL,
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `finished_at`   DATETIME NULL,
  PRIMARY KEY (`id`),
  KEY `idx_user_spec` (`user_id`, `spec_hash`, `created_at`),
  KEY `idx_guest_time` (`guest_id`, `created_at`),
  KEY `idx_task` (`task_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='定制练习';

-- ========== 4. 限额记账 ==========
CREATE TABLE IF NOT EXISTS `guest_trials` (
  `guest_id` VARCHAR(64) NOT NULL COMMENT 'Cookie eng_guest_id',
  `feature`  VARCHAR(20) NOT NULL COMMENT 'explain/custom_exercise',
  `day`      DATE NOT NULL,
  `used`     INT NOT NULL DEFAULT 0,
  PRIMARY KEY (`guest_id`, `feature`, `day`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='游客每日 AI 试用计数';

CREATE TABLE IF NOT EXISTS `ai_usage_daily` (
  `user_id` INT NOT NULL,
  `day`     DATE NOT NULL,
  `kind`    VARCHAR(20) NOT NULL COMMENT 'explain/custom_exercise',
  `calls`   INT NOT NULL DEFAULT 0,
  `tokens`  BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (`user_id`, `day`, `kind`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='登录用户每日 AI 记账';

-- ========== 5. 收尾 ==========
DROP PROCEDURE IF EXISTS ait_add_column;
DROP PROCEDURE IF EXISTS ait_add_index;

SELECT 'explanations' AS t, COUNT(*) AS c FROM `explanations`
UNION ALL SELECT 'custom_exercises', COUNT(*) FROM `custom_exercises`
UNION ALL SELECT 'guest_trials', COUNT(*) FROM `guest_trials`
UNION ALL SELECT 'ai_usage_daily', COUNT(*) FROM `ai_usage_daily`;
