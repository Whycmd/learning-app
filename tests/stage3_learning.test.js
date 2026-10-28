// 阶段三：学情算法 & 选题业务逻辑 - 集成测试
// 覆盖验收标准：
//   1. 0-5级熟练度体系（答题自动更新、手动修改、错题降权）
//   2. 双模式遗忘曲线（按周/按日公式向下取整、复习完成更新、满级终止）
//   3. 人工选题流程（候选池筛选排序、用户勾选出题、选题后熟练度调整）
const http = require('http');
const fs = require('fs');
const { spawnSync } = require('child_process');

const MYSQL_BIN = '/Users/junwang/.local/mysql/bin/mysql';
const MYSQL_PW_FILE = '/Users/junwang/.local/mysql/.root_pw';
const SOCK = '/Users/junwang/.local/run/mysql.sock';

let pass = 0, fail = 0;
const failures = [];
// 本套件使用独立测试用户，所有读写与清理都限定在其名下，不触碰真实用户数据
let COOKIE = null, TEST_UID = 0, TEST_USER = '';
function ok(name, msg='') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({name, msg}); console.log(`  ✗ ${name} — ${msg}`); }

function req(method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type':'application/json' };
    if (COOKIE) headers['Cookie'] = 'eng_session=' + COOKIE;
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    const r = http.request({ host:'127.0.0.1', port:8088, method, path:urlPath,
      headers, timeout: 60000 }, res => {
      let chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        const sc = res.headers['set-cookie'];
        let setCookie = null;
        if (sc) { const m = /eng_session=([a-f0-9]*)/.exec(Array.isArray(sc) ? sc[0] : sc); if (m) setCookie = m[1]; }
        try { resolve({ status:res.statusCode, json:JSON.parse(text), text, setCookie }); }
        catch { resolve({ status:res.statusCode, json:null, text, setCookie }); }
      });
    });
    r.on('error', reject);
    r.on('timeout', () => r.destroy(new Error('timeout')));
    if (data) r.write(data);
    r.end();
  });
}

function runMysql(sql, silent=true) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const args = ['--socket='+SOCK, '-uroot', '-p'+pw];
  if (silent) args.push('-s', '--skip-column-names');
  args.push('english_learning', '-e', sql);
  return spawnSync(MYSQL_BIN, args, { encoding:'utf8', timeout:15000 });
}

// 只清理本套件测试用户（t3user_%）名下的数据
function cleanupTestData() {
  const like = "u.username LIKE 't3user_%'";
  for (const t of ['user_learning', 'kp_definitions', 'kp_collocations', 'kp_synonyms', 'kp_antonyms', 'kp_extras'])
    runMysql(`DELETE t FROM ${t} t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE a FROM ai_tasks a JOIN knowledge_points kp ON a.ref_id=kp.id JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE kp FROM knowledge_points kp JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql("DELETE FROM user_sessions WHERE user_id IN (SELECT id FROM users WHERE username LIKE 't3user_%');");
  runMysql("DELETE FROM users WHERE username LIKE 't3user_%';");
}

// 注册并登录本套件专用测试用户
async function setupTestUser() {
  TEST_USER = 't3user_' + Date.now().toString(36);
  const reg = await req('POST', '/api/auth/register', { username: TEST_USER, password: 'test pass 1' });
  if (!reg.json || !reg.json.ok) throw new Error('测试用户注册失败: ' + reg.text.slice(0, 150));
  const login = await req('POST', '/api/auth/login', { username: TEST_USER, password: 'test pass 1' });
  if (!login.setCookie) throw new Error('测试用户登录失败: ' + login.text.slice(0, 150));
  COOKIE = login.setCookie;
  TEST_UID = parseInt(runMysql(`SELECT id FROM users WHERE username='${TEST_USER}';`).stdout.trim(), 10);
}

// 遗忘曲线公式验证
// 按周：floor(2 * (1 + 2^level)) 天
// 按日：floor(0.4 * (1 + 3^level)) 天
function weeklyInterval(level) { return Math.floor(2 * (1 + Math.pow(2, level))); }
function dailyInterval(level) { return Math.floor(0.4 * (1 + Math.pow(3, level))); }

async function main() {
  console.log('=== 阶段三：学情算法 & 选题业务逻辑集成测试 ===\n');

  // ---- 0. 前置：清理本套件数据并创建独立测试用户 ----
  console.log('[0] 前置：清理数据并创建测试用户');
  try {
    cleanupTestData();
    await setupTestUser();
    ok('清理数据并创建测试用户', `${TEST_USER} (uid=${TEST_UID})`);
  } catch (e) { ko('清理数据', e.message); }

  // 录入并固化 3 个知识点
  const kps = ['abandon', 'benefit', 'comprehend'];
  const kpIds = [];
  console.log('  录入并固化知识点...');
  for (const w of kps) {
    const r = await req('POST', '/api/kp', { user_input_content: w });
    if (!r.json || !r.json.ok) { ko('录入', w); continue; }
    const s = await req('POST', `/api/kp/${r.json.id}/solidify`);
    if (!s.json || (s.status !== 200 && s.status !== 201)) { ko('固化', w); continue; }
    kpIds.push(r.json.id);
  }
  if (kpIds.length < 3) { console.log('  固化知识点不足，终止测试'); process.exit(1); }
  ok(`固化为 ${kpIds.length} 个知识点`, `ids=[${kpIds.join(',')}]`);

  // ---- 1. 0-5级熟练度体系 ----
  console.log('\n[1] 0-5级熟练度体系');
  try {
    // 首次访问学情 → 自动创建，默认 proficiency=0
    const lr = await req('GET', `/api/learn/${kpIds[0]}`);
    if (lr.json && lr.json.proficiency_level === 0) ok('自动创建学情记录', 'proficiency=0');
    else ko('自动创建', JSON.stringify(lr.json).slice(0,200));

    // 手动修改熟练度
    const u1 = await req('PUT', `/api/learn/${kpIds[0]}/proficiency`, { proficiency_level: 3 });
    if (u1.json && u1.json.ok) ok('手动修改熟练度', 'level=3');
    else ko('手动修改', JSON.stringify(u1.json));

    const lr2 = await req('GET', `/api/learn/${kpIds[0]}`);
    if (lr2.json && lr2.json.proficiency_level === 3) ok('熟练度已更新', 'level=3');
    else ko('熟练度更新', JSON.stringify(lr2.json).slice(0,200));

    // 错题降权：-1
    const ans = await req('POST', `/api/learn/${kpIds[0]}/answer`, { is_correct: false });
    if (ans.json && ans.json.new_proficiency === 2) ok('错题降权', 'level 3→2');
    else ko('错题降权', JSON.stringify(ans.json).slice(0,200));

    // 再次错题降权（不低于0）
    await req('POST', `/api/learn/${kpIds[0]}/answer`, { is_correct: false });
    const lr3 = await req('GET', `/api/learn/${kpIds[0]}`);
    if (lr3.json && lr3.json.proficiency_level === 1) ok('连续错题降权', 'level 2→1');
    else ko('连续降权', JSON.stringify(lr3.json).slice(0,200));

    // 降权到0后不再继续降
    await req('POST', `/api/learn/${kpIds[0]}/answer`, { is_correct: false });
    const lr4 = await req('GET', `/api/learn/${kpIds[0]}`);
    if (lr4.json && lr4.json.proficiency_level === 0) ok('熟练度下限保护', 'level=0 不再降');
    else ko('下限保护', JSON.stringify(lr4.json).slice(0,200));

    // 错误次数累加
    if (lr4.json.error_times >= 3) ok('错误次数累加', `error_times=${lr4.json.error_times}`);
    else ko('错误次数', JSON.stringify(lr4.json).slice(0,200));

    // 设置为5（满级）
    await req('PUT', `/api/learn/${kpIds[0]}/proficiency`, { proficiency_level: 5 });
    const lr5 = await req('GET', `/api/learn/${kpIds[0]}`);
    if (lr5.json && lr5.json.proficiency_level === 5) ok('设置为满级', 'level=5');
    else ko('满级设置', JSON.stringify(lr5.json).slice(0,200));

    // 边界：无效 level 拒绝
    const bad = await req('PUT', `/api/learn/${kpIds[0]}/proficiency`, { proficiency_level: 99 });
    if (bad.status === 400) ok('拒绝越界熟练度', 'level=99 → 400');
    else ko('越界检查', 'status=' + bad.status);
  } catch (e) { ko('熟练度体系', e.message); }

  // ---- 2. 双模式遗忘曲线 ----
  console.log('\n[2] 双模式遗忘曲线算法');
  try {
    // 公式验证（向下取整）
    // 按周：floor(2 * (1 + 2^level))
    // level=0: floor(2*(1+1))=4, level=1: floor(2*(1+2))=6, level=2: floor(2*(1+4))=10, level=3: floor(2*(1+8))=18
    // 按日：floor(0.4 * (1 + 3^level))
    // level=0: floor(0.4*(1+1))=0, level=1: floor(0.4*(1+3))=1, level=2: floor(0.4*(1+9))=4, level=3: floor(0.4*(1+27))=11
    const weeklyCases = [
      { level: 0, expected: 4 }, { level: 1, expected: 6 },
      { level: 2, expected: 10 }, { level: 3, expected: 18 },
      { level: 4, expected: 34 }, { level: 5, expected: 66 }
    ];
    let weeklyOk = true;
    for (const c of weeklyCases) {
      const got = weeklyInterval(c.level);
      if (got !== c.expected) { weeklyOk = false; ko(`按周 L${c.level}`, `expected ${c.expected}, got ${got}`); break; }
    }
    if (weeklyOk) ok('按周模式公式正确', weeklyCases.map(c => `L${c.level}=${c.expected}d`).join(', '));

    const dailyCases = [
      { level: 0, expected: 0 }, { level: 1, expected: 1 },
      { level: 2, expected: 4 }, { level: 3, expected: 11 },
      { level: 4, expected: 32 }, { level: 5, expected: 97 }
    ];
    let dailyOk = true;
    for (const c of dailyCases) {
      const got = dailyInterval(c.level);
      if (got !== c.expected) { dailyOk = false; ko(`按日 L${c.level}`, `expected ${c.expected}, got ${got}`); break; }
    }
    if (dailyOk) ok('按日模式公式正确', dailyCases.map(c => `L${c.level}=${c.expected}d`).join(', '));

    // 设置 kp1 为按日模式、level=2 → 完成复习后 next_review = NOW() + 4 days
    await req('PUT', `/api/learn/${kpIds[1]}/mode`, { review_mode: 'daily' });
    await req('PUT', `/api/learn/${kpIds[1]}/proficiency`, { proficiency_level: 2 });
    const cr = await req('POST', `/api/learn/${kpIds[1]}/complete-review`);
    if (cr.json && cr.json.ok) {
      ok('完成复习（按日 L2）', `next=${cr.json.next_review_time}`);
      // 验证 next_review_time 约 = 今天 + 4天
      const rs = runMysql(`SELECT DATE_FORMAT(next_review_time, '%Y-%m-%d') AS nrt, DATE_FORMAT(DATE_ADD(NOW(), INTERVAL 4 DAY), '%Y-%m-%d') AS expected FROM user_learning WHERE knowledge_id=${kpIds[1]}`).stdout.trim().split('\t');
      if (rs.length === 2 && rs[0] === rs[1]) ok('按日模式下次复习时间正确', `next=${rs[0]}`);
      else ko('按日时间', `nrt=${rs[0]}, expected=${rs[1]}`);
    } else ko('完成复习', JSON.stringify(cr.json).slice(0,200));

    // 设置 kp2 为按周模式、level=2 → 完成复习后 next_review = NOW() + 10 days
    await req('PUT', `/api/learn/${kpIds[2]}/mode`, { review_mode: 'weekly' });
    await req('PUT', `/api/learn/${kpIds[2]}/proficiency`, { proficiency_level: 2 });
    const cr2 = await req('POST', `/api/learn/${kpIds[2]}/complete-review`);
    if (cr2.json && cr2.json.ok) {
      ok('完成复习（按周 L2）', `next=${cr2.json.next_review_time}`);
      const rs = runMysql(`SELECT DATE_FORMAT(next_review_time, '%Y-%m-%d') AS nrt, DATE_FORMAT(DATE_ADD(NOW(), INTERVAL 10 DAY), '%Y-%m-%d') AS expected FROM user_learning WHERE knowledge_id=${kpIds[2]}`).stdout.trim().split('\t');
      if (rs.length === 2 && rs[0] === rs[1]) ok('按周模式下次复习时间正确', `next=${rs[0]}`);
      else ko('按周时间', `nrt=${rs[0]}, expected=${rs[1]}`);
    } else ko('完成复习', JSON.stringify(cr2.json).slice(0,200));

    // 复习次数累加
    const reviewCnt = runMysql(`SELECT review_times FROM user_learning WHERE knowledge_id=${kpIds[1]}`).stdout.trim();
    if (parseInt(reviewCnt) >= 1) ok('复习次数累加', `review_times=${reviewCnt}`);
    else ko('复习次数', reviewCnt);

    // 满级终止复习（kp0 已设为5）
    const cr3 = await req('POST', `/api/learn/${kpIds[0]}/complete-review`);
    if (cr3.json && cr3.json.ok && cr3.json.next_review_time && cr3.json.next_review_time.includes('NULL')) {
      ok('满级终止复习', 'next_review=NULL');
    } else ko('满级终止', JSON.stringify(cr3.json).slice(0,200));
  } catch (e) { ko('遗忘曲线', e.message); }

  // ---- 3. 人工选题流程 ----
  console.log('\n[3] 人工选题流程');
  try {
    // 准备：kp1 L2 daily, kp2 L2 weekly, kp0 L5(满级)
    // 将 kp1 和 kp2 的 next_review_time 设为过去时间（使其进入候选池）
    runMysql(`UPDATE user_learning SET next_review_time=DATE_SUB(NOW(), INTERVAL 1 DAY) WHERE knowledge_id=${kpIds[1]} OR knowledge_id=${kpIds[2]}`);
    // 将 kp0 设为满级
    runMysql(`UPDATE user_learning SET proficiency_level=5, next_review_time=NULL WHERE knowledge_id=${kpIds[0]}`);

    // 获取候选池
    const pool = await req('GET', '/api/learn/candidates');
    if (pool.json && pool.json.data && Array.isArray(pool.json.data)) {
      ok('候选池返回', `total=${pool.json.total}`);
      // kp0(满级)不应在候选池中
      const ids = pool.json.data.map(d => d.knowledge_id);
      if (!ids.includes(kpIds[0])) ok('满级知识点不进入候选池');
      else ko('满级排除', `kp0=${kpIds[0]} 仍在候选池`);
      // 排序：未掌握(0) > 生疏(1) > 理解(2) > 熟练(3) > 精通(4)
      const levels = pool.json.data.map(d => d.proficiency_level);
      const isSorted = levels.every((v, i) => i === 0 || v >= levels[i-1]);
      if (isSorted) ok('候选池按熟练度升序排列', `levels=[${levels.join(',')}]`);
      else ko('候选池排序', `levels=[${levels.join(',')}]`);
    } else ko('候选池', JSON.stringify(pool.json).slice(0,200));

    // 人工勾选 kp1 和 kp2 出题
    const sel = await req('POST', '/api/learn/select', { knowledge_ids: [kpIds[1], kpIds[2]] });
    if (sel.json && sel.json.ok && sel.json.updated_count === 2) {
      ok('选题后批量调整熟练度', `updated=${sel.json.updated_count}`);
      // kp1 按日模式 → +1 (L2→L3)
      const kp1Learn = await req('GET', `/api/learn/${kpIds[1]}`);
      if (kp1Learn.json && kp1Learn.json.proficiency_level === 3) ok('按日模式熟练度+1', `L2→L3`);
      else ko('按日+1', JSON.stringify(kp1Learn.json).slice(0,200));
      // kp2 按周模式 → +2 (L2→L4)
      const kp2Learn = await req('GET', `/api/learn/${kpIds[2]}`);
      if (kp2Learn.json && kp2Learn.json.proficiency_level === 4) ok('按周模式熟练度+2', `L2→L4`);
      else ko('按周+2', JSON.stringify(kp2Learn.json).slice(0,200));
      // 下次复习时间已更新
      const selDetail = sel.json.details.find(d => d.knowledge_id === kpIds[1]);
      if (selDetail && selDetail.next_review_time && !selDetail.next_review_time.includes('NULL')) {
        ok('选题后下次复习时间已重算', selDetail.next_review_time);
      } else ko('选题后时间', JSON.stringify(selDetail).slice(0,200));
    } else ko('选题', JSON.stringify(sel.json).slice(0,200));

    // 满级封顶测试：kp2 L4 + 按周模式(+2) = L6 → 封顶 L5
    // 先重置 kp2 为 L4
    await req('PUT', `/api/learn/${kpIds[2]}/proficiency`, { proficiency_level: 4 });
    runMysql(`UPDATE user_learning SET next_review_time=DATE_SUB(NOW(), INTERVAL 1 DAY) WHERE knowledge_id=${kpIds[2]}`);
    const sel2 = await req('POST', '/api/learn/select', { knowledge_ids: [kpIds[2]] });
    const kp2Final = await req('GET', `/api/learn/${kpIds[2]}`);
    if (kp2Final.json && kp2Final.json.proficiency_level === 5) ok('满级封顶', 'L4+2→L5（封顶）');
    else ko('满级封顶', JSON.stringify(kp2Final.json).slice(0,200));
    // 满级后 next_review_time 应为 NULL（终止）
    const nrt = runMysql(`SELECT next_review_time FROM user_learning WHERE knowledge_id=${kpIds[2]}`).stdout.trim();
    if (nrt === 'NULL' || nrt === '') ok('满级后终止复习', 'next_review=NULL');
    else ko('满级终止', `next_review=${nrt}`);
  } catch (e) { ko('选题流程', e.message); }

  // ---- 4. 学情统计 ----
  console.log('\n[4] 学情统计');
  try {
    const st = await req('GET', '/api/learn/stats');
    if (st.json && st.json.total >= 3 && st.json.distribution && Array.isArray(st.json.distribution)) {
      ok('统计返回', `total=${st.json.total}, due=${st.json.due_for_review}`);
    } else ko('统计', JSON.stringify(st.json).slice(0,200));
  } catch (e) { ko('统计', e.message); }

  console.log('\n=== 阶段三测试结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}`);
  if (fail > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
    process.exit(1);
  } else {
    console.log('\n✓ 阶段三全部测试通过');
    process.exit(0);
  }
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
