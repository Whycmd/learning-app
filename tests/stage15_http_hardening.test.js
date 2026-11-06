// 阶段十五：HTTP 线程池与解析加固（v1.4.1-T002 阶段 A / 阶段 B）
// 阶段 A 覆盖：超长路径/请求头/body、畸形 Content-Length、chunked、非法方法、慢连接、
//              并发请求、大响应完整性、401 状态文本、worker 线程数上限
// 阶段 B 覆盖：keep-alive 连接复用、Keep-Alive 响应头、空闲超时、Connection: close、
//              HTTP/1.0 语义、单连接请求上限、流水线、路由参数与 404
const http = require('http');
const net = require('net');
const { execSync } = require('child_process');

const API_HOST = '127.0.0.1';
const API_PORT = 8088;

let pass = 0, fail = 0, skipped = 0;
const failures = [];

function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function api(method, path, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    const req = http.request({ host: API_HOST, port: API_PORT, method, path, headers, timeout: 30000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const buf = Buffer.concat(chunks);
        let json = null;
        try { json = JSON.parse(buf.toString('utf8')); } catch (_) {}
        resolve({ status: res.statusCode, statusMessage: res.statusMessage, headers: res.headers, bytes: buf.length, json });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

// 持久连接：按 Content-Length 切分完整响应，支持同一 socket 上的连续请求与流水线
function makeReader(sock) {
  let buf = '';
  let closed = false;
  const waiters = [];
  function extract() {
    const i = buf.indexOf('\r\n\r\n');
    if (i < 0) return null;
    const head = buf.slice(0, i);
    const cm = head.match(/Content-Length:\s*(\d+)/i);
    const len = cm ? parseInt(cm[1], 10) : 0;
    const end = i + 4 + len;
    if (buf.length < end) return null;
    const text = buf.slice(0, end);
    buf = buf.slice(end);
    const sm = head.match(/HTTP\/1\.1\s+(\d+)/);
    return { status: sm ? parseInt(sm[1], 10) : null, head, text };
  }
  function flush() {
    let r;
    while (waiters.length && (r = extract())) {
      const w = waiters.shift();
      clearTimeout(w.timer);
      w.resolve(r);
    }
    if (closed) {
      while (waiters.length) {
        const w = waiters.shift();
        clearTimeout(w.timer);
        w.resolve(null);
      }
    }
  }
  sock.on('data', d => { buf += d.toString('utf8'); flush(); });
  sock.on('close', () => { closed = true; flush(); });
  sock.on('error', () => { closed = true; flush(); });
  return {
    read(waitMs = 10000) {
      return new Promise((resolve, reject) => {
        const w = {
          resolve,
          timer: setTimeout(() => {
            const idx = waiters.indexOf(w);
            if (idx >= 0) waiters.splice(idx, 1);
            reject(new Error('read timeout'));
          }, waitMs)
        };
        waiters.push(w);
        flush();
      });
    },
    isClosed() { return closed; },
    waitClosed(waitMs = 10000) {
      const started = Date.now();
      return new Promise((resolve, reject) => {
        const poll = setInterval(() => {
          if (closed) {
            clearInterval(poll);
            clearTimeout(die);
            resolve(Date.now() - started);
          }
        }, 50);
        const die = setTimeout(() => {
          clearInterval(poll);
          reject(new Error('close timeout'));
        }, waitMs);
      });
    }
  };
}

function connectSession() {
  return new Promise((resolve, reject) => {
    const sock = net.connect(API_PORT, API_HOST, () => resolve({ sock, reader: makeReader(sock) }));
    sock.on('error', reject);
  });
}

function headerOf(head, name) {
  const m = head.match(new RegExp('^' + name + ':\\s*(.+)$', 'im'));
  return m ? m[1].trim() : '';
}

// 原始 socket 请求：payload 内含 Connection: close，因此以 close 作为结束信号
function raw(payload, waitMs = 15000) {
  return new Promise(resolve => {
    const started = Date.now();
    const sock = net.connect(API_PORT, API_HOST, () => sock.write(payload));
    let text = '';
    let done = false;
    const finish = () => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      const m = text.match(/HTTP\/1\.1 (\d+)/);
      resolve({ status: m ? parseInt(m[1], 10) : null, text, elapsed: Date.now() - started });
    };
    const timer = setTimeout(() => { sock.destroy(); finish(); }, waitMs);
    sock.on('data', d => { text += d.toString('utf8'); });
    sock.on('close', finish);
    sock.on('error', finish);
  });
}

function serverPid() {
  try {
    const out = execSync(`pgrep -f "backend/build/eng_app_server"`).toString().trim();
    return out.split('\n')[0];
  } catch (_) { return null; }
}

async function main() {
  console.log('\n=== v1.4.1-T002：HTTP 加固（stage15，阶段 A + 阶段 B） ===\n');

  // [1] 基础健康
  const health = await api('GET', '/api/health');
  if (health.status === 200 && health.json) ok('健康接口 200', 'db=' + health.json.db);
  else ko('健康接口 200', 'status=' + health.status);

  // [2] 401 状态文本
  const login = await api('POST', '/api/auth/login', { username: 'nobody', password: 'wrong' });
  if (login.status === 401 && login.statusMessage === 'Unauthorized') ok('401 状态文本', login.statusMessage);
  else ko('401 状态文本', `status=${login.status} msg=${login.statusMessage}`);

  // [3] 超长路径 → 414
  const longPath = '/a'.repeat(1200); // 2401 字符
  const r414 = await raw(`GET ${longPath} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`);
  if (r414.status === 414) ok('超长路径 414');
  else ko('超长路径 414', `status=${r414.status}`);

  // [4] 超大请求头 → 431
  const r431 = await raw(`GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nX-Big: ${'a'.repeat(20000)}\r\n\r\n`);
  if (r431.status === 431) ok('超大请求头 431');
  else ko('超大请求头 431', `status=${r431.status}`);

  // [5] 超大 body → 413（仅声明 Content-Length，不实际发送）
  const r413 = await raw(`POST /api/kp HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 9000000\r\n\r\n`);
  if (r413.status === 413) ok('超大 body 413');
  else ko('超大 body 413', `status=${r413.status}`);

  // [6] 畸形 Content-Length → 400
  for (const bad of ['abc', '-1', '99999999999999999999']) {
    const r = await raw(`POST /api/kp HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: ${bad}\r\n\r\n`);
    if (r.status === 400) ok(`Content-Length=${bad} → 400`);
    else ko(`Content-Length=${bad} → 400`, `status=${r.status}`);
  }

  // [7] Transfer-Encoding → 501
  const r501 = await raw(`POST /api/kp HTTP/1.1\r\nHost: x\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n`);
  if (r501.status === 501) ok('chunked 501');
  else ko('chunked 501', `status=${r501.status}`);

  // [8] 非法方法 → 405
  const r405 = await raw(`PATCH /api/health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n`);
  if (r405.status === 405) ok('PATCH 405');
  else ko('PATCH 405', `status=${r405.status}`);

  // [9] 畸形请求行 → 400
  const r400 = await raw(`GET /only-path-no-version\r\nHost: x\r\nConnection: close\r\n\r\n`);
  if (r400.status === 400) ok('缺 HTTP 版本 400');
  else ko('缺 HTTP 版本 400', `status=${r400.status}`);

  // [10] 慢连接：只发半截请求头，10s 读超时后应被断开
  const slow = await raw(`GET / HTTP/1.1\r\nHost: x`, 14000);
  if (slow.status === null && slow.elapsed >= 8000 && slow.elapsed <= 13000) {
    ok('慢连接按时断开', `elapsed=${slow.elapsed}ms`);
  } else {
    ko('慢连接按时断开', `status=${slow.status} elapsed=${slow.elapsed}ms`);
  }

  // [11] 攻击后进程仍存活
  const after = await api('GET', '/api/health');
  if (after.status === 200) ok('异常请求后进程存活');
  else ko('异常请求后进程存活', 'status=' + after.status);

  // [12] 50 并发
  const N = 50;
  const results = await Promise.all(
    Array.from({ length: N }, () => api('GET', '/api/health').catch(e => ({ status: 0, err: e.message })))
  );
  const bad = results.filter(r => r.status !== 200);
  if (bad.length === 0) ok('50 并发全部 200');
  else ko('50 并发全部 200', `失败=${bad.length} 首个=${bad[0].status} ${bad[0].err || ''}`);

  // [13] 大响应完整性（Content-Length 与实际字节一致、JSON 可解析）
  const big = await api('GET', '/api/kp?size=1000');
  const cl = parseInt(big.headers['content-length'] || '0', 10);
  if (big.status === 200 && cl === big.bytes && big.json) {
    ok('大响应完整', `${big.bytes} bytes`);
  } else {
    ko('大响应完整', `status=${big.status} cl=${cl} bytes=${big.bytes} json=${!!big.json}`);
  }

  // [14] worker 线程数上限（macOS 用 top 读取 #TH；ps -M 在部分系统只返回表头）
  const pid = serverPid();
  if (!pid) {
    skip('线程数上限', '未找到服务进程');
  } else {
    try {
      const out = execSync(`top -l 1 -pid ${pid} -stats pid,threads`, { encoding: 'utf8', timeout: 10000 });
      const m = out.match(new RegExp(`^\\s*${pid}\\s+(\\d+)`, 'm'));
      const threads = m ? parseInt(m[1], 10) : 0;
      if (threads > 1 && threads <= 18) ok('线程数上限', `${threads} 个线程（≤ 16+2）`);
      else ko('线程数上限', `threads=${threads}`);
    } catch (e) {
      skip('线程数上限', e.message);
    }
  }

  // ---- 阶段 B：keep-alive 与路由预编译 ----
  console.log('\n[15] keep-alive 连接复用');
  try {
    const s = await connectSession();
    s.sock.write('GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n');
    const r1 = await s.reader.read();
    s.sock.write('GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n');
    const r2 = await s.reader.read();
    if (r1 && r2 && r1.status === 200 && r2.status === 200) ok('同一 socket 连续 2 个请求均 200');
    else ko('同一 socket 连续 2 个请求均 200', `r1=${r1 && r1.status} r2=${r2 && r2.status}`);
    if (r1 && headerOf(r1.head, 'Connection').toLowerCase() === 'keep-alive') ok('响应头 Connection: keep-alive');
    else ko('响应头 Connection: keep-alive', r1 ? headerOf(r1.head, 'Connection') : '无响应');
    if (r1 && headerOf(r1.head, 'Keep-Alive') === 'timeout=5') ok('响应头 Keep-Alive: timeout=5');
    else ko('响应头 Keep-Alive: timeout=5', r1 ? headerOf(r1.head, 'Keep-Alive') : '无响应');
    s.sock.destroy();
  } catch (e) { ko('keep-alive 连接复用', e.message); }

  console.log('\n[16] keep-alive 空闲超时');
  try {
    const s = await connectSession();
    s.sock.write('GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n');
    const r = await s.reader.read();
    if (!r || r.status !== 200) ko('空闲超时前响应正常', `status=${r && r.status}`);
    const elapsed = await s.reader.waitClosed(9000).catch(() => -1);
    if (elapsed >= 3500 && elapsed <= 8000) ok('空闲 5s 后服务端关闭', `elapsed=${elapsed}ms`);
    else ko('空闲 5s 后服务端关闭', `elapsed=${elapsed}ms`);
    s.sock.destroy();
  } catch (e) { ko('keep-alive 空闲超时', e.message); }

  console.log('\n[17] Connection: close');
  try {
    const s = await connectSession();
    s.sock.write('GET /api/health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n');
    const r = await s.reader.read();
    if (r && r.status === 200 && headerOf(r.head, 'Connection').toLowerCase() === 'close') ok('响应头 Connection: close');
    else ko('响应头 Connection: close', r ? `status=${r.status} conn=${headerOf(r.head, 'Connection')}` : '无响应');
    const elapsed = await s.reader.waitClosed(3000).catch(() => -1);
    if (elapsed >= 0) ok('close 后连接立即断开', `${elapsed}ms`);
    else ko('close 后连接立即断开', '3s 内未关闭');
    s.sock.destroy();
  } catch (e) { ko('Connection: close', e.message); }

  console.log('\n[18] HTTP/1.0 默认关闭');
  try {
    const s = await connectSession();
    s.sock.write('GET /api/health HTTP/1.0\r\nHost: x\r\n\r\n');
    const r = await s.reader.read();
    if (r && r.status === 200 && headerOf(r.head, 'Connection').toLowerCase() === 'close') ok('HTTP/1.0 默认 Connection: close');
    else ko('HTTP/1.0 默认 Connection: close', r ? `status=${r.status} conn=${headerOf(r.head, 'Connection')}` : '无响应');
    const elapsed = await s.reader.waitClosed(3000).catch(() => -1);
    if (elapsed >= 0) ok('HTTP/1.0 请求后断开');
    else ko('HTTP/1.0 请求后断开', '3s 内未关闭');
    s.sock.destroy();
  } catch (e) { ko('HTTP/1.0 默认关闭', e.message); }

  console.log('\n[19] HTTP/1.0 + Connection: keep-alive');
  try {
    const s = await connectSession();
    s.sock.write('GET /api/health HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n');
    const r1 = await s.reader.read();
    s.sock.write('GET /api/health HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n');
    const r2 = await s.reader.read();
    const c1 = r1 ? headerOf(r1.head, 'Connection').toLowerCase() : '';
    if (r1 && r2 && r1.status === 200 && r2.status === 200 && c1 === 'keep-alive') ok('HTTP/1.0 显式 keep-alive 可复用');
    else ko('HTTP/1.0 显式 keep-alive 可复用', `r1=${r1 && r1.status} r2=${r2 && r2.status} conn=${c1}`);
    s.sock.destroy();
  } catch (e) { ko('HTTP/1.0 + Connection: keep-alive', e.message); }

  console.log('\n[20] 单连接 100 请求上限（流水线）');
  try {
    const s = await connectSession();
    const req = 'GET /api/health HTTP/1.1\r\nHost: x\r\n\r\n';
    s.sock.write(req.repeat(101));
    let got = 0;
    for (let i = 0; i < 101; i++) {
      const r = await s.reader.read(5000).catch(() => null);
      if (!r) break;
      got++;
    }
    if (got === 100) ok('第 100 个请求后关闭连接', `收到 ${got} 个响应`);
    else ko('第 100 个请求后关闭连接', `收到 ${got} 个响应`);
    const closed = await s.reader.waitClosed(3000).catch(() => -1);
    if (closed >= 0) ok('达到上限后连接关闭');
    else ko('达到上限后连接关闭', '3s 内未关闭');
    s.sock.destroy();
  } catch (e) { ko('单连接 100 请求上限', e.message); }

  console.log('\n[21] 路由参数与 404（预编译正则）');
  try {
    const bad = await api('GET', '/api/kp/abc');
    if (bad.status === 400) ok('GET /api/kp/abc → 400（参数已捕获）');
    else ko('GET /api/kp/abc → 400', `status=${bad.status}`);

    const nf = await api('GET', '/api/nope');
    if (nf.status === 404) ok('GET /api/nope → 404');
    else ko('GET /api/nope → 404', `status=${nf.status}`);

    const miss = await api('GET', '/api/kp/abc/extra');
    if (miss.status === 404) ok('GET /api/kp/abc/extra → 404（不跨段匹配）');
    else ko('GET /api/kp/abc/extra → 404', `status=${miss.status}`);

    const list = await api('GET', '/api/kp?size=1');
    if (list.status === 200 && list.json && Array.isArray(list.json.data)) ok('GET /api/kp 列表仍正常');
    else ko('GET /api/kp 列表仍正常', `status=${list.status}`);
  } catch (e) { ko('路由参数与 404', e.message); }

  console.log('\n=== stage15 结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}  跳过: ${skipped}`);
  if (fail > 0) {
    console.log('失败项：');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`));
    process.exit(1);
  }
  console.log('✓ stage15 全部通过');
}

main().catch(e => { console.error(e); process.exit(1); });
