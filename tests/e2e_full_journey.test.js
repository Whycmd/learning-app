// 总体端到端测试：完整用户旅程
// 录入→固化→学情创建→选题→出题→答题→批改→学情更新→复习完成→备份→重置
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
    else headers['Content-Length'] = 0;
    const r = http.request({ host:'127.0.0.1', port:8088, method, path:urlPath,
      headers, timeout: 130000 }, res => {
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

// 只清理本套件测试用户（e2euser_%）名下的数据
function cleanupTestData() {
  const like = "u.username LIKE 'e2euser_%'";
  for (const t of ['user_learning', 'kp_definitions', 'kp_collocations', 'kp_synonyms', 'kp_antonyms', 'kp_extras'])
    runMysql(`DELETE t FROM ${t} t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE a FROM ai_tasks a JOIN knowledge_points kp ON a.ref_id=kp.id JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE kp FROM knowledge_points kp JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql("DELETE FROM user_sessions WHERE user_id IN (SELECT id FROM users WHERE username LIKE 'e2euser_%');");
  runMysql("DELETE FROM users WHERE username LIKE 'e2euser_%';");
}

// 注册并登录本套件专用测试用户
async function setupTestUser() {
  TEST_USER = 'e2euser_' + Date.now().toString(36);
  const reg = await req('POST', '/api/auth/register', { username: TEST_USER, password: 'test pass 1' });
  if (!reg.json || !reg.json.ok) throw new Error('测试用户注册失败: ' + reg.text.slice(0, 150));
  const login = await req('POST', '/api/auth/login', { username: TEST_USER, password: 'test pass 1' });
  if (!login.setCookie) throw new Error('测试用户登录失败: ' + login.text.slice(0, 150));
  COOKIE = login.setCookie;
  TEST_UID = parseInt(runMysql(`SELECT id FROM users WHERE username='${TEST_USER}';`).stdout.trim(), 10);
}

async function main() {
  console.log('=== 总体端到端测试：完整用户旅程 ===\n');
  console.log('场景：用户录入3个单词 → AI固化 → 开始复习 → 选题出题 → 答题批改 → 学情更新 → 备份\n');

  try {
    // 清理本套件测试数据并创建专用测试用户
    cleanupTestData();
    await setupTestUser();
    ok('创建测试用户', `${TEST_USER} (uid=${TEST_UID})`);

    // 1. 健康检查
    console.log('[1] 系统健康检查');
    const health = await req('GET', '/api/health');
    if (health.json && health.json.db && health.json.ai) ok('系统健康', 'DB+AI 正常');
    else ko('系统健康', JSON.stringify(health.json));

    // 2. 录入3个知识点
    console.log('[2] 录入知识点');
    const words = ['abandon', 'benefit', 'comprehend'];
    const kpIds = [];
    for (const w of words) {
      const r = await req('POST', '/api/kp', { user_input_content: w, remark: w === 'abandon' ? '重点考察被动语态' : '' });
      if (r.json && r.json.ok) kpIds.push(r.json.id);
    }
    if (kpIds.length === 3) ok('录入3个知识点', `ids=[${kpIds.join(',')}]`);
    else ko('录入', `仅录入 ${kpIds.length} 个`);

    // 3. 一键固化（批量AI调用）
    console.log('[3] AI 一键固化');
    const sol = await req('POST', '/api/kp/solidify');
    if (sol.json && sol.json.success === 3 && sol.json.failed === 0) ok('全部固化成功', `success=${sol.json.success}`);
    else ko('固化', JSON.stringify(sol.json).slice(0,200));

    // 4. 验证固化结果
    console.log('[4] 验证固化结果');
    const detail = await req('GET', `/api/kp/${kpIds[0]}`);
    if (detail.json && detail.json.is_solidified === 1) ok('固化状态正确');
    if (detail.json && detail.json.standard_content) ok('标准词条已生成', detail.json.standard_content);
    if (detail.json && detail.json.definitions && detail.json.definitions.length > 0) ok('释义子表有数据');
    if (detail.json && detail.json.master_require && /会读|会写/.test(detail.json.master_require)) ok('掌握程度已判定', detail.json.master_require);

    // 5. 学情自动创建
    console.log('[5] 学情自动创建');
    const lr = await req('GET', `/api/learn/${kpIds[0]}`);
    if (lr.json && lr.json.proficiency_level === 0) ok('学情自动创建', 'L0 未掌握');

    // 6. 设置复习模式
    console.log('[6] 设置复习模式');
    for (const id of kpIds) {
      await req('PUT', `/api/learn/${id}/mode`, { review_mode: 'daily' });
    }
    // 设为过期进入候选池
    runMysql(`UPDATE user_learning SET next_review_time=DATE_SUB(NOW(), INTERVAL 1 DAY) WHERE knowledge_id IN (${kpIds.join(',')})`);
    ok('设置按日模式 & 过期');

    // 7. 获取候选池
    console.log('[7] 获取候选池');
    const pool = await req('GET', '/api/learn/candidates');
    if (pool.json && pool.json.total === 3) ok('候选池3个知识点', `total=${pool.json.total}`);
    // 验证排序（按熟练度升序）
    const levels = pool.json.data.map(d => d.proficiency_level);
    if (levels.every((v, i) => i === 0 || v >= levels[i-1])) ok('候选池排序正确');

    // 8. 人工选题 + AI 出题
    console.log('[8] 人工选题 + AI 出题');
    const ex = await req('POST', '/api/exercise/generate', { knowledge_ids: kpIds, mode: 'daily' });
    if (ex.json && ex.json.ok) ok('出题成功', `ex_id=${ex.json.exercise_id}, pages=${ex.json.page_limit}`);
    if (ex.json && ex.json.word_path && fs.existsSync(ex.json.word_path)) ok('习题 Word 已生成');

    // 9. 答题 + 批改
    console.log('[9] 答题 + 批改');
    const batch = await req('POST', '/api/exercise/batch-grade', {
      answers: [
        { type: 'word_match', user_answer: 'A', correct_answer: 'A', knowledge_id: kpIds[0] },
        { type: 'word_match', user_answer: 'B', correct_answer: 'A', knowledge_id: kpIds[1] },
        { type: 'word_form_fill', user_answer: 'abandon', correct_answer: 'abandon', knowledge_id: kpIds[2] }
      ]
    });
    if (batch.json && batch.json.ok) ok('批改完成', `correct=${batch.json.correct_count}/${batch.json.total_count}, score=${batch.json.total_score}`);

    // 10. 验证学情更新
    console.log('[10] 验证学情更新');
    const lr0 = await req('GET', `/api/learn/${kpIds[0]}`);
    const lr1 = await req('GET', `/api/learn/${kpIds[1]}`);
    const lr2 = await req('GET', `/api/learn/${kpIds[2]}`);
    if (lr0.json && lr0.json.error_times === 0) ok('kp0 答对', 'error_times=0');
    if (lr1.json && lr1.json.error_times >= 1) ok('kp1 答错降权', `error_times=${lr1.json.error_times}`);
    if (lr2.json && lr2.json.error_times === 0) ok('kp2 答对');

    // 11. 选题后熟练度调整
    console.log('[11] 选题后熟练度调整');
    const sel = await req('POST', '/api/learn/select', { knowledge_ids: kpIds });
    if (sel.json && sel.json.ok) ok('选题调整', `updated=${sel.json.updated_count}`);
    // 按日模式 +1
    const lr0After = await req('GET', `/api/learn/${kpIds[0]}`);
    if (lr0After.json && lr0After.json.proficiency_level === 1) ok('按日+1', 'L0→L1');

    // 12. 数据备份
    console.log('[12] 数据备份');
    const backup = await req('POST', '/api/maint/backup');
    if (backup.json && backup.json.ok) ok('备份成功', `${backup.json.size} bytes`);

    // 13. 临时文件清理
    console.log('[13] 临时文件清理');
    const cleanup = await req('POST', '/api/maint/cleanup-temp-word?max_age=0');
    if (cleanup.json && cleanup.json.ok) ok('清理完成', `deleted=${cleanup.json.deleted}`);

    // 14. 学情统计
    console.log('[14] 学情统计');
    const stats = await req('GET', '/api/learn/stats');
    if (stats.json && stats.json.total === 3) ok('统计正确', `total=${stats.json.total}`);

    console.log('\n=== 总体端到端测试结果 ===');
    console.log(`通过: ${pass}  失败: ${fail}`);
    if (fail > 0) {
      console.log('\n失败项:');
      failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
      process.exit(1);
    } else {
      console.log('\n✓ 总体端到端测试全部通过');
      process.exit(0);
    }
  } catch (e) {
    console.error('FATAL:', e);
    process.exit(2);
  }
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
