// 阶段三十三：AI 导航 + 首页今日概览 + 周报动作锚点（v1.5.1-T005 HOME-NAV）
// 覆盖：导航元信息/游客问答/明确动作直跳/提案全生命周期/游客防刷/7 天清理/
//       周报 suggestions[] 锚点去重/前端静态资源与关键节点
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT } = require('./_env');
const API = `http://${API_HOST}:${API_PORT}`;

let pass = 0, fail = 0;
const failures = [];

function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const args = ['--socket=' + SOCK, '-uroot', '-p' + pw, DB_NAME, '-N', '--default-character-set=utf8mb4', '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout.trim();
}

function api(method, urlPath, body, opts = {}) {
  const { timeoutMs = 30000, withCookie = true, cookie = '' } = opts;
  return new Promise((resolve, reject) => {
    const u = new URL(urlPath, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = {
      'Content-Type': 'application/json',
      'Content-Length': data ? Buffer.byteLength(data) : 0
    };
    if (withCookie) require('./_system').withSystemCookie(headers);
    if (cookie) headers['Cookie'] = cookie;
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers, timeout: timeoutMs
    }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        try { resolve({ status: res.statusCode, json: JSON.parse(text), text, headers: res.headers }); }
        catch { resolve({ status: res.statusCode, json: null, text, headers: res.headers }); }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

function pad2(n) { return String(n).padStart(2, '0'); }
function fmt(d) { return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())}`; }
function shift(dateStr, days) {
  const d = new Date(dateStr + 'T00:00:00');
  d.setDate(d.getDate() + days);
  return fmt(d);
}
function lastWeekMonday() {
  const d = new Date();
  d.setHours(0, 0, 0, 0);
  d.setDate(d.getDate() - ((d.getDay() + 6) % 7) - 7);
  return fmt(d);
}
const WK = lastWeekMonday();

function seedWeekly() {
  runMysql(`DELETE FROM user_weekly_goal WHERE user_id=1 AND goal_id=9003`);
  runMysql(`DELETE FROM error_attributions WHERE user_id=1 AND source_id='stage33'`);
  runMysql(`DELETE FROM retry_items WHERE user_id=1 AND knowledge_id IN (991001,991002)`);
  runMysql(`DELETE FROM user_learning WHERE user_id=1 AND knowledge_id IN (991001,991002)`);
  runMysql(`DELETE FROM weekly_report_ai WHERE user_id=1 AND week_start='${WK}'`);
  runMysql(
    `INSERT INTO user_weekly_goal
      (user_id,year_week,goal_type,week_key,goal_id,metric_key,target,progress,achieved,effective_target)
     VALUES (1,0,'review_items','${WK}',9003,'review_items',10,6,0,10);`
  );
  runMysql(
    `INSERT INTO error_attributions (user_id,knowledge_id,source_type,source_id,question_no,error_type,created_at)
     VALUES (1,991001,'exercise','stage33',1,'spelling','${shift(WK, 1)} 10:00:00'),
            (1,991002,'exercise','stage33',2,'spelling','${shift(WK, 2)} 10:00:00');`
  );
  runMysql(`INSERT INTO retry_items (user_id,knowledge_id,status) VALUES (1,991001,'queued');`);
  runMysql(
    `INSERT INTO user_learning (user_id,knowledge_id,proficiency_level,last_review_time)
     VALUES (1,991001,3,'${shift(WK, 1)} 09:00:00'),(1,991002,2,'${shift(WK, 2)} 09:00:00');`
  );
}

async function waitTask(taskId, timeoutMs = 20000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    const r = await api('GET', '/api/ai-tasks/' + taskId);
    if (r.json && ['done', 'failed', 'partial'].includes(r.json.status)) return r.json;
    await new Promise(res => setTimeout(res, 250));
  }
  return null;
}

function isValidRoute(route) {
  return typeof route === 'string' && route.indexOf('/pages/') === 0 && route.indexOf('//') < 0;
}

async function main() {
  console.log('=== 阶段三十三：AI 导航 + 今日概览 + 周报锚点（v1.5.1-T005）===\n');

  try {
    console.log('[0] 前置检查');
    const h = await api('GET', '/api/health');
    if (h.json && h.json.status === 'ok') ok('后端健康检查');
    else ko('后端健康检查', '未就绪');

    runMysql(`DELETE FROM ai_nav_messages WHERE user_id=1`);

    console.log('\n[1] 导航元信息');
    const meta = await api('GET', '/api/ai-nav/messages?page=/index.html', null, { withCookie: false });
    if (meta.status === 200 && meta.json && meta.json.ok) ok('未登录可读取导航元信息');
    else ko('未登录可读取导航元信息', meta.status + ' ' + meta.text.slice(0, 120));
    if (meta.json && typeof meta.json.welcome === 'string' && meta.json.welcome.length > 0) ok('返回欢迎语');
    else ko('返回欢迎语', 'welcome 为空');
    if (meta.json && Array.isArray(meta.json.quick_questions) && meta.json.quick_questions.length === 3) ok('固定快捷问题 3 个');
    else ko('固定快捷问题 3 个', JSON.stringify(meta.json && meta.json.quick_questions));
    if (meta.json && typeof meta.json.context_question === 'string' && meta.json.context_question.length > 0) ok('首页情境问题存在');
    else ko('首页情境问题存在', 'context_question 为空');

    console.log('\n[2] 游客问答与明确动作');
    const guestCookie = 'eng_guest_id=nav33normal';
    const ask = await api('POST', '/api/ai-nav/messages', { text: '今天该学什么？', page: '/index.html' },
      { withCookie: false, cookie: guestCookie });
    if (ask.status === 200 && ask.json && ask.json.reply && ask.json.reply.content) ok('游客问答返回回复');
    else ko('游客问答返回回复', ask.status + ' ' + ask.text.slice(0, 160));
    if (ask.json && ask.json.reply && ask.json.reply.action && isValidRoute(ask.json.reply.action.route)) ok('AI 回复自动挂动作锚点', ask.json.reply.action.route);
    else ko('AI 回复自动挂动作锚点', JSON.stringify(ask.json && ask.json.reply && ask.json.reply.action));

    const direct = await api('POST', '/api/ai-nav/messages', { text: '打开错题本', page: '/index.html' },
      { withCookie: false, cookie: guestCookie });
    if (direct.json && direct.json.reply && direct.json.reply.content.indexOf('已为你跳转：') === 0) ok('明确动作直接跳转');
    else ko('明确动作直接跳转', direct.text.slice(0, 160));
    if (direct.json && direct.json.reply && direct.json.reply.action && direct.json.reply.action.route.indexOf('/pages/wrongbook.html') === 0) ok('错题本路由正确', direct.json.reply.action.route);
    else ko('错题本路由正确', JSON.stringify(direct.json && direct.json.reply && direct.json.reply.action));

    // v1.5.3-T003：引导重放动作精确命中 + 误伤回归（含「引导」的报障不直跳）
    const ob = await api('POST', '/api/ai-nav/messages', { text: '再看一遍引导', page: '/index.html' },
      { withCookie: false, cookie: guestCookie });
    if (ob.json && ob.json.reply && ob.json.reply.action && ob.json.reply.action.route === '/?onboard=1') ok('引导重放命中直跳路由', ob.json.reply.action.route);
    else ko('引导重放命中直跳路由', JSON.stringify(ob.json && ob.json.reply));
    const obMiss = await api('POST', '/api/ai-nav/messages', { text: '引导页报错了', page: '/index.html' },
      { withCookie: false, cookie: guestCookie });
    if (obMiss.json && obMiss.json.reply && String(obMiss.json.reply.content).indexOf('已为你跳转') !== 0) ok('含「引导」报障不直跳（误伤回归）');
    else ko('含「引导」报障不直跳（误伤回归）', JSON.stringify(obMiss.json && obMiss.json.reply && obMiss.json.reply.action));

    // v1.5.3-T003：在功能页说「新手引导」应开本页导览，不再跳回首页
    const obPage = await api('POST', '/api/ai-nav/messages', { text: '开启本页的新手引导', page: '/pages/review.html' },
      { withCookie: false, cookie: 'eng_guest_id=nav33tour' });
    if (obPage.json && obPage.json.reply && obPage.json.reply.action && obPage.json.reply.action.route === '/pages/review.html?tour=1') ok('本页引导命中本页导览路由', obPage.json.reply.action.route);
    else ko('本页引导命中本页导览路由', JSON.stringify(obPage.json && obPage.json.reply));

    console.log('\n[3] 游客防刷');
    const rateCookie = 'eng_guest_id=nav33rate';
    let limited = 0, lastStatus = 0;
    for (let i = 1; i <= 7; i++) {
      const r = await api('POST', '/api/ai-nav/messages', { text: '今天该学什么？', page: '/index.html' },
        { withCookie: false, cookie: rateCookie });
      lastStatus = r.status;
      if (r.status === 429) { limited = i; break; }
    }
    if (limited === 7) ok('同一游客第 7 次被限流（429）');
    else ko('同一游客第 7 次被限流（429）', `首次限流序号=${limited || '无'}，最后状态=${lastStatus}`);

    const longText = await api('POST', '/api/ai-nav/messages', { text: 'a'.repeat(501), page: '/index.html' },
      { withCookie: false, cookie: 'eng_guest_id=nav33long' });
    if (longText.status === 400) ok('超过 500 字返回 400');
    else ko('超过 500 字返回 400', 'got ' + longText.status);

    console.log('\n[4] 课规提案');
    const planText = '帮我排个计划，每天复习20个词，坚持7天';
    const prop = await api('POST', '/api/ai-nav/messages', { text: planText, page: '/index.html' });
    if (prop.json && prop.json.reply && prop.json.reply.proposal && prop.json.reply.proposal_status === 'proposed') ok('登录用户生成提案');
    else ko('登录用户生成提案', prop.text.slice(0, 200));
    const propId = prop.json && prop.json.reply ? prop.json.reply.id : 0;
    if (prop.json && prop.json.reply && prop.json.reply.proposal && Number(prop.json.reply.proposal.daily_new_words) > 0) ok('提案数值由代码生成', '新词/天=' + prop.json.reply.proposal.daily_new_words);
    else ko('提案数值由代码生成', JSON.stringify(prop.json && prop.json.reply && prop.json.reply.proposal));

    const guestConfirm = await api('POST', `/api/ai-nav/proposals/${propId}/confirm`, { activate_from: 'today' },
      { withCookie: false, cookie: guestCookie });
    if (guestConfirm.status === 401) ok('游客确认提案被拒（401）');
    else ko('游客确认提案被拒（401）', 'got ' + guestConfirm.status);

    const confirm = await api('POST', `/api/ai-nav/proposals/${propId}/confirm`, { activate_from: 'today' });
    if (confirm.status === 200 && confirm.json && confirm.json.ok) ok('登录用户确认提案成功');
    else ko('登录用户确认提案成功', confirm.status + ' ' + confirm.text.slice(0, 160));
    if (confirm.json && /^\d{4}-\d{2}-\d{2}$/.test(confirm.json.deadline || '')) ok('返回课规截止日期', confirm.json.deadline);
    else ko('返回课规截止日期', JSON.stringify(confirm.json && confirm.json.deadline));
    const goalCnt = Number(runMysql(
      `SELECT COUNT(*) FROM user_goals WHERE user_id=1 AND parse_source='ai_nav' AND raw_text='${planText}'`) || 0);
    if (goalCnt === 1) ok('课规写入 user_goals（唯一）');
    else ko('课规写入 user_goals（唯一）', 'count=' + goalCnt);

    const again = await api('POST', `/api/ai-nav/proposals/${propId}/confirm`, { activate_from: 'today' });
    if (again.status === 200 && again.json && again.json.ok) ok('重复确认幂等');
    else ko('重复确认幂等', again.status + ' ' + again.text.slice(0, 120));

    console.log('\n[5] 放弃与过期');
    const p2 = await api('POST', '/api/ai-nav/messages', { text: '帮我排个计划，每天复习15个词', page: '/index.html' });
    const id2 = p2.json && p2.json.reply ? p2.json.reply.id : 0;
    const disc = await api('POST', `/api/ai-nav/proposals/${id2}/discard`);
    if (disc.status === 200 && disc.json && disc.json.ok) ok('放弃提案成功');
    else ko('放弃提案成功', disc.status + ' ' + disc.text.slice(0, 120));
    const discStatus = runMysql(`SELECT proposal_status FROM ai_nav_messages WHERE id=${id2}`);
    if (discStatus === 'discarded') ok('放弃状态已落库');
    else ko('放弃状态已落库', discStatus);
    const discConfirm = await api('POST', `/api/ai-nav/proposals/${id2}/confirm`, { activate_from: 'today' });
    if (discConfirm.status === 400) ok('已放弃提案不可确认');
    else ko('已放弃提案不可确认', 'got ' + discConfirm.status);

    const p3 = await api('POST', '/api/ai-nav/messages', { text: '帮我排个计划，每天复习5个词', page: '/index.html' });
    const id3 = p3.json && p3.json.reply ? p3.json.reply.id : 0;
    runMysql(`UPDATE ai_nav_messages SET created_at = DATE_SUB(NOW(), INTERVAL 25 HOUR) WHERE id=${id3}`);
    const expired = await api('POST', `/api/ai-nav/proposals/${id3}/confirm`, { activate_from: 'today' });
    if (expired.status === 400 && /过期/.test(expired.text)) ok('超 24 小时提案不可确认');
    else ko('超 24 小时提案不可确认', expired.status + ' ' + expired.text.slice(0, 120));

    console.log('\n[6] 历史与清理');
    const history = await api('GET', '/api/ai-nav/messages?page=/index.html');
    const hmsgs = history.json && Array.isArray(history.json.messages) ? history.json.messages : [];
    const confirmedMsg = hmsgs.find(m => m.id === propId);
    if (confirmedMsg && confirmedMsg.proposal_status === 'confirmed') ok('历史消息带提案状态');
    else ko('历史消息带提案状态', JSON.stringify(confirmedMsg && confirmedMsg.proposal_status));

    runMysql(
      `INSERT INTO ai_nav_messages (user_id,role,content,action_json,proposal_json,proposal_status,created_at)
       VALUES (1,'assistant','stage33-old','',NULL,'none',DATE_SUB(NOW(), INTERVAL 8 DAY))`
    );
    const oldId = Number(runMysql(
      `SELECT id FROM ai_nav_messages WHERE user_id=1 AND content='stage33-old' ORDER BY id DESC LIMIT 1`) || 0);
    const history2 = await api('GET', '/api/ai-nav/messages?page=/index.html');
    const stillOld = (history2.json && history2.json.messages || []).some(m => m.id === oldId);
    const oldCnt = Number(runMysql(`SELECT COUNT(*) FROM ai_nav_messages WHERE id=${oldId}`) || 0);
    if (!stillOld && oldCnt === 0) ok('8 天前消息读取时被清理');
    else ko('8 天前消息读取时被清理', `返回=${stillOld} 库内=${oldCnt}`);

    const clearRes = await api('DELETE', '/api/ai-nav/messages');
    const history3 = await api('GET', '/api/ai-nav/messages?page=/index.html');
    const leftCnt = history3.json && Array.isArray(history3.json.messages) ? history3.json.messages.length : -1;
    if (clearRes.status === 200 && clearRes.json && clearRes.json.ok && hmsgs.length > 0 && leftCnt === 0)
      ok('登录用户可清空对话');
    else ko('登录用户可清空对话', `status=${clearRes.status} before=${hmsgs.length} after=${leftCnt}`);
    const guestClear = await api('DELETE', '/api/ai-nav/messages', null, { withCookie: false });
    if (guestClear.status === 401) ok('游客清空对话需登录');
    else ko('游客清空对话需登录', guestClear.status + ' ' + guestClear.text.slice(0, 120));

    console.log('\n[7] 周报动作锚点');
    seedWeekly();
    const wr = await api('GET', `/api/weekly-reports?week=${WK}`);
    const sug = wr.json && Array.isArray(wr.json.suggestions) ? wr.json.suggestions : [];
    if (wr.status === 200 && sug.length > 0) ok('周报返回 suggestions[]', sug.length + ' 条');
    else ko('周报返回 suggestions[]', wr.status + ' ' + wr.text.slice(0, 160));
    const ruleSug = sug.filter(s => s.source === 'rule');
    if (ruleSug.some(s => s.action && isValidRoute(s.action.route))) ok('规则建议命中动作锚点', ruleSug.filter(s => s.action).map(s => s.action.id).join(','));
    else ko('规则建议命中动作锚点', JSON.stringify(ruleSug));

    const aiPost = await api('POST', '/api/weekly-reports/ai', { week_start: WK });
    const taskId = aiPost.json && (aiPost.json.task_id || aiPost.json.taskId);
    if (aiPost.status === 200 && taskId) ok('触发 AI 建议生成', 'task=' + taskId);
    else ko('触发 AI 建议生成', aiPost.status + ' ' + aiPost.text.slice(0, 160));
    if (taskId) {
      const t = await waitTask(taskId);
      if (t && t.status === 'done') ok('AI 建议任务完成');
      else ko('AI 建议任务完成', JSON.stringify(t));
    }

    const wr2 = await api('GET', `/api/weekly-reports?week=${WK}`);
    const sug2 = wr2.json && Array.isArray(wr2.json.suggestions) ? wr2.json.suggestions : [];
    const aiSug = sug2.filter(s => s.source === 'ai');
    if (aiSug.length > 0) ok('AI 建议逐行进入 suggestions[]', aiSug.length + ' 条');
    else ko('AI 建议逐行进入 suggestions[]', JSON.stringify(sug2));
    const ids = sug2.filter(s => s.action).map(s => s.action.id);
    if (new Set(ids).size === ids.length) ok('同一动作整页只挂一个锚点', ids.join(','));
    else ko('同一动作整页只挂一个锚点', ids.join(','));
    if (sug2.filter(s => s.action).every(s => isValidRoute(s.action.route))) ok('所有锚点路由在站内白名单');
    else ko('所有锚点路由在站内白名单', JSON.stringify(sug2.filter(s => s.action)));

    console.log('\n[8] 前端静态资源');
    const f = p => fs.readFileSync(path.join(ROOT, p), 'utf8');
    if (fs.existsSync(path.join(ROOT, 'frontend/static/ai-nav.js')) &&
        fs.existsSync(path.join(ROOT, 'frontend/static/ai-nav.css'))) ok('ai-nav.js / ai-nav.css 存在');
    else ko('ai-nav.js / ai-nav.css 存在', '文件缺失');
    if (f('frontend/static/ai-nav.js').includes('data-na-confirm') &&
        f('frontend/static/ai-nav.js').includes('ai-nav-fab')) ok('导航前端含入口与提案交互');
    else ko('导航前端含入口与提案交互', '关键标记缺失');
    if (f('frontend/static/nav.js').includes('injectAiNav')) ok('nav.js 幂等注入导航');
    else ko('nav.js 幂等注入导航', '未找到 injectAiNav');
    if (f('frontend/index.html').includes('today-coin') && f('frontend/index.html').includes('今日概览')) ok('首页今日概览四格卡');
    else ko('首页今日概览四格卡', '关键节点缺失');
    if (f('frontend/index.html').includes('href="/pages/review.html"') &&
        f('frontend/index.html').includes('data-treasury-open')) ok('首页四格可点击跳转');
    else ko('首页四格可点击跳转', '缺少链接或宝库入口');
    if (f('frontend/pages/learning-center.html').includes('d.suggestions')) ok('周报按 suggestions[] 渲染');
    else ko('周报按 suggestions[] 渲染', '未使用 suggestions');
    if (f('frontend/static/ai-nav.js').includes('STORE_GUEST_MSGS') &&
        f('frontend/static/ai-nav.js').includes('ai-nav-jump')) ok('体验优化：游客会话保留与跳转可取消');
    else ko('体验优化：游客会话保留与跳转可取消', '关键实现缺失');
    if (f('frontend/static/ai-nav.js').includes('ai-nav-quick') &&
        f('frontend/static/ai-nav.js').includes('清空对话')) ok('常用问题固定与清空入口可见');
    else ko('常用问题固定与清空入口可见', '关键标记缺失');
  } catch (e) {
    ko('测试执行', e.stack || e.message);
  }

  console.log(`\n=== 结果: ${pass} 通过, ${fail} 失败 ===\n`);
  if (fail > 0) {
    failures.forEach(x => console.log(`  ✗ ${x.name} — ${x.msg}`));
    process.exit(1);
  }
  process.exit(0);
}

main();
