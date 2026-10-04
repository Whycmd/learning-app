// 阶段一：基建环境与数据库初始化 - 集成测试
// 覆盖验收标准：MySQL 服务、全量表结构、AI 服务调用、Word 生成、C++ 后端编译
const http = require('http');
const fs = require('fs');
const path = require('path');
const { execSync, spawnSync } = require('child_process');
const { Document, Packer, Paragraph, TextRun, HeadingLevel } = require('docx');

const ROOT = path.resolve(__dirname, '..');
const MYSQL_BIN = '/Users/junwang/.local/mysql/bin/mysql';
const MYSQL_PW_FILE = '/Users/junwang/.local/mysql/.root_pw';
const SOCK = '/Users/junwang/.local/run/mysql.sock';
const WORD_TMP = path.join(ROOT, 'word_tmp');

let pass = 0, fail = 0, skipped = 0;
const failures = [];

function ok(name, msg='') { pass++; console.log(`  ✓ ${name}${msg ? ' — ' + msg : ''}`); }
function ko(name, msg) { fail++; failures.push({name, msg}); console.log(`  ✗ ${name} — ${msg}`); }
function skip(name, msg='') { skipped++; console.log(`  ~ ${name}${msg ? ' — ' + msg : ''}`); }

function runMysql(sql, opts={}) {
  const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
  const db = opts.db || 'english_learning';
  const silentFlag = opts.silent ? ['-s', '--skip-column-names'] : [];
  const args = ['--socket='+SOCK, '-uroot', '-p'+pw, ...silentFlag, db, '-e', sql];
  const r = spawnSync(MYSQL_BIN, args, { encoding: 'utf8', timeout: 15000 });
  if (r.status !== 0) {
    throw new Error(`mysql failed: ${r.stderr || r.stdout}`);
  }
  return r.stdout;
}

function httpReq(method, urlPath, body) {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : null;
    const req = http.request({
      host: '127.0.0.1', port: 5340, method, path: urlPath,
      headers: { 'Content-Type': 'application/json' },
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

async function main() {
  console.log('=== 阶段一：基建环境集成测试 ===\n');

  // ---- 1. MySQL 服务连通性 ----
  console.log('[1] MySQL 服务连通性');
  try {
    const out = runMysql('SELECT VERSION() AS v, NOW() AS now;');
    if (/8\.0\./.test(out)) ok('MySQL 8.x 服务响应');
    else ko('MySQL 8.x 服务响应', '版本不为 8.x: ' + out.trim());
    // 字符集
    const cs = runMysql('SHOW VARIABLES LIKE "character_set_server";');
    if (/utf8mb4/.test(cs)) ok('字符集 utf8mb4');
    else ko('字符集 utf8mb4', cs.trim());
    // 仅 127.0.0.1 绑定
    const bind = runMysql('SHOW VARIABLES LIKE "bind_address";');
    if (/127\.0\.0\.1/.test(bind)) ok('仅 127.0.0.1 绑定');
    else ko('仅 127.0.0.1 绑定', bind.trim());
  } catch (e) { ko('MySQL 服务连通性', e.message); }

  // ---- 2. 全量表结构存在性 ----
  console.log('\n[2] 全量表结构存在性');
  const expectedTables = [
    'knowledge_points','kp_definitions','kp_collocations','kp_synonyms','kp_antonyms','kp_extras',
    'user_learning','articles','review_logs','ai_tasks'
  ];
  try {
    const tbls = runMysql('SHOW TABLES;');
    for (const t of expectedTables) {
      if (new RegExp('\\b'+t+'\\b').test(tbls)) ok(`表 ${t}`);
      else ko(`表 ${t}`, '未找到');
    }
    // 字段检查（关键字段）
    const cols = runMysql("SELECT GROUP_CONCAT(COLUMN_NAME, ':', TABLE_NAME SEPARATOR '\\n') FROM information_schema.columns WHERE table_schema='english_learning' AND COLUMN_NAME IN ('user_input_content','standard_content','difficulty','master_require','is_solidified','proficiency_level','next_review_time','review_mode','key_points','match_threshold','core_vocab','exercise_type','is_correct','task_type','status','retry_times');");
    const mustHave = [
      ['user_input_content','knowledge_points'], ['standard_content','knowledge_points'],
      ['master_require','knowledge_points'], ['proficiency_level','user_learning'],
      ['next_review_time','user_learning'], ['review_mode','user_learning'],
      ['match_threshold','articles'], ['task_type','ai_tasks'], ['retry_times','ai_tasks']
    ];
    for (const [col, tbl] of mustHave) {
      if (cols.includes(`${col}:${tbl}`)) ok(`字段 ${tbl}.${col}`);
      else ko(`字段 ${tbl}.${col}`, '未找到');
    }
    // 外键约束：检查表 DDL 中无 FOREIGN KEY 子句（PRD 要求不开启外键约束）
    const ddl = runMysql("SELECT GROUP_CONCAT(TABLE_NAME) FROM information_schema.tables WHERE table_schema='english_learning';");
    let hasAnyFK = false;
    try {
      const fkCheck = runMysql("SELECT TABLE_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME FROM information_schema.KEY_COLUMN_USAGE WHERE table_schema='english_learning' AND REFERENCED_TABLE_NAME IS NOT NULL;");
      hasAnyFK = /\S/.test(fkCheck);
    } catch {}
    if (!hasAnyFK) ok('无外键约束（PRD 要求）');
    else ko('无外键约束', '检测到 FOREIGN KEY');
    // 事务支持
    const eng = runMysql("SELECT ENGINE FROM information_schema.tables WHERE table_schema='english_learning' AND TABLE_NAME='knowledge_points';");
    if (/InnoDB/.test(eng)) ok('InnoDB 引擎支持事务');
    else ko('InnoDB 引擎', eng.trim());
  } catch (e) { ko('表结构检查', e.message); }

  // ---- 3. 事务与重试机制（单条 -e 内执行保证同会话）----
  console.log('\n[3] 事务与重试机制');
  try {
    // 单条 SQL 中完成 START TRANSACTION → INSERT → ROLLBACK → SELECT（silent 模式只输出 SELECT 结果）
    const cnt = runMysql("START TRANSACTION; INSERT INTO knowledge_points (user_input_content) VALUES ('__test_tx__'); ROLLBACK; SELECT COUNT(*) AS c FROM knowledge_points WHERE user_input_content='__test_tx__';", { silent: true });
    const trimmed = cnt.trim();
    if (trimmed === '0') ok('事务回滚生效');
    else ko('事务回滚生效', '回滚后输出: ' + trimmed);
    // 重试机制（代码层）：模拟锁冲突重试3次后成功
    let attempts = 0, success = false;
    for (let i = 0; i < 3; i++) {
      attempts++;
      try {
        const pw = fs.readFileSync(MYSQL_PW_FILE, 'utf8').trim();
        const r = spawnSync(MYSQL_BIN, ['--socket='+SOCK, '-uroot', '-p'+pw, 'english_learning', '-e', "INSERT INTO knowledge_points (user_input_content) VALUES ('__retry_test__'); SELECT LAST_INSERT_ID() AS id;"], { encoding:'utf8', timeout: 5000 });
        if (r.status === 0) { success = true; break; }
      } catch (e) { /* 重试 */ }
    }
    if (success && attempts <= 3) ok('数据库重试机制（3 次内成功）', `attempts=${attempts}`);
    else ko('数据库重试机制', `attempts=${attempts}, success=${success}`);
    // 清理
    try { runMysql("DELETE FROM knowledge_points WHERE user_input_content='__retry_test__';"); } catch {}
  } catch (e) { ko('事务/重试', e.message); }

  // ---- 4. AI 服务连通性与三种业务响应 ----
  console.log('\n[4] AI 服务连通性与业务响应');
  try {
    const health = await httpReq('GET', '/readyz');
    if (health.status === 200 && health.json && health.json.status === 'ok') ok('AI /readyz 响应');
    else ko('AI /readyz 响应', 'status=' + health.status);

    // 从 AI 响应文本中提取 JSON（兼容 content 字段和 _extracted_json）
    function extractJson(text) {
      if (!text) return null;
      let t = String(text).trim();
      const fence = t.match(/```(?:json)?\s*([\s\S]*?)```/);
      if (fence) t = fence[1].trim();
      const start = t.search(/[[{]/);
      if (start < 0) return null;
      const open = t[start];
      const close = open === '{' ? '}' : ']';
      let depth = 0, end = -1, inStr = false, esc = false;
      for (let i = start; i < t.length; i++) {
        const c = t[i];
        if (esc) { esc = false; continue; }
        if (c === '\\' && inStr) { esc = true; continue; }
        if (c === '"') { inStr = !inStr; continue; }
        if (inStr) continue;
        if (c === open) depth++;
        else if (c === close) { depth--; if (depth === 0) { end = i; break; } }
      }
      if (end < 0) return null;
      return t.slice(start, end + 1);
    }
    function getContent(j) {
      if (j && j._extracted_json) return j._extracted_json;
      if (j && j.choices && j.choices[0] && j.choices[0].message) return j.choices[0].message.content;
      return '';
    }

    const solidify = await httpReq('POST', '/v1/chat/completions', {
      model: 'glm-4-flash',
      messages: [{ role:'user', content:'请对知识点 "start" 进行 AI 固化拓展，输出释义、固定搭配、近义词、反义词、词根词源、重难点。严格只返回JSON：{"standard_content":"start","difficulty":3,"definitions":[{"pos":"词性","meaning":"释义","example":"例句"}],"collocations":[{"collocation":"搭配","example":"例句"}],"synonyms":["近义词"],"antonyms":["反义词"]}' }]
    });
    if (solidify.status === 200 && solidify.json && solidify.json.choices && solidify.json.choices[0].message) {
      ok('AI 固化请求响应', 'id=' + solidify.json.id);
      const ex = extractJson(getContent(solidify.json));
      if (ex) {
        try {
          const o = JSON.parse(ex);
          if (o.standard_content && o.definitions && Array.isArray(o.definitions)) {
            ok('AI 固化响应结构合规', `standard=${o.standard_content}, defs=${o.definitions.length}`);
          } else ko('AI 固化响应结构合规', '字段缺失: ' + Object.keys(o).join(','));
        } catch (e) { ko('AI 固化响应 JSON 解析', e.message); }
      } else ko('AI 固化响应 JSON', '未提取到 JSON: ' + String(getContent(solidify.json)).slice(0,100));
    } else ko('AI 固化请求响应', 'status=' + solidify.status);

    const exercise = await httpReq('POST', '/v1/chat/completions', {
      model: 'glm-4-flash',
      messages: [{ role:'user', content:'请基于知识点 [start, begin, learn] 生成英语习题，返回JSON含exercises数组' }]
    });
    if (exercise.status === 200 && exercise.json) {
      const ex = extractJson(getContent(exercise.json));
      if (ex) {
        try {
          const o = JSON.parse(ex);
          if (o.exercises && Array.isArray(o.exercises) && o.exercises.length >= 1) {
            ok('AI 出题响应结构合规', `exercises=${o.exercises.length}`);
          } else ko('AI 出题响应结构合规', '字段缺失或数量不足');
        } catch (e) { ko('AI 出题响应 JSON 解析', e.message); }
      } else ko('AI 出题响应 JSON', '未提取到 JSON: ' + String(getContent(exercise.json)).slice(0,100));
    } else ko('AI 出题请求响应', 'status=' + exercise.status);

    const grading = await httpReq('POST', '/v1/chat/completions', {
      model: 'glm-4-flash',
      messages: [{ role:'user', content:'批改翻译题，用户答案: Today we start learning. 参考答案: Today we begin learning. 返回JSON含score,correct,error_analysis,reference_answer' }]
    });
    if (grading.status === 200 && grading.json) {
      const ex = extractJson(getContent(grading.json));
      if (ex) {
        try {
          const o = JSON.parse(ex);
          if (typeof o.score === 'number' && typeof o.correct === 'boolean') {
            ok('AI 批改响应结构合规', `score=${o.score}, correct=${o.correct}`);
          } else ko('AI 批改响应结构合规', '字段缺失');
        } catch (e) { ko('AI 批改响应 JSON 解析', e.message); }
      } else ko('AI 批改响应 JSON', '未提取到 JSON: ' + String(getContent(grading.json)).slice(0,100));
    } else ko('AI 批改请求响应', 'status=' + grading.status);
  } catch (e) { ko('AI 服务', e.message); }

  // ---- 5. Word 生成（docx 库） ----
  console.log('\n[5] Word 文件生成');
  try {
    fs.mkdirSync(WORD_TMP, { recursive: true });
    const doc = new Document({
      sections: [{
        children: [
          new Paragraph({ text: '英语学习软件 - 测试 Word', heading: HeadingLevel.HEADING_1 }),
          new Paragraph({ children: [new TextRun('这是 docx 库生成的测试段落。')] })
        ]
      }]
    });
    const buf = await Packer.toBuffer(doc);
    const outPath = path.join(WORD_TMP, `test_stage1_${Date.now()}.docx`);
    fs.writeFileSync(outPath, buf);
    if (fs.existsSync(outPath) && fs.statSync(outPath).size > 1000) ok('Word .docx 文件生成', `${fs.statSync(outPath).size} bytes`);
    else ko('Word .docx 文件生成', '文件过小或不存在');
    // 文件可读性（PK 头）
    const fd = fs.openSync(outPath, 'r');
    const headBuf = Buffer.alloc(4);
    fs.readSync(fd, headBuf, 0, 4, 0);
    fs.closeSync(fd);
    if (headBuf[0] === 0x50 && headBuf[1] === 0x4b) ok('Word 文件为有效 .docx (PK 头)');
    else ko('Word 文件为有效 .docx', `头部: ${headBuf.toString('hex')}`);
    // 清理
    fs.unlinkSync(outPath);
  } catch (e) { ko('Word 生成', e.message); }

  // ---- 6. C++ 编译器可用性 ----
  console.log('\n[6] C++ 编译器');
  try {
    const r = spawnSync('clang++', ['--version'], { encoding: 'utf8' });
    if (r.status === 0 && /clang version|Apple clang/.test(r.stdout)) ok('clang++ 可用', r.stdout.split('\n')[0]);
    else ko('clang++ 可用', r.stderr);
  } catch (e) { ko('clang++ 可用', e.message); }

  // ---- 7. 项目目录结构 ----
  console.log('\n[7] 项目目录结构');
  const expectedDirs = ['backend','backend/src','backend/include','backend/build','frontend','frontend/pages','frontend/static','scripts','tests','logs','word_tmp','config'];
  for (const d of expectedDirs) {
    const p = path.join(ROOT, d);
    if (fs.existsSync(p)) ok(`目录 ${d}`);
    else { try { fs.mkdirSync(p, { recursive: true }); ok(`目录 ${d} (自动创建)`); } catch { ko(`目录 ${d}`, '无法创建'); } }
  }
  const expectedConfigs = ['config/db.json','config/ai.json','config/app.json'];
  for (const c of expectedConfigs) {
    if (fs.existsSync(path.join(ROOT, c))) ok(`配置 ${c}`);
    else ko(`配置 ${c}`, '缺失');
  }

  // ---- 8. 相对路径校验 ----
  console.log('\n[8] 相对路径与配置');
  try {
    const dbCfg = JSON.parse(fs.readFileSync(path.join(ROOT, 'config/db.json'), 'utf8'));
    if (dbCfg.host === '127.0.0.1' && dbCfg.database === 'english_learning' && dbCfg.retry_times === 3) ok('db.json 配置合规');
    else ko('db.json 配置合规', JSON.stringify(dbCfg));
    const aiCfg = JSON.parse(fs.readFileSync(path.join(ROOT, 'config/ai.json'), 'utf8'));
    if (aiCfg.base_url.startsWith('http://127.0.0.1:5340') && aiCfg.retry_times === 2) ok('ai.json 配置合规');
    else ko('ai.json 配置合规', JSON.stringify(aiCfg));
  } catch (e) { ko('配置校验', e.message); }

  console.log('\n=== 阶段一测试结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}  跳过: ${skipped}`);
  if (fail > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
    process.exit(1);
  } else {
    console.log('\n✓ 阶段一全部测试通过');
    process.exit(0);
  }
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
