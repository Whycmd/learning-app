#!/usr/bin/env node
// v1.5.0-T011 OPS-BASELINE：每日 mysqldump 备份（默认保留 7 天）
//
// 用法：
//   node scripts/backup_daily.js --once              # 备份一次（当日已有则跳过）
//   node scripts/backup_daily.js --once --force      # 强制再备份一次
//   node scripts/backup_daily.js --watch             # 常驻，按 config/ops.json 的 daily_time 每日执行
//   node scripts/backup_daily.js --once --dir /tmp/bk --retain-days 3
//
// 规则：
//   - 只清理 english_learning_daily_*.sql，手动备份 english_learning_*.sql 永不清理
//   - 先写 .tmp 再 rename，避免半成品被当成有效备份
//   - 密码通过 MYSQL_PWD 传给子进程，不出现在命令行
const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const DAILY_PREFIX = 'english_learning_daily_';
const MANUAL_PREFIX = 'english_learning_';

function env(key, fallback) {
  const v = process.env[key];
  return v === undefined || v === '' ? fallback : v;
}

function readJson(file) {
  try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return null; }
}

function hasFlag(name) { return process.argv.includes('--' + name); }

function arg(name, fallback) {
  const i = process.argv.indexOf('--' + name);
  if (i < 0) return fallback;
  const v = process.argv[i + 1];
  return v && !v.startsWith('--') ? v : fallback;
}

function log(msg) {
  const t = new Date().toLocaleString('zh-CN', { hour12: false });
  console.log(`[backup ${t}] ${msg}`);
}

function pad(n) { return String(n).padStart(2, '0'); }
function ymd(d) { return `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}`; }
function hms(d) { return `${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`; }

function which(name) {
  const r = spawnSync('which', [name], { encoding: 'utf8' });
  return r.status === 0 ? r.stdout.trim() : '';
}

// 查找 mysqldump：环境变量 > 配置 > 便携版 > PATH（含 mariadb-dump 兼容名）
function findDump(ops) {
  const cands = [
    env('MYSQLDUMP_BIN', ''),
    (ops && ops.backup && ops.backup.mysqldump_bin) || '',
    path.join(os.homedir(), '.local/mysql/bin/mysqldump'),
    path.join(os.homedir(), '.local/mysql/bin/mariadb-dump'),
    which('mysqldump'),
    which('mariadb-dump'),
  ];
  return cands.find((p) => p && fs.existsSync(p)) || '';
}

function readPassword(db, configDir) {
  if (db.password_file) {
    const pwFile = path.resolve(configDir, db.password_file.replace(/^~(?=$|\/)/, os.homedir()));
    try { return fs.readFileSync(pwFile, 'utf8').trim(); } catch { /* 落到 password 字段 */ }
  }
  return db.password || '';
}

// 当天是否已有自动备份
function hasTodayBackup(dir) {
  if (!fs.existsSync(dir)) return false;
  const today = ymd(new Date());
  return fs.readdirSync(dir).some((f) => f.startsWith(DAILY_PREFIX + today) && f.endsWith('.sql'));
}

// 清理超过保留天数的自动备份；手动备份不参与
function cleanup(dir, retainDays) {
  if (!fs.existsSync(dir)) return 0;
  const now = new Date();
  const cutoff = new Date(now.getFullYear(), now.getMonth(), now.getDate());
  cutoff.setDate(cutoff.getDate() - (retainDays - 1)); // 保留含今天在内的 retainDays 个自然日
  let removed = 0;
  for (const name of fs.readdirSync(dir)) {
    if (!name.startsWith(DAILY_PREFIX) || !name.endsWith('.sql')) continue;
    const m = name.match(/^english_learning_daily_(\d{8})_/);
    let fileDay = null;
    if (m) fileDay = new Date(+m[1].slice(0, 4), +m[1].slice(4, 6) - 1, +m[1].slice(6, 8));
    if (!fileDay) {
      const st = fs.statSync(path.join(dir, name));
      fileDay = new Date(st.mtime.getFullYear(), st.mtime.getMonth(), st.mtime.getDate());
    }
    if (fileDay < cutoff) {
      try { fs.unlinkSync(path.join(dir, name)); removed++; log(`已清理过期备份 ${name}`); } catch (e) { log(`清理失败 ${name}: ${e.message}`); }
    }
  }
  return removed;
}

function runBackup(opts) {
  const configDir = path.join(ROOT, 'config');
  const db = readJson(path.join(configDir, 'db.json'));
  if (!db || !db.database) {
    log('无法读取 config/db.json 或缺少 database 字段');
    return false;
  }
  const dir = opts.dir;
  fs.mkdirSync(dir, { recursive: true });

  if (!opts.force && hasTodayBackup(dir)) {
    log(`今日已有自动备份，跳过（--force 可强制）`);
    cleanup(dir, opts.retainDays);
    return true;
  }

  const bin = findDump(opts.ops);
  if (!bin) {
    log('未找到 mysqldump / mariadb-dump，可通过 config/ops.json 的 backup.mysqldump_bin 指定');
    return false;
  }

  const args = [
    '--user=' + (db.user || 'root'),
    '--default-character-set=utf8mb4',
    '--single-transaction',
    '--quick',
    '--no-tablespaces',
    '--skip-comments',
  ];
  // 本机优先 socket；容器内 socket 不存在时走 TCP
  if (db.socket && fs.existsSync(db.socket)) {
    args.push('--socket=' + db.socket);
  } else {
    args.push('--host=' + (db.host || '127.0.0.1'), '--port=' + String(db.port || 3306), '--protocol=TCP');
  }
  args.push(db.database);

  const now = new Date();
  const file = path.join(dir, `${DAILY_PREFIX}${ymd(now)}_${hms(now)}.sql`);
  const tmp = file + '.tmp';
  let fd = -1;
  try {
    fd = fs.openSync(tmp, 'w', 0o600);
  } catch (e) {
    log(`无法创建备份文件 ${tmp}: ${e.message}`);
    return false;
  }

  const pw = readPassword(db, configDir);
  const r = spawnSync(bin, args, {
    stdio: ['ignore', fd, 'pipe'],
    env: { ...process.env, MYSQL_PWD: pw },
    timeout: 10 * 60 * 1000,
  });
  fs.closeSync(fd);

  const err = (r.stderr || '').toString().trim();
  let size = 0;
  try { size = fs.statSync(tmp).size; } catch { /* ignore */ }

  if (r.status !== 0 || size === 0) {
    log(`备份失败（exit=${r.status}，size=${size}）${err ? ': ' + err.split('\n').slice(0, 3).join(' | ') : ''}`);
    try { fs.unlinkSync(tmp); } catch { /* ignore */ }
    return false;
  }

  try { fs.renameSync(tmp, file); } catch (e) {
    log(`备份重命名失败: ${e.message}`);
    try { fs.unlinkSync(tmp); } catch { /* ignore */ }
    return false;
  }
  log(`备份完成 ${path.basename(file)}（${(size / 1024 / 1024).toFixed(2)} MB）`);
  cleanup(dir, opts.retainDays);
  return true;
}

// 计算距离下一次 daily_time 的毫秒数
function msUntilNext(hhmm) {
  const [h, m] = String(hhmm).split(':').map(Number);
  const now = new Date();
  const next = new Date(now.getFullYear(), now.getMonth(), now.getDate(), h || 0, m || 0, 0, 0);
  if (next <= now) next.setDate(next.getDate() + 1);
  return next - now;
}

function main() {
  const ops = readJson(path.join(ROOT, 'config/ops.json')) || {};
  const retainDays = Math.max(1, Number(arg('retain-days', env('ENG_BACKUP_RETAIN_DAYS', (ops.backup && ops.backup.retain_days) || 7))) || 7);
  const dir = path.resolve(arg('dir', env('ENG_BACKUP_DIR', path.join(ROOT, 'backups'))));
  const opts = { dir, retainDays, force: hasFlag('force'), ops };

  if (hasFlag('watch')) {
    const dailyTime = (ops.backup && ops.backup.daily_time) || '03:00';
    // 启动即补跑：错过当天定时也能有一份备份（幂等）
    runBackup(opts);
    const tick = () => {
      const wait = msUntilNext(dailyTime);
      log(`下次自动备份：${new Date(Date.now() + wait).toLocaleString('zh-CN', { hour12: false })}`);
      setTimeout(() => {
        runBackup(opts);
        tick();
      }, wait);
    };
    tick();
    return;
  }

  const ok = runBackup(opts);
  process.exitCode = ok ? 0 : 1;
}

main();
