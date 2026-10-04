// 阶段五：前端页面 & 全链路联调 - 集成测试
// 覆盖验收标准：
//   1. 五大核心页面加载
//   2. 全业务闭环联调：录入→固化→选题→出题→答题→批改→学情更新
const http = require('http');
const fs = require('fs');
const { spawnSync } = require('child_process');

const MYSQL_BIN = '/Users/junwang/.local/mysql/bin/mysql';
const MYSQL_PW_FILE = '/Users/junwang/.local/mysql/.root_pw';
const SOCK = '/Users/junwang/.local/run/mysql.sock';

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
        try { resolve({ status:res.statusCode, json:JSON.parse(text), text, headers:res.headers }); }
        catch { resolve({ status:res.statusCode, json:null, text, headers:res.headers }); }
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
  console.log('=== 阶段五：前端页面 & 全链路联调测试 ===\n');

  // ---- 1. 五大核心页面加载 ----
  console.log('[1] 五大核心页面加载');
  try {
    const pages = [
      { name: '首页', path: '/' },
      { name: '登录注册', path: '/pages/login.html', noNav: true },
      { name: '知识点录入', path: '/pages/kp-entry.html' },
      { name: '知识点详情', path: '/pages/kp-detail.html' },
      { name: '每日复习', path: '/pages/review.html' },
      { name: '每日小测', path: '/pages/daily-quiz.html' },
      { name: '单词本', path: '/pages/wordbook.html' },
      { name: '文章阅读', path: '/pages/articles.html' },
      { name: '学情中心', path: '/pages/learning-center.html' }
    ];
    for (const p of pages) {
      const r = await req('GET', p.path);
      if (r.status === 200 && r.text.includes('<html')) ok(p.name + ' 加载');
      else ko(p.name + ' 加载', `status=${r.status}`);
      // 新视觉结构：字体样式 + 导航注入锚点（T102 nav.js 统一注入）
      // 首页 = T103 学习仪表盘（narr + hall + caisson）；
      // T104 知识点录入/详情 = 圣殿构件（section-title + wing-card）；
      // T105 每日复习 = 圣殿构件 + 两栏栅格（section-title + wing-card + review-layout）；
      // T106 每日小测 = 圣殿构件（section-title + wing-card + quiz-）；
      // T107 单词本 = 圣殿构件 + 抽屉（section-title + wing-card + drawer-overlay）；
      // 其余功能副页 = hero 横幅（待 T108–T109 逐步翻新）
      let visual = false, label = '';
      if (p.path === '/')
      {
        visual = r.text.includes('fonts.css') && r.text.includes('topbar-slot')
               && r.text.includes('narr') && r.text.includes('hall') && r.text.includes('caisson');
        label = '仪表盘';
      }
      else if (p.path === '/pages/kp-entry.html' || p.path === '/pages/kp-detail.html')
      {
        visual = r.text.includes('fonts.css') && r.text.includes('topbar-slot')
               && r.text.includes('section-title') && r.text.includes('wing-card');
        label = 'T104-圣殿构件';
      }
      else if (p.path === '/pages/review.html')
      {
        visual = r.text.includes('fonts.css') && r.text.includes('topbar-slot')
               && r.text.includes('section-title') && r.text.includes('wing-card')
               && r.text.includes('review-layout');
        label = 'T105-圣殿构件+两栏';
      }
      else if (p.path === '/pages/daily-quiz.html')
      {
        visual = r.text.includes('fonts.css') && r.text.includes('topbar-slot')
               && r.text.includes('section-title') && r.text.includes('wing-card')
               && r.text.includes('quiz-score');
        label = 'T106-圣殿构件+小测';
      }
      else if (p.path === '/pages/wordbook.html')
      {
        visual = r.text.includes('fonts.css') && r.text.includes('topbar-slot')
               && r.text.includes('section-title') && r.text.includes('wing-card')
               && r.text.includes('drawer-overlay');
        label = 'T107-圣殿构件+抽屉';
      }
      else if (p.path === '/pages/learning-center.html')
      {
        visual = r.text.includes('fonts.css') && r.text.includes('topbar-slot')
               && r.text.includes('section-title') && r.text.includes('wing-card')
               && r.text.includes('lc-grid') && r.text.includes('lc-chart-wrap')
               && r.text.includes('drawer-overlay');
        label = 'T108-圣殿构件+两栏栅格+折线图+抽屉';
      }
      else if (p.path === '/pages/login.html')
      {
        visual = r.text.includes('fonts.css') && r.text.includes('auth-wrap')
               && r.text.includes('auth-card') && r.text.includes('auth-tabs')
               && r.text.includes('seal-deco') && r.text.includes('theme.js');
        label = 'T109-登录页+金色顶条+印章';
      }
      else if (p.path === '/pages/articles.html')
      {
        visual = r.text.includes('fonts.css') && r.text.includes('topbar-slot')
               && r.text.includes('section-title') && r.text.includes('wing-card')
               && r.text.includes('filter-chip') && r.text.includes('art-card')
               && r.text.includes('drawer-overlay');
        label = 'T109-圣殿构件+筛选+抽屉';
      }
      else
      {
        visual = r.text.includes('fonts.css') && r.text.includes('topbar-slot') && r.text.includes('hero');
        label = 'hero';
      }
      if (visual) ok(p.name + ' 视觉结构（fonts.css/topbar-slot/' + label + '）');
      else ko(p.name + ' 视觉结构', '缺少标记');
      // 设计系统 v2 + T102 导航：组件基座 + 主题脚本 + nav.js（login.html 无导航豁免）
      if (p.noNav) {
        if (r.text.includes('components.css') && r.text.includes('theme.js'))
          ok(p.name + ' 引入组件基座 + 主题');
        else ko(p.name + ' 组件/脚本引入', '缺少 components.css / theme.js');
      } else {
        if (r.text.includes('components.css') && r.text.includes('theme.js') && r.text.includes('nav.js'))
          ok(p.name + ' 引入组件基座 + 主题 + nav.js');
        else ko(p.name + ' 组件/脚本引入', '缺少 components.css / theme.js / nav.js');
      }
    }
    // CSS
    const css = await req('GET', '/static/style.css');
    if (css.status === 200 && css.headers['content-type'].includes('css')) ok('style.css 加载');
    else ko('style.css 加载', `status=${css.status}`);
    // 暗色主题变量层（T101 双主题）
    if (css.text && css.text.includes('data-theme="dark"')) ok('暗色主题变量层');
    else ko('暗色主题层', 'style.css 缺少 data-theme="dark" 覆盖层');
    // 组件基座
    const comps = await req('GET', '/static/components.css');
    if (comps.status === 200 && comps.headers['content-type'].includes('css')) ok('components.css 加载');
    else ko('components.css 加载', `status=${comps.status}`);
    // 主题切换脚本
    const themejs = await req('GET', '/static/theme.js');
    if (themejs.status === 200 && themejs.headers['content-type'].includes('javascript')) ok('theme.js 加载');
    else ko('theme.js 加载', `status=${themejs.status}`);
    // 全局导航脚本（T102 统一注入）
    const navjs = await req('GET', '/static/nav.js');
    if (navjs.status === 200 && navjs.headers['content-type'].includes('javascript')) ok('nav.js 加载');
    else ko('nav.js 加载', `status=${navjs.status}`);
    // fonts.css
    const fcss = await req('GET', '/static/fonts.css');
    if (fcss.status === 200 && fcss.headers['content-type'].includes('css')) ok('fonts.css 加载');
    else ko('fonts.css 加载', `status=${fcss.status}`);
    // 离线字体（woff2 必须以 font/woff2 提供）
    const fonts = ['fraunces-normal','fraunces-italic','karla-normal','karla-italic'];
    let fontOk = 0;
    for (const f of fonts) {
      const r = await req('GET', '/static/fonts/' + f + '.woff2');
      if (r.status === 200 && r.headers['content-type'] && r.headers['content-type'].includes('font/woff2')) fontOk++;
    }
    if (fontOk === 4) ok('4 个 woff2 离线字体（font/woff2）');
    else ko('woff2 字体', `${fontOk}/4 正确`);

    // ---- T110 收口：微交互/无障碍/响应式走查 ----
    // 焦点环
    if (css.text && css.text.includes('focus-visible')) ok('T110 焦点环 :focus-visible');
    else ko('T110 焦点环', 'style.css 缺少 :focus-visible');
    // 动效令牌（过渡属性统一使用 --dur-*）
    const durTokens = ['--dur-fast','--dur-mid','--dur-slow','--ease-out'];
    const allDurUsed = durTokens.every(t => css.text.includes(t) || comps.text.includes(t));
    if (allDurUsed) ok('T110 动效令牌全覆盖');
    else ko('T110 动效令牌', '非 --dur-* 令牌过渡值未清零');
    // 响应式断点
    if (css.text && css.text.includes('640px')) ok('T110 响应式断点 640px');
    else ko('T110 响应式断点', 'style.css 缺少 640px');
  } catch (e) { ko('页面加载', e.message); }

  // ---- 2. 全业务闭环联调 ----
  console.log('\n[2] 全业务闭环联调：录入→固化→选题→出题→答题→批改→学情更新');
  try {
    // 清理
    runMysql("DELETE FROM kp_extras; DELETE FROM kp_antonyms; DELETE FROM kp_synonyms; DELETE FROM kp_collocations; DELETE FROM kp_definitions; DELETE FROM user_learning; DELETE FROM knowledge_points;");

    // 步骤1: 录入
    console.log('  [2.1] 录入知识点');
    const kp1 = await req('POST', '/api/kp', { user_input_content: 'abandon', remark: '重点考察被动语态' });
    if (kp1.json && kp1.json.ok) ok('录入', `id=${kp1.json.id}`);
    else ko('录入', JSON.stringify(kp1.json).slice(0,100));
    const kp2 = await req('POST', '/api/kp', { user_input_content: 'benefit' });
    if (kp2.json && kp2.json.ok) ok('录入第二个', `id=${kp2.json.id}`);
    else ko('录入第二个', JSON.stringify(kp2.json).slice(0,100));

    // 步骤2: 固化
    console.log('  [2.2] AI 固化');
    const sol1 = await req('POST', `/api/kp/${kp1.json.id}/solidify`);
    if (sol1.json && (sol1.status === 200 || sol1.status === 201)) ok('固化第一个', `word=${!!sol1.json.word_path}`);
    else ko('固化第一个', JSON.stringify(sol1.json).slice(0,100));
    const sol2 = await req('POST', `/api/kp/${kp2.json.id}/solidify`);
    if (sol2.json && (sol2.status === 200 || sol2.status === 201)) ok('固化第二个');
    else ko('固化第二个', JSON.stringify(sol2.json).slice(0,100));

    // 步骤3: 学情创建 & 选题
    console.log('  [2.3] 学情创建 & 选题');
    const lr1 = await req('GET', `/api/learn/${kp1.json.id}`);
    if (lr1.json && lr1.json.proficiency_level === 0) ok('学情自动创建', 'L0');
    else ko('学情创建', JSON.stringify(lr1.json).slice(0,100));

    // 设置复习模式为按日
    await req('PUT', `/api/learn/${kp1.json.id}/mode`, { review_mode: 'daily' });
    await req('PUT', `/api/learn/${kp2.json.id}/mode`, { review_mode: 'daily' });

    // 将 next_review_time 设为过去（进入候选池）
    runMysql(`UPDATE user_learning SET next_review_time=DATE_SUB(NOW(), INTERVAL 1 DAY) WHERE knowledge_id IN (${kp1.json.id},${kp2.json.id})`);

    const pool = await req('GET', '/api/learn/candidates');
    if (pool.json && pool.json.total === 2) ok('候选池', `total=${pool.json.total}`);
    else ko('候选池', JSON.stringify(pool.json).slice(0,100));

    // 步骤4: 出题
    console.log('  [2.4] AI 出题');
    const ex = await req('POST', '/api/exercise/generate', { knowledge_ids: [kp1.json.id, kp2.json.id], mode: 'daily' });
    if (ex.json && ex.json.ok) ok('出题成功', `ex_id=${ex.json.exercise_id}`);
    else ko('出题', JSON.stringify(ex.json).slice(0,100));
    if (ex.json && ex.json.word_path && fs.existsSync(ex.json.word_path)) ok('习题 Word 生成');

    // 步骤5: 答题 & 批改
    console.log('  [2.5] 答题 & 批改');
    const grade = await req('POST', '/api/exercise/batch-grade', {
      answers: [
        { type: 'word_match', user_answer: 'A', correct_answer: 'A', knowledge_id: kp1.json.id },
        { type: 'word_form_fill', user_answer: 'wrong', correct_answer: 'abandon', knowledge_id: kp2.json.id }
      ]
    });
    if (grade.json && grade.json.ok) ok('批量批改', `correct=${grade.json.correct_count}/${grade.json.total_count}`);
    else ko('批量批改', JSON.stringify(grade.json).slice(0,100));

    // 步骤6: 学情更新验证
    console.log('  [2.6] 学情更新验证');
    const lr1After = await req('GET', `/api/learn/${kp1.json.id}`);
    if (lr1After.json && lr1After.json.error_times === 0) ok('答对不降权', `error_times=${lr1After.json.error_times}`);
    else ko('答对验证', JSON.stringify(lr1After.json).slice(0,100));
    const lr2After = await req('GET', `/api/learn/${kp2.json.id}`);
    if (lr2After.json && lr2After.json.error_times >= 1) ok('答错降权', `error_times=${lr2After.json.error_times}`);
    else ko('答错验证', JSON.stringify(lr2After.json).slice(0,100));

    // 步骤7: 完成复习更新下次时间
    console.log('  [2.7] 完成复习更新');
    const cr = await req('POST', `/api/learn/${kp1.json.id}/complete-review`);
    if (cr.json && cr.json.ok) ok('复习完成', `next=${cr.json.next_review_time}`);
    else ko('复习完成', JSON.stringify(cr.json).slice(0,100));

  } catch (e) { ko('全链路', e.message); }

  // ---- 3. 页面数据绑定验证 ----
  console.log('\n[3] 页面数据绑定验证');
  try {
    // 列表 API
    const list = await req('GET', '/api/kp?page=1&size=10');
    if (list.json && list.json.data && list.json.data.length > 0) ok('列表 API 返回数据');
    else ko('列表 API', '无数据');

    // 学情统计
    const stats = await req('GET', '/api/learn/stats');
    if (stats.json && stats.json.total > 0) ok('统计 API 返回数据', `total=${stats.json.total}`);
    else ko('统计 API', '无数据');

    // Word 下载
    const list2 = await req('GET', '/api/kp?page=1&size=1');
    if (list2.json && list2.json.data && list2.json.data[0]) {
      const kpId = list2.json.data[0].id;
      const exp = await req('POST', `/api/kp/${kpId}/export-word`);
      if (exp.json && exp.json.word_path) {
        const dl = await req('GET', '/api/word/download?path=' + encodeURIComponent(exp.json.word_path));
        if (dl.status === 200 && dl.text.length > 1000) ok('Word 下载');
        else ko('Word 下载', `status=${dl.status}`);
      }
    }
  } catch (e) { ko('数据绑定', e.message); }

  console.log('\n=== 阶段五测试结果 ===');
  console.log(`通过: ${pass}  失败: ${fail}`);
  if (fail > 0) {
    console.log('\n失败项:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.msg}`));
    process.exit(1);
  } else {
    console.log('\n✓ 阶段五全部测试通过');
    process.exit(0);
  }
}

main().catch(e => { console.error('FATAL:', e); process.exit(2); });
