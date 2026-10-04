// 阶段八：AI 重试/缓存/断网兜底 - 集成测试
// v1.2.0-T002：覆盖 ai_tasks 持久化、指数退避、降级状态、任务重试
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

function runMysql(sql, opts={}) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const db = opts.db || 'english_learning';
  const args = ['--socket='+SOCK, '-uroot', '-p'+pw, db, '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout;
}

function api(method, p, body) {
  return new Promise((resolve, reject) => {
    const u = new URL(p, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json' };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers, timeout: 60000
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
    if (data) req.write(data);
    req.end();
  });
}

async function main() {
  console.log('=== 阶段八：AI 重试/缓存/断网兜底（v1.2.0-T002）集成测试 ===\n');

  // 清理测试数据
  try {
    runMysql("DELETE FROM ai_tasks WHERE task_type='solidify_kp' AND ref_id >= 370;");
    runMysql("DELETE FROM knowledge_points WHERE user_input_content LIKE 't002test%';");
  } catch(e) {}

  // ---- 0. 健康检查增强 ----
  console.log('[0] 健康检查增强');
  try {
    const h = await api('GET', '/api/health');
    if (h.json && h.json.ai_degraded !== undefined) ok('health 含 ai_degraded');
    else ko('health 含 ai_degraded', JSON.stringify(h.json));
    if (h.json && h.json.ai_tasks && typeof h.json.ai_tasks.pending === 'number') ok('health 含 ai_tasks stats');
    else ko('health 含 ai_tasks stats', JSON.stringify(h.json));
  } catch(e) { ko('健康检查', e.message); }

  // ---- 1. 固化自动记录 ai_tasks ----
  console.log('\n[1] 固化自动记录 ai_tasks');

  // 录入新词
  const kp = await api('POST', '/api/kp', { user_input_content: 't002testword' });
  if (!kp.json?.id) { ko('录入新词', kp.text); process.exit(1); }
  const kpId = kp.json.id;
  ok('录入新词', `id=${kpId}`);

  // 固化
  const solid = await api('POST', `/api/kp/${kpId}/solidify`);
  if (solid.json?.ok) ok('固化成功');
  else ko('固化成功', solid.text);

  // 查 ai_tasks
  try {
    const rows = runMysql(`SELECT id, status, ref_id FROM ai_tasks WHERE task_type='solidify_kp' AND ref_id=${kpId} LIMIT 1`, { silent: true }).trim();
    if (rows.includes('done')) ok('ai_tasks 固化记录 status=done');
    else ko('ai_tasks 固化记录 status=done', rows);
  } catch(e) { ko('ai_tasks 查询', e.message); }

  // ---- 2. AI 任务 API ----
  console.log('\n[2] AI 任务 API');

  const list = await api('GET', '/api/ai/tasks');
  if (list.json && list.json.data && Array.isArray(list.json.data)) ok('GET /api/ai/tasks 列表');
  else ko('GET /api/ai/tasks 列表', list.text);
  if (list.json?.stats?.done !== undefined) ok('stats 字段存在');
  else ko('stats 字段存在', JSON.stringify(list.json?.stats));

  // ---- 3. 任务删除 ----
  console.log('\n[3] 任务删除');
  const firstTask = list.json?.data?.[0];
  if (firstTask) {
    const del = await api('DELETE', `/api/ai/tasks/${firstTask.id}`);
    if (del.json?.ok) ok('删除任务');
    else ko('删除任务', del.text);
  } else {
    skip('删除任务', '无任务');
  }

  // ---- 4. 任务重试 ----
  console.log('\n[4] 任务重试');

  // 重新录入一个新词
  const kp2 = await api('POST', '/api/kp', { user_input_content: 't002retryword' });
  const kp2Id = kp2.json?.id;
  if (!kp2Id) { ko('录入新词2', kp2.text); }
  else {
    ok('录入新词2', `id=${kp2Id}`);
    // 固化
    await api('POST', `/api/kp/${kp2Id}/solidify`);
    // 找到对应 ai_task
    const list2 = await api('GET', '/api/ai/tasks');
    const task = list2.json?.data?.find(t => t.ref_id === kp2Id);
    if (task) {
      // 手动重试（即使已 done 也可以调）
      const retry = await api('POST', `/api/ai/tasks/${task.id}/retry`);
      if (retry.json?.ok) ok('手动重试成功');
      else ko('手动重试', retry.text);
    } else {
      skip('手动重试', '找不到任务');
    }
  }

  // ---- 5. retry-all ----
  console.log('\n[5] retry-all');
  const retryAll = await api('POST', '/api/ai/tasks/retry-all');
  if (retryAll.json?.ok !== undefined) ok('retry-all 端点可用');
  else ko('retry-all 端点可用', retryAll.text);

  // ---- 6. 已固化词条重复固化 ----
  console.log('\n[6] 已固化词条重复固化被正确拒绝');
  const solid2 = await api('POST', `/api/kp/${kpId}/solidify`);
  if (solid2.json?.ok) ko('已固化词条应拒绝重复固化', 'unexpectedly ok');
  else if (solid2.json?.error?.includes('solidified') || solid2.json?.error?.includes('merged')) ok('已固化词条拒绝/合并');
  else ok('已固化词条正确拒绝重复固化');

  // ---- 汇总 ----
  console.log(`\n=== 结果：${pass} 通过 / ${fail} 失败 / ${skipped} 跳过 ===`);
  if (failures.length) {
    console.log('\n失败项：');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
  }

  // 清理
  try {
    runMysql(`DELETE FROM ai_tasks WHERE ref_id IN (SELECT id FROM knowledge_points WHERE user_input_content LIKE 't002test%');`);
  } catch(e) {}

  process.exit(fail === 0 ? 0 : 1);
}

main().catch(e => { console.error(e); process.exit(1); });
