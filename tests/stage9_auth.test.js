// 阶段九：多用户认证 - 集成测试
// v1.2.0-T004：注册/登录/会话、用户数据隔离、跨用户固化复用（零AI调用）、登出
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const MYSQL_BIN = '/Users/junwang/.local/mysql/bin/mysql';
const MYSQL_PW_FILE = '/Users/junwang/.local/mysql/.root_pw';
const SOCK = '/Users/junwang/.local/run/mysql.sock';
const API = 'http://127.0.0.1:8088';

let pass = 0, fail = 0, skipped = 0;
const failures = [];

function ok(name, msg='') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({name, msg}); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg='') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const args = ['--socket='+SOCK, '-uroot', '-p'+pw, 'english_learning', '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout;
}

// 带 cookie 的 HTTP 客户端；返回体中 setCookie 为 eng_session 值
function api(method, p, body, cookie) {
  return new Promise((resolve, reject) => {
    const u = new URL(p, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json' };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    if (cookie) headers['Cookie'] = 'eng_session=' + cookie;
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers, timeout: 90000
    }, res => {
      let chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        const sc = res.headers['set-cookie'];
        let setCookie = null;
        if (sc) {
          const m = /eng_session=([a-f0-9]*)/.exec(Array.isArray(sc) ? sc[0] : sc);
          if (m) setCookie = m[1];
        }
        try { resolve({ status: res.statusCode, json: JSON.parse(text), text, setCookie }); }
        catch { resolve({ status: res.statusCode, json: null, text, setCookie }); }
      });
    });
    req.on('error', reject);
    if (data) req.write(data);
    req.end();
  });
}

function cleanup() {
  // 测试用户（t9user_%）名下知识点的全部子表 / 学情 / AI任务
  runMysql("DELETE ul FROM user_learning ul JOIN knowledge_points kp ON ul.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE u.username LIKE 't9user_%';");
  runMysql("DELETE t FROM kp_definitions t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE u.username LIKE 't9user_%';");
  runMysql("DELETE t FROM kp_collocations t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE u.username LIKE 't9user_%';");
  runMysql("DELETE t FROM kp_synonyms t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE u.username LIKE 't9user_%';");
  runMysql("DELETE t FROM kp_antonyms t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE u.username LIKE 't9user_%';");
  runMysql("DELETE t FROM kp_extras t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE u.username LIKE 't9user_%';");
  runMysql("DELETE a FROM ai_tasks a JOIN knowledge_points kp ON a.ref_id=kp.id JOIN users u ON kp.user_id=u.id WHERE u.username LIKE 't9user_%';");
  runMysql("DELETE kp FROM knowledge_points kp JOIN users u ON kp.user_id=u.id WHERE u.username LIKE 't9user_%';");
  runMysql("DELETE FROM user_sessions WHERE user_id IN (SELECT id FROM users WHERE username LIKE 't9user_%');");
  runMysql("DELETE FROM users WHERE username LIKE 't9user_%';");
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// AI 偶发抖动容忍：固化最多尝试 3 次（"AI JSON not object/parse failed" 时知识点仍未固化，重试安全）
async function solidifyWithRetry(kpId, cookie) {
  let last = null;
  for (let i = 0; i < 3; i++) {
    last = await api('POST', `/api/kp/${kpId}/solidify`, null, cookie);
    if (last.json?.ok) return { r: last, tries: i + 1 };
    if (!/JSON|parse|AI/i.test(last.json?.error || '')) return { r: last, tries: i + 1 };
    await sleep(3000);
  }
  return { r: last, tries: 3 };
}

async function main() {
  console.log('=== 阶段九：多用户认证（v1.2.0-T004）集成测试 ===\n');

  try { cleanup(); } catch(e) { console.log('预清理跳过:', e.message); }

  // ---- 0. 游客默认身份 ----
  console.log('[0] 游客默认身份');
  const guestMe = await api('GET', '/api/auth/me');
  if (guestMe.json?.guest === true && guestMe.json.user_id === 1) ok('无 Cookie 时身份为游客 user_id=1');
  else ko('无 Cookie 时身份为游客', JSON.stringify(guestMe.json));

  const junkMe = await api('GET', '/api/auth/me', null, 'deadbeef0123456789abcdef');
  if (junkMe.json?.guest === true) ok('无效 Cookie 回退为游客');
  else ko('无效 Cookie 回退为游客', JSON.stringify(junkMe.json));

  // ---- 1. 注册校验 ----
  console.log('\n[1] 注册校验');
  const bad1 = await api('POST', '/api/auth/register', { username: 'ab', password: 'test123456' });
  if (bad1.status === 400) ok('用户名短于3字符被拒');
  else ko('用户名短于3字符被拒', `status=${bad1.status}`);

  const bad2 = await api('POST', '/api/auth/register', { username: 't9user_x', password: '12345' });
  if (bad2.status === 400) ok('密码短于6位被拒');
  else ko('密码短于6位被拒', `status=${bad2.status}`);

  const bad3 = await api('POST', '/api/auth/register', { username: 'bad$name', password: 'test123456' });
  if (bad3.status === 400) ok('用户名含非法字符被拒');
  else ko('用户名含非法字符被拒', `status=${bad3.status}`);

  // ---- 2. 注册成功与重名 ----
  console.log('\n[2] 注册成功与重名');
  const regA = await api('POST', '/api/auth/register',
    { username: 't9user_a', password: 'test123456', display_name: '用户A' });
  if (regA.json?.ok && regA.json.user_id > 1) ok('用户A注册成功', `user_id=${regA.json.user_id}`);
  else ko('用户A注册成功', regA.text);

  const regDup = await api('POST', '/api/auth/register',
    { username: 't9user_a', password: 'test123456' });
  if (regDup.status === 409) ok('重名注册返回409');
  else ko('重名注册返回409', `status=${regDup.status}`);

  const regB = await api('POST', '/api/auth/register',
    { username: 't9user_b', password: 'test654321' });
  if (regB.json?.ok) ok('用户B注册成功', `user_id=${regB.json.user_id}`);
  else ko('用户B注册成功', regB.text);

  // ---- 3. 登录 ----
  console.log('\n[3] 登录');
  const wrong = await api('POST', '/api/auth/login',
    { username: 't9user_a', password: 'wrongpwd' });
  if (wrong.status === 401) ok('错误密码返回401');
  else ko('错误密码返回401', `status=${wrong.status}`);

  const noUser = await api('POST', '/api/auth/login',
    { username: 'nobody_here', password: 'test123456' });
  if (noUser.status === 401) ok('不存在用户返回401');
  else ko('不存在用户返回401', `status=${noUser.status}`);

  const loginA = await api('POST', '/api/auth/login',
    { username: 't9user_a', password: 'test123456' });
  if (loginA.json?.ok && loginA.setCookie && loginA.setCookie.length === 64)
    ok('用户A登录成功并下发32字节token', 'cookie 64hex');
  else ko('用户A登录成功', loginA.text);
  const cookieA = loginA.setCookie;

  const loginB = await api('POST', '/api/auth/login',
    { username: 't9user_b', password: 'test654321' });
  if (loginB.json?.ok && loginB.setCookie) ok('用户B登录成功');
  else ko('用户B登录成功', loginB.text);
  const cookieB = loginB.setCookie;

  const meA = await api('GET', '/api/auth/me', null, cookieA);
  if (meA.json?.guest === false && meA.json.username === 't9user_a'
      && meA.json.display_name === '用户A') ok('/api/auth/me 返回用户A信息');
  else ko('/api/auth/me 返回用户A信息', JSON.stringify(meA.json));

  // ---- 4. 用户数据隔离 ----
  console.log('\n[4] 用户数据隔离');
  const guestCount = runMysql(
    "SELECT COUNT(*) AS c FROM knowledge_points WHERE user_id=1 AND is_solidified=1;"
  ).trim().split('\n').pop();
  const aList0 = await api('GET', '/api/kp?size=5', null, cookieA);
  if (Array.isArray(aList0.json?.data) && aList0.json.data.length === 0)
    ok('用户A看不到游客的知识点（初始为空）');
  else ko('用户A看不到游客的知识点', JSON.stringify(aList0.json?.data?.length));

  // 用户A录入知识点
  const kpA = await api('POST', '/api/kp',
    { user_input_content: 't9cacheword' }, cookieA);
  const kpAId = kpA.json?.id;
  if (kpAId) ok('用户A录入知识点', `id=${kpAId}`);
  else { ko('用户A录入知识点', kpA.text); process.exit(1); }

  const ownerRow = runMysql(
    `SELECT user_id FROM knowledge_points WHERE id=${kpAId};`
  ).trim().split('\n').pop();
  if (ownerRow === String(meA.json.user_id)) ok('知识点归属用户A');
  else ko('知识点归属用户A', ownerRow);

  // 游客看不到该知识点
  const guestList = await api('GET', '/api/kp?size=500');
  const guestSees = (guestList.json?.data || []).some(k => k.id === kpAId);
  if (!guestSees) ok('游客列表中无用户A的知识点');
  else ko('游客列表中无用户A的知识点', 'leak!');

  // ---- 5. 用户A正常AI固化（建立跨用户复用的源）----
  console.log('\n[5] 用户A固化（AI 路径）');
  const solidAResult = await solidifyWithRetry(kpAId, cookieA);
  const solidA = solidAResult.r;
  if (solidA.json?.ok) ok('用户A固化成功（调用AI）', solidAResult.tries > 1 ? `第${solidAResult.tries}次尝试` : '');
  else ko('用户A固化成功', solidA.text);

  const srcTask = runMysql(
    `SELECT result FROM ai_tasks WHERE ref_id=${kpAId} ORDER BY id DESC LIMIT 1;`
  ).trim().split('\n').pop();
  if (!srcTask.includes('cross-user-cache')) ok('用户A的固化任务为正常AI路径');
  else ko('用户A的固化任务为正常AI路径', srcTask);

  // 用户A学情独立
  const statsA = await api('GET', '/api/learn/stats', null, cookieA);
  // A 的学情惰性创建前可能为0，触发 getOrCreate
  await api('GET', `/api/learn/${kpAId}`, null, cookieA);
  const statsA2 = await api('GET', '/api/learn/stats', null, cookieA);
  if (statsA2.json.total >= 1) ok('用户A学情独立创建', `total=${statsA2.json.total}`);
  else ko('用户A学情独立创建', JSON.stringify(statsA2.json));

  // ---- 6. 跨用户固化复用（核心：零AI调用）----
  console.log('\n[6] 用户B固化同词条 → 跨用户复用（零AI调用）');
  const kpB = await api('POST', '/api/kp',
    { user_input_content: 't9cacheword' }, cookieB);
  const kpBId = kpB.json?.id;
  if (kpBId && kpBId !== kpAId) ok('用户B录入同词获得独立知识点', `id=${kpBId}`);
  else ko('用户B录入同词获得独立知识点', kpB.text);

  const solidB = await api('POST', `/api/kp/${kpBId}/solidify`, null, cookieB);
  if (solidB.json?.ok && solidB.json.id === kpBId) ok('用户B固化成功且保留自己的知识点ID');
  else ko('用户B固化成功且保留ID', solidB.text);

  // ai_tasks 标记 cross-user-cache
  const bTask = runMysql(
    `SELECT result FROM ai_tasks WHERE ref_id=${kpBId} ORDER BY id DESC LIMIT 1;`
  ).trim().split('\n').pop();
  if (bTask.includes('cross-user-cache') && bTask.includes(String(kpAId)))
    ok('用户B任务标记 source=cross-user-cache 指向用户A知识点');
  else ko('用户B任务标记 cross-user-cache', bTask || '(empty)');

  // 用户B的知识点仍归属自己且已固化
  const bKpInfo = await api('GET', `/api/kp/${kpBId}`, null, cookieB);
  if (bKpInfo.json?.is_solidified === 1 && bKpInfo.json.standard_content === 't9cacheword')
    ok('用户B知识点已固化且归属不变');
  else ko('用户B知识点归属', JSON.stringify(bKpInfo.json));

  // 子表内容与源一致
  const aKpInfo = await api('GET', `/api/kp/${kpAId}`, null, cookieA);
  const fields = ['definitions', 'collocations', 'synonyms', 'antonyms'];
  let allMatch = true, mismatch = '';
  for (const f of fields) {
    const na = (aKpInfo.json?.[f] || []).length;
    const nb = (bKpInfo.json?.[f] || []).length;
    if (na !== nb) { allMatch = false; mismatch = `${f}: A=${na} B=${nb}`; break; }
  }
  const defNonEmpty = (bKpInfo.json?.definitions || []).length > 0;
  if (allMatch && defNonEmpty) ok('用户B子表数量与源一致', fields.map(f => `${f}=${(bKpInfo.json[f]||[]).length}`).join(', '));
  else ko('用户B子表与源一致', allMatch ? 'definitions 为空' : mismatch);

  if (bKpInfo.json.difficulty === aKpInfo.json.difficulty
      && bKpInfo.json.master_require === aKpInfo.json.master_require)
    ok('difficulty / master_require 同步一致');
  else ko('difficulty/master 一致',
    `A=${aKpInfo.json.difficulty}/${aKpInfo.json.master_require} B=${bKpInfo.json.difficulty}/${bKpInfo.json.master_require}`);

  // 用户B学情独立
  await api('GET', `/api/learn/${kpBId}`, null, cookieB);
  const statsB = await api('GET', '/api/learn/stats', null, cookieB);
  if (statsB.json.total >= 1) ok('用户B学情独立创建', `total=${statsB.json.total}`);
  else ko('用户B学情独立创建', JSON.stringify(statsB.json));

  // 用户A不受用户B操作影响
  const statsAAfter = await api('GET', '/api/learn/stats', null, cookieA);
  if (statsAAfter.json.total === statsA2.json.total) ok('用户A学情未被用户B操作影响');
  else ko('用户A学情未被影响', `${statsA2.json.total} → ${statsAAfter.json.total}`);

  // ---- 7. 登出 ----
  console.log('\n[7] 登出');
  const logout = await api('POST', '/api/auth/logout', null, cookieA);
  if (logout.json?.ok && logout.setCookie === '') ok('登出成功并清空 Cookie（Max-Age=0）');
  else ko('登出清空Cookie', logout.text);

  const afterLogout = await api('GET', '/api/auth/me');
  if (afterLogout.json?.guest === true) ok('登出后恢复游客身份');
  else ko('登出后恢复游客身份', JSON.stringify(afterLogout.json));

  // ---- 8. 前端资源 ----
  console.log('\n[8] 前端资源');
  const loginPage = await api('GET', '/pages/login.html');
  if (loginPage.status === 200 && loginPage.text.includes('/api/auth/register'))
    ok('login.html 可访问且含注册逻辑');
  else ko('login.html 可访问', `status=${loginPage.status}`);

  const navJs = await api('GET', '/static/auth-nav.js');
  if (navJs.status === 200 && navJs.text.includes('/api/auth/me')) ok('auth-nav.js 可访问');
  else ko('auth-nav.js 可访问', `status=${navJs.status}`);

  const pages = ['index.html', 'pages/kp-entry.html', 'pages/kp-detail.html',
    'pages/wordbook.html', 'pages/review.html', 'pages/articles.html',
    'pages/learning-center.html'];
  let navCount = 0;
  for (const pg of pages) {
    if (fs.readFileSync(path.join(ROOT, 'frontend', pg), 'utf8').includes('auth-nav.js')) navCount++;
  }
  if (navCount === pages.length) ok(`全部 ${pages.length} 个页面引入 auth-nav.js`);
  else ko('页面引入 auth-nav.js', `${navCount}/${pages.length}`);

  // ---- 9. 用户名含点 + 密码含空格 ----
  console.log('\n[9] 用户名含点、密码含空格（含首尾空格，空格为有效字符）');
  const regDot = await api('POST', '/api/auth/register',
    { username: 't9user.dot', password: '  my secret  ' });
  if (regDot.json?.ok) ok('用户名含点注册成功');
  else ko('用户名含点注册成功', regDot.text);

  const loginSpace = await api('POST', '/api/auth/login',
    { username: 't9user.dot', password: '  my secret  ' });
  if (loginSpace.json?.ok) ok('带首尾空格的密码登录成功');
  else ko('空格密码登录', loginSpace.text);
  const cookieDot = loginSpace.setCookie;

  const loginWrongSpace = await api('POST', '/api/auth/login',
    { username: 't9user.dot', password: 'my secret' });
  if (loginWrongSpace.status === 401) ok('去掉首尾空格的密码401（空格参与校验）');
  else ko('空格参与校验', `status=${loginWrongSpace.status}`);

  // ---- 10. 一键批量固化路径的跨用户复用 ----
  console.log('\n[10] 一键批量固化路径同样命中跨用户缓存');
  const kpBatch = await api('POST', '/api/kp',
    { user_input_content: 'abandon' }, cookieDot);
  const kpBatchId = kpBatch.json?.id;
  if (!kpBatchId) { ko('批量路径录入', kpBatch.text); }
  else
  {
    const t0 = Date.now();
    const batch = await api('POST', '/api/kp/solidify', null, cookieDot);
    const ms = Date.now() - t0;
    if (batch.json?.success === 1 && batch.json?.failed === 0 && ms < 5000)
      ok('批量固化零AI、即时完成', `${ms}ms`);
    else ko('批量固化即时完成', `${batch.text} ${ms}ms`);

    // 运行时查找游客侧 abandon 源（回归中 e2e 等套件可能重建该词，id 不固定）
    const srcRows = runMysql(
      "SELECT kp2.id FROM knowledge_points kp2 WHERE kp2.user_id=1 AND kp2.standard_content='abandon' "
      + "AND kp2.is_solidified=1 AND (SELECT COUNT(*) FROM kp_definitions d2 WHERE d2.knowledge_id=kp2.id)>0 "
      + "LIMIT 1;"
    ).trim().split('\n');
    const srcId = srcRows.length > 1 ? srcRows[1].trim() : '';
    if (!srcId)
    {
      skip('批量路径内容与源一致', '当前无游客 abandon 源');
    }
    else
    {
      const identical = runMysql(
        `SELECT (SELECT GROUP_CONCAT(meaning ORDER BY display_order) FROM kp_definitions WHERE knowledge_id=${srcId})`
        + ` = (SELECT GROUP_CONCAT(meaning ORDER BY display_order) FROM kp_definitions WHERE knowledge_id=${kpBatchId}) AS same;`
      ).trim().split('\n').pop();
      if (identical === '1') ok('批量路径内容与源字节一致', `src=${srcId}`);
      else ko('批量路径内容一致', identical);
    }

    const bTask = runMysql(
      `SELECT result FROM ai_tasks WHERE ref_id=${kpBatchId} ORDER BY id DESC LIMIT 1;`
    ).trim().split('\n').pop();
    if (bTask.includes('cross-user-cache')) ok('批量路径标记 cross-user-cache');
    else ko('批量路径标记', bTask);
  }

  // ---- 11. 直接按 ID 访问他人知识点 → 404 ----
  console.log('\n[11] 越权防护：按 ID 访问他人知识点');
  const otherDetail = await api('GET', `/api/kp/${kpAId}`, null, cookieB);
  if (otherDetail.status === 404) ok('用户B访问用户A的知识点详情返回404');
  else ko('他人知识点404', `status=${otherDetail.status}`);

  const guestKpId = runMysql(
    "SELECT id FROM knowledge_points WHERE user_id=1 AND is_solidified=1 LIMIT 1;"
  ).trim().split('\n').pop();
  const guestDetail = await api('GET', `/api/kp/${guestKpId}`, null, cookieB);
  if (guestDetail.status === 404) ok('登录用户访问游客的知识点详情返回404');
  else ko('游客知识点404', `status=${guestDetail.status}`);

  // ---- 汇总 ----
  console.log(`\n=== 结果：${pass} 通过 / ${fail} 失败 / ${skipped} 跳过 ===`);
  if (failures.length) {
    console.log('\n失败项：');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
  }

  try { cleanup(); } catch(e) { console.log('清理失败:', e.message); }

  process.exit(fail === 0 ? 0 : 1);
}

main().catch(e => {
  console.error(e);
  try { cleanup(); } catch(_) {}
  process.exit(1);
});
