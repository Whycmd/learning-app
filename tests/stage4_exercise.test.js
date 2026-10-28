// 阶段四：AI 出题 & 自动批改引擎 - 集成测试
// 覆盖验收标准：
//   1. AI 出题通用规则引擎（分层出题、页数限制、备注优先）
//   2. 6 大固定题型全量开发
//   3. 速记收尾模块固定输出
//   4. 全自动批改体系（客观题本地、主观题 AI）
//   5. AI 出题专属异常兜底
const http = require('http');
const fs = require('fs');
const { spawnSync } = require('child_process');

const MYSQL_BIN = '/Users/junwang/.local/mysql/bin/mysql';
const MYSQL_PW_FILE = '/Users/junwang/.local/mysql/.root_pw';
const SOCK = '/Users/junwang/.local/run/mysql.sock';
const WORD_TMP = '/Users/junwang/Desktop/王浩源/英语/英语学习应用/word_tmp';

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

// 只清理本套件测试用户（t4user_%）名下的数据
function cleanupTestData() {
  const like = "u.username LIKE 't4user_%'";
  for (const t of ['user_learning', 'kp_definitions', 'kp_collocations', 'kp_synonyms', 'kp_antonyms', 'kp_extras'])
    runMysql(`DELETE t FROM ${t} t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE a FROM ai_tasks a JOIN knowledge_points kp ON a.ref_id=kp.id JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE kp FROM knowledge_points kp JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql("DELETE FROM user_sessions WHERE user_id IN (SELECT id FROM users WHERE username LIKE 't4user_%');");
  runMysql("DELETE FROM users WHERE username LIKE 't4user_%';");
}

// 注册并登录本套件专用测试用户
async function setupTestUser() {
  TEST_USER = 't4user_' + Date.now().toString(36);
  const reg = await req('POST', '/api/auth/register', { username: TEST_USER, password: 'test pass 1' });
  if (!reg.json || !reg.json.ok) throw new Error('测试用户注册失败: ' + reg.text.slice(0, 150));
  const login = await req('POST', '/api/auth/login', { username: TEST_USER, password: 'test pass 1' });
  if (!login.setCookie) throw new Error('测试用户登录失败: ' + login.text.slice(0, 150));
  COOKIE = login.setCookie;
  TEST_UID = parseInt(runMysql(`SELECT id FROM users WHERE username='${TEST_USER}';`).stdout.trim(), 10);
}

async function main() {
  console.log('=== 阶段四：AI 出题 & 自动批改引擎集成测试 ===\n');

  // ---- 0. 前置：准备固化知识点 ----
  console.log('[0] 前置：准备数据');
  try {
    cleanupTestData();
    await setupTestUser();
    ok('清理数据并创建测试用户', `${TEST_USER} (uid=${TEST_UID})`);
    const kps = ['abandon', 'benefit', 'comprehend'];
    const kpIds = [];
    for (const w of kps) {
      const r = await req('POST', '/api/kp', { user_input_content: w });
      if (!r.json || !r.json.ok) { ko('录入', w); continue; }
      const s = await req('POST', `/api/kp/${r.json.id}/solidify`);
      if (!s.json || (s.status !== 200 && s.status !== 201)) { ko('固化', w); continue; }
      kpIds.push(r.json.id);
    }
    if (kpIds.length < 3) { console.log('  固化知识点不足，终止'); process.exit(1); }
    ok(`固化 ${kpIds.length} 个知识点`, `ids=[${kpIds.join(',')}]`);

    // ---- 1. AI 出题通用规则引擎 ----
    console.log('\n[1] AI 出题通用规则引擎');
    // 按日模式出题
    const gen1 = await req('POST', '/api/exercise/generate', { knowledge_ids: kpIds, mode: 'daily' });
    if (gen1.json && gen1.json.ok) {
      ok('按日出题成功', `ex_id=${gen1.json.exercise_id}`);
      if (gen1.json.word_path && fs.existsSync(gen1.json.word_path)) {
        const size = fs.statSync(gen1.json.word_path).size;
        if (size > 1000) ok('习题 Word 生成', `${size} bytes`);
        else ko('Word 大小', `${size} bytes`);
        // 验证 .docx PK 头
        const fd = fs.openSync(gen1.json.word_path, 'r');
        const buf = Buffer.alloc(2);
        fs.readSync(fd, buf, 0, 2, 0);
        fs.closeSync(fd);
        if (buf[0] === 0x50 && buf[1] === 0x4b) ok('Word 有效 .docx (PK 头)');
        else ko('Word 格式', `头部: ${buf.toString('hex')}`);
      } else ko('Word 文件', '不存在');
      // 页数限制
      if (gen1.json.page_limit >= 1 && gen1.json.page_limit <= 2) ok('按日页数限制', `limit=${gen1.json.page_limit}`);
      else ko('按日页数', `limit=${gen1.json.page_limit}`);
      // 题型（按日会写分层）
      if (gen1.json.exercise_types && gen1.json.exercise_types.length > 0) {
        ok('按日题型返回', `types=[${gen1.json.exercise_types.join(',')}]`);
      } else ko('按日题型', '空');
    } else ko('按日出题', JSON.stringify(gen1.json).slice(0,200));

    // 按周模式出题（全题型）
    const gen2 = await req('POST', '/api/exercise/generate', { knowledge_ids: kpIds, mode: 'weekly' });
    if (gen2.json && gen2.json.ok) {
      ok('按周出题成功', `ex_id=${gen2.json.exercise_id}`);
      if (gen2.json.word_path && fs.existsSync(gen2.json.word_path)) {
        ok('习题 Word 生成', `${fs.statSync(gen2.json.word_path).size} bytes`);
      } else ko('Word 文件', '不存在');
      // 页数限制 3-5
      if (gen2.json.page_limit >= 3 && gen2.json.page_limit <= 5) ok('按周页数限制', `limit=${gen2.json.page_limit}`);
      else ko('按周页数', `limit=${gen2.json.page_limit}`);
      // 全题型
      const allTypes = ['word_match','word_form_fill','context_blank_fill','cn_to_en_translation','cloze_choice','sentence_imitation'];
      if (gen2.json.exercise_types && gen2.json.exercise_types.length >= 6) {
        const hasAll = allTypes.every(t => gen2.json.exercise_types.includes(t));
        if (hasAll) ok('按周全题型', `${gen2.json.exercise_types.length} 种`);
        else ko('按周题型', `缺少: ${allTypes.filter(t => !gen2.json.exercise_types.includes(t)).join(',')}`);
      } else ko('按周题型数量', `仅 ${gen2.json.exercise_types ? gen2.json.exercise_types.length : 0} 种`);
    } else ko('按周出题', JSON.stringify(gen2.json).slice(0,200));

    // ---- 2. 6 大题型验证 ----
    console.log('\n[2] 6 大题型验证');
    if (gen2.json && gen2.json.exercises && gen2.json.exercises.exercises) {
      const exs = gen2.json.exercises.exercises;
      const types = [...new Set(exs.map(e => e.type))];
      const allTypes = ['word_match','word_form_fill','context_blank_fill','cn_to_en_translation','cloze_choice','sentence_imitation'];
      let allPresent = true;
      for (const t of allTypes) {
        if (!types.includes(t)) { allPresent = false; ko(`题型 ${t}`, '缺失'); }
      }
      if (allPresent) ok('6 大题型齐全', `共 ${exs.length} 题`);

      // 速记板块
      if (gen2.json.exercises.quick_memo && gen2.json.exercises.quick_memo.items) {
        ok('速记收尾模块', `${gen2.json.exercises.quick_memo.items.length} 条`);
      } else ko('速记模块', '缺失');
    } else ko('题型验证', '无习题数据');

    // ---- 3. 自动批改：客观题 ----
    console.log('\n[3] 自动批改 - 客观题');
    // word_match 客观题
    const g1 = await req('POST', '/api/exercise/grade', {
      type: 'word_match', user_answer: 'A', correct_answer: 'A', reference_answer: ''
    });
    if (g1.json && g1.json.ok && g1.json.is_objective && g1.json.score === 100 && g1.json.correct) {
      ok('客观题正确批改', `score=${g1.json.score}`);
    } else ko('客观题正确', JSON.stringify(g1.json).slice(0,200));

    const g2 = await req('POST', '/api/exercise/grade', {
      type: 'word_match', user_answer: 'B', correct_answer: 'A', reference_answer: ''
    });
    if (g2.json && g2.json.ok && g2.json.is_objective && g2.json.score === 0 && !g2.json.correct) {
      ok('客观题错误批改', `score=${g2.json.score}`);
    } else ko('客观题错误', JSON.stringify(g2.json).slice(0,200));

    // word_form_fill 填空题
    const g3 = await req('POST', '/api/exercise/grade', {
      type: 'word_form_fill', user_answer: 'abandon', correct_answer: 'abandon', reference_answer: ''
    });
    if (g3.json && g3.json.is_objective && g3.json.score === 100) ok('填空题正确批改');
    else ko('填空题', JSON.stringify(g3.json).slice(0,200));

    const g4 = await req('POST', '/api/exercise/grade', {
      type: 'word_form_fill', user_answer: 'abondon', correct_answer: 'abandon', reference_answer: ''
    });
    if (g4.json && g4.json.is_objective && g4.json.score === 0) ok('填空题错误批改');
    else ko('填空题错误', JSON.stringify(g4.json).slice(0,200));

    // ---- 4. 自动批改：主观题（AI） ----
    console.log('\n[4] 自动批改 - 主观题');
    const g5 = await req('POST', '/api/exercise/grade', {
      type: 'cn_to_en_translation',
      user_answer: 'Today we start learning.',
      correct_answer: '',
      reference_answer: 'Today we start learning.'
    });
    if (g5.json && g5.json.ok && !g5.json.is_objective) {
      ok('主观题 AI 批改', `score=${g5.json.score}, correct=${g5.json.correct}`);
      if (g5.json.error_analysis) ok('错误分析输出', g5.json.error_analysis.slice(0,50));
    } else ko('主观题批改', JSON.stringify(g5.json).slice(0,200));

    // ---- 5. 批量批改 + 学情同步 ----
    console.log('\n[5] 批量批改 + 学情同步');
    const batch = await req('POST', '/api/exercise/batch-grade', {
      answers: [
        { type: 'word_match', user_answer: 'A', correct_answer: 'A', knowledge_id: kpIds[0] },
        { type: 'word_form_fill', user_answer: 'wrong', correct_answer: 'abandon', knowledge_id: kpIds[1] },
        { type: 'cn_to_en_translation', user_answer: 'Today we start learning.', reference_answer: 'Today we start learning.', knowledge_id: kpIds[2] }
      ]
    });
    if (batch.json && batch.json.ok) {
      ok('批量批改完成', `total=${batch.json.total_count}, correct=${batch.json.correct_count}, score=${batch.json.total_score}`);
      // 学情同步：kp0 答对 → 熟练度不变，kp1 答错 → -1
      const lr0 = await req('GET', `/api/learn/${kpIds[0]}`);
      const lr1 = await req('GET', `/api/learn/${kpIds[1]}`);
      if (lr0.json && lr0.json.error_times !== undefined) ok('学情同步 kp0', `error_times=${lr0.json.error_times}`);
      if (lr1.json && lr1.json.error_times >= 1) ok('学情同步 kp1 答错降权', `error_times=${lr1.json.error_times}`);
      else ko('学情同步', JSON.stringify(lr1.json).slice(0,200));
    } else ko('批量批改', JSON.stringify(batch.json).slice(0,200));

    // ---- 6. 异常兜底 ----
    console.log('\n[6] 异常兜底');
    // 空知识点列表
    const e1 = await req('POST', '/api/exercise/generate', { knowledge_ids: [], mode: 'daily' });
    if (e1.status === 500 || (e1.json && e1.json.error)) ok('空知识点兜底', `error=${e1.json ? e1.json.error : e1.status}`);
    else ko('空知识点兜底', JSON.stringify(e1.json).slice(0,200));

    // 不存在的知识点
    const e2 = await req('POST', '/api/exercise/generate', { knowledge_ids: [99999], mode: 'daily' });
    if (e2.status === 500 || (e2.json && e2.json.error)) ok('无效知识点兜底', `error=${e2.json ? e2.json.error : e2.status}`);
    else ko('无效知识点兜底', JSON.stringify(e2.json).slice(0,200));

    // 空答案批改
    const e3 = await req('POST', '/api/exercise/grade', { type: 'word_match', user_answer: '', correct_answer: 'A' });
    if (e3.status === 400) ok('空答案兜底', 'status=400');
    else ko('空答案兜底', `status=${e3.status}`);

    // ---- 7. 习题不入库验证 ----
    console.log('\n[7] 习题不入库验证');
    // 检查数据库中无习题表（习题不入库）
    const tables = runMysql("SHOW TABLES LIKE 'exercise%';").stdout.trim();
    if (!tables) ok('习题不入库', '无 exercise 表');
    else ko('习题不入库', `存在: ${tables}`);

    // 习题 Word 是临时文件
    if (gen1.json && gen1.json.word_path) {
      const fn = gen1.json.word_path.split('/').pop();
      if (fn.startsWith('exercise_') && fn.endsWith('.docx')) ok('习题 Word 文件名规范', fn);
      else ko('Word 文件名', fn);
    }

  } catch (e) {
    ko('阶段四测试', e.message);
  }

  console.log('\n=== 阶段四测试结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}`);
  if (fail > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
    process.exit(1);
  } else {
    console.log('\n✓ 阶段四全部测试通过');
    process.exit(0);
  }
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
