-- =====================================================================
-- v1.5.0-T003 WORDLISTS · 两层词表索引（册 → 单元 → 单词）
-- 内容：新建 wordlists（词表/单元）、wordlist_items（词条）
-- 关系：wordlist_items.knowledge_id 仅作可选引用，词条本身不依赖知识点
-- 幂等：CREATE TABLE IF NOT EXISTS，可重复执行
-- 执行：mysql --socket=$HOME/.local/run/mysql.sock -uroot < scripts/sql/12_wordlists.sql
-- =====================================================================

SET NAMES utf8mb4;

-- ========== 词表（内置教材按「册 + 单元」一条；用户词表一条） ==========
CREATE TABLE IF NOT EXISTS `wordlists` (
  `id`          INT NOT NULL AUTO_INCREMENT,
  `user_id`     INT NOT NULL DEFAULT 1 COMMENT '归属用户；内置词表固定为系统账号 1',
  `title`       VARCHAR(255) NOT NULL COMMENT '词表标题',
  `source`      VARCHAR(16) NOT NULL DEFAULT 'user' COMMENT 'builtin=内置教材 / user=用户词表 / community=社区（预留）',
  `textbook`    VARCHAR(80) DEFAULT NULL COMMENT '教材版本，如 人教版（2019）',
  `volume`      VARCHAR(50) DEFAULT NULL COMMENT '册，如 必修第一册',
  `unit`        VARCHAR(50) DEFAULT NULL COMMENT '单元，如 Welcome Unit / Unit 1',
  `scope_key`   VARCHAR(255) DEFAULT NULL COMMENT '范围包合并键；用户普通词表为 NULL',
  `description` TEXT COMMENT '描述',
  `sort_no`     INT NOT NULL DEFAULT 0 COMMENT '内置词表排序号（册序号*100+单元序号）',
  `item_count`  INT NOT NULL DEFAULT 0 COMMENT '词条数（冗余字段）',
  `created_at`  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_builtin_unit` (`user_id`, `source`, `textbook`, `volume`, `unit`),
  UNIQUE KEY `uk_scope_key` (`user_id`, `scope_key`),
  KEY `idx_user_source` (`user_id`, `source`),
  KEY `idx_unit_lookup` (`textbook`, `volume`, `unit`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='词表/教材单元';

-- ========== 词条 ==========
CREATE TABLE IF NOT EXISTS `wordlist_items` (
  `id`             BIGINT NOT NULL AUTO_INCREMENT,
  `wordlist_id`    INT NOT NULL COMMENT '所属词表',
  `word`           VARCHAR(200) NOT NULL COMMENT '展示词条（保留原大小写）',
  `normalized_word` VARCHAR(200) NOT NULL COMMENT '归一化词条（小写+去首尾空白+合并空白），用于去重与搜索',
  `meaning`        TEXT COMMENT '释义（可为空，空时回落到知识点释义）',
  `pos`            VARCHAR(40) DEFAULT NULL COMMENT '词性',
  `order_no`       INT NOT NULL DEFAULT 0 COMMENT '词条顺序',
  `knowledge_id`   INT DEFAULT NULL COMMENT '可选关联 knowledge_points.id',
  `is_solidified`  TINYINT NOT NULL DEFAULT 0 COMMENT '关联知识点是否已固化',
  `created_at`     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_wordlist_word` (`wordlist_id`, `normalized_word`),
  KEY `idx_wordlist_order` (`wordlist_id`, `order_no`),
  KEY `idx_normalized_word` (`normalized_word`),
  KEY `idx_knowledge_id` (`knowledge_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='词表词条';
