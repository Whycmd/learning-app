// 阶段十七：AI Mock 三态夹具（v1.4.1-T004）
// 覆盖 success / timeout / 5xx：
//   - success：通过主测试后端验证固化全链路（不联网）
//   - timeout / 5xx：临时拉起同库独立后端，验证失败可控、不落半成品数据
// 仅在隔离测试环境执行（DB=测试库且 AI_MOCK_MODE 非 off）
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn, spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT, AI_MOCK_MODE } = require('./_env');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

const ROOT = path.resolve(__dirname, '..');
const SERVER_BIN = path.join(ROOT, 'backend/build/eng_app_server');

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const args = ['--socket=' + SOCK, '-uroot', '-p' + pw, '-N', DB_NAME, '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 30000 });
  if (r.status !== 0) throw new Error((r.stderr || r.stdout || '').trim());
  return r.stdout.trim();
}

function api(port, method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const req = http.request({
      host: API_HOST, port, method, path: urlPath,
      headers: { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 },
      timeout: 120000,
    }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        let json = null;
        try { json = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch (_) {}
        resolve({ status: res.statusCode, json, text: Buffer.concat(chunks).toString('utf8') });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

function waitHealth(port, timeoutMs = 30000) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = () => {
      const req = http.get({ host: API_HOST, port, path: '/api/health', timeout: 3000 }, res => {
        res.resume();
        if (res.statusCode === 200) return resolve();
        retry();
      });
      req.on('error', retry);
      req.on('timeout', () => req.destroy(new Error('timeout')));
      function retry() {
        if (Date.now() - start > timeoutMs) return reject(new Error(`backend not ready on ${port}`));
        setTimeout(tick, 300);
      }
    };
    tick();
  });
}

function startBackend(port, mockMode) {
  const logFile = path.join(os.tmpdir(), `eng_mock_${mockMode}_${process.pid}.log`);
  const out = fs.openSync(logFile, 'a');
  const proc = spawn(SERVER_BIN, ['--root', ROOT, '--port', String(port)], {
    env: {
      ...process.env,
      DB_NAME, TEST_DB_NAME: DB_NAME,
      ENG_DB_NAME: DB_NAME,
      ENG_AI_MOCK_MODE: mockMode,
      AI_MOCK_MODE: mockMode,
      ENG_AI_RETRY_TIMES: '1',
      ENG_AI_RETRY_INTERVAL: '1',
      ENG_AI_MAX_CONCURRENCY: '1',
    },
    stdio: ['ignore', out, out],
  });
  return { proc, logFile };
}

function stopBackend(proc) {
  return new Promise(resolve => {
    if (!proc || proc.exitCode !== null) return resolve();
    let done = false;
    const finish = () => { if (!done) { done = true; resolve(); } };
    proc.once('exit', finish);
    try { proc.kill('SIGTERM'); } catch { /* ignore */ }
    setTimeout(() => { try { proc.kill('SIGKILL'); } catch {} finish(); }, 5000);
  });
}

async function createKp(port, word) {
  const r = await api(port, 'POST', '/api/kp', { user_input_content: word });
  if (!r.json || !r.json.id) throw new Error('录入失败: ' + String(r.text).slice(0, 120));
  return r.json.id;
}

async function main() {
  console.log('=== 阶段十七：AI Mock 三态夹具（v1.4.1-T004） ===\n');

  // 仅隔离测试环境运行，避免误写开发库
  if (AI_MOCK_MODE === 'off' || DB_NAME === 'english_learning') {
    skip('AI Mock 三态夹具', `需要隔离测试环境，当前 DB=${DB_NAME}, Mock=${AI_MOCK_MODE}`);
    console.log(`\n阶段十七结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
    process.exit(0);
  }

  const word = 't017_mock_' + Date.now().toString(36).slice(-6);

  // ---- 1. success：主测试后端（Mock success）固化全链路 ----
  console.log('[1] success 夹具');
  try {
    const h = await api(API_PORT, 'GET', '/api/health');
    if (h.json && h.json.ai === true) ok('健康检查 ai=true');
    else ko('健康检查 ai', String(h.text).slice(0, 120));
    if (h.json && h.json.ai_provider === 'mock') ok('provider=mock');
    else ko('provider', h.json && h.json.ai_provider);
    if (h.json && h.json.ai_degraded === false) ok('ai_degraded=false');
    else ko('ai_degraded', h.json && h.json.ai_degraded);

    const id = await createKp(API_PORT, word);
    const sol = await api(API_PORT, 'POST', `/api/kp/${id}/solidify`);
    if (sol.status === 200 && sol.json && (sol.json.ok === true || sol.json.is_solidified === 1)) {
      ok('success 固化成功', `id=${id}`);
    } else ko('success 固化', `status=${sol.status} ${String(sol.text).slice(0, 120)}`);

    const row = runMysql(`SELECT is_solidified,difficulty,standard_content FROM knowledge_points WHERE id=${id};`).split('\t');
    if (row[0] === '1' && row[1] === '3' && row[2] === word) ok('固化字段与 fixture 一致', `difficulty=${row[1]}`);
    else ko('固化字段', row.join('|'));
    const defs = Number(runMysql(`SELECT COUNT(*) FROM kp_definitions WHERE knowledge_id=${id};`));
    if (defs >= 2) ok('子表已写入', `definitions=${defs}`);
    else ko('子表数量', `definitions=${defs}`);
    const md = Number(runMysql(`SELECT COUNT(*) FROM kp_extras WHERE knowledge_id=${id} AND extra_type='markdown_content';`));
    if (md >= 1) ok('markdown_content 已写入', `rows=${md}`);
    else ko('markdown_content', `rows=${md}`);
  } catch (e) { ko('success 夹具', e.message); }

  // ---- 2/3. timeout、5xx：临时独立后端 ----
  const auxPort = API_PORT + 1;
  for (const mode of ['timeout', '5xx']) {
    console.log(`\n[${mode === 'timeout' ? 2 : 3}] ${mode} 夹具`);
    let proc = null;
    try {
      const started = startBackend(auxPort, mode);
      proc = started.proc;
      await waitHealth(auxPort);

      const h = await api(auxPort, 'GET', '/api/health');
      if (h.json && h.json.ai === false) ok(`${mode} 健康检查 ai=false`);
      else ko(`${mode} 健康检查`, String(h.text).slice(0, 120));
      if (h.json && h.json.ai_provider === 'mock') ok(`${mode} provider=mock`);
      else ko(`${mode} provider`, h.json && h.json.ai_provider);

      const w = `${word}_${mode}`;
      const id = await createKp(auxPort, w);
      const sol = await api(auxPort, 'POST', `/api/kp/${id}/solidify`);
      const failed = !(sol.status === 200 && sol.json && (sol.json.ok === true || sol.json.is_solidified === 1));
      if (failed) ok(`${mode} 固化请求失败可控`, `status=${sol.status}, ${String(sol.text).slice(0, 80)}`);
      else ko(`${mode} 固化应失败`, `status=${sol.status} ${String(sol.text).slice(0, 80)}`);

      const row = runMysql(`SELECT is_solidified FROM knowledge_points WHERE id=${id};`).trim();
      if (row === '0') ok(`${mode} 未落半成品数据`);
      else ko(`${mode} 数据完整性`, `is_solidified=${row}`);
    } catch (e) {
      ko(`${mode} 夹具`, e.message);
    } finally {
      await stopBackend(proc);
    }
  }

  // 清理本套件数据（测试库每轮重建，此处仅保证同轮后续套件干净）
  try {
    const ids = runMysql(`SELECT id FROM knowledge_points WHERE user_id=1 AND standard_content LIKE 't017_mock_%';`)
      .split('\n').map(s => s.trim()).filter(Boolean);
    if (ids.length) {
      const inIds = ids.join(',');
      runMysql(`DELETE FROM ai_tasks WHERE task_type='solidify_kp' AND ref_id IN (${inIds});`);
      for (const t of ['kp_definitions', 'kp_collocations', 'kp_synonyms', 'kp_antonyms', 'kp_extras', 'user_learning']) {
        runMysql(`DELETE FROM ${t} WHERE knowledge_id IN (${inIds});`);
      }
      runMysql(`DELETE FROM knowledge_points WHERE id IN (${inIds});`);
    }
  } catch { /* ignore */ }

  console.log('\n=== 阶段十七结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}  跳过: ${skipped}`);
  if (fail > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
    process.exit(1);
  }
  process.exit(0);
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
