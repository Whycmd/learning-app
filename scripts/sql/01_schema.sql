-- 英语学习软件 - 数据库全量表结构（对齐 PRD §3）
-- 数据库: english_learning  字符集: utf8mb4  排序: utf8mb4_unicode_ci
-- 主键自增INT，不开启外键约束（代码保障一致性），默认单用户（预留 user_id 多用户字段）

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ========== 主表1：知识点主表 ==========
DROP TABLE IF EXISTS `knowledge_points`;
CREATE TABLE `knowledge_points` (
  `id` INT NOT NULL AUTO_INCREMENT COMMENT '知识点唯一主键ID',
  `user_id` INT NOT NULL DEFAULT 1 COMMENT '用户ID（多用户预留，默认1）',
  `user_input_content` VARCHAR(255) NOT NULL COMMENT '用户原始录入知识点',
  `standard_content` VARCHAR(255) DEFAULT NULL COMMENT 'AI固化后最简标准知识点（查重依据，固化后不可修改）',
  `difficulty` TINYINT DEFAULT NULL COMMENT '难度等级（1-5级）',
  `master_require` VARCHAR(20) DEFAULT NULL COMMENT '要求掌握程度（会读/会写）',
  `remark` VARCHAR(255) DEFAULT NULL COMMENT '用户备注（专项标注，影响拓展维度）',
  `is_solidified` TINYINT(1) NOT NULL DEFAULT 0 COMMENT '是否固化（0未固化/1已固化）',
  `create_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '录入时间',
  `solidify_time` DATETIME DEFAULT NULL COMMENT 'AI固化完成时间',
  PRIMARY KEY (`id`),
  KEY `idx_user_id` (`user_id`),
  KEY `idx_is_solidified` (`is_solidified`),
  KEY `idx_standard_content` (`standard_content`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='知识点主表';

-- ========== 子表1：知识点释义表（多值拆分） ==========
DROP TABLE IF EXISTS `kp_definitions`;
CREATE TABLE `kp_definitions` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `knowledge_id` INT NOT NULL COMMENT '关联知识点主表ID',
  `pos` VARCHAR(20) DEFAULT NULL COMMENT '词性（n./v./adj./adv.等）',
  `meaning` VARCHAR(500) NOT NULL COMMENT '英文释义（优先英文原生释义）',
  `example` VARCHAR(500) DEFAULT NULL COMMENT '配套例句',
  `display_order` INT DEFAULT 0 COMMENT '展示顺序',
  PRIMARY KEY (`id`),
  KEY `idx_knowledge_id` (`knowledge_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='知识点释义子表';

-- ========== 子表2：固定搭配表 ==========
DROP TABLE IF EXISTS `kp_collocations`;
CREATE TABLE `kp_collocations` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `knowledge_id` INT NOT NULL,
  `collocation` VARCHAR(255) NOT NULL COMMENT '固定搭配',
  `example` VARCHAR(500) DEFAULT NULL COMMENT '搭配例句（单类最多2句）',
  `display_order` INT DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_knowledge_id` (`knowledge_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='固定搭配子表';

-- ========== 子表3：同源/近义词表 ==========
DROP TABLE IF EXISTS `kp_synonyms`;
CREATE TABLE `kp_synonyms` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `knowledge_id` INT NOT NULL,
  `synonym` VARCHAR(255) NOT NULL COMMENT '同源词/近义词',
  `usage_diff` VARCHAR(500) DEFAULT NULL COMMENT '用法差异/等价写法整理',
  `display_order` INT DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_knowledge_id` (`knowledge_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='同源/近义词子表';

-- ========== 子表4：反义词表 ==========
DROP TABLE IF EXISTS `kp_antonyms`;
CREATE TABLE `kp_antonyms` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `knowledge_id` INT NOT NULL,
  `antonym` VARCHAR(255) NOT NULL COMMENT '反义词',
  `display_order` INT DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_knowledge_id` (`knowledge_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='反义词子表';

-- ========== 子表5：词根词源/重难点扩展表（结构化精简存储） ==========
DROP TABLE IF EXISTS `kp_extras`;
CREATE TABLE `kp_extras` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `knowledge_id` INT NOT NULL,
  `extra_type` VARCHAR(50) NOT NULL COMMENT '类型: root/etymology/cultural/key_points/register',
  `content` TEXT NOT NULL COMMENT '精简内容',
  `display_order` INT DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `idx_knowledge_id` (`knowledge_id`),
  KEY `idx_extra_type` (`extra_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='词根/词源/重难点等扩展子表';

-- ========== 主表2：用户学情表 ==========
DROP TABLE IF EXISTS `user_learning`;
CREATE TABLE `user_learning` (
  `id` INT NOT NULL AUTO_INCREMENT COMMENT '学情记录ID',
  `user_id` INT NOT NULL DEFAULT 1,
  `knowledge_id` INT NOT NULL COMMENT '关联知识点主表ID',
  `review_times` INT NOT NULL DEFAULT 0 COMMENT '累计复习次数',
  `error_times` INT NOT NULL DEFAULT 0 COMMENT '累计错误次数',
  `proficiency_level` TINYINT NOT NULL DEFAULT 0 COMMENT '熟练度等级（0-5级，允许负值/略大于5）',
  `review_mode` VARCHAR(10) NOT NULL DEFAULT 'daily' COMMENT '复习模式: daily/weekly',
  `is_active` TINYINT(1) NOT NULL DEFAULT 1 COMMENT '是否参与复习(1是/0否)',
  `next_review_time` DATETIME DEFAULT NULL COMMENT '下次复习时间',
  `last_review_time` DATETIME DEFAULT NULL COMMENT '上次复习时间',
  `create_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `update_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_user_id` (`user_id`),
  KEY `idx_knowledge_id` (`knowledge_id`),
  KEY `idx_next_review` (`next_review_time`),
  KEY `idx_proficiency` (`proficiency_level`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='用户学情表';

-- ========== 主表3：英语文章资源表 ==========
-- 注意：此表的完整定义已迁移到 06_articles_cefr.sql（v1.3.0），此处保留占位以兼容旧文档
-- 新表结构：cefr_level 替代 difficulty TINYINT，新增 source_url/source_name/category/content_cn/image_data/import_method/ai_analysis/word_count/read_at 等字段
-- 执行顺序：先执行 01_schema.sql 的其余部分，再执行 06_articles_cefr.sql 完成 articles 表及 5 张子表的创建
-- DROP TABLE IF EXISTS `articles`;
-- CREATE TABLE `articles` (...);  -- 完整定义见 06_articles_cefr.sql

-- ========== 系统配置/复习记录日志表（用于审计与定时任务追踪） ==========
DROP TABLE IF EXISTS `review_logs`;
CREATE TABLE `review_logs` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `user_id` INT NOT NULL DEFAULT 1,
  `knowledge_id` INT NOT NULL,
  `review_mode` VARCHAR(10) NOT NULL,
  `exercise_type` VARCHAR(50) DEFAULT NULL COMMENT '题型',
  `is_correct` TINYINT(1) DEFAULT NULL COMMENT '本次是否答对',
  `score` INT DEFAULT NULL,
  `review_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_user_id` (`user_id`),
  KEY `idx_knowledge_id` (`knowledge_id`),
  KEY `idx_review_time` (`review_time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='复习日志表';

-- ========== AI 任务调度表（异常兜底，记录待固化重试） ==========
DROP TABLE IF EXISTS `ai_tasks`;
CREATE TABLE `ai_tasks` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `task_type` VARCHAR(50) NOT NULL COMMENT 'solidify_kp/solidify_article/exercise/grading',
  `ref_id` INT DEFAULT NULL COMMENT '关联业务ID（知识点ID/文章ID等）',
  `status` VARCHAR(20) NOT NULL DEFAULT 'pending' COMMENT 'pending/running/done/failed',
  `payload` TEXT DEFAULT NULL COMMENT '请求快照JSON',
  `result` TEXT DEFAULT NULL COMMENT '响应快照JSON',
  `error_msg` TEXT DEFAULT NULL,
  `retry_times` INT NOT NULL DEFAULT 0,
  `create_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `update_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_task_type` (`task_type`),
  KEY `idx_status` (`status`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AI任务调度与重试表';

SET FOREIGN_KEY_CHECKS = 1;
