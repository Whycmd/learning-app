-- v1.2.0-T007: 每日小测 数据库扩展
-- 新增表: daily_quiz（小测记录）, user_coin（词币账户，本期仅建表预留）
-- 加表为兼容变更（MINOR），向后兼容

SET NAMES utf8mb4;

-- ========== 每日小测记录表（每次测验一条，每天可多次） ==========
CREATE TABLE IF NOT EXISTS `daily_quiz` (
  `id`            INT NOT NULL AUTO_INCREMENT,
  `user_id`       INT NOT NULL,
  `attempt_no`    INT NOT NULL DEFAULT 1 COMMENT '当日第几测（未来词币递减依据）',
  `quiz_date`     DATE NOT NULL COMMENT '小测日期',
  `questions`     MEDIUMTEXT NOT NULL COMMENT '题目快照JSON（含答案，服务端留存）',
  `answers`       TEXT DEFAULT NULL COMMENT '用户作答JSON',
  `score`         TINYINT DEFAULT NULL COMMENT '得分0-100',
  `correct_count` TINYINT NOT NULL DEFAULT 0,
  `status`        VARCHAR(10) NOT NULL DEFAULT 'ongoing' COMMENT 'ongoing/done',
  `start_time`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `finish_time`   DATETIME DEFAULT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_user_date` (`user_id`, `quiz_date`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='每日小测记录';

-- ========== 用户词币账户表（v1.2.0-T007 仅建表预留，本期无读写逻辑） ==========
CREATE TABLE IF NOT EXISTS `user_coin` (
  `id`           INT NOT NULL AUTO_INCREMENT,
  `user_id`      INT NOT NULL,
  `balance`      INT NOT NULL DEFAULT 0 COMMENT '当前余额',
  `total_earned` INT NOT NULL DEFAULT 0 COMMENT '累计获得',
  `updated_at`   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_user` (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='用户词币账户（预留）';
