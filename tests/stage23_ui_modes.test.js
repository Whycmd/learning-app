// 阶段二十三：界面模式（功能模式 / 计划模式）（v1.5.0-T004）
// 覆盖：users.ui_mode 列与默认值、迁移脚本可重复执行、
//       /api/auth/me 返回 ui_mode、/api/auth/profile 切换与校验、
//       前端静态结构（模式胶囊 / 双模式显隐 / 五入口卡）、深链可达
const fs = require('fs');
const path = require('path');
const http = require('http');
const { spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT } = require('./_env');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

const ROOT = path.resolve(__dirname, '..');

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const r = spawnSync(MYSQL_BIN,
    ['--socket=' + SOCK, '-uroot', '-p' + pw, '--default-character-set=utf8mb4', '-N', DB_NAME, '-e', sql],
    { encoding: 'utf8', timeout: 30000 });
  if (r.status !== 0) throw new Error((r.stderr || r.stdout || '').trim());
  return r.stdout.trim();
}

function request(method, urlPath, body, cookie) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    if (cookie) headers['Cookie'] = 'eng_session=' + cookie;
    const req = http.request({ host: API_HOST, port: API_PORT, method, path: urlPath, headers, timeout: 60000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(text); } catch (_) {}
        const sc = res.headers['set-cookie'];
        let setCookie = null;
        if (sc) {
          const m = /eng_session=([a-f0-9]*)/.exec(Array.isArray(sc) ? sc[0] : sc);
          if (m) setCookie = m[1];
        }
        resolve({ status: res.statusCode, json, text, setCookie });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

function readFile(rel) {
  return fs.readFileSync(path.join(ROOT, rel), 'utf8');
}

function cleanupTestUser() {
  try {
    runMysql("DELETE FROM user_sessions WHERE user_id IN (SELECT id FROM users WHERE username LIKE 't23user_%');");
    runMysql("DELETE FROM users WHERE username LIKE 't23user_%';");
  } catch (e) { /* 忽略 */ }
}

async function main() {
  console.log('=== 阶段二十三：界面模式（v1.5.0-T004）测试 ===\n');

  // ---- 1. 数据库列与默认值 ----
  console.log('[1] users.ui_mode 列');
  try {
    const row = runMysql(
      "SELECT COLUMN_NAME, COLUMN_DEFAULT, IS_NULLABLE FROM information_schema.COLUMNS " +
      "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='users' AND COLUMN_NAME='ui_mode';"
    );
    if (row.includes('ui_mode') && row.includes('function')) ok('ui_mode 列存在且默认 function', row.replace(/\t/g, ' '));
    else ko('ui_mode 列存在且默认 function', row || '未找到列');
    if (/\tNO$|\tNO\t/.test(row) || !row.includes('YES')) ok('ui_mode 为非空列');
    else ko('ui_mode 为非空列', row);
  } catch (e) { ko('ui_mode 列检查', e.message); }

  // ---- 2. 迁移脚本可重复执行 ----
  console.log('\n[2] 迁移脚本幂等');
  try {
    const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
    const r = spawnSync(MYSQL_BIN,
      ['--socket=' + SOCK, '-uroot', '-p' + pw, '--default-character-set=utf8mb4', DB_NAME],
      { input: readFile('scripts/sql/13_ui_mode.sql'), encoding: 'utf8', timeout: 30000 });
    if (r.status === 0) ok('13_ui_mode.sql 重复执行不报错');
    else ko('13_ui_mode.sql 重复执行不报错', (r.stderr || r.stdout || '').trim());
  } catch (e) { ko('迁移脚本重复执行', e.message); }

  // ---- 3. /api/auth/me 返回 ui_mode ----
  console.log('\n[3] /api/auth/me');
  const guest = await request('GET', '/api/auth/me');
  if (guest.json && guest.json.ui_mode === 'function') ok('游客 ui_mode = function');
  else ko('游客 ui_mode = function', guest.text.slice(0, 120));

  cleanupTestUser();
  const username = 't23user_' + Date.now().toString(36);
  const reg = await request('POST', '/api/auth/register', { username, password: 'test123456' });
  if (!reg.json || !reg.json.ok) { ko('测试用户注册', reg.text.slice(0, 150)); report(); return; }
  const login = await request('POST', '/api/auth/login', { username, password: 'test123456' });
  const cookie = login.setCookie;
  if (!cookie) { ko('测试用户登录', login.text.slice(0, 150)); report(); return; }
  ok('测试用户就绪', username);

  const me1 = await request('GET', '/api/auth/me', null, cookie);
  if (me1.json && me1.json.ui_mode === 'function') ok('新用户默认 ui_mode = function');
  else ko('新用户默认 ui_mode = function', me1.text.slice(0, 120));

  // ---- 4. /api/auth/profile 切换与校验 ----
  console.log('\n[4] /api/auth/profile');
  const toPlan = await request('PUT', '/api/auth/profile', { ui_mode: 'plan' }, cookie);
  if (toPlan.json && toPlan.json.ok && toPlan.json.ui_mode === 'plan') ok('可切换为 plan 并回显');
  else ko('可切换为 plan 并回显', toPlan.text.slice(0, 150));

  const me2 = await request('GET', '/api/auth/me', null, cookie);
  if (me2.json && me2.json.ui_mode === 'plan') ok('切换后 /api/auth/me 返回 plan');
  else ko('切换后 /api/auth/me 返回 plan', me2.text.slice(0, 120));

  const bad = await request('PUT', '/api/auth/profile', { ui_mode: 'dark' }, cookie);
  if (bad.status === 400) ok('非法 ui_mode 返回 400', bad.text.slice(0, 80));
  else ko('非法 ui_mode 返回 400', `status=${bad.status} ${bad.text.slice(0, 80)}`);

  const empty = await request('PUT', '/api/auth/profile', {}, cookie);
  if (empty.status === 400) ok('空请求体返回 400');
  else ko('空请求体返回 400', `status=${empty.status}`);

  const onlyName = await request('PUT', '/api/auth/profile', { display_name: '模式测试' }, cookie);
  if (onlyName.json && onlyName.json.ok && onlyName.json.ui_mode === undefined) ok('旧请求体不受影响（不回显 ui_mode）');
  else ko('旧请求体不受影响', onlyName.text.slice(0, 150));

  const me3 = await request('GET', '/api/auth/me', null, cookie);
  if (me3.json && me3.json.ui_mode === 'plan') ok('旧请求体不会重置 ui_mode');
  else ko('旧请求体不会重置 ui_mode', me3.text.slice(0, 120));

  const guestWrite = await request('PUT', '/api/auth/profile', { ui_mode: 'plan' });
  if (guestWrite.status === 401 || guestWrite.status === 403) ok('游客修改界面模式被拒绝', `status=${guestWrite.status}`);
  else ko('游客修改界面模式被拒绝', `status=${guestWrite.status}`);

  // ---- 5. 前端静态结构 ----
  console.log('\n[5] 前端静态结构');
  try {
    const navJs = readFile('frontend/static/nav.js');
    if (navJs.includes("data-ui-mode") && navJs.includes('nc-ui-mode')) ok('nav.js 含模式胶囊与 data-ui-mode');
    else ko('nav.js 含模式胶囊与 data-ui-mode', '缺少关键标记');
    if (navJs.includes('eng_ui_mode') && navJs.includes('/api/auth/profile')) ok('nav.js 含本机兜底与账号同步');
    else ko('nav.js 含本机兜底与账号同步', '缺少关键标记');

    const html = readFile('frontend/index.html');
    if (html.includes('data-ui-mode="function"')) ok('index.html 预设 data-ui-mode="function"');
    else ko('index.html 预设 data-ui-mode="function"', '未找到');
    if (html.includes('plan-only') && html.includes('function-only')) ok('index.html 含 plan-only / function-only 分区');
    else ko('index.html 含 plan-only / function-only 分区', '未找到');
    const reviewLinks = (html.match(/href="\/pages\/review\.html"/g) || []).length;
    const hasFive = reviewLinks >= 2
      && ['kp-entry', 'daily-quiz', 'wordbook'].every(p => html.includes('href="/pages/' + p + '.html"'));
    if (hasFive) ok('功能模式首页五入口（复用 task-item）', `review×${reviewLinks}`);
    else ko('功能模式首页五入口（复用 task-item）', `review×${reviewLinks}`);
    if (html.includes('wing-card') && html.includes('hall__wing function-only')) ok('功能模式首页复用 wing-card 厢房结构');
    else ko('功能模式首页复用 wing-card 厢房结构', '未找到');
    if (html.includes('mode-hint')) ok('index.html 含功能模式提醒卡');
    else ko('index.html 含功能模式提醒卡', '未找到');

    const css = readFile('frontend/static/components.css');
    if (css.includes('[data-ui-mode="function"] .plan-only') && css.includes('[data-ui-mode="plan"] .function-only')) {
      ok('CSS 含双模式显隐规则');
    } else ko('CSS 含双模式显隐规则', '未找到');
    if (css.includes('.ui-mode__trigger') && css.includes('.wing-card[hidden]')) ok('CSS 含模式胶囊与提醒卡显隐');
    else ko('CSS 含模式胶囊与提醒卡显隐', '未找到');
  } catch (e) { ko('前端静态检查', e.message); }

  // ---- 6. 深链可达（功能模式下不 404、不强制跳回） ----
  console.log('\n[6] 深链可达');
  for (const p of ['/pages/learning-center.html', '/pages/articles.html']) {
    const r = await request('GET', p);
    if (r.status === 200) ok(`功能模式下 ${p} 仍返回 200`);
    else ko(`功能模式下 ${p} 仍返回 200`, `status=${r.status}`);
  }

  cleanupTestUser();
  report();
}

function report() {
  console.log(`\n=== 阶段二十三结果：${pass} 通过，${fail} 失败，${skipped} 跳过 ===`);
  if (fail) {
    console.log('失败项：');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`));
  }
  process.exit(fail ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });
