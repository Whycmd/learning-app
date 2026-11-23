// 阶段二十五：单词本页可用性修复（v1.5.0-T007）
// 覆盖：教材词 knowledge_id 归属隔离（不再暴露他人知识点）、组合混排创建与顺序、
//       成员增删与上下移、前端 Tab/筛选/折叠/拖拽/组合入口静态契约
// 仅在隔离测试环境执行（测试库）
const fs = require('fs');
const http = require('http');
const path = require('path');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT } = require('./_env');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

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

async function itemsOf(listId, ck) {
  const r = await api('GET', `/api/wordlists/${listId}/items?size=300`, null, ck);
  return (r.json && r.json.data) || [];
}

async function itemIdOf(listId, word, ck) {
  const hit = (await itemsOf(listId, ck)).find(i => String(i.word).toLowerCase() === word.toLowerCase());
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
  return { status: r.status, json: r.json || {} };
}

function readFrontend(rel) {
  return fs.readFileSync(path.join(__dirname, '..', 'frontend', rel), 'utf8');
}

async function main() {
  console.log('=== 阶段二十五：单词本页可用性修复（v1.5.0-T007） ===\n');

  if (DB_NAME === 'english_learning') {
    skip('单词本 UX 套件', `需要隔离测试库，当前 DB=${DB_NAME}`);
    console.log(`\n阶段二十五结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
    process.exit(0);
  }

  const stamp = Date.now().toString(36).slice(-6);
  const W = 't25' + stamp;
  let ckA = '', uidA = 0, ckB = '', uidB = 0;

  try {
    const a = await registerUser('t25a_' + stamp);
    ckA = a.ck; uidA = a.uid;
    const b = await registerUser('t25b_' + stamp);
    ckB = b.ck; uidB = b.uid;
    if (ckA && ckB && uidA > 0 && uidB > 0) ok('双测试用户注册登录', `A=${uidA}, B=${uidB}`);
    else throw new Error('测试用户注册失败');

    // ---- 1. 教材词知识点归属隔离（7 项） ----
    console.log('[1] 教材词 knowledge_id 归属');
    const builtinRes = await api('GET', '/api/wordlists?source=builtin', null, ckA);
    const builtin = (builtinRes.json && builtinRes.json.data) || [];
    if (!builtin.length) throw new Error('缺少内置教材词表');
    const unitId = builtin[0].id;

    const before = await itemsOf(unitId, ckA);
    const beforeOwned = before.filter(i => i.knowledge_id != null).length;
    if (beforeOwned === 0) ok('未开始学习时教材词不携带 knowledge_id', `items=${before.length}`);
    else ko('未开始学习时教材词不携带 knowledge_id', `owned=${beforeOwned}`);

    const study = await api('POST', `/api/wordlists/${unitId}/start-study`, {}, ckA);
    if (study.status === 200 && study.json && study.json.kp_created >= 0) ok('用户 A 对教材单元开始学习');
    else ko('用户 A 对教材单元开始学习', `HTTP ${study.status} ${study.text.slice(0, 120)}`);

    const aItems = await itemsOf(unitId, ckA);
    const aOwned = aItems.filter(i => i.knowledge_id != null);
    if (aOwned.length > 0) ok('用户 A 读取到自己的知识点 id', `owned=${aOwned.length}`);
    else ko('用户 A 读取到自己的知识点 id', `owned=0，items=${aItems.length}`);

    if (aOwned.length) {
      const kp = await api('GET', '/api/kp/' + aOwned[0].knowledge_id, null, ckA);
      if (kp.status === 200 && kp.json && !kp.json.error) ok('用户 A 可打开自己的词条详情');
      else ko('用户 A 可打开自己的词条详情', `HTTP ${kp.status} ${kp.text.slice(0, 120)}`);
    } else skip('用户 A 可打开自己的词条详情', '无知识点');

    const bItems = await itemsOf(unitId, ckB);
    const bLeak = bItems.filter(i => i.knowledge_id != null).length;
    if (bLeak === 0) ok('用户 B 不继承 A 的 knowledge_id（图2 根因）', `items=${bItems.length}`);
    else ko('用户 B 不继承 A 的 knowledge_id（图2 根因）', `leak=${bLeak}`);

    await api('POST', `/api/wordlists/${unitId}/start-study`, {}, ckB);
    const bAfter = await itemsOf(unitId, ckB);
    const bOwned = bAfter.filter(i => i.knowledge_id != null);
    if (bOwned.length > 0) {
      const kpB = await api('GET', '/api/kp/' + bOwned[0].knowledge_id, null, ckB);
      const kpBAsA = await api('GET', '/api/kp/' + bOwned[0].knowledge_id, null, ckA);
      if (kpB.status === 200 && !(kpB.json && kpB.json.error)) ok('用户 B 学习后指向自己的知识点');
      else ko('用户 B 学习后指向自己的知识点', `HTTP ${kpB.status}`);
      if (kpBAsA.status === 404 || (kpBAsA.json && kpBAsA.json.error)) ok('B 的知识点对 A 不可读（隔离成立）');
      else ko('B 的知识点对 A 不可读（隔离成立）', `HTTP ${kpBAsA.status}`);
    } else ko('用户 B 学习后指向自己的知识点', 'owned=0');

    const aAgain = await itemsOf(unitId, ckA);
    const aAgainIds = aAgain.filter(i => i.knowledge_id != null).map(i => i.knowledge_id).sort();
    const aFirstIds = aOwned.map(i => i.knowledge_id).sort();
    if (aAgainIds.length === aFirstIds.length && aAgainIds.every((v, i) => v === aFirstIds[i]))
      ok('B 学习后 A 的 knowledge_id 不被覆盖');
    else ko('B 学习后 A 的 knowledge_id 不被覆盖', `before=${aFirstIds.slice(0, 3)}, after=${aAgainIds.slice(0, 3)}`);

    // ---- 2. 组合混排创建（6 项） ----
    console.log('[2] 组合混排创建');
    const b1 = await createBook('T25书A_' + stamp, [W + 'alpha', W + 'beta'], ckA);
    const b2 = await createBook('T25书B_' + stamp, [W + 'gamma'], ckA);
    const c1 = await api('POST', '/api/wordlists/compose', {
      title: 'T25旧组合_' + stamp,
      description: '已有组合',
      members: [{ type: 'list', id: b1 }]
    }, ckA);
    const c1Id = (c1.json && c1.json.id) || 0;
    if (c1Id) ok('创建已有组合', `id=${c1Id}`);
    else ko('创建已有组合', `HTTP ${c1.status} ${c1.text.slice(0, 120)}`);

    const wordItem = await itemIdOf(b1, W + 'beta', ckA);
    const c2 = await api('POST', '/api/wordlists/compose', {
      title: 'T25混排组合_' + stamp,
      description: '词表 + 组合 + 单词',
      members: [
        { type: 'list', id: b2 },
        { type: 'list', id: c1Id },
        { type: 'word', id: wordItem }
      ]
    }, ckA);
    const c2Id = (c2.json && c2.json.id) || 0;
    if (c2Id) ok('混排创建组合成功', `id=${c2Id}`);
    else ko('混排创建组合成功', `HTTP ${c2.status} ${c2.text.slice(0, 120)}`);

    const kids = await childrenOf(c2Id, ckA);
    const shape = kids.map(k => k.child_type + ':' + k.child_id).join(',');
    const want = `list:${b2},list:${c1Id},word:${wordItem}`;
    if (shape === want) ok('成员顺序与请求一致', shape);
    else ko('成员顺序与请求一致', `got=${shape}`);

    const ex = await expandOf(c2Id, ckA);
    if (ex.status === 200 && ex.json.unique_count === 3) ok('混排展开去重计数=3', `unique=${ex.json.unique_count}`);
    else ko('混排展开去重计数=3', `HTTP ${ex.status} unique=${ex.json.unique_count}`);

    const c2Detail = await detailOf(c2Id, ckA);
    if (c2Detail && c2Detail.kind === 'combo' && c2Detail.item_count === 3) ok('组合详情 kind=combo 且计数=3');
    else ko('组合详情 kind=combo 且计数=3', `kind=${c2Detail && c2Detail.kind} count=${c2Detail && c2Detail.item_count}`);

    const dup = await api('POST', `/api/wordlists/${c2Id}/children`, {
      items: [{ type: 'list', id: b2 }]
    }, ckA);
    if (dup.status === 200) ok('重复成员请求被接受（uk_child 去重）');
    else ko('重复成员请求被接受（uk_child 去重）', `HTTP ${dup.status}`);
    const kidsAfterDup = await childrenOf(c2Id, ckA);
    if (kidsAfterDup.length === 3) ok('重复添加后成员数仍为 3', `n=${kidsAfterDup.length}`);
    else ko('重复添加后成员数仍为 3', `n=${kidsAfterDup.length}`);

    // ---- 3. 成员管理：增删与上下移（6 项） ----
    console.log('[3] 成员管理');
    const b3 = await createBook('T25书C_' + stamp, [W + 'delta'], ckA);
    const addR = await api('POST', `/api/wordlists/${c2Id}/children`, {
      items: [{ type: 'list', id: b3 }]
    }, ckA);
    if (addR.status === 200) ok('追加成员成功');
    else ko('追加成员成功', `HTTP ${addR.status} ${addR.text.slice(0, 120)}`);
    let kids2 = await childrenOf(c2Id, ckA);
    if (kids2.length === 4) ok('追加后成员数=4');
    else ko('追加后成员数=4', `n=${kids2.length}`);

    const reversed = kids2.map(k => k.member_id).reverse();
    const orderR = await api('PUT', `/api/wordlists/${c2Id}/children/order`, { ids: reversed }, ckA);
    if (orderR.status === 200) ok('提交成员新顺序成功');
    else ko('提交成员新顺序成功', `HTTP ${orderR.status} ${orderR.text.slice(0, 120)}`);
    kids2 = await childrenOf(c2Id, ckA);
    const gotIds = kids2.map(k => k.member_id);
    if (gotIds.join(',') === reversed.join(',')) ok('上下移后顺序生效', gotIds.join(','));
    else ko('上下移后顺序生效', `want=${reversed.join(',')} got=${gotIds.join(',')}`);

    const removedKey = kids2[0].child_type + ':' + kids2[0].child_id;
    const delR = await api('DELETE', `/api/wordlists/${c2Id}/children/${kids2[0].member_id}`, null, ckA);
    if (delR.status === 200) ok('移除成员成功', removedKey);
    else ko('移除成员成功', `HTTP ${delR.status} ${delR.text.slice(0, 120)}`);
    kids2 = await childrenOf(c2Id, ckA);
    if (kids2.length === 3) ok('移除后成员数=3', `n=${kids2.length}`);
    else ko('移除后成员数=3', `n=${kids2.length}`);

    const c2After = await detailOf(c2Id, ckA);
    const exAfter = await expandOf(c2Id, ckA);
    if (c2After && c2After.item_count === exAfter.json.unique_count && c2After.item_count >= 0)
      ok('移除后计数与实际展开一致', `count=${c2After.item_count}`);
    else ko('移除后计数与实际展开一致', `count=${c2After && c2After.item_count} unique=${exAfter.json.unique_count}`);

    // ---- 4. 整本加入（11 项）：整本作为层级成员；共用词表自动转私有副本 ----
    console.log('[4] 整本加入');
    const b4 = await createBook('T25书D_' + stamp, [W + 'echo', W + 'foxtrot'], ckA);
    const impCombo = await api('POST', `/api/wordlists/${b4}/import-whole`, { target_id: c2Id }, ckA);
    if (impCombo.status === 200 && impCombo.json && impCombo.json.added === 2)
      ok('整本加入合并本', `added=${impCombo.json.added}`);
    else ko('整本加入合并本', `HTTP ${impCombo.status} ${impCombo.text.slice(0, 120)}`);

    const kids3 = await childrenOf(c2Id, ckA);
    const lastKid = kids3[kids3.length - 1];
    if (lastKid && lastKid.child_type === 'list' && lastKid.child_id === b4)
      ok('合并本新增整本成员指针');
    else ko('合并本新增整本成员指针', JSON.stringify(lastKid || {}));

    const ex3 = await expandOf(c2Id, ckA);
    if (ex3.json && ex3.json.unique_count === 5) ok('整本加入后展开去重=5', `unique=${ex3.json.unique_count}`);
    else ko('整本加入后展开去重=5', `unique=${ex3.json && ex3.json.unique_count}`);

    // 回归到用户反馈：普通单词本整本加入后应出现层级成员，而不是悄悄复制词条
    const impBook = await api('POST', `/api/wordlists/${b1}/import-whole`, { target_id: b2 }, ckA);
    const b2Items = await itemsOf(b2, ckA);
    if (impBook.status === 200 && impBook.json && impBook.json.added === 2 && b2Items.length === 1)
      ok('整本加入普通单词本不复制词条', `own_items=${b2Items.length}`);
    else ko('整本加入普通单词本不复制词条', `HTTP ${impBook.status} own_items=${b2Items.length}`);

    const b2Detail = await detailOf(b2, ckA);
    const b2Kids = (b2Detail && b2Detail.children) || [];
    if (b2Kids.length === 1 && b2Kids[0].child_id === b1 && b2Kids[0].child_type === 'list')
      ok('普通单词本返回层级成员', JSON.stringify(b2Kids[0] && { type: b2Kids[0].child_type, id: b2Kids[0].child_id }));
    else ko('普通单词本返回层级成员', JSON.stringify(b2Kids));

    const b2Ex = await expandOf(b2, ckA);
    if (b2Detail && b2Detail.item_count === 3 && b2Ex.json.unique_count === 3)
      ok('层级成员计入词数', `count=${b2Detail.item_count} unique=${b2Ex.json.unique_count}`);
    else ko('层级成员计入词数', `count=${b2Detail && b2Detail.item_count} unique=${b2Ex.json && b2Ex.json.unique_count}`);

    // 共用词表不能直接挂成成员：自动复制为私有本后再加入
    const impBuiltin = await api('POST', `/api/wordlists/${builtin[0].id}/import-whole`, { target_id: b2 }, ckA);
    const copiedId = (impBuiltin.json && impBuiltin.json.copied_id) || 0;
    const copiedTitle = (impBuiltin.json && impBuiltin.json.copied_title) || '';
    const b2Kids2 = ((await detailOf(b2, ckA)) || {}).children || [];
    if (impBuiltin.status === 200 && copiedId > 0 && copiedTitle.endsWith('（副本）')
        && b2Kids2.some(k => k.child_id === copiedId) && !b2Kids2.some(k => k.child_id === builtin[0].id))
      ok('共用词表自动复制为私有本后加入', `copy=${copiedId} ${copiedTitle}`);
    else ko('共用词表自动复制为私有本后加入', `HTTP ${impBuiltin.status} copy=${copiedId} kids=${b2Kids2.length}`);

    const copyDetail = await detailOf(copiedId, ckA);
    const copyItems = await itemsOf(copiedId, ckA);
    if (copyDetail && copyDetail.source === 'user' && copyItems.length === builtin[0].item_count)
      ok('私有副本词条数与来源一致', `n=${copyItems.length}`);
    else ko('私有副本词条数与来源一致', `source=${copyDetail && copyDetail.source} n=${copyItems.length} want=${builtin[0].item_count}`);

    // 同一本共用词表再次加入另一本时复用已有私有副本，不重复复制
    const impBuiltin2 = await api('POST', `/api/wordlists/${builtin[0].id}/import-whole`, { target_id: c2Id }, ckA);
    const c2KidsAgain = await childrenOf(c2Id, ckA);
    if (impBuiltin2.status === 200 && (impBuiltin2.json.copied_id || 0) === 0
        && c2KidsAgain.some(k => k.child_id === copiedId))
      ok('重复加入共用词表复用私有副本', `child=${copiedId}`);
    else ko('重复加入共用词表复用私有副本', `HTTP ${impBuiltin2.status} copied=${impBuiltin2.json && impBuiltin2.json.copied_id}`);

    const impCycle = await api('POST', `/api/wordlists/${c2Id}/import-whole`, { target_id: b1 }, ckA);
    if (impCycle.status === 400) ok('层级环路被拒绝', impCycle.text.slice(0, 60));
    else ko('层级环路被拒绝', `HTTP ${impCycle.status}`);

    const impSelf = await api('POST', `/api/wordlists/${b1}/import-whole`, { target_id: b1 }, ckA);
    if (impSelf.status === 400) ok('整本加入自身被拒绝');
    else ko('整本加入自身被拒绝', `HTTP ${impSelf.status}`);

    // ---- 5. 前端静态契约（17 项） ----
    console.log('[5] 前端静态契约');
    const html = readFrontend('pages/wordbook.html');
    const js = readFrontend('static/wordlib.js');
    const css = readFrontend('static/components.css');
    const kp = readFrontend('pages/kp-entry.html');

    const htmlChecks = [
      ['Tab 容器', 'data-lib-tab="shared"'],
      ['私人默认 Tab 按钮', 'data-lib-tab="private"'],
      ['共用分类筛选', 'data-shared-filter="textbook"'],
      ['私人分类筛选', 'data-private-filter="list"'],
      ['合并入口并入新建单词本', 'data-new-book="combo"'],
      ['组合成员抽屉', 'id="modal-combo"'],
      ['整本加入抽屉', 'id="modal-wl-import"'],
      ['每组显示更多', 'data-more="list"'],
      ['FAB 可拖动提示', 'title="待整理生词（可拖动）"']
    ];
    for (const [name, needle] of htmlChecks) {
      if (html.indexOf(needle) >= 0) ok('页面含' + name);
      else ko('页面含' + name, `missing ${needle}`);
    }

    if (html.indexOf('id="btn-new-combo"') < 0) ok('页面无独立「新建组合」按钮');
    else ko('页面无独立「新建组合」按钮', '仍存在 btn-new-combo');

    const jsChecks = [
      ['名称本地匹配', 'function localHits('],
      ['Tab 切换', 'function setTab('],
      ['FAB 拖拽记忆', "localStorage.setItem(fabKey()"],
      ['组合创建接口', '/api/wordlists/compose'],
      ['成员顺序接口', '/children/order'],
      ['整本加入接口', '/import-whole'],
      ['整本加入入口', 'data-act-wl-import'],
      ['整本加入后刷新当前本可见层级', 'openWordlist(importTargetId'],
      ['整本加入候选来源', 'data-import-source'],
      ['成员三角号就地展开', 'data-member-toggle'],
      ['层级返回上一级', "btn-back-label"],
      ['教材词标签', "'教材词'"]
    ];
    for (const [name, needle] of jsChecks) {
      if (js.indexOf(needle) >= 0) ok('脚本含' + name);
      else ko('脚本含' + name, `missing ${needle}`);
    }

    if (css.indexOf('.lib-tabs') >= 0 && css.indexOf('.lib-vol') >= 0 &&
        css.indexOf('.combo-chip') >= 0 && css.indexOf('.kp-chip') >= 0)
      ok('样式含 Tab / 折叠 / 成员选择');
    else ko('样式含 Tab / 折叠 / 成员选择', '缺少选择器');

    if (js.indexOf('state.user.concat(state.range)') >= 0 && js.indexOf("meta: '组合'") < 0)
      ok('组合并入「我的单词本」，卡片无组合标识');
    else ko('组合并入「我的单词本」，卡片无组合标识', '仍分离或存在组合标识');

    if (js.indexOf('openVols: []') >= 0)
      ok('教材册默认折叠');
    else ko('教材册默认折叠', '默认值不是空数组');

    if (css.indexOf('[hidden] { display: none !important; }') >= 0)
      ok('折叠可用：hidden 覆盖 grid/flex 显示');
    else ko('折叠可用：hidden 覆盖 grid/flex 显示', '缺少 [hidden] 强制隐藏规则');

    if (kp.indexOf('detail-missing') >= 0 && kp.indexOf('不存在或已被合并') < 0)
      ok('词条详情改为可行动空态（去掉误导红字）');
    else ko('词条详情改为可行动空态（去掉误导红字）', '仍含旧文案或缺少空态');

  } catch (e) {
    ko('套件执行', e.message);
  }

  console.log(`\n阶段二十五结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
  if (failures.length) {
    console.log('失败明细：');
    failures.forEach(f => console.log(' - ' + f.name + ': ' + f.msg));
  }
  process.exit(fail > 0 ? 1 : 0);
}

main();
