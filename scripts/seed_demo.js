#!/usr/bin/env node
// v1.5.3-T002 COMPETE-READY：评审演示账号与演示数据（幂等）
// 人工修改入口：DEMO_USERS（账号）、DEMO_WORDS（词条与精讲）、GOALS（课规）三处
// 重复执行：账号与词条按唯一键更新；学习进度/错题/课规只在首次（该用户无课规）写入，不重置已有进度
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const CFG = JSON.parse(fs.readFileSync(path.join(ROOT, 'config', 'db.json'), 'utf8'));

const HOME_MYSQL_BIN = path.join(os.homedir(), '.local', 'mysql', 'bin', 'mysql');
const MYSQL_BIN = process.env.MYSQL_BIN || (fs.existsSync(HOME_MYSQL_BIN) ? HOME_MYSQL_BIN : 'mysql');
const SOCKET = process.env.DB_SOCKET || CFG.socket;
const DATABASE = process.env.DB_NAME || CFG.database;
const DB_USER = process.env.DB_USER || 'root';
const PW_FILE = process.env.DB_PASSWORD_FILE || process.env.MYSQL_PW_FILE ||
  path.resolve(ROOT, 'config', CFG.password_file);

const DEMO_USERS = [
  { username: 'templecat', password: 'demo1234', displayName: '圣殿猫', uiMode: 'plan' },
  { username: 'wordninja', password: 'demo1234', displayName: '单词忍者', uiMode: 'plan' },
  { username: 'goldfox', password: 'demo1234', displayName: '金桂狐', uiMode: 'plan' },
];

// 旧演示账号 → 新账号名：保留原 uid 与全部演示数据
const RENAMES = [
  { from: 'demo1', to: 'templecat', displayName: '圣殿猫' },
  { from: 'demo2', to: 'wordninja', displayName: '单词忍者' },
  { from: 'demo3', to: 'goldfox', displayName: '金桂狐' },
];

const IDENTITY = '高中生';

const DEMO_WORDS = [
  {
    word: 'critical', pos: 'adj.', meaning: '关键的；批判性的',
    example: 'Time is critical to the success of the project.',
    require: '会写',
    md: '## critical\n\n**核心释义**：adj. 关键的；批判性的。\n\n- 表示“至关重要”时常与 `to` 搭配：`critical to success`。\n- 表示“批判性”时常见 `critical thinking`。\n\n**例句**\n- Time is critical to the success of the project.\n- Good teachers encourage critical thinking.\n\n**易混**：`crucial` 侧重“决定性的”；`critical` 还可表示“危急的”。',
  },
  {
    word: 'abandon', pos: 'v.', meaning: '放弃；抛弃',
    example: 'They had to abandon the plan because of the storm.',
    require: '会写',
    md: '## abandon\n\n**核心释义**：v. 放弃；抛弃；离弃。\n\n- `abandon doing sth`：放弃做某事。\n- `abandon oneself to`：沉溺于。\n\n**例句**\n- They had to abandon the plan because of the storm.\n- She never abandoned hope of becoming a writer.\n\n**派生**：`abandonment` n. 放弃；遗弃。',
  },
  {
    word: 'sustain', pos: 'v.', meaning: '维持；支撑；遭受',
    example: 'The economy has sustained steady growth for years.',
    require: '会写',
    md: '## sustain\n\n**核心释义**：v. 维持；支撑；遭受。\n\n- `sustain growth`：保持增长。\n- `sustain an injury`：受伤。\n\n**例句**\n- The economy has sustained steady growth for years.\n- A small mistake can sustain serious damage to trust.\n\n**派生**：`sustainable` adj. 可持续的。',
  },
  {
    word: 'approach', pos: 'n. / v.', meaning: '方法；接近',
    example: 'We need a new approach to solving this problem.',
    require: '会读',
    md: '## approach\n\n**核心释义**：n. 方法；途径；v. 接近；着手处理。\n\n- `an approach to (doing) sth`：做某事的方法（to 为介词）。\n- `approach a problem`：着手处理问题。\n\n**例句**\n- We need a new approach to solving this problem.\n- Winter is approaching.\n\n**易混**：`method` 更强调步骤；`approach` 更强调思路。',
  },
  {
    word: 'benefit', pos: 'n. / v.', meaning: '好处；受益',
    example: 'Regular exercise benefits both body and mind.',
    require: '会写',
    md: '## benefit\n\n**核心释义**：n. 好处；利益；v. 使受益；得益于。\n\n- `benefit from`：从……中受益。\n- `be of benefit to`：对……有益。\n\n**例句**\n- Regular exercise benefits both body and mind.\n- Students benefit from reading every day.\n\n**派生**：`beneficial` adj. 有益的。',
  },
  {
    word: 'reduce', pos: 'v.', meaning: '减少；降低',
    example: 'The new policy aims to reduce waste.',
    require: '会读',
    md: '## reduce\n\n**核心释义**：v. 减少；降低；使陷入。\n\n- `reduce sth by`：减少了多少。\n- `reduce sth to`：减少到多少。\n\n**例句**\n- The new policy aims to reduce waste.\n- Stress can be reduced by regular exercise.\n\n**易混**：`decrease` 可作不及物动词；`reduce` 多为及物动词。',
  },
  {
    word: 'significant', pos: 'adj.', meaning: '重要的；显著的',
    example: 'There has been a significant change in the climate.',
    require: '会写',
    md: '## significant\n\n**核心释义**：adj. 重要的；显著的；有意义的。\n\n- 修饰变化、差异、进步时，常译作“显著的”。\n- `significant other`：伴侣（口语）。\n\n**例句**\n- There has been a significant change in the climate.\n- Her contribution to the team was significant.\n\n**派生**：`significance` n. 重要性。',
  },
  {
    word: 'achieve', pos: 'v.', meaning: '实现；达到',
    example: 'She achieved her goal through years of effort.',
    require: '会写',
    md: '## achieve\n\n**核心释义**：v. 实现；达到；取得。\n\n- `achieve a goal`：实现目标。\n- `achieve success`：取得成功。\n\n**例句**\n- She achieved her goal through years of effort.\n- The team achieved a great result.\n\n**派生**：`achievement` n. 成就。',
  },
];

const GOALS = [
  {
    name: '背完必修一 Unit 1–3 新词',
    goalType: 'new_words',
    targetDesc: '每天 10 个新词，连续 7 天',
    current: 1, offsetDays: 14, dailyMinutes: 30, dailyNewWords: 10,
  },
  {
    name: '错题清零计划',
    goalType: 'retry',
    targetDesc: '本周完成 30 个错题重练',
    current: 0, offsetDays: 10, dailyMinutes: 20, dailyNewWords: 0,
  },
  {
    name: '精读两篇文章',
    goalType: 'reading',
    targetDesc: '完成 2 篇精读并整理生词',
    current: 0, offsetDays: 21, dailyMinutes: 25, dailyNewWords: 5,
  },
];

function q(v) {
  return "'" + String(v).replace(/\\/g, '\\\\').replace(/'/g, "\\'") + "'";
}

function runMysql(sql) {
  let pw = '';
  try { pw = fs.readFileSync(PW_FILE, 'utf8').trim(); }
  catch (e) { throw new Error(`读取数据库密码失败: ${PW_FILE}`); }
  const args = [];
  if (process.env.DB_HOST) {
    args.push(`--host=${process.env.DB_HOST}`, `--port=${process.env.DB_PORT || 3306}`, '--protocol=TCP');
  } else {
    args.push(`--socket=${SOCKET}`);
  }
  args.push(`-u${DB_USER}`, `-p${pw}`, '-N', '-B', DATABASE);
  const r = spawnSync(MYSQL_BIN, args, { input: sql, encoding: 'utf8', timeout: 120000 });
  if (r.error) throw new Error(`无法执行 mysql: ${r.error.message}`);
  if (r.status !== 0) {
    const msg = (r.stderr || '').split('\n').filter(l => !/^mysql: \[Warning\]/.test(l)).join(' ').trim();
    throw new Error(msg || `mysql 退出码 ${r.status}`);
  }
  return (r.stdout || '').trim();
}

function one(sql) {
  return runMysql(sql).split('\n')[0].trim();
}

function hashPassword(salt, password) {
  return crypto.createHash('sha256').update(salt + password).digest('hex');
}

function seedUser(u) {
  const salt = crypto.randomBytes(16).toString('hex');
  const hash = hashPassword(salt, u.password);
  runMysql(
    'INSERT INTO users (username,password_hash,salt,display_name,learner_identity,ui_mode) VALUES (' +
    [u.username, hash, salt, u.displayName, IDENTITY, u.uiMode].map(q).join(',') +
    ') ON DUPLICATE KEY UPDATE password_hash=VALUES(password_hash), salt=VALUES(salt), ' +
    'display_name=VALUES(display_name), learner_identity=VALUES(learner_identity), ui_mode=VALUES(ui_mode);'
  );
  const uid = parseInt(one(`SELECT id FROM users WHERE username=${q(u.username)};`), 10);
  if (!uid) throw new Error(`创建账号失败: ${u.username}`);

  // 词条与精讲：按 (user_id, normalized_key) 唯一键更新，重复执行不新增
  for (const w of DEMO_WORDS) {
    runMysql(
      'INSERT INTO knowledge_points (user_id,user_input_content,standard_content,difficulty,master_require,remark,' +
      'is_solidified,solidify_time,normalized_key,identity_snapshot,visibility,origin) VALUES (' +
      [uid, w.word, w.word, 3, w.require, '', 1, 'NOW()', w.word.toLowerCase(), IDENTITY, 'private', 'manual']
        .map((v, i) => (i === 7 ? v : q(v))).join(',') +
      ') ON DUPLICATE KEY UPDATE standard_content=VALUES(standard_content), difficulty=VALUES(difficulty), ' +
      'is_solidified=1, solidify_time=NOW(), identity_snapshot=VALUES(identity_snapshot), origin=VALUES(origin);'
    );
    const kid = parseInt(one(
      `SELECT id FROM knowledge_points WHERE user_id=${uid} AND normalized_key=${q(w.word.toLowerCase())};`
    ), 10);
    if (!kid) throw new Error(`词条写入失败: ${w.word}`);
    runMysql([
      `DELETE FROM kp_definitions WHERE knowledge_id=${kid};`,
      `DELETE FROM kp_extras WHERE knowledge_id=${kid} AND extra_type='markdown_content';`,
      `INSERT INTO kp_definitions (knowledge_id,pos,meaning,example,display_order) VALUES (` +
        [kid, w.pos, w.meaning, w.example, 1].map((v, i) => (i === 0 || i === 4 ? v : q(v))).join(',') + ');',
      `INSERT INTO kp_extras (knowledge_id,extra_type,content,display_order) VALUES (` +
        [kid, q('markdown_content'), q(w.md), 1].map((v, i) => (i === 0 || i === 3 ? v : v)).join(',') + ');',
    ].join('\n'));
  }

  // 学习进度、复习日志、错题、课规：只在首次写入，避免重置评委操作过的数据
  const goalCount = parseInt(one(`SELECT COUNT(*) FROM user_goals WHERE user_id=${uid};`), 10);
  if (goalCount > 0) {
    ensureExtras(uid);
    return { uid, seededActivity: false };
  }

  const kpRows = runMysql(
    `SELECT id, normalized_key FROM knowledge_points WHERE user_id=${uid} ORDER BY id;`
  ).split('\n').filter(Boolean).map(line => {
    const [id, key] = line.split('\t');
    return { id: parseInt(id, 10), key };
  });
  const byKey = Object.fromEntries(kpRows.map(r => [r.key, r.id]));

  const sql = ['START TRANSACTION;'];
  // 学情：前 4 个待复习，后 4 个已安排到未来
  kpRows.forEach((kp, i) => {
    sql.push(
      'INSERT INTO user_learning (user_id,knowledge_id,review_times,error_times,proficiency_level,' +
      'review_mode,is_active,next_review_time,last_review_time) VALUES (' +
      [uid, kp.id, 2 + (i % 3), i % 2, 2 + (i % 3), q('daily'), 1].join(',') +
      `, ${i < 4 ? 'DATE_SUB(NOW(), INTERVAL 2 HOUR)' : 'DATE_ADD(NOW(), INTERVAL 2 DAY)'}, ` +
      `DATE_SUB(NOW(), INTERVAL ${1 + i * 2} DAY));`
    );
  });
  // 近 14 天复习日志：每天 3 条，覆盖上周与本周，保证周报默认上周有数据
  for (let d = 13; d >= 0; d--) {
    for (let n = 0; n < 3; n++) {
      const kp = kpRows[(d + n) % kpRows.length];
      const correct = (d + n) % 3 !== 0 ? 1 : 0;
      sql.push(
        `INSERT INTO review_logs (user_id,knowledge_id,review_mode,exercise_type,is_correct,score,review_time) VALUES (` +
        [uid, kp.id, q('daily'), q('choice'), correct, correct ? 100 : 0].join(',') +
        `, DATE_SUB(NOW(), INTERVAL ${d * 24 + n} HOUR));`
      );
    }
  }
  // 错题归因 4 条 + 重练队列 3 条
  const errWords = DEMO_WORDS.slice(0, 4);
  const errTypes = ['meaning', 'spelling', 'collocation', 'grammar'];
  errWords.forEach((w, i) => {
    sql.push(
      'INSERT INTO error_attributions (user_id,knowledge_id,source_type,source_id,question_no,question_type,' +
      'user_answer,correct_answer,error_type,rule_reason,ai_reason,attribution_source,created_at) VALUES (' +
      [uid, byKey[w.word.toLowerCase()], 'exercise', 'demo-seed', i + 1, 'choice',
        '错误选项 ' + (i + 1), w.meaning, errTypes[i],
        '规则判定：' + errTypes[i] + ' 类错误', 'AI 补充：结合例句再记一遍', 'rule'].map(q).join(',') +
      `, DATE_SUB(NOW(), INTERVAL ${i < 2 ? 1 : 9 + i} DAY));`
    );
  });
  DEMO_WORDS.slice(0, 3).forEach((w, i) => {
    sql.push(
      'INSERT INTO retry_items (user_id,knowledge_id,attribution_id,retry_count,consecutive_correct,' +
      'next_retry_time,status) VALUES (' +
      [uid, byKey[w.word.toLowerCase()], 0, i, 0].join(',') +
      ', DATE_SUB(NOW(), INTERVAL 30 MINUTE), \'queued\');'
    );
  });
  // 三条课规：仅一条“正在执行”
  GOALS.forEach(g => {
    sql.push(
      'INSERT INTO user_goals (user_id,name,raw_text,goal_type,target_desc,deadline,daily_minutes,' +
      'daily_new_words,is_current,parse_source,status,start_date,activate_week,end_date) VALUES (' +
      [uid, g.name, g.name, g.goalType, g.targetDesc].map(q).join(',') +
      `, DATE_ADD(CURDATE(), INTERVAL ${g.offsetDays} DAY), ${g.dailyMinutes}, ${g.dailyNewWords}, ` +
      `${g.current}, 'template', 'active', DATE_SUB(CURDATE(), INTERVAL 7 DAY), ` +
      'DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) DAY), ' +
      `DATE_ADD(CURDATE(), INTERVAL ${g.offsetDays} DAY));`
    );
  });
  // 金桂余额：仅首次写入，之后不覆盖评委消耗
  sql.push(
    'INSERT INTO user_coin (user_id,balance,total_earned) VALUES (' +
    `${uid},120,120) ON DUPLICATE KEY UPDATE balance=GREATEST(balance,120), total_earned=GREATEST(total_earned,120);`
  );
  sql.push('COMMIT;');
  runMysql(sql.join('\n'));
  ensureExtras(uid);
  return { uid, seededActivity: true };
}

// 幂等补齐：私人词表/单词本、上周复习日志与错题、上周课规
function ensureExtras(uid) {
  const rows = runMysql(
    `SELECT id, normalized_key FROM knowledge_points WHERE user_id=${uid} ORDER BY id;`
  ).split('\n').filter(Boolean).map(line => {
    const [id, key] = line.split('\t');
    return { id: parseInt(id, 10), key };
  });
  if (!rows.length) return;
  const meta = Object.fromEntries(DEMO_WORDS.map(w => [w.word.toLowerCase(), w]));
  const demoRows = rows.filter(r => meta[r.key]).slice(0, DEMO_WORDS.length);
  const sql = ['START TRANSACTION;'];

  // 私人单词本
  let wbId = parseInt(one(`SELECT id FROM wordbook WHERE user_id=${uid} AND name='核心 8 词' LIMIT 1;`), 10);
  if (!wbId) {
    runMysql(
      `INSERT INTO wordbook (user_id,name,description,is_public,entry_count) VALUES (` +
      `${uid},'核心 8 词','演示用私人单词本：8 个已精讲词条',0,0);`
    );
    wbId = parseInt(one(`SELECT id FROM wordbook WHERE user_id=${uid} AND name='核心 8 词' LIMIT 1;`), 10);
  }
  if (!wbId) throw new Error(`单词本创建失败: uid=${uid}`);
  demoRows.forEach((kp, i) => {
    sql.push(
      `INSERT IGNORE INTO wordbook_entry (wordbook_id,knowledge_id,display_order) VALUES (${wbId},${kp.id},${i});`
    );
  });
  sql.push(`UPDATE wordbook SET entry_count=(SELECT COUNT(*) FROM wordbook_entry WHERE wordbook_id=${wbId}) WHERE id=${wbId};`);

  // 私人词表
  let wlId = parseInt(one(`SELECT id FROM wordlists WHERE user_id=${uid} AND source='user' AND title='核心 8 词' LIMIT 1;`), 10);
  if (!wlId) {
    runMysql(
      `INSERT INTO wordlists (user_id,title,source,description,sort_no,item_count) VALUES (` +
      `${uid},'核心 8 词','user','演示用私人词表：8 个已精讲词条',0,0);`
    );
    wlId = parseInt(one(`SELECT id FROM wordlists WHERE user_id=${uid} AND source='user' AND title='核心 8 词' LIMIT 1;`), 10);
  }
  if (!wlId) throw new Error(`词表创建失败: uid=${uid}`);
  demoRows.forEach((kp, i) => {
    const w = meta[kp.key];
    sql.push(
      `INSERT IGNORE INTO wordlist_items (wordlist_id,word,normalized_word,meaning,pos,order_no,knowledge_id,is_solidified) VALUES (` +
      [wlId, w.word, kp.key, w.meaning, w.pos, i, kp.id, 1].map(q).join(',') + ');'
    );
  });
  sql.push(`UPDATE wordlists SET item_count=(SELECT COUNT(*) FROM wordlist_items WHERE wordlist_id=${wlId}) WHERE id=${wlId};`);

  // 上周复习日志：已有 7 天前的数据则不再补
  const oldLogs = parseInt(one(
    `SELECT COUNT(*) FROM review_logs WHERE user_id=${uid} AND review_time < DATE_SUB(NOW(), INTERVAL 7 DAY);`
  ), 10);
  if (oldLogs === 0) {
    for (let d = 13; d >= 7; d--) {
      for (let n = 0; n < 3; n++) {
        const kp = rows[(d + n) % rows.length];
        const correct = (d + n) % 3 !== 0 ? 1 : 0;
        sql.push(
          `INSERT INTO review_logs (user_id,knowledge_id,review_mode,exercise_type,is_correct,score,review_time) VALUES (` +
          [uid, kp.id, q('daily'), q('choice'), correct, correct ? 100 : 0].join(',') +
          `, DATE_SUB(NOW(), INTERVAL ${d * 24 + n} HOUR));`
        );
      }
    }
  }

  // 上周错题：补 2 条，保证周报错题分布非空
  const oldErrs = parseInt(one(
    `SELECT COUNT(*) FROM error_attributions WHERE user_id=${uid} AND created_at < DATE_SUB(NOW(), INTERVAL 7 DAY);`
  ), 10);
  if (oldErrs === 0) {
    DEMO_WORDS.slice(0, 2).forEach((w, i) => {
      const kid = rows.find(r => r.key === w.word.toLowerCase());
      if (!kid) return;
      sql.push(
        'INSERT INTO error_attributions (user_id,knowledge_id,source_type,source_id,question_no,question_type,' +
        'user_answer,correct_answer,error_type,rule_reason,ai_reason,attribution_source,created_at) VALUES (' +
        [uid, kid.id, 'exercise', 'demo-seed-prev', i + 1, 'choice',
          '错误选项 ' + (i + 1), w.meaning, ['meaning', 'spelling'][i],
          '规则判定：上周错题', 'AI 补充：结合例句再记一遍', 'rule'].map(q).join(',') +
        `, DATE_SUB(NOW(), INTERVAL ${9 + i} DAY));`
      );
    });
  }

  // 上周学习时间分布：让周报趋势非零
  const oldLearn = parseInt(one(
    `SELECT COUNT(*) FROM user_learning WHERE user_id=${uid} AND last_review_time < DATE_SUB(NOW(), INTERVAL 7 DAY);`
  ), 10);
  if (oldLearn === 0) {
    rows.slice(0, 4).forEach((kp, i) => {
      sql.push(
        `UPDATE user_learning SET last_review_time=DATE_SUB(NOW(), INTERVAL ${8 + i} DAY) ` +
        `WHERE user_id=${uid} AND knowledge_id=${kp.id};`
      );
    });
  }

  // 上周课规完成记录：周报显示完成率
  const wk = 'DATE_SUB(CURDATE(), INTERVAL (WEEKDAY(CURDATE()) + 7) DAY)';
  sql.push(
    'INSERT INTO user_weekly_goal (user_id,year_week,goal_type,target,progress,achieved,week_key,goal_id,metric_key,plan_rank,joined_at,effective_target,claimed) VALUES (' +
    `${uid},YEARWEEK(${wk},3),'review_items',20,20,1,${wk},0,'review_items',1,${wk},20,1) ` +
    'ON DUPLICATE KEY UPDATE target=GREATEST(target,20), progress=GREATEST(progress,20), achieved=1, ' +
    'effective_target=GREATEST(effective_target,20);'
  );

  sql.push('COMMIT;');
  runMysql(sql.join('\n'));
}

function verify() {
  console.log(`[verify] db=${DATABASE}`);
  runMysql('SELECT 1;');
  for (const u of DEMO_USERS) {
    const r = one(
      'SELECT u.id, ' +
      `(SELECT COUNT(*) FROM users x WHERE x.id=u.id AND x.password_hash=SHA2(CONCAT(x.salt,${q(u.password)}),256)) AS pass_ok, ` +
      '(SELECT COUNT(*) FROM knowledge_points k WHERE k.user_id=u.id) AS kp, ' +
      '(SELECT COUNT(*) FROM knowledge_points k WHERE k.user_id=u.id AND k.is_solidified=1) AS solid, ' +
      "(SELECT COUNT(*) FROM kp_extras e JOIN knowledge_points k ON k.id=e.knowledge_id WHERE k.user_id=u.id AND e.extra_type='markdown_content') AS md, " +
      '(SELECT COUNT(*) FROM user_learning l WHERE l.user_id=u.id) AS learning, ' +
      '(SELECT COUNT(*) FROM review_logs g WHERE g.user_id=u.id) AS logs, ' +
      '(SELECT COUNT(*) FROM error_attributions a WHERE a.user_id=u.id) AS errs, ' +
      '(SELECT COUNT(*) FROM retry_items t WHERE t.user_id=u.id) AS retry, ' +
      '(SELECT COUNT(*) FROM user_goals o WHERE o.user_id=u.id) AS goals, ' +
      '(SELECT COUNT(*) FROM user_goals o WHERE o.user_id=u.id AND o.is_current=1) AS current_goals, ' +
      '(SELECT COUNT(*) FROM review_logs g WHERE g.user_id=u.id AND g.review_time < DATE_SUB(NOW(), INTERVAL 7 DAY)) AS old_logs, ' +
      '(SELECT COUNT(*) FROM wordbook b WHERE b.user_id=u.id) AS wb, ' +
      "(SELECT COUNT(*) FROM wordlists l WHERE l.user_id=u.id AND l.source='user') AS wl, " +
      '(SELECT COUNT(*) FROM user_weekly_goal w WHERE w.user_id=u.id AND w.week_key=DATE_SUB(CURDATE(), INTERVAL (WEEKDAY(CURDATE()) + 7) DAY)) AS prev_goal, ' +
      '(SELECT IFNULL(MAX(balance),-1) FROM user_coin c WHERE c.user_id=u.id) AS coin ' +
      `FROM users u WHERE u.username=${q(u.username)};`
    );
    if (!r) { console.log(`[verify] ${u.username} 不存在`); continue; }
    const [id, passOk, kp, solid, md, learning, logs, errs, retry, goals, cur, oldLogs, wb, wl, prevGoal, coin] = r.split('\t');
    const ok = passOk === '1' && Number(kp) >= 8 && Number(solid) >= 8 && Number(md) >= 8 &&
      Number(learning) >= 8 && Number(goals) >= 3 && Number(cur) >= 1 &&
      Number(oldLogs) >= 3 && Number(wb) >= 1 && Number(wl) >= 1 && Number(prevGoal) >= 1;
    console.log(`[verify] ${u.username} uid=${id} 密码=${passOk === '1' ? 'ok' : 'FAIL'} ` +
      `词条=${kp}(≥8) 精讲=${solid}(≥8) 正文=${md}(≥8) 学情=${learning}(≥8) 日志=${logs} 上周日志=${oldLogs} ` +
      `错题=${errs} 重练=${retry} 课规=${goals}(≥3) 执行中=${cur}(≥1) 单词本=${wb} 私人词表=${wl} ` +
      `上周课规=${prevGoal} 金桂=${coin} → ${ok ? 'PASS' : 'FAIL'}`);
  }
}

function migrateRenames() {
  for (const r of RENAMES) {
    const old = one(`SELECT id FROM users WHERE username=${q(r.from)};`);
    if (!old) continue;
    const taken = one(`SELECT id FROM users WHERE username=${q(r.to)};`);
    if (taken) continue;
    runMysql(
      `UPDATE users SET username=${q(r.to)}, display_name=${q(r.displayName)} WHERE id=${old};`
    );
    console.log(`[seed] 账号改名：${r.from} → ${r.to}（uid=${old}）`);
  }
}

function main() {
  if (process.argv.includes('--verify')) { verify(); return; }
  console.log(`[seed] mysql=${MYSQL_BIN} db=${DATABASE}`);
  runMysql('SELECT 1;');
  migrateRenames();
  let created = 0, activity = 0;
  for (const u of DEMO_USERS) {
    const r = seedUser(u);
    created++;
    if (r.seededActivity) activity++;
    console.log(`[seed] ${u.username} uid=${r.uid} ${r.seededActivity ? '已写入演示进度' : '已有课规，跳过进度写入'}`);
  }
  console.log(`[seed] 完成：账号 ${created} 个，首次写入进度 ${activity} 个`);
  console.log('[seed] 登录密码：demo1234（三个账号相同；评委主推 wordninja）');
}

try { main(); }
catch (e) {
  console.error('[seed] 失败：' + e.message);
  process.exit(1);
}
