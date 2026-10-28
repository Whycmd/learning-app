// v1.4.0-T001：新词条固化后丢失/不显示 修复专项测试
// 覆盖：分页参数 size 生效、合并场景返回有效 ID、导出 Word 路径参数化
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const MYSQL_BIN = '/Users/junwang/.local/mysql/bin/mysql';
const MYSQL_PW_FILE = '/Users/junwang/.local/mysql/.root_pw';
const SOCK = '/Users/junwang/.local/run/mysql.sock';
const MARKER_WORD = 'serendipity';
const MERGE_WORD = 'merge_probe_word';

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function req(method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json' };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    const r = http.request({ host: '127.0.0.1', port: 8088, method, path: urlPath,
      headers, timeout: 130000 }, res => {
      let chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        try { resolve({ status: res.statusCode, json: JSON.parse(text), text }); }
        catch { resolve({ status: res.statusCode, json: null, text }); }
      });
    });
    r.on('error', reject);
    r.on('timeout', () => r.destroy(new Error('timeout')));
    if (data) r.write(data);
    r.end();
  });
}

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const args = ['--socket=' + SOCK, '-uroot', '-p' + pw, '-s', '--skip-column-names', 'english_learning', '-e', sql];
  return spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
}

const OWN_WORDS = "user_input_content LIKE 'seed_word_%' OR user_input_content IN ('serendipity','merge_probe_word')";
const OWN_IDS = "(SELECT id FROM knowledge_points WHERE " + OWN_WORDS + ")";

// 只清理本套件数据，避免误删用户词条
function cleanup() {
  for (const t of ['kp_definitions', 'kp_collocations', 'kp_synonyms', 'kp_antonyms', 'kp_extras'])
    runMysql(`DELETE FROM ${t} WHERE knowledge_id IN ${OWN_IDS};`);
  runMysql(`DELETE FROM user_learning WHERE knowledge_id IN ${OWN_IDS};`);
  runMysql(`DELETE FROM knowledge_points WHERE ${OWN_WORDS};`);
  runMysql("DELETE FROM ai_tasks WHERE task_type='solidify_kp' AND ref_id NOT IN (SELECT id FROM knowledge_points);");
}

async function main() {
  console.log('=== v1.4.0-T001：新词条固化修复专项测试 ===\n');

  // ---- 0. 清理 + 构造 25 条已固化数据（制造超过默认分页 20 条的场景）----
  console.log('[0] 清理并构造基线数据');
  try {
    cleanup();
    const values = [];
    for (let i = 1; i <= 25; i++) {
      const w = 'seed_word_' + String(i).padStart(2, '0');
      values.push(`(1,'${w}','${w}',2,'会写',NULL,1,NOW(),NOW())`);
    }
    const ins = runMysql("INSERT INTO knowledge_points (user_id, user_input_content, standard_content, "
      + "difficulty, master_require, remark, is_solidified, create_time, solidify_time) VALUES "
      + values.join(',') + ";");
    if (ins.status !== 0) throw new Error(ins.stderr.trim());
    const cnt = runMysql("SELECT COUNT(*) FROM knowledge_points WHERE user_input_content LIKE 'seed_word_%';").stdout.trim();
    if (cnt === '25') ok('构造 25 条已固化知识点', `count=${cnt}`);
    else ko('构造基线数据', 'count=' + cnt);
  } catch (e) { ko('构造基线数据', e.message); }

  // ---- 1. 新建词条 ----
  console.log('\n[1] 新建词条');
  let newId = 0;
  try {
    const r = await req('POST', '/api/kp', { user_input_content: MARKER_WORD });
    if (r.status === 200 || r.status === 201) {
      if (r.json && r.json.ok && r.json.id > 0) { newId = r.json.id; ok('新建词条', `${MARKER_WORD} id=${newId}`); }
      else ko('新建词条', JSON.stringify(r.json));
    } else ko('新建词条', 'status=' + r.status);
  } catch (e) { ko('新建词条', e.message); }

  // ---- 2. T13-1：分页参数 size 生效 ----
  console.log('\n[2] T13-1 分页参数 size 生效（默认 20 vs size=1000）');
  try {
    const def = await req('GET', '/api/kp');
    if (def.json && Array.isArray(def.json.data) && def.json.data.length === 20) {
      ok('默认分页返回 20 条');
      if (def.json.data.every(k => k.user_input_content !== 'seed_word_01')) ok('默认分页不含最早词条（seed_word_01）');
      else ko('默认分页范围', '意外包含 seed_word_01');
    } else ko('默认分页', JSON.stringify(def.json).slice(0, 200));

    const all = await req('GET', '/api/kp?size=1000');
    const data = (all.json && Array.isArray(all.json.data)) ? all.json.data : [];
    if (data.length >= 26) ok('size=1000 返回全部词条', `count=${data.length}`);
    else ko('size=1000 返回数量', 'count=' + data.length + '（期望 >= 26）');

    const contents = data.map(k => k.user_input_content);
    if (contents.includes('seed_word_01')) ok('size=1000 覆盖最早词条');
    else ko('size=1000 覆盖范围', '缺少 seed_word_01');
    if (newId && data.some(k => k.id === newId)) ok('新建词条在列表中可见');
    else ko('新建词条可见', 'id=' + newId + ' 不在列表中');
  } catch (e) { ko('分页校验', e.message); }

  // ---- 3. T13-1：新建词条固化后列表可见 ----
  console.log('\n[3] T13-1 新建词条固化后列表可见');
  let markerSolidified = false;
  try {
    const sol = await req('POST', `/api/kp/${newId}/solidify`);
    if (sol.status === 200 && sol.json && sol.json.ok) {
      ok('固化成功');
      if (sol.json.id === newId) ok('返回固化前 ID（未合并）', `id=${sol.json.id}`);
      else ko('返回 ID', `期望 ${newId}，实际 ${sol.json.id}`);

      const d = await req('GET', `/api/kp/${newId}`);
      if (d.status === 200 && d.json && d.json.is_solidified === 1) { ok('详情可加载且已固化'); markerSolidified = true; }
      else ko('详情加载', 'status=' + d.status + ' ' + JSON.stringify(d.json).slice(0, 160));
    } else ko('固化成功', 'status=' + sol.status + ' ' + JSON.stringify(sol.json).slice(0, 200));
  } catch (e) { ko('固化', e.message); }

  try {
    if (markerSolidified) {
      const all = await req('GET', '/api/kp?size=1000');
      const row = ((all.json && all.json.data) || []).find(k => k.id === newId);
      if (row && row.is_solidified === 1) ok('固化后刷新列表仍可见');
      else ko('固化后列表可见', 'id=' + newId + ' 缺失或状态不符');
    } else skip('固化后列表可见', '前置固化未成功');
  } catch (e) { ko('固化后列表校验', e.message); }

  // ---- 4. T13-2：合并场景返回有效 ID ----
  console.log('\n[4] T13-2 重复词条固化合并');
  let mergeTargetId = 0;
  try {
    const ins = runMysql(`INSERT INTO knowledge_points (user_id, user_input_content, standard_content, `
      + `difficulty, master_require, remark, is_solidified, create_time, solidify_time) VALUES `
      + `(1,'${MERGE_WORD}','${MERGE_WORD}',2,'会写',NULL,1,NOW(),NOW());`);
    if (ins.status !== 0) throw new Error(ins.stderr.trim());
    mergeTargetId = parseInt(runMysql(`SELECT id FROM knowledge_points WHERE user_input_content='${MERGE_WORD}' ORDER BY id DESC LIMIT 1;`).stdout.trim(), 10);
    if (mergeTargetId > 0) ok('构造已固化同名词条', `id=${mergeTargetId}`);
    else ko('构造已固化同名词条', 'id=0');

    const created = await req('POST', '/api/kp', { user_input_content: MERGE_WORD });
    if (!created.json || !created.json.ok) { ko('录入手动重复词条', JSON.stringify(created.json)); }
    else {
      const dupId = created.json.id;
      ok('录入手动重复词条', `id=${dupId}`);
      const sol = await req('POST', `/api/kp/${dupId}/solidify`);
      if (sol.status === 200 && sol.json && sol.json.ok) {
        ok('重复固化不报错');
        if (sol.json.id === mergeTargetId) ok('返回合并后的有效 ID', `id=${sol.json.id}`);
        else ko('合并有效 ID', `期望 ${mergeTargetId}，实际 ${sol.json.id}`);

        const d = await req('GET', `/api/kp/${mergeTargetId}`);
        if (d.status === 200 && d.json && d.json.id === mergeTargetId && d.json.is_solidified === 1) {
          ok('合并后详情页可正常加载');
        } else ko('合并后详情', 'status=' + d.status + ' ' + JSON.stringify(d.json).slice(0, 160));

        const gone = await req('GET', `/api/kp/${dupId}`);
        if (gone.status === 404) ok('重复源记录已合并删除');
        else ko('重复源记录', '仍存在 status=' + gone.status);
      } else ko('重复固化', 'status=' + sol.status + ' ' + JSON.stringify(sol.json).slice(0, 200));
    }
  } catch (e) { ko('合并场景', e.message); }

  // ---- 5. T13-3：导出 Word 路径参数化 ----
  console.log('\n[5] T13-3 导出 Word 路径参数化');
  try {
    const scriptOk = fs.existsSync(path.join(ROOT, 'scripts', 'gen_word_kp.js'));
    if (scriptOk) ok('Word 生成脚本路径可解析', path.join('scripts', 'gen_word_kp.js'));
    else ko('Word 生成脚本路径', 'scripts/gen_word_kp.js 不存在');

    if (markerSolidified) {
      const exp = await req('POST', `/api/kp/${newId}/export-word`);
      if (exp.status === 200 && exp.json && exp.json.word_path && fs.existsSync(exp.json.word_path)) {
        const size = fs.statSync(exp.json.word_path).size;
        if (size > 1000) ok('导出 Word 成功', `${exp.json.word_path} (${size} bytes)`);
        else ko('导出 Word 文件', 'size=' + size);
      } else ko('导出 Word', 'status=' + exp.status + ' ' + JSON.stringify(exp.json).slice(0, 200));
    } else skip('导出 Word 成功', '前置固化未成功');
  } catch (e) { ko('导出 Word', e.message); }

  // ---- 6. 清理 ----
  console.log('\n[6] 清理测试数据');
  try { cleanup(); ok('清理数据'); } catch (e) { ko('清理数据', e.message); }

  console.log('\n=== stage13 测试结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}  跳过: ${skipped}`);
  if (fail > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
    process.exit(1);
  }
  console.log('\n✓ stage13 全部测试通过');
  process.exit(0);
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
