#!/usr/bin/env node
// 全量测试运行器（Node.js 原生，无需 jest）
//
// 测试隔离策略（v1.4.1-T004）：
//   1. 每轮重建独立测试库 eng_app_test（结构来自 scripts/sql/），不触碰开发库；
//   2. 在独立端口（默认 18088）启动后端，AI 走 Mock（success），断网可跑；
//   3. 测试产物写入系统临时目录，不污染 word_tmp / backups；
//   4. 运行前后对开发库逐表行数快照，发现变化即判定失败；
//   5. 结束后停止后端并删除测试库（KEEP_TEST_DB=1 可保留排查）。
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn, spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const TEST_DIR = __dirname;

function env(key, fallback) {
  const v = process.env[key];
  return v === undefined || v === '' ? fallback : v;
}

const LOCAL = path.join(os.homedir(), '.local');
const MYSQL_BIN = env('MYSQL_BIN', path.join(LOCAL, 'mysql/bin/mysql'));
const MYSQLD_BIN = env('MYSQLD_BIN', path.join(LOCAL, 'mysql/bin/mysqld'));
const MYSQL_HOME = env('MYSQL_HOME', path.join(LOCAL, 'mysql'));
const MYSQL_PW_FILE = env('MYSQL_PW_FILE', path.join(LOCAL, 'mysql/.root_pw'));
const MYSQL_RUN_DIR = env('MYSQL_RUN_DIR', path.join(LOCAL, 'run'));
const MYSQL_LOG_DIR = env('MYSQL_LOG_DIR', path.join(LOCAL, 'log'));
const SOCK = env('MYSQL_SOCKET', path.join(LOCAL, 'run/mysql.sock'));
const DB_NAME = env('TEST_DB_NAME', 'eng_app_test');
const API_PORT = Number(env('TEST_API_PORT', '18088'));
const MOCK_MODE = env('TEST_AI_MOCK_MODE', 'success');
const KEEP_TEST_DB = env('KEEP_TEST_DB', '0') === '1';

const SERVER_BIN = path.join(ROOT, 'backend/build/eng_app_server');
const SQL_DIR = path.join(ROOT, 'scripts/sql');
const ARTIFACT_DIR = fs.mkdtempSync(path.join(os.tmpdir(), 'eng_app_test_artifacts_'));

function pw() {
  return fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
}

function mysqlExec(sql, database) {
  const args = ['--socket=' + SOCK, '-uroot', '-p' + pw(), '-N', '--default-character-set=utf8mb4'];
  if (database) args.push(database);
  args.push('-e', sql);
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 60000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${(r.stderr || r.stdout || '').trim()}`);
  return r.stdout;
}

function mysqlFile(file, database) {
  const args = ['--socket=' + SOCK, '-uroot', '-p' + pw(), '--default-character-set=utf8mb4', database];
  const r = spawnSync(MYSQL_BIN, args, {
    input: fs.readFileSync(file, 'utf8'),
    encoding: 'utf8',
    timeout: 60000,
  });
  if (r.status !== 0) throw new Error(`mysql import failed (${path.basename(file)}): ${(r.stderr || r.stdout || '').trim()}`);
}

// 开发库逐表精确行数快照
function snapshotDb(database) {
  const out = mysqlExec(
    `SELECT TABLE_NAME FROM information_schema.tables WHERE table_schema='${database.replace(/'/g, "''")}' AND TABLE_TYPE='BASE TABLE' ORDER BY TABLE_NAME;`
  );
  const tables = out.split('\n').map(s => s.trim()).filter(Boolean);
  const counts = {};
  for (const t of tables) {
    const r = mysqlExec(`SELECT COUNT(*) FROM \`${database}\`.\`${t}\`;`);
    counts[t] = Number(r.trim());
  }
  return counts;
}

function diffCounts(before, after) {
  const diffs = [];
  const names = new Set([...Object.keys(before), ...Object.keys(after)]);
  for (const t of names) {
    const b = before[t];
    const a = after[t];
    if (b !== a) diffs.push(`${t}: ${b === undefined ? '缺失' : b} -> ${a === undefined ? '缺失' : a}`);
  }
  return diffs;
}

function waitSocket(timeoutMs = 30000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      mysqlExec('SELECT 1;');
      return true;
    } catch {
      spawnWait(500);
    }
  }
  return false;
}

function spawnWait(ms) {
  try { spawnSync(process.execPath, ['-e', `setTimeout(()=>{},${ms})`], { timeout: ms + 1000 }); }
  catch { /* ignore */ }
}

// 直接在 run_all 进程内启动 mysqld，避免中间脚本退出后进程被回收
function startMysql() {
  fs.mkdirSync(MYSQL_RUN_DIR, { recursive: true });
  fs.mkdirSync(MYSQL_LOG_DIR, { recursive: true });
  fs.mkdirSync(path.join(MYSQL_HOME, 'data'), { recursive: true });
  const sock = path.join(MYSQL_RUN_DIR, 'mysql.sock');
  const out = fs.openSync(path.join(MYSQL_LOG_DIR, 'mysqld.out'), 'a');
  const proc = spawn(MYSQLD_BIN, [
    '--user=' + os.userInfo().username,
    '--datadir=' + path.join(MYSQL_HOME, 'data'),
    '--basedir=' + MYSQL_HOME,
    '--socket=' + sock,
    '--port=3306',
    '--bind-address=127.0.0.1',
    '--pid-file=' + path.join(MYSQL_RUN_DIR, 'mysqld.pid'),
    '--log-error=' + path.join(MYSQL_LOG_DIR, 'mysqld.err'),
    '--lower_case_table_names=2',
    '--default-authentication-plugin=mysql_native_password',
    '--character-set-server=utf8mb4',
    '--collation-server=utf8mb4_unicode_ci',
    '--innodb_buffer_pool_size=128M',
    '--max_connections=50',
  ], { stdio: ['ignore', out, out] });
  return proc;
}

function waitHealth(port, timeoutMs = 30000) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = () => {
      const req = http.get({ host: '127.0.0.1', port, path: '/api/health', timeout: 3000 }, res => {
        res.resume();
        if (res.statusCode === 200) return resolve();
        retry();
      });
      req.on('error', retry);
      req.on('timeout', () => { req.destroy(new Error('timeout')); });
      function retry() {
        if (Date.now() - start > timeoutMs) return reject(new Error(`backend not ready on ${port}`));
        setTimeout(tick, 300);
      }
    };
    tick();
  });
}

function startBackend(port, mockMode, logFile) {
  const childEnv = {
    ...process.env,
    DB_NAME,
    TEST_DB_NAME: DB_NAME,
    API_PORT: String(port),
    TEST_API_PORT: String(port),
    AI_MOCK_MODE: mockMode,
    ENG_DB_NAME: DB_NAME,
    ENG_AI_MOCK_MODE: mockMode,
    ENG_AI_RETRY_TIMES: env('ENG_AI_RETRY_TIMES', '1'),
    ENG_AI_RETRY_INTERVAL: env('ENG_AI_RETRY_INTERVAL', '1'),
    ENG_AI_MAX_CONCURRENCY: env('ENG_AI_MAX_CONCURRENCY', '1'),
    ENG_WORD_TMP: path.join(ARTIFACT_DIR, 'word_tmp'),
    ENG_BACKUP_DIR: path.join(ARTIFACT_DIR, 'backups'),
    WORD_TMP: path.join(ARTIFACT_DIR, 'word_tmp'),
    BACKUP_DIR: path.join(ARTIFACT_DIR, 'backups'),
  };
  const out = fs.openSync(logFile, 'a');
  const proc = spawn(SERVER_BIN, ['--root', ROOT, '--port', String(port)], {
    env: childEnv,
    stdio: ['ignore', out, out],
  });
  return proc;
}

function stopBackend(proc) {
  return new Promise(resolve => {
    if (!proc || proc.exitCode !== null) return resolve();
    let done = false;
    const finish = () => { if (!done) { done = true; resolve(); } };
    proc.once('exit', finish);
    try { proc.kill('SIGTERM'); } catch { /* ignore */ }
    setTimeout(() => {
      try { proc.kill('SIGKILL'); } catch { /* ignore */ }
      finish();
    }, 5000);
  });
}

async function runTest(file) {
  return new Promise(resolve => {
    const proc = spawn(process.execPath, [path.join(TEST_DIR, file)], {
      stdio: ['inherit', 'inherit', 'inherit'],
      env: {
        ...process.env,
        DB_NAME,
        TEST_DB_NAME: DB_NAME,
        API_HOST: '127.0.0.1',
        API_PORT: String(API_PORT),
        TEST_API_PORT: String(API_PORT),
        AI_MOCK_MODE: MOCK_MODE,
        ENG_DB_NAME: DB_NAME,
        ENG_AI_MOCK_MODE: MOCK_MODE,
        WORD_TMP: path.join(ARTIFACT_DIR, 'word_tmp'),
        BACKUP_DIR: path.join(ARTIFACT_DIR, 'backups'),
        TEST_ARTIFACT_DIR: ARTIFACT_DIR,
      },
    });
    proc.on('close', code => resolve(code === 0));
  });
}

async function main() {
  const testFiles = fs.readdirSync(TEST_DIR)
    .filter(f => f.endsWith('.test.js'))
    .sort();

  // 产物目录
  fs.mkdirSync(path.join(ARTIFACT_DIR, 'word_tmp'), { recursive: true });
  fs.mkdirSync(path.join(ARTIFACT_DIR, 'backups'), { recursive: true });
  const logFile = path.join(ARTIFACT_DIR, 'server.log');

  let serverProc = null;
  let mysqlProc = null;
  let devBefore = {};
  const devDb = (() => {
    try { return JSON.parse(fs.readFileSync(path.join(ROOT, 'config/db.json'), 'utf8')).database || 'english_learning'; }
    catch { return 'english_learning'; }
  })();
  const dbUser = (() => {
    try { return JSON.parse(fs.readFileSync(path.join(ROOT, 'config/db.json'), 'utf8')).user || 'eng_app'; }
    catch { return 'eng_app'; }
  })();

  try {
    // MySQL：未启动则在本进程内拉起（避免启动脚本退出后 mysqld 被回收）
    try {
      mysqlExec('SELECT 1;');
    } catch {
      console.log('[setup] MySQL 未响应，启动 mysqld ...');
      mysqlProc = startMysql();
      if (!waitSocket(30000)) throw new Error('MySQL 启动失败：30 秒内 socket 不可用');
      console.log('[setup] MySQL 已就绪');
    }

    // 后端二进制：缺失则编译
    if (!fs.existsSync(SERVER_BIN)) {
      console.log('[setup] 后端二进制不存在，执行 scripts/build_backend.sh ...');
      const b = spawnSync('bash', [path.join(ROOT, 'scripts/build_backend.sh')], { cwd: ROOT, stdio: 'inherit' });
      if (b.status !== 0) throw new Error('后端编译失败');
    }

    // 开发库快照（运行前）
    try {
      devBefore = snapshotDb(devDb);
      console.log(`[isolate] 开发库 ${devDb} 快照：${Object.keys(devBefore).length} 张表`);
    } catch (e) {
      console.log(`[isolate] 开发库快照跳过：${e.message}`);
    }

    // 重建独立测试库
    mysqlExec(`DROP DATABASE IF EXISTS \`${DB_NAME}\`; CREATE DATABASE \`${DB_NAME}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;`);
    // 后端使用 config/db.json 的账号连接；测试库是新建的，需给该账号授权
    mysqlExec(`GRANT ALL PRIVILEGES ON \`${DB_NAME}\`.* TO '${dbUser}'@'localhost'; FLUSH PRIVILEGES;`);
    const sqlFiles = fs.readdirSync(SQL_DIR).filter(f => f.endsWith('.sql')).sort();
    for (const f of sqlFiles) mysqlFile(path.join(SQL_DIR, f), DB_NAME);
    console.log(`[isolate] 测试库 ${DB_NAME} 已重建（${sqlFiles.length} 个 SQL 文件）`);

    // 启动独立后端（Mock success）
    serverProc = startBackend(API_PORT, MOCK_MODE, logFile);
    await waitHealth(API_PORT);
    console.log(`[isolate] 测试后端已就绪 http://127.0.0.1:${API_PORT}（AI Mock=${MOCK_MODE}）`);

    console.log(`\n=== 运行 ${testFiles.length} 个测试套件 ===\n`);
    let passed = 0, failed = 0;
    for (const f of testFiles) {
      process.stdout.write(`▶ ${f} ... `);
      const ok = await runTest(f);
      if (ok) {
        passed++;
        console.log('✓');
      } else {
        failed++;
        console.log('✗');
      }
    }
    console.log(`\n=== 结果: ${passed} 通过, ${failed} 失败, 共 ${testFiles.length} 套件 ===\n`);

    // 开发库行数对比
    let devDiffs = null;
    try {
      const devAfter = snapshotDb(devDb);
      devDiffs = diffCounts(devBefore, devAfter);
      if (devDiffs.length) {
        console.log('[isolate] ✗ 开发库行数发生变化（可能被测试污染或外部写入）：');
        devDiffs.forEach(d => console.log('  ' + d));
      } else {
        console.log(`[isolate] ✓ 开发库 ${devDb} 行数未变化`);
      }
    } catch (e) {
      console.log(`[isolate] 开发库对比跳过：${e.message}`);
    }

    if (failed > 0 || (devDiffs && devDiffs.length)) {
      console.log(`[isolate] 服务端日志：${logFile}`);
      process.exitCode = 1;
    }
  } catch (e) {
    console.error('[run_all] 失败：' + e.message);
    try {
      const tail = fs.readFileSync(logFile, 'utf8').split('\n').slice(-30).join('\n');
      if (tail.trim()) console.error('[run_all] 服务端日志尾部：\n' + tail);
    } catch { /* ignore */ }
    process.exitCode = 1;
  } finally {
    await stopBackend(serverProc);
    if (!KEEP_TEST_DB) {
      try { mysqlExec(`DROP DATABASE IF EXISTS \`${DB_NAME}\`;`); }
      catch { /* ignore */ }
    } else {
      console.log(`[isolate] KEEP_TEST_DB=1，保留测试库 ${DB_NAME}`);
    }
    fs.rmSync(ARTIFACT_DIR, { recursive: true, force: true });
  }

  process.exit(process.exitCode || 0);
}

main().catch(e => { console.error(e); process.exit(1); });
