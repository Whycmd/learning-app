// 阶段七：单词本系统 - 集成测试
// v1.2.0-T003：覆盖单词本 CRUD、词条管理、导入导出
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
const createdWordbookIds = [];

function ok(name, msg='') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({name, msg}); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg='') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function runMysql(sql, opts={}) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const db = opts.db || 'english_learning';
  const silentFlag = opts.silent ? ['-s', '--skip-column-names'] : [];
  const args = ['--socket='+SOCK, '-uroot', '-p'+pw, ...silentFlag, db, '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  return r.stdout;
}

function api(method, path, body) {
  return new Promise((resolve, reject) => {
    const u = new URL(path, API);
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json' };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    else headers['Content-Length'] = 0;
    const req = http.request({
      host: u.hostname, port: u.port, method, path: u.pathname + u.search,
      headers,
      timeout: 30000
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
    req.on('timeout', () => { req.destroy(new Error('timeout')); });
    if (data) req.write(data);
    req.end();
  });
}

async function cleanup() {
  // 清理测试创建的单词本
  if (createdWordbookIds.length) {
    const ids = createdWordbookIds.join(',');
    try {
      runMysql(`DELETE FROM wordbook_entry WHERE wordbook_id IN (${ids});`);
      runMysql(`DELETE FROM wordbook WHERE id IN (${ids});`);
    } catch(e) {}
  }
}

async function main() {
  console.log('=== 阶段七：单词本系统（v1.2.0-T003）集成测试 ===\n');

  // ---- 0. 健康检查 ----
  console.log('[0] 前置检查');
  try {
    const r = await api('GET', '/api/health');
    if (r.json && r.json.status === 'ok') ok('后端健康检查');
    else ko('后端健康检查', r.text);
  } catch(e) { ko('后端健康检查', e.message); }

  // 表结构检查
  try {
    runMysql('SELECT COUNT(*) FROM wordbook;');
    ok('wordbook 表存在');
  } catch(e) { ko('wordbook 表存在', e.message); }
  try {
    runMysql('SELECT COUNT(*) FROM wordbook_entry;');
    ok('wordbook_entry 表存在');
  } catch(e) { ko('wordbook_entry 表存在', e.message); }

  // ---- 1. CRUD ----
  console.log('\n[1] 单词本 CRUD');

  // 1.1 空列表
  const list0 = await api('GET', '/api/wordbook');
  if (list0.json && list0.json.data && Array.isArray(list0.json.data)) ok('列表返回数组');
  else ko('列表返回数组', list0.text);

  // 1.2 创建
  const create1 = await api('POST', '/api/wordbook', { name: '测试单词本_A', description: 'unit test A' });
  if (create1.json && create1.json.ok && create1.json.id > 0) {
    ok('创建单词本 A', `id=${create1.json.id}`);
    createdWordbookIds.push(create1.json.id);
  } else ko('创建单词本 A', create1.text);

  const create2 = await api('POST', '/api/wordbook', { name: '测试单词本_B' });
  if (create2.json && create2.json.ok && create2.json.id > 0) {
    ok('创建单词本 B（无描述）', `id=${create2.json.id}`);
    createdWordbookIds.push(create2.json.id);
  } else ko('创建单词本 B', create2.text);

  // 1.3 空名称校验
  const createFail = await api('POST', '/api/wordbook', { name: '' });
  if (createFail.status === 400 || (createFail.json && createFail.json.error)) ok('空名称被拒绝');
  else ko('空名称被拒绝', createFail.text);

  // 1.4 列表更新
  const list1 = await api('GET', '/api/wordbook');
  if (list1.json && list1.json.data.length >= 2) ok('列表包含新建单词本');
  else ko('列表包含新建单词本', `count=${list1.json?.data?.length}`);

  // 1.5 详情
  const detail1 = await api('GET', `/api/wordbook/${create1.json.id}`);
  if (detail1.json && detail1.json.name === '测试单词本_A' && detail1.json.description === 'unit test A')
    ok('详情返回正确');
  else ko('详情返回正确', detail1.text);

  // 1.6 更新
  const update1 = await api('PUT', `/api/wordbook/${create1.json.id}`, { name: '测试单词本_A_改', is_public: 1 });
  if (update1.json && update1.json.ok) ok('更新单词本');
  else ko('更新单词本', update1.text);

  const detail1b = await api('GET', `/api/wordbook/${create1.json.id}`);
  if (detail1b.json && detail1b.json.name === '测试单词本_A_改' && detail1b.json.is_public === 1)
    ok('更新后详情一致');
  else ko('更新后详情一致', detail1b.text);

  // 1.7 不存在的 id
  const detailNf = await api('GET', '/api/wordbook/99999');
  if (detailNf.status === 404) ok('不存在的 id 返回 404');
  else ko('不存在的 id 返回 404', detailNf.text);

  // ---- 2. 词条管理 ----
  console.log('\n[2] 词条管理');

  // 2.1 先录入几个知识点
  const kpIds = [];
  const words = ['unittestapple', 'unittestbanana', 'unittestcherry'];
  for (const w of words) {
    const r = await api('POST', '/api/kp', { user_input_content: w });
    if (r.json && r.json.ok) kpIds.push(r.json.id);
  }
  if (kpIds.length === 3) ok('前置知识点录入 3 个');
  else ko('前置知识点录入', `got ${kpIds.length}`);

  // 2.2 添加词条（knowledge_ids 模式）
  const add1 = await api('POST', `/api/wordbook/${create1.json.id}/entries`, { knowledge_ids: kpIds });
  if (add1.json && add1.json.ok && add1.json.added === 3) ok('添加词条 knowledge_ids 模式', `added=3`);
  else ko('添加词条 knowledge_ids 模式', add1.text);

  // 2.3 entry_count 同步
  const detail1c = await api('GET', `/api/wordbook/${create1.json.id}`);
  if (detail1c.json && detail1c.json.entry_count === 3) ok('entry_count 冗余字段同步');
  else ko('entry_count 冗余字段同步', detail1c.text);

  // 2.4 重复添加（应该跳过）
  const addDup = await api('POST', `/api/wordbook/${create1.json.id}/entries`, { knowledge_ids: kpIds });
  if (addDup.json && addDup.json.skipped === 3) ok('重复添加被跳过');
  else ko('重复添加被跳过', addDup.text);

  // 2.5 词条列表
  const entries1 = await api('GET', `/api/wordbook/${create1.json.id}/entries`);
  if (entries1.json && entries1.json.data.length === 3) ok('词条列表返回 3 条');
  else ko('词条列表返回 3 条', entries1.text);

  // 2.6 搜索过滤
  const entriesF = await api('GET', `/api/wordbook/${create1.json.id}/entries?search=apple`);
  if (entriesF.json && entriesF.json.data.length === 1) ok('搜索过滤命中 1 条');
  else ko('搜索过滤命中 1 条', entriesF.text);

  // 2.7 new_words 模式
  const add2 = await api('POST', `/api/wordbook/${create1.json.id}/entries`, { new_words: ['unittestdog', 'unittestcat'] });
  if (add2.json && add2.json.ok && add2.json.added === 2) ok('new_words 模式', `added=2`);
  else ko('new_words 模式', add2.text);

  // 2.8 移除词条
  const entries2 = await api('GET', `/api/wordbook/${create1.json.id}/entries`);
  const entryId = entries2.json?.data?.[0]?.entry_id;
  if (entryId) {
    const rm = await api('DELETE', `/api/wordbook/entries/${entryId}`);
    if (rm.json && rm.json.ok) ok('移除词条');
    else ko('移除词条', rm.text);

    const entries3 = await api('GET', `/api/wordbook/${create1.json.id}/entries`);
    if (entries3.json && entries3.json.data.length === 4) ok('移除后剩余 4 条');
    else ko('移除后剩余 4 条', entries3.text);
  } else {
    skip('移除词条', '无 entry_id');
  }

  // ---- 3. 导入导出 ----
  console.log('\n[3] 导入导出');

  // 3.1 导出 JSON
  const expJson = await api('POST', `/api/wordbook/${create1.json.id}/export-json`);
  if (expJson.json && expJson.json.ok && expJson.json.json_path) ok('导出 JSON', expJson.json.json_path);
  else ko('导出 JSON', expJson.text);

  // 3.2 导出的文件可访问
  if (expJson.json?.json_path) {
    try {
      const content = fs.readFileSync(expJson.json.json_path, 'utf8');
      const parsed = JSON.parse(content);
      if (parsed.version === '1.0' && Array.isArray(parsed.entries))
        ok('导出 JSON 文件格式正确', `entries=${parsed.entries.length}`);
      else ko('导出 JSON 文件格式正确', content.slice(0, 100));
    } catch(e) { ko('导出 JSON 文件格式正确', e.message); }
  }

  // 3.3 导出后有 share_code
  if (expJson.json?.json_path) {
    try {
      const content = fs.readFileSync(expJson.json.json_path, 'utf8');
      const parsed = JSON.parse(content);
      if (parsed.share_code && parsed.share_code.length === 6) ok('share_code 生成', parsed.share_code);
      else ko('share_code 生成', parsed.share_code);
    } catch(e) {}
  }

  // 3.4 导入（从文件路径）
  if (expJson.json?.json_path) {
    const imp = await api('POST', '/api/wordbook/import', { file_path: expJson.json.json_path });
    if (imp.json && imp.json.ok && imp.json.wordbook_id > 0) {
      ok('导入成功', `wordbook_id=${imp.json.wordbook_id}, imported=${imp.json.imported}, merged=${imp.json.merged}`);
      createdWordbookIds.push(imp.json.wordbook_id);
    } else ko('导入成功', imp.text);
  } else {
    skip('导入（从文件路径）', '无导出文件');
  }

  // 3.5 导出 Word（需要已固化词条在单词本里）
  let solidifiedKpId = null;
  for (const kid of kpIds) {
    try {
      const solidify = await api('POST', `/api/kp/${kid}/solidify`);
      // 固化可能合并到已有词条：接口返回 effective id，原始 kid 可能已被删除
      if (solidify.json?.ok) { solidifiedKpId = solidify.json.id || kid; break; }
    } catch(e) {}
  }

  if (solidifiedKpId) {
    // 确保单词本里有这个已固化词条
    await api('POST', `/api/wordbook/${create1.json.id}/entries`, { knowledge_ids: [solidifiedKpId] });
    const expWd = await api('POST', `/api/wordbook/${create1.json.id}/export-word`);
    if (expWd.json && expWd.json.ok && expWd.json.word_path) ok('导出 Word');
    else ko('导出 Word', expWd.json?.error || expWd.text);
  } else {
    skip('导出 Word', 'AI 固化未完成（跳过）');
  }

  // ---- 4. 删除 ----
  console.log('\n[4] 删除单词本');

  const del = await api('DELETE', `/api/wordbook/${create2.json.id}`);
  if (del.json && del.json.ok) ok('删除单词本 B');
  else ko('删除单词本 B', del.text);

  const delDetail = await api('GET', `/api/wordbook/${create2.json.id}`);
  if (delDetail.status === 404) ok('删除后详情 404');
  else ko('删除后详情 404', delDetail.text);

  // ---- 汇总 ----
  console.log(`\n=== 结果：${pass} 通过 / ${fail} 失败 / ${skipped} 跳过 ===`);
  if (failures.length) {
    console.log('\n失败项：');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
  }

  // 清理
  await cleanup();

  process.exit(fail === 0 ? 0 : 1);
}

main().catch(e => { console.error(e); process.exit(1); });
