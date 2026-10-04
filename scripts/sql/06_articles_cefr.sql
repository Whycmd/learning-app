-- =====================================================================
-- v1.3.0 文章模块 · 数据库迁移脚本
-- 执行前需确认：旧 articles 表无真实数据（前端 articles.html 从未开放录入）
-- 与 01_schema.sql 中旧 articles 表定义冲突，本脚本执行后以本脚本为准
-- =====================================================================

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ========== 文章主表 ==========
DROP TABLE IF EXISTS `articles`;
CREATE TABLE `articles` (
  `id`              INT AUTO_INCREMENT PRIMARY KEY,
  `user_id`         INT NOT NULL DEFAULT 1 COMMENT '归属用户（1=游客，NULL 表示公共推荐文章）',
  `title`           VARCHAR(500) NOT NULL,
  `source_url`      VARCHAR(1000) COMMENT '原文链接（URL 导入时保留）',
  `source_name`     VARCHAR(200) COMMENT '来源媒体（如 BBC、The Guardian）',
  `category`        ENUM('news','history','literature','science','culture','professional','other')
                    NOT NULL DEFAULT 'other',
  `content`         MEDIUMTEXT NOT NULL COMMENT '英文原文（段落用 <p> 标签分隔）',
  `content_cn`      MEDIUMTEXT COMMENT '中文译文（中英对照用，入库时可预生成）',
  `image_data`      MEDIUMBLOB COMMENT '图片导入时的原始数据（可选）',
  `import_method`   ENUM('url','text','image','ai_generated','ai_recommended','manual')
                    NOT NULL DEFAULT 'text',
  `ai_analysis`     JSON COMMENT 'AI 分析结果快照（背景/赏析/素材，缓存复用）',
  `cefr_level`      VARCHAR(10) NOT NULL DEFAULT 'B1' COMMENT 'CEFR 难度等级（A1/A2/B1/B2/C1）',
  `word_count`      INT NOT NULL DEFAULT 0 COMMENT '字数缓存',
  `match_threshold` INT COMMENT 'v1.2.0遗留字段，暂不使用',
  `read_at`         DATETIME COMMENT '用户完成阅读时间（NULL 表示未读）',
  `created_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_user_id (user_id),
  INDEX idx_category (category),
  INDEX idx_cefr (cefr_level),
  INDEX idx_import_method (import_method)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='文章主表';

-- ========== 文章句子表（TTS 分句 + 长难句拆解缓存） ==========
DROP TABLE IF EXISTS `article_sentences`;
CREATE TABLE `article_sentences` (
  `id`            INT AUTO_INCREMENT PRIMARY KEY,
  `article_id`    INT NOT NULL,
  `sentence_no`   INT NOT NULL COMMENT '句子序号（从 1 开始）',
  `sentence_en`   TEXT NOT NULL COMMENT '英文原文句子',
  `sentence_cn`   TEXT COMMENT '中文翻译（可选）',
  `cefr_level`    VARCHAR(10) NOT NULL DEFAULT 'B1' COMMENT '句子 CEFR 难度',
  `analysis_json` JSON COMMENT '长难句拆解缓存（AI 生成后复用）',
  INDEX idx_article_id (article_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='文章分句表';

-- ========== 文章词汇难度表 ==========
DROP TABLE IF EXISTS `article_vocabulary`;
CREATE TABLE `article_vocabulary` (
  `id`           INT AUTO_INCREMENT PRIMARY KEY,
  `article_id`   INT NOT NULL,
  `word`         VARCHAR(100) NOT NULL COMMENT '归一化（小写、去标点）',
  `position`     INT NOT NULL COMMENT '在文章中首次出现的字符位置',
  `occurrence`   INT NOT NULL DEFAULT 1 COMMENT '出现次数',
  `cefr_level`   VARCHAR(10) NOT NULL DEFAULT 'B1' COMMENT '词汇 CEFR 难度（查 EFLLex 表）',
  `is_new_word`  TINYINT(1) NOT NULL DEFAULT 0 COMMENT '是否对用户为"生词"',
  `user_id`      INT COMMENT 'NULL 表示全局标记，非 NULL 为用户特定标记',
  INDEX idx_article_id (article_id),
  INDEX idx_word (word),
  INDEX idx_user_id (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='文章词汇难度表';

-- ========== 阅读理解题 ==========
DROP TABLE IF EXISTS `article_quizzes`;
CREATE TABLE `article_quizzes` (
  `id`            INT AUTO_INCREMENT PRIMARY KEY,
  `article_id`    INT NOT NULL,
  `question_no`   INT NOT NULL,
  `question_type` ENUM('reading_comprehension','vocabulary_in_context','inference','literary_device') NOT NULL,
  `question_text` TEXT NOT NULL,
  `options`       JSON COMMENT '选项数组 [{"text":"...","is_correct":false}, ...]',
  `correct_answer` VARCHAR(200) COMMENT '正确答案文本或选项标识',
  `explanation`   TEXT COMMENT '答案解析',
  `cefr_level`    VARCHAR(10) NOT NULL DEFAULT 'B1',
  `created_by`    ENUM('ai','manual') NOT NULL DEFAULT 'ai',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_article_id (article_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='阅读理解题';

-- ========== 图片导入记录（追踪 OCR 处理状态） ==========
DROP TABLE IF EXISTS `article_import_records`;
CREATE TABLE `article_import_records` (
  `id`              INT AUTO_INCREMENT PRIMARY KEY,
  `user_id`         INT NOT NULL,
  `original_image`  MEDIUMBLOB COMMENT '原始截图/拍照',
  `extracted_text`  TEXT COMMENT 'OCR/AI 提取的文本',
  `status`          ENUM('pending','processing','completed','failed') NOT NULL DEFAULT 'pending',
  `article_id`      INT COMMENT '处理后关联的文章 ID',
  `error_msg`       VARCHAR(500),
  `created_at`      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_user_id (user_id),
  INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='图片导入记录';

-- ========== AI 推荐文章队列 ==========
DROP TABLE IF EXISTS `article_recommend_queue`;
CREATE TABLE `article_recommend_queue` (
  `id`          INT AUTO_INCREMENT PRIMARY KEY,
  `user_id`     INT COMMENT '目标用户（NULL 表示公开推荐）',
  `topic`       VARCHAR(200) COMMENT '推荐主题关键词',
  `cefr_level`  VARCHAR(10) NOT NULL COMMENT '目标 CEFR 等级',
  `source_url`  VARCHAR(1000) COMMENT '原文链接（Currents API 返回）',
  `source_name` VARCHAR(200) COMMENT '来源媒体',
  `title`       VARCHAR(500) COMMENT '推荐文章标题',
  `summary`     TEXT COMMENT '预览摘要',
  `status`      ENUM('pending','fetched','processed','failed') NOT NULL DEFAULT 'pending',
  `article_id`  INT COMMENT '用户保存后关联的文章 ID',
  `error_msg`   VARCHAR(500),
  `created_at`  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_user_id (user_id),
  INDEX idx_status (status),
  INDEX idx_cefr (cefr_level)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='AI 推荐文章队列';

SET FOREIGN_KEY_CHECKS = 1;
