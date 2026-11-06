// v1.4.1-T003 数据一致性校验脚本 - 自动化验收
// 覆盖计划 §5 的 1-9 条断言；不依赖人工检查，探针数据自动清理
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const SCRIPT = path.join(ROOT, 'scripts', 'check_consistency.js');
const { MYSQL_BIN, SOCK, PW_FILE, IN_CONTAINER } = require('./_env');
const DB = 'english_learning';
const PROBE_KP = 999999;

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function mysql(sql) {
  const pw = fs.readFileSync(PW_FILE, 'utf8').trim();
  const r = spawnSync(MYSQL_BIN, [`--socket=${SOCK}`, '-uroot', `-p${pw}`, '-N', '-B', DB], {
    input: sql, encoding: 'utf8', timeout: 60000,
  });
  if (r.error) throw new Error(r.error.message);
  if (r.status !== 0) throw new Error((r.stderr || '').trim());
  return (r.stdout || '').trim();
}

function runScript(args, env = {}) {
  return spawnSync(process.execPath, [SCRIPT, ...args], {
    cwd: ROOT, encoding: 'utf8', timeout: 120000,
    env: { ...process.env, ...env },
  });
}

function probeCleanup() {
  try { mysql(`DELETE FROM kp_extras WHERE knowledge_id = ${PROBE_KP};`); } catch (e) { /* 忽略清理失败 */ }
}

function main() {
  console.log('\n=== v1.4.1-T003 数据一致性校验（stage14） ===');

  try { mysql('SELECT 1;'); }
  catch (e) {
    console.log('  ✗ MySQL 不可用，请先执行 bash scripts/start_mysql.sh');
    process.exit(1);
  }
  probeCleanup();

  // 1) --help
  const help = runScript(['--help']);
  if (help.status === 0 && /--fix/.test(help.stdout) && /--json/.test(help.stdout)) ok('1 --help 退出码 0 且含 --fix/--json');
  else ko('1 --help', `status=${help.status} out=${help.stdout.slice(0, 80)}`);

  // 2) socket 无效 → 退出码 1 + 启动提示
  // 容器内连接走 TCP，不存在"本地 socket 文件缺失"这条路径，
  // 而且 DB_SOCKET 覆盖对 TCP 模式无效，因此跳过该本机专属断言。
  if (IN_CONTAINER) {
    skip('2 连接失败退出码 1 且提示 start_mysql.sh', '容器内走 TCP 无 socket 场景');
  }
  else {
    const bad = runScript(['--json', path.join(os.tmpdir(), 't003_bad.json')], { DB_SOCKET: '/tmp/definitely_missing.sock' });
    if (bad.status === 1 && /start_mysql\.sh/.test(bad.stderr || '')) ok('2 连接失败退出码 1 且提示 start_mysql.sh');
    else ko('2 连接失败', `status=${bad.status} err=${(bad.stderr || '').slice(0, 120)}`);
  }

  // 3) 默认运行：退出码 0 或 2，输出关系与合计
  const baseJson = path.join(os.tmpdir(), 't003_base.json');
  const base = runScript(['--json', baseJson]);
  const outOk = /kp_extras→knowledge_points/.test(base.stdout) && /合计孤儿行数/.test(base.stdout);
  if ((base.status === 0 || base.status === 2) && outOk) ok('3 默认运行输出关系清单与合计');
  else ko('3 默认运行', `status=${base.status} out=${base.stdout.slice(0, 120)}`);

  // 8) JSON 结构
  let report = null;
  try { report = JSON.parse(fs.readFileSync(baseJson, 'utf8')); } catch (e) { report = null; }
  const rel = report && Array.isArray(report.relations)
    ? report.relations.find(r => r.name === 'kp_extras→knowledge_points') : null;
  const shapeOk = report && typeof report.database === 'string' && typeof report.scanned_at === 'string'
    && Array.isArray(report.relations) && Array.isArray(report.owners)
    && rel && typeof rel.count === 'number' && Array.isArray(rel.sample)
    && typeof rel.table === 'string' && typeof rel.column === 'string' && typeof rel.parent === 'string';
  if (shapeOk) ok('8 JSON 结构完整', `relations=${report.relations.length} owners=${report.owners.length}`);
  else ko('8 JSON 结构', JSON.stringify(report && report.relations ? report.relations.slice(0, 1) : null));

  // 9) 源码静态断言：写操作全部限定在 fixOrphans 内；无 DDL
  const src = fs.readFileSync(SCRIPT, 'utf8');
  const code = src.replace(/\/\/.*$/gm, '');
  const noDdl = !/DROP\s+TABLE|ALTER\s+TABLE|TRUNCATE/i.test(code);
  const fixStart = code.indexOf('function fixOrphans');
  const fixEnd = code.indexOf('function render');
  const writes = [];
  const wre = /\b(INSERT|UPDATE|DELETE)\b/g;
  let wm; while ((wm = wre.exec(code)) !== null) writes.push(wm.index);
  const writesScoped = writes.length > 0 && fixStart >= 0 && fixEnd > fixStart
    && writes.every(i => i > fixStart && i < fixEnd);
  if (noDdl && writesScoped) ok('9 写操作仅 fixOrphans 内', `写标记 ${writes.length} 处`);
  else ko('9 源码静态断言', `noDdl=${noDdl} scoped=${writesScoped} idx=${writes.join(',')}`);

  // 关系条数 ≥ 14（防清单被误删）
  const relCount = (src.match(/name: '/g) || []).length;
  if (relCount >= 14) ok('9b 关系清单 ≥14 条', `${relCount} 条`);
  else ko('9b 关系清单', `${relCount} 条`);

  // 4) 探针孤儿被检出（计数 +1）
  const kpBefore = Number(mysql('SELECT COUNT(*) FROM kp_extras;'));
  mysql(`INSERT INTO kp_extras (knowledge_id, extra_type, content) VALUES (${PROBE_KP}, 't003_probe', 'x');`);
  const probeJson = path.join(os.tmpdir(), 't003_probe.json');
  const probe = runScript(['--json', probeJson]);
  let probeReport = null;
  try { probeReport = JSON.parse(fs.readFileSync(probeJson, 'utf8')); } catch (e) { probeReport = null; }
  const probeRel = probeReport && probeReport.relations.find(r => r.name === 'kp_extras→knowledge_points');
  const expected = (rel ? rel.count : 0) + 1;
  if (probeRel && probeRel.count === expected) ok('4 探针孤儿被检出', `count ${probeRel.count}（基线 ${rel ? rel.count : 0}）`);
  else ko('4 探针孤儿检出', `got=${probeRel && probeRel.count} want=${expected}`);

  // 7) 默认运行不写库（行数快照）
  const kpMid = Number(mysql('SELECT COUNT(*) FROM kp_extras;'));
  const ro = runScript([]);
  const kpAfterRo = Number(mysql('SELECT COUNT(*) FROM kp_extras;'));
  if (kpMid === kpAfterRo && (ro.status === 0 || ro.status === 2)) ok('7 默认模式只读', `kp_extras=${kpAfterRo}`);
  else ko('7 默认模式只读', `before=${kpMid} after=${kpAfterRo} status=${ro.status}`);

  // 5) --fix 删除探针
  const fixJson = path.join(os.tmpdir(), 't003_fix.json');
  const fix = runScript(['--fix', '--json', fixJson]);
  const remainProbe = Number(mysql(`SELECT COUNT(*) FROM kp_extras WHERE knowledge_id = ${PROBE_KP};`));
  let fixReport = null;
  try { fixReport = JSON.parse(fs.readFileSync(fixJson, 'utf8')); } catch (e) { fixReport = null; }
  const fixedEntry = fixReport && fixReport.fixed && fixReport.fixed.structure.find(x => x.name === 'kp_extras→knowledge_points');
  const fixOk = remainProbe === 0 && fixedEntry && fixedEntry.deleted >= 1 && (fix.status === 0 || fix.status === 2);
  if (fixOk) ok('5 --fix 自动删除探针孤儿', `deleted=${fixedEntry.deleted} status=${fix.status}`);
  else ko('5 --fix 自动修复', `remain=${remainProbe} entry=${JSON.stringify(fixedEntry)} status=${fix.status}`);

  // 6) --fix 只删孤儿：kp_extras 减少量 = 基线孤儿数（探针不在 kpBefore 内）
  const kpAfterFix = Number(mysql('SELECT COUNT(*) FROM kp_extras;'));
  const expectAfter = kpBefore - (rel ? rel.count : 0);
  if (kpAfterFix === expectAfter) ok('6 --fix 只删孤儿', `kp_extras ${kpBefore}→${kpAfterFix}（删除 ${kpBefore - kpAfterFix}）`);
  else ko('6 --fix 误删', `before=${kpBefore} after=${kpAfterFix} want=${expectAfter}`);

  // 5b) 修复后该关系归零
  const rescan = runScript(['--json', path.join(os.tmpdir(), 't003_after.json')]);
  let afterReport = null;
  try { afterReport = JSON.parse(fs.readFileSync(path.join(os.tmpdir(), 't003_after.json'), 'utf8')); } catch (e) { }
  const afterRel = afterReport && afterReport.relations.find(r => r.name === 'kp_extras→knowledge_points');
  if (afterRel && afterRel.count === 0) ok('5b 修复后该关系归零', 'count=0');
  else ko('5b 修复后计数', `got=${afterRel && afterRel.count} want=0`);

  probeCleanup();

  console.log(`\n  stage14 结果: ${pass} 通过 / ${fail} 失败`);
  if (fail > 0) {
    failures.forEach(f => console.log(`   - ${f.name}: ${f.msg}`));
    process.exit(1);
  }
  process.exit(0);
}

try { main(); }
catch (e) {
  probeCleanup();
  console.error('测试异常:', e.message);
  process.exit(1);
}
