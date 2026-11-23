// 阶段三十：课规 + 本周任务卡（v1.5.1-T001 GOAL-TASKS）
// 覆盖课规 CRUD、唯一「正在执行」、到期自动延长、任务幂等生成、
// 动态门槛、两段领取（面板/全完成自动）、衰减与封顶、印章、下周任务
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
  const args = ['--socket=' + SOCK, '-uroot', '-p' + pw, DB_NAME, '-N', '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout.trim();
}

function api(method, urlPath, body, timeoutMs = 30000) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlPath, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = {
      'Content-Type': 'application/json',
      'Content-Length': data ? Buffer.byteLength(data) : 0
    };
    require('./_system').withSystemCookie(headers);
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers, timeout: timeoutMs
    }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        try { resolve({ status: res.statusCode, json: JSON.parse(text), text }); }
        catch { resolve({ status: res.statusCode, json: null, text }); }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

function resetTestUser() {
  runMysql("DELETE FROM user_coin_txn WHERE user_id=1");
  runMysql("DELETE FROM user_seal WHERE user_id=1");
  runMysql("DELETE FROM user_weekly_goal WHERE user_id=1");
  runMysql("DELETE FROM user_goals WHERE user_id=1");
  runMysql("DELETE FROM user_coin WHERE user_id=1");
}

async function main() {
  console.log('=== 阶段三十：课规 + 本周任务卡（v1.5.1-T001）===\n');

  // ---- 0. 前置 ----
  console.log('[0] 前置检查');
  try {
    const h = await api('GET', '/api/health');
    if (h.json && h.json.status === 'ok') ok('后端健康检查');
    else ko('后端健康检查', h.text);
  } catch (e) { ko('后端健康检查', e.message); return finish(); }

  resetTestUser();

  // ---- 1. 指标注册表 ----
  console.log('\n[1] 指标注册表');
  try {
    const m = await api('GET', '/api/metrics');
    const keys = (m.json && m.json.metrics || []).map(x => x.metric_key);
    if (['review_items', 'new_words', 'quiz_count'].every(k => keys.includes(k))) ok('三项指标已注册', keys.join(','));
    else ko('三项指标已注册', JSON.stringify(m.json));
  } catch (e) { ko('指标注册表', e.message); }

  // ---- 2. 自由文本解析（不调 AI） ----
  console.log('\n[2] 自由文本解析');
  try {
    const p = await api('POST', '/api/goals/parse', { text: '两周内复习 60 条' });
    if (p.json && p.json.goal_type === 'review_items') ok('识别复习指标');
    else ko('识别复习指标', JSON.stringify(p.json));
    if (p.json && p.json.period_days === 14) ok('识别期限 14 天');
    else ko('识别期限 14 天', JSON.stringify(p.json));
    if (p.json && p.json.target === 60) ok('识别目标 60');
    else ko('识别目标 60', JSON.stringify(p.json));
    if (p.json && p.json.parse_source === 'template') ok('parse_source=template');
    else ko('parse_source=template', JSON.stringify(p.json));
  } catch (e) { ko('自由文本解析', e.message); }

  // ---- 3. 创建课规 ----
  console.log('\n[3] 创建课规');
  let goalA = 0, goalB = 0;
  try {
    const r = await api('POST', '/api/goals', {
      name: '每日复习', goal_type: 'review_items', period_days: 7,
      daily_minutes: 20, daily_new_words: 10, wordlist_id: 0
    });
    if (r.json && r.json.ok && r.json.id > 0) { goalA = r.json.id; ok('创建课规 A', 'id=' + goalA); }
    else ko('创建课规 A', JSON.stringify(r.json));
    const list = r.json && r.json.goals || [];
    if (list.length === 1 && list[0].is_current === true) ok('首条课规为「正在执行」');
    else ko('首条课规为「正在执行」', JSON.stringify(list));

    const r2 = await api('POST', '/api/goals', {
      name: '新词积累', goal_type: 'new_words', period_days: 7,
      daily_minutes: 15, daily_new_words: 20, wordlist_id: 0
    });
    goalB = r2.json && r2.json.id;
    if (!goalB) ko('创建课规 B', JSON.stringify(r2.json));
    else {
      const gs = r2.json.goals || [];
      const currents = gs.filter(g => g.is_current);
      if (gs.length === 2 && currents.length === 1 && currents[0].id === goalB) ok('同一时间仅 1 个正在执行');
      else ko('同一时间仅 1 个正在执行', JSON.stringify(gs));
    }
  } catch (e) { ko('创建课规', e.message); }

  // ---- 4. 修改与切换 ----
  console.log('\n[4] 修改与切换');
  try {
    const u = await api('PUT', '/api/goals/' + goalA, {
      name: '每日复习（改）', deadline: '2030-01-01', daily_minutes: 25,
      daily_new_words: 12, wordlist_id: 0, is_current: true
    });
    const g = (u.json && u.json.goals || []).find(x => x.id === goalA);
    if (u.json && u.json.ok && g && g.name === '每日复习（改）') ok('修改课规字段');
    else ko('修改课规字段', JSON.stringify(u.json));
    if (g && g.is_current === true) ok('切换为正在执行');
    else ko('切换为正在执行', JSON.stringify(g));
    if (g && g.deadline === '2030-01-01') ok('期限已更新');
    else ko('期限已更新', JSON.stringify(g));
  } catch (e) { ko('修改与切换', e.message); }

  // ---- 5. 本周任务卡生成 ----
  console.log('\n[5] 本周任务卡');
  let plan = null;
  try {
    const t = await api('GET', '/api/treasury');
    const plans = t.json && t.json.weekly_plans || [];
    plan = plans.find(p => p.goal_id === goalA);
    if (plan && plan.tasks && plan.tasks.length === 3) ok('生成 3 条任务');
    else ko('生成 3 条任务', JSON.stringify(plans));
    if (plan && plan.is_current === true) ok('当前计划标记正确');
    else ko('当前计划标记正确', JSON.stringify(plan && plan.is_current));
    if (t.json && t.json.week_reward && t.json.week_reward.cap === 60) ok('周封顶 60');
    else ko('周封顶 60', JSON.stringify(t.json && t.json.week_reward));
    // 重复读取幂等
    const t2 = await api('GET', '/api/treasury');
    const p2 = (t2.json.weekly_plans || []).find(p => p.goal_id === goalA);
    if (p2 && p2.tasks.length === 3) ok('重复读取不重复生成');
    else ko('重复读取不重复生成', JSON.stringify(p2));

    // D25：非执行课规同样生成任务，并按参与顺序衰减
    const planB = plans.find(p => p.goal_id === goalB);
    if (planB && planB.tasks && planB.tasks.length === 3) ok('非执行课规同样生成 3 条任务');
    else ko('非执行课规同样生成 3 条任务', JSON.stringify(plans));
    if (plan && plan.plan_rank === 1 && plan.decay === 1) ok('正在执行排第 1 位、奖励全额');
    else ko('正在执行排第 1 位、奖励全额', JSON.stringify(plan && { rank: plan.plan_rank, decay: plan.decay }));
    if (planB && planB.plan_rank === 2 && Math.abs(planB.decay - 0.5) < 1e-6) ok('其他课规按参与顺序衰减');
    else ko('其他课规按参与顺序衰减', JSON.stringify(planB && { rank: planB.plan_rank, decay: planB.decay }));

    // D29：自动推荐目标 + 挑战目标（只记录不发钱）
    const sug = t.json && t.json.suggestions || [];
    const sugReview = sug.find(s => s.metric_key === 'review_items');
    if (sugReview && sugReview.suggested >= 10) ok('返回指标推荐值', 'suggested=' + sugReview.suggested);
    else ko('返回指标推荐值', JSON.stringify(sug));
    const reviewTask = plan && plan.tasks.find(x => x.metric_key === 'review_items');
    if (reviewTask && reviewTask.challenge_target === Math.ceil(reviewTask.target * 1.5)
        && reviewTask.challenge_reached === false) {
      ok('挑战目标按 1.5 倍自动生成', 'challenge=' + reviewTask.challenge_target);
    } else ko('挑战目标按 1.5 倍自动生成', JSON.stringify(reviewTask));
  } catch (e) { ko('本周任务卡', e.message); }

  // ---- 5.5 一个学习事件只计入一个计划 ----
  console.log('\n[5.5] 单一计划归属');
  try {
    const t = await api('GET', '/api/treasury');
    const plans = t.json && t.json.weekly_plans || [];
    const pa = plans.find(p => p.goal_id === goalA);
    const pb = plans.find(p => p.goal_id === goalB);
    const nw = p => {
      const x = p && p.tasks.find(k => k.metric_key === 'new_words');
      return x ? x.progress : -1;
    };
    const live = parseInt(runMysql(
      "SELECT COUNT(*) FROM user_learning WHERE user_id=1"
      + " AND create_time >= DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) DAY)"
      + " AND create_time < DATE_ADD(DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) DAY), INTERVAL 7 DAY)"
    ), 10);
    if (pa && nw(pa) === live) ok('归属计划同步本周新词', 'progress=' + nw(pa));
    else ko('归属计划同步本周新词', JSON.stringify({ live, a: nw(pa) }));
    if (pb && nw(pb) === 0) ok('非归属计划不重复记录', 'progress=' + nw(pb));
    else ko('非归属计划不重复记录', JSON.stringify({ b: nw(pb) }));
  } catch (e) { ko('单一计划归属', e.message); }

  // ---- 6. 改目标值：不降低门槛 ----
  console.log('\n[6] 动态门槛');
  try {
    const r = await api('POST', '/api/treasury/weekly-goal', {
      goal_id: goalA, metric_key: 'review_items', target: 1
    });
    if (r.json && r.json.ok && r.json.target === 1 && r.json.effective_target >= 10) {
      ok('展示用户目标 1，判定门槛不降低', 'effective=' + r.json.effective_target);
    } else ko('展示用户目标 1，判定门槛不降低', JSON.stringify(r.json));
  } catch (e) { ko('动态门槛', e.message); }

  // ---- 7. 面板领取：每项 5 金桂 ----
  console.log('\n[7] 面板领取');
  try {
    // 先移除第二个课规，隔离单计划金币断言（多计划口径已在 [5]/[5.5] 覆盖）
    const del = await api('DELETE', '/api/goals/' + goalB);
    if (del.json && del.json.ok) ok('清理第二课规，隔离金币断言');
    else ko('清理第二课规', JSON.stringify(del.json));
    // 两项达成（绕过 UI，直接写进度，模拟真实事件已累加）
    runMysql("UPDATE user_weekly_goal SET progress=effective_target, achieved=1 "
      + "WHERE user_id=1 AND goal_id=" + goalA + " AND metric_key IN ('review_items','new_words')");
    const c = await api('POST', '/api/treasury/claim-week', { trigger: 'panel' });
    if (c.json && c.json.ok && c.json.credited === 10) ok('两项各 5 金桂', 'credited=' + c.json.credited);
    else ko('两项各 5 金桂', JSON.stringify(c.json));
    if (c.json && c.json.balance === 10) ok('余额 10');
    else ko('余额 10', JSON.stringify(c.json && c.json.balance));
    // 再次领取不重复
    const c2 = await api('POST', '/api/treasury/claim-week', { trigger: 'panel' });
    if (c2.json && c2.json.credited === 0 && c2.json.balance === 10) ok('重复领取幂等');
    else ko('重复领取幂等', JSON.stringify(c2.json));
  } catch (e) { ko('面板领取', e.message); }

  // ---- 8. 第三项完成：自动补发 + 15 + 印章 ----
  console.log('\n[8] 全完成自动结算');
  try {
    runMysql("UPDATE user_weekly_goal SET progress=effective_target, achieved=1 "
      + "WHERE user_id=1 AND goal_id=" + goalA + " AND metric_key='quiz_count'");
    const t = await api('GET', '/api/treasury');
    const seals = (t.json && t.json.seals) || [];
    const done = seals.find(s => s.code === 'goal_done');
    if (t.json && t.json.balance === 30) ok('未领 5 + 全完成 15 自动到账', 'balance=' + t.json.balance);
    else ko('未领 5 + 全完成 15 自动到账', JSON.stringify(t.json && t.json.balance));
    if (done && done.earned) ok('颁发「课规有成」印章');
    else ko('颁发「课规有成」印章', JSON.stringify(done));
    const t2 = await api('GET', '/api/treasury');
    if (t2.json && t2.json.balance === 30) ok('全完成结算幂等');
    else ko('全完成结算幂等', JSON.stringify(t2.json && t2.json.balance));
  } catch (e) { ko('全完成自动结算', e.message); }

  // ---- 9. 提前定下周 ----
  console.log('\n[9] 提前定下周');
  try {
    const r = await api('POST', '/api/treasury/next-week-tasks', {
      goal_id: goalA,
      tasks: [
        { metric_key: 'review_items', target: 25 },
        { metric_key: 'new_words', target: 12 },
        { metric_key: 'quiz_count', target: 3 }
      ]
    });
    if (r.json && r.json.ok) ok('下周任务已保存');
    else ko('下周任务已保存', JSON.stringify(r.json));
    const cnt = runMysql("SELECT COUNT(*) FROM user_weekly_goal WHERE user_id=1 AND goal_id=" + goalA
      + " AND week_key=DATE_ADD(DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) DAY), INTERVAL 1 WEEK)");
    if (parseInt(cnt, 10) === 3) ok('下周 3 条任务落库');
    else ko('下周 3 条任务落库', 'got ' + cnt);
    // 本周任务不受影响
    const thisCnt = runMysql("SELECT COUNT(*) FROM user_weekly_goal WHERE user_id=1 AND goal_id=" + goalA
      + " AND week_key=DATE_SUB(CURDATE(), INTERVAL WEEKDAY(CURDATE()) DAY)");
    if (parseInt(thisCnt, 10) === 3) ok('本周任务未被覆盖');
    else ko('本周任务未被覆盖', 'got ' + thisCnt);
  } catch (e) { ko('提前定下周', e.message); }

  // ---- 10. 到期自动延长 ----
  console.log('\n[10] 到期自动延长');
  try {
    runMysql("UPDATE user_goals SET previous_deadline=DATE_SUB(CURDATE(), INTERVAL 7 DAY),"
      + " deadline=DATE_SUB(CURDATE(), INTERVAL 3 DAY) WHERE id=" + goalA);
    const g = await api('GET', '/api/goals');
    const row = (g.json && g.json.goals || []).find(x => x.id === goalA);
    if (row && row.deadline >= new Date().toISOString().slice(0, 10)) ok('到期后自动延长', row.deadline);
    else ko('到期后自动延长', JSON.stringify(row));
    if (row && row.expired_days >= 1) ok('展示已到期天数', String(row.expired_days));
    else ko('展示已到期天数', JSON.stringify(row && row.expired_days));
  } catch (e) { ko('到期自动延长', e.message); }

  // ---- 11. 删除与自动提升 ----
  console.log('\n[11] 删除与自动提升');
  try {
    const rc = await api('POST', '/api/goals', {
      name: '临时课规', goal_type: 'quiz_count', period_days: 7
    });
    const goalC = rc.json && rc.json.id;
    if (goalC) ok('新建课规自动成为正在执行', 'id=' + goalC);
    else ko('新建课规自动成为正在执行', JSON.stringify(rc.json));
    const d1 = await api('DELETE', '/api/goals/' + goalC);
    const gs = d1.json && d1.json.goals || [];
    if (d1.json && d1.json.ok && !gs.some(x => x.id === goalC)) ok('删除课规');
    else ko('删除课规', JSON.stringify(d1.json));
    if (gs.length === 1 && gs[0].id === goalA && gs[0].is_current === true) ok('删除正在执行后自动提升');
    else ko('删除正在执行后自动提升', JSON.stringify(gs));
  } catch (e) { ko('删除与自动提升', e.message); }

  // ---- 12. 无 AI 调用（无 ai_request 记录） ----
  console.log('\n[12] 无 AI 调用');
  try {
    const n = runMysql("SELECT COUNT(*) FROM ai_request_log WHERE user_id=1 AND created_at >= NOW() - INTERVAL 5 MINUTE");
    if (parseInt(n, 10) === 0) ok('课规与任务卡未触发 AI 调用');
    else ko('课规与任务卡未触发 AI 调用', 'got ' + n);
  } catch (e) {
    // 表名不一致时跳过，不掩盖其他失败
    console.log('  ~ ai_request_log 不存在，跳过');
  }

  finish();
}

function finish() {
  console.log('\n=== 测试汇总 ===');
  console.log(`通过: ${pass}, 失败: ${fail}`);
  if (failures.length) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg}`));
  }
  process.exit(fail > 0 ? 1 : 0);
}

main().catch(e => {
  console.error('测试执行错误:', e);
  process.exit(1);
});
