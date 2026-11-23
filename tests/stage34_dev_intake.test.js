#!/usr/bin/env node
// v1.5.2-T001 DEV-INTAKE：AI 导航需求/Bug 自动沉淀 + 开发者台账接口
// 判定四路落表、低置信 triage、降级、去重计数、游客 user_id=0、admin 401/403、PUT 白名单与枚举、summary 一致性
// 判定相关断言依赖 Mock 关键词分支（backend/src/ai_provider.cpp 导航夹具）；非 Mock 环境自动跳过
const { API_PORT, API_HOST, MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, AI_MOCK_MODE } = require('./_env');
const { withSystemCookie } = require('./_system');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

function req(method, urlPath, body, cookie) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = {};
    if (data) { headers['Content-Type'] = 'application/json'; headers['Content-Length'] = Buffer.byteLength(data); }
    if (cookie) headers['Cookie'] = cookie;
    const r = http.request({ host: API_HOST, port: API_PORT, path: urlPath, method, headers, timeout: 60000 }, res => {
      let buf = '';
      res.on('data', c => buf += c);
      res.on('end', () => { let json = null; try { json = JSON.parse(buf); } catch (e) {} resolve({ status: res.statusCode, json, headers: res.headers }); });
    });
    r.on('error', reject);
    r.on('timeout', () => r.destroy(new Error('timeout')));
    if (data) r.write(data);
    r.end();
  });
}

function runMysql(sql) {
  return execFileSync(MYSQL_BIN,
    ['--socket=' + SOCK, '-uroot', '-p' + fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim(),
     '--default-character-set=utf8mb4', '-N', DB_NAME, '-e', sql],
    { encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }).trim();
}
function q1(sql) { const o = runMysql(sql); return o ? o.split('\n')[0].split('\t') : []; }
function nav(text, cookie, page) {
  return req('POST', '/api/ai-nav/messages', { text, page: page || '/index.html' }, cookie);
}

let fail = 0;
function check(ok, name, detail) {
  console.log(`${ok ? '✓' : '✗'} ${name}${!ok && detail !== undefined ? ' → ' + JSON.stringify(detail).slice(0, 200) : ''}`);
  if (!ok) fail++;
}

async function main() {
  const sysCookie = withSystemCookie({})['Cookie'];
  const MOCK = AI_MOCK_MODE !== 'off';
  let r;

  r = await req('GET', '/api/health');
  check(r.status === 200, '[0] 服务可用');

  // 前置：系统账号设为 admin、清空台账（admin 设置方式即 D4 裁决：直接改库）
  runMysql("UPDATE users SET role='admin' WHERE id=1; DELETE FROM dev_backlog; DELETE FROM dev_bugs;");

  // ---------- [1] 判定四路落表 ----------
  if (MOCK) {
    r = await nav('能不能练日语听力', sysCookie);
    check(r.status === 200 && /暂不在本应用范围/.test((r.json.reply || {}).content || ''),
      '[1.1] 超范围：回复含范围外文案', r.json);
    check(!/#B\d|#G\d|#T\d/.test((r.json.reply || {}).content || ''), '[1.2] 超范围：不附编号');
    let row = q1("SELECT status,scope_fit FROM dev_backlog WHERE title='支持日语听力练习'");
    check(row[0] === 'rejected' && row[1] === 'out_of_scope', '[1.3] 超范围落 rejected/out_of_scope', row);

    r = await nav('导出单词卡点了没反应', sysCookie, '/pages/wordbook.html');
    check(r.status === 200 && /#G\d+/.test((r.json.reply || {}).content || ''), '[1.4] Bug 回执含 #G', r.json);
    row = q1("SELECT status,category,page_path FROM dev_bugs WHERE title='导出单词卡按钮无反应'");
    check(row[0] === 'new' && row[1] === 'export' && row[2] === '/pages/wordbook.html', '[1.5] Bug 落表字段', row);

    r = await nav('想要横版单词卡', sysCookie);
    check(r.status === 200 && /#B\d+/.test((r.json.reply || {}).content || ''), '[1.6] 需求回执含 #B', r.json);
    row = q1("SELECT status,scope_fit FROM dev_backlog WHERE title='想要横版单词卡'");
    check(row[0] === 'new' && row[1] === 'in_scope', '[1.7] 需求落 new/in_scope', row);

    const before = q1("SELECT (SELECT COUNT(*) FROM dev_backlog)+(SELECT COUNT(*) FROM dev_bugs)")[0];
    r = await nav('decay 怎么记', sysCookie);
    check(r.status === 200 && ((r.json.reply || {}).content || '').length > 0, '[1.8] 普通提问正常回答');
    check(!/#B\d|#G\d|#T\d/.test((r.json.reply || {}).content || ''), '[1.9] 普通提问无回执');
    check(q1("SELECT (SELECT COUNT(*) FROM dev_backlog)+(SELECT COUNT(*) FROM dev_bugs)")[0] === before,
      '[1.10] 普通提问两表无新增');

    r = await nav('低置信，想要日历视图', sysCookie);
    check(r.status === 200 && /#T\d+/.test((r.json.reply || {}).content || ''), '[1.11] 低置信回执 #T', r.json);
    row = q1("SELECT status FROM dev_backlog WHERE title='低置信测试需求'");
    check(row[0] === 'triage', '[1.12] 低置信落 triage', row);

    r = await nav('用纯文本回我一句', sysCookie);
    check(r.status === 200 && ((r.json.reply || {}).content || '').length > 0 &&
      !/#B\d|#G\d|#T\d/.test((r.json.reply || {}).content || ''), '[1.13] 纯文本降级：正常回复不记录');
    r = await nav('返回坏JSON试试', sysCookie);
    check(r.status === 200 && ((r.json.reply || {}).content || '').length > 0 &&
      !/#B\d|#G\d|#T\d/.test((r.json.reply || {}).content || ''), '[1.14] 坏 JSON 降级：不报错不记录');

    // ---------- [2] 去重：行数不变、occurrences+1、编号/状态不变（D7） ----------
    const idA = q1("SELECT id,occurrences FROM dev_backlog WHERE title='想要横版单词卡'");
    r = await nav('想要横版单词卡', sysCookie);
    check(r.status === 200 && r.json.reply.content.includes('#B' + idA[0]), '[2.1] 重复提交编号不变', r.json);
    row = q1("SELECT occurrences,status FROM dev_backlog WHERE title='想要横版单词卡'");
    check(Number(row[0]) === Number(idA[1]) + 1 && row[1] === 'new', '[2.2] occurrences+1 且状态不变', row);

    // ---------- [3] 游客落表 ----------
    r = await nav('想要听写练习功能', 'eng_guest_id=s34guest');
    check(r.status === 200 && /#B\d+/.test((r.json.reply || {}).content || ''), '[3.1] 游客回执正常', r.json);
    row = q1("SELECT user_id,source_message_id FROM dev_backlog WHERE title='支持听写练习'");
    check(row[0] === '0' && (row[1] === 'NULL' || row[1] === '' || row[1] === null), '[3.2] 游客 user_id=0、来源 NULL', row);

    // 注意：文案不能含动作词典关键词（如"错题/复习/小测"），否则被直接跳转不经过 AI
    r = await nav('收藏页数据不显示', sysCookie);
    check(r.status === 200 && /#G\d+/.test((r.json.reply || {}).content || ''), '[3.3] 第二条 Bug 落表', r.json);
  } else {
    console.log('~ [1~3] 判定/去重/游客断言依赖 Mock 夹具，非 Mock 环境跳过（全量回归下会执行）');
    runMysql(
      "INSERT INTO dev_backlog (user_id,title,category,scope_fit,confidence,dedupe_key,status) VALUES " +
      "(1,'测试需求A','enhancement','in_scope',0.9,'t-a','new'),(1,'测试需求B','feature','in_scope',0.9,'t-b','triage')," +
      "(0,'测试需求R','enhancement','out_of_scope',0.9,'t-r','rejected')," +
      "(1,'测试BugA','content','in_scope',0.9,'t-g1','new'),(1,'测试BugB','ui','in_scope',0.9,'t-g2','new');");
    runMysql("INSERT INTO dev_bugs (user_id,title,category,severity,confidence,dedupe_key,status) VALUES " +
      "(1,'测试BugA','content','major',0.9,'t-b1','new'),(1,'测试BugB','ui','minor',0.9,'t-b2','new');");
  }

  // ---------- [4] 权限：未登录 401、非 admin 403、admin 200 ----------
  for (const p of ['/api/dev/summary', '/api/dev/backlog', '/api/dev/bugs']) {
    r = await req('GET', p);
    check(r.status === 401, `[4.1] 未登录 ${p} → 401`, r.status);
  }
  const regName = 's34u' + (Date.now() % 100000000);
  await req('POST', '/api/auth/register', { username: regName, password: 'pass1234', display_name: regName });
  const login = await req('POST', '/api/auth/login', { username: regName, password: 'pass1234' });
  const m = ((login.headers['set-cookie'] || []).join(';').match(/eng_session=([0-9a-f]+)/) || [])[1] || '';
  const userCk = m ? 'eng_session=' + m : '';
  check(userCk !== '', '[4.0] 非 admin 测试账号已登录');
  r = await req('GET', '/api/dev/summary', null, userCk);
  check(r.status === 403, '[4.2] 已登录非 admin → 403', r.status);
  r = await req('PUT', '/api/dev/backlog/1', { status: 'accepted' }, userCk);
  check(r.status === 403, '[4.3] 非 admin PUT → 403', r.status);
  r = await req('GET', '/api/dev/summary', null, sysCookie);
  check(r.status === 200, '[4.4] admin → 200', r.status);

  // ---------- [5] PUT 白名单与枚举（admin） ----------
  const blId = q1("SELECT id FROM dev_backlog WHERE status<>'rejected' ORDER BY id LIMIT 1")[0];
  const bugIds = runMysql("SELECT id FROM dev_bugs ORDER BY id LIMIT 2").split('\n').filter(Boolean);
  if (blId && bugIds.length >= 2) {
    r = await req('PUT', '/api/dev/backlog/' + blId, { status: 'accepted', priority: 'P1', admin_note: '纳入计划' }, sysCookie);
    check(r.status === 200, '[5.1] 更新需求状态/优先级/备注', r.json);
    const row = q1("SELECT status,priority,admin_note FROM dev_backlog WHERE id=" + blId);
    check(row[0] === 'accepted' && row[1] === 'P1' && row[2] === '纳入计划', '[5.2] 更新已落库', row);

    r = await req('PUT', '/api/dev/backlog/' + blId, { status: 'not_a_status' }, sysCookie);
    check(r.status === 400, '[5.3] 非法状态枚举 → 400', r.status);
    r = await req('PUT', '/api/dev/backlog/' + blId, { priority: 'P3' }, sysCookie);
    check(r.status === 400, '[5.4] 非法优先级 → 400', r.status);
    r = await req('PUT', '/api/dev/backlog/999999', { status: 'new' }, sysCookie);
    check(r.status === 404, '[5.5] 不存在的 id → 404', r.status);

    r = await req('PUT', '/api/dev/bugs/' + bugIds[0], { duplicate_of: 999999 }, sysCookie);
    check(r.status === 400, '[5.6] duplicate_of 指向不存在 → 400', r.status);
    r = await req('PUT', '/api/dev/bugs/' + bugIds[0], { status: 'duplicate', duplicate_of: Number(bugIds[1]) }, sysCookie);
    check(r.status === 200, '[5.7] duplicate_of 合法 → 200', r.json);
    const dupRow = q1("SELECT status,duplicate_of FROM dev_bugs WHERE id=" + bugIds[0]);
    check(dupRow[0] === 'duplicate' && dupRow[1] === bugIds[1], '[5.8] duplicate 状态与指向落库', dupRow);
  } else {
    check(false, '[5] 台账数据不足，无法验证更新', { blId, bugIds });
  }

  // ---------- [6] summary 与列表口径 ----------
  r = await req('GET', '/api/dev/summary', null, sysCookie);
  const s = r.json || {};
  const bt = q1("SELECT COUNT(*) FROM dev_backlog WHERE status<>'rejected'")[0];
  const gt = q1("SELECT COUNT(*) FROM dev_bugs WHERE status<>'duplicate'")[0];
  const tt = q1("SELECT (SELECT COUNT(*) FROM dev_backlog WHERE status='triage')+(SELECT COUNT(*) FROM dev_bugs WHERE status='triage')")[0];
  check(String(s.backlog_total) === bt && String(s.bugs_total) === gt && String(s.triage_total) === tt,
    '[6.1] summary 计数与库一致', s);

  r = await req('GET', '/api/dev/backlog', null, sysCookie);
  const items = (r.json || {}).items || [];
  check(items.every(it => it.status !== 'rejected'), '[6.2] 需求列表默认排除 rejected');
  check(items.every((it, i, a) => i === 0 || a[i - 1].occurrences >= it.occurrences),
    '[6.3] 需求列表按被提次数降序', items.map(it => it.occurrences));
  r = await req('GET', '/api/dev/backlog?status=rejected', null, sysCookie);
  check(((r.json || {}).items || []).every(it => it.status === 'rejected'), '[6.4] status 筛选生效');
  r = await req('GET', '/api/dev/bugs', null, sysCookie);
  check(((r.json || {}).items || []).every(it => it.status !== 'duplicate'), '[6.5] Bug 列表默认排除 duplicate');

  // ---------- [7] 前端页面与人工修改渠道 ----------
  const root = path.join(__dirname, '..');
  const html = fs.readFileSync(path.join(root, 'frontend/pages/dev.html'), 'utf8');
  const js = fs.readFileSync(path.join(root, 'frontend/static/dev.js'), 'utf8');
  check(fs.existsSync(path.join(root, 'frontend/static/dev.css')), '[7.1] dev.css 存在');
  check(html.includes('DEV_LABELS') && html.includes('此页面仅开发者可访问'), '[7.2] 展示名映射与不可访问提示在页面顶部');
  check(js.includes('/api/dev/summary') && js.includes('dv-denied'), '[7.3] 页面加载即探测权限（D5）');
  check(!html.includes('dedupe_key') && !html.includes('source_message_id'), '[7.4] 界面零技术术语（P6）');

  // ---------- [8] users.role 就位且既有登录不受影响 ----------
  const col = q1("SELECT COLUMN_DEFAULT FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='" + DB_NAME + "' AND TABLE_NAME='users' AND COLUMN_NAME='role'");
  check(col[0] === "'user'" || col[0] === 'user', '[8.1] users.role 列存在且默认 user', col);
  r = await req('GET', '/api/auth/me', null, sysCookie);
  check(r.status === 200, '[8.2] 既有登录/会话不受加列影响');

  console.log(fail ? `\n✗ stage34 有 ${fail} 项失败` : '\n✓ stage34 全部通过');
  process.exit(fail ? 1 : 0);
}

main().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
