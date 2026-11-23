-- =====================================================================
-- v1.5.0-T005 WORDLIB-STRUCT · 单词本体系重构（组合指针 / 统一记忆库）
-- 内容：
--   1) wordlists 增 kind（book/combo）、count_dirty
--   2) 新建 wordlist_children（组合成员指针，只存编号）
--   3) knowledge_points 增 origin / source_ref，并加词级唯一键 uk_user_norm
--   4) user_learning 加唯一键 uk_user_kp，并合并重复学情
--   5) 历史重复知识点去重合并（备份 + 指针改指，不丢释义）
--   6) 旧范围包（scope_key）转组合指针（kind='combo'）
-- 幂等：列/索引/表已存在时跳过，可重复执行；重复执行不会重复建备份行
-- 执行前置：先备份数据库（backups/ 目录）
-- 执行：mysql --socket=$HOME/.local/run/mysql.sock -uroot < scripts/sql/14_wordlib_struct.sql
-- 迁移顺序：01 → ... → 12 → 13 → 14（本文件）
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 0. 幂等辅助过程 ==========
DROP PROCEDURE IF EXISTS wls_add_column;
DELIMITER //
CREATE PROCEDURE wls_add_column(IN tbl VARCHAR(64), IN col VARCHAR(64), IN ddl TEXT)
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

DROP PROCEDURE IF EXISTS wls_add_index;
DELIMITER //
CREATE PROCEDURE wls_add_index(IN tbl VARCHAR(64), IN idx VARCHAR(64), IN cols TEXT, IN uniq INT)
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND INDEX_NAME = idx
  ) THEN
    SET @s = CONCAT('ALTER TABLE `', tbl, '` ADD ', IF(uniq = 1, 'UNIQUE ', ''), 'KEY `', idx, '` (', cols, ')');
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
  END IF;
END//
DELIMITER ;

-- ========== 1. 表结构（先建，后续步骤依赖） ==========

-- 1.1 wordlists：基层本 / 组合
CALL wls_add_column('wordlists', 'kind',
  '`kind` VARCHAR(8) NOT NULL DEFAULT ''book'' COMMENT ''book=基层单词本 / combo=组合（只存成员指针）'' AFTER `source`');
CALL wls_add_column('wordlists', 'count_dirty',
  '`count_dirty` TINYINT(1) NOT NULL DEFAULT 0 COMMENT ''组合词数待重算''');

-- 1.2 组合成员（只存编号，不复制词条）
CREATE TABLE IF NOT EXISTS `wordlist_children` (
  `id`          BIGINT NOT NULL AUTO_INCREMENT,
  `parent_id`   INT NOT NULL COMMENT '组合 wordlists.id',
  `child_type`  VARCHAR(8) NOT NULL COMMENT 'list=单词本/组合 / word=单个词条',
  `child_id`    BIGINT NOT NULL COMMENT 'list→wordlists.id；word→wordlist_items.id',
  `order_no`    INT NOT NULL DEFAULT 0 COMMENT '成员顺序（单词与单词本共用同一序号空间）',
  `created_at`  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_child` (`parent_id`, `child_type`, `child_id`),
  KEY `idx_parent_order` (`parent_id`, `order_no`),
  KEY `idx_child` (`child_type`, `child_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='组合成员指针';

-- 1.3 knowledge_points：来源标记
CALL wls_add_column('knowledge_points', 'origin',
  '`origin` VARCHAR(16) NOT NULL DEFAULT ''manual'' COMMENT ''manual=用户录入 / wordlist=词表升级''');
CALL wls_add_column('knowledge_points', 'source_ref',
  '`source_ref` INT DEFAULT NULL COMMENT ''升级来源 wordlists.id''');
CALL wls_add_index('knowledge_points', 'idx_origin', '`origin`', 0);

-- ========== 2. 归一化回填（唯一键前置） ==========
-- 规则与 AiQueueService::normalizeKey / wordlist_items.normalized_word 一致
UPDATE `knowledge_points`
SET `normalized_key` = TRIM(REGEXP_REPLACE(
      REGEXP_REPLACE(
        LOWER(IFNULL(NULLIF(`standard_content`, ''), `user_input_content`)),
        '[[:punct:]]', ''
      ),
      '[[:space:]]+', ' '
    ))
WHERE `normalized_key` IS NULL OR `normalized_key` = '';

-- 兜底：仍为空（异常数据）时用 id 造键，避免唯一键把多行 NULL 放过
UPDATE `knowledge_points`
SET `normalized_key` = CONCAT('__raw_', `id`)
WHERE `normalized_key` IS NULL OR `normalized_key` = '';

-- ========== 3. 历史重复知识点合并 ==========
-- 3.1 备份（只写一次；重复执行不会重复插入）
CREATE TABLE IF NOT EXISTS `knowledge_points_bak_20261109` LIKE `knowledge_points`;
INSERT IGNORE INTO `knowledge_points_bak_20261109` SELECT * FROM `knowledge_points`;

-- 3.2 重复映射表（每个重复 id → 保留 id）
DROP TEMPORARY TABLE IF EXISTS `__kp_dup_map`;
CREATE TEMPORARY TABLE `__kp_dup_map` (
  `dup_id`  INT NOT NULL PRIMARY KEY,
  `keep_id` INT NOT NULL
) ENGINE=InnoDB;

INSERT INTO `__kp_dup_map` (`dup_id`, `keep_id`)
SELECT kp.`id`, m.`keep_id`
FROM `knowledge_points` kp
JOIN (
  SELECT `user_id`, `normalized_key`, MIN(`id`) AS `keep_id`
  FROM `knowledge_points`
  GROUP BY `user_id`, `normalized_key`
  HAVING COUNT(*) > 1
) m ON m.`user_id` = kp.`user_id` AND m.`normalized_key` = kp.`normalized_key`
WHERE kp.`id` <> m.`keep_id`;

-- 3.3 先处理带业务唯一键的引用表，避免改指时撞键
-- 单词本：保留已有指向的同一本，删除重复指向
DELETE e FROM `wordbook_entry` e
JOIN `__kp_dup_map` d ON e.`knowledge_id` = d.`dup_id`
JOIN `wordbook_entry` k ON k.`wordbook_id` = e.`wordbook_id` AND k.`knowledge_id` = d.`keep_id`;

UPDATE `wordbook_entry` e
JOIN `__kp_dup_map` d ON e.`knowledge_id` = d.`dup_id`
SET e.`knowledge_id` = d.`keep_id`;

-- 学情：保留条已有学情则丢弃重复条；否则改指；随后按业务键清理
DELETE ul FROM `user_learning` ul
JOIN `__kp_dup_map` d ON ul.`knowledge_id` = d.`dup_id`
JOIN `user_learning` k ON k.`user_id` = ul.`user_id` AND k.`knowledge_id` = d.`keep_id`;

UPDATE `user_learning` ul
JOIN `__kp_dup_map` d ON ul.`knowledge_id` = d.`dup_id`
SET ul.`knowledge_id` = d.`keep_id`;

DELETE u1 FROM `user_learning` u1
JOIN `user_learning` u2
  ON u1.`user_id` = u2.`user_id` AND u1.`knowledge_id` = u2.`knowledge_id` AND u1.`id` > u2.`id`;

-- 3.4 其余含 knowledge_id 的表统一改指（自动发现，跳过上面已处理的三张）
DROP PROCEDURE IF EXISTS wls_repoint_refs;
DELIMITER //
CREATE PROCEDURE wls_repoint_refs()
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE tname VARCHAR(64);
  DECLARE cur CURSOR FOR
    SELECT c.`TABLE_NAME`
    FROM information_schema.COLUMNS c
    JOIN information_schema.TABLES t
      ON t.`TABLE_SCHEMA` = c.`TABLE_SCHEMA` AND t.`TABLE_NAME` = c.`TABLE_NAME` AND t.`TABLE_TYPE` = 'BASE TABLE'
    WHERE c.`TABLE_SCHEMA` = DATABASE()
      AND c.`COLUMN_NAME` = 'knowledge_id'
      AND c.`TABLE_NAME` NOT IN ('knowledge_points', 'wordbook_entry', 'user_learning');
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  OPEN cur;
  read_loop: LOOP
    FETCH cur INTO tname;
    IF done = 1 THEN LEAVE read_loop; END IF;
    SET @s = CONCAT('UPDATE `', tname, '` t JOIN `__kp_dup_map` d ON t.`knowledge_id` = d.`dup_id` SET t.`knowledge_id` = d.`keep_id`');
    PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;
  END LOOP;
  CLOSE cur;
END//
DELIMITER ;
CALL wls_repoint_refs();
DROP PROCEDURE IF EXISTS wls_repoint_refs;

-- 3.5 子表去重（同一保留条下重复释义/搭配/同义/反义/扩展）
DELETE d1 FROM `kp_definitions` d1
JOIN `kp_definitions` d2
  ON d1.`knowledge_id` = d2.`knowledge_id` AND d1.`meaning` = d2.`meaning` AND d1.`id` > d2.`id`;

DELETE c1 FROM `kp_collocations` c1
JOIN `kp_collocations` c2
  ON c1.`knowledge_id` = c2.`knowledge_id` AND c1.`collocation` = c2.`collocation` AND c1.`id` > c2.`id`;

DELETE s1 FROM `kp_synonyms` s1
JOIN `kp_synonyms` s2
  ON s1.`knowledge_id` = s2.`knowledge_id` AND s1.`synonym` = s2.`synonym` AND s1.`id` > s2.`id`;

DELETE a1 FROM `kp_antonyms` a1
JOIN `kp_antonyms` a2
  ON a1.`knowledge_id` = a2.`knowledge_id` AND a1.`antonym` = a2.`antonym` AND a1.`id` > a2.`id`;

DELETE x1 FROM `kp_extras` x1
JOIN `kp_extras` x2
  ON x1.`knowledge_id` = x2.`knowledge_id` AND x1.`extra_type` = x2.`extra_type`
     AND x1.`content` = x2.`content` AND x1.`id` > x2.`id`;

-- 3.6 删除重复知识点
DELETE kp FROM `knowledge_points` kp JOIN `__kp_dup_map` d ON kp.`id` = d.`dup_id`;

DROP TEMPORARY TABLE IF EXISTS `__kp_dup_map`;

-- ========== 4. 唯一键（去重后添加） ==========
CALL wls_add_index('knowledge_points', 'uk_user_norm', '`user_id`,`normalized_key`', 1);
CALL wls_add_index('user_learning', 'uk_user_kp', '`user_id`,`knowledge_id`', 1);

-- ========== 5. 旧范围包 → 组合指针 ==========
-- 5.1 标记为组合，并要求重算计数
UPDATE `wordlists`
SET `kind` = 'combo', `count_dirty` = 1
WHERE `scope_key` IS NOT NULL AND `scope_key` <> '' AND `kind` <> 'combo';

-- 5.2 序号辅助：拆 '|' 分隔的成员（最多 64 个）
DROP TEMPORARY TABLE IF EXISTS `__seq64`;
CREATE TEMPORARY TABLE `__seq64` (`n` INT NOT NULL PRIMARY KEY) ENGINE=InnoDB;
INSERT INTO `__seq64` (`n`)
SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8
UNION ALL SELECT 9 UNION ALL SELECT 10 UNION ALL SELECT 11 UNION ALL SELECT 12
UNION ALL SELECT 13 UNION ALL SELECT 14 UNION ALL SELECT 15 UNION ALL SELECT 16
UNION ALL SELECT 17 UNION ALL SELECT 18 UNION ALL SELECT 19 UNION ALL SELECT 20
UNION ALL SELECT 21 UNION ALL SELECT 22 UNION ALL SELECT 23 UNION ALL SELECT 24
UNION ALL SELECT 25 UNION ALL SELECT 26 UNION ALL SELECT 27 UNION ALL SELECT 28
UNION ALL SELECT 29 UNION ALL SELECT 30 UNION ALL SELECT 31 UNION ALL SELECT 32
UNION ALL SELECT 33 UNION ALL SELECT 34 UNION ALL SELECT 35 UNION ALL SELECT 36
UNION ALL SELECT 37 UNION ALL SELECT 38 UNION ALL SELECT 39 UNION ALL SELECT 40
UNION ALL SELECT 41 UNION ALL SELECT 42 UNION ALL SELECT 43 UNION ALL SELECT 44
UNION ALL SELECT 45 UNION ALL SELECT 46 UNION ALL SELECT 47 UNION ALL SELECT 48
UNION ALL SELECT 49 UNION ALL SELECT 50 UNION ALL SELECT 51 UNION ALL SELECT 52
UNION ALL SELECT 53 UNION ALL SELECT 54 UNION ALL SELECT 55 UNION ALL SELECT 56
UNION ALL SELECT 57 UNION ALL SELECT 58 UNION ALL SELECT 59 UNION ALL SELECT 60
UNION ALL SELECT 61 UNION ALL SELECT 62 UNION ALL SELECT 63 UNION ALL SELECT 64;

-- 5.3 scope_key 形如 'ids:1|2|3' → 子项为单词本 id
INSERT INTO `wordlist_children` (`parent_id`, `child_type`, `child_id`, `order_no`)
SELECT w.`id`, 'list', c.`id`, s.`n`
FROM `wordlists` w
JOIN `__seq64` s ON s.`n` <= 1 + LENGTH(w.`scope_key`) - LENGTH(REPLACE(w.`scope_key`, '|', ''))
JOIN `wordlists` c
  ON c.`id` = CAST(SUBSTRING_INDEX(SUBSTRING_INDEX(SUBSTRING(w.`scope_key`, 5), '|', s.`n`), '|', -1) AS UNSIGNED)
 AND c.`user_id` = w.`user_id`
WHERE w.`scope_key` LIKE 'ids:%'
ON DUPLICATE KEY UPDATE `order_no` = VALUES(`order_no`);

-- 5.4 scope_key 形如 'units:必修第一册/Unit 1|必修第一册/Unit 2' → 反查内置单元
INSERT INTO `wordlist_children` (`parent_id`, `child_type`, `child_id`, `order_no`)
SELECT w.`id`, 'list', b.`id`, s.`n`
FROM `wordlists` w
JOIN `__seq64` s ON s.`n` <= 1 + LENGTH(w.`scope_key`) - LENGTH(REPLACE(w.`scope_key`, '|', ''))
JOIN `wordlists` b
  ON b.`source` = 'builtin'
 AND b.`user_id` = w.`user_id`
 AND b.`volume` = SUBSTRING_INDEX(SUBSTRING_INDEX(SUBSTRING(w.`scope_key`, 7), '|', s.`n`), '/', 1)
 AND b.`unit`   = SUBSTRING_INDEX(SUBSTRING_INDEX(SUBSTRING(w.`scope_key`, 7), '|', s.`n`), '/', -1)
WHERE w.`scope_key` LIKE 'units:%'
ON DUPLICATE KEY UPDATE `order_no` = VALUES(`order_no`);

DROP TEMPORARY TABLE IF EXISTS `__seq64`;

-- ========== 6. 清理辅助过程 ==========
DROP PROCEDURE IF EXISTS wls_add_column;
DROP PROCEDURE IF EXISTS wls_add_index;

-- ========== 7. 执行结果核对 ==========
SELECT 'knowledge_points' AS 表, COUNT(*) AS 行数 FROM `knowledge_points`
UNION ALL SELECT 'knowledge_points(normalized_key为空)', COUNT(*) FROM `knowledge_points` WHERE `normalized_key` IS NULL OR `normalized_key` = ''
UNION ALL SELECT 'knowledge_points(去重后仍重复的键)', COUNT(*) FROM (
  SELECT `user_id`, `normalized_key` FROM `knowledge_points` GROUP BY `user_id`, `normalized_key` HAVING COUNT(*) > 1
) t
UNION ALL SELECT 'user_learning(仍重复)', COUNT(*) FROM (
  SELECT `user_id`, `knowledge_id` FROM `user_learning` GROUP BY `user_id`, `knowledge_id` HAVING COUNT(*) > 1
) t
UNION ALL SELECT 'wordlists(kind=combo)', COUNT(*) FROM `wordlists` WHERE `kind` = 'combo'
UNION ALL SELECT 'wordlist_children', COUNT(*) FROM `wordlist_children`;
