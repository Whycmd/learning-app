// 阶段二十七：导出中心（v1.5.0-T009）
// 覆盖：summary 计数与去重、精讲单条/批量模板、按需练习卷、单词卡递归去重、
//       游客当次讲解/练习导出、越权与失败边界、导出不写 AI 用量、docx 关键 XML 断言
// 仅在隔离测试库执行（DB_NAME=eng_app_test）
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT, ROOT } = require('./_env');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const r = spawnSync(MYSQL_BIN,
    ['--socket=' + SOCK, '-uroot', '-p' + pw, '--default-character-set=utf8mb4', '-N', DB_NAME, '-e', sql],
    { encoding: 'utf8', timeout: 30000 });
  if (r.status !== 0) throw new Error((r.stderr || r.stdout || '').trim());
  return r.stdout.trim();
}
function q1(sql) {
  const out = runMysql(sql);
  return out === '' ? 0 : Number(out.split('\n')[0]);
}

function req(method, urlPath, body, cookieHeader) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    if (cookieHeader) headers['Cookie'] = cookieHeader;
    const r = http.request({ host: API_HOST, port: API_PORT, method, path: urlPath, headers, timeout: 120000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(text); } catch (_) {}
        resolve({ status: res.statusCode, json, text, headers: res.headers });
      });
    });
    r.on('error', reject);
    r.on('timeout', () => r.destroy(new Error('timeout')));
    if (data) r.write(data);
    r.end();
  });
}

function sessionCookie(res) {
  const m = (res.headers['set-cookie'] || []).join(';').match(/eng_session=([0-9a-f]+)/);
  return m ? 'eng_session=' + m[1] : '';
}

async function registerUser(name) {
  await req('POST', '/api/auth/register', { username: name, password: 'pass1234', display_name: name });
  const login = await req('POST', '/api/auth/login', { username: name, password: 'pass1234' });
  const ck = sessionCookie(login);
  const me = await req('GET', '/api/auth/me', null, ck);
  return { ck, uid: (me.json && me.json.user_id) || 0 };
}

// 解压 docx，返回 document / header / footer 文本与 XML
function readDocx(docxPath) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 't27docx-'));
  try {
    const r = spawnSync('unzip', ['-o', docxPath, '-d', dir], { encoding: 'utf8' });
    if (r.status !== 0) throw new Error((r.stderr || 'unzip failed').slice(0, 120));
    const wordDir = path.join(dir, 'word');
    const files = fs.readdirSync(wordDir).sort();
    const read = f => fs.readFileSync(path.join(wordDir, f), 'utf8');
    const plain = xml => [...xml.matchAll(/<w:t(?:\s[^>]*)?>([\s\S]*?)<\/w:t>/g)]
      .map(m => m[1]
        .replace(/&lt;/g, '<').replace(/&gt;/g, '>')
        .replace(/&quot;/g, '"').replace(/&apos;/g, "'")
        .replace(/&amp;/g, '&'))
      .join('');
    const bodyXml = read('document.xml');
    const hfFiles = files.filter(f => /^(header|footer)\d*\.xml$/.test(f));
    const hfXml = hfFiles.map(read).join('\n');
    const text = plain(hfXml) + plain(bodyXml);
    const count = s => text.split(s).length - 1;
    return { xml: bodyXml, text, header: plain(hfXml), count };
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

// A4 版式：pgSz 同时含 11906 × 16838（属性顺序不限）
function isA4(xml) {
  const m = xml.match(/<w:pgSz[^>]*>/);
  return !!m && /w:w="11906"/.test(m[0]) && /w:h="16838"/.test(m[0]);
}

function fileSize(p) {
  try { return fs.statSync(p).size; } catch (_) { return -1; }
}

async function main() {
  console.log('=== 阶段二十七：导出中心（v1.5.0-T009） ===\n');

  if (DB_NAME === 'english_learning') {
    console.log('~ 需要隔离测试库，当前 DB=english_learning');
    console.log(`\n阶段二十七结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
    process.exit(0);
  }

  const stamp = Date.now().toString(36).slice(-6);
  const wordTmp = process.env.WORD_TMP || path.join(ROOT, 'word_tmp');
  const usageBefore = q1('SELECT IFNULL(SUM(calls),0) FROM ai_usage_daily');
  const guestBefore = q1('SELECT IFNULL(SUM(used),0) FROM guest_trials');

  try {
    const a = await registerUser('t27a_' + stamp);
    const b = await registerUser('t27b_' + stamp);
    if (a.uid > 0 && b.uid > 0) ok('测试用户注册', `a=${a.uid} b=${b.uid}`);
    else throw new Error('注册失败');

    // ---------- 1. 登录边界 ----------
    const anon = await req('GET', '/api/exports/summary');
    if (anon.status === 401) ok('summary 未登录 401');
    else ko('summary 未登录 401', `status=${anon.status} ${anon.text.slice(0, 120)}`);

    // ---------- 2. 种子：2 个已固化精讲 ----------
    const w1 = 't27alpha' + stamp;
    const w2 = 't27beta' + stamp;
    const kp1 = await req('POST', '/api/kp', { user_input_content: w1 }, a.ck);
    const kp2 = await req('POST', '/api/kp', { user_input_content: w2 }, a.ck);
    const id1 = kp1.json && kp1.json.id;
    const id2 = kp2.json && kp2.json.id;
    if (id1 && id2) ok('录入 2 个待固化词条', `id=${id1},${id2}`);
    else throw new Error('词条录入失败: ' + kp1.text + ' / ' + kp2.text);

    runMysql(`UPDATE knowledge_points SET is_solidified=1, solidify_time=NOW(),
      standard_content='${w1}', difficulty=2, master_require='会写', normalized_key='${w1}'
      WHERE id=${id1};`);
    runMysql(`UPDATE knowledge_points SET is_solidified=1, solidify_time=NOW(),
      standard_content='${w2}', difficulty=1, master_require='会读', normalized_key='${w2}'
      WHERE id=${id2};`);
    runMysql(`INSERT INTO kp_extras (knowledge_id,extra_type,content) VALUES
      (${id1},'markdown_content','# 单词精讲\\n\\n${w1} 的精讲正文。'),
      (${id2},'markdown_content','# 单词精讲\\n\\n${w2} 的精讲正文。');`);

    // kp1：五类结构化素材齐备 → 5 道自测题；kp2：无素材 → 暂无自测题
    runMysql(`INSERT INTO kp_definitions (knowledge_id,pos,meaning,example,display_order) VALUES
      (${id1},'adj.','一丝不苟的','She is meticulous about details.',1);`);
    runMysql(`INSERT INTO kp_collocations (knowledge_id,collocation,display_order) VALUES
      (${id1},'meticulous attention to detail',1);`);
    runMysql(`INSERT INTO kp_synonyms (knowledge_id,synonym,display_order) VALUES
      (${id1},'careful',1);`);
    runMysql(`INSERT INTO kp_antonyms (knowledge_id,antonym,display_order) VALUES
      (${id1},'careless',1);`);
    ok('种子精讲与自测素材就绪');

    // ---------- 3. 种子：词表库（父 3 词 + 子 2 词，1 个重复） ----------
    runMysql(`INSERT INTO wordlists (user_id,title,source,kind,item_count) VALUES (${a.uid},'T27 父词表','user','book',3);`);
    const parentId = q1(`SELECT MAX(id) FROM wordlists WHERE user_id=${a.uid}`);
    runMysql(`INSERT INTO wordlists (user_id,title,source,kind,item_count) VALUES (${a.uid},'T27 子词表','user','book',2);`);
    const childId = q1(`SELECT MAX(id) FROM wordlists WHERE user_id=${a.uid}`);
    runMysql(`INSERT INTO wordlist_items (wordlist_id,word,normalized_word,meaning,order_no) VALUES
      (${parentId},'alpha','alpha','第一个',1),
      (${parentId},'beta','beta','第二个',2),
      (${parentId},'gamma','gamma','第三个',3),
      (${childId},'beta','beta','第二个',1),
      (${childId},'delta','delta','第四个',1);
      INSERT INTO wordlist_children (parent_id,child_type,child_id,order_no) VALUES (${parentId},'list',${childId},1);`);
    ok('种子词表（父 3 + 子 2，重复 1）');

    // ---------- 4. 种子：1 份已完成定制练习（真实定制题字段：题干在 word） ----------
    const exJson = JSON.stringify({
      items: [
        { type: 'word_match', word: 'robot', options: ['A. 机器人', 'B. 互联网'], answer: 'A' },
        { type: 'word_form_fill', word: 'develop', stem: 'The company is __________ new products.', answer: 'developing' },
        { type: 'context_blank_fill', word: 'The rise of __________ has changed our lives.', answer: 'technology' },
        { type: 'cn_to_en_translation', word: '人工智能', answer: 'artificial intelligence' }
      ]
    });
    runMysql(`INSERT INTO custom_exercises (user_id,spec_json,spec_hash,status,item_count,difficulty,style,exercise_json)
      VALUES (${a.uid},'{}','t27${stamp}', 'done',4,'basic','exam',${JSON.stringify(exJson).replace(/'/g, "''")});`);
    const seedCeId = q1(`SELECT MAX(id) FROM custom_exercises WHERE user_id=${a.uid}`);
    ok('种子定制练习');

    // ---------- 5. summary 计数与去重 ----------
    const sum = await req('GET', '/api/exports/summary', null, a.ck);
    const s = (sum.json || {});
    if (sum.status === 200 && s.kp && s.kp.count === 2) ok('summary 精讲计数', `count=${s.kp.count}`);
    else ko('summary 精讲计数', `${sum.status} ${sum.text.slice(0, 140)}`);
    if (s.kp && s.kp.chars > 0) ok('summary 精讲字符数', `chars=${s.kp.chars}`);
    else ko('summary 精讲字符数', JSON.stringify(s.kp));
    if (s.wordbook && s.wordbook.count === 4) ok('summary 单词本递归去重', `count=${s.wordbook.count}`);
    else ko('summary 单词本递归去重', JSON.stringify(s.wordbook));
    if (s.custom_exercise && s.custom_exercise.count === 1 && s.custom_exercise.chars > 0)
      ok('summary 练习卷计数', `count=1 chars=${s.custom_exercise.chars}`);
    else ko('summary 练习卷计数', JSON.stringify(s.custom_exercise));

    const sumB = await req('GET', '/api/exports/summary', null, b.ck);
    if (sumB.json && sumB.json.kp.count === 0 && sumB.json.wordbook.count === 0 && sumB.json.custom_exercise.count === 0)
      ok('summary 用户隔离（B 全 0）');
    else ko('summary 用户隔离（B 全 0）', JSON.stringify(sumB.json));

    // ---------- 5.5 失败任务状态同步（人工测试回归：不再无限「AI 出题中」） ----------
    runMysql(`INSERT INTO ai_tasks (task_type,ref_id,status,error_msg,user_id,kind,lane)
      VALUES ('custom_exercise',0,'failed','模拟生成失败',${a.uid},'custom_exercise','custom_exercise');`);
    const failedTaskId = q1(`SELECT MAX(id) FROM ai_tasks WHERE user_id=${a.uid}`);
    runMysql(`INSERT INTO custom_exercises (user_id,spec_json,spec_hash,status,item_count,difficulty,style,task_id)
      VALUES (${a.uid},'{}','t27${stamp}f','queued',2,'basic','exam',${failedTaskId});`);
    const staleId = q1(`SELECT MAX(id) FROM custom_exercises WHERE user_id=${a.uid}`);
    const stale = await req('GET', `/api/custom-exercise/${staleId}`, null, a.ck);
    if (stale.json && stale.json.status === 'failed' && stale.json.error)
      ok('失败任务回写为 failed', stale.json.error);
    else ko('失败任务回写为 failed', JSON.stringify(stale.json));
    const staleDb = runMysql(`SELECT status FROM custom_exercises WHERE id=${staleId}`);
    if (staleDb === 'failed') ok('失败状态已落库'); else ko('失败状态已落库', staleDb);
    const hist = await req('GET', '/api/custom-exercise/history?limit=10', null, a.ck);
    const histItem = ((hist.json && hist.json.items) || []).find(x => Number(x.id) === staleId);
    if (histItem && histItem.status === 'failed') ok('历史列表同步失败状态');
    else ko('历史列表同步失败状态', JSON.stringify(histItem || hist.json));

    // ---------- 6. 精讲单条导出（docx 断言） ----------
    const kpExp1 = await req('POST', `/api/kp/${id1}/export-word`, null, a.ck);
    const p1 = kpExp1.json && kpExp1.json.word_path;
    if (kpExp1.status === 200 && p1 && fileSize(p1) > 1000) ok('精讲单条导出文件', `${fileSize(p1)} bytes`);
    else ko('精讲单条导出文件', `${kpExp1.status} ${kpExp1.text.slice(0, 140)}`);
    if (p1 && fileSize(p1) > 0) {
      const d = readDocx(p1);
      if (isA4(d.xml)) ok('精讲 A4 版式');
      else ko('精讲 A4 版式', 'pgSz 缺失');
      if (d.header.includes('英语精讲讲义') && d.header.includes('页 / 共'))
        ok('精讲页眉标题与页脚页码');
      else ko('精讲页眉标题与页脚页码', d.header.slice(0, 160));
      if (d.text.includes('自测题（共 5 道）')) ok('精讲自测题 5 道'); else ko('精讲自测题 5 道', d.text.slice(0, 200));
      if (d.text.includes('自测题参考答案') && d.text.includes('一丝不苟的'))
        ok('精讲答案独立页与素材内容');
      else ko('精讲答案独立页与素材内容', d.text.slice(0, 200));
      if (d.text.includes('careful') && d.text.includes('careless'))
        ok('精讲同义/反义素材入题');
      else ko('精讲同义/反义素材入题', d.text.slice(0, 200));
    }

    const kpExp2 = await req('POST', `/api/kp/${id2}/export-word`, null, a.ck);
    const p2 = kpExp2.json && kpExp2.json.word_path;
    if (p2 && fileSize(p2) > 0) {
      const d = readDocx(p2);
      if (d.text.includes('暂无自测题')) ok('素材不足显示暂无自测题');
      else ko('素材不足显示暂无自测题', d.text.slice(0, 200));
    } else ko('素材不足显示暂无自测题', `${kpExp2.status} ${kpExp2.text.slice(0, 120)}`);

    const kpExpB = await req('POST', `/api/kp/${id1}/export-word`, null, b.ck);
    if (kpExpB.status === 404) ok('他人精讲导出 404'); else ko('他人精讲导出 404', `status=${kpExpB.status}`);

    const kpExpPending = await req('POST', '/api/kp/999999/export-word', null, a.ck);
    if (kpExpPending.status === 404) ok('不存在精讲导出 404'); else ko('不存在精讲导出 404', `status=${kpExpPending.status}`);

    // ---------- 7. 精讲批量导出 ----------
    const batch = await req('POST', '/api/kp/export-batch-word', null, a.ck);
    const bp = batch.json && batch.json.word_path;
    if (batch.status === 200 && bp && fileSize(bp) > 1000 && batch.json.count === 2)
      ok('精讲批量导出', `count=${batch.json.count} ${fileSize(bp)} bytes`);
    else ko('精讲批量导出', `${batch.status} ${batch.text.slice(0, 140)}`);
    if (bp && fileSize(bp) > 0) {
      const d = readDocx(bp);
      if (d.text.includes('英语精讲讲义（共 2 个词条）') && d.text.includes(w1) && d.text.includes(w2))
        ok('批量讲义含 2 个词条');
      else ko('批量讲义含 2 个词条', d.text.slice(0, 200));
    }

    // ---------- 8. 按需练习卷导出 ----------
    const paperPayload = {
      exercises: [
        { type: 'cn_to_en_translation', prompt: '他做事一丝不苟。', answer: 'He is meticulous.', user_answer: 'ZZUSERANSWER' },
        { type: 'word_match', stem: 'alpha 的意思是？', options: ['第一个', '第二个'], answer: 'A', user_answer: 'ZZUSERANSWER' }
      ],
      knowledge_points: [{ standard_content: w1, master_require: '会写' }],
      quick_memo: {},
      title: '复习练习卷'
    };
    const paper = await req('POST', '/api/exercise/export-word', paperPayload, a.ck);
    const pp = paper.json && paper.json.word_path;
    if (paper.status === 200 && pp && fileSize(pp) > 1000) ok('练习卷按需导出文件', `${fileSize(pp)} bytes`);
    else ko('练习卷按需导出文件', `${paper.status} ${paper.text.slice(0, 140)}`);
    if (pp && fileSize(pp) > 0) {
      const d = readDocx(pp);
      if (isA4(d.xml)) ok('练习卷 A4 版式'); else ko('练习卷 A4 版式', 'pgSz 缺失');
      if (d.text.includes('复习练习卷') && d.text.includes('参考答案与解析') && d.text.includes('满分词汇总结'))
        ok('练习卷标题/答案页/词汇总结');
      else ko('练习卷标题/答案页/词汇总结', d.text.slice(0, 200));
      if (!d.text.includes('ZZUSERANSWER')) ok('练习卷为空白卷（不含作答）');
      else ko('练习卷为空白卷（不含作答）', '出现用户答案字段');
    }
    const paperEmpty = await req('POST', '/api/exercise/export-word', { exercises: [] }, a.ck);
    if (paperEmpty.status === 400 && !(paperEmpty.json && paperEmpty.json.word_path))
      ok('空试卷导出 400 且无文件');
    else ko('空试卷导出 400 且无文件', `${paperEmpty.status} ${paperEmpty.text.slice(0, 140)}`);

    // ---------- 9. 单词卡导出（递归去重） ----------
    const cards = await req('POST', `/api/wordlists/${parentId}/export-word`, null, a.ck);
    const cp = cards.json && cards.json.word_path;
    if (cards.status === 200 && cp && fileSize(cp) > 1000) ok('单词卡导出文件', `${fileSize(cp)} bytes`);
    else ko('单词卡导出文件', `${cards.status} ${cards.text.slice(0, 140)}`);
    if (cp && fileSize(cp) > 0) {
      const d = readDocx(cp);
      if (isA4(d.xml)) ok('单词卡 A4 版式'); else ko('单词卡 A4 版式', 'pgSz 缺失');
      if (d.text.includes('页 / 共')) ok('单词卡页脚页码'); else ko('单词卡页脚页码', d.text.slice(0, 120));
      if (d.count('beta') === 1) ok('单词卡递归包含子本并按 normalized 去重', `beta×${d.count('beta')}`);
      else ko('单词卡递归包含子本并按 normalized 去重', `beta×${d.count('beta')}`);
      if (d.text.includes('alpha') && d.text.includes('gamma') && d.text.includes('delta') && d.text.includes('第四个'))
        ok('单词卡含单词与释义');
      else ko('单词卡含单词与释义', d.text.slice(0, 200));
    }
    const cardsB = await req('POST', `/api/wordlists/${parentId}/export-word`, null, b.ck);
    if (cardsB.status === 404) ok('他人词表导出 404'); else ko('他人词表导出 404', `status=${cardsB.status}`);

    // ---------- 9.5 定制练习卷：真实字段渲染（词义匹配题干、词性转换候选词） ----------
    const cexp = await req('POST', `/api/custom-exercise/${seedCeId}/export-word`, null, a.ck);
    const cpp = cexp.json && cexp.json.word_path;
    if (cexp.status === 200 && cpp && fileSize(cpp) > 1000) ok('定制练习卷导出文件', `${fileSize(cpp)} bytes`);
    else ko('定制练习卷导出文件', `${cexp.status} ${cexp.text.slice(0, 140)}`);
    if (cpp && fileSize(cpp) > 0) {
      const d = readDocx(cpp);
      if (!d.text.includes('robot 的中文意思是') && d.text.includes('阅读短文，完成下列题目。')) ok('词义匹配题不再直接给出目标词');
      else ko('词义匹配题不再直接给出目标词', d.text.slice(0, 200));
      if (!d.text.includes('候选词：') && d.text.includes('(develop)')) ok('词性转换基础词内联在空位后');
      else ko('词性转换基础词内联在空位后', d.text.slice(0, 200));
      if (d.text.includes('The rise of __________')) ok('语境选词题干取自 word 字段');
      else ko('语境选词题干取自 word 字段', d.text.slice(0, 200));
      if (d.text.includes('人工智能')) ok('中译英题干取自 word 字段');
      else ko('中译英题干取自 word 字段', d.text.slice(0, 200));
    }

    const missingFile = path.join(wordTmp, 'wordlist_999999.docx');
    const cardsMissing = await req('POST', '/api/wordlists/999999/export-word', null, a.ck);
    if (cardsMissing.status >= 400 && fileSize(missingFile) === -1)
      ok('不存在词表导出失败且不产生文件');
    else ko('不存在词表导出失败且不产生文件', `status=${cardsMissing.status} size=${fileSize(missingFile)}`);

    // ---------- 10. 游客当次讲解导出 ----------
    const gid = 't27g' + stamp;
    const md = '# 单词精讲\n\n游客当次讲解正文 ' + stamp;
    runMysql(`INSERT INTO explanations (user_id,guest_id,input_text,normalized_key,status,summary,content_md)
      VALUES (0,'${gid}','t27old${stamp}','t27old${stamp}','done','摘要旧','# 单词精讲\\n\\n旧讲解');`);
    const oldEx = q1(`SELECT MAX(id) FROM explanations WHERE guest_id='${gid}'`);
    runMysql(`INSERT INTO explanations (user_id,guest_id,input_text,normalized_key,status,summary,content_md)
      VALUES (0,'${gid}','t27new${stamp}','t27new${stamp}','done','摘要新','${md.replace(/'/g, "''")}');`);
    const newEx = q1(`SELECT MAX(id) FROM explanations WHERE guest_id='${gid}'`);

    const gOld = await req('POST', `/api/exports/explanation/${oldEx}`, null, `eng_guest_id=${gid}`);
    if (gOld.status === 403) ok('游客非最新讲解导出 403');
    else ko('游客非最新讲解导出 403', `${gOld.status} ${gOld.text.slice(0, 140)}`);

    const gNew = await req('POST', `/api/exports/explanation/${newEx}`, null, `eng_guest_id=${gid}`);
    const gp = gNew.json && gNew.json.word_path;
    if (gNew.status === 200 && gp && fileSize(gp) > 1000) ok('游客最新讲解导出文件', `${fileSize(gp)} bytes`);
    else ko('游客最新讲解导出文件', `${gNew.status} ${gNew.text.slice(0, 140)}`);
    if (gp && fileSize(gp) > 0) {
      const d = readDocx(gp);
      if (d.text.includes('AI 精讲讲义') && d.text.includes('暂无自测题') && d.text.includes('游客当次讲解正文'))
        ok('游客讲义模板与正文');
      else ko('游客讲义模板与正文', d.text.slice(0, 200));
    }
    const gWrong = await req('POST', `/api/exports/explanation/${newEx}`, null, `eng_guest_id=other${stamp}`);
    if (gWrong.status === 404) ok('他人游客讲解导出 404'); else ko('他人游客讲解导出 404', `status=${gWrong.status}`);

    // ---------- 11. 游客当次练习导出 ----------
    const gExJson = JSON.stringify({
      items: [{ type: 'cn_to_en_translation', prompt: '游客练习旧题', answer: 'old' }]
    });
    runMysql(`INSERT INTO custom_exercises (guest_id,spec_json,spec_hash,status,item_count,difficulty,style,exercise_json)
      VALUES ('${gid}','{}','t27g${stamp}a','done',1,'basic','exam',${JSON.stringify(gExJson).replace(/'/g, "''")});`);
    const gOldCe = q1(`SELECT MAX(id) FROM custom_exercises WHERE guest_id='${gid}'`);
    const gExJson2 = JSON.stringify({
      items: [{ type: 'cn_to_en_translation', prompt: '游客练习新题', answer: 'new' }]
    });
    runMysql(`INSERT INTO custom_exercises (guest_id,spec_json,spec_hash,status,item_count,difficulty,style,exercise_json)
      VALUES ('${gid}','{}','t27g${stamp}b','done',1,'basic','exam',${JSON.stringify(gExJson2).replace(/'/g, "''")});`);
    const gNewCe = q1(`SELECT MAX(id) FROM custom_exercises WHERE guest_id='${gid}'`);

    const geOld = await req('POST', `/api/exports/exercise/${gOldCe}`, null, `eng_guest_id=${gid}`);
    if (geOld.status === 403) ok('游客非最新练习导出 403');
    else ko('游客非最新练习导出 403', `${geOld.status} ${geOld.text.slice(0, 140)}`);

    const geNew = await req('POST', `/api/exports/exercise/${gNewCe}`, null, `eng_guest_id=${gid}`);
    const gep = geNew.json && geNew.json.word_path;
    if (geNew.status === 200 && gep && fileSize(gep) > 1000) ok('游客最新练习导出文件', `${fileSize(gep)} bytes`);
    else ko('游客最新练习导出文件', `${geNew.status} ${geNew.text.slice(0, 140)}`);
    if (gep && fileSize(gep) > 0) {
      const d = readDocx(gep);
      if (d.text.includes('定制练习卷') && d.text.includes('参考答案与解析') && d.text.includes('游客练习新题'))
        ok('游客练习卷模板与内容');
      else ko('游客练习卷模板与内容', d.text.slice(0, 200));
    }

    // ---------- 12. 导出不写 AI 用量 ----------
    const usageAfter = q1('SELECT IFNULL(SUM(calls),0) FROM ai_usage_daily');
    const guestAfter = q1('SELECT IFNULL(SUM(used),0) FROM guest_trials');
    if (usageAfter === usageBefore && guestAfter === guestBefore)
      ok('全部导出不写 ai_usage_daily / guest_trials', `${usageAfter}/${guestAfter}`);
    else ko('全部导出不写 ai_usage_daily / guest_trials', `usage ${usageBefore}->${usageAfter}, guest ${guestBefore}->${guestAfter}`);

    // ---------- 13. 综合练习按词表取词（回归：DISTINCT + ORDER BY 导致任务失败） ----------
    const genSpec = { source: { type: 'wordlist', id: parentId }, difficulty: 'basic', style: 'exam', counts: { cloze_choice: 2 } };
    const gen = await req('POST', '/api/custom-exercise', genSpec, a.ck);
    const genId = gen.json && gen.json.id;
    if (gen.status === 200 && genId) ok('词表来源综合练习创建', `id=${genId}`);
    else ko('词表来源综合练习创建', `${gen.status} ${gen.text.slice(0, 160)}`);
    let genDone = null;
    for (let i = 0; i < 30 && genId; i++) {
      await new Promise(r => setTimeout(r, 500));
      const g = await req('GET', `/api/custom-exercise/${genId}`, null, a.ck);
      if (g.json && (g.json.status === 'done' || g.json.status === 'failed')) { genDone = g.json; break; }
    }
    if (genDone && genDone.status === 'done' && (genDone.items || []).length === 2)
      ok('词表来源综合练习生成完成', `items=${(genDone.items || []).length}`);
    else ko('词表来源综合练习生成完成', JSON.stringify(genDone || {}).slice(0, 200));
  } catch (e) {
    ko('套件异常', e.message);
  }

  console.log(`\n阶段二十七结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
  if (fail) {
    console.log('失败项：');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg || ''}`));
    process.exit(1);
  }
}

main();
