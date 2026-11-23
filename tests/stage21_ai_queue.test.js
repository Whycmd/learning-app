// 阶段二十一：AI 分层队列与多模型兜底（v1.5.0-T002）
// 覆盖：L1 秒回复用、L2 排队 position/eta、旧固化接口兼容、/api/ai-tasks、
//       多模型主备切换与降级、队列满 429、重启断点续跑
// 仅在隔离测试环境执行（测试库 + AI Mock 非 off）
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn, spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT, AI_MOCK_MODE } = require('./_env');
const { withSystemCookie } = require('./_system');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

const ROOT = path.resolve(__dirname, '..');
const SERVER_BIN = path.join(ROOT, 'backend/build/eng_app_server');
const AUX_PORT = API_PORT + 2;

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const r = spawnSync(MYSQL_BIN,
    ['--socket=' + SOCK, '-uroot', '-p' + pw, '--default-character-set=utf8mb4', '-N', DB_NAME, '-e', sql],
    { encoding: 'utf8', timeout: 30000 });
  if (r.status !== 0) throw new Error((r.stderr || r.stdout || '').trim());
  return r.stdout.trim();
}

function api(port, method, urlPath, body, withCookie = true) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    if (withCookie) withSystemCookie(headers);
    const req = http.request({ host: API_HOST, port, method, path: urlPath, headers, timeout: 120000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        let json = null;
        const text = Buffer.concat(chunks).toString('utf8');
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

function startBackend(port, mockMode, extraEnv = {}) {
  const logFile = path.join(os.tmpdir(), `eng_aiq_${port}_${process.pid}.log`);
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
      ...extraEnv,
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

async function waitTask(port, taskId, timeoutMs = 90000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    const r = await api(port, 'GET', `/api/ai-tasks/${taskId}`);
    if (r.status === 200 && r.json && ['done', 'partial', 'failed'].includes(r.json.status)) return r.json;
    await new Promise(r2 => setTimeout(r2, 500));
  }
  throw new Error('task timeout: ' + taskId);
}

function isSolidified(kpId) {
  return runMysql(`SELECT is_solidified FROM knowledge_points WHERE id=${Number(kpId)};`).trim() === '1';
}

function resetSystemPendingKps() {
  runMysql("DELETE FROM knowledge_points WHERE user_id=1 AND is_solidified=0;");
  runMysql("UPDATE ai_tasks SET status='failed' WHERE user_id=1 AND kind='solidify_kp' AND status IN ('queued','running');");
}

async function main() {
  console.log('=== 阶段二十一：AI 分层队列与多模型兜底（v1.5.0-T002） ===\n');

  if (AI_MOCK_MODE === 'off' || DB_NAME === 'english_learning') {
    skip('AI 队列套件', `需要隔离测试环境，当前 DB=${DB_NAME}, Mock=${AI_MOCK_MODE}`);
    console.log(`\n阶段二十一结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
    process.exit(0);
  }

  const stamp = Date.now().toString(36).slice(-6);
  let aux = null;

  try {
    // ---- 1. L1 复用秒回（同 normalize + 同身份 → 合并，零 AI） ----
    console.log('[1] L1 复用秒回');
    resetSystemPendingKps();
    const wordA = 't021a_' + stamp;
    const idA = await createKp(API_PORT, wordA);
    const solA = await api(API_PORT, 'POST', `/api/kp/${idA}/solidify`);
    if (solA.status === 200 && solA.json && solA.json.ok) ok('首词固化成功', `id=${solA.json.id}`);
    else ko('首词固化成功', `HTTP ${solA.status} ${String(solA.text).slice(0, 100)}`);

    const variant = '  ' + wordA.toUpperCase() + '!  ';
    const idB = await createKp(API_PORT, variant);
    const solB = await api(API_PORT, 'POST', `/api/kp/${idB}/solidify`);
    if (solB.status === 200 && solB.json && solB.json.ok && Number(solB.json.id) === Number(idA))
      ok('L1 命中：变体词复用原词条', `effective_id=${solB.json.id}`);
    else ko('L1 命中：变体词复用原词条', `HTTP ${solB.status} ${String(solB.text).slice(0, 100)}`);
    const getB = await api(API_PORT, 'GET', `/api/kp/${idB}`);
    if (getB.status === 404) ok('L1 命中后被合并词条已删除');
    else ko('L1 命中后被合并词条已删除', `期望404，实际 ${getB.status}`);

    // ---- 2. L2 排队：异步任务字段与完成 ----
    console.log('[2] L2 异步队列');
    resetSystemPendingKps();
    const wordC = 't021c_' + stamp;
    const wordD = 't021d_' + stamp;
    const idC = await createKp(API_PORT, wordC);
    const idD = await createKp(API_PORT, wordD);
    const created = await api(API_PORT, 'POST', '/api/kp/solidify/async');
    if (created.status === 200 && created.json && created.json.job_id)
      ok('任务创建成功', `job_id=${created.json.job_id}, total=${created.json.total}`);
    else ko('任务创建成功', `HTTP ${created.status} ${String(created.text).slice(0, 100)}`);

    const jobId = created.json && created.json.job_id;
    const jobView = await api(API_PORT, 'GET', `/api/kp/solidify/jobs/${jobId}`);
    const hasQueueFields = jobView.json && 'queue_position' in jobView.json && 'eta_sec' in jobView.json;
    if (jobView.status === 200 && jobView.json.total === 2)
      ok('旧接口返回 total=2', `status=${jobView.json.status}`);
    else ko('旧接口返回 total=2', `HTTP ${jobView.status} ${String(jobView.text).slice(0, 120)}`);
    if (hasQueueFields) ok('旧接口包含 queue_position/eta_sec', `pos=${jobView.json.queue_position}, eta=${jobView.json.eta_sec}`);

    let finalJob = jobView.json;
    const start = Date.now();
    while (Date.now() - start < 60000 && finalJob && !['done', 'partial', 'failed'].includes(finalJob.status)) {
      await new Promise(r => setTimeout(r, 500));
      const r2 = await api(API_PORT, 'GET', `/api/kp/solidify/jobs/${jobId}`);
      finalJob = r2.json;
    }
    if (finalJob && finalJob.status === 'done') ok('队列任务完成', `done=${finalJob.done}/${finalJob.total}`);
    else ko('队列任务完成', `status=${finalJob && finalJob.status}`);
    if (finalJob && Array.isArray(finalJob.completed_ids) &&
        finalJob.completed_ids.includes(idC) && finalJob.completed_ids.includes(idD))
      ok('completed_ids 覆盖全部子项');
    else ko('completed_ids 覆盖全部子项', JSON.stringify(finalJob && finalJob.completed_ids));
    if (isSolidified(idC) && isSolidified(idD)) ok('词条均已落库固化');
    else ko('词条均已落库固化');

    // ---- 3. 旧接口列表兼容 ----
    console.log('[3] 旧接口兼容');
    const jobs = await api(API_PORT, 'GET', '/api/kp/solidify/jobs?limit=10');
    if (jobs.status === 200 && Array.isArray(jobs.json.data) && jobs.json.data.some(j => String(j.job_id) === String(jobId)))
      ok('任务列表包含新任务');
    else ko('任务列表包含新任务', `HTTP ${jobs.status}`);

    // ---- 4. /api/ai-tasks ----
    console.log('[4] 通用任务接口');
    const list = await api(API_PORT, 'GET', '/api/ai-tasks?kind=solidify_kp');
    if (list.status === 200 && Array.isArray(list.json.data) && list.json.data.some(t => String(t.id) === String(jobId)))
      ok('任务列表命中', `count=${list.json.data.length}`);
    else ko('任务列表命中', `HTTP ${list.status} ${String(list.text).slice(0, 100)}`);

    const detail = await api(API_PORT, 'GET', `/api/ai-tasks/${jobId}`);
    if (detail.status === 200 && detail.json.status === 'done')
      ok('任务详情 status=done', `progress=${detail.json.progress}`);
    else ko('任务详情 status=done', `HTTP ${detail.status} ${String(detail.text).slice(0, 100)}`);

    const anon = await api(API_PORT, 'GET', `/api/ai-tasks/${jobId}`, null, false);
    if (anon.status === 404) ok('未登录访问他人任务返回 404');
    else ko('未登录访问他人任务返回 404', `期望404，实际 ${anon.status}`);

    const retried = await api(API_PORT, 'POST', `/api/ai-tasks/${jobId}/retry`);
    if (retried.status === 200) ok('重试入口可用');
    else ko('重试入口可用', `HTTP ${retried.status} ${String(retried.text).slice(0, 100)}`);

    const legacyTasks = await api(API_PORT, 'GET', '/api/ai/tasks');
    if (legacyTasks.status === 200) ok('旧 /api/ai/tasks 仍可用');
    else ko('旧 /api/ai/tasks 仍可用', `HTTP ${legacyTasks.status}`);

    // ---- 5. 独立后端：多模型切换 / 队列满 / 断点续跑 ----
    console.log('[5] 独立后端：多模型与队列');
    const primary = 'http://127.0.0.1:9/v1';
    aux = startBackend(AUX_PORT, 'success', {
      ENG_AI_QUEUE_MAX: '1',
      ENG_AI_FAIL_THRESHOLD: '3',
      ENG_AI_DEGRADED_COOLDOWN: '600',
      ENG_AI_PROVIDERS_JSON: JSON.stringify([
        { name: 'primary', base_url: primary, chat_model: 'mock-primary' },
        { name: 'backup', base_url: 'http://127.0.0.1:10/v1', chat_model: 'mock-backup' },
      ]),
      ENG_AI_MOCK_FAIL_BY_URL: JSON.stringify({ [primary]: 3 }),
    });
    await waitHealth(AUX_PORT);

    // 主模型前 3 次失败 → 自动切备 → 固化仍成功
    resetSystemPendingKps();
    let multiOk = 0;
    for (let i = 0; i < 3; i++) {
      const w = `t021p${i}_${stamp}`;
      const id = await createKp(AUX_PORT, w);
      const r = await api(AUX_PORT, 'POST', `/api/kp/${id}/solidify`);
      if (r.status === 200 && r.json && r.json.ok) multiOk++;
    }
    if (multiOk === 3) ok('主模型失败后自动切换备用模型', `${multiOk}/3 成功`);
    else ko('主模型失败后自动切换备用模型', `${multiOk}/3 成功`);

    const health = await api(AUX_PORT, 'GET', '/api/health');
    const providers = (health.json && health.json.ai_providers) || [];
    if (providers.length === 2) ok('health 暴露主备模型摘要', `count=${providers.length}`);
    else ko('health 暴露主备模型摘要', JSON.stringify(providers));
    if (providers[0] && providers[0].degraded === true) ok('主模型连续失败后进入降级');
    else ko('主模型连续失败后进入降级', JSON.stringify(providers[0]));
    if (health.json && health.json.ai_degraded === false) ok('备用可用时全局未降级');
    else ko('备用可用时全局未降级', JSON.stringify(health.json && health.json.ai_degraded));

    // 队列满 429：占位一个不可执行的 queued 任务，使 max_size=1 达到上限
    // 占位任务归属其他用户/类型，避免被 createOrReuse 当作当前用户可复用任务
    resetSystemPendingKps();
    const queueWord = 't021q_' + stamp;
    await createKp(AUX_PORT, queueWord);
    runMysql("INSERT INTO ai_tasks (task_type,status,user_id,kind,lane,total,progress,retry_count,"
      + "next_run_at,update_time) VALUES ('exercise','queued',2,'exercise','exercise',0,0,0,"
      + "DATE_ADD(NOW(), INTERVAL 1 HOUR),NOW());");
    const placeholderId = runMysql("SELECT MAX(id) FROM ai_tasks;");
    const full = await api(AUX_PORT, 'POST', '/api/kp/solidify/async');
    if (full.status === 429) ok('队列满返回 429');
    else ko('队列满返回 429', `期望429，实际 ${full.status} ${String(full.text).slice(0, 100)}`);
    runMysql(`DELETE FROM ai_tasks WHERE id=${Number(placeholderId)};`);

    // 断点续跑：模拟服务重启后遗留 running 任务
    resetSystemPendingKps();
    const resumeWord = 't021r_' + stamp;
    const resumeKp = await createKp(AUX_PORT, resumeWord);
    const legacyJob = runMysql(`INSERT INTO ai_tasks (task_type,status,user_id,kind,lane,total,progress,retry_count,started_at,update_time) VALUES ('solidify_kp','running',1,'solidify_kp','solidify',1,0,0,NOW(),NOW()); SELECT MAX(id) FROM ai_tasks WHERE user_id=1 AND kind='solidify_kp';`);
    const resumeTaskId = Number(legacyJob.split('\n').pop().trim());
    runMysql(`INSERT INTO ai_task_items (task_id,item_index,item_type,ref_id,input_text,status,started_at) VALUES (${resumeTaskId},0,'solidify_kp',${resumeKp},'${resumeWord}','running',NOW());`);

    await stopBackend(aux.proc);
    aux = startBackend(AUX_PORT, 'success', {
      ENG_AI_QUEUE_MAX: '1',
      ENG_AI_PROVIDERS_JSON: JSON.stringify([
        { name: 'primary', base_url: primary, chat_model: 'mock-primary' },
        { name: 'backup', base_url: 'http://127.0.0.1:10/v1', chat_model: 'mock-backup' },
      ]),
    });
    await waitHealth(AUX_PORT);

    const recovered = await waitTask(AUX_PORT, resumeTaskId, 90000);
    if (['done', 'partial'].includes(recovered.status) && recovered.done >= 1)
      ok('重启后 running 任务恢复并完成', `status=${recovered.status}, done=${recovered.done}`);
    else ko('重启后 running 任务恢复并完成', `status=${recovered && recovered.status}`);
    if (isSolidified(resumeKp)) ok('断点续跑子项已固化');
    else ko('断点续跑子项已固化');

  } catch (e) {
    ko('套件异常', String(e && e.message || e));
  } finally {
    if (aux) await stopBackend(aux.proc);
  }

  console.log(`\n阶段二十一结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
  if (fail > 0) {
    console.log('失败项:');
    for (const f of failures) console.log('  - ' + f.name + ': ' + f.msg);
    process.exit(1);
  }
}

main();
