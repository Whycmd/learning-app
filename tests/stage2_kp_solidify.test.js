// 阶段二：知识点录入 & AI 固化模块 - 集成测试
// 覆盖验收标准：录入、状态区分、字段落库、AI 固化拓展、Word 导出、掌握程度判定、查重兜底、人工兜底
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const BACKEND_URL = 'http://127.0.0.1:8088';
const MYSQL_BIN = '/Users/junwang/.local/mysql/bin/mysql';
const MYSQL_PW_FILE = '/Users/junwang/.local/mysql/.root_pw';
const SOCK = '/Users/junwang/.local/run/mysql.sock';
const WORD_TMP = path.join(ROOT, 'word_tmp');

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

// 解压 .docx 并读取 document.xml（T013 导出内容断言）
function readDocxXml(docxPath) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 't013docx-'));
  try {
    const r = spawnSync('unzip', ['-o', docxPath, '-d', dir], { encoding:'utf8' });
    if (r.status !== 0) throw new Error((r.stderr || 'unzip failed').slice(0, 120));
    return fs.readFileSync(path.join(dir, 'word/document.xml'), 'utf8');
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

// 只清理本套件测试用户（t2user_%）名下的数据
function cleanupTestData() {
  const like = "u.username LIKE 't2user_%'";
  runMysql(`DELETE i FROM solidify_job_items i JOIN solidify_jobs j ON i.job_id=j.id JOIN users u ON j.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE j FROM solidify_jobs j JOIN users u ON j.user_id=u.id WHERE ${like};`);
  for (const t of ['user_learning', 'kp_definitions', 'kp_collocations', 'kp_synonyms', 'kp_antonyms', 'kp_extras'])
    runMysql(`DELETE t FROM ${t} t JOIN knowledge_points kp ON t.knowledge_id=kp.id JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE a FROM ai_tasks a JOIN knowledge_points kp ON a.ref_id=kp.id JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE we FROM wordbook_entry we JOIN wordbook wb ON we.wordbook_id=wb.id JOIN users u ON wb.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE wb FROM wordbook wb JOIN users u ON wb.user_id=u.id WHERE ${like};`);
  runMysql(`DELETE kp FROM knowledge_points kp JOIN users u ON kp.user_id=u.id WHERE ${like};`);
  runMysql("DELETE FROM user_sessions WHERE user_id IN (SELECT id FROM users WHERE username LIKE 't2user_%');");
  runMysql("DELETE FROM users WHERE username LIKE 't2user_%';");
}

// 注册并登录本套件专用测试用户
async function setupTestUser() {
  TEST_USER = 't2user_' + Date.now().toString(36);
  const reg = await req('POST', '/api/auth/register', { username: TEST_USER, password: 'test pass 1' });
  if (!reg.json || !reg.json.ok) throw new Error('测试用户注册失败: ' + reg.text.slice(0, 150));
  const login = await req('POST', '/api/auth/login', { username: TEST_USER, password: 'test pass 1' });
  if (!login.setCookie) throw new Error('测试用户登录失败: ' + login.text.slice(0, 150));
  COOKIE = login.setCookie;
  TEST_UID = parseInt(runMysql(`SELECT id FROM users WHERE username='${TEST_USER}';`).stdout.trim(), 10);
}

async function main() {
  console.log('=== 阶段二：知识点录入 & AI 固化集成测试 ===\n');

  // ---- 0. 前置：清理本套件测试数据并创建独立测试用户 ----
  console.log('[0] 清理测试数据并创建测试用户');
  try {
    cleanupTestData();
    await setupTestUser();
    ok('清理数据并创建测试用户', `${TEST_USER} (uid=${TEST_UID})`);
  } catch (e) { ko('清理数据', e.message); }

  // ---- 1. 知识点录入 ----
  console.log('\n[1] 知识点录入（未固化不查重）');
  try {
    const r1 = await req('POST', '/api/kp', { user_input_content: 'start', remark: '启动相关用法' });
    if (r1.status === 201 || r1.status === 200) {
      if (r1.json && r1.json.ok && r1.json.id > 0) ok(`单条录入 start`, `id=${r1.json.id}`);
      else ko('单条录入', JSON.stringify(r1.json));
    } else ko('单条录入', 'status=' + r1.status);

    const r2 = await req('POST', '/api/kp', { user_input_content: 'begin' });
    if (r2.json && r2.json.ok) ok('单条录入 begin');
    else ko('单条录入 begin', JSON.stringify(r2.json));

    const r3 = await req('POST', '/api/kp', { user_input_content: 'start' }); // 重复录入
    if (r3.json && r3.json.ok) ok('重复录入 start（未固化不查重）', `id=${r3.json.id}`);
    else ko('重复录入', JSON.stringify(r3.json));

    // 验证字段落库
    const cnt = runMysql(`SELECT COUNT(*) FROM knowledge_points WHERE user_id=${TEST_UID} AND user_input_content='start';`).stdout.trim();
    if (cnt === '2') ok('字段 user_input_content 落库');
    else ko('字段落库', 'start 出现次数=' + cnt);

    // 默认未固化
    const unsol = runMysql(`SELECT COUNT(*) FROM knowledge_points WHERE user_id=${TEST_UID} AND is_solidified=0;`).stdout.trim();
    if (parseInt(unsol) >= 3) ok('默认未固化（is_solidified=0）');
    else ko('默认未固化', 'count=' + unsol);

    // create_time 自动生成
    const ct = runMysql(`SELECT create_time FROM knowledge_points WHERE user_id=${TEST_UID} ORDER BY id DESC LIMIT 1;`).stdout.trim();
    if (/^2026-/.test(ct)) ok('create_time 自动生成', ct);
    else ko('create_time', ct);
  } catch (e) { ko('录入', e.message); }

  // ---- 2. 列表查询 ----
  console.log('\n[2] 列表查询');
  try {
    const r = await req('GET', '/api/kp?page=1&size=10');
    if (r.json && r.json.data && Array.isArray(r.json.data) && r.json.data.length >= 3) {
      ok('列表分页', `返回 ${r.json.data.length} 条`);
      // 状态字段存在
      const first = r.json.data[0];
      ['id','user_input_content','standard_content','difficulty','master_require','is_solidified','create_time'].forEach(k => {
        if (k in first) {} else ko(`列表字段 ${k}`, '缺失');
      });
      ok('列表字段完整');
    } else ko('列表', JSON.stringify(r.text).slice(0,200));
    // 仅未固化
    const r2 = await req('GET', '/api/kp?unsolidified=1');
    if (r2.json && r2.json.data && r2.json.data.every(k => k.is_solidified === 0)) ok('unsolidified 过滤生效');
    else ko('unsolidified 过滤', JSON.stringify(r2.json).slice(0,200));
  } catch (e) { ko('列表', e.message); }

  // ---- 3. AI 一键固化 ----
  console.log('\n[3] AI 一键批量固化');
  try {
    const r = await req('POST', '/api/kp/solidify');
    if (r.json && r.json.total >= 3) {
      ok('固化任务执行', `total=${r.json.total}, success=${r.json.success}, failed=${r.json.failed}`);
      if (r.json.failed === 0) ok('全部固化成功');
      else ko('全部固化成功', 'failed=' + r.json.failed + ' ' + (r.json.failed_reasons||[]).join(';'));
      // Word 文件生成
      if (r.json.word_paths && r.json.word_paths.length > 0) {
        const wp = r.json.word_paths[0];
        if (fs.existsSync(wp) && fs.statSync(wp).size > 1000) {
          ok('Word 文件生成', `${wp} (${fs.statSync(wp).size} bytes)`);
        } else ko('Word 文件生成', wp + ' 不存在或过小');
      }
    } else ko('固化任务', JSON.stringify(r).slice(0,300));
  } catch (e) { ko('固化', e.message); }

  // ---- 4. 固化后字段校验 ----
  console.log('\n[4] 固化后字段与子表');
  try {
    // 取第一个固化知识点
    const r = await req('GET', '/api/kp?page=1&size=1');
    if (r.json && r.json.data && r.json.data[0]) {
      const id = r.json.data[0].id;
      const d = await req('GET', `/api/kp/${id}`);
      if (d.json && d.json.is_solidified === 1) {
        ok('is_solidified=1');
        if (d.json.standard_content) ok('standard_content 已固化', d.json.standard_content);
        else ko('standard_content', '空');
        if (d.json.difficulty !== null && d.json.difficulty > 0) ok('difficulty 已赋值', `level=${d.json.difficulty}`);
        else ko('difficulty', JSON.stringify(d.json.difficulty));
        if (d.json.master_require && /会读|会写/.test(d.json.master_require)) ok('master_require 已固化', d.json.master_require);
        else ko('master_require', JSON.stringify(d.json.master_require));
        if (d.json.solidify_time) ok('solidify_time 已赋值', d.json.solidify_time);
        else ko('solidify_time', '空');
        // 子表
        if (Array.isArray(d.json.definitions) && d.json.definitions.length > 0) ok(`子表 kp_definitions (${d.json.definitions.length} 条)`);
        else ko('子表 kp_definitions', '空');
        if (Array.isArray(d.json.collocations) && d.json.collocations.length > 0) ok(`子表 kp_collocations (${d.json.collocations.length} 条)`);
        else ko('子表 kp_collocations', '空');
        if (Array.isArray(d.json.synonyms) && d.json.synonyms.length > 0) ok(`子表 kp_synonyms (${d.json.synonyms.length} 条)`);
        else ko('子表 kp_synonyms', '空');
        if (Array.isArray(d.json.antonyms) && d.json.antonyms.length > 0) ok(`子表 kp_antonyms (${d.json.antonyms.length} 条)`);
        else ko('子表 kp_antonyms', '空');
        if (Array.isArray(d.json.extras) && d.json.extras.length > 0) ok(`子表 kp_extras (${d.json.extras.length} 条)`);
        else ko('子表 kp_extras', '空');
        // v1.4.0-T010：讲解类内容整块 Markdown 存储
        const extraTypes = (d.json.extras || []).map(e => e.extra_type);
        if (extraTypes.includes('markdown_content')) {
          ok('extras 含 markdown_content');
          const md = d.json.extras.find(e => e.extra_type === 'markdown_content');
          if (md && md.content && md.content.trim().length >= 20) ok('markdown_content 内容非空', `${md.content.trim().length} 字符`);
          else ko('markdown_content 内容', '空或过短');
        } else ko('extras 含 markdown_content', '未找到，types=' + extraTypes.join(','));
      } else ko('固化状态', JSON.stringify(d.json).slice(0,200));
    }
  } catch (e) { ko('字段校验', e.message); }

  // ---- 4b. 旧格式 extras 兼容（T010）----
  console.log('\n[4b] 旧格式 extras 兼容');
  try {
    runMysql(`INSERT INTO knowledge_points (user_id, user_input_content, standard_content, difficulty, master_require, is_solidified, create_time, solidify_time) VALUES (${TEST_UID}, 't010_legacy_word', 't010_legacy_word', 2, '会写', 1, NOW(), NOW());`);
    const lid = parseInt(runMysql(`SELECT id FROM knowledge_points WHERE user_id=${TEST_UID} AND user_input_content='t010_legacy_word' ORDER BY id DESC LIMIT 1;`).stdout.trim(), 10);
    if (!lid) throw new Error('构造旧格式词条失败');
    runMysql(`INSERT INTO kp_extras (knowledge_id, extra_type, content, display_order) VALUES (${lid}, 'phonetic', '/ˌleɡəsi/', 0), (${lid}, 'root', '旧格式词根内容', 1);`);
    const d = await req('GET', `/api/kp/${lid}`);
    if (d.status === 200 && d.json && Array.isArray(d.json.extras)) {
      const oldTypes = d.json.extras.map(e => e.extra_type);
      if (oldTypes.includes('phonetic') && oldTypes.includes('root')) ok('旧格式 extras 读取正常', oldTypes.join(','));
      else ko('旧格式 extras', 'types=' + oldTypes.join(','));
    } else ko('旧格式详情', 'status=' + d.status + ' ' + String(d.text).slice(0, 120));
  } catch (e) { ko('旧格式 extras 兼容', e.message); }

  // ---- 4c. 单词本导入完整数据还原（T010 导入修复）----
  console.log('\n[4c] 单词本导入还原 extras / 同反义词');
  try {
    const word = 't010_import_' + Date.now().toString(36);
    const payload = {
      name: 't010_import_probe',
      entries: [{
        standard_content: word,
        difficulty: 2,
        definitions: [{ pos: 'n.', meaning: 'probe meaning', example: 'A probe example.' }],
        collocations: [{ collocation: 'probe collocation', example: 'An example.' }],
        synonyms: [{ id: 1, synonym: 'probe_syn', usage_diff: '用法差异示例' }],
        antonyms: [{ id: 1, antonym: 'probe_ant' }],
        extras: [{ id: 1, extra_type: 'markdown_content', content: '# 单词精讲\n## 词根与词源\nprobe root content' }]
      }]
    };
    const im = await req('POST', '/api/wordbook/import', { json_data: payload });
    if (im.status === 200 && im.json && im.json.imported === 1) ok('导入新词条', `wordbook_id=${im.json.wordbook_id}`);
    else ko('导入新词条', `status=${im.status} ${String(im.text).slice(0, 150)}`);

    const list = await req('GET', '/api/kp?size=1000');
    const hit = (list.json && list.json.data || []).find(k => k.standard_content === word);
    if (!hit) throw new Error('导入后未找到知识点');
    const d = await req('GET', `/api/kp/${hit.id}`);
    const types = (d.json.extras || []).map(e => e.extra_type);
    if (types.includes('markdown_content')) ok('导入保留 markdown_content');
    else ko('导入保留 markdown_content', 'types=' + types.join(','));
    if ((d.json.synonyms || []).length && d.json.synonyms[0].synonym === 'probe_syn') ok('导入保留同义词及用法差异');
    else ko('导入同义词', JSON.stringify(d.json.synonyms));
    if ((d.json.antonyms || []).length && d.json.antonyms[0].antonym === 'probe_ant') ok('导入保留反义词');
    else ko('导入反义词', JSON.stringify(d.json.antonyms));
  } catch (e) { ko('导入还原', e.message); }

  // ---- 4d. 异步批量固化 + 词条级渐进展示（T012）----
  console.log('\n[4d] 异步批量固化与渐进展示');
  try {
    const words = Array.from({ length: 5 }, (_, i) => `t012_async_${i}`);
    const created = [];
    for (const w of words) {
      const r = await req('POST', '/api/kp', { user_input_content: w });
      if (r.json && r.json.ok) created.push(r.json.id);
    }
    if (created.length !== 5) throw new Error('录入异步测试词失败: ' + created.length);
    ok('录入 5 个异步测试词', created.join(','));

    const t0 = Date.now();
    const sub = await req('POST', '/api/kp/solidify/async', {});
    const submitMs = Date.now() - t0;
    if (sub.status === 200 && sub.json && sub.json.job_id && sub.json.total === 5) {
      ok('异步提交立即返回', `job=${sub.json.job_id}, total=5, ${submitMs}ms`);
      if (submitMs < 5000) ok('提交耗时 < 5s', `${submitMs}ms`);
      else ko('提交耗时', `${submitMs}ms`);
    } else throw new Error('异步提交失败: ' + String(sub.text).slice(0, 200));

    const jobId = sub.json.job_id;
    let partial = null, last = null;
    for (let i = 0; i < 300; i++) {
      await new Promise(r => setTimeout(r, 1000));
      const p = await req('GET', `/api/kp/solidify/jobs/${jobId}`);
      if (p.status !== 200 || !p.json) continue;
      last = p.json;
      if (!partial && p.json.done > 0 && p.json.done < p.json.total && p.json.completed_ids.length > 0)
        partial = p.json;
      if (p.json.status === 'done' || p.json.status === 'partial' || p.json.status === 'failed') break;
    }
    if (partial) {
      ok('生成中即可见部分词条', `done=${partial.done}/${partial.total}, ids=${partial.completed_ids.join(',')}`);
      const d = await req('GET', `/api/kp/${partial.completed_ids[0]}`);
      if (d.status === 200 && d.json && d.json.is_solidified === 1) ok('部分完成词条详情可打开');
      else ko('部分完成词条详情', 'status=' + d.status);
    } else ko('生成中即可见部分词条', '未捕获到 done>0 且 done<total 的快照');

    if (last && (last.status === 'done' || last.status === 'partial')) {
      ok('异步任务结束', `status=${last.status}, done=${last.done}, failed=${last.failed}`);
      if (last.failed === 0 && last.done === 5) ok('5 个词全部固化');
      else ko('异步固化结果', JSON.stringify(last).slice(0, 200));
    } else ko('异步任务结束', JSON.stringify(last).slice(0, 200));

    const hist = await req('GET', '/api/kp/solidify/jobs?limit=20');
    const hit = (hist.json && (hist.json.data || [])).find(j => j.job_id === jobId);
    if (hit) ok('任务历史可查询');
    else ko('任务历史', JSON.stringify(hist.json).slice(0, 200));

    const again = await req('POST', '/api/kp/solidify/async', {});
    if (again.json && again.json.reused === true) {
      ok('重复提交复用任务', `job=${again.json.job_id}`);
      if (again.json.job_id === jobId) ok('复用同一任务 ID');
    } else if (again.json && again.json.total === 0) ok('重复提交时已无未固化词');
    else ko('重复提交复用', JSON.stringify(again.json).slice(0, 150));

    const rs = await req('POST', `/api/kp/solidify/jobs/${jobId}/resume`);
    if (rs.status === 409) ok('已完成任务不可继续');
    else ko('已完成任务继续', 'status=' + rs.status + ' ' + String(rs.text).slice(0, 100));
  } catch (e) { ko('异步固化', e.message); }

  // ---- 4e. 断点续跑（T012）----
  console.log('\n[4e] 断点续跑');
  try {
    // 构造一个“中断 + 已固化”的任务明细：继续后应跳过 AI，直接完成
    const lid = parseInt(runMysql(`SELECT id FROM knowledge_points WHERE user_id=${TEST_UID} AND is_solidified=1 ORDER BY id LIMIT 1;`).stdout.trim(), 10);
    if (!lid) throw new Error('缺少已固化知识点');
    const jid = 't012resume' + Date.now().toString(36).slice(-8);
    runMysql(`INSERT INTO solidify_jobs (id, user_id, total, done, failed, status) VALUES ('${jid}', ${TEST_UID}, 1, 0, 1, 'interrupted');`);
    runMysql(`INSERT INTO solidify_job_items (job_id, knowledge_id, status, error_msg) VALUES ('${jid}', ${lid}, 'failed', 'simulated network error');`);
    const rr = await req('POST', `/api/kp/solidify/jobs/${jid}/resume`);
    if (rr.status === 200 && rr.json && rr.json.ok) ok('断点继续已启动');
    else ko('断点继续', 'status=' + rr.status + ' ' + String(rr.text).slice(0, 100));
    let jj = null;
    for (let i = 0; i < 15; i++) {
      await new Promise(r => setTimeout(r, 1000));
      const p = await req('GET', `/api/kp/solidify/jobs/${jid}`);
      jj = p.json;
      if (jj && (jj.status === 'done' || jj.status === 'partial' || jj.status === 'failed')) break;
    }
    if (jj && jj.status === 'done' && jj.done === 1 && jj.failed === 0) ok('续跑跳过已完成项并结束', 'done=1');
    else ko('断点续跑结果', JSON.stringify(jj).slice(0, 200));
  } catch (e) { ko('断点续跑', e.message); }

  // ---- 5. 掌握程度判定规则 ----
  console.log('\n[5] 掌握程度判定');
  try {
    // 判定规则：难度 ≥ 雅思(5) = 会读，否则会写（与文档/代码一致）
    // 检查固化后所有知识点的 master_require
    const rs = runMysql(`SELECT difficulty, master_require FROM knowledge_points WHERE user_id=${TEST_UID} AND is_solidified=1;`).stdout.trim().split('\n').filter(l=>l.trim());
    let ok1 = true;
    for (const line of rs) {
      const [diff, mr] = line.split('\t');
      const diffNum = parseInt(diff || '0', 10);
      const expected = diffNum >= 5 ? '会读' : '会写';
      if (mr !== expected) { ok1 = false; ko(`判定 ${diff}/${mr}`, `期望 ${expected}`); break; }
    }
    if (ok1 && rs.length > 0) ok('判定规则合规', `${rs.length} 条全部判定正确`);
    else if (rs.length === 0) ko('判定规则', '无固化数据');
  } catch (e) { ko('判定', e.message); }

  // ---- 6. Word 导出与下载 ----
  console.log('\n[6] Word 导出与下载');
  try {
    const r = await req('GET', '/api/kp?page=1&size=1');
    const id = r.json.data[0].id;
    const exp = await req('POST', `/api/kp/${id}/export-word`);
    if (exp.json && exp.json.word_path && fs.existsSync(exp.json.word_path)) {
      ok('Word 导出', exp.json.word_path);
      // 文件下载
      const dl = await new Promise((resolve, reject) => {
        http.get(`${BACKEND_URL}/api/word/download?path=${encodeURIComponent(exp.json.word_path)}`, res => {
          let chunks = [];
          res.on('data', c => chunks.push(c));
          res.on('end', () => resolve({ status:res.statusCode, buf:Buffer.concat(chunks), headers:res.headers }));
        }).on('error', reject);
      });
      if (dl.status === 200 && dl.buf.length > 1000 && dl.headers['content-type'].includes('wordprocessing')) {
        ok('Word 下载', `${dl.buf.length} bytes, type=${dl.headers['content-type']}`);
        const head = dl.buf.slice(0,2);
        if (head[0] === 0x50 && head[1] === 0x4b) ok('下载文件为有效 .docx (PK 头)');
        else ko('下载文件 .docx', '头部: ' + head.toString('hex'));
      } else ko('Word 下载', 'status=' + dl.status + ' size=' + dl.buf.length);
    } else ko('Word 导出', JSON.stringify(exp.json).slice(0,200));
  } catch (e) { ko('Word', e.message); }

  // ---- 6b. Word 导出 Markdown 精讲与 LaTeX（T013）----
  console.log('\n[6b] Word 导出 Markdown 精讲与 LaTeX');
  let mdExportId = 0;
  try {
    const mdSample = String.raw`# 单词精讲
## 核心含义与例句
### 形容词
- **t013_md_export** (adj.) 导出测试
- **例句**: This is a T013 export test. 这是导出测试。
## 公式示例
行内公式 \(E=mc^2\)，以及 \(\alpha \rightarrow \beta\)。
\[\frac{a}{b}=c\]
## 固定搭配
1. **t013 bold phrase**: 粗体搭配
2. **t013 plan**: 列表第二项
> 引用块内容
<script>alert('t013')</script>
`;
    runMysql(`INSERT INTO knowledge_points (user_id, user_input_content, standard_content, difficulty, master_require, is_solidified, create_time, solidify_time) VALUES (${TEST_UID}, 't013_md_export', 't013_md_export', 2, '会写', 1, NOW(), NOW());`);
    mdExportId = parseInt(runMysql(`SELECT id FROM knowledge_points WHERE user_id=${TEST_UID} AND user_input_content='t013_md_export' ORDER BY id DESC LIMIT 1;`).stdout.trim(), 10);
    if (!mdExportId) throw new Error('构造 Markdown 测试词条失败');
    const b64 = Buffer.from(mdSample, 'utf8').toString('base64');
    runMysql(`INSERT INTO kp_extras (knowledge_id, extra_type, content, display_order) VALUES (${mdExportId}, 'markdown_content', CONVERT(FROM_BASE64('${b64}') USING utf8mb4), 0);`);

    const exp = await req('POST', `/api/kp/${mdExportId}/export-word`);
    if (exp.json && exp.json.word_path && fs.existsSync(exp.json.word_path)) {
      ok('Markdown 词条 Word 导出', `${fs.statSync(exp.json.word_path).size} bytes`);
      const xml = readDocxXml(exp.json.word_path);
      ['核心含义与例句', 't013_md_export', 't013 bold phrase', '列表第二项', '引用块内容'].forEach(t => {
        if (xml.includes(t)) ok(`精讲内容导出 ${t}`);
        else ko(`精讲内容导出 ${t}`, 'document.xml 未找到');
      });
      if (!xml.includes('**') && !xml.includes('##')) ok('无字面量 Markdown 标记');
      else ko('无字面量 Markdown 标记', '仍含 ** 或 ##');
      if (xml.includes('<w:b/>')) ok('粗体样式导出');
      else ko('粗体样式', '缺少 <w:b/>');
      if (xml.includes('w:numId')) ok('有序列表编号导出');
      else ko('列表编号', '缺少 w:numId');
      if (xml.includes('<m:oMath>')) ok('LaTeX 转为 Word 公式（OMML）', `oMath=${(xml.match(/<m:oMath>/g)||[]).length}`);
      else ko('LaTeX 公式', 'document.xml 无 <m:oMath>');
      if (!xml.includes('\\frac') && !xml.includes('\\alpha') && !xml.includes('\\rightarrow')) ok('公式源码未残留');
      else ko('公式源码残留', '仍含 \\frac/\\alpha/\\rightarrow');
      if (!xml.includes('alert(') && !xml.includes('<script')) ok('原始脚本未进入 Word');
      else ko('原始 HTML 清理', '脚本内容残留');
      if (!xml.includes('单词精讲')) ok('精讲 H1 已去重');
      else ko('精讲 H1 去重', '标题重复');
    } else ko('Markdown 词条 Word 导出', JSON.stringify(exp.json).slice(0, 200));

    // 旧格式仍在同一导出链路中正常
    const legacyId = parseInt(runMysql(`SELECT id FROM knowledge_points WHERE user_id=${TEST_UID} AND user_input_content='t010_legacy_word' ORDER BY id DESC LIMIT 1;`).stdout.trim(), 10);
    const oldExp = await req('POST', `/api/kp/${legacyId}/export-word`);
    if (oldExp.json && oldExp.json.word_path && fs.existsSync(oldExp.json.word_path)) {
      const oldXml = readDocxXml(oldExp.json.word_path);
      if (oldXml.includes('/ˌleɡəsi/') && oldXml.includes('旧格式词根内容')) ok('旧格式词条导出不变');
      else ko('旧格式词条导出', '音标或词根缺失');
    } else ko('旧格式词条导出', JSON.stringify(oldExp.json).slice(0, 200));

    // 批量导出同时覆盖新旧格式
    const batchExp = await req('POST', '/api/kp/export-batch-word');
    if (batchExp.json && batchExp.json.word_path && fs.existsSync(batchExp.json.word_path)) {
      const batchXml = readDocxXml(batchExp.json.word_path);
      if (batchXml.includes('t013_md_export') && batchXml.includes('核心含义与例句') && batchXml.includes('t010_legacy_word') && batchXml.includes('/ˌleɡəsi/')) {
        ok('批量导出覆盖新旧格式', `count=${batchExp.json.count}`);
      } else ko('批量导出覆盖新旧格式', '缺少新或旧格式内容');
    } else ko('批量导出', JSON.stringify(batchExp.json).slice(0, 200));
  } catch (e) { ko('Markdown Word 导出', e.message); }

  // ---- 7. 查重兜底 ----
  console.log('\n[7] 查重兜底（重复固化应合并）');
  try {
    // 录入重复知识点（与已固化的 standard_content 相同），固化应合并
    const r1 = await req('POST', '/api/kp', { user_input_content: 'start' });
    if (!r1.json.ok) { ko('录入重复', JSON.stringify(r1.json)); }
    else {
      const newId = r1.json.id;
      // 固化该知识点（应被合并到已固化的）
      const sol = await req('POST', `/api/kp/${newId}/solidify`);
      if (sol.json && (sol.json.ok || sol.status === 200)) {
        ok('重复固化被合并（兜底生效）');
      } else ko('重复固化', JSON.stringify(sol.json).slice(0,200));
      // 验证：原知识点（newId）应被删除
      const chk = await req('GET', `/api/kp/${newId}`);
      if (chk.status === 404) ok('合并后源记录已删除');
      else ko('合并后源记录', '仍存在 status=' + chk.status);
    }
  } catch (e) { ko('查重', e.message); }

  // ---- 8. 人工兜底（编辑/合并/拆分）----
  console.log('\n[8] 人工兜底');
  try {
    const list = await req('GET', '/api/kp?page=1&size=5');
    const idA = list.json.data[0].id;
    const idB = list.json.data[1].id;
    // 编辑 standard
    const ed = await req('PUT', `/api/kp/${idA}/standard`, { standard_content: 'start_v_edited' });
    if (ed.json && ed.json.ok) ok('人工编辑 standard_content');
    else ko('人工编辑', JSON.stringify(ed.json));
    // 拆分
    const sp = await req('POST', `/api/kp/${idA}/split`);
    if (sp.json && sp.json.new_id > 0) ok('拆分知识点', `new_id=${sp.json.new_id}`);
    else ko('拆分', JSON.stringify(sp.json));
    // 合并
    if (sp.json && sp.json.new_id) {
      const mg = await req('POST', `/api/kp/${sp.json.new_id}/merge/${idB}`);
      if (mg.json && mg.json.ok) ok('合并知识点');
      else ko('合并', JSON.stringify(mg.json));
    }
  } catch (e) { ko('人工兜底', e.message); }

  console.log('\n=== 阶段二测试结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}`);
  if (fail > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
    process.exit(1);
  } else {
    console.log('\n✓ 阶段二全部测试通过');
    process.exit(0);
  }
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
