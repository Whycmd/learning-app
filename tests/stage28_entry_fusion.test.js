// 阶段二十八：入口融合验收（v1.5.0-T010 ENTRY-FUSION）
// 覆盖：三处融合页签静态结构、导航单入口、首页五入口与导出小卡、
//       首页 §8.1 #1 三项统计（到期复习 / 当日小测 / 金桂余额）的 API 契约与前端消费、
//       未登录与接口失败时的容错分支
const fs = require('fs');
const path = require('path');
const http = require('http');
const { API_HOST, API_PORT } = require('./_env');
const { token } = require('./_system');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

const ROOT = path.resolve(__dirname, '..');
function readFile(rel) { return fs.readFileSync(path.join(ROOT, rel), 'utf8'); }

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
        resolve({ status: res.statusCode, json, text });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

const isNum = v => typeof v === 'number' && Number.isFinite(v);

async function main() {
  console.log('=== 阶段二十八：入口融合验收（v1.5.0-T010）测试 ===\n');

  // ---- 1. 三处融合页签 ----
  console.log('[1] 三处融合静态结构');
  try {
    const kpEntry = readFile('frontend/pages/kp-entry.html');
    if (kpEntry.includes('data-tab="explain"') && kpEntry.includes('id="panel-explain"') && kpEntry.includes('AI 讲解')) {
      ok('kp-entry 含「AI 讲解」页签与面板');
    } else ko('kp-entry 含「AI 讲解」页签与面板', '缺少关键标记');

    const review = readFile('frontend/pages/review.html');
    if (review.includes('data-tab="practice"') && review.includes('id="panel-practice"') && review.includes('定制练习')) {
      ok('review 含「定制练习」页签与面板');
    } else ko('review 含「定制练习」页签与面板', '缺少关键标记');

    const wordbook = readFile('frontend/pages/wordbook.html');
    if (wordbook.includes('data-lib-tab="shared"') && wordbook.includes('data-lib-tab="private"')) {
      ok('wordbook 共用/私人单词本并列');
    } else ko('wordbook 共用/私人单词本并列', '缺少关键标记');
  } catch (e) { ko('融合页签静态检查', e.message); }

  // ---- 2. 导航单入口 ----
  console.log('\n[2] 导航单入口');
  try {
    const nav = readFile('frontend/static/nav.js');
    const kpMenu = (nav.match(/>知识点<\/a>/g) || []).length;
    if (kpMenu === 1) ok('导航只有单个「知识点」入口');
    else ko('导航只有单个「知识点」入口', `出现 ${kpMenu} 次`);
    if (!nav.includes('/pages/kp-detail.html')) ok('导航不再单列 kp-detail 入口');
    else ko('导航不再单列 kp-detail 入口', '仍存在 kp-detail 菜单链接');
  } catch (e) { ko('导航静态检查', e.message); }

  // ---- 3. 首页五入口与导出小卡 ----
  console.log('\n[3] 首页入口结构');
  let html = '';
  try {
    html = readFile('frontend/index.html');
    const items = (html.match(/^\s*<a class="task-item"/gm) || []).length;
    if (items === 6) ok('功能模式首页六入口 task-item（v1.5.1-T002 加错题本）');
    else ko('功能模式首页六入口 task-item', `实际 ${items} 个`);
    if (html.includes('export-center') && html.includes('id="export-open"')) ok('首页含导出中心小卡');
    else ko('首页含导出中心小卡', '缺少关键标记');
    if (!fs.existsSync(path.join(ROOT, 'frontend/pages/export.html')) && !html.includes('/pages/export.html')) {
      ok('未新增导出独立页（不新增一级入口）');
    } else ko('未新增导出独立页（不新增一级入口）', '存在 export.html');
  } catch (e) { ko('首页结构静态检查', e.message); }

  // ---- 4. 首页 §8.1 #1 统计项前端消费 ----
  console.log('\n[4] 首页统计项前端实现');
  try {
    if (html.includes('id="quiz-flag"')) ok('每日小测入口含动态状态位 quiz-flag');
    else ko('每日小测入口含动态状态位 quiz-flag', '未找到');
    if (!html.includes('十题')) ok('已移除错误文案「十题」');
    else ko('已移除错误文案「十题」', '仍存在');
    if (html.includes('renderQuizStatus') && html.includes('/api/quiz/daily/history')) ok('小测状态读取 /api/quiz/daily/history');
    else ko('小测状态读取 /api/quiz/daily/history', '未找到');
    if (html.includes('coinSuffix') && html.includes('t.balance')) ok('金桂余额读取 /api/treasury 的 balance');
    else ko('金桂余额读取 /api/treasury 的 balance', '未找到');
    if (html.includes("'今日未测'") && html.includes("'继续作答'") && html.includes("'今日已完成'")) {
      ok('小测状态三态文案齐全');
    } else ko('小测状态三态文案齐全', '缺少文案');
    const guard = html.includes('401') && html.includes('403');
    if (guard) ok('小测状态含 401/403 登录态分支');
    else ko('小测状态含 401/403 登录态分支', '未找到');
    if (html.includes('今日到期') && html.includes('due_today')) ok('今日到期复习数读 /api/learn/summary.due_today');
    else ko('今日到期复习数读 /api/learn/summary.due_today', '未找到');
  } catch (e) { ko('首页统计项静态检查', e.message); }

  // ---- 5. API 契约（系统账号） ----
  console.log('\n[5] 统计接口契约');
  const treasury = await request('GET', '/api/treasury', null, token);
  if (treasury.status === 200 && treasury.json && isNum(treasury.json.balance)) {
    ok('GET /api/treasury 返回数值 balance', String(treasury.json.balance));
  } else ko('GET /api/treasury 返回数值 balance', `status=${treasury.status} ${treasury.text.slice(0, 120)}`);

  if (treasury.json && isNum(treasury.json.solidify_count) && typeof treasury.json.current_title === 'string') {
    ok('GET /api/treasury 返回 solidify_count / current_title');
  } else ko('GET /api/treasury 返回 solidify_count / current_title', treasury.text.slice(0, 120));

  const history = await request('GET', '/api/quiz/daily/history', null, token);
  if (history.status === 200 && history.json && isNum(history.json.today_count) && isNum(history.json.today_finished)) {
    ok('GET /api/quiz/daily/history 返回 today_count / today_finished',
      `${history.json.today_count}/${history.json.today_finished}`);
  } else ko('GET /api/quiz/daily/history 返回 today_count / today_finished', `status=${history.status} ${history.text.slice(0, 120)}`);

  const summary = await request('GET', '/api/learn/summary', null, token);
  if (summary.status === 200 && summary.json && isNum(summary.json.to_learn) && isNum(summary.json.due_today)) {
    ok('GET /api/learn/summary 返回 to_learn / due_today', `${summary.json.to_learn}/${summary.json.due_today}`);
  } else ko('GET /api/learn/summary 返回 to_learn / due_today', `status=${summary.status} ${summary.text.slice(0, 120)}`);

  // ---- 6. 未登录容错 ----
  console.log('\n[6] 未登录容错');
  const guestHistory = await request('GET', '/api/quiz/daily/history');
  if (guestHistory.status !== 500) ok('未登录 /api/quiz/daily/history 不返回 500', `status=${guestHistory.status}`);
  else ko('未登录 /api/quiz/daily/history 不返回 500', guestHistory.text.slice(0, 120));
  const guestTreasury = await request('GET', '/api/treasury');
  if (guestTreasury.status !== 500) ok('未登录 /api/treasury 不返回 500', `status=${guestTreasury.status}`);
  else ko('未登录 /api/treasury 不返回 500', guestTreasury.text.slice(0, 120));
  const guestSummary = await request('GET', '/api/learn/summary');
  if (guestSummary.status !== 500) ok('未登录 /api/learn/summary 不返回 500', `status=${guestSummary.status}`);
  else ko('未登录 /api/learn/summary 不返回 500', guestSummary.text.slice(0, 120));

  report();
}

function report() {
  console.log(`\n=== 阶段二十八结果：${pass} 通过，${fail} 失败，${skipped} 跳过 ===`);
  if (fail) {
    console.log('失败项：');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`));
  }
  process.exit(fail ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });
