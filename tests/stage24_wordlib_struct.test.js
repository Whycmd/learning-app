// 阶段二十四：单词本体系重构（v1.5.0-T005）
// 覆盖：组合成员 CRUD 与 uk_child 去重、递归展开（单层/三层/混排/去重）、
//       防环与限深、计数重算、教材词升级知识点、加入学习开关、30 条阈值、
//       学习汇总与正在学习分页、注销级联清理组合成员
// 仅在隔离测试环境执行（测试库）
const fs = require('fs');
const http = require('http');
const path = require('path');
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

function request(method, urlPath, body, cookie) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    if (cookie) headers['Cookie'] = 'eng_session=' + cookie;
    const req = http.request({ host: API_HOST, port: API_PORT, method, path: urlPath, headers, timeout: 120000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(text); } catch (_) {}
        resolve({ status: res.statusCode, json, text, headers: res.headers });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

function api(method, urlPath, body, cookie) { return request(method, urlPath, body, cookie); }

function cookieOf(res) {
  const m = (res.headers['set-cookie'] || []).join(';').match(/eng_session=([0-9a-f]+)/);
  return m && m[1];
}

async function registerUser(name) {
  await api('POST', '/api/auth/register', { username: name, password: 'pass1234', display_name: name });
  const login = await api('POST', '/api/auth/login', { username: name, password: 'pass1234' });
  const ck = cookieOf(login);
  const me = await api('GET', '/api/auth/me', null, ck);
  return { ck, uid: (me.json && me.json.user_id) || 0 };
}

async function createBook(title, words, ck) {
  const r = await api('POST', '/api/wordlists', {
    title,
    items: words.map(w => ({ word: w, meaning: w + '_释义', pos: 'n.' }))
  }, ck);
  return (r.json && r.json.id) || 0;
}

async function itemIdOf(bookId, word, ck) {
  const r = await api('GET', `/api/wordlists/${bookId}/items?size=200`, null, ck);
  const hit = ((r.json && r.json.data) || []).find(i => String(i.word).toLowerCase() === word.toLowerCase());
  return hit ? hit.item_id : 0;
}

async function childrenOf(comboId, ck) {
  const r = await api('GET', `/api/wordlists/${comboId}/children`, null, ck);
  return (r.json && r.json.data) || [];
}

async function detailOf(id, ck) {
  const r = await api('GET', `/api/wordlists/${id}`, null, ck);
  return r.json || null;
}

async function expandOf(id, ck) {
  const r = await api('POST', `/api/wordlists/${id}/expand`, {}, ck);
  return { status: r.status, json: r.json || {}, text: r.text };
}

async function main() {
  console.log('=== 阶段二十四：单词本体系重构（v1.5.0-T005） ===\n');

  if (DB_NAME === 'english_learning') {
    skip('单词本结构套件', `需要隔离测试库，当前 DB=${DB_NAME}`);
    console.log(`\n阶段二十四结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
    process.exit(0);
  }

  const stamp = Date.now().toString(36).slice(-6);
  const W = 't24' + stamp;
  let ck = '', uid = 0;

  try {
    const reg = await registerUser('t24u_' + stamp);
    ck = reg.ck;
    uid = reg.uid;
    if (ck && uid > 0) ok('测试用户注册登录', `uid=${uid}`);
    else throw new Error('测试用户注册失败');

    // ---- 1. 组合成员 CRUD / uk_child 去重（6 项） ----
    console.log('[1] 组合成员 CRUD');
    const b1 = await createBook('T24书A_' + stamp, [W + 'alpha', W + 'beta'], ck);
    const b2 = await createBook('T24书B_' + stamp, [W + 'beta', W + 'gamma'], ck);

    const c1r = await api('POST', '/api/wordlists/compose', {
      title: 'T24组合1_' + stamp,
      members: [{ type: 'list', id: b1 }, { type: 'list', id: b2 }]
    }, ck);
    const c1 = (c1r.json && c1r.json.id) || 0;
    if (c1 && c1r.json.kind === 'combo' && c1r.json.item_count === 3)
      ok('新建组合只存指针（3 个去重词）', `id=${c1}, item_count=${c1r.json.item_count}`);
    else ko('新建组合只存指针（3 个去重词）', `HTTP ${c1r.status} ${String(c1r.text).slice(0, 140)}`);

    let kids = await childrenOf(c1, ck);
    if (kids.length === 2 && kids[0].child_type === 'list' && kids[0].title === 'T24书A_' + stamp
        && kids[1].title === 'T24书B_' + stamp && kids[0].item_count === 2)
      ok('成员列表返回类型/标题/词数', kids.map(k => k.title).join(' | '));
    else ko('成员列表返回类型/标题/词数', JSON.stringify(kids).slice(0, 200));

    await api('POST', `/api/wordlists/${b1}/items`, { items: [{ word: W + 'delta', meaning: '德尔塔' }] }, ck);
    const b3 = await createBook('T24书C_' + stamp, [W + 'zeta'], ck);
    const zetaItem = await itemIdOf(b3, W + 'zeta', ck);
    const addWord = await api('POST', `/api/wordlists/${c1}/children`, {
      items: [{ type: 'word', id: zetaItem }]
    }, ck);
    const dAfterWord = await detailOf(c1, ck);
    if (addWord.status === 200 && dAfterWord && dAfterWord.item_count === 5)
      ok('混排加入单个单词后计数=5', `item_count=${dAfterWord.item_count}`);
    else ko('混排加入单个单词后计数=5', `HTTP ${addWord.status}, item_count=${dAfterWord && dAfterWord.item_count}`);

    const dup = await api('POST', `/api/wordlists/${c1}/children`, { items: [{ type: 'list', id: b1 }] }, ck);
    const kidsAfterDup = await childrenOf(c1, ck);
    if (dup.status === 200 && kidsAfterDup.length === 3)
      ok('重复成员被 uk_child 去重（成员仍 3）', `members=${kidsAfterDup.length}`);
    else ko('重复成员被 uk_child 去重（成员仍 3）', `HTTP ${dup.status}, members=${kidsAfterDup.length}`);

    const deltaMember = kidsAfterDup.find(k => k.child_type === 'word');
    const rm = await api('DELETE', `/api/wordlists/${c1}/children/${deltaMember && deltaMember.member_id}`, null, ck);
    const dAfterRm = await detailOf(c1, ck);
    if (rm.status === 200 && dAfterRm && dAfterRm.item_count === 4)
      ok('移除成员后计数回落=4', `item_count=${dAfterRm.item_count}`);
    else ko('移除成员后计数回落=4', `HTTP ${rm.status}, item_count=${dAfterRm && dAfterRm.item_count}`);

    kids = await childrenOf(c1, ck);
    const listMembers = kids.filter(k => k.child_type === 'list');
    const reorder = await api('PUT', `/api/wordlists/${c1}/children/order`, {
      ids: [listMembers[1].member_id, listMembers[0].member_id]
    }, ck);
    const kidsRe = await childrenOf(c1, ck);
    if (reorder.status === 200 && kidsRe[0].child_id === b2 && kidsRe[1].child_id === b1)
      ok('成员排序生效（B 在前）', kidsRe.map(k => k.child_id).join(','));
    else ko('成员排序生效（B 在前）', `HTTP ${reorder.status}, order=${kidsRe.map(k => k.child_id).join(',')}`);

    // ---- 2. 递归展开：单层/三层/混排/去重（6 项） ----
    console.log('[2] 递归展开');
    const exBook = await expandOf(b1, ck);
    if (exBook.status === 200 && exBook.json.unique_count === 3)
      ok('基层本展开只含自身词条', `unique=${exBook.json.unique_count}`);
    else ko('基层本展开只含自身词条', `HTTP ${exBook.status}, unique=${exBook.json.unique_count}`);

    const exC1 = await expandOf(c1, ck);
    if (exC1.status === 200 && exC1.json.unique_count === 4)
      ok('一层组合展开去重=4', `unique=${exC1.json.unique_count}`);
    else ko('一层组合展开去重=4', `HTTP ${exC1.status}, unique=${exC1.json.unique_count}`);

    const cInnerR = await api('POST', '/api/wordlists/compose', {
      title: 'T24内层_' + stamp, members: [{ type: 'list', id: b2 }]
    }, ck);
    const cInner = cInnerR.json && cInnerR.json.id;
    const cMidR = await api('POST', '/api/wordlists/compose', {
      title: 'T24中层_' + stamp, members: [{ type: 'list', id: b1 }, { type: 'list', id: cInner }]
    }, ck);
    const cMid = cMidR.json && cMidR.json.id;
    const gammaItem = await itemIdOf(b2, W + 'gamma', ck);
    const cTopR = await api('POST', '/api/wordlists/compose', {
      title: 'T24顶层_' + stamp,
      members: [{ type: 'word', id: gammaItem }, { type: 'list', id: cMid }]
    }, ck);
    const cTop = cTopR.json && cTopR.json.id;

    const exTop = await expandOf(cTop, ck);
    if (exTop.status === 200 && exTop.json.unique_count === 4)
      ok('三层组合展开到最深层=4', `unique=${exTop.json.unique_count}`);
    else ko('三层组合展开到最深层=4', `HTTP ${exTop.status}, unique=${exTop.json.unique_count}`);

    if (exTop.json.data && exTop.json.data[0] && exTop.json.data[0].word === W + 'gamma')
      ok('单词与组合混排保序（单词在前）', `first=${exTop.json.data[0].word}`);
    else ko('单词与组合混排保序（单词在前）', `first=${exTop.json.data && exTop.json.data[0] && exTop.json.data[0].word}`);

    const betaHits = ((exTop.json.data) || []).filter(w => w.word === W + 'beta').length;
    if (betaHits === 1)
      ok('跨单词本重复词只保留一次', `beta=${betaHits}`);
    else ko('跨单词本重复词只保留一次', `beta=${betaHits}`);

    const dTop = await detailOf(cTop, ck);
    if (dTop && dTop.unique_count === undefined && dTop.item_count === exTop.json.unique_count)
      ok('展开数与 item_count 一致', `count=${dTop.item_count}`);
    else ko('展开数与 item_count 一致', `item_count=${dTop && dTop.item_count}, unique=${exTop.json.unique_count}`);

    // ---- 3. 防环与限深（3 项） ----
    console.log('[3] 防环与限深');
    const selfLoop = await api('POST', `/api/wordlists/${cTop}/children`, {
      items: [{ type: 'list', id: cTop }]
    }, ck);
    if (selfLoop.status === 400 && String(selfLoop.text).indexOf('环路') >= 0)
      ok('自引用被拒绝', `HTTP ${selfLoop.status}`);
    else ko('自引用被拒绝', `HTTP ${selfLoop.status} ${String(selfLoop.text).slice(0, 120)}`);

    const xR = await api('POST', '/api/wordlists/compose', {
      title: 'T24环X_' + stamp, members: [{ type: 'list', id: b2 }]
    }, ck);
    const xId = xR.json && xR.json.id;
    const yR = await api('POST', '/api/wordlists/compose', {
      title: 'T24环Y_' + stamp, members: [{ type: 'list', id: xId }]
    }, ck);
    const yId = yR.json && yR.json.id;
    const twoWay = await api('POST', `/api/wordlists/${xId}/children`, {
      items: [{ type: 'list', id: yId }]
    }, ck);
    if (twoWay.status === 400 && String(twoWay.text).indexOf('环路') >= 0)
      ok('A→B→A 环路被拒绝', `HTTP ${twoWay.status}`);
    else ko('A→B→A 环路被拒绝', `HTTP ${twoWay.status} ${String(twoWay.text).slice(0, 120)}`);

    let chainId = cInner;
    for (let i = 0; i < 34; i++) {
      const rr = await api('POST', '/api/wordlists/compose', {
        title: `T24链${i}_` + stamp, members: [{ type: 'list', id: chainId }]
      }, ck);
      chainId = rr.json && rr.json.id;
    }
    const deep = await expandOf(chainId, ck);
    if (deep.status === 400 && String(deep.text).indexOf('32') >= 0)
      ok('超过 32 层被拒绝', `HTTP ${deep.status}`);
    else ko('超过 32 层被拒绝', `HTTP ${deep.status} ${String(deep.text).slice(0, 120)}`);

    // ---- 4. 计数重算（3 项） ----
    console.log('[4] 计数重算');
    await api('POST', `/api/wordlists/${b2}/items`, { items: [{ word: W + 'epsilon', meaning: '艾普西龙' }] }, ck);
    const rangeList = await api('GET', '/api/wordlists?source=range', null, ck);
    const c1Row = ((rangeList.json && rangeList.json.data) || []).find(l => l.id === c1);
    if (c1Row && c1Row.item_count === 5)
      ok('子本新增词后组合读取时重算=5', `item_count=${c1Row.item_count}`);
    else ko('子本新增词后组合读取时重算=5', `item_count=${c1Row && c1Row.item_count}`);

    const dTop2 = await detailOf(cTop, ck);
    if (dTop2 && dTop2.item_count === 5)
      ok('三层祖先链计数同步重算=5', `item_count=${dTop2.item_count}`);
    else ko('三层祖先链计数同步重算=5', `item_count=${dTop2 && dTop2.item_count}`);

    if (dTop2 && dTop2.count_dirty === 0)
      ok('重算后 count_dirty 归零', `count_dirty=${dTop2.count_dirty}`);
    else ko('重算后 count_dirty 归零', `count_dirty=${dTop2 && dTop2.count_dirty}`);

    // ---- 5. 教材词升级为知识点（4 项） ----
    console.log('[5] 教材词升级');
    const tbBook = await createBook('T24教材_' + stamp, [W + 'texta', W + 'textb'], ck);
    const st1 = await api('POST', `/api/wordlists/${tbBook}/start-study`, {}, ck);
    if (st1.status === 200 && st1.json.kp_created === 2 && st1.json.learning_created === 2)
      ok('首次升级创建知识点与学情', `kp=${st1.json.kp_created}, learning=${st1.json.learning_created}`);
    else ko('首次升级创建知识点与学情', `HTTP ${st1.status} ${String(st1.text).slice(0, 140)}`);

    const st2 = await api('POST', `/api/wordlists/${tbBook}/start-study`, {}, ck);
    if (st2.status === 200 && st2.json.kp_created === 0 && st2.json.kp_skipped === 2)
      ok('重复升级不新建知识点', `created=${st2.json.kp_created}, skipped=${st2.json.kp_skipped}`);
    else ko('重复升级不新建知识点', `HTTP ${st2.status} ${String(st2.text).slice(0, 140)}`);

    const tbOrigin = q1(`SELECT COUNT(*) FROM knowledge_points WHERE user_id=${uid}
      AND user_input_content IN ('${W}texta','${W}textb') AND origin='wordlist' AND is_solidified=0`);
    if (tbOrigin === 2) ok('词表升级词条保持待生成精讲', `count=${tbOrigin}`);
    else ko('词表升级词条保持待生成精讲', `count=${tbOrigin}`);

    const tbDefs = q1(`SELECT COUNT(*) FROM kp_definitions d JOIN knowledge_points kp ON kp.id=d.knowledge_id
      WHERE kp.user_id=${uid} AND kp.user_input_content IN ('${W}texta','${W}textb')`);
    if (tbDefs === 2) ok('释义写入 kp_definitions', `definitions=${tbDefs}`);
    else ko('释义写入 kp_definitions', `definitions=${tbDefs}`);

    // ---- 6. 加入学习开关（4 项） ----
    console.log('[6] 加入学习开关');
    const kpOn = await api('POST', '/api/kp', { user_input_content: W + 'study1', join_study: true }, ck);
    const lOn = q1(`SELECT COUNT(*) FROM user_learning ul JOIN knowledge_points kp ON kp.id=ul.knowledge_id
      WHERE kp.user_id=${uid} AND kp.user_input_content='${W}study1'`);
    if (kpOn.status === 200 && kpOn.json.learning_created === 1 && lOn === 1)
      ok('单条录入开启开关即建学情', `learning_created=${kpOn.json.learning_created}`);
    else ko('单条录入开启开关即建学情', `HTTP ${kpOn.status}, rows=${lOn}`);

    const kpOff = await api('POST', '/api/kp', { user_input_content: W + 'study2', join_study: false }, ck);
    const lOff = q1(`SELECT COUNT(*) FROM user_learning ul JOIN knowledge_points kp ON kp.id=ul.knowledge_id
      WHERE kp.user_id=${uid} AND kp.user_input_content='${W}study2'`);
    if (kpOff.status === 200 && kpOff.json.learning_created === 0 && lOff === 0)
      ok('单条录入关闭开关不建学情', `rows=${lOff}`);
    else ko('单条录入关闭开关不建学情', `HTTP ${kpOff.status}, rows=${lOff}`);

    const bulkOn = await api('POST', '/api/kp/bulk', {
      words: [{ word: W + 'study3', meaning: '三' }, { word: W + 'study4', meaning: '四' }],
      join_study: true
    }, ck);
    if (bulkOn.status === 200 && bulkOn.json.created === 2 && bulkOn.json.learning_created === 2)
      ok('批量录入开启开关建 2 条学情', `learning=${bulkOn.json.learning_created}`);
    else ko('批量录入开启开关建 2 条学情', `HTTP ${bulkOn.status} ${String(bulkOn.text).slice(0, 140)}`);

    const bulkOff = await api('POST', '/api/kp/bulk', {
      words: [{ word: W + 'study5' }, { word: W + 'study6' }],
      join_study: false
    }, ck);
    const lBulkOff = q1(`SELECT COUNT(*) FROM user_learning ul JOIN knowledge_points kp ON kp.id=ul.knowledge_id
      WHERE kp.user_id=${uid} AND kp.user_input_content IN ('${W}study5','${W}study6')`);
    if (bulkOff.status === 200 && bulkOff.json.learning_created === 0 && lBulkOff === 0)
      ok('批量录入关闭开关不建学情', `rows=${lBulkOff}`);
    else ko('批量录入关闭开关不建学情', `HTTP ${bulkOff.status}, rows=${lBulkOff}`);

    // ---- 7. 30 条阈值（3 项） ----
    console.log('[7] 30 条阈值');
    const words30 = Array.from({ length: 30 }, (_, i) => W + 'n' + (i + 1));
    const p30 = await api('POST', '/api/kp/preview', { words: words30 }, ck);
    if (p30.status === 200 && p30.json.to_create === 30 && p30.json.need_confirm === false)
      ok('30 条不弹询问', `to_create=${p30.json.to_create}, need_confirm=${p30.json.need_confirm}`);
    else ko('30 条不弹询问', `HTTP ${p30.status} ${String(p30.text).slice(0, 140)}`);

    const words31 = words30.concat([W + 'n31']);
    const p31 = await api('POST', '/api/kp/preview', { words: words31 }, ck);
    if (p31.status === 200 && p31.json.to_create === 31 && p31.json.need_confirm === true)
      ok('31 条触发询问', `to_create=${p31.json.to_create}, need_confirm=${p31.json.need_confirm}`);
    else ko('31 条触发询问', `HTTP ${p31.status} ${String(p31.text).slice(0, 140)}`);

    const bulkNoConfirm = await api('POST', '/api/kp/bulk', {
      words: words31.map(w => ({ word: w })), join_study: true
    }, ck);
    if (bulkNoConfirm.status === 400 && String(bulkNoConfirm.text).indexOf('30') >= 0)
      ok('未确认的 31 条批量被拒绝', `HTTP ${bulkNoConfirm.status}`);
    else ko('未确认的 31 条批量被拒绝', `HTTP ${bulkNoConfirm.status} ${String(bulkNoConfirm.text).slice(0, 140)}`);

    // ---- 8. 学习汇总与正在学习（4 项） ----
    console.log('[8] 学习汇总与正在学习');
    const learnIds = runMysql(`SELECT id FROM user_learning WHERE user_id=${uid} ORDER BY id`).split('\n').filter(Boolean);
    const masteredId = learnIds[0];
    const reviewedId = learnIds[1];
    runMysql(`UPDATE user_learning SET proficiency_level=5 WHERE id=${masteredId};`);
    runMysql(`UPDATE user_learning SET review_times=1, last_review_time=NOW(),
      next_review_time=DATE_SUB(NOW(), INTERVAL 1 DAY) WHERE id=${reviewedId};`);

    const dbSum = runMysql(`SELECT
      SUM(CASE WHEN is_active=1 AND proficiency_level<5 THEN 1 ELSE 0 END),
      SUM(CASE WHEN is_active=1 AND proficiency_level>=5 THEN 1 ELSE 0 END),
      SUM(CASE WHEN is_active=1 AND proficiency_level<5 AND review_times=0 THEN 1 ELSE 0 END),
      SUM(CASE WHEN is_active=1 AND proficiency_level<5 AND next_review_time IS NOT NULL
               AND DATE(next_review_time)<=CURDATE() THEN 1 ELSE 0 END)
      FROM user_learning WHERE user_id=${uid}`).split('\t').map(Number);
    const sum = await api('GET', '/api/learn/summary', null, ck);
    if (sum.status === 200 && sum.json.learning === dbSum[0] && sum.json.mastered === dbSum[1]
        && sum.json.to_learn === dbSum[2] && sum.json.due_today === dbSum[3])
      ok('汇总四项与库一致', `learning=${sum.json.learning}, mastered=${sum.json.mastered}, to_learn=${sum.json.to_learn}, due=${sum.json.due_today}`);
    else ko('汇总四项与库一致', `API=${JSON.stringify(sum.json)}, DB=${dbSum.join('/')}`);

    const studyAll = await api('GET', '/api/learn/studying?filter=all&size=200', null, ck);
    const allRows = (studyAll.json && studyAll.json.data) || [];
    if (studyAll.status === 200 && studyAll.json.total === dbSum[0]
        && allRows.every(r => r.proficiency_level < 5))
      ok('正在学习仅含未满级', `total=${studyAll.json.total}`);
    else ko('正在学习仅含未满级', `total=${studyAll.json && studyAll.json.total}, DB=${dbSum[0]}`);

    const studyLearning = await api('GET', '/api/learn/studying?filter=learning&size=200', null, ck);
    const learningRows = (studyLearning.json && studyLearning.json.data) || [];
    if (studyLearning.status === 200 && learningRows.length === 1
        && learningRows.every(r => r.review_times > 0))
      ok('filter=learning 仅含已复习', `rows=${learningRows.length}`);
    else ko('filter=learning 仅含已复习', `HTTP ${studyLearning.status}, rows=${learningRows.length}`);

    const studyPage = await api('GET', '/api/learn/studying?filter=all&page=2&size=2', null, ck);
    if (studyPage.status === 200 && (studyPage.json.data || []).length === 2
        && studyPage.json.total === dbSum[0] && studyPage.json.page === 2)
      ok('正在学习分页第二页 2 条', `rows=${(studyPage.json.data || []).length}, total=${studyPage.json.total}`);
    else ko('正在学习分页第二页 2 条', `HTTP ${studyPage.status} ${String(studyPage.text).slice(0, 140)}`);

    // ---- 9. 注销级联清理组合成员（1 项） ----
    console.log('[9] 注销级联');
    const reg2 = await registerUser('t24d_' + stamp);
    const b2ck = reg2.ck;
    const leaf = await createBook('T24注销书_' + stamp, [W + 'gone1'], b2ck);
    const comboR = await api('POST', '/api/wordlists/compose', {
      title: 'T24注销组合_' + stamp, members: [{ type: 'list', id: leaf }]
    }, b2ck);
    const comboId = comboR.json && comboR.json.id;
    const beforeChildren = q1(`SELECT COUNT(*) FROM wordlist_children WHERE parent_id=${Number(comboId)}`);
    const dereg = await api('POST', '/api/auth/deregister', null, b2ck);
    const afterChildren = q1(`SELECT COUNT(*) FROM wordlist_children
      WHERE parent_id=${Number(comboId)} OR (child_type='list' AND child_id IN (${Number(comboId)},${Number(leaf)}))`);
    if (beforeChildren === 1 && dereg.status === 200 && afterChildren === 0)
      ok('注销级联清理 wordlist_children', `before=${beforeChildren}, after=${afterChildren}`);
    else ko('注销级联清理 wordlist_children', `before=${beforeChildren}, dereg=${dereg.status}, after=${afterChildren}`);

  } catch (e) {
    ko('套件异常', String((e && e.stack) || e).slice(0, 300));
  }

  console.log(`\n阶段二十四结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
  if (fail > 0) {
    console.log('失败项:');
    for (const f of failures) console.log('  - ' + f.name + ': ' + f.msg);
    process.exit(1);
  }
}

main();
