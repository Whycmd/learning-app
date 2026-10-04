-- =====================================================================
-- v1.3.0-T001：学习者身份（字典表 + users 加列）
-- 执行前置：先备份数据库（backups/ 目录）
--
-- ⚠️ 迁移顺序（必须严格遵守，且每个脚本只跑一次）：
--   01_schema.sql    （基础 10 张表）
--   02_wordbook.sql  （wordbook + wordbook_entry）
--   03_auth.sql      （users + user_sessions）—— ⚠️ 会 DROP TABLE IF EXISTS users，
--                                                     重跑会清空所有注册用户！
--   04_daily_quiz.sql （daily_quiz + user_coin）
--   05_learner_identity.sql （本文件）
--
-- 如果发现 wordbook / daily_quiz / users 表缺失，说明 02 / 03 / 04
-- 尚未执行，需要按上述顺序补跑（注意 03 重跑会重建 users 表）
-- =====================================================================

-- 1. 新建字典表（仅作建议清单，不设外键，允许用户填自定义文本）
CREATE TABLE IF NOT EXISTS `learner_identities` (
  `id`      INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  `name`    VARCHAR(30) NOT NULL COMMENT '身份名称，如 高中生、雅思',
  `sort`    INT NOT NULL DEFAULT 0 COMMENT '排序权重，越小越靠前',
  `enabled` TINYINT NOT NULL DEFAULT 1 COMMENT '1=启用 0=禁用',
  UNIQUE KEY `uk_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='学习者身份建议清单';

-- 2. Seed：初始建议项（排序留 10 间隔，方便中间插入）
INSERT IGNORE INTO `learner_identities` (`name`, `sort`) VALUES
  ('初中生',           10),
  ('高中生',           20),
  ('大学生（非英专）', 30),
  ('英专生',           40),
  ('CET-4',            50),
  ('CET-6',            60),
  ('考研英语',         70),
  ('雅思',             80),
  ('托福',             90),
  ('GRE',              100);

-- 3. users 表加列（兼容变更，存量全部填 '高中生'）
-- MySQL 8.0 不支持 ADD COLUMN IF NOT EXISTS，用存储过程判断列是否存在后再加列
DROP PROCEDURE IF EXISTS `__add_learner_identity_col`;
DELIMITER $$
CREATE PROCEDURE `__add_learner_identity_col`()
BEGIN
  IF NOT EXISTS (
    SELECT 1 FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'users'
      AND COLUMN_NAME = 'learner_identity'
  ) THEN
    ALTER TABLE `users`
      ADD COLUMN `learner_identity` VARCHAR(50) NOT NULL DEFAULT '高中生'
      COMMENT '学习者身份（可自定义文本）'
      AFTER `display_name`;
  END IF;
END$$
DELIMITER ;
CALL `__add_learner_identity_col`();
DROP PROCEDURE IF EXISTS `__add_learner_identity_col`;
