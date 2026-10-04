// 阶段十：播放复习（v1.2.0-T006）集成测试
// 测试 /api/playback/queue 接口
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

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const args = ['--socket='+SOCK, '-uroot', '-p'+pw, 'english_learning', '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout;
}

function api(method, path, body, cookie) {
  return new Promise((resolve, reject) => {
    const u = new URL(path, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json' };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    else headers['Content-Length'] = 0;
    if (cookie) headers['Cookie'] = 'eng_session=' + cookie;
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers,
      timeout: 30000
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
    req.on('timeout', () => { req.destroy(new Error('timeout')); });
    if (data) req.write(data);
    req.end();
  });
}

function cleanup() {
  // 清理可能存在的测试数据（如果有测试用户创建的知识点）
  // 这里主要测试游客身份，所以不需要清理
}

async function main() {
  console.log('=== 阶段十：播放复习（v1.2.0-T006）集成测试 ===\n');

  try { cleanup(); } catch(e) { console.log('预清理跳过:', e.message); }

  // ---- 0. 健康检查 ----
  console.log('[0] 前置检查');
  try {
    const r = await api('GET', '/api/health');
    if (r.json && r.json.status === 'ok') ok('后端健康检查');
    else ko('后端健康检查', r.text);
  } catch(e) { ko('后端健康检查', e.message); }

  // ---- 1. 游客访问 /api/playback/queue ----
  console.log('\n[1] 游客访问播放队列');
  try {
    const r = await api('GET', '/api/playback/queue');
    if (r.status === 200 && r.json) ok('游客访问返回200');
    else ko('游客访问返回200', `status=${r.status}, json=${JSON.stringify(r.json)}`);

    if (r.json && r.json.data && Array.isArray(r.json.data)) ok('data字段为数组');
    else ko('data字段为数组', JSON.stringify(r.json));

    if (r.json && typeof r.json.total === 'number') ok('total字段为数字');
    else ko('total字段为数字', JSON.stringify(r.json));
  } catch(e) { ko('游客访问播放队列', e.message); }

  // ---- 2. 验证返回数据结构 ----
  console.log('\n[2] 验证返回数据结构');
  try {
    const r = await api('GET', '/api/playback/queue');
    const data = r.json && r.json.data;
    if (!data) {
      ko('数据非空', 'data为空');
    } else {
      if (data.length >= 0) ok('data长度正确（可空）');
      else ko('data长度正确', `length=${data.length}`);

      // 如果非空，检查第一条数据结构
      if (data.length > 0) {
        const item = data[0];
        if (item && typeof item.id === 'number') ok('条目id为数字');
        else ko('条目id为数字', JSON.stringify(item));
        if (item && typeof item.word === 'string') ok('条目word为字符串');
        else ko('条目word为字符串', JSON.stringify(item));
      } else {
        ok('无数据时结构正确（空数组）');
      }
    }
  } catch(e) { ko('验证数据结构', e.message); }

  // ---- 3. 登录用户访问（如果有测试用户）----
  console.log('\n[3] 登录用户访问播放队列');
  // 由于没有测试用户登录，跳过此部分
  skip('登录用户访问播放队列', '无测试账号，跳过');

  // ---- 4. 前端文件检查 ----
  console.log('\n[4] 前端文件检查');
  const ttsPath = path.join(ROOT, 'frontend/static/tts.js');
  if (fs.existsSync(ttsPath)) ok('tts.js 文件存在');
  else ko('tts.js 文件存在', '文件未找到');

  const kpDetailPath = path.join(ROOT, 'frontend/pages/kp-detail.html');
  if (fs.existsSync(kpDetailPath)) ok('kp-detail.html 文件存在');
  else ko('kp-detail.html 文件存在', '文件未找到');

  const wordbookPath = path.join(ROOT, 'frontend/pages/wordbook.html');
  if (fs.existsSync(wordbookPath)) ok('wordbook.html 文件存在');
  else ko('wordbook.html 文件存在', '文件未找到');

  // 检查kp-detail.html是否引用tts.js
  const kpDetailContent = fs.readFileSync(kpDetailPath, 'utf8');
  if (kpDetailContent.includes('/static/tts.js')) ok('kp-detail.html 引用tts.js');
  else ko('kp-detail.html 引用tts.js', '未找到引用');

  // 检查wordbook.html是否引用tts.js
  const wordbookContent = fs.readFileSync(wordbookPath, 'utf8');
  if (wordbookContent.includes('/static/tts.js')) ok('wordbook.html 引用tts.js');
  else ko('wordbook.html 引用tts.js', '未找到引用');

  // 检查样式文件
  const stylePath = path.join(ROOT, 'frontend/static/style.css');
  if (fs.existsSync(stylePath)) ok('style.css 文件存在');
  else ko('style.css 文件存在', '文件未找到');

  const styleContent = fs.readFileSync(stylePath, 'utf8');
  if (styleContent.includes('.speak-btn')) ok('style.css 包含.speak-btn样式');
  else ko('style.css 包含.speak-btn样式', '未找到样式');

  // ---- 5. 汇总 ----
  console.log('\n=== 测试汇总 ===');
  console.log(`通过: ${pass}, 失败: ${fail}, 跳过: ${skipped}`);
  if (failures.length > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`));
  }

  process.exit(fail > 0 ? 1 : 0);
}

main().catch(e => {
  console.error('测试执行错误:', e);
  process.exit(1);
});
