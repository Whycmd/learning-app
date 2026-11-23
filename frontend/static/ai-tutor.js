// ============================================================================
// ai-tutor.js — v1.5.0-T006 AI 讲解页签 + 知识点详情渲染
// 依赖：/static/marked.min.js、/static/md-render.js、/static/tts.js（按需懒加载）
// 约定：AI 文本一律走 renderMarkdown（解析 → 白名单消毒 → 输出），不直接 innerHTML
// ============================================================================
(function ()
{
  'use strict';

  var libPromise = null;

  function loadScript(src)
  {
    return new Promise(function (resolve, reject)
    {
      var s = document.createElement('script');
      s.src = src;
      s.onload = function () { resolve(); };
      s.onerror = function () { reject(new Error('load failed: ' + src)); };
      document.head.appendChild(s);
    });
  }

  // 按需加载：marked → md-render → tts
  function ensureLibs()
  {
    if (libPromise) return libPromise;
    libPromise = loadScript('/static/marked.min.js')
      .then(function () { return loadScript('/static/md-render.js'); })
      .then(function () { return loadScript('/static/tts.js'); })
      .catch(function () { /* 渲染库失败时 md-render 内部降级为纯文本 */ });
    return libPromise;
  }

  function esc(s)
  {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function (c)
    {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c];
    });
  }

  function fmtEta(sec)
  {
    var s = Number(sec || 0);
    if (s <= 0) return '';
    if (s < 60) return s + ' 秒';
    return Math.ceil(s / 60) + ' 分钟';
  }

  // 本地粗略识别输入类型，仅用于提示
  function typeHint(text)
  {
    var t = String(text || '').trim();
    if (!t) return '';
    if (t.length > 200) return '识别为：段落（只提取长难句）';
    var words = t.split(/\s+/).filter(Boolean).length;
    if (/[.!?]$/.test(t) || words >= 6) return '识别为：句子';
    if (/\s/.test(t)) return '识别为：短语';
    return '识别为：单词';
  }

  function setMsg(id, cls, text)
  {
    var e = document.getElementById(id);
    if (!e) return;
    e.className = 'state-line' + (cls ? ' ' + cls : '');
    e.textContent = text || '';
  }

  function loginLink(text)
  {
    var back = encodeURIComponent(location.pathname + location.search);
    return '<a href="/pages/login.html?redirect=' + back + '&keep=1">' + esc(text) + '</a>';
  }

  // ==========================================================================
  // 知识点详情渲染（原 kp-detail.html 逻辑迁入）
  // ==========================================================================

  var EXTRA_LABEL = {
    phonetic: '音标', root: '词根与词源', word_family: '同源词汇', allusions: '典故、俗语',
    synonym_analysis: '同义词辨析', phrase_analysis: '短语额外解析', key_points: '重点补充',
    frequency: '使用频率', register: '语域说明', cultural: '文化延伸', related_words: '同类词语'
  };

  var EXTRA_ORDER = [
    'phonetic', 'root', 'word_family', 'allusions',
    'synonym_analysis', 'phrase_analysis', 'key_points',
    'frequency', 'register', 'cultural', 'related_words'
  ];

  var DETAIL_ICON = {
    markdown_content: '<svg viewBox="0 0 24 24"><path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/></svg>',
    phonetic: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/></svg>',
    root: '<svg viewBox="0 0 24 24"><path d="M12 2v20"/><path d="M12 2a5 5 0 0 1 5 5v0a5 5 0 0 1-5 5"/><path d="M12 7a5 5 0 0 0-5 5v0a5 5 0 0 0 5 5"/></svg>',
    word_family: '<svg viewBox="0 0 24 24"><path d="M17 10H8"/><path d="M17 14H8"/><path d="M21 6H3"/><path d="M21 18H3"/></svg>',
    allusions: '<svg viewBox="0 0 24 24"><path d="M12 2L2 7l10 5 10-5-10-5z"/><path d="M2 17l10 5 10-5"/><path d="M2 12l10 5 10-5"/></svg>',
    synonym_analysis: '<svg viewBox="0 0 24 24"><path d="M9 11l3-3 3 3"/><path d="M9 13l3 3 3-3"/></svg>',
    phrase_analysis: '<svg viewBox="0 0 24 24"><rect x="3" y="3" width="18" height="18" rx="2"/><path d="M3 9h18"/><path d="M9 21V9"/></svg>',
    key_points: '<svg viewBox="0 0 24 24"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/></svg>',
    frequency: '<svg viewBox="0 0 24 24"><path d="M3 3v18h18"/><path d="m19 9-5 5-4-4-3 3"/></svg>',
    register: '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><path d="M12 8v4l3 3"/></svg>',
    cultural: '<svg viewBox="0 0 24 24"><path d="M12 2a7 7 0 0 1 7 7c0 5-7 13-7 13S5 14 5 9a7 7 0 0 1 7-7z"/><circle cx="12" cy="9" r="2.5"/></svg>',
    related_words: '<svg viewBox="0 0 24 24"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/></svg>'
  };

  function extraIcon(type)
  {
    return DETAIL_ICON[type] || '<svg viewBox="0 0 24 24"><path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/></svg>';
  }

  function mdBlock(md)
  {
    if (typeof window.renderMarkdown === 'function') return window.renderMarkdown(md);
    return esc(md).replace(/\n/g, '<br>');
  }

  // 返回详情 HTML；TTS 由调用方在插入 DOM 后绑定（事件委托亦可）
  function renderKpDetail(d)
  {
    var extras = {};
    (d.extras || []).forEach(function (e) { extras[e.extra_type] = e.content; });

    var blocks = [];
    var word = d.standard_content || d.user_input_content || '';
    var ttsArg = esc(word);

    var defs = (d.definitions || []).map(function (x, i)
    {
      return '<div class="entry"><div class="entry__main">' +
        '<div class="entry__foreign">' + (i + 1) + '. [' + esc(x.pos) + '] ' + esc(x.meaning) + '</div>' +
        (x.example ? '<div class="entry__native">' + esc(x.example) + '</div>' : '') +
        '</div></div>';
    }).join('');

    blocks.push(
      '<section class="wing-card">' +
        '<div class="wing-card__cap">' +
          '<span class="wing-card__ico"><svg viewBox="0 0 24 24"><path d="M3 18v-6a9 9 0 0 1 18 0v6"/><path d="M21 19a2 2 0 0 1-2 2h-1a2 2 0 0 1-2-2v-3a2 2 0 0 1 2-2h3zM3 19a2 2 0 0 0 2 2h1a2 2 0 0 0 2-2v-3a2 2 0 0 0-2-2H3z"/></svg></span>' +
          '<span class="kp-title">' + esc(word) + '</span>' +
          '<button class="speak-btn u-push-end" type="button" data-tts="' + ttsArg + '" title="朗读">' +
            '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18"><polygon points="11 5 6 9 2 9 2 15 6 19 11 5"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14"/></svg>' +
          '</button>' +
        '</div>' +
        '<div class="pack__badges">' +
          (extras.phonetic ? '<span class="pill">' + esc(extras.phonetic) + '</span>' : '') +
          (d.master_require ? '<span class="pill pill--ai">' + esc(d.master_require) + '</span>' : '') +
          '<span class="pill">难度 ' + (d.difficulty != null ? esc(d.difficulty) : '-') + '</span>' +
          (d.is_solidified === 1 ? '' : '<span class="pill">待生成精讲</span>') +
        '</div>' +
        '<div class="kp-sub">核心释义 &amp; 例句</div>' +
        (defs || '<p class="state-line">无</p>') +
      '</section>');

    if ((d.collocations || []).length)
    {
      var colHtml = d.collocations.map(function (c)
      {
        return '<div class="entry"><div class="entry__main">' +
          '<div class="entry__foreign">• ' + esc(c.collocation) + '</div>' +
          (c.example ? '<div class="entry__native">' + esc(c.example) + '</div>' : '') +
          '</div></div>';
      }).join('');
      blocks.push(
        '<section class="wing-card">' +
          '<div class="wing-card__cap">' +
            '<span class="wing-card__ico"><svg viewBox="0 0 24 24"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg></span>' +
            '<span class="wing-card__title">固定搭配</span>' +
            '<span class="wing-card__meta">共 ' + d.collocations.length + ' 条</span>' +
          '</div>' + colHtml +
        '</section>');
    }

    var syns = (d.synonyms || []).map(function (s) { return esc(s.synonym); }).join(' / ');
    var ants = (d.antonyms || []).map(function (s) { return esc(s.antonym); }).join(' / ');
    if (syns || ants)
    {
      blocks.push(
        '<section class="wing-card">' +
          '<div class="wing-card__cap">' +
            '<span class="wing-card__ico"><svg viewBox="0 0 24 24"><path d="m18 16 4-4-4-4"/><path d="m6 8-4 4 4 4"/><path d="m14.5 4-5 16"/></svg></span>' +
            '<span class="wing-card__title">近义 / 反义</span>' +
          '</div>' +
          (syns ? '<p class="kp-synonym"><strong>近义：</strong>' + syns + '</p>' : '') +
          (ants ? '<p class="kp-synonym"><strong>反义：</strong>' + ants + '</p>' : '') +
        '</section>');
    }

    if (extras.markdown_content)
    {
      blocks.push(
        '<section class="wing-card">' +
          '<div class="wing-card__cap">' +
            '<span class="wing-card__ico">' + extraIcon('markdown_content') + '</span>' +
            '<span class="wing-card__title">单词精讲</span>' +
          '</div>' +
          '<div class="md-content">' + mdBlock(extras.markdown_content) + '</div>' +
        '</section>');
    }

    EXTRA_ORDER.forEach(function (t)
    {
      if (!extras[t]) return;
      blocks.push(
        '<section class="wing-card">' +
          '<div class="wing-card__cap">' +
            '<span class="wing-card__ico">' + extraIcon(t) + '</span>' +
            '<span class="wing-card__title">' + esc(EXTRA_LABEL[t] || t) + '</span>' +
          '</div>' +
          '<div class="kp-extra">' + esc(extras[t]).replace(/\n/g, '<br>') + '</div>' +
        '</section>');
    });

    if (!blocks.length)
      blocks.push('<section class="wing-card"><p class="state-line state-line--danger">该词条暂无精讲内容，请点击「一键生成精讲」生成。</p></section>');

    return blocks.join('');
  }

  function bindTts(container)
  {
    container.querySelectorAll('[data-tts]').forEach(function (btn)
    {
      btn.addEventListener('click', function ()
      {
        if (window.TTS && typeof window.TTS.speak === 'function') window.TTS.speak(btn.dataset.tts);
      });
    });
  }

  // ==========================================================================
  // AI 讲解页签
  // ==========================================================================

  var ex = {
    id: 0, taskId: 0, status: '', depth: 'standard', level: 0,
    position: 0, eta: 0, etaLeft: 0,
    poll: null, etaTimer: null, me: { guest: true, user_id: 0 }, inited: false
  };

  function exStop()
  {
    if (ex.poll) { clearTimeout(ex.poll); ex.poll = null; }
    if (ex.etaTimer) { clearInterval(ex.etaTimer); ex.etaTimer = null; }
  }

  function queueText()
  {
    var label = ex.status === 'running' ? 'AI 生成中' : '排队中';
    var line = label;
    if (ex.level > 0) line += '（空闲队列 L' + ex.level + '，正常队列空闲后执行）';
    if (ex.position > 0) line += ' · 前方 ' + ex.position + ' 个任务';
    var eta = fmtEta(ex.etaLeft);
    if (eta) line += ' · 预计等待 ' + eta;
    else if (ex.status === 'queued') line += ' · 马上开始';
    else line += ' · 即将完成';
    return line;
  }

  function showQueue(hintTransfer)
  {
    var box = document.getElementById('ex-queue');
    if (!box) return;
    box.hidden = false;
    var t = document.getElementById('ex-queue-text');
    if (t) t.textContent = queueText();

    var canOps = ex.me && !ex.me.guest;
    var expBtn = document.getElementById('ex-expedite');
    var defBtn = document.getElementById('ex-defer');
    if (expBtn) expBtn.hidden = !canOps || ex.level <= 0;
    if (defBtn) defBtn.hidden = !canOps || ex.level > 0 || !hintTransfer;

    if (!ex.etaTimer)
    {
      ex.etaTimer = setInterval(function ()
      {
        if (ex.etaLeft > 0) ex.etaLeft = Math.max(0, ex.etaLeft - 1);
        var tt = document.getElementById('ex-queue-text');
        if (tt) tt.textContent = queueText();
      }, 1000);
    }
  }

  function hideQueue()
  {
    var box = document.getElementById('ex-queue');
    if (box) box.hidden = true;
    if (ex.etaTimer) { clearInterval(ex.etaTimer); ex.etaTimer = null; }
  }

  function renderResult(item, l1Hit)
  {
    var card = document.getElementById('ex-result');
    if (!card) return;
    card.hidden = false;
    var tipOld = document.getElementById('ex-public-tip');
    if (tipOld) tipOld.hidden = true;

    var meta = document.getElementById('ex-result-meta');
    if (meta)
    {
      var parts = [];
      parts.push(item.l1_hit ? '命中已有精讲 · 秒回' : 'AI 生成');
      parts.push('深度 ' + ({ brief: '简明', standard: '标准', deep: '深度' }[item.depth] || item.depth));
      if (item.created_at) parts.push(item.created_at);
      meta.textContent = parts.join(' · ');
    }

    var sum = document.getElementById('ex-summary');
    if (sum) sum.textContent = item.summary || '';

    // v1.5.0-T008：精讲正文是唯一正文，直接展示，不再有独立 sections 区
    var md = item.content_md || (item.result && item.result.markdown_content) || '';
    var mdBox = document.getElementById('ex-md');
    if (mdBox)
    {
      if (md)
      {
        ensureLibs().then(function () { mdBox.innerHTML = mdBlock(md); });
      }
      else mdBox.innerHTML = '<p class="state-line">暂无精讲正文</p>';
    }

    var hint = document.getElementById('ex-l1-hint');
    if (hint) hint.hidden = !item.l1_hit;

    updateActionState();
  }

  function updateActionState()
  {
    var logged = ex.me && !ex.me.guest;
    ['ex-save-kp', 'ex-to-wb'].forEach(function (id)
    {
      var b = document.getElementById(id);
      if (b) b.title = logged ? '' : '登录后可保存';
    });
    var wb = document.getElementById('ex-wb-wrap');
    if (wb) wb.hidden = !logged;
    var hist = document.getElementById('ex-history-card');
    if (hist) hist.hidden = !logged;
    var guestTip = document.getElementById('ex-guest-tip');
    if (guestTip) guestTip.hidden = logged;
  }

  function startPoll(id)
  {
    exStop();
    ex.id = id;
    try { sessionStorage.setItem('ex_last_id', String(id)); } catch (e) {}

    function tick()
    {
      fetch('/api/explain/' + id, { cache: 'no-store' })
        .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
        .then(function (res)
        {
          var j = res.j || {};
          if (!res.status || res.status >= 400 || !j.ok)
          {
            exStop();
            hideQueue();
            setMsg('ex-msg', 'state-line--danger', j.error || '讲解查询失败');
            return;
          }
          var it = j.item || {};
          ex.status = it.status;
          ex.taskId = it.task_id || 0;
          ex.level = it.queue_level || 0;
          ex.position = it.position || 0;
          ex.eta = it.eta_sec || 0;
          var first = ex.etaLeft <= 0 && (ex.status === 'queued' || ex.status === 'running');
          if (ex.status === 'queued' || ex.status === 'running')
          {
            ex.etaLeft = first ? ex.eta : Math.min(ex.etaLeft, ex.eta);
            showQueue(!!j.hint_transfer);
            ex.poll = setTimeout(tick, ex.status === 'queued' ? 1500 : 1200);
            return;
          }
          exStop();
          hideQueue();
          if (ex.status === 'done')
          {
            setMsg('ex-msg', 'state-line--success', it.l1_hit ? '命中已有精讲，已同步返回' : '讲解生成完成');
            renderResult(it, it.l1_hit);
            loadFollowups(id);
          }
          else if (ex.status === 'failed')
          {
            setMsg('ex-msg', 'state-line--danger', it.error || '生成失败，可重试');
          }
        })
        .catch(function ()
        {
          exStop();
          setMsg('ex-msg', 'state-line--danger', '网络异常，无法获取进度');
        });
    }

    setMsg('ex-msg', '', '正在获取讲解…');
    var card = document.getElementById('ex-result');
    if (card) card.hidden = true;
    tick();
  }

  function submit()
  {
    var input = document.getElementById('ex-text');
    var text = (input.value || '').trim();
    if (!text) { setMsg('ex-msg', 'state-line--danger', '请输入要讲解的内容'); return; }

    var btn = document.getElementById('ex-submit');
    btn.disabled = true;
    setMsg('ex-msg', '', '提交中…');

    fetch('/api/explain', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: text, depth: ex.depth, queue_level: 0 })
    })
      .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
      .then(function (res)
      {
        var j = res.j || {};
        if (res.status >= 400 || !j.ok)
        {
          var msg = j.error || '提交失败';
          if (res.status === 401)
          {
            var el = document.getElementById('ex-msg');
            if (el)
            {
              el.className = 'state-line state-line--danger';
              el.innerHTML = esc(msg) + ' · ' + loginLink('去登录');
            }
          }
          else setMsg('ex-msg', 'state-line--danger', msg);
          btn.disabled = false;
          return;
        }
        startPoll(j.id);
        loadHistory();
        btn.disabled = false;
      })
      .catch(function ()
      {
        setMsg('ex-msg', 'state-line--danger', '网络异常');
        btn.disabled = false;
      });
  }

  function regenerate()
  {
    if (!ex.id) return;
    var btn = document.getElementById('ex-regenerate');
    btn.disabled = true;
    setMsg('ex-msg', '', '重新生成会跳过 L1 缓存，按当前深度重跑…');
    fetch('/api/explain/' + ex.id + '/regenerate', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ depth: ex.depth, queue_level: 0 })
    })
      .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
      .then(function (res)
      {
        var j = res.j || {};
        if (res.status >= 400 || !j.ok)
        {
          var msg = j.error || '重新生成失败';
          if (res.status === 401)
          {
            var el = document.getElementById('ex-msg');
            if (el)
            {
              el.className = 'state-line state-line--danger';
              el.innerHTML = esc(msg) + ' · ' + loginLink('去登录');
            }
          }
          else setMsg('ex-msg', 'state-line--danger', msg);
          btn.disabled = false;
          return;
        }
        startPoll(j.id);
        loadHistory();
        btn.disabled = false;
      })
      .catch(function ()
      {
        setMsg('ex-msg', 'state-line--danger', '网络异常');
        btn.disabled = false;
      });
  }

  function saveKp()
  {
    if (!ex.id) return;
    if (ex.me.guest) { setMsg('ex-act-msg', 'state-line--danger', '登录后可保存为词条'); return; }
    var btn = document.getElementById('ex-save-kp');
    btn.disabled = true;
    fetch('/api/explain/' + ex.id + '/save-kp', { method: 'POST' })
      .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
      .then(function (res)
      {
        var j = res.j || {};
        if (res.status >= 400 || !j.ok) setMsg('ex-act-msg', 'state-line--danger', j.error || '保存失败');
        else
        {
          var el = document.getElementById('ex-act-msg');
          el.className = 'state-line state-line--success';
          el.innerHTML = '已保存为词条 #' + j.kp_id + ' · <a href="/pages/kp-entry.html?id=' + j.kp_id + '">查看</a>';
          var tip = document.getElementById('ex-public-tip');
          if (tip)
          {
            tip.hidden = false;
            tip.className = 'state-line';
            tip.innerHTML = '本次更新仅自己可见 · <button class="link-btn" type="button" data-pub="' + j.kp_id + '">公开本次更新</button>';
          }
        }
        btn.disabled = false;
      })
      .catch(function () { setMsg('ex-act-msg', 'state-line--danger', '网络异常'); btn.disabled = false; });
  }

  // v1.5.0-T009 T6：讲解导出 Word
  // 游客：仅可导出当次最新讲解（后端按 guest_id 校验）
  // 登录用户：不直出，提示先保存为词条（D13）
  function exportExplainWord()
  {
    if (!ex.id) return setMsg('ex-act-msg', 'state-line--danger', '暂无可导出内容');
    if (ex.me && !ex.me.guest)
    {
      setMsg('ex-act-msg', '', '请先「保存为词条」，再从词条导出讲义。');
      return;
    }
    if (ex.status !== 'done') return setMsg('ex-act-msg', 'state-line--danger', '讲解完成后可导出');
    var md = document.getElementById('ex-md');
    var chars = md ? md.textContent.length : 0;
    var est = ExportEstimator.explanation(1, chars);
    var btn = document.getElementById('ex-export-word');
    ExportEstimator.confirm({ title: '导出讲解 Word', pages: est.pages, chars: est.chars })
      .then(function (go)
      {
        if (!go) return;
        if (btn) btn.disabled = true;
        setMsg('ex-act-msg', '', '正在生成 Word…');
        fetch('/api/exports/explanation/' + ex.id, { method: 'POST' })
          .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
          .then(function (res)
          {
            var j = res.j || {};
            if (res.status >= 400 || !j.ok || !j.word_path)
            {
              setMsg('ex-act-msg', 'state-line--danger', j.error || '导出服务暂不可用，请重试');
              return;
            }
            setMsg('ex-act-msg', 'state-line--success', '已生成，开始下载');
            window.open('/api/word/download?path=' + encodeURIComponent(j.word_path), '_blank');
          })
          .catch(function () { setMsg('ex-act-msg', 'state-line--danger', '导出服务暂不可用，请重试'); })
          .then(function () { if (btn) btn.disabled = false; });
      });
  }

  function loadWordbooks()
  {
    if (ex.me.guest) return;
    fetch('/api/wordbook', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (j)
      {
        var sel = document.getElementById('ex-wb-select');
        if (!sel) return;
        var list = j.data || [];
        sel.innerHTML = list.map(function (w)
        {
          return '<option value="' + w.id + '">' + esc(w.name) + '（' + w.entry_count + '）</option>';
        }).join('');
        if (!list.length) sel.innerHTML = '<option value="">暂无单词本</option>';
      })
      .catch(function () {});
  }

  function toWordbook()
  {
    if (!ex.id) return;
    if (ex.me.guest) { setMsg('ex-act-msg', 'state-line--danger', '登录后可加入单词本'); return; }
    var sel = document.getElementById('ex-wb-select');
    var wbId = sel && sel.value ? Number(sel.value) : 0;
    if (!wbId) { setMsg('ex-act-msg', 'state-line--danger', '请先在单词本页创建一个单词本'); return; }

    var btn = document.getElementById('ex-to-wb');
    btn.disabled = true;
    fetch('/api/explain/' + ex.id + '/to-wordbook', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ wordbook_id: wbId })
    })
      .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
      .then(function (res)
      {
        var j = res.j || {};
        if (res.status >= 400 || !j.ok) setMsg('ex-act-msg', 'state-line--danger', j.error || '加入失败');
        else setMsg('ex-act-msg', 'state-line--success', '已加入单词本（新增 ' + (j.added || 0) + '，跳过 ' + (j.skipped || 0) + '）');
        btn.disabled = false;
      })
      .catch(function () { setMsg('ex-act-msg', 'state-line--danger', '网络异常'); btn.disabled = false; });
  }

  function taskOp(kind)
  {
    if (!ex.taskId || ex.me.guest) return;
    var url = '/api/ai-tasks/' + ex.taskId + '/' + kind;
    var body = kind === 'defer' ? JSON.stringify({ level: Math.max(1, ex.level + 1) }) : '{}';
    fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: body })
      .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
      .then(function (res)
      {
        var j = res.j || {};
        if (res.status >= 400 || !j.ok) setMsg('ex-msg', 'state-line--danger', j.error || '操作失败');
        else
        {
          setMsg('ex-msg', '', kind === 'expedite' ? '已加急，回到正常队列' : '已转入空闲队列');
          startPoll(ex.id);
        }
      })
      .catch(function () { setMsg('ex-msg', 'state-line--danger', '网络异常'); });
  }

  function loadHistory()
  {
    if (ex.me.guest) return;
    var box = document.getElementById('ex-history');
    if (!box) return;
    fetch('/api/explain/history?limit=10', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (j)
      {
        var items = j.items || [];
        if (!items.length) { box.innerHTML = '<p class="state-line">暂无讲解记录。</p>'; return; }
        box.innerHTML = items.map(function (it)
        {
          var label = { done: '完成', queued: '排队中', running: '生成中', failed: '失败' }[it.status] || it.status;
          return '<div class="ex-history__row" data-id="' + it.id + '">' +
            '<span class="ex-history__word">' + esc(it.input_text) + '</span>' +
            '<span class="state-line">' + esc(label) + '</span>' +
            '<span class="ex-history__meta">' + esc(it.created_at) + '</span>' +
          '</div>';
        }).join('');
      })
      .catch(function () { box.innerHTML = '<p class="state-line">加载失败</p>'; });
  }

  // ===== v1.5.0-T008：学习追问 =====
  function renderFollowups(items)
  {
    var box = document.getElementById('ex-followup-list');
    if (!box) return;
    if (!items || !items.length)
    {
      box.innerHTML = '<p class="state-line">还没有追问。学习中有疑问，可以在下方继续问。</p>';
      return;
    }
    box.innerHTML = items.map(function (f)
    {
      var label = { queued: '排队中', running: '生成中', failed: '生成失败' }[f.status] || '';
      var ans = f.answer_md ? mdBlock(f.answer_md) : (label ? '<p class="state-line">' + esc(label) + '</p>' : '');
      var act = '';
      if (f.status === 'done' && f.answer_md)
      {
        if (f.merged) act = '<span class="pill pill--ai">已加入公共精讲</span>';
        else if (ex.me.guest) act = '<span class="state-line">登录后可加入公共精讲</span>';
        else act = '<button class="btn btn-secondary u-btn-sm" type="button" data-merge="' + f.id + '">加入公共精讲</button>';
      }
      return '<div class="ex-fu">' +
               '<div class="ex-fu__q">' + esc(f.question) + '</div>' +
               '<div class="ex-fu__a">' + ans + '</div>' +
               (act ? '<div class="ex-fu__act">' + act + '</div>' : '') +
             '</div>';
    }).join('');
  }

  function loadFollowups(explainId)
  {
    if (!explainId) return;
    var card = document.getElementById('ex-followup');
    if (!card) return;
    card.hidden = false;
    fetch('/api/explain/' + explainId + '/followup', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (j)
      {
        var items = j.items || [];
        ensureLibs().then(function () { renderFollowups(items); });
        var pending = items.some(function (f) { return f.status === 'queued' || f.status === 'running'; });
        if (pending && ex.id === Number(explainId))
        {
          setTimeout(function () { loadFollowups(explainId); }, 1500);
        }
      })
      .catch(function () {});
  }

  function sendFollowup()
  {
    var input = document.getElementById('ex-followup-input');
    var q = input ? input.value.trim() : '';
    if (!ex.id) return;
    if (!q) { setMsg('ex-followup-msg', 'state-line--danger', '请输入追问内容'); return; }
    var btn = document.getElementById('ex-followup-send');
    btn.disabled = true;
    fetch('/api/explain/' + ex.id + '/followup', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ question: q })
    })
      .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
      .then(function (res)
      {
        var j = res.j || {};
        if (res.status >= 400 || !j.ok)
        {
          var msg = j.error || '追问失败';
          if (res.status === 401)
          {
            var el = document.getElementById('ex-followup-msg');
            if (el) { el.className = 'state-line state-line--danger'; el.innerHTML = esc(msg) + ' · ' + loginLink('去登录'); }
          }
          else setMsg('ex-followup-msg', 'state-line--danger', msg);
        }
        else
        {
          input.value = '';
          setMsg('ex-followup-msg', '', '已提问，正在生成回答…');
          loadFollowups(ex.id);
        }
        btn.disabled = false;
      })
      .catch(function () { setMsg('ex-followup-msg', 'state-line--danger', '网络异常'); btn.disabled = false; });
  }

  function mergeFollowup(fid, btn)
  {
    if (!ex.id || !fid) return;
    btn.disabled = true;
    fetch('/api/explain/' + ex.id + '/followup/' + fid + '/merge', { method: 'POST' })
      .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
      .then(function (res)
      {
        var j = res.j || {};
        if (res.status >= 400 || !j.ok) setMsg('ex-followup-msg', 'state-line--danger', j.error || '加入失败');
        else setMsg('ex-followup-msg', 'state-line--success', j.already ? '已加入过公共精讲' : '已加入公共精讲');
        loadFollowups(ex.id);
      })
      .catch(function () { setMsg('ex-followup-msg', 'state-line--danger', '网络异常'); btn.disabled = false; });
  }

  function initExplain()
  {
    if (ex.inited) return;
    var root = document.getElementById('panel-explain');
    if (!root) return;
    ex.inited = true;

    var input = document.getElementById('ex-text');
    var hint = document.getElementById('ex-type');

    input.addEventListener('input', function ()
    {
      if (hint) hint.textContent = typeHint(input.value);
    });

    var depthBox = document.getElementById('ex-depth');
    depthBox.addEventListener('click', function (e)
    {
      var btn = e.target.closest('button[data-depth]');
      if (!btn) return;
      depthBox.querySelectorAll('button').forEach(function (b) { b.classList.remove('is-active'); });
      btn.classList.add('is-active');
      ex.depth = btn.dataset.depth;
    });

    document.getElementById('ex-submit').addEventListener('click', submit);
    input.addEventListener('keydown', function (e)
    {
      if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') submit();
    });
    document.getElementById('ex-regenerate').addEventListener('click', regenerate);
    document.getElementById('ex-export-word').addEventListener('click', exportExplainWord);
    document.getElementById('ex-save-kp').addEventListener('click', saveKp);
    document.getElementById('ex-to-wb').addEventListener('click', toWordbook);
    document.getElementById('ex-expedite').addEventListener('click', function () { taskOp('expedite'); });
    document.getElementById('ex-defer').addEventListener('click', function () { taskOp('defer'); });

    var fuSend = document.getElementById('ex-followup-send');
    if (fuSend) fuSend.addEventListener('click', sendFollowup);
    var fuInput = document.getElementById('ex-followup-input');
    if (fuInput) fuInput.addEventListener('keydown', function (e)
    {
      if (e.key === 'Enter') { e.preventDefault(); sendFollowup(); }
    });
    var fuList = document.getElementById('ex-followup-list');
    if (fuList) fuList.addEventListener('click', function (e)
    {
      var b = e.target.closest('button[data-merge]');
      if (!b) return;
      mergeFollowup(Number(b.dataset.merge), b);
    });
    var pubTip = document.getElementById('ex-public-tip');
    if (pubTip) pubTip.addEventListener('click', function (e)
    {
      var b = e.target.closest('button[data-pub]');
      if (!b) return;
      b.disabled = true;
      fetch('/api/kp/' + b.dataset.pub + '/visibility', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ public: true })
      })
        .then(function (r) { return r.json().then(function (j) { return { status: r.status, j: j }; }); })
        .then(function (res)
        {
          var j = res.j || {};
          if (res.status >= 400 || !j.ok) { setMsg('ex-act-msg', 'state-line--danger', j.error || '公开失败'); b.disabled = false; return; }
          pubTip.hidden = true;
          setMsg('ex-act-msg', 'state-line--success', '已公开到公共精讲');
        })
        .catch(function () { setMsg('ex-act-msg', 'state-line--danger', '网络异常'); b.disabled = false; });
    });

    document.getElementById('ex-history').addEventListener('click', function (e)
    {
      var row = e.target.closest('.ex-history__row');
      if (!row) return;
      startPoll(Number(row.dataset.id));
      document.getElementById('ex-result').scrollIntoView({ behavior: 'smooth', block: 'start' });
    });

    // 身份 + 历史
    fetch('/api/auth/me', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (me)
      {
        ex.me = { guest: !!me.guest, user_id: me.user_id || 0 };
        updateActionState();
        loadWordbooks();
        loadHistory();
      })
      .catch(function () {});

    // 刷新恢复：继续上一次未完成的讲解
    var last = 0;
    try { last = Number(sessionStorage.getItem('ex_last_id') || 0); } catch (e) {}
    if (last > 0) startPoll(last);
  }

  window.AITutor = {
    ensureLibs: ensureLibs,
    renderKpDetail: renderKpDetail,
    bindTts: bindTts,
    initExplain: initExplain,
    esc: esc,
    mdBlock: mdBlock
  };
})();
