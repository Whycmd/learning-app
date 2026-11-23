-- =====================================================================
-- v1.5.0-T002 AI-QUEUE · 通用 AI 队列与 L1 复用字段
-- 内容：扩展 ai_tasks；新建 ai_task_items；knowledge_points 增 normalized_key / identity_snapshot
-- 幂等：列、索引、表已存在时跳过，可重复执行
-- 回填：ai_tasks.kind（取 task_type）；normalized_key（trim+lower+去标点+合并空白）；
--       identity_snapshot（取归属用户当前 learner_identity，取不到为空）
-- 执行：mysql --socket=$HOME/.local/run/mysql.sock -uroot < scripts/sql/11_ai_queue.sql
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 0. 幂等辅助过程 ==========
DROP PROCEDURE IF EXISTS aiq_add_column;
DELIMITER //
CREATE PROCEDURE aiq_add_column(IN tbl VARCHAR(64), IN col VARCHAR(64), IN ddl TEXT)
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND COLUMN_NAME = col
  ) THEN
    SET @aiq_sql = CONCAT('ALTER TABLE `', tbl, '` ADD COLUMN ', ddl);
    PREPARE aiq_stmt FROM @aiq_sql;
    EXECUTE aiq_stmt;
    DEALLOCATE PREPARE aiq_stmt;
  END IF;
END//
DELIMITER ;

DROP PROCEDURE IF EXISTS aiq_add_index;
DELIMITER //
CREATE PROCEDURE aiq_add_index(IN tbl VARCHAR(64), IN idx VARCHAR(64), IN cols VARCHAR(255))
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND INDEX_NAME = idx
  ) THEN
    SET @aiq_sql = CONCAT('ALTER TABLE `', tbl, '` ADD INDEX `', idx, '` (', cols, ')');
    PREPARE aiq_stmt FROM @aiq_sql;
    EXECUTE aiq_stmt;
    DEALLOCATE PREPARE aiq_stmt;
  END IF;
END//
DELIMITER ;

-- ========== 1. 扩展 ai_tasks（旧列保留，旧 /api/ai/tasks 兼容） ==========
CALL aiq_add_column('ai_tasks', 'user_id',      '`user_id` INT NOT NULL DEFAULT 0 COMMENT ''任务归属用户''');
CALL aiq_add_column('ai_tasks', 'kind',         '`kind` VARCHAR(30) NOT NULL DEFAULT '''' COMMENT ''任务类型：solidify_kp/explanation/custom_exercise/assist''');
CALL aiq_add_column('ai_tasks', 'lane',         '`lane` VARCHAR(20) NOT NULL DEFAULT ''default'' COMMENT ''队列通道''');
CALL aiq_add_column('ai_tasks', 'progress',     '`progress` TINYINT NOT NULL DEFAULT 0 COMMENT ''进度百分比''');
CALL aiq_add_column('ai_tasks', 'total',        '`total` INT NOT NULL DEFAULT 0 COMMENT ''子项总数''');
CALL aiq_add_column('ai_tasks', 'payload_json', '`payload_json` TEXT NULL COMMENT ''请求快照 JSON''');
CALL aiq_add_column('ai_tasks', 'result_ref',   '`result_ref` VARCHAR(64) NULL COMMENT ''结果引用''');
CALL aiq_add_column('ai_tasks', 'retry_count',  '`retry_count` INT NOT NULL DEFAULT 0 COMMENT ''队列层重试次数''');
CALL aiq_add_column('ai_tasks', 'model_used',   '`model_used` VARCHAR(50) NULL COMMENT ''实际使用的模型''');
CALL aiq_add_column('ai_tasks', 'tokens_used',  '`tokens_used` INT NOT NULL DEFAULT 0 COMMENT ''累计 tokens''');
CALL aiq_add_column('ai_tasks', 'started_at',   '`started_at` DATETIME NULL COMMENT ''开始执行时间''');
CALL aiq_add_column('ai_tasks', 'finished_at',  '`finished_at` DATETIME NULL COMMENT ''完成时间''');
CALL aiq_add_column('ai_tasks', 'next_run_at',  '`next_run_at` DATETIME NULL COMMENT ''重试退避的下次可执行时间''');

CALL aiq_add_index('ai_tasks', 'idx_user_kind_status', '`user_id`,`kind`,`status`');
CALL aiq_add_index('ai_tasks', 'idx_lane_status', '`lane`,`status`,`create_time`');

-- 旧记录回填：kind 取 task_type；user_id 无历史归属，保持默认 0
UPDATE `ai_tasks` SET `kind` = `task_type` WHERE `kind` = '' AND `task_type` <> '';

-- ========== 2. 新建 ai_task_items ==========
CREATE TABLE IF NOT EXISTS `ai_task_items` (
  `id` BIGINT NOT NULL AUTO_INCREMENT,
  `task_id` INT NOT NULL COMMENT '关联 ai_tasks.id',
  `item_index` INT NOT NULL DEFAULT 0 COMMENT '子项序号（断点续跑依据）',
  `item_type` VARCHAR(30) NOT NULL DEFAULT '' COMMENT 'solidify_kp 等',
  `ref_id` INT DEFAULT NULL COMMENT '业务关联 ID（知识点 ID）',
  `input_text` VARCHAR(255) DEFAULT NULL COMMENT '原始输入，失败项展示用',
  `status` VARCHAR(20) NOT NULL DEFAULT 'pending' COMMENT 'pending/running/done/failed/skipped',
  `tokens_used` INT NOT NULL DEFAULT 0,
  `error` TEXT,
  `started_at` DATETIME DEFAULT NULL,
  `finished_at` DATETIME DEFAULT NULL,
  `create_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `update_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_task_item` (`task_id`, `item_index`),
  KEY `idx_task_status` (`task_id`, `status`),
  KEY `idx_ref` (`ref_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AI 任务子项（断点续跑/失败项）';

-- ========== 3. knowledge_points 增列（L1 复用判定键） ==========
CALL aiq_add_column('knowledge_points', 'normalized_key',    '`normalized_key` VARCHAR(255) DEFAULT NULL COMMENT ''归一化键（trim+lower+去标点+合并空白）''');
CALL aiq_add_column('knowledge_points', 'identity_snapshot', '`identity_snapshot` VARCHAR(50) NOT NULL DEFAULT '''' COMMENT ''生成时身份快照，L1 同身份复用''');

CALL aiq_add_index('knowledge_points', 'idx_reuse', '`normalized_key`,`identity_snapshot`,`is_solidified`');

-- 回填 normalized_key（与 C++ AiQueueService::normalizeKey 规则一致）
UPDATE `knowledge_points`
SET `normalized_key` = TRIM(REGEXP_REPLACE(
      REGEXP_REPLACE(
        LOWER(IFNULL(NULLIF(`standard_content`, ''), `user_input_content`)),
        '[[:punct:]]', ''
      ),
      '[[:space:]]+', ' '
    ))
WHERE `normalized_key` IS NULL OR `normalized_key` = '';

-- 回填 identity_snapshot：按归属用户当前身份（取不到为空）
UPDATE `knowledge_points` kp
LEFT JOIN `users` u ON u.`id` = kp.`user_id`
SET kp.`identity_snapshot` = IFNULL(u.`learner_identity`, '')
WHERE kp.`identity_snapshot` = '';

-- ========== 4. 清理辅助过程 ==========
DROP PROCEDURE IF EXISTS aiq_add_column;
DROP PROCEDURE IF EXISTS aiq_add_index;
