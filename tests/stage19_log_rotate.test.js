// 阶段十九：日志轮转（v1.4.1-T006）
// 覆盖：默认配置、超阈值切分、7 份保留、启动清理、缺失文件、watcher 进程参数与事件日志不泄漏密钥
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'eng_log_rotate_test_'));
const lr = require('../scripts/log_rotate');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function writeBytes(file, size, tail = '') {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const base = 'x'.repeat(Math.max(0, size - tail.length));
  fs.writeFileSync(file, base + tail);
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

function stopProc(proc) {
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
  console.log('=== 阶段十九：日志轮转 ===\n');

  console.log('[1] 默认配置');
  const cfg = lr.loadConfig();
  if (cfg.maxBytes === 10 * 1024 * 1024) ok('默认阈值 10MB');
  else ko('默认阈值 10MB', String(cfg.maxBytes));
  if (cfg.keepFiles === 7) ok('默认保留 7 份');
  else ko('默认保留 7 份', String(cfg.keepFiles));
  if (cfg.intervalSec === 60 && cfg.files.includes('localai.out') && cfg.files.includes('eng_app.out')) {
    ok('默认间隔与文件清单');
  } else ko('默认间隔与文件清单', JSON.stringify(cfg));

  console.log('[2] 超阈值切分（copytruncate）');
  const dir = path.join(TMP, 'rotate');
  const active = path.join(dir, 'eng_app.out');
  writeBytes(active, 1500, 'GEN1');
  const r1 = lr.rotateFile(active, { maxBytes: 1000, keepFiles: 3 });
  const a1 = active + '.1';
  if (r1.rotated && fs.existsSync(a1) && fs.statSync(a1).size === 1500) ok('超阈值生成 .1 且内容完整');
  else ko('超阈值生成 .1 且内容完整', JSON.stringify(r1));
  if (fs.statSync(active).size === 0) ok('活动文件被清空');
  else ko('活动文件被清空', String(fs.statSync(active).size));
  if (fs.readFileSync(a1, 'utf8').endsWith('GEN1')) ok('归档保留原内容');
  else ko('归档保留原内容', '内容不匹配');

  const r2 = lr.rotateFile(active, { maxBytes: 1000, keepFiles: 3 });
  if (!r2.rotated && r2.reason === 'below-threshold') ok('未达阈值不轮转');
  else ko('未达阈值不轮转', JSON.stringify(r2));

  console.log('[3] 保留份数上限');
  for (let i = 2; i <= 5; i++) {
    writeBytes(active, 1200, 'GEN' + i);
    lr.rotateFile(active, { maxBytes: 1000, keepFiles: 3 });
  }
  const archives = [1, 2, 3, 4, 5].map(i => active + '.' + i);
  if (fs.existsSync(active + '.1') && fs.existsSync(active + '.2') && fs.existsSync(active + '.3')) ok('保留最近 3 份归档');
  else ko('保留最近 3 份归档', archives.map(a => path.basename(a) + '=' + fs.existsSync(a)).join(','));
  if (!fs.existsSync(active + '.4') && !fs.existsSync(active + '.5')) ok('超出份数的归档被删除');
  else ko('超出份数的归档被删除', '发现 .4/.5');
  if (fs.readFileSync(active + '.1', 'utf8').endsWith('GEN5')) ok('.1 为最新归档');
  else ko('.1 为最新归档', '内容不匹配');

  console.log('[4] 启动清理');
  const cleanupDir = path.join(TMP, 'cleanup');
  fs.mkdirSync(cleanupDir, { recursive: true });
  for (let i = 1; i <= 9; i++) writeBytes(path.join(cleanupDir, 'localai.out.' + i), 20, 'A' + i);
  const old = new Date(Date.now() - 2 * 60 * 60 * 1000);
  const tmpFile = path.join(cleanupDir, 'localai.out.1.tmp');
  const lockFile = path.join(cleanupDir, 'localai.out.rotate.lock');
  writeBytes(tmpFile, 20, 'TMP');
  writeBytes(lockFile, 20, 'LOCK');
  fs.utimesSync(tmpFile, old, old);
  fs.utimesSync(lockFile, old, old);
  const c = lr.cleanupDir(cleanupDir, { files: ['localai.out'], keepFiles: 7 });
  if (c.removed === 4) ok('清理超量归档、残留临时文件与残留锁', 'removed=' + c.removed);
  else ko('清理超量归档、残留临时文件与残留锁', 'removed=' + c.removed);
  const remain = [1, 2, 3, 4, 5, 6, 7, 8, 9].filter(i => fs.existsSync(path.join(cleanupDir, 'localai.out.' + i)));
  if (remain.join(',') === '1,2,3,4,5,6,7') ok('仅保留 1~7 份');
  else ko('仅保留 1~7 份', remain.join(','));
  if (!fs.existsSync(tmpFile) && !fs.existsSync(lockFile)) ok('残留临时文件与锁已删除');
  else ko('残留临时文件与锁已删除', '仍存在');

  console.log('[5] 缺失文件');
  const missing = lr.rotateFile(path.join(TMP, 'none.log'), { maxBytes: 100, keepFiles: 3 });
  if (!missing.rotated && missing.reason === 'missing') ok('缺失文件安全跳过');
  else ko('缺失文件安全跳过', JSON.stringify(missing));

  console.log('[6] watcher 进程参数与事件日志不泄漏密钥');
  const watchDir = path.join(TMP, 'watch');
  fs.mkdirSync(watchDir, { recursive: true });
  const marker = 'STAGE19_LOG_CONTENT_SECRET';
  fs.writeFileSync(path.join(watchDir, 'localai.out'), 'log line ' + marker + '\n');
  const outFile = path.join(watchDir, 'log_rotate.out');
  const secret = 'stage19-process-secret-key';
  const proc = spawn(process.execPath, [path.join(ROOT, 'scripts/log_rotate.js'), '--watch', '--dir', watchDir], {
    env: { ...process.env, APP_LOG_DIR: watchDir, ZHIPU_API_KEY: secret },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let output = '';
  proc.stdout.on('data', c => output += c);
  proc.stderr.on('data', c => output += c);
  await sleep(1200);
  let psOut = '';
  try {
    const r = spawnSync('ps', ['-o', 'args=', '-p', String(proc.pid)], { encoding: 'utf8' });
    psOut = r.stdout || '';
  } catch { /* ignore */ }
  if (psOut.includes('log_rotate.js') && !psOut.includes(secret)) ok('watcher 进程参数不含密钥');
  else ko('watcher 进程参数不含密钥', psOut.trim() || '无法读取进程参数');
  await stopProc(proc);
  if (!output.includes(marker) && !output.includes(secret)) ok('watcher 输出不含日志正文与密钥');
  else ko('watcher 输出不含日志正文与密钥', '发现泄漏');
  if (!fs.existsSync(outFile) || !fs.readFileSync(outFile, 'utf8').includes(marker)) ok('事件日志不含日志正文');
  else ko('事件日志不含日志正文', '发现日志正文');

  console.log('[7] 启动/停止脚本接入');
  const startAll = fs.readFileSync(path.join(ROOT, 'start_all.command'), 'utf8');
  const devScript = fs.readFileSync(path.join(ROOT, 'dev'), 'utf8');
  const stopAll = fs.readFileSync(path.join(ROOT, 'scripts/stop_all.sh'), 'utf8');
  const entrypoint = fs.readFileSync(path.join(ROOT, 'docker/entrypoint.sh'), 'utf8');
  if (startAll.includes('log_rotate.js') && startAll.includes('--watch') && /nohup[^\n]*log_rotate\.js[^\n]*&/.test(startAll)) {
    ok('start_all 非阻塞拉起 watcher');
  } else ko('start_all 非阻塞拉起 watcher', '未找到后台启动');
  if (devScript.includes('start_log_rotate') && devScript.includes('log_rotate.pid')) ok('dev 已接入 watcher');
  else ko('dev 已接入 watcher', '未找到');
  if (stopAll.includes('log_rotate.pid')) ok('stop_all 可停止 watcher');
  else ko('stop_all 可停止 watcher', '未找到');
  if (entrypoint.includes('log_rotate.js') && entrypoint.includes('LOG_ROTATE_PID')) ok('容器 entrypoint 已接入 watcher');
  else ko('容器 entrypoint 已接入 watcher', '未找到');

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
    fs.rmSync(TMP, { recursive: true, force: true });
  });
