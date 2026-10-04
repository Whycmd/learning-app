// 阶段二：知识点录入 & AI 固化模块 - 集成测试
// 覆盖验收标准：录入、状态区分、字段落库、AI 固化拓展、Word 导出、掌握程度判定、查重兜底、人工兜底
const http = require('http');
const fs = require('fs');
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
function ok(name, msg='') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({name, msg}); console.log(`  ✗ ${name} — ${msg}`); }

function req(method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type':'application/json' };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    const r = http.request({ host:'127.0.0.1', port:8088, method, path:urlPath,
      headers, timeout: 130000 }, res => {
      let chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        try { resolve({ status:res.statusCode, json:JSON.parse(text), text }); }
        catch { resolve({ status:res.statusCode, json:null, text }); }
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

async function main() {
  console.log('=== 阶段二：知识点录入 & AI 固化集成测试 ===\n');

  // ---- 0. 前置：清空测试数据 ----
  console.log('[0] 清理测试数据');
  try {
    runMysql("DELETE FROM kp_extras; DELETE FROM kp_antonyms; DELETE FROM kp_synonyms; DELETE FROM kp_collocations; DELETE FROM kp_definitions; DELETE FROM user_learning; DELETE FROM knowledge_points;");
    ok('清理数据');
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
    const cnt = runMysql("SELECT COUNT(*) FROM knowledge_points WHERE user_input_content='start';").stdout.trim();
    if (cnt === '2') ok('字段 user_input_content 落库');
    else ko('字段落库', 'start 出现次数=' + cnt);

    // 默认未固化
    const unsol = runMysql("SELECT COUNT(*) FROM knowledge_points WHERE is_solidified=0;").stdout.trim();
    if (parseInt(unsol) >= 3) ok('默认未固化（is_solidified=0）');
    else ko('默认未固化', 'count=' + unsol);

    // create_time 自动生成
    const ct = runMysql("SELECT create_time FROM knowledge_points ORDER BY id DESC LIMIT 1;").stdout.trim();
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
      } else ko('固化状态', JSON.stringify(d.json).slice(0,200));
    }
  } catch (e) { ko('字段校验', e.message); }

  // ---- 5. 掌握程度判定规则 ----
  console.log('\n[5] 掌握程度判定');
  try {
    // 判定规则：难度 > 雅思(5) = 会读，否则会写
    // 检查固化后所有知识点的 master_require
    const rs = runMysql("SELECT difficulty, master_require FROM knowledge_points WHERE is_solidified=1;").stdout.trim().split('\n').filter(l=>l.trim());
    let ok1 = true;
    for (const line of rs) {
      const [diff, mr] = line.split('\t');
      const diffNum = parseInt(diff || '0', 10);
      const expected = diffNum > 5 ? '会读' : '会写';
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
