// 阶段十八：API Key 安全存储（密钥文件）
// 覆盖：读取优先级 文件 → 环境变量 → 配置；0600 权限校验；迁移脚本；网关 /readyz 与日志安全
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn, spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'eng_key_test_'));
const KEY_FILE = path.join(TMP, 'ai.key');
const LOG_DIR = path.join(TMP, 'log');

// 必须在 require 网关模块前设置，KEY_FILE_PATH 在模块加载时确定
process.env.ENG_AI_KEY_FILE = KEY_FILE;
delete process.env.ZHIPU_API_KEY;
const gw = require('../scripts/local_ai_server');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function writeKey(content, mode = 0o600) {
  fs.mkdirSync(path.dirname(KEY_FILE), { recursive: true });
  fs.writeFileSync(KEY_FILE, content + '\n');
  fs.chmodSync(KEY_FILE, mode);
}

function removeKey() {
  try { fs.rmSync(KEY_FILE, { force: true }); } catch { /* ignore */ }
}

function modeOf(file) {
  const r = spawnSync('stat', ['-c', '%a', file], { encoding: 'utf8' });
  if (r.status === 0) return r.stdout.trim();
  const r2 = spawnSync('stat', ['-f', '%Lp', file], { encoding: 'utf8' });
  return r2.status === 0 ? r2.stdout.trim() : 'unknown';
}

function httpGet(port, urlPath) {
  return new Promise((resolve, reject) => {
    const req = http.get({ host: '127.0.0.1', port, path: urlPath, timeout: 5000 }, res => {
      let text = '';
      res.on('data', c => text += c);
      res.on('end', () => {
        try { resolve({ status: res.statusCode, json: JSON.parse(text), text }); }
        catch { resolve({ status: res.statusCode, json: null, text }); }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
  });
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function waitReady(port, timeoutMs = 5000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try { return await httpGet(port, '/readyz'); } catch { /* retry */ }
    await sleep(200);
  }
  throw new Error(`gateway not ready on ${port}`);
}

function runScript(args, input, extraEnv = {}) {
  return spawnSync('bash', [path.join(ROOT, 'scripts/ai_key_store.sh'), ...args], {
    encoding: 'utf8',
    input,
    env: { ...process.env, ENG_AI_KEY_FILE: KEY_FILE, ...extraEnv },
    timeout: 10000,
  });
}

async function spawnGateway(port, envOverride, logDir) {
  fs.mkdirSync(logDir, { recursive: true });
  const childEnv = { ...process.env, ...envOverride, LOCALAI_PORT: String(port), LOCALAI_HOST: '127.0.0.1', LOCALAI_LOG_DIR: logDir };
  const proc = spawn(process.execPath, [path.join(ROOT, 'scripts/local_ai_server.js')], {
    env: childEnv,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  proc.stdout.on('data', c => output += c);
  proc.stderr.on('data', c => output += c);
  return { proc, output: () => output };
}

function stopGateway(proc) {
  return new Promise(resolve => {
    if (!proc || proc.exitCode !== null) return resolve();
    let done = false;
    const finish = () => { if (!done) { done = true; resolve(); } };
    proc.once('exit', finish);
    try { proc.kill('SIGTERM'); } catch { /* ignore */ }
    setTimeout(() => { try { proc.kill('SIGKILL'); } catch { /* ignore */ } finish(); }, 3000);
  });
}

async function main() {
  console.log('=== 阶段十八：API Key 安全存储 ===\n');

  console.log('[1] 读取优先级（文件 → 环境变量 → 配置）');
  writeKey('test-file-key');
  process.env.ZHIPU_API_KEY = 'test-env-key';
  let r = gw.resolveApiKey('test-config-key');
  if (r.key === 'test-file-key' && r.source === 'file') ok('密钥文件优先', 'source=file');
  else ko('密钥文件优先', `实际 ${r.source}:${r.key}`);

  fs.chmodSync(KEY_FILE, 0o644);
  r = gw.resolveApiKey('test-config-key');
  if (r.key === 'test-env-key' && r.source === 'env') ok('权限过宽时回退环境变量', 'source=env');
  else ko('权限过宽时回退环境变量', `实际 ${r.source}:${r.key}`);

  removeKey();
  r = gw.resolveApiKey('test-config-key');
  if (r.key === 'test-env-key' && r.source === 'env') ok('无文件时使用环境变量', 'source=env');
  else ko('无文件时使用环境变量', `实际 ${r.source}:${r.key}`);

  delete process.env.ZHIPU_API_KEY;
  r = gw.resolveApiKey('test-config-key');
  if (r.key === 'test-config-key' && r.source === 'config') ok('无文件无环境变量时使用配置', 'source=config');
  else ko('无文件无环境变量时使用配置', `实际 ${r.source}:${r.key}`);

  r = gw.resolveApiKey('');
  if (r.key === '' && r.source === 'none') ok('三者均无时返回 none');
  else ko('三者均无时返回 none', `实际 ${r.source}:${r.key}`);

  writeKey('中文Key');
  process.env.ZHIPU_API_KEY = 'test-env-key';
  r = gw.resolveApiKey('test-config-key');
  if (r.source === 'env') ok('非法文件内容被忽略');
  else ko('非法文件内容被忽略', `实际 ${r.source}:${r.key}`);

  writeKey('YOUR_API_KEY_HERE');
  r = gw.resolveApiKey('');
  if (r.source === 'env') ok('占位 Key 被忽略');
  else ko('占位 Key 被忽略', `实际 ${r.source}:${r.key}`);
  delete process.env.ZHIPU_API_KEY;

  console.log('[2] 维护脚本 set / check / delete');
  removeKey();
  let s = runScript(['set'], 'script-test-key\n');
  if (s.status === 0 && fs.existsSync(KEY_FILE) && fs.readFileSync(KEY_FILE, 'utf8').trim() === 'script-test-key') ok('set 写入密钥');
  else ko('set 写入密钥', s.stderr || s.stdout);
  if (modeOf(KEY_FILE) === '600') ok('密钥文件权限为 600');
  else ko('密钥文件权限为 600', modeOf(KEY_FILE));

  s = runScript(['check'], '');
  if (s.status === 0 && !s.stdout.includes('script-test-key') && s.stdout.includes('已配置')) ok('check 不输出 Key');
  else ko('check 不输出 Key', s.stdout + s.stderr);

  s = runScript(['set'], 'bad key with space\n');
  if (s.status !== 0) ok('set 拒绝含空格 Key');
  else ko('set 拒绝含空格 Key', '意外成功');

  s = runScript(['delete'], '');
  if (s.status === 0 && !fs.existsSync(KEY_FILE)) ok('delete 删除密钥文件');
  else ko('delete 删除密钥文件', s.stderr || s.stdout);

  console.log('[3] 迁移脚本 migrate');
  const tmpConfig = path.join(TMP, 'ai.json');
  fs.writeFileSync(tmpConfig, JSON.stringify({
    provider_type: 'openai_compatible',
    base_url: 'http://127.0.0.1:5340/v1',
    chat_model: 'glm-4-flash',
    request_timeout_sec: 180,
    api_key: 'migrate-key-123',
  }, null, 2) + '\n');
  s = runScript(['migrate'], '', { AI_CONFIG_FILE: tmpConfig });
  const migratedCfg = JSON.parse(fs.readFileSync(tmpConfig, 'utf8'));
  if (s.status === 0 && fs.readFileSync(KEY_FILE, 'utf8').trim() === 'migrate-key-123'
      && migratedCfg.api_key === '' && migratedCfg.chat_model === 'glm-4-flash') ok('migrate 写入文件并清空配置');
  else ko('migrate 写入文件并清空配置', s.stderr || JSON.stringify(migratedCfg));
  if (!s.stdout.includes('migrate-key-123')) ok('migrate 输出不含 Key');
  else ko('migrate 输出不含 Key', s.stdout);

  console.log('[4] 配置文件不得保留明文 Key');
  const realCfg = JSON.parse(fs.readFileSync(path.join(ROOT, 'config/ai.json'), 'utf8'));
  if (gw.isPlaceholderKey(realCfg.api_key)) ok('config/ai.json 的 api_key 为空或占位值');
  else ko('config/ai.json 的 api_key 为空或占位值', '仍存在明文 Key，请执行 bash scripts/ai_key_store.sh migrate');

  console.log('[5] 网关 /readyz 与日志安全');
  // 5.1 密钥文件生效
  writeKey('gateway-file-key');
  let g = await spawnGateway(15340, {}, path.join(LOG_DIR, 'file'));
  try {
    const res = await waitReady(15340);
    if (res.json && res.json.status === 'ok' && res.json.api_key_configured === true && res.json.api_key_source === 'file') {
      ok('文件来源时 /readyz 报 ok', 'source=file');
    } else ko('文件来源时 /readyz 报 ok', JSON.stringify(res.json));
    await sleep(200);
    const logText = fs.existsSync(path.join(LOG_DIR, 'file', 'localai.out'))
      ? fs.readFileSync(path.join(LOG_DIR, 'file', 'localai.out'), 'utf8') : '';
    if (!g.output().includes('gateway-file-key') && !logText.includes('gateway-file-key')) ok('日志与输出不含 Key');
    else ko('日志与输出不含 Key', '发现 Key 泄漏');
  } finally {
    await stopGateway(g.proc);
  }

  // 5.2 环境变量回退
  removeKey();
  g = await spawnGateway(15340, { ZHIPU_API_KEY: 'gateway-env-key' }, path.join(LOG_DIR, 'env'));
  try {
    const res = await waitReady(15340);
    if (res.json && res.json.status === 'ok' && res.json.api_key_source === 'env') ok('无文件时回退环境变量', 'source=env');
    else ko('无文件时回退环境变量', JSON.stringify(res.json));
  } finally {
    await stopGateway(g.proc);
  }

  console.log(`\n结果：${pass} 通过，${fail} 失败，${skipped} 跳过\n`);
  if (fail > 0) {
    console.log('失败项：');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`));
  }
  process.exitCode = fail > 0 ? 1 : 0;
}

main()
  .catch(e => { fail++; console.error('  ✗ 未捕获异常 — ' + e.message); process.exitCode = 1; })
  .finally(() => {
    removeKey();
    fs.rmSync(TMP, { recursive: true, force: true });
  });
