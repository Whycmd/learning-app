#!/usr/bin/env node
// 日志轮转（v1.4.1-T006）
//
// 职责：只做"按大小切分 + 保留 N 份 + 启动清理"，不改日志内容，不读取密钥。
// 原理：现有日志由 shell 重定向或 Node 追加流持有文件句柄，直接 rename 会让写入方
//       继续写旧文件；因此采用 copytruncate：先快照到 .1，再原地截断活动文件。
//
// [CONFIG] 配置在 config/log.json，可改阈值、保留份数、检查间隔与文件清单。
// 用法：
//   node scripts/log_rotate.js --once              # 执行一次清理与轮转
//   node scripts/log_rotate.js --watch             # 后台常驻，按间隔检查
//   node scripts/log_rotate.js --dir <日志目录>     # 覆盖日志目录（默认 ~/.local/log）
const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const DEFAULT_MAX_MB = 10;
const DEFAULT_KEEP = 7;
const DEFAULT_INTERVAL_SEC = 60;
const DEFAULT_FILES = ['eng_app.out', 'backend.out', 'localai.out', 'log_rotate.out'];
const LOCK_STALE_MS = 5 * 60 * 1000;
const TMP_STALE_MS = 60 * 60 * 1000;

function loadConfig() {
  let raw = {};
  try {
    raw = JSON.parse(fs.readFileSync(path.join(ROOT, 'config/log.json'), 'utf8'));
  } catch {
    raw = {};
  }
  const maxMb = Number(raw.max_size_mb);
  const keep = Number(raw.keep_files);
  const interval = Number(raw.check_interval_sec);
  const files = Array.isArray(raw.files) && raw.files.length
    ? raw.files.filter(f => typeof f === 'string' && f.trim()).map(f => path.basename(f.trim()))
    : DEFAULT_FILES;
  return {
    maxBytes: (Number.isFinite(maxMb) && maxMb > 0 ? maxMb : DEFAULT_MAX_MB) * 1024 * 1024,
    keepFiles: Number.isInteger(keep) && keep >= 1 ? keep : DEFAULT_KEEP,
    intervalSec: Number.isFinite(interval) && interval > 0 ? interval : DEFAULT_INTERVAL_SEC,
    files,
  };
}

// 日志目录优先级：命令行 --dir → APP_LOG_DIR → LOCALAI_LOG_DIR → ~/.local/log
function resolveLogDir(cliDir) {
  if (cliDir) return path.resolve(cliDir);
  if (process.env.APP_LOG_DIR) return path.resolve(process.env.APP_LOG_DIR);
  if (process.env.LOCALAI_LOG_DIR) return path.resolve(process.env.LOCALAI_LOG_DIR);
  return path.join(os.homedir(), '.local', 'log');
}

function archiveName(file, index) {
  return file + '.' + index;
}

function isStale(file, ms) {
  try {
    return Date.now() - fs.statSync(file).mtimeMs > ms;
  } catch {
    return false;
  }
}

function escapeRegExp(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// 按快照大小复制前 size 字节，避免把"复制期间新追加的内容"也算进去
function copySnapshot(src, dst, size) {
  const inFd = fs.openSync(src, 'r');
  const outFd = fs.openSync(dst, 'w');
  const buf = Buffer.alloc(1024 * 1024);
  let pos = 0;
  try {
    while (pos < size) {
      const want = Math.min(buf.length, size - pos);
      const n = fs.readSync(inFd, buf, 0, want, pos);
      if (n <= 0) break;
      fs.writeSync(outFd, buf, 0, n, pos);
      pos += n;
    }
  } finally {
    fs.closeSync(inFd);
    fs.closeSync(outFd);
  }
  return pos;
}

// 单文件轮转：返回 { rotated, size, reason }
function rotateFile(file, opts = {}) {
  const maxBytes = opts.maxBytes || DEFAULT_MAX_MB * 1024 * 1024;
  const keep = opts.keepFiles || DEFAULT_KEEP;
  let st;
  try {
    st = fs.statSync(file);
  } catch (e) {
    if (e.code === 'ENOENT') return { rotated: false, reason: 'missing' };
    throw e;
  }
  if (!st.isFile()) return { rotated: false, reason: 'not-file' };
  if (st.size < maxBytes) return { rotated: false, reason: 'below-threshold', size: st.size };

  const lock = file + '.rotate.lock';
  let lockFd = null;
  try {
    lockFd = fs.openSync(lock, 'wx');
  } catch (e) {
    if (e.code !== 'EEXIST') throw e;
    if (isStale(lock, LOCK_STALE_MS)) {
      fs.rmSync(lock, { force: true });
      return rotateFile(file, opts);
    }
    return { rotated: false, reason: 'locked' };
  }

  try {
    fs.writeSync(lockFd, String(process.pid));
    fs.rmSync(archiveName(file, keep), { force: true });
    for (let i = keep - 1; i >= 1; i--) {
      const from = archiveName(file, i);
      if (!fs.existsSync(from)) continue;
      const to = archiveName(file, i + 1);
      fs.rmSync(to, { force: true });
      fs.renameSync(from, to);
    }
    const tmp = file + '.1.tmp';
    copySnapshot(file, tmp, st.size);
    fs.rmSync(archiveName(file, 1), { force: true });
    fs.renameSync(tmp, archiveName(file, 1));
    fs.truncateSync(file, 0);
    return { rotated: true, size: st.size, archive: archiveName(file, 1) };
  } finally {
    try { fs.closeSync(lockFd); } catch { /* ignore */ }
    try { fs.rmSync(lock, { force: true }); } catch { /* ignore */ }
  }
}

// 启动清理：删除超出保留份数的归档、超时临时文件与残留锁
function cleanupDir(dir, cfg) {
  let entries = [];
  try {
    entries = fs.readdirSync(dir);
  } catch {
    return { removed: 0 };
  }
  let removed = 0;
  for (const base of cfg.files) {
    const escaped = escapeRegExp(base);
    const archiveRe = new RegExp('^' + escaped + '\\.(\\d+)$');
    const tmpRe = new RegExp('^' + escaped + '\\.(\\d+)\\.tmp$');
    for (const name of entries) {
      const full = path.join(dir, name);
      const m = archiveRe.exec(name);
      if (m && Number(m[1]) > cfg.keepFiles) {
        fs.rmSync(full, { force: true });
        removed++;
        continue;
      }
      if (tmpRe.test(name) && isStale(full, TMP_STALE_MS)) {
        fs.rmSync(full, { force: true });
        removed++;
        continue;
      }
      if (name === base + '.rotate.lock' && isStale(full, LOCK_STALE_MS)) {
        fs.rmSync(full, { force: true });
        removed++;
      }
    }
  }
  return { removed };
}

function formatMb(bytes) {
  return (bytes / (1024 * 1024)).toFixed(1) + 'MB';
}

function appendEvent(dir, line) {
  try {
    fs.appendFileSync(path.join(dir, 'log_rotate.out'), '[' + new Date().toISOString() + '] ' + line + '\n');
  } catch {
    // 事件记录失败不影响轮转
  }
}

// 执行一次清理 + 轮转；opts.eventLog=false 时不写事件文件（供测试使用）
function runOnce(dir, cfg = loadConfig(), opts = {}) {
  fs.mkdirSync(dir, { recursive: true });
  const cleaned = cleanupDir(dir, cfg).removed;
  let rotated = 0;
  const events = [];
  for (const name of cfg.files) {
    const file = path.join(dir, name);
    const r = rotateFile(file, { maxBytes: cfg.maxBytes, keepFiles: cfg.keepFiles });
    if (r.rotated) {
      rotated++;
      events.push('rotated ' + name + ' (' + formatMb(r.size) + ') -> ' + path.basename(r.archive));
    }
  }
  if (opts.eventLog !== false) {
    if (cleaned > 0) appendEvent(dir, 'cleanup removed ' + cleaned + ' stale file(s)');
    events.forEach(e => appendEvent(dir, e));
  }
  return { cleaned, rotated, events };
}

function startWatcher(dir, cfg = loadConfig()) {
  fs.mkdirSync(dir, { recursive: true });
  let running = false;
  const tick = () => {
    if (running) return;
    running = true;
    try {
      runOnce(dir, cfg);
    } catch (e) {
      try { appendEvent(dir, 'error: ' + e.message); } catch { /* ignore */ }
    } finally {
      running = false;
    }
  };
  tick();
  const timer = setInterval(tick, cfg.intervalSec * 1000);
  const stop = () => {
    clearInterval(timer);
    process.exit(0);
  };
  process.on('SIGTERM', stop);
  process.on('SIGINT', stop);
  return timer;
}

function main() {
  const args = process.argv.slice(2);
  let dir = '';
  let watch = false;
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--dir' && i + 1 < args.length) dir = args[++i];
    else if (args[i] === '--watch') watch = true;
    else if (args[i] === '--once') watch = false;
    else if (args[i] === '--help' || args[i] === '-h') {
      console.log('用法：node scripts/log_rotate.js [--once|--watch] [--dir <日志目录>]');
      return;
    }
  }
  const cfg = loadConfig();
  const logDir = resolveLogDir(dir);
  if (watch) {
    startWatcher(logDir, cfg);
    console.log('[log-rotate] watching ' + logDir + ' (max=' + formatMb(cfg.maxBytes)
      + ', keep=' + cfg.keepFiles + ', interval=' + cfg.intervalSec + 's)');
  } else {
    const r = runOnce(logDir, cfg);
    console.log('[log-rotate] dir=' + logDir + ' cleaned=' + r.cleaned + ' rotated=' + r.rotated);
  }
}

if (require.main === module) {
  try {
    main();
  } catch (e) {
    console.error('[log-rotate] 失败：' + e.message);
    process.exit(1);
  }
}

module.exports = {
  loadConfig,
  resolveLogDir,
  rotateFile,
  cleanupDir,
  runOnce,
  startWatcher,
};
