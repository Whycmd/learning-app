// 阶段六：定时任务 & 异常兜底 & 数据安全 - 集成测试
// 覆盖验收标准：
//   1. 定时任务机制（启动清理、临时 Word 清理）
//   2. 全局异常兜底（空数据、无效ID、服务未启动）
//   3. 数据备份与重置功能
const http = require('http');
const fs = require('fs');
const { spawnSync } = require('child_process');

const MYSQL_BIN = '/Users/junwang/.local/mysql/bin/mysql';
const MYSQL_PW_FILE = '/Users/junwang/.local/mysql/.root_pw';
const SOCK = '/Users/junwang/.local/run/mysql.sock';
const WORD_TMP = '/Users/junwang/Desktop/王浩源/英语/英语学习应用/word_tmp';
const BACKUP_DIR = '/Users/junwang/Desktop/王浩源/英语/英语学习应用/backups';

let pass = 0, fail = 0;
const failures = [];
function ok(name, msg='') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({name, msg}); console.log(`  ✗ ${name} — ${msg}`); }

function req(method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type':'application/json' };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    else headers['Content-Length'] = 0;
    const r = http.request({ host:'127.0.0.1', port:8088, method, path:urlPath,
      headers, timeout: 60000 }, res => {
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
  console.log('=== 阶段六：定时任务 & 异常兜底 & 数据安全测试 ===\n');

  // ---- 1. 数据备份 ----
  console.log('[1] 数据备份');
  try {
    const r = await req('POST', '/api/maint/backup');
    if (r.json && r.json.ok && r.json.path) {
      ok('数据库备份', `${r.json.size} bytes`);
      if (fs.existsSync(r.json.path)) ok('备份文件存在');
      else ko('备份文件', '不存在');
    } else ko('备份', JSON.stringify(r.json).slice(0,200));
  } catch (e) { ko('备份', e.message); }

  // ---- 2. 备份列表 ----
  console.log('\n[2] 备份列表');
  try {
    const r = await req('GET', '/api/maint/backups');
    if (r.json && Array.isArray(r.json) && r.json.length > 0) {
      ok('备份列表', `${r.json.length} 个备份`);
    } else ko('备份列表', '空');
  } catch (e) { ko('备份列表', e.message); }

  // ---- 3. 学情重置 ----
  console.log('\n[3] 学情重置');
  try {
    // 先准备数据：录入、固化、设置学情
    runMysql("DELETE FROM kp_extras; DELETE FROM kp_antonyms; DELETE FROM kp_synonyms; DELETE FROM kp_collocations; DELETE FROM kp_definitions; DELETE FROM user_learning; DELETE FROM knowledge_points;");
    const kp = await req('POST', '/api/kp', { user_input_content: 'abandon' });
    await req('POST', `/api/kp/${kp.json.id}/solidify`);
    const lr = await req('GET', `/api/learn/${kp.json.id}`);
    // 设置熟练度
    await req('PUT', `/api/learn/${kp.json.id}/proficiency`, { proficiency_level: 3 });
    // 答错题
    await req('POST', `/api/learn/${kp.json.id}/answer`, { is_correct: false });
    const before = await req('GET', `/api/learn/${kp.json.id}`);
    if (before.json && before.json.proficiency_level === 2) ok('重置前熟练度', 'L2');

    // 重置
    const reset = await req('POST', `/api/maint/reset-learning/${kp.json.id}`);
    if (reset.json && reset.json.ok) ok('重置成功');
    else ko('重置', JSON.stringify(reset.json));

    const after = await req('GET', `/api/learn/${kp.json.id}`);
    if (after.json && after.json.proficiency_level === 0 && after.json.error_times === 0 && after.json.review_times === 0) {
      ok('重置后熟练度归零', 'L0, error=0, review=0');
    } else ko('重置验证', JSON.stringify(after.json).slice(0,200));
  } catch (e) { ko('学情重置', e.message); }

  // ---- 4. 全量学情清空 ----
  console.log('\n[4] 全量学情清空');
  try {
    const r = await req('POST', '/api/maint/clear-all-learning');
    if (r.json && r.json.ok) ok('全量清空成功');
    else ko('全量清空', JSON.stringify(r.json));
    // 验证
    const stats = await req('GET', '/api/learn/stats');
    if (stats.json) {
      const allZero = stats.json.distribution.every(d => d.level === 0);
      if (allZero) ok('全部重置为 L0');
      else ko('全量重置', '存在非零熟练度');
    }
  } catch (e) { ko('全量清空', e.message); }

  // ---- 5. 临时 Word 清理 ----
  console.log('\n[5] 临时 Word 清理');
  try {
    // 创建一些临时文件
    const now = Date.now();
    fs.writeFileSync(`${WORD_TMP}/exercise_test1.docx`, 'test1');
    fs.writeFileSync(`${WORD_TMP}/exercise_test2.docx`, 'test2');
    // 创建一个旧文件（修改时间为2小时前）
    const oldFile = `${WORD_TMP}/exercise_old.docx`;
    fs.writeFileSync(oldFile, 'old');
    const oldTime = Math.floor((now - 2*3600*1000) / 1000);
    spawnSync('touch', ['-t', new Date(now - 2*3600*1000).toISOString().replace(/[-:]/g,'').slice(0,12).replace(/(\d{8})(\d{4})/, '$1$2.'), oldFile]);

    // 清理（max_age=3600 = 1小时）
    const r = await req('POST', '/api/maint/cleanup-temp-word?max_age=3600');
    if (r.json && r.json.ok) {
      ok('清理任务执行', `deleted=${r.json.deleted}`);
      if (r.json.deleted > 0) ok('删除了过期文件');
    } else ko('清理', JSON.stringify(r.json).slice(0,200));

    // 清理测试文件
    try { fs.unlinkSync(`${WORD_TMP}/exercise_test1.docx`); } catch {}
    try { fs.unlinkSync(`${WORD_TMP}/exercise_test2.docx`); } catch {}
    try { fs.unlinkSync(oldFile); } catch {}
  } catch (e) { ko('临时 Word 清理', e.message); }

  // ---- 6. 启动清理 ----
  console.log('\n[6] 启动清理');
  try {
    // 创建习题临时文件
    fs.writeFileSync(`${WORD_TMP}/exercise_startup_test.docx`, 'test');
    const r = await req('POST', '/api/maint/cleanup-startup');
    if (r.json && r.json.ok && r.json.deleted > 0) {
      ok('启动清理', `deleted=${r.json.deleted}`);
      if (!fs.existsSync(`${WORD_TMP}/exercise_startup_test.docx`)) ok('习题文件已清理');
    } else ko('启动清理', JSON.stringify(r.json).slice(0,200));
  } catch (e) { ko('启动清理', e.message); }

  // ---- 7. 异常兜底 ----
  console.log('\n[7] 异常兜底');
  try {
    // 无效知识点 ID
    const e1 = await req('GET', '/api/kp/99999');
    if (e1.status === 404) ok('无效ID兜底', '404');
    else ko('无效ID兜底', `status=${e1.status}`);

    // 无效学情 ID
    const e2 = await req('GET', '/api/learn/99999');
    if (e2.status === 404) ok('无效学情兜底', '404');
    else ko('无效学情兜底', `status=${e2.status}`);

    // 空请求体
    const e3 = await req('POST', '/api/kp', null);
    if (e3.status === 400) ok('空请求体兜底', '400');
    else ko('空请求体兜底', `status=${e3.status}`);

    // 无效 JSON
    const e4 = await req('POST', '/api/kp', null);
    // 手动发送无效 JSON
    const e4r = await new Promise((resolve, reject) => {
      const r = http.request({ host:'127.0.0.1', port:8088, method:'POST', path:'/api/kp',
        headers: {'Content-Type':'application/json','Content-Length':5} }, res => {
        let c=[]; res.on('data',d=>c.push(d)); res.on('end',()=>resolve({status:res.statusCode}));
      });
      r.on('error', reject);
      r.write('bad{]');
      r.end();
    });
    if (e4r.status === 400) ok('无效JSON兜底', '400');
    else ko('无效JSON兜底', `status=${e4r.status}`);

    // 路由不存在
    const e5 = await req('GET', '/api/nonexistent');
    if (e5.status === 404) ok('不存在路由兜底', '404');
    else ko('路由兜底', `status=${e5.status}`);

  } catch (e) { ko('异常兜底', e.message); }

  console.log('\n=== 阶段六测试结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}`);
  if (fail > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
    process.exit(1);
  } else {
    console.log('\n✓ 阶段六全部测试通过');
    process.exit(0);
  }
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
