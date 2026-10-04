-- v1.2.0-T003: 单词本系统 数据库扩展
-- 新增表: wordbook, wordbook_entry
-- 加表为兼容变更（MINOR），向后兼容 v1.1.0 功能

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ========== 单词本主表 ==========
DROP TABLE IF EXISTS `wordbook`;
CREATE TABLE `wordbook` (
  `id`          INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  `user_id`     INT NOT NULL DEFAULT 1 COMMENT '用户ID（多用户预留）',
  `name`        VARCHAR(255) NOT NULL COMMENT '单词本名称',
  `description` TEXT COMMENT '描述',
  `share_code`  VARCHAR(16) DEFAULT NULL COMMENT '分享识别码（NULL=不分享）',
  `is_public`   TINYINT(1) DEFAULT 0 COMMENT '0=私有 1=公开',
  `entry_count` INT DEFAULT 0 COMMENT '词条数量（冗余字段）',
  `created_at`  DATETIME DEFAULT CURRENT_TIMESTAMP,
  `updated_at`  DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY `uk_share_code` (`share_code`),
  KEY `idx_user_id` (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='单词本主表';

-- ========== 单词本词条关联表 ==========
DROP TABLE IF EXISTS `wordbook_entry`;
CREATE TABLE `wordbook_entry` (
  `id`            INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  `wordbook_id`   INT NOT NULL COMMENT '所属单词本',
  `knowledge_id`  INT NOT NULL COMMENT '指向 knowledge_points.id',
  `display_order` INT DEFAULT 0 COMMENT '展示顺序',
  `created_at`    DATETIME DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY `uk_wb_kp` (`wordbook_id`, `knowledge_id`),
  KEY `idx_wordbook_id` (`wordbook_id`),
  KEY `idx_knowledge_id` (`knowledge_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='单词本词条关联表';

SET FOREIGN_KEY_CHECKS = 1;
