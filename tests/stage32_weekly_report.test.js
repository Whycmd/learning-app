// 阶段三十二：学情周报（v1.5.1-T003 WEEKLY-REPORT）
// 覆盖：登录拦截、空态、周聚合（课规/趋势/错题/待重练）、4 周切换、
//       词表开关、AI 生成与缓存、AI 失败回退由前端兜底（后端规则建议始终返回）
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT } = require('./_env');
const API = `http://${API_HOST}:${API_PORT}`;

let pass = 0, fail = 0;
const failures = [];

function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const args = ['--socket=' + SOCK, '-uroot', '-p' + pw, DB_NAME, '-N', '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout.trim();
}

function api(method, urlPath, body, timeoutMs = 30000, withCookie = true) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlPath, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = {
      'Content-Type': 'application/json',
      'Content-Length': data ? Buffer.byteLength(data) : 0
    };
    if (withCookie) require('./_system').withSystemCookie(headers);
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers, timeout: timeoutMs
    }, res => {
      const chunks = [];
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

function pad2(n) { return String(n).padStart(2, '0'); }
function fmt(d) { return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`; }
function shift(dateStr, days) {
  const d = new Date(dateStr + 'T00:00:00');
  d.setDate(d.getDate() + days);
  return fmt(d);
}

function lastWeekMonday() {
  const d = new Date();
  d.setHours(0, 0, 0, 0);
  d.setDate(d.getDate() - ((d.getDay() + 6) % 7) - 7);
  return fmt(d);
}

const WK = lastWeekMonday();
const EMPTY_WK = shift(WK, -28);

function seed() {
  cleanup();
  runMysql(
    `INSERT INTO user_weekly_goal
      (user_id,year_week,goal_type,week_key,goal_id,metric_key,target,progress,achieved,effective_target)
     VALUES
      (1,0,'review_items','${WK}',9001,'review_items',10,10,1,10),
      (1,0,'quiz_count','${WK}',9001,'quiz_count',2,2,1,2),
      (1,0,'new_words','${WK}',9001,'new_words',5,1,0,5);`
  );
  runMysql(
    `INSERT INTO error_attributions (user_id,knowledge_id,source_type,source_id,question_no,error_type,created_at)
     VALUES (1,990001,'exercise','stage32',1,'spelling','${shift(WK, 1)} 10:00:00'),
            (1,990002,'exercise','stage32',2,'spelling','${shift(WK, 1)} 10:05:00'),
            (1,990003,'exercise','stage32',3,'meaning','${shift(WK, 2)} 10:05:00');`
  );
  runMysql(
    `INSERT INTO retry_items (user_id,knowledge_id,status) VALUES (1,990001,'queued');`
  );
  runMysql(
    `INSERT INTO user_learning (user_id,knowledge_id,proficiency_level,last_review_time)
     VALUES (1,990001,3,'${shift(WK, 1)} 09:00:00'),
            (1,990002,4,'${shift(WK, 1)} 09:10:00'),
            (1,990003,2,'${shift(WK, 3)} 09:10:00');`
  );
}

function cleanup() {
  runMysql(`DELETE FROM user_weekly_goal WHERE user_id=1 AND goal_id=9001`);
  runMysql(`DELETE FROM error_attributions WHERE user_id=1 AND source_id='stage32'`);
  runMysql(`DELETE FROM retry_items WHERE user_id=1 AND knowledge_id IN (990001,990002,990003)`);
  runMysql(`DELETE FROM user_learning WHERE user_id=1 AND knowledge_id IN (990001,990002,990003)`);
  runMysql(`DELETE FROM weekly_report_ai WHERE user_id=1`);
}

async function waitTask(taskId, timeoutMs = 30000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    const r = await api('GET', '/api/ai-tasks/' + taskId);
    if (r.json && (r.json.status === 'done' || r.json.status === 'failed' || r.json.status === 'partial')) return r.json;
    await new Promise(res => setTimeout(res, 300));
  }
  return null;
}

async function main() {
  console.log('=== 阶段三十二：学情周报（v1.5.1-T003）===\n');

  try {
    console.log('[0] 前置检查');
    const h = await api('GET', '/api/health');
    if (h.json && h.json.status === 'ok') ok('后端健康检查');
    else ko('后端健康检查', '未就绪');

    console.log('\n[1] 登录拦截');
    const noAuth = await api('GET', `/api/weekly-reports?week=${WK}`, null, 30000, false);
    if (noAuth.status === 401) ok('未登录 GET 返回 401');
    else ko('未登录 GET 返回 401', `got ${noAuth.status}`);
    const noAuthPost = await api('POST', '/api/weekly-reports/ai', { week_start: WK }, 30000, false);
    if (noAuthPost.status === 401) ok('未登录 POST 返回 401');
    else ko('未登录 POST 返回 401', `got ${noAuthPost.status}`);

    console.log('\n[2] 空态');
    seed();
    const empty = await api('GET', `/api/weekly-reports?week=${EMPTY_WK}`);
    if (empty.status === 200 && empty.json && empty.json.has_data === false) ok('无记录周 has_data=false');
    else ko('无记录周 has_data=false', empty.text.slice(0, 160));
    if (empty.json && Array.isArray(empty.json.rules) && empty.json.rules.length > 0) ok('空态仍有规则建议');
    else ko('空态仍有规则建议', empty.text.slice(0, 160));

    console.log('\n[3] 周聚合');
    const rep = await api('GET', `/api/weekly-reports?week=${WK}`);
    const r = rep.json || {};
    if (rep.status === 200 && r.has_data === true) ok('有记录周 has_data=true');
    else ko('有记录周 has_data=true', rep.text.slice(0, 160));
    if (r.goal && r.goal.total === 3 && r.goal.done === 2 && r.goal.rate === 66) ok('课规完成 2/3（66%）');
    else ko('课规完成 2/3（66%）', JSON.stringify(r.goal));
    if (r.wrong && r.wrong.total_words === 3 && r.wrong.by_type && r.wrong.by_type.spelling === 2) ok('错题按词去重与错因分布');
    else ko('错题按词去重与错因分布', JSON.stringify(r.wrong));
    if (r.retry_pending === 1) ok('待重练 1');
    else ko('待重练 1', String(r.retry_pending));
    const trendSum = (r.trend || []).reduce((s, x) => s + (x.count || 0), 0);
    if (Array.isArray(r.trend) && r.trend.length === 7 && trendSum === 3) ok('7 天趋势合计 3 次');
    else ko('7 天趋势合计 3 次', JSON.stringify(r.trend));
    if ((r.rules || []).some(x => x.indexOf('课规完成 2/3') >= 0)) ok('规则建议含课规未完成提示');
    else ko('规则建议含课规未完成提示', JSON.stringify(r.rules));

    console.log('\n[4] 周切换');
    const prev = await api('GET', `/api/weekly-reports?week=${shift(WK, -7)}`);
    if (prev.status === 200 && prev.json.week_start === shift(WK, -7) && prev.json.has_data === false) ok('可切换上一周');
    else ko('可切换上一周', prev.text.slice(0, 160));

    console.log('\n[5] 词表开关');
    const off = await api('GET', `/api/weekly-reports?week=${WK}&include_wordlist=0`);
    if (off.json && off.json.wordlist && off.json.wordlist.enabled === false) ok('开关关闭时不统计词表');
    else ko('开关关闭时不统计词表', JSON.stringify(off.json && off.json.wordlist));
    const on = await api('GET', `/api/weekly-reports?week=${WK}&include_wordlist=1`);
    if (on.json && on.json.wordlist && on.json.wordlist.enabled === true && on.json.wordlist.bound === false) ok('开启后未绑定给引导');
    else ko('开启后未绑定给引导', JSON.stringify(on.json && on.json.wordlist));

    console.log('\n[6] AI 生成与缓存');
    const ai1 = await api('POST', '/api/weekly-reports/ai', { week_start: WK });
    if (ai1.status === 200 && ai1.json && ai1.json.task_id > 0) ok('提交 AI 生成任务');
    else ko('提交 AI 生成任务', ai1.text.slice(0, 160));
    const task = ai1.json && ai1.json.task_id ? await waitTask(ai1.json.task_id) : null;
    if (task && task.status === 'done') ok('AI 任务完成');
    else ko('AI 任务完成', JSON.stringify(task));
    const after = await api('GET', `/api/weekly-reports?week=${WK}`);
    if (after.json && after.json.ai && after.json.ai.status === 'done' && after.json.ai.text) ok('周报返回 AI 文本');
    else ko('周报返回 AI 文本', JSON.stringify(after.json && after.json.ai));
    const aiText = ((after.json && after.json.ai && after.json.ai.text) || '').trim();
    if (aiText && !aiText.startsWith('{') && aiText.includes('（模拟）')) ok('AI 文本已解析为纯文本');
    else ko('AI 文本已解析为纯文本', JSON.stringify(aiText));
    if (after.json && (after.json.rules || []).length > 0) ok('AI 完成后规则建议仍在');
    else ko('AI 完成后规则建议仍在', '');
    const ai2 = await api('POST', '/api/weekly-reports/ai', { week_start: WK });
    if (ai2.status === 200 && ai2.json && ai2.json.cached === true && ai2.json.status === 'done') ok('重复提交命中缓存');
    else ko('重复提交命中缓存', ai2.text.slice(0, 160));

    console.log('\n[7] 首页提示');
    const sum = await api('GET', '/api/weekly-reports?summary=1');
    if (sum.status === 200 && sum.json && sum.json.has_data === true) ok('上周有记录时首页提示可见');
    else ko('上周有记录时首页提示可见', sum.text.slice(0, 160));
    runMysql(`DELETE FROM user_weekly_goal WHERE user_id=1 AND goal_id=9001`);
    runMysql(`DELETE FROM error_attributions WHERE user_id=1 AND source_id='stage32'`);
    runMysql(`DELETE FROM user_learning WHERE user_id=1 AND knowledge_id IN (990001,990002,990003)`);
    const sum2 = await api('GET', '/api/weekly-reports?summary=1');
    if (sum2.status === 200 && sum2.json && sum2.json.has_data === false) ok('上周无记录时首页提示隐藏');
    else ko('上周无记录时首页提示隐藏', sum2.text.slice(0, 160));
  } catch (e) {
    ko('执行异常', e.message);
  } finally {
    try { cleanup(); } catch (e) { console.log('清理失败：' + e.message); }
  }

  console.log(`\n=== 结果：${pass} 通过, ${fail} 失败 ===`);
  if (fail > 0) {
    failures.forEach(f => console.log(`  ✗ ${f.name} — ${f.msg}`));
    process.exit(1);
  }
}

main();
