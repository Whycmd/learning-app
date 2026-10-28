-- v1.4.0-T012：异步批量固化任务表（支持历史、断点续跑）
-- 可重复执行；不设外键，关联一致性由 Service 层保证。

CREATE TABLE IF NOT EXISTS solidify_jobs (
  id VARCHAR(32) NOT NULL,
  user_id INT NOT NULL,
  total INT NOT NULL DEFAULT 0,
  done INT NOT NULL DEFAULT 0,
  failed INT NOT NULL DEFAULT 0,
  status VARCHAR(20) NOT NULL DEFAULT 'pending',
  error TEXT,
  create_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  update_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_user_status (user_id, status),
  KEY idx_update (update_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS solidify_job_items (
  id BIGINT NOT NULL AUTO_INCREMENT,
  job_id VARCHAR(32) NOT NULL,
  knowledge_id INT NOT NULL,
  status VARCHAR(20) NOT NULL DEFAULT 'pending',
  error_msg TEXT,
  update_time DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_job_kp (job_id, knowledge_id),
  KEY idx_job_status (job_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
