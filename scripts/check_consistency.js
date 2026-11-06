#!/usr/bin/env node
// v1.4.1-T003 数据一致性校验：默认只报告；--fix 自动修复孤儿记录
// 人工修改入口：本文件的 RELATIONS / OWNER_RELATIONS 两个数组（新增检查项只改这里）
// 只读保证：默认模式只发 SELECT；DELETE/UPDATE 都在 fixOrphans() 内且受 --fix 开关保护
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const CFG = JSON.parse(fs.readFileSync(path.join(ROOT, 'config', 'db.json'), 'utf8'));

const HOME_MYSQL_BIN = path.join(os.homedir(), '.local', 'mysql', 'bin', 'mysql');
const MYSQL_BIN = process.env.MYSQL_BIN || (fs.existsSync(HOME_MYSQL_BIN) ? HOME_MYSQL_BIN : 'mysql');
const SOCKET = process.env.DB_SOCKET || CFG.socket;
const DATABASE = process.env.DB_NAME || CFG.database;
const DB_USER = process.env.DB_USER || 'root';
const PW_FILE = process.env.DB_PASSWORD_FILE || path.resolve(ROOT, 'config', CFG.password_file);

// 结构关系：子表外键列指向不存在的父行（父行已不存在，可安全删除子行）
const RELATIONS = [
  { name: 'kp_definitions→knowledge_points', child: 'kp_definitions', column: 'knowledge_id', parent: 'knowledge_points' },
  { name: 'kp_collocations→knowledge_points', child: 'kp_collocations', column: 'knowledge_id', parent: 'knowledge_points' },
  { name: 'kp_synonyms→knowledge_points', child: 'kp_synonyms', column: 'knowledge_id', parent: 'knowledge_points' },
  { name: 'kp_antonyms→knowledge_points', child: 'kp_antonyms', column: 'knowledge_id', parent: 'knowledge_points' },
  { name: 'kp_extras→knowledge_points', child: 'kp_extras', column: 'knowledge_id', parent: 'knowledge_points' },
  { name: 'user_learning→knowledge_points', child: 'user_learning', column: 'knowledge_id', parent: 'knowledge_points' },
  { name: 'review_logs→knowledge_points', child: 'review_logs', column: 'knowledge_id', parent: 'knowledge_points' },
  { name: 'wordbook_entry→wordbook', child: 'wordbook_entry', column: 'wordbook_id', parent: 'wordbook' },
  { name: 'wordbook_entry→knowledge_points', child: 'wordbook_entry', column: 'knowledge_id', parent: 'knowledge_points' },
  { name: 'article_sentences→articles', child: 'article_sentences', column: 'article_id', parent: 'articles' },
  { name: 'article_vocabulary→articles', child: 'article_vocabulary', column: 'article_id', parent: 'articles' },
  { name: 'article_quizzes→articles', child: 'article_quizzes', column: 'article_id', parent: 'articles' },
  { name: 'article_import_records→articles', child: 'article_import_records', column: 'article_id', parent: 'articles', allowNull: true },
  { name: 'article_recommend_queue→articles', child: 'article_recommend_queue', column: 'article_id', parent: 'articles', allowNull: true },
  { name: 'solidify_job_items→solidify_jobs', child: 'solidify_job_items', column: 'job_id', parent: 'solidify_jobs' },
  { name: 'solidify_job_items→knowledge_points', child: 'solidify_job_items', column: 'knowledge_id', parent: 'knowledge_points' },
];

// 用户归属：user_id 指向不存在的用户（数据不删，--fix 时改挂游客 id=1）
const OWNER_RELATIONS = [
  { table: 'knowledge_points', allowNull: false },
  { table: 'user_learning', allowNull: false },
  { table: 'review_logs', allowNull: false },
  { table: 'wordbook', allowNull: false },
  { table: 'daily_quiz', allowNull: false },
  { table: 'user_coin', allowNull: false },
  { table: 'user_coin_txn', allowNull: false },
  { table: 'user_seal', allowNull: false },
  { table: 'user_title', allowNull: false },
  { table: 'user_weekly_goal', allowNull: false },
  { table: 'user_sessions', allowNull: false },
  { table: 'articles', allowNull: false },
  { table: 'article_import_records', allowNull: false },
  { table: 'article_recommend_queue', allowNull: true },
  { table: 'solidify_jobs', allowNull: false },
  { table: 'article_vocabulary', allowNull: true },
];

// ai_tasks.ref_id、user_coin_txn.ref_id 是多义引用（可指向不同业务表），不检查、不修复。

function printHelp() {
  console.log(`用法: node scripts/check_consistency.js [选项]

选项:
  --json [路径]   输出 JSON 报告（默认 logs/consistency_<时间>.json）
  --limit <n>     每条关系的示例 id 数量（默认 5）
  --fix           自动修复孤儿：删除结构孤儿；用户归属改挂游客 id=1
  --help          显示本帮助

退出码: 0=无孤儿  2=仍有孤儿  1=运行错误`);
}

function parseArgs(argv) {
  const opts = { limit: 5, fix: false, json: null };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--help' || a === '-h') opts.help = true;
    else if (a === '--fix') opts.fix = true;
    else if (a === '--json') {
      opts.json = argv[i + 1] && !argv[i + 1].startsWith('--') ? argv[++i] : 'default';
    } else if (a.startsWith('--json=')) opts.json = a.slice(7);
    else if (a === '--limit') {
      const n = Number(argv[++i]);
      if (!Number.isInteger(n) || n <= 0) throw new Error('--limit 需为正整数');
      opts.limit = n;
    } else if (a.startsWith('--limit=')) {
      const n = Number(a.slice(8));
      if (!Number.isInteger(n) || n <= 0) throw new Error('--limit 需为正整数');
      opts.limit = n;
    } else throw new Error('未知参数: ' + a);
  }
  return opts;
}

function runMysql(sql) {
  let pw = '';
  try { pw = fs.readFileSync(PW_FILE, 'utf8').trim(); }
  catch (e) { throw new Error(`读取数据库密码失败: ${PW_FILE}`); }
  const r = spawnSync(MYSQL_BIN, [`--socket=${SOCKET}`, `-u${DB_USER}`, `-p${pw}`, '-N', '-B', DATABASE], {
    input: sql, encoding: 'utf8', timeout: 180000,
  });
  if (r.error) throw new Error(`无法执行 mysql: ${r.error.message}`);
  if (r.status !== 0) {
    const msg = (r.stderr || '').split('\n').filter(l => !/^mysql: \[Warning\]/.test(l)).join(' ').trim();
    throw new Error(msg || `mysql 退出码 ${r.status}`);
  }
  return (r.stdout || '').trim();
}

function ensureConn() {
  try { runMysql('SELECT 1;'); }
  catch (e) { throw new Error(`连接失败（socket=${SOCKET}）：${e.message}\n请先执行: bash scripts/start_mysql.sh`); }
}

const nullClause = (t, col, allowNull) =>
  `p.id IS NULL${allowNull ? ` AND c.\`${col}\` IS NOT NULL` : ''}`;

function scanRelation(rel, limit) {
  const where = nullClause(rel.child, rel.column, rel.allowNull);
  const base = `FROM \`${rel.child}\` c LEFT JOIN \`${rel.parent}\` p ON p.id = c.\`${rel.column}\` WHERE ${where}`;
  const count = Number(runMysql(`SELECT COUNT(*) ${base};`) || 0);
  let sample = [];
  if (count > 0) {
    const rows = runMysql(`SELECT c.id ${base} ORDER BY c.id LIMIT ${limit};`);
    sample = rows ? rows.split('\n').map(Number) : [];
  }
  return { name: rel.name, table: rel.child, column: rel.column, parent: rel.parent, count, sample };
}

function scanOwner(rel, limit) {
  const where = `u.id IS NULL${rel.allowNull ? ' AND c.user_id IS NOT NULL' : ''}`;
  const base = `FROM \`${rel.table}\` c LEFT JOIN \`users\` u ON u.id = c.user_id WHERE ${where}`;
  const count = Number(runMysql(`SELECT COUNT(*) ${base};`) || 0);
  let sample = [];
  if (count > 0) {
    const rows = runMysql(`SELECT DISTINCT c.user_id ${base} ORDER BY c.user_id LIMIT ${limit};`);
    sample = rows ? rows.split('\n').map(Number) : [];
  }
  return { name: `${rel.table}.user_id→users`, table: rel.table, column: 'user_id', parent: 'users', count, sample };
}

function scan(opts) {
  const relations = RELATIONS.map(r => scanRelation(r, opts.limit));
  const owners = OWNER_RELATIONS.map(r => scanOwner(r, opts.limit));
  const warnings = [];
  const guestCount = Number(runMysql('SELECT COUNT(*) FROM users WHERE id = 1;') || 0);
  if (!guestCount && owners.some(o => o.count > 0)) {
    warnings.push('users 缺少 id=1（游客）；--fix 会按库表约定自动创建游客 id=1 并改挂这些数据');
  }
  return {
    database: DATABASE,
    scanned_at: new Date().toISOString(),
    guest_exists: guestCount > 0,
    warnings,
    relations,
    owners,
    total: [...relations, ...owners].reduce((s, r) => s + r.count, 0),
  };
}

function fixOrphans(report) {
  const fixed = { structure: [], owners: [], errors: [], guest_created: false };
  for (const rel of RELATIONS) {
    const rep = report.relations.find(r => r.name === rel.name);
    if (!rep || rep.count === 0) continue;
    const where = nullClause(rel.child, rel.column, rel.allowNull);
    try {
      runMysql(`DELETE c FROM \`${rel.child}\` c LEFT JOIN \`${rel.parent}\` p ON p.id = c.\`${rel.column}\` WHERE ${where};`);
      fixed.structure.push({ name: rel.name, deleted: rep.count });
    } catch (e) { fixed.errors.push(`${rel.name}: ${e.message}`); }
  }
  if (!report.guest_exists) {
    // 库表约定：id=1 永久保留给「默认本地用户（游客）」（见 scripts/sql/03_auth.sql 头部注释）
    try {
      runMysql(`INSERT INTO users (id, username, password_hash, salt, display_name)
        VALUES (1, 'guest', REPEAT('0', 64), REPEAT('0', 32), '本地游客');`);
      report.guest_exists = true;
      fixed.guest_created = true;
    } catch (e) { fixed.errors.push(`创建游客: ${e.message}`); }
  }
  if (report.guest_exists) {
    for (const rel of OWNER_RELATIONS) {
      const r = report.owners.find(o => o.table === rel.table);
      if (!r || r.count === 0) continue;
      const where = `c.user_id IS NOT NULL AND c.user_id <> 1 AND NOT EXISTS (SELECT 1 FROM users u WHERE u.id = c.user_id)`;
      try {
        runMysql(`UPDATE \`${rel.table}\` c SET user_id = 1 WHERE ${where};`);
        fixed.owners.push({ name: r.name, reassigned: r.count });
      } catch (e) {
        // 主键含 user_id 的表（user_coin 等）合并到游客会撞键：改为清理不可达孤儿行
        if (/Duplicate entry|1062/i.test(e.message)) {
          try {
            runMysql(`DELETE c FROM \`${rel.table}\` c LEFT JOIN \`users\` u ON u.id = c.user_id WHERE u.id IS NULL${rel.allowNull ? ' AND c.user_id IS NOT NULL' : ''};`);
            fixed.owners.push({ name: r.name, removed: r.count });
          } catch (e2) { fixed.errors.push(`${r.name}: ${e2.message}`); }
        } else { fixed.errors.push(`${r.name}: ${e.message}`); }
      }
    }
  }
  return fixed;
}

function render(report, opts) {
  console.log(`\n数据库: ${report.database}  时间: ${report.scanned_at}`);
  const all = [...report.relations, ...report.owners];
  for (const r of all) {
    const mark = r.count > 0 ? '✗' : '✓';
    const sample = r.sample.length ? `  示例: ${r.sample.join(', ')}` : '';
    console.log(`  ${mark} ${r.name} | 孤儿 ${r.count}${sample}`);
  }
  report.warnings.forEach(w => console.log(`  ! ${w}`));
  console.log(`\n合计孤儿行数: ${report.total}${report.total ? '（需修复）' : '（无）'}`);
  if (opts.fix) console.log('已执行 --fix 自动修复，见 JSON 报告 fixed 字段。');
}

function main() {
  let opts;
  try { opts = parseArgs(process.argv.slice(2)); }
  catch (e) { console.error('[ERR] ' + e.message); process.exit(1); }
  if (opts.help) { printHelp(); process.exit(0); }

  let report;
  try {
    ensureConn();
    report = scan(opts);
    if (opts.fix) {
      report.fixed = fixOrphans(report);
      const after = scan(opts);
      report.after_fix = { total: after.total, relations: after.relations, owners: after.owners };
      report.total_after_fix = after.total;
    }
  } catch (e) {
    console.error('[ERR] ' + e.message);
    process.exit(1);
  }

  render(report, opts);
  if (report.fixed) {
    console.log(`修复前: ${report.total}  修复后: ${report.total_after_fix}`);
    if (report.fixed.errors.length) report.fixed.errors.forEach(x => console.log('  ! 修复失败: ' + x));
  }

  if (opts.json) {
    const p = opts.json === 'default'
      ? path.join(ROOT, 'logs', `consistency_${report.scanned_at.replace(/[:.]/g, '-')}.json`)
      : opts.json;
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, JSON.stringify(report, null, 2));
    console.log(`JSON 报告: ${p}`);
  }

  const remaining = report.fixed ? report.total_after_fix : report.total;
  process.exit(remaining > 0 ? 2 : 0);
}

main();
