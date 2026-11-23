// 阶段二十二：两层词表索引与词表库（v1.5.0-T003）
// 覆盖：内置教材 seed、词表/词条/搜索只读接口、用户词表 CRUD 与复制、
//       范围包幂等合并、我的单词（排除单词本引用）与一键导入、
//       AI 生成词表（mock + result_ref + 每日限额 + 队列满）、注销级联、前端资源本地化
// 仅在隔离测试环境执行（测试库 + AI Mock 非 off）
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn, spawnSync } = require('child_process');
const { MYSQL_BIN, MYSQL_PW_FILE, SOCK, DB_NAME, API_HOST, API_PORT, AI_MOCK_MODE } = require('./_env');
const { withSystemCookie } = require('./_system');

let pass = 0, fail = 0, skipped = 0;
const failures = [];
function ok(name, msg = '') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({ name, msg }); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg = '') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

const ROOT = path.resolve(__dirname, '..');
const SERVER_BIN = path.join(ROOT, 'backend/build/eng_app_server');
const AUX_PORT = API_PORT + 3;

function runMysql(sql) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const r = spawnSync(MYSQL_BIN,
    ['--socket=' + SOCK, '-uroot', '-p' + pw, '--default-character-set=utf8mb4', '-N', DB_NAME, '-e', sql],
    { encoding: 'utf8', timeout: 30000 });
  if (r.status !== 0) throw new Error((r.stderr || r.stdout || '').trim());
  return r.stdout.trim();
}

function request(method, urlPath, body, cookie) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    if (cookie) headers['Cookie'] = 'eng_session=' + cookie;
    else withSystemCookie(headers);
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

function auxApi(port, method, urlPath, body, cookie) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const headers = { 'Content-Type': 'application/json', 'Content-Length': data ? Buffer.byteLength(data) : 0 };
    if (cookie) headers['Cookie'] = 'eng_session=' + cookie;
    else withSystemCookie(headers);
    const req = http.request({ host: API_HOST, port, method, path: urlPath, headers, timeout: 120000 }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(text); } catch (_) {}
        resolve({ status: res.statusCode, json, text });
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
    if (data) req.write(data);
    req.end();
  });
}

function waitHealth(port, timeoutMs = 30000) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = () => {
      const req = http.get({ host: API_HOST, port, path: '/api/health', timeout: 3000 }, res => {
        res.resume();
        if (res.statusCode === 200) return resolve();
        retry();
      });
      req.on('error', retry);
      req.on('timeout', () => req.destroy(new Error('timeout')));
      function retry() {
        if (Date.now() - start > timeoutMs) return reject(new Error(`backend not ready on ${port}`));
        setTimeout(tick, 300);
      }
    };
    tick();
  });
}

function startBackend(port, mockMode, extraEnv = {}) {
  const logFile = path.join(os.tmpdir(), `eng_wl_${port}_${process.pid}.log`);
  const out = fs.openSync(logFile, 'a');
  const proc = spawn(SERVER_BIN, ['--root', ROOT, '--port', String(port)], {
    env: {
      ...process.env,
      DB_NAME, TEST_DB_NAME: DB_NAME,
      ENG_DB_NAME: DB_NAME,
      ENG_AI_MOCK_MODE: mockMode,
      AI_MOCK_MODE: mockMode,
      ENG_AI_RETRY_TIMES: '1',
      ENG_AI_RETRY_INTERVAL: '1',
      ...extraEnv,
    },
    stdio: ['ignore', out, out],
  });
  return { proc, logFile };
}

function stopBackend(proc) {
  return new Promise(resolve => {
    if (!proc || proc.exitCode !== null) return resolve();
    let done = false;
    const finish = () => { if (!done) { done = true; resolve(); } };
    proc.once('exit', finish);
    try { proc.kill('SIGTERM'); } catch { /* ignore */ }
    setTimeout(() => { try { proc.kill('SIGKILL'); } catch {} finish(); }, 5000);
  });
}

async function waitTask(taskId, timeoutMs = 60000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    const r = await api('GET', `/api/ai-tasks/${taskId}`);
    if (r.status === 200 && r.json && ['done', 'partial', 'failed'].includes(r.json.status)) return r.json;
    await new Promise(r2 => setTimeout(r2, 500));
  }
  throw new Error('task timeout: ' + taskId);
}

async function main() {
  console.log('=== 阶段二十二：词表库（v1.5.0-T003） ===\n');

  if (AI_MOCK_MODE === 'off' || DB_NAME === 'english_learning') {
    skip('词表库套件', `需要隔离测试环境，当前 DB=${DB_NAME}, Mock=${AI_MOCK_MODE}`);
    console.log(`\n阶段二十二结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
    process.exit(0);
  }

  const stamp = Date.now().toString(36).slice(-6);
  let aux = null;

  try {
    // ---- 1. 内置教材 seed ----
    console.log('[1] 内置教材 seed');
    const builtin = await api('GET', '/api/wordlists?source=builtin');
    const lists = (builtin.json && builtin.json.data) || [];
    if (lists.length === 36) ok('内置词表 36 个单元', `count=${lists.length}`);
    else ko('内置词表 36 个单元', `期望36，实际 ${lists.length}`);

    const volumes = new Set(lists.map(l => l.volume));
    if (volumes.size === 7) ok('覆盖 7 册教材', Array.from(volumes).join(' / '));
    else ko('覆盖 7 册教材', `期望7，实际 ${volumes.size}`);

    const sum = lists.reduce((n, l) => n + Number(l.item_count || 0), 0);
    if (sum === 2723) ok('词条总数 2723', `sum=${sum}`);
    else ko('词条总数 2723', `期望2723，实际 ${sum}`);

    const first = lists[0] || {};
    const detail = await api('GET', `/api/wordlists/${first.id}`);
    if (detail.status === 200 && detail.json && detail.json.source === 'builtin' && detail.json.scope_key === null)
      ok('详情字段完整（source/scope_key）', `title=${detail.json.title}`);
    else ko('详情字段完整（source/scope_key）', `HTTP ${detail.status} ${String(detail.text).slice(0, 120)}`);

    const items = await api('GET', `/api/wordlists/${first.id}/items?size=5`);
    if (items.status === 200 && items.json.total === 56 && items.json.data.length === 5 && items.json.data[0].word === 'exchange')
      ok('词条分页', `total=${items.json.total}, first=${items.json.data[0].word}`);
    else ko('词条分页', `HTTP ${items.status} ${String(items.text).slice(0, 120)}`);

    const inside = await api('GET', `/api/wordlists/${first.id}/items?size=5&q=lecture`);
    if (inside.status === 200 && inside.json.total === 1)
      ok('词表内搜索命中', `total=${inside.json.total}`);
    else ko('词表内搜索命中', `HTTP ${inside.status} ${String(inside.text).slice(0, 120)}`);

    const search = await api('GET', '/api/wordlists/search?q=exchange');
    if (search.status === 200 && search.json.data.length >= 1 && search.json.data[0].word === 'exchange')
      ok('全局搜索命中内置词条', `hits=${search.json.data.length}, list=${search.json.data[0].title}`);
    else ko('全局搜索命中内置词条', `HTTP ${search.status} ${String(search.text).slice(0, 120)}`);

    // ---- 2. 用户词表 CRUD / 复制 ----
    console.log('[2] 用户词表 CRUD');
    const created = await api('POST', '/api/wordlists', {
      title: '阶段22词表_' + stamp,
      description: '套件创建',
      items: [
        { word: 'apple', meaning: '苹果', pos: 'n.' },
        { word: 'Banana', meaning: '香蕉', pos: 'n.' },
        { word: 'apple', meaning: '重复条目' }
      ]
    });
    if (created.status === 200 && created.json && created.json.id && created.json.item_count === 2)
      ok('新建词表并去重', `id=${created.json.id}, item_count=${created.json.item_count}`);
    else ko('新建词表并去重', `HTTP ${created.status} ${String(created.text).slice(0, 120)}`);
    const wlId = created.json && created.json.id;

    const userName = '阶段22词表_改_' + stamp;
    const renamed = await api('PUT', `/api/wordlists/${wlId}`, { title: userName });
    const afterRename = await api('GET', `/api/wordlists/${wlId}`);
    if (renamed.status === 200 && afterRename.json && afterRename.json.title === userName && afterRename.json.source === 'user')
      ok('重命名词表', `title=${afterRename.json.title}`);
    else ko('重命名词表', `HTTP ${renamed.status} ${String(renamed.text).slice(0, 120)}`);

    const added = await api('POST', `/api/wordlists/${wlId}/items`, { items: [{ word: 'cherry' }, { word: 'APPLE' }] });
    if (added.status === 200 && added.json.added === 1 && added.json.skipped === 1)
      ok('追加词条去重', `added=${added.json.added}, skipped=${added.json.skipped}`);
    else ko('追加词条去重', `HTTP ${added.status} ${String(added.text).slice(0, 120)}`);

    const fullItems = await api('GET', `/api/wordlists/${wlId}/items?size=50`);
    if (fullItems.json && fullItems.json.total === 3)
      ok('追加后词条数 3', `total=${fullItems.json.total}`);
    else ko('追加后词条数 3', JSON.stringify(fullItems.json && fullItems.json.total));

    const cherry = (fullItems.json.data || []).find(i => i.word === 'cherry');
    const removed = await api('DELETE', `/api/wordlists/${wlId}/items/${cherry && cherry.item_id}`);
    const afterRemove = await api('GET', `/api/wordlists/${wlId}/items?size=50`);
    if (removed.status === 200 && afterRemove.json.total === 2)
      ok('移除词条', `total=${afterRemove.json.total}`);
    else ko('移除词条', `HTTP ${removed.status} ${String(removed.text).slice(0, 120)}`);

    const copied = await api('POST', `/api/wordlists/${wlId}/copy`);
    if (copied.status === 200 && copied.json && copied.json.id && copied.json.item_count === 2)
      ok('复制词表', `new_id=${copied.json.id}`);
    else ko('复制词表', `HTTP ${copied.status} ${String(copied.text).slice(0, 120)}`);
    const copyId = copied.json && copied.json.id;

    if (copyId) {
      const delCopy = await api('DELETE', `/api/wordlists/${copyId}`);
      if (delCopy.status === 200) ok('删除副本');
      else ko('删除副本', `HTTP ${delCopy.status}`);
    }

    const delOrig = await api('DELETE', `/api/wordlists/${wlId}`);
    const gone = await api('GET', `/api/wordlists/${wlId}`);
    if (delOrig.status === 200 && gone.status === 404)
      ok('删除词表后详情 404', `HTTP ${gone.status}`);
    else ko('删除词表后详情 404', `delete=${delOrig.status}, get=${gone.status}`);

    // 未登录访问写接口
    const anonRes = await new Promise(resolve => {
      const data = JSON.stringify({ title: 'x' });
      const req = http.request({ host: API_HOST, port: API_PORT, method: 'POST', path: '/api/wordlists',
        headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) }, timeout: 10000 }, res => {
        const chunks = []; res.on('data', c => chunks.push(c));
        res.on('end', () => resolve({ status: res.statusCode }));
      });
      req.on('error', () => resolve({ status: 0 }));
      req.write(data); req.end();
    });
    if (anonRes.status === 401) ok('未登录创建词表返回 401');
    else ko('未登录创建词表返回 401', `期望401，实际 ${anonRes.status}`);

    // ---- 3. 范围包 ----
    console.log('[3] 范围包');
    const units = [
      { textbook: '人教版（2019）', volume: '必修第一册', unit: 'Welcome Unit' },
      { textbook: '人教版（2019）', volume: '必修第一册', unit: 'Unit 1' }
    ];
    const range1 = await api('POST', '/api/wordlists/range-package', { units });
    if (range1.status === 200 && range1.json && range1.json.id && range1.json.item_count > 50 && range1.json.existing === false)
      ok('范围包合并生成', `id=${range1.json.id}, item_count=${range1.json.item_count}`);
    else ko('范围包合并生成', `HTTP ${range1.status} ${String(range1.text).slice(0, 120)}`);
    const rangeId = range1.json && range1.json.id;
    const range2 = await api('POST', '/api/wordlists/range-package', { units });
    if (range2.status === 200 && range2.json && range2.json.id === rangeId && range2.json.existing === true)
      ok('同范围重复生成幂等', `id=${range2.json.id}, existing=${range2.json.existing}`);
    else ko('同范围重复生成幂等', `HTTP ${range2.status} ${String(range2.text).slice(0, 120)}`);

    const rangeList = await api('GET', '/api/wordlists?source=range');
    const userList = await api('GET', '/api/wordlists?source=user');
    const inRange = (rangeList.json.data || []).some(l => l.id === rangeId && l.scope_key);
    const notInUser = !(userList.json.data || []).some(l => l.id === rangeId);
    if (inRange && notInUser) ok('范围包独立分组（不混入我的词表）');
    else ko('范围包独立分组（不混入我的词表）', `inRange=${inRange}, notInUser=${notInUser}`);

    if (rangeId) {
      const delRange = await api('DELETE', `/api/wordlists/${rangeId}`);
      if (delRange.status === 200) ok('删除范围包');
      else ko('删除范围包', `HTTP ${delRange.status}`);
    }

    // ---- 4. 我的单词与一键导入 ----
    console.log('[4] 我的单词与一键导入');
    const wordA = 'wl22a_' + stamp;
    const wordB = 'wl22b_' + stamp;
    const kpA = await api('POST', '/api/kp', { user_input_content: wordA });
    const kpB = await api('POST', '/api/kp', { user_input_content: wordB });
    if (kpA.json && kpB.json && kpA.json.id && kpB.json.id) ok('准备知识点', `ids=${kpA.json.id},${kpB.json.id}`);
    else ko('准备知识点', `HTTP ${kpA.status}/${kpB.status}`);

    const mine = await api('GET', `/api/wordlists/my-words?size=200&q=${encodeURIComponent(wordA)}`);
    if (mine.status === 200 && mine.json.total === 1)
      ok('我的单词包含新知识点', `total=${mine.json.total}`);
    else ko('我的单词包含新知识点', `HTTP ${mine.status} ${String(mine.text).slice(0, 120)}`);

    // 把 A 加入一个单词本 → 应从「我的单词」消失
    const book = await api('POST', '/api/wordbook', { name: '阶段22单词本_' + stamp });
    const bookId = book.json && book.json.id;
    const addEntry = await api('POST', `/api/wordbook/${bookId}/entries`, { knowledge_ids: [kpA.json.id] });
    const afterRef = await api('GET', `/api/wordlists/my-words?size=200&q=${encodeURIComponent(wordA)}`);
    if (addEntry.status === 200 && afterRef.json.total === 0)
      ok('已归入单词本的知识点从我的单词排除', `total=${afterRef.json.total}`);
    else ko('已归入单词本的知识点从我的单词排除', `add=${addEntry.status}, total=${afterRef.json.total}`);

    const imported = await api('POST', '/api/wordlists/my-words/import', { name: '阶段22导入_' + stamp });
    if (imported.status === 200 && imported.json && imported.json.wordbook_id && imported.json.added >= 1)
      ok('一键导入全部我的单词', `wordbook=${imported.json.wordbook_id}, added=${imported.json.added}`);
    else ko('一键导入全部我的单词', `HTTP ${imported.status} ${String(imported.text).slice(0, 120)}`);

    const afterImport = await api('GET', `/api/wordlists/my-words?size=200`);
    if (afterImport.json && afterImport.json.total === 0)
      ok('导入后我的单词清空', `total=${afterImport.json.total}`);
    else ko('导入后我的单词清空', `total=${afterImport.json.total}`);

    const again = await api('POST', '/api/wordlists/my-words/import', {});
    if (again.status === 400) ok('无剩余单词时再次导入返回 400');
    else ko('无剩余单词时再次导入返回 400', `期望400，实际 ${again.status}`);

    // 导入到他人单词本应 404（先补一个未归本的知识点，确保走归属校验）
    await api('POST', '/api/kp', { user_input_content: 'wl22c_' + stamp });
    const otherName = 'wl22u_' + stamp;
    await api('POST', '/api/auth/register', { username: otherName, password: 'pass1234', display_name: otherName });
    const otherLogin = await api('POST', '/api/auth/login', { username: otherName, password: 'pass1234' });
    const otherCookie = (otherLogin.headers['set-cookie'] || []).join(';').match(/eng_session=([0-9a-f]+)/);
    const otherTok = otherCookie && otherCookie[1];
    const otherBook = await api('POST', '/api/wordbook', { name: '他人单词本_' + stamp }, otherTok);
    const crossImport = await api('POST', '/api/wordlists/my-words/import', { wordbook_id: otherBook.json.id });
    if (otherTok && crossImport.status === 404) ok('导入他人单词本返回 404');
    else ko('导入他人单词本返回 404', `HTTP ${crossImport.status} ${String(crossImport.text).slice(0, 120)}`);

    // ---- 5. AI 生成词表 ----
    console.log('[5] AI 生成词表');
    const gen = await api('POST', '/api/wordlists/generate', { topic: '校园生活', count: 10, level: 'easy' });
    if (gen.status === 202 && gen.json && gen.json.task_id)
      ok('提交生成任务', `task_id=${gen.json.task_id}`);
    else ko('提交生成任务', `HTTP ${gen.status} ${String(gen.text).slice(0, 120)}`);

    const task = await waitTask(gen.json && gen.json.task_id, 60000);
    if (task.status === 'done' && task.result_ref) ok('生成任务完成并回写 result_ref', `ref=${task.result_ref}`);
    else ko('生成任务完成并回写 result_ref', `status=${task && task.status}, ref=${task && task.result_ref}`);

    if (task.result_ref) {
      const aiList = await api('GET', `/api/wordlists/${task.result_ref}`);
      const aiItems = await api('GET', `/api/wordlists/${task.result_ref}/items?size=50`);
      const titleOk = aiList.json && aiList.json.title.indexOf('校园生活') >= 0;
      const countOk = aiItems.json && aiItems.json.total === 10;
      if (titleOk && countOk) ok('AI 词表落库', `title=${aiList.json.title}, words=${aiItems.json.total}`);
      else ko('AI 词表落库', `title=${aiList.json && aiList.json.title}, words=${aiItems.json && aiItems.json.total}`);
      await api('DELETE', `/api/wordlists/${task.result_ref}`);
    }

    // ---- 6. 每日限额与队列满（独立后端） ----
    console.log('[6] 每日限额与队列满');
    aux = startBackend(AUX_PORT, 'success', { ENG_WORDLIST_DAILY_LIMIT: '2', ENG_AI_QUEUE_MAX: '1' });
    await waitHealth(AUX_PORT);

    runMysql("INSERT INTO ai_tasks (task_type,status,user_id,kind,lane,total,progress,retry_count,"
      + "next_run_at,update_time) VALUES ('exercise','queued',2,'exercise','exercise',0,0,0,"
      + "DATE_ADD(NOW(), INTERVAL 1 HOUR),NOW());");
    const placeholderId = runMysql("SELECT MAX(id) FROM ai_tasks;");
    const full = await auxApi(AUX_PORT, 'POST', '/api/wordlists/generate', { topic: '队列满', count: 5 });
    if (full.status === 429) ok('队列满返回 429');
    else ko('队列满返回 429', `期望429，实际 ${full.status} ${String(full.text).slice(0, 120)}`);
    runMysql(`DELETE FROM ai_tasks WHERE id=${Number(placeholderId)};`);

    const limitFirst = await auxApi(AUX_PORT, 'POST', '/api/wordlists/generate', { topic: '限额', count: 5 });
    if (limitFirst.status === 202) ok('限额内首次生成 202');
    else ko('限额内首次生成 202', `HTTP ${limitFirst.status} ${String(limitFirst.text).slice(0, 120)}`);
    const second = await auxApi(AUX_PORT, 'POST', '/api/wordlists/generate', { topic: '限额2', count: 5 });
    if (second.status === 429) ok('超出每日限额返回 429');
    else ko('超出每日限额返回 429', `期望429，实际 ${second.status} ${String(second.text).slice(0, 120)}`);

    // ---- 7. 注销级联 ----
    console.log('[7] 注销级联');
    const cascadeName = 'wl22c_' + stamp;
    await api('POST', '/api/auth/register', { username: cascadeName, password: 'pass1234', display_name: cascadeName });
    const cascadeLogin = await api('POST', '/api/auth/login', { username: cascadeName, password: 'pass1234' });
    const ckMatch = (cascadeLogin.headers['set-cookie'] || []).join(';').match(/eng_session=([0-9a-f]+)/);
    const ck = ckMatch && ckMatch[1];
    const me = await api('GET', '/api/auth/me', null, ck);
    const cuid = me.json && me.json.user_id;
    const cList = await api('POST', '/api/wordlists', { title: '注销词表_' + stamp, items: [{ word: 'zeta' }, { word: 'eta' }] }, ck);
    const cWl = cList.json && cList.json.id;
    runMysql(`INSERT INTO ai_tasks (task_type,status,user_id,kind,lane,total,progress,retry_count,result_ref,finished_at,update_time)
      VALUES ('wordlist_generate','done',${Number(cuid)},'wordlist_generate','wordlist',1,100,0,'${Number(cWl)}',NOW(),NOW());`);
    const deregi = await api('POST', '/api/auth/deregister', null, ck);
    const leftLists = Number(runMysql(`SELECT COUNT(*) FROM wordlists WHERE user_id=${Number(cuid)};`));
    const leftItems = Number(runMysql(`SELECT COUNT(*) FROM wordlist_items WHERE wordlist_id=${Number(cWl)};`));
    const leftTasks = Number(runMysql(`SELECT COUNT(*) FROM ai_tasks WHERE user_id=${Number(cuid)};`));
    if (deregi.status === 200 && cWl && leftLists === 0 && leftItems === 0 && leftTasks === 0)
      ok('注销级联清理词表/词条/AI 任务', `lists=${leftLists}, items=${leftItems}, tasks=${leftTasks}`);
    else ko('注销级联清理词表/词条/AI 任务',
      `dereg=${deregi.status}, lists=${leftLists}, items=${leftItems}, tasks=${leftTasks}`);

    // ---- 8. 前端静态资源 ----
    console.log('[8] 前端静态资源');
    const page = await request('GET', '/pages/wordbook.html', null, null);
    const groups = ['builtin', 'user', 'range', 'wordbook', 'mywords'].filter(g => page.text.indexOf('data-group="' + g + '"') >= 0);
    if (page.status === 200 && groups.length === 5) ok('页面含 5 个分组', groups.join('/'));
    else ko('页面含 5 个分组', `HTTP ${page.status}, groups=${groups.join('/')}`);

    const js = await request('GET', '/static/wordlib.js', null, null);
    if (js.status === 200 && js.text.indexOf('/api/wordlists/my-words/import') >= 0 && js.text.indexOf('range-package') >= 0)
      ok('前端脚本接入导入与范围包接口');
    else ko('前端脚本接入导入与范围包接口', `HTTP ${js.status}`);

    const xlsx = await request('GET', '/static/xlsx.full.min.js', null, null);
    if (xlsx.status === 200 && xlsx.text.length > 100000) ok('本地 SheetJS 已就绪', `${Math.round(xlsx.text.length / 1024)}KB`);
    else ko('本地 SheetJS 已就绪', `HTTP ${xlsx.status}, len=${xlsx.text.length}`);

    const remote = /cdn[.\-]|unpkg|jsdelivr/i.test(page.text + js.text);
    if (!remote) ok('页面无 CDN 外部依赖');
    else ko('页面无 CDN 外部依赖', '发现外部 CDN 引用');

  } catch (e) {
    ko('套件异常', String((e && e.stack) || e).slice(0, 300));
  } finally {
    if (aux) await stopBackend(aux.proc);
  }

  console.log(`\n阶段二十二结果: ${pass} 通过 / ${fail} 失败 / ${skipped} 跳过`);
  if (fail > 0) {
    console.log('失败项:');
    for (const f of failures) console.log('  - ' + f.name + ': ' + f.msg);
    process.exit(1);
  }
}

main();
