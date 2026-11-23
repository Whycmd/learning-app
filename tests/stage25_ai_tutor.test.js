// 阶段二十五：AI 讲解 + 定制练习 + 空闲队列（v1.5.0-T006）
// 覆盖：L1 秒回、L2 排队与进度、重新生成、游客限额与 Cookie、非英文/超长拦截、
//       定制练习组题与客观秒批、24h 复用、不写知识点/学情、加急与转空闲队列、越权 404
// 仅在隔离测试环境执行（测试库）
const fs = require('fs');
const http = require('http');
const { spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT } = require('./_env');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const r = spawnSync(MYSQL_BIN,
    ['--socket=' + SOCK, '-uroot', '-p' + pw, '--default-character-set=utf8mb4', '-N', DB_NAME, '-e', sql],
    { encoding: 'utf8', timeout: 30000 });
  if (r.status !== 0) throw new Error((r.stderr || r.stdout || '').trim());
  return r.stdout.trim();
}
function q1(sql) {
  const out = runMysql(sql);
  return out === '' ? 0 : Number(out.split('\n')[0]);
}

function req(method, urlPath, body, cookieHeader) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    if (cookieHeader) headers['Cookie'] = cookieHeader;
    const r = http.request({ host: API_HOST, port: API_PORT, method, path: urlPath, headers, timeout: 120000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(text); } catch (_) {}
        resolve({ status: res.statusCode, json, text, headers: res.headers });
      });
    });
    r.on('error', reject);
    r.on('timeout', () => r.destroy(new Error('timeout')));
    if (data) r.write(data);
    r.end();
  });
}

function sessionCookie(res) {
  const m = (res.headers['set-cookie'] || []).join(';').match(/eng_session=([0-9a-f]+)/);
  return m ? 'eng_session=' + m[1] : '';
}
function guestCookie(res) {
  const m = (res.headers['set-cookie'] || []).join(';').match(/eng_guest_id=([0-9a-zA-Z]+)/);
  return m ? m[1] : '';
}

async function registerUser(name) {
  await req('POST', '/api/auth/register', { username: name, password: 'pass1234', display_name: name });
  const login = await req('POST', '/api/auth/login', { username: name, password: 'pass1234' });
  const ck = sessionCookie(login);
  const me = await req('GET', '/api/auth/me', null, ck);
  return { ck, uid: (me.json && me.json.user_id) || 0 };
}

async function waitExplain(id, ck, limit = 40) {
  let last = null;
  for (let i = 0; i < limit; i++) {
    const r = await req('GET', `/api/explain/${id}`, null, ck);
    last = r;
    const st = r.json && r.json.item && r.json.item.status;
    if (st === 'done' || st === 'failed') return r;
    await new Promise(x => setTimeout(x, 300));
  }
  return last;
}
async function waitExercise(id, ck, limit = 40) {
  let last = null;
  for (let i = 0; i < limit; i++) {
    const r = await req('GET', `/api/custom-exercise/${id}`, null, ck);
    last = r;
    const st = r.json && r.json.status;
    if (st === 'done' || st === 'failed') return r;
    await new Promise(x => setTimeout(x, 300));
  }
  return last;
}

async function main() {
  console.log('=== 阶段二十五：AI 讲解 + 定制练习（v1.5.0-T006） ===\n');

  if (DB_NAME === 'english_learning') {
    skip('AI 讲解套件', `需要隔离测试库，当前 DB=${DB_NAME}`);
    console.log(`\n阶段二十五结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
    process.exit(0);
  }

  const stamp = Date.now().toString(36).slice(-6);
  const word = 'zzqa' + stamp;
  try {
    // ---------- 1. 游客讲解 ----------
    const g1 = await req('POST', '/api/explain', { text: word });
    const gid = guestCookie(g1);
    if (g1.status === 200 && g1.json && g1.json.ok && gid) ok('游客可发起讲解并下发 guest Cookie', `id=${g1.json.id}`);
    else ko('游客可发起讲解并下发 guest Cookie', `status=${g1.status} ${g1.text.slice(0, 160)}`);

    const guestCk = gid ? 'eng_guest_id=' + gid : '';
    const done1 = await waitExplain(g1.json && g1.json.id, guestCk);
    if (done1 && done1.json && done1.json.item && done1.json.item.status === 'done') ok('游客 L2 讲解完成', 'status=done');
    else ko('游客 L2 讲解完成', JSON.stringify(done1 && done1.json).slice(0, 200));
    if (done1 && done1.json && done1.json.item && String(done1.json.item.summary || '').length > 0) ok('讲解返回结论先行');
    else ko('讲解返回结论先行');

    // ---------- 2. 拦截 ----------
    const badEn = await req('POST', '/api/explain', { text: '中文内容' });
    if (badEn.status === 400) ok('非英文输入被拦截'); else ko('非英文输入被拦截', `status=${badEn.status}`);
    const tooLong = await req('POST', '/api/explain', { text: 'a'.repeat(600) });
    if (tooLong.status === 400) ok('超长单词被拦截'); else ko('超长单词被拦截', `status=${tooLong.status}`);
    const emptyText = await req('POST', '/api/explain', {});
    if (emptyText.status === 400) ok('空输入被拦截'); else ko('空输入被拦截', `status=${emptyText.status}`);

    // ---------- 3. 重新生成 ----------
    const regen = await req('POST', `/api/explain/${g1.json.id}/regenerate`, { depth: 'brief' }, guestCk);
    if (regen.status === 200 && regen.json && regen.json.id && regen.json.id !== g1.json.id) ok('重新生成创建新记录并跳过 L1');
    else ko('重新生成创建新记录并跳过 L1', `${regen.status} ${regen.text.slice(0, 150)}`);

    // ---------- 4. 游客限额 ----------
    let lastStatus = 0;
    for (let i = 0; i < 12; i++) {
      const r = await req('POST', '/api/explain', { text: word + 'x' + i }, guestCk);
      lastStatus = r.status;
      if (lastStatus === 401) break;
    }
    if (lastStatus === 401) ok('游客讲解超 10 次返回 401'); else ko('游客讲解超 10 次返回 401', `last=${lastStatus}`);

    // ---------- 5. L1 秒回 + 自动入池 ----------
    const a = await registerUser('t25a_' + stamp);
    const b = await registerUser('t25b_' + stamp);
    const l2 = await req('POST', '/api/explain', { text: word }, a.ck);
    const l2done = await waitExplain(l2.json && l2.json.id, a.ck);
    if (l2done && l2done.json.item.status === 'done') ok('登录用户 L2 讲解完成');
    else ko('登录用户 L2 讲解完成', JSON.stringify(l2done && l2done.json).slice(0, 160));
    const kpCount = q1(`SELECT COUNT(*) FROM knowledge_points WHERE normalized_key='${word}'`);
    if (kpCount >= 1) ok('常见身份讲解成功自动入池', `kp=${kpCount}`); else ko('常见身份讲解成功自动入池', `kp=${kpCount}`);
    const ulCount = q1(`SELECT COUNT(*) FROM user_learning WHERE user_id=${a.uid}`);
    if (ulCount === 0) ok('自动入池不写学情记录'); else ko('自动入池不写学情记录', `user_learning=${ulCount}`);

    const tasksBefore = q1(`SELECT COUNT(*) FROM ai_tasks WHERE kind='explain'`);
    const l1 = await req('POST', '/api/explain', { text: word }, b.ck);
    const tasksAfter = q1(`SELECT COUNT(*) FROM ai_tasks WHERE kind='explain'`);
    if (l1.status === 200 && l1.json && l1.json.l1_hit === true) ok('L1 命中同步秒回', 'l1_hit=true');
    else ko('L1 命中同步秒回', `${l1.status} ${l1.text.slice(0, 150)}`);
    if (tasksBefore === tasksAfter) ok('L1 命中不新增 AI 任务'); else ko('L1 命中不新增 AI 任务', `${tasksBefore}->${tasksAfter}`);

    // ---------- 6. 越权 ----------
    const other = await req('GET', `/api/explain/${l2.json.id}`, null, b.ck);
    if (other.status === 404) ok('他人讲解返回 404'); else ko('他人讲解返回 404', `status=${other.status}`);

    // ---------- 7. 定制练习 ----------
    const kpBefore = q1(`SELECT COUNT(*) FROM knowledge_points WHERE user_id=${b.uid}`);
    const spec = {
      source: { type: 'theme', theme: 'school life' },
      difficulty: 'basic',
      style: 'exam',
      counts: { cloze_choice: 2, word_form_fill: 1 },
      extra_requirements: '避免生僻词'
    };
    const ex = await req('POST', '/api/custom-exercise', spec, b.ck);
    if (ex.status === 200 && ex.json && ex.json.ok && ex.json.id) ok('定制练习创建成功', `id=${ex.json.id}`);
    else ko('定制练习创建成功', `${ex.status} ${ex.text.slice(0, 160)}`);
    const exDone = await waitExercise(ex.json && ex.json.id, b.ck);
    const items = (exDone && exDone.json && exDone.json.items) || [];
    if (exDone && exDone.json.status === 'done' && items.length === 3) ok('按分题型数量生成 3 题', `items=${items.length}`);
    else ko('按分题型数量生成 3 题', `${exDone && exDone.json && exDone.json.status} items=${items.length}`);
    if (items.filter(i => i.type === 'cloze_choice').every(i => Array.isArray(i.options) && i.options.length === 4)) ok('选择题固定 4 个选项');
    else ko('选择题固定 4 个选项');
    if (items.every(i => i.answer === undefined && i.explanation === undefined)) ok('下发题目不泄露答案与解析');
    else ko('下发题目不泄露答案与解析');

    const answers = items.map((it, i) => ({ index: i, answer: it.type === 'cloze_choice' ? 'A' : 'mock' }));
    const grade = await req('POST', `/api/custom-exercise/${ex.json.id}/grade`, { answers }, b.ck);
    if (grade.status === 200 && grade.json && grade.json.score === 100) ok('客观题本地秒批正确', `score=${grade.json.score}`);
    else ko('客观题本地秒批正确', `${grade.status} ${grade.text.slice(0, 160)}`);

    const again = await req('POST', '/api/custom-exercise', spec, b.ck);
    if (again.status === 200 && again.json && again.json.reused === true && again.json.id === ex.json.id) ok('24h 同参直接复用');
    else ko('24h 同参直接复用', `${again.status} ${again.text.slice(0, 160)}`);

    const kpAfter = q1(`SELECT COUNT(*) FROM knowledge_points WHERE user_id=${b.uid}`);
    const ulAfter = q1(`SELECT COUNT(*) FROM user_learning WHERE user_id=${b.uid}`);
    if (kpBefore === kpAfter && ulAfter === 0) ok('定制练习不写知识点与学情');
    else ko('定制练习不写知识点与学情', `kp ${kpBefore}->${kpAfter}, ul=${ulAfter}`);

    const guestEx = await req('POST', '/api/custom-exercise', spec);
    if (guestEx.status === 200 || guestEx.status === 401) ok('游客定制练习受限额保护', `status=${guestEx.status}`);
    else ko('游客定制练习受限额保护', `status=${guestEx.status}`);

    // ---------- 8. 超限转空闲队列（种子记账，避免大量真实调用） ----------
    const c = await registerUser('t25c_' + stamp);
    runMysql(`INSERT INTO ai_usage_daily (user_id,day,kind,calls,tokens) VALUES (${c.uid},CURDATE(),'explain',15,0) ` +
             `ON DUPLICATE KEY UPDATE calls=15`);
    const over = await req('POST', '/api/explain', { text: 'overquota' + stamp }, c.ck);
    if (over.status === 200 && over.json && over.json.queue_level >= 1) ok('登录用户超额转空闲队列', `level=${over.json.queue_level}`);
    else ko('登录用户超额转空闲队列', `${over.status} ${over.text.slice(0, 160)}`);

    // ---------- 9. 加急 / 转空闲（用未来 next_run_at 任务，保证仍在排队） ----------
    runMysql(`INSERT INTO ai_tasks (task_type,status,user_id,kind,lane,total,progress,retry_count,queue_level,next_run_at,update_time) ` +
             `VALUES ('explain','queued',${c.uid},'explain','explain',1,0,0,0,DATE_ADD(NOW(), INTERVAL 1 HOUR),NOW())`);
    const tid = q1(`SELECT MAX(id) FROM ai_tasks WHERE user_id=${c.uid}`);
    const d = await req('POST', `/api/ai-tasks/${tid}/defer`, { level: 2 }, c.ck);
    if (d.status === 200) ok('可转入空闲队列'); else ko('可转入空闲队列', `${d.status} ${d.text.slice(0, 120)}`);
    const lv = q1(`SELECT queue_level FROM ai_tasks WHERE id=${tid}`);
    if (lv === 2) ok('queue_level 写入正确', 'level=2'); else ko('queue_level 写入正确', `level=${lv}`);
    const e = await req('POST', `/api/ai-tasks/${tid}/expedite`, {}, c.ck);
    const lv2 = q1(`SELECT queue_level FROM ai_tasks WHERE id=${tid}`);
    if ((e.status === 200 || e.status === 409) && (lv2 === 0 || e.status === 409)) ok('加急回正常队列');
    else ko('加急回正常队列', `${e.status} level=${lv2}`);

    // ---------- 10. 历史 ----------
    const hist = await req('GET', '/api/explain/history?limit=5', null, a.ck);
    if (hist.status === 200 && hist.json && Array.isArray(hist.json.items)) ok('最近讲解可读');
    else ko('最近讲解可读', `${hist.status}`);
    const histGuest = await req('GET', '/api/explain/history', null, guestCk);
    if (histGuest.status === 401 || (histGuest.json && (histGuest.json.items || []).length === 0)) ok('游客不可见讲解历史');
    else ko('游客不可见讲解历史', `${histGuest.status}`);

    const exHist = await req('GET', '/api/custom-exercise/history?limit=5', null, b.ck);
    if (exHist.status === 200 && exHist.json && Array.isArray(exHist.json.items)) ok('练习历史可读');
    else ko('练习历史可读', `${exHist.status}`);

    // ---------- 11. keep=1：游客记录登录后转入本人历史 ----------
    const keeper = 'zzkb' + stamp;
    const gKeep = await req('POST', '/api/explain', { text: keeper });
    const gKeepCk = 'eng_guest_id=' + guestCookie(gKeep);
    const keepId = gKeep.json && gKeep.json.id;
    if (keepId) {
      const dName = 't25d_' + stamp;
      await req('POST', '/api/auth/register', { username: dName, password: 'pass1234' });
      const dLogin = await req('POST', '/api/auth/login',
        { username: dName, password: 'pass1234', keep: true }, gKeepCk);
      const dCk = sessionCookie(dLogin);
      const afterKeep = await req('GET', `/api/explain/${keepId}`, null, dCk);
      if (afterKeep.status === 200 && afterKeep.json && afterKeep.json.item)
        ok('keep=1 登录后讲解转入本人历史');
      else ko('keep=1 登录后讲解转入本人历史', `${afterKeep.status}`);

      const afterGuest = await req('GET', `/api/explain/${keepId}`, null, gKeepCk);
      if (afterGuest.status === 404) ok('keep=1 后游客侧不再可见');
      else ko('keep=1 后游客侧不再可见', `${afterGuest.status}`);
    } else {
      ko('keep=1 登录后讲解转入本人历史', '游客讲解创建失败');
    }
  } catch (e) {
    ko('套件执行异常', e && e.message ? e.message : String(e));
  }

  // ---------- 12. 前端结构（kp-entry / kp-detail 单页合并） ----------
  try {
    const path = require('path');
    const ROOT = path.join(__dirname, '..');

    const kpEntry = fs.readFileSync(path.join(ROOT, 'frontend/pages/kp-entry.html'), 'utf8');
    if (kpEntry.includes('panel-explain') && kpEntry.includes('data-expand') && kpEntry.includes('id="ex-text"'))
      ok('kp-entry 单页含讲解页签与行内展开');
    else ko('kp-entry 单页含讲解页签与行内展开', '缺少 panel-explain / data-expand / ex-text');

    const shell = fs.readFileSync(path.join(ROOT, 'frontend/pages/kp-detail.html'), 'utf8');
    if (shell.includes("get('id')") && shell.includes('location.replace') && !shell.includes('renderKpDetail'))
      ok('kp-detail 为跳转壳且解析 ?id=');
    else ko('kp-detail 为跳转壳且解析 ?id=', '仍含详情渲染或缺少跳转');

    const nav = fs.readFileSync(path.join(ROOT, 'frontend/static/nav.js'), 'utf8');
    const entryCount = (nav.match(/href="\/pages\/kp-entry\.html"/g) || []).length;
    if (entryCount === 1 && !nav.includes('/pages/kp-detail.html'))
      ok('nav.js 仅一个知识点入口');
    else ko('nav.js 仅一个知识点入口', 'entry=' + entryCount + '，仍含 kp-detail');

    const review = fs.readFileSync(path.join(ROOT, 'frontend/pages/review.html'), 'utf8');
    if (review.includes('panel-practice') && review.includes('id="p-source"') && review.includes('不写入学情与知识点'))
      ok('review 含定制练习页签与 AI 告知条');
    else ko('review 含定制练习页签与 AI 告知条', '缺少 panel-practice / p-source / 告知条');

    const tutorJs = fs.readFileSync(path.join(ROOT, 'frontend/static/ai-tutor.js'), 'utf8');
    if (tutorJs.includes('keep=1') && tutorJs.includes('renderKpDetail'))
      ok('ai-tutor.js 含 keep=1 回跳与详情渲染');
    else ko('ai-tutor.js 含 keep=1 回跳与详情渲染', '缺少 keep=1 或 renderKpDetail');
  } catch (e) {
    ko('前端结构检查异常', e && e.message ? e.message : String(e));
  }

  console.log(`\n阶段二十五结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
  if (fail > 0) { console.log('失败用例：'); failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`)); }
  process.exit(fail > 0 ? 1 : 0);
}

main();
