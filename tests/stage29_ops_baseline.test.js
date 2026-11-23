// 阶段二十九：运维基线验收（v1.5.0-T011 OPS-BASELINE）
// 覆盖：每日 mysqldump 备份（幂等 / 7 天清理 / 手动备份保留）、
//       AI 调用留痕（ai_call_log + 场景标签 + tokens 不臆造）、
//       GET /api/ops/ai-cost 的聚合结构与登录校验
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawnSync } = require('child_process');
const { API_HOST, API_PORT, DB_NAME } = require('./_env');
const { token: systemToken } = require('./_system');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

const ROOT = path.resolve(__dirname, '..');

function request(method, urlPath, body, cookie) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    if (cookie) headers['Cookie'] = cookie;
    const req = http.request({ host: API_HOST, port: API_PORT, method, path: urlPath, headers, timeout: 60000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(text); } catch (_) {}
        resolve({ status: res.statusCode, json, text, headers: res.headers });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

function sessionCookie(res) {
  const raw = res.headers['set-cookie'] || [];
  for (const c of raw) {
    const m = String(c).match(/eng_session=([^;]+)/);
    if (m) return 'eng_session=' + m[1];
  }
  return '';
}

async function registerUser(name) {
  await request('POST', '/api/auth/register', { username: name, password: 'pass1234', display_name: name });
  const login = await request('POST', '/api/auth/login', { username: name, password: 'pass1234' });
  return sessionCookie(login);
}

async function waitExplain(id, ck, limit = 40) {
  let last = null;
  for (let i = 0; i < limit; i++) {
    const r = await request('GET', `/api/explain/${id}`, null, ck);
    last = r;
    const st = r.json && r.json.item && r.json.item.status;
    if (st === 'done' || st === 'failed') return r;
    await new Promise(x => setTimeout(x, 300));
  }
  return last;
}

function runBackup(dir, extra = []) {
  return spawnSync(process.execPath, [path.join(ROOT, 'scripts/backup_daily.js'), '--once', '--dir', dir, ...extra], {
    cwd: ROOT, encoding: 'utf8', timeout: 120000,
  });
}

function listDaily(dir) {
  if (!fs.existsSync(dir)) return [];
  return fs.readdirSync(dir).filter(f => f.startsWith('english_learning_daily_') && f.endsWith('.sql')).sort();
}

async function main() {
  console.log('=== 阶段二十九：运维基线验收（v1.5.0-T011）测试 ===\n');

  // ---------- 1. 每日备份 ----------
  console.log('[1] 每日 mysqldump 备份');
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ops_backup_'));
  try {
    const first = runBackup(tmp);
    const out1 = (first.stdout || '') + (first.stderr || '');
    if (first.status !== 0 && /未找到 mysqldump|未找到 mariadb-dump/.test(out1)) {
      skip('备份脚本执行', '当前环境未安装 mysqldump');
    } else {
      if (first.status === 0) ok('备份脚本执行成功'); else ko('备份脚本执行成功', `exit=${first.status} ${out1.slice(0, 200)}`);

      const files1 = listDaily(tmp);
      if (files1.length === 1) ok('生成 1 个当日自动备份', files1[0]); else ko('生成 1 个当日自动备份', `count=${files1.length}`);
      const f = files1[0] ? path.join(tmp, files1[0]) : '';
      const size = f && fs.existsSync(f) ? fs.statSync(f).size : 0;
      if (size > 1024) ok('备份文件非空', `${(size / 1024).toFixed(1)} KB`); else ko('备份文件非空', `size=${size}`);
      let content = '';
      try { content = fs.readFileSync(f, 'utf8'); } catch (_) {}
      if (content.includes('CREATE TABLE')) ok('备份内容含建表语句'); else ko('备份内容含建表语句', '未找到 CREATE TABLE');

      const second = runBackup(tmp);
      const files2 = listDaily(tmp);
      if (second.status === 0 && files2.length === 1) ok('同日重复执行幂等（不新增文件）');
      else ko('同日重复执行幂等（不新增文件）', `exit=${second.status} count=${files2.length}`);

      // 造 8 天前自动备份 + 1 个手动备份
      const d8 = new Date(); d8.setDate(d8.getDate() - 8);
      const ymd = `${d8.getFullYear()}${String(d8.getMonth() + 1).padStart(2, '0')}${String(d8.getDate()).padStart(2, '0')}`;
      fs.writeFileSync(path.join(tmp, `english_learning_daily_${ymd}_030000.sql`), '-- old\n');
      fs.writeFileSync(path.join(tmp, 'english_learning_20200101_010101.sql'), '-- manual\n');
      const third = runBackup(tmp);
      const left = fs.readdirSync(tmp);
      const oldGone = !left.some(n => n.includes(`daily_${ymd}_`));
      const manualKept = left.includes('english_learning_20200101_010101.sql');
      if (third.status === 0 && oldGone) ok('超过 7 天的自动备份被清理'); else ko('超过 7 天的自动备份被清理', `exit=${third.status} files=${left.length}`);
      if (manualKept) ok('手动备份不被清理'); else ko('手动备份不被清理');
    }
  } catch (e) {
    ko('每日备份测试', e.message);
  } finally {
    try { fs.rmSync(tmp, { recursive: true, force: true }); } catch (_) {}
  }

  // ---------- 2. AI 调用留痕 ----------
  console.log('\n[2] AI 调用留痕');
  const stamp = Date.now().toString(36).slice(-6);
  const word = 'zzops' + stamp;
  let userCk = '';
  try {
    userCk = await registerUser('opsuser' + stamp);
    if (userCk) ok('测试用户注册登录'); else ko('测试用户注册登录', '未获取会话');
    const created = await request('POST', '/api/explain', { text: word }, userCk);
    if (created.status === 200 && created.json && created.json.id) ok('发起一次 AI 讲解', `id=${created.json.id}`);
    else ko('发起一次 AI 讲解', `status=${created.status} ${created.text.slice(0, 160)}`);
    const done = await waitExplain(created.json && created.json.id, userCk);
    const st = done && done.json && done.json.item && done.json.item.status;
    if (st === 'done') ok('讲解完成（AI 调用已发生）'); else ko('讲解完成（AI 调用已发生）', `status=${st}`);

    const cost = await request('GET', '/api/ops/ai-cost?days=1', null, userCk);
    if (cost.status === 200 && cost.json) ok('成本接口登录可访问'); else ko('成本接口登录可访问', `status=${cost.status}`);
    if (cost.json && cost.json.scope === 'self') ok('默认只统计本人数据'); else ko('默认只统计本人数据', `scope=${cost.json && cost.json.scope}`);
    if (cost.json && cost.json.calls >= 1) ok('留痕计数 >= 1', `calls=${cost.json.calls}`); else ko('留痕计数 >= 1', `calls=${cost.json && cost.json.calls}`);
    const feats = (cost.json && cost.json.by_feature) || [];
    const explainRow = feats.find(x => x.feature === 'explain');
    if (explainRow && explainRow.calls >= 1) ok('场景标签 explain 正确落库'); else ko('场景标签 explain 正确落库', JSON.stringify(feats).slice(0, 200));
    if (cost.json && typeof cost.json.tokens_available === 'boolean') ok('tokens_available 字段存在');
    else ko('tokens_available 字段存在');
    if (cost.json && Array.isArray(cost.json.coin_flows)) ok('金桂流水聚合字段存在'); else ko('金桂流水聚合字段存在');

    const anon = await request('GET', '/api/ops/ai-cost?days=1', null, '');
    if (anon.status === 401) ok('未登录访问返回 401'); else ko('未登录访问返回 401', `status=${anon.status}`);

    const allScope = await request('GET', '/api/ops/ai-cost?days=1&scope=all', null, 'eng_session=' + systemToken);
    if (allScope.status === 200 && allScope.json && allScope.json.scope === 'all') ok('系统账号可看全局');
    else ko('系统账号可看全局', `status=${allScope.status} scope=${allScope.json && allScope.json.scope}`);

    const normalAll = await request('GET', '/api/ops/ai-cost?days=1&scope=all', null, userCk);
    if (normalAll.status === 200 && normalAll.json && normalAll.json.scope === 'self') ok('普通用户 scope=all 被降级为本人');
    else ko('普通用户 scope=all 被降级为本人', `scope=${normalAll.json && normalAll.json.scope}`);
  } catch (e) {
    ko('AI 留痕测试', e.message);
  }

  // ---------- 3. 数据库表结构 ----------
  console.log('\n[3] 数据库表结构');
  try {
    const { MYSQL_BIN, MYSQL_PW_FILE, SOCK } = require('./_env');
    const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
    const r = spawnSync(MYSQL_BIN, ['--socket=' + SOCK, '-uroot', '-p' + pw, '-N', DB_NAME,
      '-e', "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='ai_call_log'"], { encoding: 'utf8' });
    const cols = Number((r.stdout || '0').trim());
    if (cols >= 11) ok('ai_call_log 字段完整', `${cols} 列`); else ko('ai_call_log 字段完整', `cols=${cols} ${(r.stderr || '').slice(0, 120)}`);
  } catch (e) {
    ko('数据库表结构检查', e.message);
  }

  console.log(`\n阶段二十九结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
  if (fail) {
    console.log('失败详情:');
    failures.forEach(f => console.log(` - ${f.name}: ${f.msg}`));
  }
  process.exit(fail ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });
