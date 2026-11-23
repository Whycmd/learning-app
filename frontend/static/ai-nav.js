// AI 导航前端（v1.5.1-T005 T4 + 体验优化）
// 职责：吸边入口（可拖拽、记忆位置）、抽屉对话、快捷问题、动作胶囊、课规提案卡、游客登录拦截
// 约定：AI 只产出文本；是否跳转与提案数值均由后端判定，本文件只负责展示与提交
(function ()
{
  'use strict';

  if (window.__engAiNavLoaded) return;
  window.__engAiNavLoaded = true;

  const PAGE = location.pathname;
  const MAX_CHARS = 500;
  const COUNT_HINT = 450;              // 超过该字数显示计数
  const JUMP_DELAY = 1400;             // 明确指令跳转前的可取消时间
  const STORE_Y = 'eng_ai_nav_y';        // 桌面端吸边高度（百分比）
  const STORE_X = 'eng_ai_nav_x';        // 移动端吸边水平位置（px）
  const STORE_HINT = 'eng_ai_nav_hint';  // 一次性提示是否已看过
  const STORE_PENDING = 'eng_ai_nav_pending_plan'; // 游客提案待登录确认
  const STORE_GUEST_MSGS = 'eng_ai_nav_guest_msgs'; // 游客当前标签页会话

  const state = {
    open: false,
    loaded: false,
    loading: false,
    guest: true,
    meta: null,
    messages: [],
    sending: false,
    clearArmed: false,
    clearing: false,
    loadError: '',
    lastFocus: null
  };

  const WIDGET_HTML = `
<button class="ai-nav-fab" id="ai-nav-fab" type="button" aria-label="打开 AI 向导" aria-expanded="false">
  <span class="ai-nav-fab__orb" aria-hidden="true">✦</span>
  <span class="ai-nav-fab__label">AI 向导</span>
</button>
<div class="ai-nav-hint" id="ai-nav-hint" hidden>今天学什么？问我</div>
<div class="ai-nav-overlay" id="ai-nav-drawer">
  <aside class="ai-nav-panel" role="dialog" aria-modal="true" aria-labelledby="ai-nav-title">
    <header class="ai-nav-head">
      <span class="ai-nav-head__orb" aria-hidden="true">✦</span>
      <div class="ai-nav-head__text">
        <h3 class="ai-nav-title" id="ai-nav-title">圣殿向导</h3>
      </div>
      <button class="ai-nav-clear" type="button" id="ai-nav-clear" hidden>清空对话</button>
      <button class="ai-nav-close" type="button" id="ai-nav-close" aria-label="关闭">×</button>
    </header>
    <div class="ai-nav-body">
      <div class="ai-nav-list" id="ai-nav-list" aria-live="polite"></div>
    </div>
    <button class="ai-nav-tobottom" id="ai-nav-tobottom" type="button" hidden>回到底部 ↓</button>
    <div class="ai-nav-jump" id="ai-nav-jump" hidden>
      <span id="ai-nav-jump-text"></span>
      <button type="button" id="ai-nav-jump-cancel">取消</button>
    </div>
    <div class="ai-nav-quick">
      <div class="ai-nav-chips" id="ai-nav-chips"></div>
    </div>
    <form class="ai-nav-form" id="ai-nav-form">
      <div class="ai-nav-form__box">
        <textarea class="ai-nav-input" id="ai-nav-input" rows="1" maxlength="${MAX_CHARS}"
          placeholder="说说你想学什么…"></textarea>
        <span class="ai-nav-count" id="ai-nav-count"></span>
      </div>
      <button class="ai-nav-send" id="ai-nav-send" type="submit" aria-label="发送">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
          stroke-linecap="round" stroke-linejoin="round"><path d="M22 2 11 13"/><path d="M22 2 15 22l-4-9-9-4z"/></svg>
      </button>
    </form>
    <div class="ai-nav-toast" id="ai-nav-toast" hidden></div>
  </aside>
</div>
`;

  let jumpTimer = null;
  let toastTimer = null;

  function $(id) { return document.getElementById(id); }
  function esc(s)
  {
    return String(s == null ? '' : s).replace(/[&<>"']/g, c =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }
  function charLen(s) { return Array.from(String(s || '')).length; }
  function readNum(key)
  {
    try
    {
      const v = Number(localStorage.getItem(key));
      return Number.isFinite(v) ? v : null;
    }
    catch (e) { return null; }
  }
  function writeNum(key, v)
  {
    try { localStorage.setItem(key, String(v)); } catch (e) {}
  }
  function isMobile() { return window.matchMedia('(max-width: 640px)').matches; }
  function pendingRedirect()
  {
    return '/pages/login.html?redirect=' + encodeURIComponent(location.pathname + location.search);
  }

  // ---------- 吸边入口：位置恢复 + 拖拽 ----------
  function applyPosition()
  {
    const fab = $('ai-nav-fab');
    const hint = $('ai-nav-hint');
    if (!fab) return;
    if (isMobile())
    {
      const x = readNum(STORE_X);
      if (x != null)
      {
        const clamped = Math.min(Math.max(60, x), Math.max(60, window.innerWidth - 60));
        fab.style.left = clamped + 'px';
      }
      fab.style.top = '';
      if (hint) { hint.style.top = ''; hint.style.left = ''; }
    }
    else
    {
      const y = readNum(STORE_Y);
      const pct = y == null ? 45 : Math.min(88, Math.max(12, y));
      fab.style.top = pct + '%';
      fab.style.left = '';
      if (hint) { hint.style.top = pct + '%'; }
    }
  }

  function installDrag()
  {
    const fab = $('ai-nav-fab');
    if (!fab) return;
    let dragging = false, moved = false, startX = 0, startY = 0, base = 0;

    fab.addEventListener('pointerdown', e =>
    {
      dragging = true;
      moved = false;
      startX = e.clientX;
      startY = e.clientY;
      base = isMobile()
        ? (readNum(STORE_X) != null ? readNum(STORE_X) : fab.getBoundingClientRect().left + fab.offsetWidth / 2)
        : (readNum(STORE_Y) != null ? readNum(STORE_Y) : 45);
      fab.classList.add('is-dragging');
      try { fab.setPointerCapture(e.pointerId); } catch (e2) {}
    });

    fab.addEventListener('pointermove', e =>
    {
      if (!dragging) return;
      const dx = e.clientX - startX;
      const dy = e.clientY - startY;
      if (Math.abs(dx) + Math.abs(dy) > 4) moved = true;
      if (!moved) return;
      if (isMobile())
      {
        const x = Math.min(Math.max(60, base + dx), Math.max(60, window.innerWidth - 60));
        fab.style.left = x + 'px';
        writeNum(STORE_X, Math.round(x));
      }
      else
      {
        const pct = Math.min(88, Math.max(12, base + dy / window.innerHeight * 100));
        fab.style.top = pct + '%';
        const hint = $('ai-nav-hint');
        if (hint) hint.style.top = pct + '%';
        writeNum(STORE_Y, Math.round(pct));
      }
    });

    function endDrag()
    {
      if (!dragging) return;
      dragging = false;
      fab.classList.remove('is-dragging');
      if (moved) fab.dataset.suppressClick = '1';
    }
    fab.addEventListener('pointerup', endDrag);
    fab.addEventListener('pointercancel', endDrag);

    fab.addEventListener('click', () =>
    {
      if (fab.dataset.suppressClick === '1')
      {
        delete fab.dataset.suppressClick;
        return;
      }
      openDrawer();
    });
  }

  // ---------- 一次性提示与首次脉冲 ----------
  function hintSeen()
  {
    try { return localStorage.getItem(STORE_HINT) === '1'; } catch (e) { return false; }
  }

  function maybeShowHint()
  {
    if (hintSeen()) return;
    const fab = $('ai-nav-fab');
    if (fab) fab.classList.add('is-new');
    setTimeout(() => { if (fab) fab.classList.remove('is-new'); }, 4200);
    setTimeout(() =>
    {
      const hint = $('ai-nav-hint');
      if (!hint || state.open) return;
      hint.hidden = false;
    }, 1600);
  }

  function dismissHint()
  {
    const hint = $('ai-nav-hint');
    if (hint) hint.hidden = true;
    try { localStorage.setItem(STORE_HINT, '1'); } catch (e) {}
  }

  // ---------- 抽屉 ----------
  let bodyOverflow = null;

  function openDrawer()
  {
    const d = $('ai-nav-drawer');
    if (!d) return;
    state.lastFocus = document.activeElement;
    state.clearArmed = false;
    dismissHint();
    d.classList.add('open');
    state.open = true;
    bodyOverflow = document.body.style.overflow;
    document.body.style.overflow = 'hidden';   // 抽屉打开时锁住背景滚动
    $('ai-nav-fab').setAttribute('aria-expanded', 'true');
    if (!state.loaded) loadInitial();
    else if (state.guest && !state.messages.length) renderAll();
    setTimeout(() =>
    {
      const ta = $('ai-nav-input');
      if (ta && !isMobile()) ta.focus();
      const body = document.querySelector('.ai-nav-body');
      if (body) body.scrollTop = body.scrollHeight;
      updateToBottom(body);
      updateCount();
    }, 220);
  }

  function closeDrawer()
  {
    const d = $('ai-nav-drawer');
    if (!d) return;
    d.classList.remove('open');
    state.open = false;
    cancelJump();
    document.body.style.overflow = bodyOverflow || '';
    $('ai-nav-fab').setAttribute('aria-expanded', 'false');
    const back = state.lastFocus && state.lastFocus.focus ? state.lastFocus : $('ai-nav-fab');
    if (back) setTimeout(() => back.focus(), 50);
  }

  // 焦点陷阱：Tab 在抽屉内循环
  function trapFocus(e)
  {
    if (!state.open || e.key !== 'Tab') return;
    const panel = document.querySelector('.ai-nav-panel');
    if (!panel) return;
    const nodes = Array.from(panel.querySelectorAll('button, a[href], textarea, input'))
      .filter(el => !el.disabled && el.offsetParent !== null);
    if (!nodes.length) return;
    const first = nodes[0], last = nodes[nodes.length - 1];
    if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
    else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
  }

  // ---------- 数据 ----------
  async function loadInitial()
  {
    state.loading = true;
    state.loadError = '';
    renderAll();
    try
    {
      const [metaRes, meRes] = await Promise.all([
        fetch('/api/ai-nav/messages?page=' + encodeURIComponent(PAGE), { cache: 'no-store' }),
        fetch('/api/auth/me', { cache: 'no-store' }).catch(() => null)
      ]);
      const meta = await metaRes.json().catch(() => null);
      if (!metaRes.ok || !meta) throw new Error((meta && meta.error) || '导航加载失败');
      const me = meRes ? await meRes.json().catch(() => null) : null;
      state.guest = !me || !!me.guest;
      state.meta = meta;
      state.messages = Array.isArray(meta.messages) ? meta.messages : [];
      if (state.guest) restoreGuestMessages();
      state.loaded = true;
      state.loading = false;
      renderAll();
      consumePendingPlan();
    }
    catch (e)
    {
      state.loading = false;
      state.loadError = e.message || '导航加载失败，请稍后再试';
      renderAll();
    }
  }

  // ---------- 游客会话保留（同一标签页） ----------
  function persistGuestMessages()
  {
    if (!state.guest) return;
    try
    {
      const slim = state.messages.slice(-20).map(m => ({
        id: m.id || 0,
        role: m.role,
        content: m.content,
        action: m.action || null,
        proposal: m.proposal || null,
        proposal_status: m.proposal_status || 'none'
      }));
      sessionStorage.setItem(STORE_GUEST_MSGS, JSON.stringify(slim));
    }
    catch (e) {}
  }

  function restoreGuestMessages()
  {
    if (state.messages.length) return;
    try
    {
      const raw = sessionStorage.getItem(STORE_GUEST_MSGS);
      if (!raw) return;
      const list = JSON.parse(raw);
      if (Array.isArray(list)) state.messages = list.filter(m => m && m.role !== 'system');
    }
    catch (e) {}
  }

  // 清空需二次确认，避免误触丢对话
  async function clearConversation()
  {
    if (state.clearing) return;
    if (!state.clearArmed)
    {
      state.clearArmed = true;
      renderAll();
      setTimeout(() =>
      {
        if (state.clearArmed)
        {
          state.clearArmed = false;
          renderAll();
        }
      }, 3500);
      return;
    }
    state.clearArmed = false;
    state.clearing = true;
    renderAll();
    try
    {
      // 登录用户的对话存在服务端，需同步删除；游客只存当前标签页
      if (!state.guest)
      {
        const r = await fetch('/api/ai-nav/messages', { method: 'DELETE' });
        const j = await r.json().catch(() => ({}));
        if (!r.ok || !j.ok) throw new Error(j.error || '清空失败，请稍后再试');
      }
      state.messages = [];
      try { sessionStorage.removeItem(STORE_GUEST_MSGS); } catch (e) {}
      state.clearing = false;
      renderAll();
      toast('对话已清空');
    }
    catch (e)
    {
      state.clearing = false;
      renderAll();
      toast(e.message || '清空失败，请稍后再试');
    }
  }

  // 游客提案：登录后回到本页自动恢复为一条新提案
  function consumePendingPlan()
  {
    let raw = null;
    try { raw = sessionStorage.getItem(STORE_PENDING); } catch (e) {}
    if (!raw || state.guest) return;
    try { sessionStorage.removeItem(STORE_PENDING); } catch (e) {}
    send(raw, true);
  }

  function rememberPendingPlan(text)
  {
    try { sessionStorage.setItem(STORE_PENDING, text); } catch (e) {}
  }

  async function send(text, silentUser)
  {
    const msg = String(text || '').trim();
    if (!msg || state.sending) return;
    if (charLen(msg) > MAX_CHARS)
    {
      pushError('消息太长了，请控制在 ' + MAX_CHARS + ' 字以内');
      return;
    }
    if (!silentUser)
    {
      state.messages.push({ role: 'user', content: msg, action: null, proposal: null, proposal_status: 'none', id: 0 });
    }
    state.sending = true;
    persistGuestMessages();
    renderAll();

    const history = state.messages
      .filter(m => m.role === 'user' || m.role === 'assistant')
      .slice(-6)
      .map(m => ({ role: m.role, content: m.content }));

    try
    {
      const r = await fetch('/api/ai-nav/messages', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text: msg, page: PAGE, history })
      });
      const j = await r.json().catch(() => ({}));
      if (r.status === 429)
      {
        restoreDraft(msg);
        pushError(j.error || '问得有点快，请稍后再试');
        return;
      }
      if (!r.ok || !j.ok || !j.reply) throw new Error(j.error || '发送失败，请稍后再试');
      state.messages.push(j.reply);
      if (j.meta) state.meta = Object.assign({}, state.meta || {}, j.meta);

      const action = j.reply.action;
      const direct = action && action.route && String(j.reply.content || '').indexOf('已为你跳转：') === 0;
      if (direct)
      {
        persistGuestMessages();
        renderAll();
        scheduleJump(action);
        return;
      }
    }
    catch (e)
    {
      restoreDraft(msg);
      pushError(e.message || '发送失败，请稍后再试');
    }
    finally
    {
      state.sending = false;
      persistGuestMessages();
      renderAll();
    }
  }

  // 发送失败时把原文放回输入框，避免用户重打
  function restoreDraft(text)
  {
    const ta = $('ai-nav-input');
    if (!ta || ta.value.trim()) return;
    ta.value = text;
    updateCount();
    if (!isMobile()) ta.focus();
  }

  function pushError(text)
  {
    state.messages.push({ role: 'system', content: text, action: null, proposal: null, proposal_status: 'none', id: 0 });
    state.sending = false;
    renderAll();
  }

  // ---------- 明确指令：跳转前留出取消时间 ----------
  function scheduleJump(action)
  {
    const bar = $('ai-nav-jump');
    const text = $('ai-nav-jump-text');
    if (!bar || !text) { location.assign(action.route); return; }
    text.textContent = '即将打开：' + (action.label || action.route);
    bar.hidden = false;
    clearTimeout(jumpTimer);
    jumpTimer = setTimeout(() => { location.assign(action.route); }, JUMP_DELAY);
  }

  function cancelJump()
  {
    clearTimeout(jumpTimer);
    jumpTimer = null;
    const bar = $('ai-nav-jump');
    if (bar) bar.hidden = true;
  }

  // ---------- 轻提示 ----------
  function toast(text)
  {
    const el = $('ai-nav-toast');
    if (!el) return;
    el.textContent = text;
    el.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { el.hidden = true; }, 2400);
  }

  // ---------- 渲染 ----------
  function nearBottomOf(body)
  {
    return !body || (body.scrollHeight - body.scrollTop - body.clientHeight < 56);
  }

  function updateToBottom(body)
  {
    const btn = $('ai-nav-tobottom');
    if (!btn) return;
    btn.hidden = !body || nearBottomOf(body) || state.messages.length < 3;
  }

  function renderAll()
  {
    renderChips();
    const list = $('ai-nav-list');
    if (!list) return;
    const body = list.closest('.ai-nav-body');
    const stick = nearBottomOf(body);

    const parts = [];
    if (state.meta && state.meta.welcome)
    {
      parts.push('<div class="ai-nav-msg ai-nav-msg--assistant">' +
        '<span class="ai-nav-avatar" aria-hidden="true">✦</span>' +
        '<div class="ai-nav-bubble"><p class="ai-nav-text">' + esc(state.meta.welcome) + '</p></div></div>');
    }
    state.messages.forEach((m, i) => { parts.push(messageHtml(m, i)); });
    if (state.loading) parts.push('<p class="ai-nav-loading">正在准备导航…</p>');
    if (state.sending) parts.push('<p class="ai-nav-loading">正在想…</p>');
    if (state.loadError)
    {
      parts.push('<div class="ai-nav-sys">' + esc(state.loadError) +
        ' <button type="button" class="ai-nav-linkbtn" data-na-retry>重试</button></div>');
    }
    list.innerHTML = parts.join('');

    const clearBtn = $('ai-nav-clear');
    if (clearBtn)
    {
      clearBtn.hidden = state.messages.length === 0;
      clearBtn.textContent = state.clearArmed ? '确认清空？' : (state.clearing ? '清空中…' : '清空对话');
      clearBtn.classList.toggle('is-armed', state.clearArmed);
      clearBtn.disabled = !!state.clearing;
    }
    if (body && stick) body.scrollTop = body.scrollHeight;
    updateToBottom(body);
    updateCount();
  }

  function renderChips()
  {
    const host = $('ai-nav-chips');
    if (!host) return;
    const meta = state.meta || {};
    const qs = Array.isArray(meta.quick_questions) ? meta.quick_questions.slice() : [];
    if (meta.context_question) qs.push(meta.context_question);

    host.innerHTML = qs.map(q =>
      '<button type="button" class="ai-nav-chip' +
      (q === meta.context_question ? ' ai-nav-chip--context' : '') +
      '" data-na-ask="' + esc(q) + '">' + esc(q) + '</button>'
    ).join('');
  }

  function messageHtml(m, idx)
  {
    if (m.role === 'system')
      return '<p class="ai-nav-sys">' + esc(m.content) + '</p>';

    const isUser = m.role === 'user';
    let inner = '<p class="ai-nav-text">' + esc(m.content).replace(/\n/g, '<br>') + '</p>';

    if (m.action && m.action.route && (!m.proposal || m.proposal_status !== 'proposed'))
    {
      inner += '<a class="ai-nav-chip" href="' + esc(m.action.route) + '">' +
        esc(m.action.label || '前往') + ' →</a>';
    }
    if (m.proposal)
      inner += proposalHtml(m, idx);
    else if (!isUser && m.content)
      inner += '<div class="ai-nav-msg__acts">' +
        '<button type="button" class="ai-nav-linkbtn" data-na-copy="' + idx + '">复制</button>' +
        '<button type="button" class="ai-nav-linkbtn" data-na-regen="' + idx + '">重新回答</button>' +
        '</div>';

    return '<div class="ai-nav-msg' + (isUser ? ' ai-nav-msg--user' : '') + '">' +
      (isUser ? '' : '<span class="ai-nav-avatar" aria-hidden="true">✦</span>') +
      '<div class="ai-nav-bubble">' + inner + '</div></div>';
  }

  function proposalHtml(m, idx)
  {
    const p = m.proposal || {};
    const status = m.proposal_status || 'proposed';
    const key = String(m.id || 'g' + idx);
    const desc = p.target_desc || p.name || '为你排一份课规';

    if (status === 'confirmed')
      return '<div class="ai-nav-proposal"><div class="ai-nav-proposal__title">课规提案</div>' +
        '<p class="ai-nav-proposal__desc">' + esc(desc) + '</p>' +
        '<p class="ai-nav-proposal__note">已生效' +
        (m.confirm_deadline ? '，截止 ' + esc(m.confirm_deadline) : '') +
        '。<a class="ai-nav-chip" href="/pages/plan.html">去课规查看 →</a></p></div>';
    if (status === 'discarded')
      return '<div class="ai-nav-proposal"><div class="ai-nav-proposal__title">课规提案</div>' +
        '<p class="ai-nav-proposal__note">已放弃这份提案。想重新安排可以直接告诉我。</p></div>';
    if (status === 'expired')
      return '<div class="ai-nav-proposal"><div class="ai-nav-proposal__title">课规提案</div>' +
        '<p class="ai-nav-proposal__note">提案已过期，请重新让我排一份。</p></div>';

    const days = Number(p.period_days || 7);
    const mins = Number(p.daily_minutes || 20);
    const words = Number(p.daily_new_words || 10);
    const stats = '<div class="ai-nav-proposal__stats">' +
      '<div><b>' + mins + '</b><span>分钟/天</span></div>' +
      '<div><b>' + words + '</b><span>新词/天</span></div>' +
      '<div><b>' + days + '</b><span>持续天数</span></div>' +
      '</div>';
    const loginHint = m.login_hint
      ? '<p class="ai-nav-login">登录后才能保存课规，提案内容不会丢。' +
        '<a href="' + esc(pendingRedirect()) + '" data-na-login="' + esc(p.raw_text || '') + '">去登录</a></p>'
      : '';
    const err = m.error ? '<p class="ai-nav-login">' + esc(m.error) + '</p>' : '';
    const discardLabel = m.discard_armed ? '再点一次放弃' : '放弃';

    return '<div class="ai-nav-proposal">' +
      '<div class="ai-nav-proposal__title">课规提案</div>' +
      '<p class="ai-nav-proposal__desc">' + esc(desc) + '</p>' +
      stats +
      '<div class="ai-nav-proposal__when">' +
        '<label class="ai-nav-seg"><input type="radio" name="na-when-' + esc(key) + '" value="today" checked><span>今天生效</span></label>' +
        '<label class="ai-nav-seg"><input type="radio" name="na-when-' + esc(key) + '" value="tomorrow"><span>明天生效</span></label>' +
      '</div>' +
      '<p class="ai-nav-proposal__note">确认后立即生效，之后可随时在课规里修改。</p>' +
      '<div class="ai-nav-proposal__acts">' +
        '<button class="btn btn-primary u-btn-sm" type="button" data-na-confirm="' + esc(key) + '"' +
          (m.confirming ? ' disabled' : '') + '>' + (m.confirming ? '确认中…' : '确认生效') + '</button>' +
        '<button class="btn btn-secondary u-btn-sm" type="button" data-na-discard="' + esc(key) + '">' +
          esc(discardLabel) + '</button>' +
      '</div>' + loginHint + err + '</div>';
  }

  // ---------- 提案操作 ----------
  function findMessage(key)
  {
    return state.messages.find((m, i) => String(m.id || 'g' + i) === String(key));
  }

  function selectedWhen(key)
  {
    const el = document.querySelector('input[name="na-when-' + CSS.escape(key) + '"]:checked');
    return el ? el.value : 'today';
  }

  async function confirmProposal(key)
  {
    const m = findMessage(key);
    if (!m || !m.proposal) return;
    m.error = '';

    if (state.guest || !m.id)
    {
      m.login_hint = true;
      renderAll();
      return;
    }
    if (m.confirming) return;

    m.confirming = true;
    renderAll();
    try
    {
      const r = await fetch('/api/ai-nav/proposals/' + m.id + '/confirm', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ activate_from: selectedWhen(key) })
      });
      const j = await r.json().catch(() => ({}));
      if (!r.ok || !j.ok) throw new Error(j.error || '确认失败，请稍后再试');
      m.proposal_status = 'confirmed';
      m.confirm_deadline = j.deadline || '';
      m.login_hint = false;
      toast('课规已生效' + (j.deadline ? '，截止 ' + j.deadline : ''));
    }
    catch (e)
    {
      m.error = e.message || '确认失败，请稍后再试';
    }
    m.confirming = false;
    persistGuestMessages();
    renderAll();
  }

  async function discardProposal(key)
  {
    const m = findMessage(key);
    if (!m || !m.proposal) return;
    // 二次确认，避免误触丢失提案
    if (!m.discard_armed)
    {
      m.discard_armed = true;
      renderAll();
      setTimeout(() =>
      {
        if (m.discard_armed && m.proposal_status === 'proposed')
        {
          m.discard_armed = false;
          renderAll();
        }
      }, 3500);
      return;
    }
    m.discard_armed = false;

    if (state.guest || !m.id)
    {
      m.proposal_status = 'discarded';
      persistGuestMessages();
      renderAll();
      return;
    }
    try
    {
      const r = await fetch('/api/ai-nav/proposals/' + m.id + '/discard', { method: 'POST' });
      const j = await r.json().catch(() => ({}));
      if (!r.ok || !j.ok) throw new Error(j.error || '操作失败');
      m.proposal_status = 'discarded';
      m.error = '';
      toast('已放弃这份提案');
    }
    catch (e)
    {
      m.error = e.message || '操作失败';
    }
    persistGuestMessages();
    renderAll();
  }

  // ---------- 消息操作 ----------
  async function copyMessage(idx)
  {
    const m = state.messages[idx];
    if (!m) return;
    try
    {
      if (navigator.clipboard && navigator.clipboard.writeText)
        await navigator.clipboard.writeText(m.content);
      else
      {
        const ta = document.createElement('textarea');
        ta.value = m.content;
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
      }
      toast('已复制');
    }
    catch (e) { toast('复制失败，请手动选择'); }
  }

  function regenerate(idx)
  {
    if (state.sending) return;
    for (let i = idx - 1; i >= 0; i--)
    {
      const m = state.messages[i];
      if (m.role === 'user')
      {
        send(m.content, true);
        return;
      }
    }
  }

  // ---------- 事件 ----------
  function updateCount()
  {
    const ta = $('ai-nav-input');
    const el = $('ai-nav-count');
    const send = $('ai-nav-send');
    if (!ta || !el) return;
    const n = charLen(ta.value);
    el.textContent = n >= COUNT_HINT ? n + '/' + MAX_CHARS : '';
    el.classList.toggle('is-warn', n >= MAX_CHARS);
    // 输入框随内容增高（上限 120px）
    ta.style.height = 'auto';
    ta.style.height = Math.min(ta.scrollHeight, 120) + 'px';
    // 空内容或发送中时禁用发送按钮
    if (send) send.disabled = !String(ta.value).trim() || state.sending;
  }

  function installEvents()
  {
    $('ai-nav-close').addEventListener('click', closeDrawer);
    $('ai-nav-clear').addEventListener('click', clearConversation);

    const hint = $('ai-nav-hint');
    if (hint) hint.addEventListener('click', openDrawer);

    // 回到底部 + 滚动监听
    const body = document.querySelector('.ai-nav-body');
    const toBottom = $('ai-nav-tobottom');
    if (body && toBottom)
    {
      toBottom.addEventListener('click', () => { body.scrollTop = body.scrollHeight; });
      body.addEventListener('scroll', () => updateToBottom(body), { passive: true });
    }
    $('ai-nav-drawer').addEventListener('click', e =>
    {
      if (e.target === $('ai-nav-drawer')) closeDrawer();
    });
    $('ai-nav-drawer').addEventListener('keydown', trapFocus);

    $('ai-nav-jump-cancel').addEventListener('click', () =>
    {
      cancelJump();
      toast('已取消跳转');
    });

    $('ai-nav-form').addEventListener('submit', e =>
    {
      e.preventDefault();
      const ta = $('ai-nav-input');
      const text = ta.value;
      if (!String(text).trim()) return;
      ta.value = '';
      updateCount();
      send(text);
    });

    $('ai-nav-input').addEventListener('keydown', e =>
    {
      if (e.key === 'Enter' && !e.shiftKey)
      {
        e.preventDefault();
        $('ai-nav-form').requestSubmit();
      }
    });
    $('ai-nav-input').addEventListener('input', updateCount);

    $('ai-nav-chips').addEventListener('click', e =>
    {
      const chip = e.target.closest('[data-na-ask]');
      if (chip) send(chip.getAttribute('data-na-ask'));
    });

    $('ai-nav-list').addEventListener('click', e =>
    {
      const confirmBtn = e.target.closest('[data-na-confirm]');
      if (confirmBtn) { confirmProposal(confirmBtn.getAttribute('data-na-confirm')); return; }
      const discardBtn = e.target.closest('[data-na-discard]');
      if (discardBtn) { discardProposal(discardBtn.getAttribute('data-na-discard')); return; }
      const login = e.target.closest('[data-na-login]');
      if (login) { rememberPendingPlan(login.getAttribute('data-na-login')); return; }
      const copyBtn = e.target.closest('[data-na-copy]');
      if (copyBtn) { copyMessage(Number(copyBtn.getAttribute('data-na-copy'))); return; }
      const regenBtn = e.target.closest('[data-na-regen]');
      if (regenBtn) { regenerate(Number(regenBtn.getAttribute('data-na-regen'))); return; }
      if (e.target.closest('[data-na-retry]')) loadInitial();
    });

    document.addEventListener('keydown', e =>
    {
      if (e.key === 'Escape' && state.open) closeDrawer();
    });

    window.addEventListener('resize', applyPosition);
  }

  function boot()
  {
    if ($('ai-nav-fab')) return;
    document.body.insertAdjacentHTML('beforeend', WIDGET_HTML);
    applyPosition();
    installDrag();
    installEvents();
    updateCount();
    maybeShowHint();
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
