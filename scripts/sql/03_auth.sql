-- v1.2.0-T004: 多用户认证 数据库扩展
-- 新增表: users, user_sessions
-- 加表为兼容变更（MINOR），现有表零改动（user_id 字段早已预留）
-- 约定: id=1 永久保留给"默认本地用户（游客）"，注册用户 ID 从 2 开始

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ========== 注册用户表 ==========
DROP TABLE IF EXISTS `users`;
CREATE TABLE `users` (
  `id`            INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  `username`      VARCHAR(50) NOT NULL COMMENT '登录用户名',
  `password_hash` CHAR(64) NOT NULL COMMENT 'SHA256(salt+password) hex',
  `salt`          CHAR(32) NOT NULL COMMENT '随机盐 hex',
  `display_name`  VARCHAR(50) DEFAULT NULL COMMENT '显示名',
  `created_at`    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `last_login_at` DATETIME DEFAULT NULL,
  UNIQUE KEY `uk_username` (`username`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='注册用户表';

ALTER TABLE `users` AUTO_INCREMENT = 2;

-- ========== 登录会话表 ==========
DROP TABLE IF EXISTS `user_sessions`;
CREATE TABLE `user_sessions` (
  `token`      CHAR(64) NOT NULL PRIMARY KEY COMMENT '会话令牌 hex',
  `user_id`    INT NOT NULL,
  `expires_at` DATETIME NOT NULL,
  `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  KEY `idx_user_id` (`user_id`),
  KEY `idx_expires_at` (`expires_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='登录会话表';

SET FOREIGN_KEY_CHECKS = 1;
