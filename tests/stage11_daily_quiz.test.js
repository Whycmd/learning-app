// 阶段十一：每日小测（v1.2.0-T007）集成测试
// 覆盖 daily_quiz / user_coin 两表、出题/提交/固化/历史 4 个接口、前端页面元素
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
  const args = ['--socket='+SOCK, '-uroot', '-p'+pw, 'english_learning', '-N', '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout.trim();
}

function api(method, urlPath, body, timeoutMs=30000) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlPath, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json',
                      'Content-Length': data ? Buffer.byteLength(data) : 0 };
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers, timeout: timeoutMs
    }, res => {
      let chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        try { resolve({ status: res.statusCode, json: JSON.parse(text), text }); }
        catch { resolve({ status: res.statusCode, json: null, text }); }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

// 清理测试数据：重置 user1 的词币账户（与 stage12 共用 user_id=1）
function resetTestUser() {
  try { runMysql("DELETE FROM user_coin_txn WHERE user_id=1"); } catch(e) {}
  try { runMysql("DELETE FROM user_coin WHERE user_id=1"); } catch(e) {}
}

async function main() {
  console.log('=== 阶段十一：每日小测（v1.2.0-T007）集成测试 ===\n');

  resetTestUser();

  // ---- 0. 健康检查 ----
  console.log('[0] 前置检查');
  try {
    const r = await api('GET', '/api/health');
    if (r.json && r.json.status === 'ok') ok('后端健康检查');
    else ko('后端健康检查', r.text);
  } catch(e) { ko('后端健康检查', e.message); }

  // ---- 1. 两表存在 ----
  console.log('\n[1] 数据表检查');
  try {
    const tables = runMysql('SHOW TABLES').split('\n');
    if (tables.includes('daily_quiz')) ok('daily_quiz 表存在');
    else ko('daily_quiz 表存在', '未找到');
    if (tables.includes('user_coin_txn')) ok('user_coin_txn 表存在');
    else ko('user_coin_txn 表存在', '未找到');
    if (tables.includes('user_coin')) ok('user_coin 表存在（词币）');
    else ko('user_coin 表存在', '未找到');
  } catch(e) { ko('数据表检查', e.message); }

  // ---- 2. 历史接口 ----
  console.log('\n[2] 历史接口');
  let beforeCount = 0;
  try {
    const r = await api('GET', '/api/quiz/daily/history');
    if (r.status === 200 && r.json) ok('历史接口返回200');
    else ko('历史接口返回200', r.text);
    if (r.json && Array.isArray(r.json.data)) ok('data 为数组');
    else ko('data 为数组', JSON.stringify(r.json));
    if (r.json && typeof r.json.today_count === 'number') {
      ok('today_count 为数字');
      beforeCount = r.json.today_count;
    } else ko('today_count 为数字', JSON.stringify(r.json));
    if (r.json && typeof r.json.today_finished === 'number') ok('today_finished 为数字');
    else ko('today_finished 为数字', JSON.stringify(r.json));
  } catch(e) { ko('历史接口', e.message); }

  // ---- 3. 出题接口（AI，允许一次重试） ----
  console.log('\n[3] AI 出题');
  let start = null;
  try {
    start = await api('POST', '/api/quiz/daily', null, 120000);
    if (!start.json || !start.json.quiz_id) {
      console.log('  首次出题失败，重试一次…');
      start = await api('POST', '/api/quiz/daily', null, 120000);
    }
    const d = start.json;
    if (start.status === 200 && d && d.quiz_id) ok('出题返回200与 quiz_id', 'id=' + d.quiz_id);
    else { ko('出题返回200与 quiz_id', start.text); throw new Error('start failed'); }

    if (d.attempt_no === beforeCount + 1) ok('attempt_no 递增', '=' + d.attempt_no);
    else ko('attempt_no 递增', `got ${d.attempt_no}, expect ${beforeCount+1}`);
    if (typeof d.quiz_date === 'string' && d.quiz_date.length === 10) ok('quiz_date 格式正确');
    else ko('quiz_date 格式正确', d.quiz_date);

    const qs = d.questions;
    if (Array.isArray(qs) && qs.length === 5) ok('questions 为 5 题');
    else { ko('questions 为 5 题', JSON.stringify(qs)); throw new Error('bad questions'); }

    const cats = qs.map(q => q.category);
    if (cats.slice(0,3).every(c => c === 'solidified')
        && cats[3] === 'familiar_meaning' && cats[4] === 'new_kp')
      ok('3+1+1 结构正确');
    else ko('3+1+1 结构正确', cats.join(','));

    let structureOk = true;
    qs.forEach((q, i) => {
      if (q.no !== i + 1) structureOk = false;
      if (q.answer !== undefined) structureOk = false; // 响应必须剥离答案
      if (typeof q.question !== 'string' || !q.question.includes('____')) structureOk = false;
      if (q.type === 'cloze_choice') {
        if (!Array.isArray(q.options) || q.options.length !== 4) structureOk = false;
      } else if (q.type === 'word_form_fill') {
        if (q.options !== undefined) structureOk = false;
      } else structureOk = false;
    });
    if (structureOk) ok('题号/挖空/题型/选项结构正确且无答案泄露');
    else ko('题号/挖空/题型/选项结构正确且无答案泄露', JSON.stringify(qs));

    let extOk = true;
    [qs[3], qs[4]].forEach(q => {
      if (!q.word || !q.meaning) extOk = false;
      if (q.example && q.example.includes('____')) extOk = false;
    });
    if (extOk) ok('第4/5题含 word/meaning，example 无退化');
    else ko('第4/5题含 word/meaning，example 无退化', JSON.stringify([qs[3],qs[4]]));
  } catch(e) {
    if (e.message === 'start failed' || e.message === 'bad questions') {
      // 已计数
    } else ko('AI 出题', e.message);
  }

  // ---- 4. 提交批改 ----
  console.log('\n[4] 提交批改');
  const quizId = start && start.json && start.json.quiz_id;
  let submit = null;
  if (quizId) {
    try {
      const stored = JSON.parse(runMysql(
        `SELECT questions FROM daily_quiz WHERE id=${quizId}`));
      // 前 3 题答对，后 2 题答错 → 期望 60 分
      const answers = stored.map(q => ({
        no: q.no,
        user_answer: q.no <= 3 ? q.answer : '__wrong__'
      }));
      submit = await api('POST', '/api/quiz/daily/submit', { quiz_id: quizId, answers });
      const d = submit.json;
      if (submit.status === 200 && d) ok('提交返回200');
      else { ko('提交返回200', submit.text); throw new Error('submit failed'); }
      if (d.score === 60) ok('得分 60（3/5）');
      else ko('得分 60（3/5）', 'got ' + d.score);
      if (d.correct_count === 3) ok('correct_count=3');
      else ko('correct_count=3', 'got ' + d.correct_count);
      if (Array.isArray(d.results) && d.results.length === 5) ok('results 为 5 条');
      else ko('results 为 5 条', JSON.stringify(d.results));
      if (d.results && d.results.slice(0,3).every(r => r.correct)
          && d.results.slice(3).every(r => !r.correct))
        ok('逐题对错与作答一致');
      else ko('逐题对错与作答一致', JSON.stringify(d.results));
      if (Array.isArray(d.solidify_candidates) && d.solidify_candidates.length === 2)
        ok('固化候选 2 条');
      else ko('固化候选 2 条', JSON.stringify(d.solidify_candidates));
      if (d.coins_earned === 10) ok('词币入账 coins_earned=10（3/5 正确）');
      else ko('词币入账 coins_earned=10', 'got ' + d.coins_earned);
      if (typeof d.coins_note === 'string' && d.coins_note.includes('+5')) ok('含词币说明');
      else ko('含词币说明', JSON.stringify(d.coins_note));

      // 重复提交：不再重复入账，coins_earned=0
      const again = await api('POST', '/api/quiz/daily/submit', { quiz_id: quizId, answers });
      if (again.status === 200 && again.json && again.json.coins_earned === 0) ok('重复提交 coins_earned=0');
      else ko('重复提交 coins_earned=0', JSON.stringify(again.json));

      // 学情联动验证：user_learning 已写
      const ul = runMysql(
        `SELECT CONCAT(knowledge_id,':',review_times,':',error_times) FROM user_learning
         WHERE knowledge_id IN (${stored.filter(q=>q.category==='solidified').map(q=>q.knowledge_id).join(',')})
         AND user_id=1`);
      if (ul && ul.split('\n').every(line => {
        const [,rt] = line.split(':'); return Number(rt) >= 1;
      })) ok('已固化题学情已联动（review_times≥1）');
      else ko('已固化题学情已联动', ul);
    } catch(e) { ko('提交批改', e.message); }
  } else {
    skip('提交批改', '出题失败，跳过');
  }

  // ---- 5. 考后固化 ----
  console.log('\n[5] 考后固化');
  if (quizId && submit && submit.json) {
    try {
      // 非固化题型（第1题）应拒绝
      const bad = await api('POST', '/api/quiz/solidify-item', { quiz_id: quizId, no: 1 });
      if (!bad.json || !bad.json.ok) ok('第1题拒绝固化');
      else ko('第1题拒绝固化', JSON.stringify(bad.json));

      const sol = await api('POST', '/api/quiz/solidify-item', { quiz_id: quizId, no: 4 }, 130000);
      if (sol.status === 200 && sol.json && sol.json.ok && sol.json.knowledge_id)
        ok('第4题固化成功', 'kp=' + sol.json.knowledge_id);
      else ko('第4题固化成功', sol.text);

      const dup = await api('POST', '/api/quiz/solidify-item', { quiz_id: quizId, no: 4 });
      if (dup.json && dup.json.ok && dup.json.duplicated) ok('再次固化返回 duplicated');
      else ko('再次固化返回 duplicated', JSON.stringify(dup.json));

      // 归属/不存在测验
      const nf = await api('POST', '/api/quiz/solidify-item', { quiz_id: 99999999, no: 4 });
      if (nf.status === 404) ok('他人/不存在测验返回404');
      else ko('他人/不存在测验返回404', nf.text);
    } catch(e) { ko('考后固化', e.message); }
  } else {
    skip('考后固化', '前置失败，跳过');
  }

  // ---- 6. 前端页面 ----
  console.log('\n[6] 前端页面检查');
  const pagePath = path.join(ROOT, 'frontend/pages/daily-quiz.html');
  if (fs.existsSync(pagePath)) ok('daily-quiz.html 存在');
  else ko('daily-quiz.html 存在', '未找到');
  if (fs.existsSync(pagePath)) {
    const html = fs.readFileSync(pagePath, 'utf8');
    ['btn-start','quiz-card','result-card','history-card'].forEach(id => {
      if (html.includes(`id="${id}"`)) ok(`页面含 #${id}`);
      else ko(`页面含 #${id}`, '未找到');
    });
    if (html.includes('/api/quiz/daily') && html.includes('solidify-item'))
      ok('页面调用 4 个接口');
    else ko('页面调用 4 个接口', '接口引用缺失');
    if (html.includes('/static/nav.js')) ok('引入 nav.js');
    else ko('引入 nav.js', '未找到（T102 已统一）');
  }

  const navJs = fs.readFileSync(path.join(ROOT, 'frontend/static/nav.js'), 'utf8');
  if (navJs.includes('/pages/daily-quiz.html')) ok('导航含小测入口（T102 nav.js 统一）');
  else ko('导航含小测入口', '未找到（nav.js 缺失 daily-quiz 链接）');

  const css = fs.readFileSync(path.join(ROOT, 'frontend/static/components.css'), 'utf8');
  if (css.includes('.quiz-score') && css.includes('.quiz-solidify'))
    ok('组件基座含 T106 样式（quiz-score / quiz-solidify）');
  else ko('组件基座含 T106 样式', '缺少新 quiz- 样式');

  // ---- 7. 汇总 ----
  console.log('\n=== 测试汇总 ===');
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
