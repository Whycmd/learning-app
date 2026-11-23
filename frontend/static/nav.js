// 全局导航 v2（v1.3.0-T102）
// 负责：注入 .topbar 导航壳层 · active 自动判定 · 激励区渲染 · 用户区渲染（合并旧 auth-nav.js）
// 9 页统一引入此脚本，不再手工写 <nav class="topnav">
(function ()
{
  'use strict';

  // ---------- 界面模式（v1.5.0-T004） ----------
  // function = 功能模式（随手学）；plan = 计划模式（长期课规）
  // <html data-ui-mode> 决定首页区块与顶栏入口的显隐
  const UI_MODE_KEY = 'eng_ui_mode';
  const UI_MODE_TEXT = { function: '功能模式', plan: '计划模式' };

  function readLocalUiMode()
  {
    try
    {
      const v = localStorage.getItem(UI_MODE_KEY);
      return v === 'plan' || v === 'function' ? v : 'function';
    }
    catch (e) { return 'function'; }
  }

  function writeLocalUiMode(mode)
  {
    try { localStorage.setItem(UI_MODE_KEY, mode); } catch (e) {}
  }

  // 首帧即写入，避免首页在脚本加载后才切换造成闪烁
  document.documentElement.setAttribute('data-ui-mode', readLocalUiMode());
  let uiModeLoggedIn = false;   // 由 renderUser 根据 /api/auth/me 校正

  // ---------- 匿名标识（v1.5.0-T001） ----------
  // 未登录用户在本地生成稳定假名，仅用于前端区分会话；本轮不上传、不落库
  const ANON_KEY_STORAGE = 'eng_anon_key';

  function ensureAnonKey()
  {
    try
    {
      if (localStorage.getItem(ANON_KEY_STORAGE)) return;
      const key = (typeof crypto !== 'undefined' && crypto.randomUUID)
        ? crypto.randomUUID()
        : 'anon-' + Date.now().toString(36) + '-' + Math.random().toString(36).slice(2, 10);
      localStorage.setItem(ANON_KEY_STORAGE, key);
    }
    catch (e) {}
  }
  ensureAnonKey();

  // ---------- 写操作 401 → 跳转登录页（带回跳地址） ----------
  function installAuthRedirect()
  {
    if (window.__authRedirectInstalled) return;
    window.__authRedirectInstalled = true;

    const origFetch = window.fetch.bind(window);
    const AUTH_PATHS = /\/api\/auth\/(login|register|logout|deregister)(\?|$)/;

    window.fetch = function (input, init)
    {
      const url = typeof input === 'string' ? input : (input && input.url) || '';
      const method = ((init && init.method) || (input && input.method) || 'GET').toUpperCase();
      const write = method === 'POST' || method === 'PUT' || method === 'DELETE' || method === 'PATCH';

      return origFetch(input, init).then(res =>
      {
        if (res.status === 401 && write && !AUTH_PATHS.test(url)
          && !/\/pages\/login\.html$/.test(location.pathname))
        {
          const back = location.pathname + location.search;
          location.assign('/pages/login.html?redirect=' + encodeURIComponent(back));
        }
        return res;
      });
    };
  }
  installAuthRedirect();

  // ---------- 导航结构定义 ----------

  const NAV_HTML = `
<header class="topbar">
  <nav class="topnav">
    <a href="/" class="topnav__brand">学习圣殿</a>

    <div class="topnav-dropdown">
    <button class="topnav-dropdown__trigger" data-group="study" aria-haspopup="true">
      学习
      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
    </button>
    <div class="topnav-dropdown__menu" role="menu">
      <a href="/pages/kp-entry.html" role="menuitem">知识点</a>
      <a href="/pages/wordbook.html" role="menuitem">单词本</a>
    </div>
    </div>

    <div class="topnav-dropdown">
    <button class="topnav-dropdown__trigger" data-group="practice" aria-haspopup="true">
      练习
      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
    </button>
    <div class="topnav-dropdown__menu" role="menu">
      <a href="/pages/review.html" role="menuitem">每日复习</a>
      <a href="/pages/daily-quiz.html" role="menuitem">每日小测</a>
    </div>
    </div>

    <a href="/" class="topnav__link">首页</a>
    <a href="/pages/articles.html" class="topnav__link topnav__link--plan">文章阅读<span class="soon-badge">即将开放</span></a>
    <a href="/pages/learning-center.html" class="topnav__link topnav__link--plan">学情中心</a>

    <span class="topnav__incentive">
      <button type="button" class="topnav__incentive-item topnav__incentive-item--coin" id="nc-coin" title="圣殿宝库" data-treasury-open>🪙 <span>–</span></button>
      <button type="button" class="topnav__incentive-item topnav__incentive-item--title" id="nc-title" title="圣殿宝库" data-treasury-open>—</button>
      <button type="button" class="topnav__avatar" id="nc-avatar" title="圣殿宝库" data-treasury-open>—</button>
    </span>

    <span class="topnav__user" id="nc-user"></span>

    <div class="ui-mode" id="nc-ui-mode">
      <button class="ui-mode__trigger" type="button" aria-haspopup="true" aria-expanded="false">
        <span class="ui-mode__label" id="nc-ui-mode-label">功能模式</span>
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"/></svg>
      </button>
      <div class="ui-mode__menu" role="menu">
        <button class="ui-mode__option" type="button" role="menuitemradio" aria-checked="false" data-ui-mode-set="function">
          <span class="ui-mode__option-title">功能模式</span>
          <span class="ui-mode__option-desc">随手学：AI 讲解、定制练习、复习、小测、词表</span>
        </button>
        <button class="ui-mode__option" type="button" role="menuitemradio" aria-checked="false" data-ui-mode-set="plan">
          <span class="ui-mode__option-title">计划模式</span>
          <span class="ui-mode__option-desc">长期课规：本周课规、今日所得、登阶、留印</span>
        </button>
        <p class="ui-mode__note" id="nc-ui-mode-note">登录后可在设备间同步界面模式</p>
        <div class="ui-mode__links">
          <a href="/pages/articles.html">文章阅读</a>
          <a href="/pages/learning-center.html">学情中心</a>
        </div>
      </div>
    </div>

    <button class="theme-toggle" id="theme-toggle" type="button" title="切换主题">☀</button>
  </nav>
  <div class="temple-roof"></div>
</header>
`;

  // ---------- 圣殿宝库抽屉（T103：由独立页改为抽屉浮层，全站可用） ----------
  // 内容裁剪：只保留「金桂余额 + 三统计 + 圣殿账册」；
  // 功名登阶 / 朱砂印 / 本周课规 由首页仪表盘承载，不在此重复。
  const TREASURY_HTML = `
<div class="drawer-overlay" id="treasury-drawer">
  <aside class="drawer-panel" role="dialog" aria-modal="true" aria-labelledby="treasury-drawer-title">
    <div class="drawer-header">
      <h3 class="drawer-title" id="treasury-drawer-title">圣殿宝库</h3>
      <button class="drawer-close" type="button" data-treasury-close aria-label="关闭">×</button>
    </div>
    <div class="drawer-body">
      <div class="treas-drawer-head">
        <span class="treas-badge">点金圣殿</span>
        <div class="treas-balance__cap">金桂余额</div>
        <div class="treas-balance__num" id="td-balance">–</div>
      </div>
      <div class="treas-stats">
        <div class="stat-card"><span class="treas-stat__value" id="td-earned">–</span><span class="treas-stat__label">累计收入</span></div>
        <div class="stat-card"><span class="treas-stat__value" id="td-spent">–</span><span class="treas-stat__label">累计消耗</span></div>
        <div class="stat-card"><span class="treas-stat__value" id="td-mastery">–</span><span class="treas-stat__label">掌握词条</span></div>
      </div>
      <p class="state-line" id="td-error"></p>
      <div class="terrace__head"><h3>圣殿账册</h3><span class="terrace__hint">最近流水</span></div>
      <div class="scroll">
        <div class="task-list" id="td-txn-list">
          <div class="skeleton skeleton--text"></div>
          <div class="skeleton skeleton--text"></div>
          <div class="skeleton skeleton--text"></div>
        </div>
      </div>
    </div>
  </aside>
</div>
`;

  // active 判定表：pathname → { 目标选择器, 自身 active 标记 }
  // 下拉子页：给 .topnav-dropdown__trigger 加 active（菜单通过 hover/focus-within 自动展开）
  const ACTIVE_MAP = [
    { path: '/', trigger: '.topnav__link[href="/"]',   activeSel: '.topnav__link[href="/"]' },
    { path: '/pages/kp-entry.html',       trigger: '.topnav-dropdown__trigger[data-group="study"]' },
    { path: '/pages/wordbook.html',       trigger: '.topnav-dropdown__trigger[data-group="study"]' },
    { path: '/pages/review.html',         trigger: '.topnav-dropdown__trigger[data-group="practice"]' },
    { path: '/pages/daily-quiz.html',     trigger: '.topnav-dropdown__trigger[data-group="practice"]' },
    { path: '/pages/articles.html',       trigger: '.topnav__link[href="/pages/articles.html"]',       activeSel: '.topnav__link[href="/pages/articles.html"]' },
    { path: '/pages/learning-center.html',trigger: '.topnav__link[href="/pages/learning-center.html"]', activeSel: '.topnav__link[href="/pages/learning-center.html"]' },
  ];

  // ---------- 注入 ----------

  function inject()
  {
    const slot = document.getElementById('topbar-slot');
    const host = slot || document.querySelector('.shell') || document.body;
    if (!host) return false;

    host.insertAdjacentHTML('afterbegin', NAV_HTML);
    return true;
  }

  // ---------- active 自动判定 ----------

  function markActive()
  {
    const path = location.pathname;
    for (const rule of ACTIVE_MAP)
    {
      if (rule.path === path)
      {
        const node = document.querySelector(rule.trigger);
        if (node) node.classList.add('active');
        break;
      }
    }
  }

  // ---------- 界面模式：渲染 / 切换 / 账号同步 ----------

  function syncModeLabel()
  {
    const mode = document.documentElement.getAttribute('data-ui-mode') || 'function';
    const label = document.getElementById('nc-ui-mode-label');
    if (label) label.textContent = UI_MODE_TEXT[mode] || UI_MODE_TEXT.function;
    const trigger = document.querySelector('.ui-mode__trigger');
    if (trigger)
      trigger.setAttribute('aria-label', '当前界面：' + (UI_MODE_TEXT[mode] || UI_MODE_TEXT.function) + '，点击切换');
    document.querySelectorAll('[data-ui-mode-set]').forEach(btn =>
    {
      const on = btn.getAttribute('data-ui-mode-set') === mode;
      btn.classList.toggle('active', on);
      btn.setAttribute('aria-checked', on ? 'true' : 'false');
    });
  }

  // persistLocal：游客与登录用户均把当前值落到本机，供未登录时兜底
  function applyUiMode(mode, persistLocal)
  {
    const next = mode === 'plan' ? 'plan' : 'function';
    document.documentElement.setAttribute('data-ui-mode', next);
    if (persistLocal) writeLocalUiMode(next);
    syncModeLabel();
    return next;
  }

  function closeModeMenu()
  {
    const box = document.getElementById('nc-ui-mode');
    if (box) box.classList.remove('open');
    const trigger = document.querySelector('.ui-mode__trigger');
    if (trigger) trigger.setAttribute('aria-expanded', 'false');
  }

  function toggleModeMenu()
  {
    const box = document.getElementById('nc-ui-mode');
    if (!box) return;
    const open = !box.classList.contains('open');
    box.classList.toggle('open', open);
    const trigger = box.querySelector('.ui-mode__trigger');
    if (trigger) trigger.setAttribute('aria-expanded', open ? 'true' : 'false');
  }

  // 切换界面模式：先乐观更新；登录用户写账号，失败回滚
  async function switchUiMode(mode)
  {
    const prev = document.documentElement.getAttribute('data-ui-mode') || 'function';
    const next = applyUiMode(mode, true);
    closeModeMenu();
    if (prev === next || !uiModeLoggedIn) return;
    try
    {
      const r = await fetch('/api/auth/profile',
      {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ui_mode: next })
      });
      if (!r.ok) throw new Error('save failed');
      const j = await r.json();
      if (!j.ok) throw new Error(j.error || 'save failed');
    }
    catch (e)
    {
      applyUiMode(prev, true);
      alert('界面模式保存失败，已恢复到「' + (UI_MODE_TEXT[prev] || prev) + '」。');
    }
  }
  window.engSetUiMode = switchUiMode;
  window.engGetUiMode = function () { return document.documentElement.getAttribute('data-ui-mode') || 'function'; };

  // ---------- 激励区 ----------

  async function renderIncentive()
  {
    const coin = document.getElementById('nc-coin');
    const title = document.getElementById('nc-title');
    const avatar = document.getElementById('nc-avatar');
    if (!coin || !title || !avatar) return;

    try
    {
      const r = await fetch('/api/treasury', { cache: 'no-store' });
      if (!r.ok) throw new Error('treasury fail');
      const j = await r.json();

      coin.innerHTML = '🪙 <span>' + (j.balance != null ? j.balance : '0') + '</span>';
      const titleText = j.current_title && j.current_title.length > 0 ? j.current_title : '访客';
      title.textContent = titleText;

      // 头像首字：先尝试 auth/me 的名字；treasury 不返回用户名，需要另一路
      try
      {
        const me = await (await fetch('/api/auth/me', { cache: 'no-store' })).json();
        const name = me && !me.guest ? (me.display_name || me.username || '') : '';
        const initial = name ? name.trim().charAt(0).toUpperCase() : '访';
        avatar.textContent = initial;
      }
      catch (e2)
      {
        avatar.textContent = '访';
      }
    }
    catch (e)
    {
      coin.innerHTML = '🪙 <span>–</span>';
      title.textContent = '—';
      avatar.textContent = '—';
    }
  }

  // ---------- 用户区（合并旧 auth-nav.js 逻辑） ----------

  function escapeHtml(s)
  {
    return String(s).replace(/[&<>"']/g, c =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  async function renderUser()
  {
    const slot = document.getElementById('nc-user');
    if (!slot) return;

    let me = { guest: true };
    try
    {
      me = await (await fetch('/api/auth/me', { cache: 'no-store' })).json();
    }
    catch (e) {}

    if (!me || me.guest)
    {
      uiModeLoggedIn = false;
      applyUiMode(readLocalUiMode(), false);   // 游客沿用本机偏好
      const note = document.getElementById('nc-ui-mode-note');
      if (note) note.classList.add('show');
      slot.innerHTML =
        '<a href="/pages/privacy.html" class="topnav__legal">隐私</a>' +
        '<a href="/pages/terms.html" class="topnav__legal">协议</a>' +
        '<a href="/pages/login.html" class="topnav__login">登录</a>';
      return;
    }

    // 登录后以账号设置为准，并同步到本机（退出登录后游客态沿用）
    uiModeLoggedIn = true;
    applyUiMode(me.ui_mode === 'plan' ? 'plan' : 'function', true);
    const modeNote = document.getElementById('nc-ui-mode-note');
    if (modeNote) modeNote.classList.remove('show');

    const name = me.display_name || me.username || '';
    const safeName = escapeHtml(name);

    slot.innerHTML =
      '<span class="topnav__name">' + safeName + '</span>' +
      '<span class="topnav__divider"></span>' +
      '<a href="/pages/privacy.html" class="topnav__legal">隐私</a>' +
      '<a href="/pages/terms.html" class="topnav__legal">协议</a>' +
      '<a href="javascript:void(0)" class="topnav__logout">退出</a>' +
      '<a href="javascript:void(0)" class="topnav__deregister" title="删除账号及全部数据">注销</a>';

    slot.querySelector('.topnav__logout').addEventListener('click', async () =>
    {
      try { await fetch('/api/auth/logout', { method: 'POST' }); } catch (e) {}
      location.assign('/');
    });

    slot.querySelector('.topnav__deregister').addEventListener('click', async () =>
    {
      const targetName = me.display_name || me.username;
      if (!confirm('确认注销账号「' + targetName + '」？\n该操作将删除你所有的知识点、单词本、学情数据，且无法恢复。')) return;
      try { await fetch('/api/auth/deregister', { method: 'POST' }); } catch (e) {}
      location.assign('/');
    });
  }

  // ---------- 圣殿宝库抽屉：注入 / 开关 / 数据渲染 ----------

  const BALANCE_CAP = 10000;   // 余额 ≥ 此值显示 9,999+（边界态封顶）
  const TREAS_REASONS = {
    quiz_done: '应试（小测完成）',
    quiz_perfect: '妙笔（小测满分）',
    review_done: '晨诵（每日复习）',
    title_up: '登科（称谓升级）',
    goal_done: '课规有成',
    streak_bonus: '守规加持',
    quiz_cost: '研墨（生成试题）',
    solidify_cost: '镌刻（固化）',
    lookup_spend: '问道（查词）',
    wordbook_spend: '编经（单词本）'
  };

  function injectTreasury()
  {
    if (document.getElementById('treasury-drawer')) return;
    document.body.insertAdjacentHTML('beforeend', TREASURY_HTML);
  }

  function openTreasury()
  {
    injectTreasury();
    const d = document.getElementById('treasury-drawer');
    if (!d) return;
    d.classList.add('open');
    loadTreasury();
  }

  function closeTreasury()
  {
    const d = document.getElementById('treasury-drawer');
    if (d) d.classList.remove('open');
  }

  function renderTreasury(t)
  {
    const err = document.getElementById('td-error');
    if (!t)
    {
      err.className = 'state-line state-line--danger';
      err.innerHTML = escapeHtml('宝库数据读取失败 ') +
        '<button class="btn btn-secondary u-btn-sm" type="button" data-treasury-retry>重试</button>';
      document.getElementById('td-balance').textContent = '–';
      document.getElementById('td-earned').textContent = '–';
      document.getElementById('td-spent').textContent = '–';
      document.getElementById('td-mastery').textContent = '–';
      document.getElementById('td-txn-list').innerHTML = '';
      return;
    }
    err.className = 'state-line';
    err.textContent = '';
    document.getElementById('td-balance').textContent = t.balance >= BALANCE_CAP ? '9,999+' : t.balance;
    document.getElementById('td-earned').textContent = t.total_earned;
    document.getElementById('td-spent').textContent = t.total_spent;
    document.getElementById('td-mastery').textContent = t.mastery_count;
    renderTreasTxns(t.txns);
  }

  function renderTreasTxns(txns)
  {
    const host = document.getElementById('td-txn-list');
    if (!Array.isArray(txns) || !txns.length)
    {
      host.innerHTML = '<p class="state-line">暂无流水记录 —— 完成复习或小测即可得金桂。</p>';
      return;
    }
    host.innerHTML = txns.map(t =>
    {
      const reason = TREAS_REASONS[t.reason_code] || t.reason_code;
      const positive = t.delta > 0;
      return '<div class="task-item task-item--static">' +
        '<span class="task-check' + (positive ? ' done' : '') + '"></span>' +
        '<span class="task-text">' +
          '<span class="treas-txn__name">' + escapeHtml(reason) + '</span>' +
          '<span class="treas-txn__time">' + escapeHtml(t.created_at || '') + '</span>' +
        '</span>' +
        '<span class="task-flag ' + (positive ? 'u-gold' : 'u-danger') + '">' +
          (positive ? '+' : '') + t.delta +
        '</span>' +
      '</div>';
    }).join('');
  }

  async function loadTreasury()
  {
    try
    {
      const r = await fetch('/api/treasury', { cache: 'no-store' });
      if (!r.ok) throw new Error('treasury fail');
      renderTreasury(await r.json());
    }
    catch (e)
    {
      renderTreasury(null);
    }
  }

  // ---------- AI 导航注入（v1.5.1-T005 T4） ----------
  // 仅在含顶栏的页面加载；幂等，重复调用不会重复插入
  function injectAiNav()
  {
    if (!document.getElementById('topbar-slot')) return;
    if (window.__engAiNavLoaded || document.querySelector('link[data-ai-nav]')) return;
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = '/static/ai-nav.css?v=20261118e';
    link.setAttribute('data-ai-nav', '');
    document.head.appendChild(link);
    const script = document.createElement('script');
    script.src = '/static/ai-nav.js?v=20261118e';
    script.async = true;
    document.body.appendChild(script);
  }

  // ---------- 启动 ----------

  document.addEventListener('DOMContentLoaded', () =>
  {
    injectAiNav();
    if (!inject()) return; // login.html 等无 .shell 的页面：跳过导航注入

    markActive();
    syncModeLabel();
    renderIncentive();
    renderUser();
    // 预热注入抽屉壳层（.drawer-overlay 未加 .open 时不可见、不可点）
    injectTreasury();

    // theme.js 在 <head> 同步引入，initThemeButton 此时已就绪
    if (typeof window.initThemeButton === 'function') window.initThemeButton();
  });

  // 事件委托（导航与抽屉均为动态注入，无行内事件）
  document.addEventListener('click', e =>
  {
    if (e.target.closest('.ui-mode__trigger')) { toggleModeMenu(); return; }
    if (e.target.closest('[data-ui-mode-set]'))
    {
      switchUiMode(e.target.closest('[data-ui-mode-set]').getAttribute('data-ui-mode-set'));
      return;
    }
    if (!e.target.closest('#nc-ui-mode')) closeModeMenu();

    if (e.target.closest('[data-treasury-open]')) { openTreasury(); return; }
    if (e.target.closest('[data-treasury-close]')) { closeTreasury(); return; }
    if (e.target.closest('[data-treasury-retry]')) { loadTreasury(); return; }
    if (e.target.id === 'treasury-drawer') closeTreasury();   // 点遮罩关闭
  });
  document.addEventListener('keydown', e =>
  {
    if (e.key === 'Escape') { closeTreasury(); closeModeMenu(); }
  });
})();
