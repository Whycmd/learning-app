// 阶段二十六：精讲公开与追问（v1.5.0-T008）
// 覆盖：POST /api/kp/check 查重、visibility 默认 private、词表升级不提前置精讲、
//       精讲正文唯一（无 sections）、public/private 的 L1 复用边界、
//       追问发起/轮询/越权、同意后并入公共精讲且幂等、is_solidified 与正文严格一致
// 仅在隔离测试环境执行（测试库）
const fs = require('fs');
const http = require('http');
const { spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT } = require('./_env');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

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
function qs(sql) {
  const out = runMysql(sql);
  return out === '' ? '' : out.split('\n')[0].trim();
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
function guestCookie(res) {
  const m = (res.headers['set-cookie'] || []).join(';').match(/eng_guest_id=([0-9a-zA-Z]+)/);
  return m ? m[1] : '';
}

async function registerUser(name) {
  await req('POST', '/api/auth/register', { username: name, password: 'pass1234', display_name: name });
  const login = await req('POST', '/api/auth/login', { username: name, password: 'pass1234' });
  const ck = sessionCookie(login);
  const me = await req('GET', '/api/auth/me', null, ck);
  return { ck, uid: (me.json && me.json.user_id) || 0 };
}

async function waitExplain(id, ck, limit = 60) {
  let last = null;
  for (let i = 0; i < limit; i++) {
    const r = await req('GET', `/api/explain/${id}`, null, ck);
    last = r;
    const st = r.json && r.json.item && r.json.item.status;
    if (st === 'done' || st === 'failed') return r;
    await new Promise(x => setTimeout(x, 300));
  }
  return last;
}

async function waitFollowup(explainId, fid, ck, limit = 60) {
  let last = null;
  for (let i = 0; i < limit; i++) {
    const r = await req('GET', `/api/explain/${explainId}/followup`, null, ck);
    last = r;
    const it = ((r.json && r.json.items) || []).find(f => Number(f.id) === Number(fid));
    if (it && (it.status === 'done' || it.status === 'failed')) return it;
    await new Promise(x => setTimeout(x, 300));
  }
  return ((last && last.json && last.json.items) || []).find(f => Number(f.id) === Number(fid)) || null;
}

async function main() {
  console.log('=== 阶段二十六：精讲公开与追问（v1.5.0-T008） ===\n');

  if (DB_NAME === 'english_learning') {
    skip('精讲公开与追问套件', `需要隔离测试库，当前 DB=${DB_NAME}`);
    console.log(`\n阶段二十六结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
    process.exit(0);
  }

  const stamp = Date.now().toString(36).slice(-6);
  const word = 'zzex' + stamp;
  try {
    const a = await registerUser('t26a_' + stamp);
    const b = await registerUser('t26b_' + stamp);

    // ---------- 1. 查重接口 ----------
    const add = await req('POST', '/api/kp', { user_input_content: word }, a.ck);
    if (add.status === 200 && add.json && add.json.id) ok('录入词条成功', `id=${add.json.id}`);
    else ko('录入词条成功', `${add.status} ${add.text.slice(0, 140)}`);

    const hit = await req('POST', '/api/kp/check', { text: word.toUpperCase() + '!' }, a.ck);
    if (hit.status === 200 && hit.json && hit.json.exists === true && Number(hit.json.id) === Number(add.json.id))
      ok('查重命中（大小写/标点归一化）', `id=${hit.json.id}`);
    else ko('查重命中（大小写/标点归一化）', `${hit.status} ${hit.text.slice(0, 160)}`);
    if (hit.json && hit.json.visibility === 'private' && Number(hit.json.is_solidified) === 0)
      ok('新建词条默认私人且待生成精讲');
    else ko('新建词条默认私人且待生成精讲', JSON.stringify(hit.json).slice(0, 160));

    const miss = await req('POST', '/api/kp/check', { text: 'zzqq' + stamp }, a.ck);
    if (miss.status === 200 && miss.json && miss.json.exists === false) ok('查重未命中');
    else ko('查重未命中', `${miss.status} ${miss.text.slice(0, 140)}`);

    // ---------- 2. 词表升级不提前标记精讲 ----------
    const bulkWord = word + 'bulk';
    await req('POST', '/api/kp/bulk', { words: [{ word: bulkWord, meaning: '批量词' }], source_ref: 1 }, a.ck);
    const bulkChk = await req('POST', '/api/kp/check', { text: bulkWord }, a.ck);
    if (bulkChk.json && bulkChk.json.exists === true && Number(bulkChk.json.is_solidified) === 0)
      ok('词表升级词条保持待生成精讲');
    else ko('词表升级词条保持待生成精讲', JSON.stringify(bulkChk.json).slice(0, 160));

    // ---------- 3. 固化后才有精讲，且正文唯一 ----------
    await req('POST', `/api/kp/${add.json.id}/solidify`, null, a.ck);
    const solidChk = await req('POST', '/api/kp/check', { text: word }, a.ck);
    if (solidChk.json && Number(solidChk.json.is_solidified) === 1) ok('精讲正文落库后标记为已生成');
    else ko('精讲正文落库后标记为已生成', JSON.stringify(solidChk.json).slice(0, 160));
    const mdCount = q1(`SELECT COUNT(*) FROM kp_extras WHERE knowledge_id=${add.json.id}
      AND extra_type='markdown_content' AND content<>''`);
    if (mdCount >= 1) ok('精讲正文写入 kp_extras', `md=${mdCount}`); else ko('精讲正文写入 kp_extras', `md=${mdCount}`);

    // ---------- 4. 讲解结果只含 summary + markdown_content ----------
    const ex1 = await req('POST', '/api/explain', { text: word }, a.ck);
    const d1 = await waitExplain(ex1.json && ex1.json.id, a.ck);
    const item1 = (d1 && d1.json && d1.json.item) || {};
    if (item1.status === 'done') ok('A 讲解完成', `id=${item1.id}`);
    else ko('A 讲解完成', JSON.stringify(item1).slice(0, 180));
    const res = item1.result || {};
    if (!('sections' in res)) ok('讲解结果不再包含 sections');
    else ko('讲解结果不再包含 sections', JSON.stringify(res).slice(0, 160));
    if (item1.content_md && item1.content_md.indexOf('# 单词精讲') === 0) ok('精讲正文以「# 单词精讲」开篇');
    else ko('精讲正文以「# 单词精讲」开篇', String(item1.content_md).slice(0, 80));

    // ---------- 5. public / private 的 L1 边界 ----------
    // 用一个未预先固化的新词，走「讲解 → 首次入池」路径
    const word2 = word + 'pb';
    const ex2 = await req('POST', '/api/explain', { text: word2 }, a.ck);
    const d2 = await waitExplain(ex2.json && ex2.json.id, a.ck);
    if (d2 && d2.json.item.status === 'done') ok('A 讲解新词完成', `id=${ex2.json.id}`);
    else ko('A 讲解新词完成', JSON.stringify(d2 && d2.json).slice(0, 160));

    const kpId = q1(`SELECT id FROM knowledge_points WHERE user_id=${a.uid} AND normalized_key='${word2}'
      ORDER BY id DESC LIMIT 1`);
    if (kpId) ok('自动入池词条存在', `kp=${kpId}`); else ko('自动入池词条存在', 'not found');
    const vis = qs(`SELECT visibility FROM knowledge_points WHERE id=${kpId}`);
    if (vis === 'public') ok('常见身份首次入池默认 public'); else ko('常见身份首次入池默认 public', `vis=${vis}`);

    const bHit = await req('POST', '/api/explain', { text: word2 }, b.ck);
    if (bHit.json && bHit.json.l1_hit === true) ok('他人可复用 public 精讲');
    else ko('他人可复用 public 精讲', `${bHit.status} ${bHit.text.slice(0, 160)}`);

    const off = await req('POST', `/api/kp/${kpId}/visibility`, { public: false }, a.ck);
    if (off.status === 200 && off.json && off.json.visibility === 'private') ok('可转私人', 'private');
    else ko('可转私人', `${off.status} ${off.text.slice(0, 140)}`);

    const bMiss = await req('POST', '/api/explain', { text: word2 }, b.ck);
    if (bMiss.status === 200 && bMiss.json && bMiss.json.l1_hit !== true) ok('private 精讲不跨用户复用');
    else ko('private 精讲不跨用户复用', `${bMiss.status} ${bMiss.text.slice(0, 160)}`);

    // ---------- 6. 追问 ----------
    const fu = await req('POST', `/api/explain/${ex2.json.id}/followup`, { question: '这个短语在写作里怎么用？' }, a.ck);
    if (fu.status === 200 && fu.json && fu.json.ok && fu.json.id) ok('发起追问成功', `id=${fu.json.id}`);
    else ko('发起追问成功', `${fu.status} ${fu.text.slice(0, 160)}`);
    const fid = fu.json && fu.json.id;
    const fdone = await waitFollowup(ex2.json.id, fid, a.ck);
    if (fdone && fdone.status === 'done' && fdone.answer_md) ok('追问生成回答', `len=${String(fdone.answer_md).length}`);
    else ko('追问生成回答', JSON.stringify(fdone).slice(0, 160));

    const otherMerge = await req('POST', `/api/explain/${ex2.json.id}/followup/${fid}/merge`, null, b.ck);
    if (otherMerge.status === 404) ok('他人不可合并追问'); else ko('他人不可合并追问', `status=${otherMerge.status}`);

    const beforeLen = q1(`SELECT CHAR_LENGTH(content) FROM kp_extras WHERE knowledge_id=${kpId} AND extra_type='markdown_content'`);
    const notMerged = q1(`SELECT merged FROM explanation_followups WHERE id=${fid}`);
    if (Number(notMerged) === 0) ok('未点按前不并入公共精讲');
    else ko('未点按前不并入公共精讲', `merged=${notMerged}`);
    const afterLen0 = q1(`SELECT CHAR_LENGTH(content) FROM kp_extras WHERE knowledge_id=${kpId} AND extra_type='markdown_content'`);
    if (beforeLen === afterLen0) ok('未合并时公共正文长度不变');
    else ko('未合并时公共正文长度不变', `${beforeLen}->${afterLen0}`);

    const m1 = await req('POST', `/api/explain/${ex2.json.id}/followup/${fid}/merge`, null, a.ck);
    if (m1.status === 200 && m1.json && m1.json.ok && Number(m1.json.kp_id) === kpId) ok('同意后并入本人词条', `kp=${m1.json.kp_id}`);
    else ko('同意后并入本人词条', `${m1.status} ${m1.text.slice(0, 160)}`);
    const mergedVis = qs(`SELECT visibility FROM knowledge_points WHERE id=${kpId}`);
    if (mergedVis === 'public') ok('并入后词条转为 public'); else ko('并入后词条转为 public', `vis=${mergedVis}`);
    const hasSection = q1(`SELECT COUNT(*) FROM kp_extras WHERE knowledge_id=${kpId}
      AND extra_type='markdown_content' AND content LIKE '%## 补充：这个短语在写作里怎么用？%'`);
    if (hasSection === 1) ok('公共正文追加「## 补充：问题」'); else ko('公共正文追加「## 补充：问题」', `count=${hasSection}`);

    const m2 = await req('POST', `/api/explain/${ex2.json.id}/followup/${fid}/merge`, null, a.ck);
    if (m2.status === 200 && m2.json && m2.json.already === true) ok('重复合并幂等（already=true）');
    else ko('重复合并幂等（already=true）', `${m2.status} ${m2.text.slice(0, 160)}`);
    const sectionCount = q1(`SELECT (CHAR_LENGTH(content)-CHAR_LENGTH(REPLACE(content,'## 补充：','')))
      /CHAR_LENGTH('## 补充：') FROM kp_extras WHERE knowledge_id=${kpId} AND extra_type='markdown_content'`);
    if (Number(sectionCount) === 1) ok('重复合并不重复追加'); else ko('重复合并不重复追加', `count=${sectionCount}`);

    // ---------- 7. 游客追问可问不可并 ----------
    const g = await req('POST', '/api/explain', { text: 'zzgu' + stamp });
    const gCk = g.json && g.json.id ? 'eng_guest_id=' + guestCookie(g) : '';
    const gDone = await waitExplain(g.json && g.json.id, gCk);
    if (gDone && gDone.json.item.status === 'done') {
      const gFu = await req('POST', `/api/explain/${g.json.id}/followup`, { question: '怎么记？' }, gCk);
      if (gFu.status === 200 && gFu.json && gFu.json.ok) ok('游客可发起追问');
      else ko('游客可发起追问', `${gFu.status} ${gFu.text.slice(0, 140)}`);
      const gMerge = await req('POST', `/api/explain/${g.json.id}/followup/${gFu.json.id}/merge`, null, gCk);
      if (gMerge.status === 401) ok('游客不可并入公共精讲'); else ko('游客不可并入公共精讲', `status=${gMerge.status}`);
    } else ko('游客讲解完成', JSON.stringify(gDone && gDone.json).slice(0, 160));

    // ---------- 8. is_solidified 与正文严格一致（仅本套件用户） ----------
    const mismatch = q1(`SELECT COUNT(*) FROM knowledge_points kp WHERE kp.user_id IN (${a.uid},${b.uid})
      AND kp.is_solidified=1 AND NOT EXISTS(
      SELECT 1 FROM kp_extras e WHERE e.knowledge_id=kp.id AND e.extra_type='markdown_content' AND e.content<>'')`);
    if (mismatch === 0) ok('is_solidified=1 必有精讲正文'); else ko('is_solidified=1 必有精讲正文', `mismatch=${mismatch}`);
  } catch (e) {
    ko('套件异常', e.message);
  }

  console.log(`\n阶段二十六结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
  if (fail) {
    console.log('失败项：');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.msg || ''}`));
    process.exit(1);
  }
}

main();
