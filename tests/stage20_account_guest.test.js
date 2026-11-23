// stage20 — v1.5.0-T001 ACCOUNT-GUEST：匿名身份、写拦截、文章可见性
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT } = require('./_env');
const { token: SYSTEM_TOKEN } = require('./_system');

const API = `http://${API_HOST}:${API_PORT}`;
const ROOT = path.resolve(__dirname, '..');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const r = spawnSync(MYSQL_BIN, ['--socket=' + SOCK, '-uroot', '-p' + pw,
    '--default-character-set=utf8mb4', DB_NAME, '-e', sql], { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout;
}

function api(method, urlPath, body, cookie) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlPath, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json' };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    if (cookie) headers['Cookie'] = 'eng_session=' + cookie;
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers, timeout: 30000
    }, res => {
      let chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const raw = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(raw); } catch (e) {}
        resolve({ status: res.statusCode, headers: res.headers, raw, json });
      });
    });
    req.on('timeout', () => { req.destroy(new Error('timeout')); });
    req.on('error', reject);
    if (data) req.write(data);
    req.end();
  });
}

async function main() {
  console.log('='.repeat(56));
  console.log('stage20 · ACCOUNT-GUEST 匿名会话');
  console.log('='.repeat(56));

  // ---- 1. 匿名身份 ----
  console.log('[1] 匿名身份');
  const me = await api('GET', '/api/auth/me');
  if (me.json && me.json.user_id === 0 && me.json.guest === true) ok('无 Cookie → user_id=0, guest=true');
  else ko('无 Cookie → user_id=0, guest=true', me.raw);

  const junk = await api('GET', '/api/auth/me', null, 'invalid_token_xxx');
  if (junk.json && junk.json.user_id === 0 && junk.json.guest === true) ok('无效 Cookie → 匿名');
  else ko('无效 Cookie → 匿名', junk.raw);

  // ---- 2. 写接口拦截 ----
  console.log('[2] 未登录写接口 401');
  const writes = [
    ['POST', '/api/kp', { user_input_content: 'guest_kp' }],
    ['POST', '/api/wordbook', { name: 'guest_wb' }],
    ['POST', '/api/quiz/daily', {}],
    ['DELETE', '/api/article/999999', null]
  ];
  for (const [m, p, b] of writes) {
    const r = await api(m, p, b);
    if (r.status === 401) ok(`${m} ${p} → 401`);
    else ko(`${m} ${p} → 401`, `status=${r.status} body=${r.raw.slice(0, 80)}`);
  }

  const kpList = await api('GET', '/api/kp');
  if (kpList.status === 200) ok('未登录 GET /api/kp → 200');
  else ko('未登录 GET /api/kp → 200', `status=${kpList.status}`);

  // 白名单：登录接口不被拦截层挡住
  const badLogin = await api('POST', '/api/auth/login', { username: 'no_such_user', password: 'wrong' });
  if (badLogin.status !== 401 || (badLogin.json && badLogin.json.error !== '请先登录')) ok('login 走白名单（非拦截 401）');
  else ko('login 走白名单', badLogin.raw);

  const logout = await api('POST', '/api/auth/logout');
  if (logout.status === 200) ok('logout 走白名单 → 200');
  else ko('logout 走白名单 → 200', `status=${logout.status}`);

  // ---- 3. 静态度量页 ----
  console.log('[3] 隐私政策 / 用户协议 / 匿名 ID 说明');
  for (const [p, word] of [['/pages/privacy.html', '匿名 ID'], ['/pages/terms.html', '用户协议']]) {
    const r = await api('GET', p);
    if (r.status === 200 && r.raw.includes(word)) ok(`${p} 可访问且含「${word}」`);
    else ko(`${p} 可访问`, `status=${r.status}`);
  }
  const privacy = await api('GET', '/pages/privacy.html');
  if (privacy.raw.includes('eng_anon_key') && privacy.raw.includes('监护人')) ok('隐私政策含匿名 ID 与未成年人声明');
  else ko('隐私政策含匿名 ID 与未成年人声明', '缺少关键字');

  const loginPage = await api('GET', '/pages/login.html');
  if (loginPage.raw.includes('redirect') && loginPage.raw.includes('/pages/privacy.html')) ok('登录页支持回跳并含法律文本入口');
  else ko('登录页支持回跳并含法律文本入口', '缺少关键字');

  const navJs = await api('GET', '/static/nav.js');
  if (navJs.raw.includes('eng_anon_key') && navJs.raw.includes('login.html?redirect=')) ok('nav.js 生成匿名标识并处理 401 回跳');
  else ko('nav.js 生成匿名标识并处理 401 回跳', '缺少关键字');

  // ---- 4. 文章可见性 ----
  console.log('[4] 文章可见性');
  const created = await api('POST', '/api/article',
    { title: 'stage20_private', content: 'This is a private article for stage20.' }, SYSTEM_TOKEN);
  if (created.status !== 200 || !created.json || !created.json.id) {
    ko('系统账号新建文章', created.raw);
  } else {
    ok('系统账号会话可写入（新建文章）', `id=${created.json.id}`);
    const aid = created.json.id;

    const guestDetail = await api('GET', `/api/article/${aid}`);
    if (guestDetail.status === 404) ok('游客读私有文章 → 404');
    else ko('游客读私有文章 → 404', `status=${guestDetail.status}`);

    const guestReport = await api('GET', `/api/article/${aid}/difficulty-report`);
    if (guestReport.status === 404) ok('游客读私有文章难度报告 → 404');
    else ko('游客读私有文章难度报告 → 404', `status=${guestReport.status}`);

    const guestList = await api('GET', '/api/article');
    const listed = guestList.json && (guestList.json.items || guestList.json.articles || guestList.json.data || []);
    const inList = Array.isArray(listed) && listed.some(x => Number(x.id) === Number(aid));
    if (guestList.status === 200 && !inList) ok('游客列表不含他人私有文章');
    else ko('游客列表不含他人私有文章', guestList.raw.slice(0, 120));

    const sysDetail = await api('GET', `/api/article/${aid}`, null, SYSTEM_TOKEN);
    if (sysDetail.status === 200) ok('归属账号可读私有文章');
    else ko('归属账号可读私有文章', `status=${sysDetail.status}`);

    // 他人不可改/删
    const uname = 't20u' + Date.now().toString().slice(-10);
    const reg = await api('POST', '/api/auth/register', { username: uname, password: 'Passw0rd!', display_name: 'T20' });
    const login = reg.status === 200
      ? await api('POST', '/api/auth/login', { username: uname, password: 'Passw0rd!' })
      : { headers: {} };
    const setCookie = login.headers['set-cookie'] || [];
    const m = /eng_session=([^;]+)/.exec(setCookie.join(';'));
    if (reg.status === 200 && m) {
      const userCookie = m[1];
      const put = await api('PUT', `/api/article/${aid}`, { title: 'hacked' }, userCookie);
      if (put.status !== 200) ok('他人不可修改系统账号文章', `status=${put.status}`);
      else ko('他人不可修改系统账号文章', '被修改成功');

      const del = await api('DELETE', `/api/article/${aid}`, null, userCookie);
      if (del.status !== 200) ok('他人不可删除系统账号文章', `status=${del.status}`);
      else ko('他人不可删除系统账号文章', '被删除成功');

      const meUser = await api('GET', '/api/auth/me', null, userCookie);
      if (meUser.json && meUser.json.user_id > 1 && meUser.json.guest === false) ok('登录用户身份正常', `uid=${meUser.json.user_id}`);
      else ko('登录用户身份正常', meUser.raw);

      await api('POST', '/api/auth/deregister', null, userCookie);
    } else {
      ko('注册临时用户', reg.raw);
    }

    // 公共化后游客可读
    runMysql(`UPDATE articles SET visibility='public' WHERE id=${aid};`);
    const pubDetail = await api('GET', `/api/article/${aid}`);
    if (pubDetail.status === 200) ok('public 文章游客可读');
    else ko('public 文章游客可读', `status=${pubDetail.status}`);

    runMysql(`DELETE FROM articles WHERE id=${aid};`);
  }

  // ---- 5. 清理 ----
  const junkUsers = runMysql("SELECT COUNT(*) AS n FROM users WHERE username LIKE 't20user\\_%';");
  console.log('  临时用户残留:', junkUsers.trim().split('\n').pop());

  console.log('\n' + '-'.repeat(56));
  console.log(`通过: ${pass}, 失败: ${fail}, 跳过: ${skipped}`);
  if (failures.length > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`));
  }
  process.exit(fail > 0 ? 1 : 0);
}

main().catch(e => {
  console.error('测试执行错误:', e);
  process.exit(1);
});
