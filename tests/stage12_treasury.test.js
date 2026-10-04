// 阶段十二：点金圣殿（v1.3.0-T005）集成测试
// 覆盖 user_coin/user_coin_txn/user_seal/user_title/user_weekly_goal 五表、
// 余额/流水/印章/称谓/课规等接口、前端页面元素
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
  const args = ['--socket='+SOCK, '-uroot', '-p'+pw, 'english_learning', '-N', '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout.trim();
}

function api(method, urlPath, body, timeoutMs=30000) {
  return new Promise((resolve, reject) => {
    const u = new URL(urlPath, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json',
                      'Content-Length': data ? Buffer.byteLength(data) : 0 };
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers, timeout: timeoutMs
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
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

// 清理测试数据：重置 user1 的金桂账户
function resetTestUser() {
  try { runMysql("DELETE FROM user_coin_txn WHERE user_id=1"); } catch(e) {}
  try { runMysql("DELETE FROM user_seal WHERE user_id=1"); } catch(e) {}
  try { runMysql("DELETE FROM user_title WHERE user_id=1"); } catch(e) {}
  try { runMysql("DELETE FROM user_weekly_goal WHERE user_id=1"); } catch(e) {}
  try { runMysql("DELETE FROM user_coin WHERE user_id=1"); } catch(e) {}
}

async function main() {
  console.log('=== 阶段十二：点金圣殿（v1.3.0-T005）集成测试 ===\n');

  resetTestUser();

  // ---- 0. 健康检查 ----
  console.log('[0] 前置检查');
  try {
    const r = await api('GET', '/api/health');
    if (r.json && r.json.status === 'ok') ok('后端健康检查');
    else ko('后端健康检查', r.text);
  } catch(e) { ko('后端健康检查', e.message); }

  // ---- 1. 数据库表存在 ----
  console.log('\n[1] 数据表检查');
  try {
    const tables = runMysql('SHOW TABLES').split('\n');
    if (tables.includes('user_coin_txn')) ok('user_coin_txn 表存在');
    else ko('user_coin_txn 表存在', '未找到');
    if (tables.includes('user_seal')) ok('user_seal 表存在');
    else ko('user_seal 表存在', '未找到');
    if (tables.includes('user_title')) ok('user_title 表存在');
    else ko('user_title 表存在', '未找到');
    if (tables.includes('user_weekly_goal')) ok('user_weekly_goal 表存在');
    else ko('user_weekly_goal 表存在', '未找到');

    // user_coin 应已有 total_spent 列
    const cols = runMysql("SHOW COLUMNS FROM user_coin LIKE 'total_spent'");
    if (cols.includes('total_spent')) ok('user_coin.total_spent 列存在');
    else ko('user_coin.total_spent 列存在', '未找到');
  } catch(e) { ko('数据表检查', e.message); }

  // ---- 2. GET /api/treasury（首次调用自动建账户） ----
  console.log('\n[2] GET /api/treasury');
  let treasury = null;
  try {
    treasury = await api('GET', '/api/treasury');
    if (treasury.status === 200 && treasury.json) ok('返回 200');
    else ko('返回 200', treasury.text);
    const d = treasury.json;
    if (d && typeof d.balance === 'number') ok('balance 字段为数字', '= ' + d.balance);
    else ko('balance 字段为数字', JSON.stringify(d));
    if (d && typeof d.total_earned === 'number') ok('total_earned 字段为数字');
    else ko('total_earned 字段为数字', JSON.stringify(d));
    if (d && typeof d.total_spent === 'number') ok('total_spent 字段为数字');
    else ko('total_spent 字段为数字', JSON.stringify(d));
    if (d && Array.isArray(d.txns)) ok('txns 为数组');
    else ko('txns 为数组', JSON.stringify(d));
    if (d && Array.isArray(d.seals)) ok('seals 为数组');
    else ko('seals 为数组', JSON.stringify(d));
    if (d && typeof d.current_title === 'string') ok('current_title 为字符串', '= ' + d.current_title);
    else ko('current_title 为字符串', JSON.stringify(d));
    if (d && d.weekly_goal) ok('weekly_goal 字段存在');
    else ko('weekly_goal 字段存在', JSON.stringify(d));
  } catch(e) { ko('GET /api/treasury', e.message); }

  // ---- 3. 首次余额应为 0 ----
  console.log('\n[3] 初始余额验证');
  if (treasury && treasury.json) {
    const d = treasury.json;
    if (d.balance === 0) ok('初始 balance=0');
    else ko('初始 balance=0', 'got ' + d.balance);
    if (d.total_earned === 0) ok('初始 total_earned=0');
    else ko('初始 total_earned=0', 'got ' + d.total_earned);
    if (d.total_spent === 0) ok('初始 total_spent=0');
    else ko('初始 total_spent=0', 'got ' + d.total_spent);
  }

  // ---- 4. 入账测试：手动写入 txn + 刷新余额 ----
  console.log('\n[4] 入账逻辑测试');
  try {
    // 确保账户存在
    runMysql(
      "INSERT INTO user_coin (user_id, balance, total_earned, total_spent) "
      + "VALUES (1, 5, 5, 0) ON DUPLICATE KEY UPDATE balance=5,total_earned=5,total_spent=0");
    // 手动插入一条 txn，模拟 quiz_done 入账
    runMysql(
      "INSERT INTO user_coin_txn (user_id, delta, reason_code, ref_id, balance_after) "
      + "VALUES (1, 5, 'quiz_done', 99999, 5)");

    const t2 = await api('GET', '/api/treasury');
    if (t2.json && t2.json.balance === 5) ok('余额更新为 5（模拟入账）');
    else ko('余额更新为 5', `got ${t2.json ? t2.json.balance : 'null'}`);
    if (t2.json && t2.json.txns.length >= 1) ok('txns 包含手动入账记录');
    else ko('txns 包含手动入账记录', JSON.stringify(t2.json));
  } catch(e) { ko('入账逻辑测试', e.message); }

  // ---- 5. POST /api/treasury/spend（扣费） ----
  console.log('\n[5] POST /api/treasury/spend');
  let spendRes = null;
  try {
    spendRes = await api('POST', '/api/treasury/spend', { amount: 3, reason: 'test_spend', ref_id: 5001 });
    if (spendRes.status === 200 && spendRes.json && spendRes.json.ok) ok('扣费 3 成功');
    else ko('扣费 3 成功', JSON.stringify(spendRes.json));
    if (spendRes.json && spendRes.json.balance === 2) ok('余额扣减后为 2');
    else ko('余额扣减后为 2', `got ${spendRes.json ? spendRes.json.balance : 'null'}`);
  } catch(e) { ko('POST /api/treasury/spend', e.message); }

  // ---- 6. 扣费幂等：同一 reason_code + ref_id 重复扣 ----
  console.log('\n[6] 幂等性测试');
  try {
    const spendDup = await api('POST', '/api/treasury/spend', { amount: 3, reason: 'test_spend', ref_id: 5001 });
    if (spendDup.status === 200 && spendDup.json && spendDup.json.ok) ok('重复扣费不报错');
    else ko('重复扣费不报错', JSON.stringify(spendDup.json));
    // 幂等应不重复扣（同一 reason+ref+date）
    const t3 = await api('GET', '/api/treasury');
    if (t3.json && t3.json.balance === 2) ok('重复扣费不重复扣减（余额仍为 2）');
    else ko('重复扣费不重复扣减', `got ${t3.json ? t3.json.balance : 'null'}`);
  } catch(e) { ko('幂等性测试', e.message); }

  // ---- 7. 余额不足扣费 ----
  console.log('\n[7] 余额不足测试');
  try {
    // 先扣到 0
    runMysql("DELETE FROM user_coin_txn WHERE user_id=1 AND reason_code='test_spend'");
    runMysql("UPDATE user_coin SET balance=0, total_spent=0 WHERE user_id=1");
    const spendInsuf = await api('POST', '/api/treasury/spend', { amount: 5, reason: 'insuf_test', ref_id: 7001 });
    if (spendInsuf.status === 400 || (spendInsuf.json && !spendInsuf.json.ok)) ok('余额不足返回错误');
    else ko('余额不足返回错误', JSON.stringify(spendInsuf.json));
  } catch(e) { ko('余额不足测试', e.message); }

  // ---- 8. 印章列表：全部 7 枚，均为未获得 ----
  console.log('\n[8] 朱砂印列表');
  try {
    const t4 = await api('GET', '/api/treasury');
    if (t4.json && Array.isArray(t4.json.seals)) {
      const seals = t4.json.seals;
      if (seals.length === 7) ok('seals 共 7 枚');
      else ko('seals 共 7 枚', `got ${seals.length}`);
      const codes = seals.map(s => s.code);
      const expectedCodes = ['mastery_100','mastery_500','mastery_1000','goal_done','streak_3','streak_10','streak_50'];
      const allPresent = expectedCodes.every(c => codes.includes(c));
      if (allPresent) ok('所有 seal_code 均存在');
      else ko('所有 seal_code 均存在', JSON.stringify(codes));
      const allUnearned = seals.every(s => !s.earned);
      if (allUnearned) ok('初始全部未获得');
      else ko('初始全部未获得', JSON.stringify(seals.filter(s => s.earned)));
    } else ko('seals 为数组', JSON.stringify(t4.json));
  } catch(e) { ko('印章列表', e.message); }

  // ---- 9. 称谓：初始为「书童」或空 ----
  console.log('\n[9] 称谓等级');
  try {
    const t5 = await api('GET', '/api/treasury');
    if (t5.json) {
      const title = t5.json.current_title;
      if (typeof title === 'string' && (title === '' || title.startsWith('scholar_')))
        ok('current_title 格式正确', title || '（无称谓）');
      else ko('current_title 格式正确', JSON.stringify(title));
    }
  } catch(e) { ko('称谓等级', e.message); }

  // ---- 10. 本周课规：自动生成 ----
  console.log('\n[10] 本周课规');
  try {
    const t6 = await api('GET', '/api/treasury');
    if (t6.json && t6.json.weekly_goal) {
      const g = t6.json.weekly_goal;
      if (g.goal_type) ok('goal_type 已设置', g.goal_type);
      else ko('goal_type 已设置', JSON.stringify(g));
      if (typeof g.target === 'number' && g.target > 0) ok('target 为正整数', '=' + g.target);
      else ko('target 为正整数', JSON.stringify(g));
      if (typeof g.progress === 'number') ok('progress 字段存在', '=' + g.progress);
      else ko('progress 字段存在', JSON.stringify(g));
    } else ko('weekly_goal 字段', JSON.stringify(t6.json));
  } catch(e) { ko('本周课规', e.message); }

  // ---- 11. POST /api/treasury/weekly-goal：修改课规 ----
  console.log('\n[11] 修改本周课规');
  try {
    const goalRes = await api('POST', '/api/treasury/weekly-goal', {
      goal_type: 'review_items', target: 20
    });
    if (goalRes.json && goalRes.json.ok) ok('修改课规成功');
    else ko('修改课规成功', JSON.stringify(goalRes.json));
    const wg = goalRes.json && goalRes.json.weekly_goal;
    if (wg && wg.goal_type === 'review_items') ok('goal_type 已更新');
    else ko('goal_type 已更新', JSON.stringify(goalRes.json));
    if (wg && wg.target === 20) ok('target 已更新为 20');
    else ko('target 已更新为 20', JSON.stringify(goalRes.json));
  } catch(e) { ko('修改本周课规', e.message); }

  // ---- 12. 无效 goal_type ----
  console.log('\n[12] 课规非法类型');
  try {
    const badGoal = await api('POST', '/api/treasury/weekly-goal', {
      goal_type: 'invalid_type', target: 10
    });
    if (badGoal.status === 400 || (badGoal.json && !badGoal.json.ok)) ok('非法 goal_type 返回错误');
    else ko('非法 goal_type 返回错误', JSON.stringify(badGoal.json));
  } catch(e) { ko('课规非法类型', e.message); }

  // ---- 13. 统计接口 /api/treasury/stats ----
  console.log('\n[13] GET /api/treasury/stats');
  try {
    const stats = await api('GET', '/api/treasury/stats');
    if (stats.status === 200 && stats.json) ok('返回 200');
    else ko('返回 200', stats.text);
    const d = stats.json;
    if (typeof d.mastery_count === 'number') ok('mastery_count 为数字', '=' + d.mastery_count);
    else ko('mastery_count 为数字', JSON.stringify(d));
    if (typeof d.solidify_count === 'number') ok('solidify_count 为数字', '=' + d.solidify_count);
    else ko('solidify_count 为数字', JSON.stringify(d));
  } catch(e) { ko('GET /api/treasury/stats', e.message); }

  // ---- 14. 前端检查（T103 起：宝库改为抽屉浮层，由 nav.js 全站注入；独立页 treasury.html 已删除） ----
  console.log('\n[14] 前端宝库抽屉 + 课规设定');
  const navJsPath = path.join(ROOT, 'frontend/static/nav.js');
  if (fs.existsSync(navJsPath)) {
    const nav = fs.readFileSync(navJsPath, 'utf8');
    if (nav.includes('id="treasury-drawer"')) ok('nav.js 含宝库抽屉 #treasury-drawer');
    else ko('nav.js 含宝库抽屉 #treasury-drawer', '未找到');
    if (nav.includes('data-treasury-open')) ok('nav.js 含抽屉打开入口 data-treasury-open');
    else ko('nav.js 含抽屉打开入口 data-treasury-open', '未找到');
    if (nav.includes('/api/treasury')) ok('nav.js 调用 /api/treasury');
    else ko('nav.js 调用 /api/treasury', '未找到');
    if (nav.includes('id="td-txn-list"')) ok('nav.js 含账册容器 #td-txn-list');
    else ko('nav.js 含账册容器 #td-txn-list', '未找到');
    // 抽屉已裁剪：不再重复仪表盘已有的功名登阶 / 朱砂印 / 本周课规（无 seal-wall / 无 stairs / 无 weekly）
    if (!nav.includes('seal-wall') && !nav.includes('"stairs"') && !nav.includes('weekly')) ok('宝库抽屉已裁剪（无功名/朱砂印/课规）');
    else ko('宝库抽屉已裁剪（无功名/朱砂印/课规）', '仍含被裁剪板块');
    if (!nav.includes('treasury.html')) ok('nav.js 不再链接 treasury.html');
    else ko('nav.js 不再链接 treasury.html', '仍有残留链接');
  } else ko('nav.js 存在', '未找到');

  const treasuryHtmlPath = path.join(ROOT, 'frontend/pages/treasury.html');
  if (!fs.existsSync(treasuryHtmlPath)) ok('treasury.html 已移除（改为抽屉浮层）');
  else ko('treasury.html 已移除（改为抽屉浮层）', '文件仍存在');

  // 设定课规已迁至仪表盘页（index.html）
  const indexPath = path.join(ROOT, 'frontend/index.html');
  if (fs.existsSync(indexPath)) {
    const index = fs.readFileSync(indexPath, 'utf8');
    if (index.includes('id="goal-drawer"')) ok('index.html 含课规抽屉 #goal-drawer');
    else ko('index.html 含课规抽屉 #goal-drawer', '未找到');
    if (index.includes('data-goal-open')) ok('index.html 含课规设定入口 data-goal-open');
    else ko('index.html 含课规设定入口 data-goal-open', '未找到');
    if (index.includes('/api/treasury/weekly-goal')) ok('index.html 调用 /api/treasury/weekly-goal');
    else ko('index.html 调用 /api/treasury/weekly-goal', '未找到');
  } else ko('index.html 存在', '未找到');

  // ---- 15. 配置文件检查 ----
  console.log('\n[15] config/rewards.json');
  const rewardsPath = path.join(ROOT, 'config/rewards.json');
  if (fs.existsSync(rewardsPath)) ok('rewards.json 存在');
  else ko('rewards.json 存在', '未找到');
  if (fs.existsSync(rewardsPath)) {
    try {
      const cfg = JSON.parse(fs.readFileSync(rewardsPath, 'utf8'));
      if (cfg.income && cfg.income.quiz_done && cfg.income.quiz_done.amount === 5) ok('quiz_done 金额=5');
      else ko('quiz_done 金额=5', JSON.stringify(cfg.income));
      if (cfg.income && cfg.income.quiz_perfect && cfg.income.quiz_perfect.amount === 10) ok('quiz_perfect 金额=10');
      else ko('quiz_perfect 金额=10', JSON.stringify(cfg.income));
      if (cfg.income && cfg.income.review_done && cfg.income.review_done.amount === 5) ok('review_done 金额=5');
      else ko('review_done 金额=5', JSON.stringify(cfg.income));
      if (cfg.seals && cfg.seals.length === 7) ok('seals 共 7 枚');
      else ko('seals 共 7 枚', 'got ' + (cfg.seals ? cfg.seals.length : 0));
      if (cfg.titles && cfg.titles.length === 5) ok('titles 共 5 级');
      else ko('titles 共 5 级', 'got ' + (cfg.titles ? cfg.titles.length : 0));
    } catch(e) { ko('rewards.json 解析', e.message); }
  }

  // ---- 16. 汇总 ----
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
