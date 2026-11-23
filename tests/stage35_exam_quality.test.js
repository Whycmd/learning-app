// v1.5.3-T004 EXAM-QUALITY：练习出题质量整改回归
// 覆盖：长篇阅读语法填空批改、Word 导出、提示词硬性规则、前端与导出防泄题静态检查
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT } = require('./_env');

let pass = 0, fail = 0;
const failures = [];
let COOKIE = null, TEST_USER = '';

function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }

function req(method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json' };
    if (COOKIE) headers['Cookie'] = 'eng_session=' + COOKIE;
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    const r = http.request({ host: API_HOST, port: API_PORT, method, path: urlPath, headers, timeout: 30000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        const sc = res.headers['set-cookie'];
        let setCookie = null;
        if (sc) { const m = /eng_session=([a-f0-9]*)/.exec(Array.isArray(sc) ? sc[0] : sc); if (m) setCookie = m[1]; }
        try { resolve({ status: res.statusCode, json: JSON.parse(text), text, setCookie }); }
        catch { resolve({ status: res.statusCode, json: null, text, setCookie }); }
      });
    });
    r.on('error', reject);
    r.on('timeout', () => r.destroy(new Error('timeout')));
    if (data) r.write(data);
    r.end();
  });
}

function mysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  return spawnSync(MYSQL_BIN, ['--socket=' + SOCK, '-uroot', '-p' + pw, '-N', DB_NAME, '-e', sql], {
    encoding: 'utf8', timeout: 15000
  });
}

function cleanup() {
  const like = "username LIKE 't35user_%'";
  mysql(`DELETE s FROM user_sessions s JOIN users u ON s.user_id=u.id WHERE u.${like};`);
  mysql(`DELETE u FROM users u WHERE u.${like};`);
}

async function setupUser() {
  TEST_USER = 't35user_' + Date.now().toString(36);
  const reg = await req('POST', '/api/auth/register', { username: TEST_USER, password: 'test pass 1' });
  if (!reg.json || !reg.json.ok) throw new Error('注册失败：' + reg.text.slice(0, 160));
  const login = await req('POST', '/api/auth/login', { username: TEST_USER, password: 'test pass 1' });
  if (!login.json || !login.json.ok || !login.setCookie) throw new Error('登录失败：' + login.text.slice(0, 160));
  COOKIE = login.setCookie;
}

const LONG_ITEM = {
  type: 'long_reading_cloze',
  word: '',
  stem: '根据短文内容，在空白处填入适当的词或所给词的正确形式。',
  passage: 'The old library stood quietly [[1]] the corner. Every morning, readers [[2]] gather before the doors opened.',
  blanks: [
    { no: 1, answer: 'on', explanation: '固定搭配 on the corner' },
    { no: 2, answer: 'would', explanation: '过去习惯用 would' }
  ],
  options: []
};

const FULL_ITEM = {
  type: 'full_cloze',
  word: '',
  stem: '阅读下面短文，从每题所给的 A、B、C、D 四个选项中选出最佳选项。',
  passage: 'The town changed slowly [[1]] the years. New shops [[2]] on the main street. ' +
    'Young people [[3]] to the city, but some [[4]] back every summer.',
  questions: [
    { no: 1, options: ['over', 'under', 'beside', 'beyond'], answer: 'A', explanation: 'over the years 固定搭配' },
    { no: 2, options: ['appeared', 'disappeared', 'remained', 'closed'], answer: 'B', explanation: '与 changed 形成转折' },
    { no: 3, options: ['moved', 'stayed', 'waited', 'looked'], answer: 'A', explanation: '年轻人搬去城市' },
    { no: 4, options: ['came', 'went', 'left', 'fled'], answer: 'D', explanation: '夏天回来' }
  ]
};

(async () => {
  console.log('\n【v1.5.3-T004 EXAM-QUALITY 练习出题质量】');
  try {
    cleanup();
    await setupUser();
    ok('测试用户登录');

    // 1) 长篇阅读多空批改：部分正确
    const g1 = await req('POST', '/api/exercise/grade', {
      type: 'long_reading_cloze',
      user_answers: ['on', 'will'],
      correct_answers: ['on', 'would'],
      knowledge_id: 0,
      source_type: 'exercise',
      source_id: 't35',
      question_no: 1,
      question_type: 'long_reading_cloze',
      options: []
    });
    if (g1.json && g1.json.ok && g1.json.score === 50 && g1.json.correct === false &&
        String(g1.json.reference_answer || '').includes('would')) {
      ok('长篇阅读多空批改', '50 分 · 指出错误空与参考答案');
    } else {
      ko('长篇阅读多空批改', (g1.text || '').slice(0, 200));
    }

    // 2) 长篇阅读多空批改：全部正确
    const g2 = await req('POST', '/api/exercise/grade', {
      type: 'long_reading_cloze',
      user_answers: ['on', 'would'],
      correct_answers: ['on', 'would'],
      knowledge_id: 0, source_type: 'exercise', source_id: 't35', question_no: 1,
      question_type: 'long_reading_cloze', options: []
    });
    if (g2.json && g2.json.ok && g2.json.score === 100 && g2.json.correct === true) {
      ok('长篇阅读全对判定');
    } else {
      ko('长篇阅读全对判定', (g2.text || '').slice(0, 200));
    }

    // 3) 旧题型批改路径保持正常
    const g3 = await req('POST', '/api/exercise/grade', {
      type: 'cloze_choice', user_answer: 'A', correct_answer: 'A', knowledge_id: 0,
      source_type: 'exercise', source_id: 't35', question_no: 1,
      question_type: 'cloze_choice', options: ['a', 'b', 'c', 'd']
    });
    if (g3.json && g3.json.ok && g3.json.correct === true) ok('旧题型批改路径未回归');
    else ko('旧题型批改路径未回归', (g3.text || '').slice(0, 200));

    // 3.5) 完整完形填空：多空选择题按空批改
    const g4 = await req('POST', '/api/exercise/grade', {
      type: 'full_cloze',
      user_answers: ['A', 'A', 'A', 'D'],
      correct_answers: ['A', 'B', 'A', 'D'],
      knowledge_id: 0, source_type: 'exercise', source_id: 't35', question_no: 1,
      question_type: 'full_cloze', options: []
    });
    if (g4.json && g4.json.ok && g4.json.score === 75 && g4.json.correct === false &&
        String(g4.json.reference_answer || '').includes('2. B')) {
      ok('完整完形填空多空批改', '75 分 · 指出错误空');
    } else {
      ko('完整完形填空多空批改', (g4.text || '').slice(0, 200));
    }

    // 4) 不限题型参数校验：总题数 0 应拒绝
    const bad = await req('POST', '/api/custom-exercise', {
      source: { type: 'theme', theme: '测试' }, difficulty: 'basic', style: 'exam',
      types_mode: 'auto', total: 0
    });
    if (bad.status === 400) ok('不限题型总题数校验');
    else ko('不限题型总题数校验', '期望 400，实际 ' + bad.status + '：' + (bad.text || '').slice(0, 120));

    // 5) Word 导出：长篇阅读短文、空位与答案页
    const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 't35_export_'));
    const jsonPath = path.join(tmpDir, 'paper.json');
    const outDir = path.join(tmpDir, 'out');
    fs.mkdirSync(outDir, { recursive: true });
    fs.writeFileSync(jsonPath, JSON.stringify({
      kind: 'custom', id: 35001, title: '出题质量测试卷',
      difficulty: 'basic', style: 'exam', items: [LONG_ITEM, FULL_ITEM]
    }), 'utf8');
    const r = spawnSync(process.execPath, [
      path.resolve(__dirname, '../scripts/gen_word_exercise.js'), '35001', jsonPath, outDir
    ], { encoding: 'utf8', timeout: 60000 });
    const docx = path.join(outDir, 'custom_exercise_35001.docx');
    if (r.status === 0 && fs.existsSync(docx) && fs.statSync(docx).size > 1000) {
      ok('Word 导出长篇阅读', '已生成 ' + path.basename(docx));
    } else {
      ko('Word 导出长篇阅读', (r.stderr || r.stdout || '文件未生成').slice(0, 200));
    }

    // 6) 静态防泄题检查
    const root = path.resolve(__dirname, '..');
    const reviewHtml = fs.readFileSync(path.join(root, 'frontend/pages/review.html'), 'utf8');
    const exportJs = fs.readFileSync(path.join(root, 'scripts/gen_word_exercise.js'), 'utf8');
    const promptsH = fs.readFileSync(path.join(root, 'backend/include/prompts.h'), 'utf8');
    if (!reviewHtml.includes('目标词：') && reviewHtml.includes('p-auto-types') &&
        reviewHtml.includes('long_reading_cloze') && reviewHtml.includes('full_cloze')) {
      ok('前端已移除目标词标签并接入新题型');
    } else {
      ko('前端已移除目标词标签并接入新题型', '静态检查未通过');
    }
    if (!exportJs.includes('的中文意思是（　）') && exportJs.includes('renderBlankPassage') &&
        exportJs.includes('long_reading_cloze') && exportJs.includes('full_cloze')) {
      ok('导出脚本已去掉目标词兜底题干');
    } else {
      ko('导出脚本已去掉目标词兜底题干', '静态检查未通过');
    }
    if (promptsH.includes('{ARTICLE_EXCERPT}') && promptsH.includes('{TYPE_MODE_RULE}') &&
        promptsH.includes('【硬性格式与防泄题规则') && promptsH.includes('long_reading_cloze') &&
        promptsH.includes('full_cloze')) {
      ok('提示词包含文章节选、硬性规则与新题型');
    } else {
      ko('提示词包含文章节选、硬性规则与新题型', '静态检查未通过');
    }

    cleanup();
  } catch (e) {
    ko('套件执行异常', e.message);
    cleanup();
  }

  console.log(`\n  结果：${pass} 通过 / ${fail} 失败`);
  if (fail) {
    console.log('  失败项：');
    failures.forEach(f => console.log('   - ' + f.name + '：' + f.msg));
    process.exit(1);
  }
})();
