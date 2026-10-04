-- v1.3.0-T005: 点金圣殿 数据库扩展
-- 兼容变更（MINOR），向后兼容

SET NAMES utf8mb4;

-- ========== 1) user_coin 升级：加列累计消耗 ==========
ALTER TABLE user_coin
  ADD COLUMN total_spent INT NOT NULL DEFAULT 0 COMMENT '累计消耗';

-- ========== 2) 金桂流水账本（幂等：uk_txn 防重入账） ==========
CREATE TABLE IF NOT EXISTS user_coin_txn (
  id          INT AUTO_INCREMENT PRIMARY KEY,
  user_id     INT NOT NULL,
  delta       INT NOT NULL COMMENT '正=入账 负=扣费',
  reason_code VARCHAR(32) NOT NULL COMMENT 'quiz_done/quiz_perfect/review_done/title_up/goal_done/streak_bonus/quiz_cost/solidify_cost/lookup_spend/wordbook_spend',
  ref_id      INT DEFAULT NULL COMMENT '关联业务记录（quiz id / kp id / goal id 等）',
  balance_after INT NOT NULL COMMENT '变动后余额（审计用）',
  created_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_txn (user_id, reason_code, ref_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='金桂流水账册';

-- ========== 3) 朱砂印（掌握成就 + 课规里程碑，纯荣誉） ==========
CREATE TABLE IF NOT EXISTS user_seal (
  user_id   INT NOT NULL,
  seal_code VARCHAR(32) NOT NULL COMMENT 'mastery_100/mastery_500/mastery_1000/goal_done/streak_3/streak_10/streak_50',
  earned_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (user_id, seal_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='朱砂印收藏（纯荣誉，不可消耗）';

-- ========== 4) 称谓等级（纯展示） ==========
CREATE TABLE IF NOT EXISTS user_title (
  user_id        INT NOT NULL,
  title_code     VARCHAR(32) NOT NULL COMMENT 'scholar_1~scholar_5',
  upgrade_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (user_id),
  UNIQUE KEY uk_title (user_id, title_code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='圣殿称谓等级';

-- ========== 5) 本周课规（自然周，未达成自动作废） ==========
CREATE TABLE IF NOT EXISTS user_weekly_goal (
  user_id    INT NOT NULL,
  year_week  INT NOT NULL COMMENT 'ISO 周年×100+周号，如 202639',
  goal_type  VARCHAR(16) NOT NULL COMMENT 'review_items/new_words/quiz_count',
  target     INT NOT NULL,
  progress   INT NOT NULL DEFAULT 0,
  achieved   TINYINT NOT NULL DEFAULT 0 COMMENT '1=已达成',
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (user_id, year_week)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='本周课规（每周一条）';
