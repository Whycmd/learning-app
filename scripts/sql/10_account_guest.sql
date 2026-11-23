-- =====================================================================
-- v1.5.0-T001 ACCOUNT-GUEST · 文章可见性迁移
-- 内容：articles 新增 visibility 列；历史 user_id=1 文章迁为 public
-- 幂等：列已存在时跳过 ALTER，可重复执行
-- 执行：mysql --socket=$HOME/.local/run/mysql.sock -uroot < scripts/sql/10_account_guest.sql
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 1. 新增 visibility 列（幂等） ==========
SET @col_exists = (
  SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'articles'
    AND COLUMN_NAME = 'visibility'
);

SET @ddl = IF(
  @col_exists = 0,
  'ALTER TABLE `articles` ADD COLUMN `visibility` ENUM(''private'',''public'') NOT NULL DEFAULT ''private'' COMMENT ''可见性：public 全站可读，private 仅归属用户'' AFTER `user_id`',
  'SELECT 1'
);

PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- ========== 2. 修正 user_id 语义注释（1 不再代表游客） ==========
ALTER TABLE `articles`
  MODIFY COLUMN `user_id` INT NOT NULL DEFAULT 1
  COMMENT '归属用户（1=系统账号，仅供测试；NULL 表示公共推荐文章，公共属性由 visibility 决定）';

-- ========== 3. 历史 user_id=1 文章迁为公共（PRD R28） ==========
UPDATE `articles` SET `visibility` = 'public' WHERE `user_id` = 1;
