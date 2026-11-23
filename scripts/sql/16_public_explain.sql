-- =====================================================================
-- v1.5.0-T008 EXPLAIN-PUBLIC · 精讲公开化（公共/私人 + 追问）
-- 内容：
--   1) knowledge_points 增 visibility（public/private），存量方案 B 回填
--   2) is_solidified 语义收口：1 当且仅当已有可复用的 AI 精讲正文
--   3) 新建 explanation_followups（学习时追问与并入公共精讲）
-- 幂等：列/索引/表已存在时跳过，可重复执行
-- 执行前置：先执行 15_ai_tutor.sql（explanations 已存在）
-- 执行：mysql --socket=$HOME/.local/run/mysql.sock -uroot --default-character-set=utf8mb4 < scripts/sql/16_public_explain.sql
-- 迁移顺序：01 → ... → 15 → 16（本文件）
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 0. 幂等辅助过程 ==========
DROP PROCEDURE IF EXISTS pex_add_column;
DELIMITER //
CREATE PROCEDURE pex_add_column(IN tbl VARCHAR(64), IN col VARCHAR(64), IN ddl TEXT)
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

DROP PROCEDURE IF EXISTS pex_add_index;
DELIMITER //
CREATE PROCEDURE pex_add_index(IN tbl VARCHAR(64), IN idx VARCHAR(64), IN cols TEXT)
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

-- ========== 1. knowledge_points 增 visibility ==========
CALL pex_add_column('knowledge_points', 'visibility',
  '`visibility` ENUM(''private'',''public'') NOT NULL DEFAULT ''private'' COMMENT ''公共/私人：public 可被同身份用户经 L1 复用'' AFTER `is_solidified`');

CALL pex_add_index('knowledge_points', 'idx_reuse_public',
  '`normalized_key`,`identity_snapshot`,`is_solidified`,`visibility`');

-- ========== 2. is_solidified 语义回填（不改列结构） ==========
-- 新语义：is_solidified=1 当且仅当该词条已有可复用的 AI 精讲正文

-- 2.1 有精讲正文却标 0 的，补 1
UPDATE `knowledge_points` kp
SET kp.`is_solidified` = 1
WHERE kp.`is_solidified` = 0
  AND EXISTS (
    SELECT 1 FROM `kp_extras` e
    WHERE e.`knowledge_id` = kp.`id`
      AND e.`extra_type` = 'markdown_content'
      AND e.`content` <> ''
  );

-- 2.2 标 1 却没有任何精讲正文的（词表升级历史遗留），按新语义回退为 0
UPDATE `knowledge_points` kp
SET kp.`is_solidified` = 0
WHERE kp.`is_solidified` = 1
  AND NOT EXISTS (
    SELECT 1 FROM `kp_extras` e
    WHERE e.`knowledge_id` = kp.`id`
      AND e.`extra_type` = 'markdown_content'
      AND e.`content` <> ''
  );

-- ========== 3. visibility 存量回填（方案 B） ==========
-- 存量中「已就绪 + 常见身份」转公共，其余保持私人。
-- 依据：L1 现有查询本就不限 user_id，这批内容今天已在被同身份用户复用。

UPDATE `knowledge_points`
SET `visibility` = 'public'
WHERE `is_solidified` = 1
  AND `identity_snapshot` IN ('高中生','初中生','大学生','雅思','托福','考研');

-- ========== 4. 新建 explanation_followups ==========
CREATE TABLE IF NOT EXISTS `explanation_followups` (
  `id`           BIGINT NOT NULL AUTO_INCREMENT,
  `explain_id`   BIGINT NOT NULL COMMENT '所属讲解 explanations.id',
  `user_id`      INT NOT NULL DEFAULT 0 COMMENT '0=游客',
  `guest_id`     VARCHAR(64) NULL COMMENT '游客标识（Cookie eng_guest_id）',
  `question`     TEXT NOT NULL,
  `answer_md`    MEDIUMTEXT NULL COMMENT '追问回答（Markdown）',
  `status`       VARCHAR(12) NOT NULL DEFAULT 'queued' COMMENT 'queued/running/done/failed',
  `task_id`      BIGINT NULL COMMENT 'ai_tasks.id',
  `merged`       TINYINT NOT NULL DEFAULT 0 COMMENT '0未并入公共精讲 1已并入',
  `merged_kp_id` INT NULL COMMENT '并入的目标知识点',
  `created_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `finished_at`  DATETIME NULL,
  PRIMARY KEY (`id`),
  KEY `idx_explain` (`explain_id`,`id`),
  KEY `idx_user_time` (`user_id`,`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='v1.5.0-T008 学习时追问与并入公共精讲';

-- ========== 5. 清理辅助过程 ==========
DROP PROCEDURE IF EXISTS pex_add_column;
DROP PROCEDURE IF EXISTS pex_add_index;
