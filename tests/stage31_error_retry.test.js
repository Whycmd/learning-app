// 阶段三十一：错题归因与重练（v1.5.1-T002 ERROR-RETRY）
// 覆盖答错归因、按题去重、错题本聚合、手动改归因、重练完成发奖、
// 连续答对归档、恢复、重练答错重置、日上限封顶
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

function api(method, urlPath, body, timeoutMs = 30000) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlPath, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = {
      'Content-Type': 'application/json',
      'Content-Length': data ? Buffer.byteLength(data) : 0
    };
    require('./_system').withSystemCookie(headers);
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

const WORD = 'retrytest';
let kid = 0;
let attrId = 0;
let retryId = 0;

function cleanup() {
  if (kid > 0) {
    runMysql(`DELETE FROM error_attributions WHERE user_id=1 AND knowledge_id=${kid}`);
    runMysql(`DELETE FROM retry_items WHERE user_id=1 AND knowledge_id=${kid}`);
    runMysql(`DELETE FROM user_learning WHERE user_id=1 AND knowledge_id=${kid}`);
    runMysql(`DELETE FROM knowledge_points WHERE id=${kid}`);
  }
  runMysql("DELETE FROM error_attributions WHERE user_id=1 AND source_id='stage31'");
  runMysql("DELETE FROM user_coin_txn WHERE user_id=1 AND reason_code='retry_done'");
}

async function gradeWrong(sourceType, sourceId, no) {
  return api('POST', '/api/exercise/grade', {
    type: 'word_form_fill',
    user_answer: 'ratrytest',
    correct_answer: WORD,
    knowledge_id: kid,
    source_type: sourceType,
    source_id: sourceId,
    question_no: no,
    question_type: 'word_form_fill'
  });
}

async function main() {
  console.log('=== 阶段三十一：错题归因与重练（v1.5.1-T002）===\n');

  console.log('[0] 前置检查');
  try {
    const h = await api('GET', '/api/health');
    if (h.json && h.json.status === 'ok') ok('后端健康检查');
    else { ko('后端健康检查', h.text); return finish(); }
  } catch (e) { ko('后端健康检查', e.message); return finish(); }

  runMysql(`DELETE FROM knowledge_points WHERE user_id=1 AND user_input_content='${WORD}'`);
  cleanup();
  kid = Number(runMysql(
    `INSERT INTO knowledge_points (user_id,user_input_content,standard_content,is_solidified,solidify_time) ` +
    `VALUES (1,'${WORD}','${WORD}',1,NOW()); SELECT LAST_INSERT_ID();`));
  if (kid > 0) ok('准备测试知识点', 'id=' + kid);
  else { ko('准备测试知识点', 'insert failed'); return finish(); }

  console.log('\n[1] 答错写入归因');
  try {
    const r = await gradeWrong('exercise', 'stage31', 1);
    const a = r.json && r.json.attribution;
    if (r.json && r.json.correct === false) ok('批改判定为错误');
    else ko('批改判定为错误', JSON.stringify(r.json));
    if (a && a.error_type === 'spelling') ok('规则归因为拼写', a.reason);
    else ko('规则归因为拼写', JSON.stringify(a));
    if (a && a.reason && !a.pending) ok('归因含理由且无需 AI 兜底');
    else ko('归因含理由且无需 AI 兜底', JSON.stringify(a));
    attrId = a ? a.id : 0;
  } catch (e) { ko('答错写入归因', e.message); }

  console.log('\n[2] 按题去重');
  try {
    const r = await gradeWrong('exercise', 'stage31', 1);
    if (r.json && r.json.correct === false && !r.json.attribution) ok('重复提交同题不重复建归因');
    else ko('重复提交同题不重复建归因', JSON.stringify(r.json));
    const n = Number(runMysql(
      `SELECT COUNT(*) FROM error_attributions WHERE user_id=1 AND knowledge_id=${kid}`));
    if (n === 1) ok('归因表仅一条', 'count=' + n);
    else ko('归因表仅一条', 'count=' + n);
  } catch (e) { ko('按题去重', e.message); }

  console.log('\n[3] 错题本聚合');
  try {
    const s = await api('GET', '/api/wrongbook/summary');
    if (s.json && s.json.total_count >= 1) ok('总览错题词数 ≥ 1', 'total=' + s.json.total_count);
    else ko('总览错题词数 ≥ 1', JSON.stringify(s.json));
    const w = await api('GET', '/api/wrongbook/words?error_type=spelling');
    const items = (w.json && w.json.items) || [];
    const hit = items.find(x => x.knowledge_id === kid);
    if (hit && hit.latest_error_type === 'spelling') ok('按错因筛选命中');
    else ko('按错因筛选命中', JSON.stringify(w.json));
    const d = await api('GET', '/api/wrongbook/words/' + kid);
    if (d.json && (d.json.items || []).length >= 1) ok('单词详情返回错因明细');
    else ko('单词详情返回错因明细', JSON.stringify(d.json));
  } catch (e) { ko('错题本聚合', e.message); }

  console.log('\n[4] 手动修正归因');
  try {
    const u = await api('PUT', '/api/error-attributions/' + attrId, { error_type: 'collocation' });
    if (u.json && u.json.ok === true) ok('PUT 改归因成功');
    else ko('PUT 改归因成功', JSON.stringify(u.json));
    const d = await api('GET', '/api/wrongbook/words/' + kid);
    const first = d.json && (d.json.items || [])[0];
    if (first && first.error_type === 'collocation' && first.source === 'user') ok('归因改为用户修正');
    else ko('归因改为用户修正', JSON.stringify(first));
  } catch (e) { ko('手动修正归因', e.message); }

  console.log('\n[5] 重练队列与发奖');
  try {
    const q = await api('GET', '/api/retry-items');
    const items = Array.isArray(q.json) ? q.json : ((q.json && q.json.data) || []);
    const it = items.find(x => x.knowledge_id === kid);
    retryId = it ? it.id : 0;
    if (retryId) ok('错题进入重练队列', 'id=' + retryId);
    else ko('错题进入重练队列', JSON.stringify(q.json));

    const ad = await api('POST', '/api/retry-items/' + retryId + '/advance');
    if (ad.json && ad.json.ok === true) ok('提前重练成功');
    else ko('提前重练成功', JSON.stringify(ad.json));

    const ad2 = await api('POST', '/api/retry-items/' + retryId + '/advance');
    if (ad2.json && ad2.json.ok === true) ok('重复提前重练仍成功（幂等）');
    else ko('重复提前重练仍成功（幂等）', JSON.stringify(ad2.json));

    const c = await api('POST', '/api/retry-items/' + retryId + '/complete', { is_correct: true });
    if (c.json && c.json.ok === true && c.json.consecutive_correct === 1) ok('答对记录连对 1 次');
    else ko('答对记录连对 1 次', JSON.stringify(c.json));
    if (c.json && c.json.reward && c.json.reward.amount === 5) ok('完成重练 +5 金桂');
    else ko('完成重练 +5 金桂', JSON.stringify(c.json));
    const tx = Number(runMysql(
      "SELECT COUNT(*) FROM user_coin_txn WHERE user_id=1 AND reason_code='retry_done'"));
    if (tx >= 1) ok('金桂流水写入 retry_done', 'count=' + tx);
    else ko('金桂流水写入 retry_done', 'count=' + tx);
  } catch (e) { ko('重练队列与发奖', e.message); }

  console.log('\n[6] 连续答对归档与恢复');
  try {
    for (let i = 0; i < 2; i++) {
      await api('POST', '/api/retry-items/' + retryId + '/advance');
      await api('POST', '/api/retry-items/' + retryId + '/complete', { is_correct: true });
    }
    const d = await api('GET', '/api/wrongbook/words/' + kid);
    const rt = d.json && d.json.retry;
    if (rt && rt.status === 'archived' && rt.consecutive_correct >= 3) ok('连续答对 3 次归档');
    else ko('连续答对 3 次归档', JSON.stringify(rt));
    const rs = await api('POST', '/api/retry-items/' + retryId + '/restore');
    if (rs.json && rs.json.ok === true) ok('归档后可恢复');
    else ko('归档后可恢复', JSON.stringify(rs.json));
  } catch (e) { ko('连续答对归档与恢复', e.message); }

  console.log('\n[7] 重练答错重置');
  try {
    await api('POST', '/api/retry-items/' + retryId + '/advance');
    const c = await api('POST', '/api/retry-items/' + retryId + '/complete', { is_correct: false });
    if (c.json && c.json.ok === true && c.json.consecutive_correct === 0) ok('答错后连对清零');
    else ko('答错后连对清零', JSON.stringify(c.json));
    const d = await api('GET', '/api/wrongbook/words/' + kid);
    const hasRetry = (d.json && d.json.items || []).some(x => x.source_type === 'retry');
    if (hasRetry) ok('写入重练来源归因');
    else ko('写入重练来源归因', JSON.stringify(d.json));
  } catch (e) { ko('重练答错重置', e.message); }

  console.log('\n[8] 日上限封顶');
  try {
    for (let i = 0; i < 10; i++) {
      runMysql(
        "INSERT INTO user_coin_txn (user_id,delta,reason_code,balance_after,created_at) " +
        "VALUES (1,5,'retry_done',0,NOW())");
    }
    await api('POST', '/api/retry-items/' + retryId + '/advance');
    const c = await api('POST', '/api/retry-items/' + retryId + '/complete', { is_correct: true });
    if (c.json && c.json.ok === true && c.json.reward && c.json.reward.capped === true)
      ok('达到日上限不再发奖');
    else ko('达到日上限不再发奖', JSON.stringify(c.json));
  } catch (e) { ko('日上限封顶', e.message); }

  cleanup();
  finish();
}

function finish() {
  console.log(`\n结果：${pass} 通过 / ${fail} 失败`);
  if (fail) {
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`));
    process.exitCode = 1;
  }
}

main().catch(e => { console.error(e); process.exitCode = 1; });
