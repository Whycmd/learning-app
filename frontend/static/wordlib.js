/* v1.5.0-T005 单词本：共用单词本 / 私人单词本 / 待整理生词抽屉 */
(function ()
{
  'use strict';

  const $ = id => document.getElementById(id);
  const state = {
    builtin: [],
    user: [],
    range: [],
    wordbooks: [],
    myWords: [],
    myWordsTotal: 0,
    detail: { kind: null, id: 0, data: null, items: [] },
    trail: [],
    me: null,
    uiMode: 'function',
    pendingItems: [],
    addTargetId: 0,
    addItems: [],
    // v1.5.0-T007：列表视图状态
    ui: {
      tab: 'shared',
      sharedFilter: 'all',
      privateFilter: 'all',
      openVols: [],            // 默认全部折叠
      limit: { list: 6, wordbook: 6 },
      more: { list: false, wordbook: false }
    },
    combo: { mode: 'create', id: 0, members: [], original: [], wordItems: [] }
  };

  function esc(s)
  {
    return String(s == null ? '' : s).replace(/[&<>"']/g, c =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }

  async function api(url, opts)
  {
    try
    {
      const res = await fetch(url, Object.assign({ cache: 'no-store' }, opts || {}));
      let body = {};
      try { body = await res.json(); } catch (e) { body = {}; }
      body.__status = res.status;
      return body;
    }
    catch (e)
    {
      return { error: '网络请求失败', __status: 0 };
    }
  }

  function applyMe(me)
  {
    if (me && !me.guest && me.user_id)
    {
      state.me = me;
      state.uiMode = me.ui_mode === 'plan' ? 'plan' : 'function';
      return true;
    }
    return false;
  }

  async function loadMe()
  {
    applyMe(await api('/api/auth/me'));
  }

  async function ensureLogin()
  {
    const me = await api('/api/auth/me');
    if (applyMe(me)) return true;
    location.assign('/pages/login.html?redirect=' + encodeURIComponent(location.pathname + location.search));
    return false;
  }

  // ---------- 抽屉 ----------

  function openDrawer(id) { $(id).classList.add('open'); }
  function closeDrawer(id) { $(id).classList.remove('open'); }

  document.querySelectorAll('[data-close]').forEach(btn =>
  {
    btn.addEventListener('click', () => closeDrawer(btn.dataset.close));
  });
  document.addEventListener('keydown', e =>
  {
    if (e.key !== 'Escape') return;
    document.querySelectorAll('.drawer-overlay.open').forEach(d => d.classList.remove('open'));
  });
  document.querySelectorAll('.drawer-overlay').forEach(mask =>
  {
    mask.addEventListener('click', e => { if (e.target === mask) mask.classList.remove('open'); });
  });

  function msg(id, text, danger)
  {
    const el = $(id);
    if (!el) return;
    el.className = 'state-line' + (danger ? ' state-line--danger' : '');
    el.textContent = text || '';
  }

  // ---------- 加入学习开关 / 30 条询问 ----------

  function studyKey(way)
  {
    const uid = (state.me && state.me.user_id) || 0;
    return 'entryStudy:' + uid + ':' + state.uiMode + ':' + way;
  }

  function studyPref(way)
  {
    let v = null;
    try { v = localStorage.getItem(studyKey(way)); } catch (e) { v = null; }
    if (v === '1' || v === '0') return v === '1';
    return state.uiMode === 'plan';
  }

  function setStudyPref(way, val)
  {
    try { localStorage.setItem(studyKey(way), val ? '1' : '0'); } catch (e) {}
  }

  function bindSwitch(id, way)
  {
    const el = $(id);
    if (el) el.addEventListener('change', () => setStudyPref(way, el.checked));
  }

  function syncSwitch(id, way)
  {
    const el = $(id);
    if (el) el.checked = studyPref(way);
  }

  // null = 用户取消；true = 加入学习；false = 仅录入
  let askResolve = null;

  function askJoinStudy(count)
  {
    return new Promise(resolve =>
    {
      askResolve = resolve;
      $('ask-title').textContent = '本次将新建 ' + count + ' 个单词';
      openDrawer('modal-ask-study');
    });
  }

  function finishAsk(val)
  {
    closeDrawer('modal-ask-study');
    if (!askResolve) return;
    const r = askResolve;
    askResolve = null;
    r(val);
  }

  $('act-ask-join').addEventListener('click', () => finishAsk(true));
  $('act-ask-only').addEventListener('click', () => finishAsk(false));
  $('modal-ask-study').addEventListener('click', e =>
  {
    if (e.target === $('modal-ask-study')) finishAsk(null);
  });
  document.addEventListener('keydown', e =>
  {
    if (e.key === 'Escape' && askResolve) finishAsk(null);
  });

  function emptyResult()
  {
    return { input: 0, unique: 0, existing: 0, to_create: 0, need_confirm: false };
  }

  async function previewItems(items)
  {
    const words = (items || []).map(x => ({
      word: x.word,
      meaning: x.meaning || '',
      pos: x.pos || ''
    })).filter(x => x.word);
    if (!words.length) return emptyResult();
    const r = await api('/api/kp/preview', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ words })
    });
    if (typeof r.to_create !== 'number') return emptyResult();
    return r;
  }

  // 去重后待新建 > 30 询问；否则用该方式的记忆默认值
  async function decideJoin(items, way)
  {
    const p = await previewItems(items);
    if (p.to_create > 30) return askJoinStudy(p.to_create);
    return studyPref(way);
  }

  async function decideJoinByList(listId, way)
  {
    const r = await api('/api/wordlists/' + listId + '/expand', { method: 'POST' });
    if (r.error)
    {
      alert(r.error);
      return null;
    }
    const n = r.unique_count || 0;
    if (n > 30) return askJoinStudy(n);
    return studyPref(way);
  }

  async function startStudy(listId)
  {
    const r = await api('/api/wordlists/' + listId + '/start-study', { method: 'POST' });
    if (r.error)
    {
      alert(r.error);
      return false;
    }
    return true;
  }

  // 详情页「开始学习」：显式动作，> 30 先询问，否则直接加入
  async function runStudyFlow(listId)
  {
    const r = await api('/api/wordlists/' + listId + '/expand', { method: 'POST' });
    if (r.error) return alert(r.error);
    const n = r.unique_count || 0;
    let join = true;
    if (n > 30)
    {
      join = await askJoinStudy(n);
      if (join === null) return;
    }
    if (!join || !n) return;
    const s = await api('/api/wordlists/' + listId + '/start-study', { method: 'POST' });
    if (s.error) return alert(s.error);
    alert('已加入学习：新建 ' + (s.kp_created || 0) + ' 个单词，已建学情 ' + (s.learning_created || 0) + ' 个');
    await reloadAll();
  }

  // 复制共用单词本到私人：先选「加入学习」；> 30 改为询问
  function openCopy(sourceId)
  {
    state.copySourceId = sourceId;
    syncSwitch('wl-copy-study', 'copy');
    openDrawer('modal-wl-copy');
  }

  async function doCopy()
  {
    const join = await decideJoinByList(state.copySourceId, 'copy');
    if (join === null) return;
    const r = await api('/api/wordlists/' + state.copySourceId + '/copy', { method: 'POST' });
    if (r.error) return alert(r.error);
    if (join) await startStudy(r.id);
    closeDrawer('modal-wl-copy');
    await reloadAll();
    openWordlist(r.id);
  }

  // ---------- 数据加载 ----------

  async function reloadAll()
  {
    const [b, u, r, w, m] = await Promise.all([
      api('/api/wordlists?source=builtin'),
      api('/api/wordlists?source=user'),
      api('/api/wordlists?source=range'),
      api('/api/wordbook'),
      api('/api/wordlists/my-words?size=200&sort=time&order=desc')
    ]);
    state.builtin = b.data || [];
    state.user = u.data || [];
    state.range = r.data || [];
    state.wordbooks = w.data || [];
    state.myWords = m.data || [];
    state.myWordsTotal = m.total || 0;

    $('cnt-builtin').textContent = state.builtin.length;
    $('cnt-list').textContent = state.user.length + state.range.length;
    $('cnt-wordbook').textContent = state.wordbooks.length;
    $('cnt-mywords').textContent = state.myWordsTotal;
    $('grp-cnt-list').textContent = state.user.length + state.range.length;
    $('grp-cnt-wordbook').textContent = state.wordbooks.length;
    $('tab-cnt-shared').textContent = state.builtin.length;
    $('tab-cnt-private').textContent = state.user.length + state.range.length + state.wordbooks.length;

    renderShared();
    renderPrivate();
    renderMyWords();
  }

  // ---------- 统一卡片 ----------

  function sharedTag(list)
  {
    if (list.textbook) return '教材';
    if (list.source === 'community') return '社区提供';
    return '官方';
  }

  const ICON_VIEW = '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>';
  const ICON_COPY = '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><rect x="9" y="9" width="13" height="13" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>';
  const ICON_DEL = '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M3 6h18"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>';

  function copyBtn(id) { return `<button class="book-card__act" type="button" title="复制到私人单词本" data-wl-copy="${id}">${ICON_COPY}</button>`; }
  function delWlBtn(id, kind) { return `<button class="book-card__act" type="button" title="删除" data-wl-del="${id}" data-kind="${kind}">${ICON_DEL}</button>`; }
  function delWbBtn(id) { return `<button class="book-card__act" type="button" title="删除" data-wb-del="${id}">${ICON_DEL}</button>`; }

  function bookCard(cfg)
  {
    const isWb = cfg.kind === 'wordbook';
    const openAttr = isWb ? `data-wb-open="${cfg.raw.id}"` : `data-wl-open="${cfg.raw.id}"`;
    const title = isWb ? cfg.raw.name : cfg.raw.title;
    const count = isWb ? (cfg.raw.entry_count || 0) : (cfg.raw.item_count || 0);
    let meta;
    if (isWb)
      meta = cfg.raw.description || (cfg.raw.is_public
        ? '公开' + (cfg.raw.share_code ? ' · 码 ' + cfg.raw.share_code : '')
        : '私有');
    else
      meta = [cfg.raw.textbook, cfg.raw.volume, cfg.raw.unit].filter(Boolean).join(' · ');
    const tag = cfg.shared ? `<span class="book-card__tag">${sharedTag(cfg.raw)}</span>` : '';
    return `
      <article class="book-card" data-group="${cfg.group}" ${openAttr}>
        <div class="book-card__top">
          ${tag}
          <span class="book-card__count">${count} 词</span>
        </div>
        <h3 class="book-card__title">${esc(title)}</h3>
        <p class="book-card__meta">${esc(meta || (cfg.shared ? '官方词表' : '私人单词本'))}</p>
        <div class="book-card__acts">
          <button class="book-card__act" type="button" title="打开" ${openAttr}>${ICON_VIEW}</button>
          ${cfg.actions || ''}
        </div>
      </article>`;
  }

  function tabKey()
  {
    return 'wordlibTab:' + ((state.me && state.me.user_id) || 0);
  }

  function setTab(tab)
  {
    state.ui.tab = tab === 'private' ? 'private' : 'shared';
    document.querySelectorAll('[data-lib-tab]').forEach(b =>
    {
      const on = b.dataset.libTab === state.ui.tab;
      b.classList.toggle('is-active', on);
      b.setAttribute('aria-selected', on ? 'true' : 'false');
    });
    document.querySelectorAll('[data-lib-panel]').forEach(p =>
    {
      p.hidden = p.dataset.libPanel !== state.ui.tab;
    });
    try { localStorage.setItem(tabKey(), state.ui.tab); } catch (e) {}
  }

  function initialTab()
  {
    let saved = null;
    try { saved = localStorage.getItem(tabKey()); } catch (e) {}
    if (saved === 'shared' || saved === 'private') return saved;
    return state.me ? 'private' : 'shared';
  }

  function sharedMatch(list)
  {
    const f = state.ui.sharedFilter;
    if (f === 'textbook') return !!list.textbook;
    if (f === 'community') return list.source === 'community';
    if (f === 'official') return !list.textbook && list.source !== 'community';
    return true;
  }

  function currentOpenVols()
  {
    const arr = [];
    document.querySelectorAll('#grid-shared [data-vol]').forEach(btn =>
    {
      if (btn.getAttribute('aria-expanded') === 'true') arr.push(parseInt(btn.dataset.vol));
    });
    return arr;
  }

  function renderShared()
  {
    const el = $('grid-shared');
    if (!state.builtin.length)
    {
      el.innerHTML = '<p class="state-line">共用单词本还没加载出来。请确认后端服务已启动。</p>';
      return;
    }
    const list = state.builtin.filter(sharedMatch);
    if (!list.length)
    {
      el.innerHTML = '<p class="state-line">这个分类还没有词表。</p>' +
        '<p class="u-mt-md"><button class="btn btn-secondary u-btn-sm" type="button" data-empty-act="shared-all">查看全部</button></p>';
      return;
    }
    const volumes = [];
    const byVol = {};
    list.forEach(l =>
    {
      const vol = l.volume || '未分册';
      if (!byVol[vol]) { byVol[vol] = []; volumes.push(vol); }
      byVol[vol].push(l);
    });
    const allOpen = state.ui.openVols === 'all';
    el.innerHTML = volumes.map((vol, idx) =>
    {
      const open = allOpen || (Array.isArray(state.ui.openVols) && state.ui.openVols.includes(idx));
      return `
      <div class="lib-vol">
        <button class="lib-vol__head" type="button" data-vol="${idx}" aria-expanded="${open}">
          <span class="lib-vol__name">${esc(vol)}</span>
          <span class="lib-vol__cnt">${byVol[vol].length} 个单元</span>
          <svg class="lib-vol__chev" viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><path d="m9 18 6-6-6-6"/></svg>
        </button>
        <div class="book-grid" ${open ? '' : 'hidden'}>${byVol[vol].map(l => bookCard({
          raw: l, kind: 'wordlist', group: 'builtin', shared: true,
          actions: copyBtn(l.id)
        })).join('')}</div>
      </div>`;
    }).join('');
    el.querySelectorAll('[data-vol]').forEach(btn =>
    {
      btn.addEventListener('click', () => toggleVol(parseInt(btn.dataset.vol)));
    });
    $('btn-shared-expand').textContent = allOpen ? '收起全部' : '展开全部';
  }

  function toggleVol(idx)
  {
    const open = currentOpenVols();
    state.ui.openVols = open.includes(idx) ? open.filter(i => i !== idx) : open.concat(idx);
    renderShared();
  }

  // 单词本与合并本同属「我的单词本」，卡片外观不做区分
  function privateGroups()
  {
    return [
      {
        key: 'list', filter: 'list', el: 'grid-user',
        items: state.user.concat(state.range),
        cfg: l => ({ raw: l, kind: 'wordlist', group: 'user', actions: delWlBtn(l.id, l.kind === 'combo' ? 'range' : 'user') })
      },
      {
        key: 'wordbook', filter: 'wordbook', el: 'grid-wordbook', items: state.wordbooks,
        cfg: w => ({ raw: w, kind: 'wordbook', group: 'wordbook', actions: delWbBtn(w.id) })
      }
    ];
  }

  function emptyPrivate(key)
  {
    if (key === 'list')
    {
      return '<p class="state-line">还没有词表。新建一本，或粘贴导入。</p>' +
        '<p class="u-mt-md"><button class="btn btn-secondary u-btn-sm" type="button" data-empty-act="wl">新建词表</button></p>';
    }
    return '<p class="state-line">还没有生词本。新建一本，或把「待整理生词」收进来。</p>' +
      '<p class="u-mt-md"><button class="btn btn-secondary u-btn-sm" type="button" data-empty-act="wb">新建生词本</button></p>';
  }

  function renderPrivate()
  {
    const f = state.ui.privateFilter;
    privateGroups().forEach(g =>
    {
      const box = document.querySelector('[data-private-group="' + g.key + '"]');
      if (box) box.hidden = !(f === 'all' || f === g.filter);
      const all = g.items;
      const shown = state.ui.more[g.key] ? all : all.slice(0, state.ui.limit[g.key]);
      $(g.el).innerHTML = shown.length
        ? shown.map(x => bookCard(g.cfg(x))).join('')
        : emptyPrivate(g.key);
      const more = document.querySelector('[data-more="' + g.key + '"]');
      if (more)
      {
        const rest = all.length - shown.length;
        more.hidden = rest <= 0;
        more.textContent = '显示更多（还有 ' + rest + ' 项）';
      }
    });
  }

  // v1.5.3-T003：词条朗读按钮（恢复 v1.2.0-T006 播放复习入口）
  function speakBtn(word)
  {
    return '<button class="speak-btn" type="button" data-tts="' + word + '" title="朗读">' +
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="18" height="18">' +
      '<polygon points="11 5 6 9 2 9 2 15 6 19 11 5"/>' +
      '<path d="M15.54 8.46a5 5 0 0 1 0 7.07"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14"/></svg></button>';
  }

  function renderMyWords()
  {
    $('cnt-mywords').textContent = state.myWordsTotal;
    $('mywords-total').textContent = state.myWordsTotal;
    const el = $('mywords-list');
    if (!state.myWords.length)
    {
      el.innerHTML = '<p class="state-line">待整理生词都已放进生词本。</p>';
      return;
    }
    el.innerHTML = state.myWords.map(item =>
    {
      const word = esc(item.word);
      const wordHtml = item.knowledge_id
        ? `<a class="wb-entry__word" href="/pages/kp-detail.html?id=${item.knowledge_id}">${word}</a>`
        : `<span class="wb-entry__word">${word}</span>`;
      return `
        <div class="wb-entry">
          <div class="wb-entry__head">${wordHtml}${speakBtn(word)}</div>
          <div class="wb-entry__badges">
            <span class="pill pill--ai">${item.is_solidified === 1 ? '已固化' : '待固化'}</span>
            <span class="pill pill--teal">熟练度 L${item.proficiency_level || 0}</span>
            ${item.meaning ? `<span class="pill">${esc(item.meaning)}</span>` : ''}
            <span class="pill">${esc(item.create_time || '')}</span>
          </div>
        </div>`;
    }).join('');
  }

  async function reloadMyWords(sort, order, q)
  {
    const params = new URLSearchParams({ size: '200', sort: sort || 'time', order: order || 'desc' });
    if (q) params.set('q', q);
    const r = await api('/api/wordlists/my-words?' + params.toString());
    state.myWords = r.data || [];
    state.myWordsTotal = r.total || 0;
    renderMyWords();
  }

  // ---------- 列表点击 ----------

  async function onListClick(e)
  {
    const copy = e.target.closest('[data-wl-copy]');
    const del = e.target.closest('[data-wl-del]');
    const wbDel = e.target.closest('[data-wb-del]');
    const open = e.target.closest('[data-wl-open]');
    const wbOpen = e.target.closest('[data-wb-open]');

    if (copy)
    {
      if (!(await ensureLogin())) return;
      openCopy(parseInt(copy.dataset.wlCopy));
      return;
    }
    if (del)
    {
      if (!confirm('确定删除这本单词本？其中的单词会一并删除，已关联的单词不受影响。')) return;
      const r = await api('/api/wordlists/' + del.dataset.wlDel, { method: 'DELETE' });
      if (r.error) return alert(r.error);
      await reloadAll();
      return;
    }
    if (wbDel)
    {
      if (!confirm('确定删除这本生词本？其中的单词会一并移除，原单词不会被删除。')) return;
      const r = await api('/api/wordbook/' + wbDel.dataset.wbDel, { method: 'DELETE' });
      if (r.error) return alert(r.error);
      await reloadAll();
      return;
    }
    if (open) return openWordlist(parseInt(open.dataset.wlOpen));
    if (wbOpen) return openWordbook(parseInt(wbOpen.dataset.wbOpen));
  }

  $('lib-body').addEventListener('click', onListClick);
  $('lib-search-result').addEventListener('click', onListClick);

  // v1.5.3-T003：词条朗读（委托，覆盖词表/生词本/待整理三处列表）
  document.addEventListener('click', e =>
  {
    const b = e.target.closest('.speak-btn[data-tts]');
    if (!b) return;
    e.preventDefault();
    if (window.TTS && typeof window.TTS.speak === 'function') window.TTS.speak(b.dataset.tts);
  });

  // ---------- 私人单词本工具条 ----------

  $('btn-new-book').addEventListener('click', async () =>
  {
    if (await ensureLogin()) openDrawer('modal-new-book');
  });

  document.querySelectorAll('[data-new-book]').forEach(btn =>
  {
    btn.addEventListener('click', async () =>
    {
      const what = btn.dataset.newBook;
      closeDrawer('modal-new-book');
      if (what === 'wl') openWlCreate();
      else if (what === 'ai') openAi();
      else if (what === 'combo') openRange();
      else if (what === 'wb') openWbCreate();
      else if (what === 'wb-import') openWbImport();
    });
  });

  // v1.5.3-T003：空态动作按钮（动态插入，统一委托）
  document.addEventListener('click', async e =>
  {
    const btn = e.target.closest('[data-empty-act]');
    if (!btn) return;
    const act = btn.dataset.emptyAct;
    if (act === 'shared-all')
    {
      const all = document.querySelector('[data-shared-filter="all"]');
      if (all) all.click();
    }
    else if (act === 'wl')
    {
      if (await ensureLogin()) openWlCreate();
    }
    else if (act === 'wb')
    {
      if (await ensureLogin()) openWbCreate();
    }
    else if (act === 'combo-manage')
    {
      const m = document.querySelector('#detail-actions [data-act-combo-manage]');
      if (m) m.click();
    }
  });

  bindSwitch('wl-create-study', 'bulk');
  bindSwitch('wl-add-study', 'bulk');
  bindSwitch('ai-study', 'bulk');
  bindSwitch('wl-copy-study', 'copy');
  bindSwitch('wb-add-study', 'manual');
  $('act-wl-copy').addEventListener('click', doCopy);

  // ---------- 待整理生词抽屉 ----------

  function myWordsQuery()
  {
    const [sort, order] = $('mywords-sort').value.split('|');
    return { sort, order, q: $('mywords-q').value.trim() };
  }

  $('btn-mywords-fab').addEventListener('click', async () =>
  {
    if (fabSuppressClick) return;
    const p = myWordsQuery();
    await reloadMyWords(p.sort, p.order, p.q);
    openDrawer('drawer-mywords');
  });

  $('mywords-sort').addEventListener('change', () =>
  {
    const p = myWordsQuery();
    reloadMyWords(p.sort, p.order, p.q);
  });

  let myWordsTimer = null;
  $('mywords-q').addEventListener('input', () =>
  {
    clearTimeout(myWordsTimer);
    myWordsTimer = setTimeout(() =>
    {
      const p = myWordsQuery();
      reloadMyWords(p.sort, p.order, p.q);
    }, 300);
  });

  $('btn-mywords-import').addEventListener('click', async () =>
  {
    if (await ensureLogin()) openMyWordsImport();
  });

  // ---------- 全局搜索（教材 / 单词本名称 + 单词） ----------

  function matchList(list, q)
  {
    const hay = [list.title, list.name, list.textbook, list.volume, list.unit, list.description]
      .filter(Boolean).join(' ').toLowerCase();
    return hay.includes(q);
  }

  function localHits(q)
  {
    const hits = [];
    state.builtin.forEach(l =>
    {
      if (!matchList(l, q)) return;
      hits.push({ kind: 'wl', id: l.id, title: l.title, count: l.item_count || 0,
        meta: [l.textbook, l.volume, l.unit].filter(Boolean).join(' · ') || '共用单词本' });
    });
    state.user.forEach(l =>
    {
      if (!matchList(l, q)) return;
      hits.push({ kind: 'wl', id: l.id, title: l.title, meta: '私人单词本', count: l.item_count || 0 });
    });
    state.range.forEach(l =>
    {
      if (!matchList(l, q)) return;
      hits.push({ kind: 'wl', id: l.id, title: l.title, meta: '私人单词本', count: l.item_count || 0 });
    });
    state.wordbooks.forEach(w =>
    {
      if (!matchList(w, q)) return;
      hits.push({ kind: 'wb', id: w.id, title: w.name, meta: '生词本', count: w.entry_count || 0 });
    });
    return hits.slice(0, 30);
  }

  function hitHtml(h)
  {
    const attr = h.kind === 'wb' ? `data-wb-open="${h.id}"` : `data-wl-open="${h.id}"`;
    return `
      <button class="lib-hit" type="button" ${attr}>
        <span class="lib-hit__title">${esc(h.title)}</span>
        <span class="lib-hit__meta">${esc(h.meta)} · ${h.count} 词</span>
      </button>`;
  }

  let searchTimer = null;
  $('lib-search').addEventListener('input', () =>
  {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(async () =>
    {
      const q = $('lib-search').value.trim();
      const box = $('lib-search-result');
      if (!q)
      {
        box.hidden = true;
        box.innerHTML = '';
        $('lib-body').hidden = false;
        return;
      }
      const lq = q.toLowerCase();
      const hits = localHits(lq);
      const r = await api('/api/wordlists/search?q=' + encodeURIComponent(q) + '&limit=50');
      const rows = r.data || [];
      $('lib-body').hidden = true;
      box.hidden = false;
      box.innerHTML = `
        <div class="lib-sec__head">
          <h2 class="lib-sec__title">单词本</h2>
          <span class="lib-sec__cnt">${hits.length} 个</span>
        </div>
        ${hits.length ? `<div class="lib-hit-list">${hits.map(hitHtml).join('')}</div>`
          : '<p class="state-line">没有名称匹配的教材或单词本。</p>'}
        <div class="lib-sec__head">
          <h2 class="lib-sec__title">单词</h2>
          <span class="lib-sec__cnt">${rows.length} 条</span>
        </div>
        ${rows.length ? rows.map(row => `
          <div class="wb-entry">
            <div class="wb-entry__head">
              <a class="wb-entry__word" href="javascript:void(0)" data-wl-open="${row.wordlist_id}">${esc(row.word)}</a>
            </div>
            <div class="wb-entry__badges">
              <span class="pill pill--ai">${esc(row.title)}</span>
              ${row.volume ? `<span class="pill">${esc(row.volume)} ${esc(row.unit || '')}</span>` : ''}
              ${row.meaning ? `<span class="pill">${esc(row.meaning)}</span>` : '<span class="pill pill--muted">释义待补充</span>'}
            </div>
          </div>`).join('') : '<p class="state-line">没有匹配单词。</p>'}`;
    }, 300);
  });

  // ---------- 详情 ----------

  $('btn-back').addEventListener('click', () =>
  {
    // 层级下钻后先回上一级，回到顶层再退出详情
    if (state.trail.length > 1)
    {
      const i = state.trail.length - 2;
      openWordlist(state.trail[i].id, { crumbIndex: i });
      return;
    }
    $('view-list').hidden = false;
    $('view-detail').hidden = true;
    state.detail = { kind: null, id: 0, data: null, items: [] };
    state.trail = [];
    $('detail-crumbs').innerHTML = '';
  });

  $('detail-filter').addEventListener('input', () =>
  {
    if (state.detail.kind === 'combo') renderComboMembers();
    else if (state.detail.kind === 'wordlist') renderWlItems();
    else renderWbEntries();
  });

  // ---------- 面包屑 ----------

  function renderCrumbs()
  {
    const parts = ['<button class="crumbs__item" type="button" data-crumb-root>单词本</button>'];
    state.trail.forEach((t, i) =>
    {
      const last = i === state.trail.length - 1;
      parts.push('<span class="crumbs__sep">/</span>');
      parts.push(`<button class="crumbs__item${last ? ' is-current' : ''}" type="button" data-crumb="${i}"${last ? ' aria-current="page"' : ''}>${esc(t.title || '未命名')}</button>`);
    });
    $('detail-crumbs').innerHTML = parts.join('');
  }

  $('detail-crumbs').addEventListener('click', e =>
  {
    if (e.target.closest('[data-crumb-root]')) return $('btn-back').click();
    const btn = e.target.closest('[data-crumb]');
    if (!btn) return;
    const i = parseInt(btn.dataset.crumb);
    if (i === state.trail.length - 1) return;
    openWordlist(state.trail[i].id, { crumbIndex: i });
  });

  function enterDetail(nav)
  {
    nav = nav || {};
    if (nav.crumbIndex != null) state.trail = state.trail.slice(0, nav.crumbIndex + 1);
    else if (nav.drill)
    {
      const last = state.trail[state.trail.length - 1];
      if (!last || last.id !== nav.id) state.trail.push({ id: nav.id, title: nav.title });
    }
    else state.trail = [{ id: nav.id, title: nav.title }];
    $('view-list').hidden = true;
    $('view-detail').hidden = false;
    $('detail-filter').value = '';
    $('btn-back-label').textContent = state.trail.length > 1 ? '返回上一级' : '返回列表';
    renderCrumbs();
  }

  async function openWordlist(id, nav)
  {
    const d = await api('/api/wordlists/' + id);
    if (d.error) return alert(d.error);

    if (d.kind === 'combo')
    {
      const r = await api('/api/wordlists/' + id + '/children');
      if (r.error) return alert(r.error);
      state.detail = { kind: 'combo', id, data: d, items: r.data || [] };
      enterDetail({ id, title: d.title, crumbIndex: nav && nav.crumbIndex, drill: !!(nav && nav.drill) });
      $('detail-title').textContent = d.title;
      $('detail-desc').textContent = (d.item_count || 0) + ' 个词 · ' + (r.data || []).length + ' 个成员';
      $('detail-count').textContent = (r.data || []).length + ' 个成员';
      $('detail-actions').innerHTML = `
        <button class="btn btn-primary" type="button" data-act-study>开始学习</button>
        <button class="btn btn-secondary" type="button" data-act-practice>综合练习</button>
        <button class="btn btn-secondary" type="button" data-act-combo-manage>管理成员</button>
        <button class="btn btn-secondary" type="button" data-act-wl-import>整本加入</button>
        <button class="btn btn-secondary" type="button" data-act-wl-edit>重命名</button>
        <button class="btn btn-secondary" type="button" data-act-wl-del>删除</button>`;
      renderComboMembers();
      return;
    }

    const items = await api('/api/wordlists/' + id + '/items?size=200');
    const kids = d.children || [];
    state.detail = { kind: 'wordlist', id, data: d, items: items.data || [], children: kids };
    enterDetail({ id, title: d.title, crumbIndex: nav && nav.crumbIndex, drill: !!(nav && nav.drill) });
    $('detail-title').textContent = d.title;
    const meta = [d.textbook, d.volume, d.unit].filter(Boolean).join(' · ');
    $('detail-desc').textContent = (meta ? meta + ' · ' : '') + (d.item_count || 0) + ' 个词'
      + (kids.length ? ' · ' + kids.length + ' 个包含的单词本' : '');
    $('detail-count').textContent = (d.item_count || 0) + ' 个词';

    const isUser = d.source === 'user';
    $('detail-actions').innerHTML = `
      <button class="btn btn-primary" type="button" data-act-study>开始学习</button>
      <button class="btn btn-secondary" type="button" data-act-practice>综合练习</button>
      ${isUser ? '<button class="btn btn-secondary" type="button" data-act-wl-import>整本加入</button>' : ''}
      ${kids.length ? '<button class="btn btn-secondary" type="button" data-act-combo-manage>管理成员</button>' : ''}
      <button class="btn btn-secondary" type="button" data-act-copy>复制到自建词表</button>
      ${isUser ? '<button class="btn btn-secondary" type="button" data-act-wl-add>+ 添加单词</button>' : ''}
      ${isUser ? '<button class="btn btn-secondary" type="button" data-act-wl-edit>重命名</button>' : ''}
      ${isUser ? '<button class="btn btn-secondary" type="button" data-act-wl-del>删除</button>' : ''}`;
    renderWlItems();
  }

  function memberRowsHtml(members)
  {
    return '<div class="member-list">' + members.map(m =>
    {
      if (m.child_type === 'list')
      {
        const hint = m.kind === 'combo' ? '合并本'
          : (m.source === 'builtin' ? '共用单词本' : '单词本');
        return `
          <div class="member-row">
            <button class="member-row__main" type="button" data-wl-open="${m.child_id}">
              <span class="member-row__title">${esc(m.title || '未命名')}</span>
              <span class="member-row__meta">${hint} · ${m.item_count || 0} 词</span>
            </button>
            <button class="member-row__toggle" type="button" data-member-toggle="${m.child_id}"
                    aria-expanded="false" title="展开查看内部单词">
              <svg class="member-row__chev" viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2"><path d="m9 18 6-6-6-6"/></svg>
            </button>
          </div>
          <div class="member-sub" hidden></div>`;
      }
      const meta = [m.pos, m.meaning].filter(Boolean).join(' · ');
      return `
        <div class="member-row member-row--word">
          <div class="member-row__main">
            <span class="member-row__title">${esc(m.title || '')}</span>
            <span class="member-row__meta">${esc(meta || '单词')}</span>
          </div>
        </div>`;
    }).join('') + '</div>';
  }

  function memberWordsHtml(words)
  {
    return '<div class="member-words">' + words.map(w => `
      <div class="member-word">
        <span class="member-word__w">${esc(w.word)}</span>
        <span class="member-word__m">${esc(w.meaning || '')}</span>
      </div>`).join('') + '</div>';
  }

  // 三角号：就地展开该成员本的内部单词，不跳转
  async function toggleMember(toggleBtn)
  {
    const row = toggleBtn.closest('.member-row');
    const sub = row && row.nextElementSibling;
    if (!sub || !sub.classList.contains('member-sub')) return;
    const isOpen = toggleBtn.getAttribute('aria-expanded') === 'true';
    if (isOpen)
    {
      toggleBtn.setAttribute('aria-expanded', 'false');
      toggleBtn.classList.remove('is-open');
      sub.hidden = true;
      return;
    }
    toggleBtn.setAttribute('aria-expanded', 'true');
    toggleBtn.classList.add('is-open');
    sub.hidden = false;
    if (sub.dataset.loaded === '1') return;

    const id = parseInt(toggleBtn.dataset.memberToggle);
    sub.innerHTML = '<p class="state-line">加载中…</p>';
    const d = await api('/api/wordlists/' + id);
    if (d.error)
    {
      sub.innerHTML = '<p class="state-line">' + esc(d.error) + '</p>';
      return;
    }
    let inner = '';
    const kids = d.children || [];
    if (kids.length) inner += memberRowsHtml(kids);
    if (d.kind !== 'combo')
    {
      const r = await api('/api/wordlists/' + id + '/items?size=200');
      const words = r.data || [];
      if (words.length) inner += memberWordsHtml(words);
    }
    sub.innerHTML = inner || '<p class="state-line">暂无内容。</p>';
    sub.dataset.loaded = '1';
  }

  function renderComboMembers()
  {
    const q = ($('detail-filter').value || '').toLowerCase();
    const members = state.detail.items.filter(m =>
      !q || String(m.title || '').toLowerCase().includes(q));
    const el = $('detail-list');
    if (!members.length)
    {
      el.innerHTML = '<p class="state-line">这本合并本还没有词表。点上方「管理成员」添加。</p>' +
        '<p class="u-mt-md"><button class="btn btn-secondary u-btn-sm" type="button" data-empty-act="combo-manage">管理成员</button></p>';
      return;
    }
    el.innerHTML = memberRowsHtml(members);
  }

  function renderWlItems()
  {
    const q = ($('detail-filter').value || '').toLowerCase();
    const items = state.detail.items.filter(item =>
      !q || (item.word || '').toLowerCase().includes(q) || (item.meaning || '').toLowerCase().includes(q));
    const el = $('detail-list');
    const kids = (state.detail.children || []).filter(m =>
      !q || String(m.title || '').toLowerCase().includes(q));
    const memberHtml = kids.length ? `
      <div class="lib-sec__head">
        <h2 class="lib-sec__title">包含的单词本</h2>
        <span class="lib-sec__cnt">${kids.length} 个</span>
      </div>
      ${memberRowsHtml(kids)}` : '';
    if (!items.length && !kids.length)
    {
      el.innerHTML = '<p class="state-line">还没有单词。点击「添加单词」或「整本加入」开始。</p>';
      return;
    }
    const isTextbook = !!state.detail.data.textbook;
    const entriesHtml = items.map(item =>
    {
      const word = esc(item.word);
      const wordHtml = item.knowledge_id
        ? `<a class="wb-entry__word" href="/pages/kp-detail.html?id=${item.knowledge_id}">${word}</a>`
        : `<span class="wb-entry__word">${word}</span>`;
      const remove = state.detail.data.source === 'user'
        ? `<button class="wb-entry__remove" data-item-remove="${item.item_id}" title="移除">×</button>` : '';
      const status = item.knowledge_id
        ? (item.is_solidified === 1 ? '已固化' : '待固化')
        : (isTextbook ? '教材词' : '未加入学习');
      return `
        <div class="wb-entry">
          <div class="wb-entry__head">${wordHtml}${speakBtn(word)}${remove}</div>
          <div class="wb-entry__badges">
            <span class="pill pill--ai">${status}</span>
            ${item.pos ? `<span class="pill">${esc(item.pos)}</span>` : ''}
            ${item.meaning
              ? `<span class="pill">${esc(item.meaning)}</span>`
              : '<span class="pill pill--muted">释义待补充</span>'}
            ${item.proficiency_level > 0 ? `<span class="pill pill--teal">熟练度 L${item.proficiency_level}</span>` : ''}
          </div>
        </div>`;
    }).join('');
    el.innerHTML = memberHtml
      + (items.length ? `
        ${kids.length ? `<div class="lib-sec__head"><h2 class="lib-sec__title">本书词条</h2><span class="lib-sec__cnt">${items.length} 条</span></div>` : ''}
        ${entriesHtml}` : '');
  }

  async function openWordbook(id)
  {
    const r = await api('/api/wordbook/' + id);
    if (r.error) return alert(r.error);
    state.detail = { kind: 'wordbook', id, data: r, items: r.entries || [] };
    enterDetail({ id, title: r.name });
    $('detail-title').textContent = r.name;
    $('detail-desc').textContent = (r.description || '') + ' · ' + (r.entry_count || 0) + ' 个词';
    $('detail-count').textContent = (r.entries || []).length + ' 个词';
    $('detail-actions').innerHTML = `
      <button class="btn btn-primary" type="button" data-act-wb-add>+ 添加单词</button>
      <button class="btn btn-secondary" type="button" data-act-wb-export-word>导出 Word</button>
      <button class="btn btn-secondary" type="button" data-act-wb-edit>编辑</button>`;
    renderWbEntries();
  }

  function renderWbEntries()
  {
    const q = ($('detail-filter').value || '').toLowerCase();
    const entries = state.detail.items.filter(e =>
      !q || (e.standard_content || '').toLowerCase().includes(q));
    const el = $('detail-list');
    if (!entries.length)
    {
      el.innerHTML = '<p class="state-line">还没有单词。点击「添加单词」开始。</p>';
      return;
    }
    el.innerHTML = entries.map(e =>
    {
      const word = esc(e.standard_content);
      const kpUrl = '/pages/kp-detail.html?id=' + e.knowledge_id;
      return `
        <div class="wb-entry">
          <div class="wb-entry__head">
            <a class="wb-entry__word" href="${kpUrl}">${word}</a>
            ${speakBtn(word)}
            <button class="wb-entry__remove" data-wb-entry="${e.entry_id}" title="移除">×</button>
          </div>
          <div class="wb-entry__badges">
            <span class="pill pill--ai">${e.is_solidified === 1 ? '已固化' : '待固化'}</span>
            ${e.proficiency_level > 0 ? `<span class="pill pill--teal">熟练度 L${e.proficiency_level}</span>` : ''}
            <span class="pill">${esc(e.difficulty ? '难度 ' + e.difficulty : '–')}</span>
            <a class="pill" href="${kpUrl}">查看详情 →</a>
          </div>
        </div>`;
    }).join('');
  }

  // ---------- 综合练习（D11：历史默认 + AI 现场生成） ----------

  const PRACTICE_DIFF = { basic: '基础', advanced: '进阶', challenge: '挑战' };
  const PRACTICE_STYLE = { exam: '应试', situational: '情景', story: '故事', academic: '学术' };
  const PRACTICE_STATUS = { queued: '排队中', running: '生成中', failed: '失败', canceled: '已取消' };
  const practice = { listId: 0, listTitle: '', words: [], lastId: 0, timer: null };

  function practiceMsg(text, danger) { msg('practice-msg', text, danger); }

  function stopPracticePoll()
  {
    if (practice.timer) { clearTimeout(practice.timer); practice.timer = null; }
  }

  function practiceTab(name)
  {
    document.querySelectorAll('#modal-practice [data-practice-tab]').forEach(b =>
      b.classList.toggle('is-active', b.dataset.practiceTab === name));
    document.querySelectorAll('#modal-practice [data-practice-panel]').forEach(p =>
      p.hidden = p.dataset.practicePanel !== name);
    practiceMsg('');
  }

  document.querySelectorAll('#modal-practice [data-practice-tab]').forEach(btn =>
    btn.addEventListener('click', () => practiceTab(btn.dataset.practiceTab)));

  async function loadPracticeWords()
  {
    if (practice.words.length) return practice.words;
    const r = await api('/api/wordlists/' + practice.listId + '/expand', { method: 'POST' });
    if (r.error) { practiceMsg(r.error, true); return []; }
    practice.words = (r.data || []).map(x => x.word).filter(Boolean);
    return practice.words;
  }

  function specOf(item)
  {
    try { return JSON.parse(item.spec_json || '{}'); } catch (e) { return {}; }
  }

  function renderPracticeHistory(items)
  {
    const box = $('practice-history');
    const wordSet = new Set(practice.words.map(w => w.toLowerCase()));
    const rows = [];
    (items || []).forEach(it =>
    {
      const spec = specOf(it);
      const src = spec.source || {};
      const fromThis = Number(src.id || 0) === practice.listId &&
        (src.type === 'wordlist' || src.type === 'range');
      const overlap = (spec.words || []).filter(w => typeof w === 'string' &&
        wordSet.has(w.toLowerCase())).length;
      if (!fromThis && !overlap) return;
      rows.push({ it, fromThis, overlap });
    });
    rows.sort((a, b) => (b.fromThis - a.fromThis) || (b.overlap - a.overlap) || (b.it.id - a.it.id));
    if (!rows.length)
    {
      box.innerHTML = '<p class="state-line">暂无与该词表相关的练习，可切换到「AI 现场生成」。</p>';
      return;
    }
    box.innerHTML = rows.map(r =>
    {
      const it = r.it;
      const done = it.status === 'done';
      const meta = [it.item_count + ' 题', PRACTICE_DIFF[it.difficulty] || it.difficulty,
        PRACTICE_STYLE[it.style] || it.style, it.created_at,
        done ? '' : (PRACTICE_STATUS[it.status] || it.status)].filter(Boolean).join(' · ');
      return `
        <div class="ex-history__row">
          <span class="ex-history__word">#${it.id}${r.fromThis ? ' · 本词表' : ''}</span>
          <span class="state-line">${esc(meta)}</span>
          ${done ? `<button class="btn btn-secondary u-btn-sm" type="button" data-practice-export="${it.id}" data-practice-count="${it.item_count || 0}">导出 Word</button>` : ''}
        </div>`;
    }).join('');
  }

  async function loadPracticeHistory()
  {
    const box = $('practice-history');
    box.innerHTML = '<p class="state-line">加载中…</p>';
    const words = await loadPracticeWords();
    if (!words.length) { box.innerHTML = '<p class="state-line">当前词表暂无可练习的词。</p>'; return; }
    const r = await api('/api/custom-exercise/history?limit=30');
    if (r.error) { box.innerHTML = '<p class="state-line">' + esc(r.error) + '</p>'; return; }
    renderPracticeHistory(r.items || []);
  }

  function practiceQueueLine(d)
  {
    let line = d.position > 0 ? '排队中 · 前方 ' + d.position + ' 个任务' : 'AI 出题中';
    if (d.eta_sec > 0)
      line += ' · 预计 ' + (d.eta_sec < 60 ? d.eta_sec + ' 秒' : Math.ceil(d.eta_sec / 60) + ' 分钟');
    return line + '…';
  }

  function showPracticeExport(id)
  {
    practice.lastId = id;
    const btn = $('act-practice-export');
    btn.hidden = false;
    btn.dataset.id = String(id);
  }

  function pollPractice(id)
  {
    stopPracticePoll();
    practice.lastId = id;
    (async function tick()
    {
      const d = await api('/api/custom-exercise/' + id);
      if (!d || d.error) { practiceMsg(d && d.error ? d.error : '查询失败', true); return; }
      if (d.status === 'queued' || d.status === 'running')
      {
        practiceMsg(practiceQueueLine(d));
        if ($('modal-practice').classList.contains('open')) practice.timer = setTimeout(tick, 1500);
        return;
      }
      if (d.status === 'done')
      {
        practice.lastCount = d.item_count || 0;
        practiceMsg('练习已生成（' + practice.lastCount + ' 题），可导出 Word。');
        showPracticeExport(id);
        loadPracticeHistory();
      }
      else
      {
        practiceMsg('生成失败，可重试。', true);
        loadPracticeHistory();
      }
    })();
  }

  async function generatePractice()
  {
    const words = await loadPracticeWords();
    if (!words.length) return;
    const btn = $('act-practice-generate');
    btn.disabled = true;
    practiceMsg('提交组题…');
    const r = await api('/api/custom-exercise', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        source: { type: 'wordlist', id: practice.listId },
        words: words.slice(0, 40),
        difficulty: 'basic',
        style: 'exam',
        counts: {
          word_match: 1, cloze_choice: 1, word_form_fill: 1,
          context_blank_fill: 1, cn_to_en_translation: 1, sentence_imitation: 1
        },
        extra_requirements: '',
        queue_level: 0
      })
    });
    btn.disabled = false;
    if (r.error || !r.id) { practiceMsg(r.error || '提交失败', true); return; }
    practiceMsg(r.reused ? '命中 24 小时内同参练习，直接复用。' : '已提交，AI 出题中…');
    pollPractice(r.id);
  }

  async function exportPractice(id, count)
  {
    const n = Number(count || 0);
    const est = ExportEstimator.paper(n * 90, n);
    if (!await ExportEstimator.confirm({ title: '导出练习卷', pages: est.pages, chars: est.chars })) return;
    const btn = $('act-practice-export');
    btn.disabled = true;
    practiceMsg('正在生成 Word…');
    try
    {
      const r = await api('/api/custom-exercise/' + id + '/export-word', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}'
      });
      if (!r.ok || !r.word_path)
      {
        practiceMsg(r.error || '导出服务暂不可用，可重试。', true);
        return;
      }
      practiceMsg('');
      window.open('/api/word/download?path=' + encodeURIComponent(r.word_path), '_blank');
    }
    finally { btn.disabled = false; }
  }

  async function openPractice(detail)
  {
    practice.listId = detail.id;
    practice.listTitle = (detail.data && (detail.data.title || detail.data.name)) || '';
    practice.words = [];
    practice.lastId = 0;
    $('practice-title').textContent = '综合练习 · ' + practice.listTitle;
    $('practice-sub').textContent = '当前词表：' + practice.listTitle;
    $('act-practice-export').hidden = true;
    practiceTab('history');
    openDrawer('modal-practice');
    await loadPracticeHistory();
  }

  $('act-practice-generate').addEventListener('click', generatePractice);
  $('act-practice-export').addEventListener('click', () =>
  {
    const id = Number($('act-practice-export').dataset.id || 0);
    if (id > 0) exportPractice(id, practice.lastCount);
  });
  $('practice-history').addEventListener('click', e =>
  {
    const btn = e.target.closest('[data-practice-export]');
    if (btn) exportPractice(Number(btn.dataset.practiceExport), Number(btn.dataset.practiceCount || 0));
  });
  $('modal-practice').addEventListener('click', e =>
  {
    if (e.target === $('modal-practice') || e.target.closest('[data-close]')) stopPracticePoll();
  });
  document.addEventListener('keydown', e =>
  {
    if (e.key === 'Escape') stopPracticePoll();
  });

  $('detail-actions').addEventListener('click', async e =>
  {
    const d = state.detail;
    if (e.target.closest('[data-act-study]'))
    {
      if (!(await ensureLogin())) return;
      await runStudyFlow(d.id);
    }
    else if (e.target.closest('[data-act-practice]'))
    {
      if (!(await ensureLogin())) return;
      await openPractice(d);
    }
    else if (e.target.closest('[data-act-copy]'))
    {
      if (!(await ensureLogin())) return;
      openCopy(d.id);
    }
    else if (e.target.closest('[data-act-wl-import]'))
    {
      if (!(await ensureLogin())) return;
      openWlImport(d.id);
    }
    else if (e.target.closest('[data-act-combo-manage]'))
    {
      if (!(await ensureLogin())) return;
      openComboManage(d.id);
    }
    else if (e.target.closest('[data-act-wl-add]'))
    {
      openWlAdd(d.id);
    }
    else if (e.target.closest('[data-act-wl-edit]'))
    {
      openWlCreate(d.id);
    }
    else if (e.target.closest('[data-act-wl-del]'))
    {
      if (!confirm('确定删除这本单词本？其中的单词会一并删除，已关联的单词不受影响。')) return;
      const r = await api('/api/wordlists/' + d.id, { method: 'DELETE' });
      if (r.error) return alert(r.error);
      await reloadAll();
      $('btn-back').click();
    }
    else if (e.target.closest('[data-act-wb-add]'))
    {
      openWbAdd();
    }
    else if (e.target.closest('[data-act-wb-edit]'))
    {
      openWbEdit();
    }
    else if (e.target.closest('[data-act-wb-export-word]'))
    {
      const items = (state.detail.items || []).filter(x => x.solidified || x.is_solidified);
      const words = items.length ? items : (state.detail.items || []);
      if (!words.length) return msg('detail-msg', '暂无可导出内容', true);
      const chars = words.reduce((n, x) =>
        n + String(x.word || '').length + String(x.meaning || '').length, 0);
      const est = ExportEstimator.cards(words.length, chars);
      if (!await ExportEstimator.confirm({ title: '导出单词卡', pages: est.pages, chars: est.chars })) return;
      const exportBtn = e.target.closest('[data-act-wb-export-word]');
      exportBtn.disabled = true;
      try
      {
        const r = await api('/api/wordbook/' + d.id + '/export-word', { method: 'POST' });
        if (!r.word_path) return msg('detail-msg', r.error || '导出服务暂不可用，请重试', true);
        window.open('/api/word/download?path=' + encodeURIComponent(r.word_path), '_blank');
      }
      finally { exportBtn.disabled = false; }
    }
  });

  $('detail-list').addEventListener('click', async e =>
  {
    const toggle = e.target.closest('[data-member-toggle]');
    if (toggle)
    {
      await toggleMember(toggle);
      return;
    }
    const drill = e.target.closest('[data-wl-open]');
    if (drill)
    {
      await openWordlist(parseInt(drill.dataset.wlOpen), { drill: true });
      return;
    }
    const itemBtn = e.target.closest('[data-item-remove]');
    const wbBtn = e.target.closest('[data-wb-entry]');
    if (itemBtn)
    {
      if (!confirm('从这张词表里移除这个单词？')) return;
      const r = await api('/api/wordlists/' + state.detail.id + '/items/' + itemBtn.dataset.itemRemove, { method: 'DELETE' });
      if (r.error) return alert(r.error);
      state.detail.items = state.detail.items.filter(x => x.item_id !== parseInt(itemBtn.dataset.itemRemove));
      state.detail.data.item_count = state.detail.items.length;
      $('detail-count').textContent = state.detail.items.length + ' 个词';
      renderWlItems();
    }
    if (wbBtn)
    {
      if (!confirm('从生词本里移除这个单词？（原单词不会被删除）')) return;
      const r = await api('/api/wordbook/entries/' + wbBtn.dataset.wbEntry, { method: 'DELETE' });
      if (r.error) return alert(r.error);
      state.detail.items = state.detail.items.filter(x => x.entry_id !== parseInt(wbBtn.dataset.wbEntry));
      state.detail.data.entry_count = state.detail.items.length;
      $('detail-count').textContent = state.detail.items.length + ' 个词';
      renderWbEntries();
    }
  });

  // ---------- 创建 / 重命名词表 ----------

  function openWlCreate(editId)
  {
    state.pendingItems = [];
    $('wl-create-title').textContent = editId ? '重命名词表' : '新建词表';
    $('wl-create-words').hidden = !!editId;
    $('wl-name').value = '';
    $('wl-desc').value = '';
    $('wl-text').value = '';
    $('wl-file').value = '';
    msg('wl-msg', '');
    if (editId)
    {
      $('wl-name').value = state.detail.data.title || '';
      $('wl-desc').value = state.detail.data.description || '';
      $('act-wl-create').dataset.editId = editId;
    }
    else
    {
      delete $('act-wl-create').dataset.editId;
      syncSwitch('wl-create-study', 'bulk');
    }
    openDrawer('modal-wl-create');
  }

  $('wl-file').addEventListener('change', async e =>
  {
    const file = e.target.files[0];
    if (!file) return;
    try
    {
      state.pendingItems = await parseFile(file);
      msg('wl-msg', '已从文件读取 ' + state.pendingItems.length + ' 个词');
    }
    catch (err)
    {
      state.pendingItems = [];
      msg('wl-msg', '文件解析失败：' + err.message, true);
    }
  });

  $('act-wl-create').addEventListener('click', async () =>
  {
    const editId = parseInt($('act-wl-create').dataset.editId || '0');
    const title = $('wl-name').value.trim();
    if (!title) return msg('wl-msg', '请输入词表名称', true);
    const description = $('wl-desc').value.trim();

    if (editId)
    {
      const r = await api('/api/wordlists/' + editId, {
        method: 'PUT', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ title, description })
      });
      if (r.error) return msg('wl-msg', r.error, true);
      closeDrawer('modal-wl-create');
      await reloadAll();
      openWordlist(editId);
      return;
    }

    const items = parseText($('wl-text').value).concat(state.pendingItems || []);
    if (!items.length) return msg('wl-msg', '请粘贴或上传至少一个单词', true);

    msg('wl-msg', '检查重复…');
    const join = await decideJoin(items, 'bulk');
    if (join === null) return msg('wl-msg', '已取消');

    msg('wl-msg', '创建中…');
    const r = await api('/api/wordlists', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ title, description, items })
    });
    if (r.error) return msg('wl-msg', r.error, true);
    if (join) await startStudy(r.id);
    closeDrawer('modal-wl-create');
    await reloadAll();
    openWordlist(r.id);
  });

  function parseText(text)
  {
    const items = [];
    (text || '').split(/\r?\n/).forEach((line, idx) =>
    {
      const raw = line.trim();
      if (!raw) return;
      let cols = raw.includes('\t') ? raw.split('\t') : raw.split(',');
      const word = (cols[0] || '').trim();
      if (!word) return;
      if (idx === 0 && /^(word|单词|词汇)$/i.test(word)) return;
      items.push({
        word,
        meaning: (cols[1] || '').trim(),
        pos: (cols[2] || '').trim()
      });
    });
    return items;
  }

  async function parseFile(file)
  {
    if (/\.json$/i.test(file.name))
    {
      const parsed = JSON.parse(await file.text());
      let raw = parsed.entries || parsed.words || parsed.data || [];
      if (!Array.isArray(raw)) raw = [];
      return raw.map(x =>
      {
        if (typeof x === 'string') return { word: x.trim() };
        return {
          word: String(x.word || x.standard_content || x.name || '').trim(),
          meaning: String(x.meaning || x.definition || '').trim(),
          pos: String(x.pos || '').trim()
        };
      }).filter(x => x.word);
    }

    const buf = await file.arrayBuffer();
    const book = XLSX.read(buf, { type: 'array' });
    const sheet = book.Sheets[book.SheetNames[0]];
    if (!sheet) return [];
    const rows = XLSX.utils.sheet_to_json(sheet, { header: 1, defval: '' });
    const items = [];
    rows.forEach((row, idx) =>
    {
      const word = String(row[0] == null ? '' : row[0]).trim();
      if (!word) return;
      if (idx === 0 && /^(word|单词|词汇)$/i.test(word)) return;
      items.push({
        word,
        meaning: String(row[1] == null ? '' : row[1]).trim(),
        pos: String(row[2] == null ? '' : row[2]).trim()
      });
    });
    return items;
  }

  // ---------- 向词表追加 ----------

  function openWlAdd(id)
  {
    state.addTargetId = id;
    state.addItems = [];
    $('wl-add-text').value = '';
    $('wl-add-file').value = '';
    msg('wl-add-msg', '');
    syncSwitch('wl-add-study', 'bulk');
    openDrawer('modal-wl-add');
  }

  $('wl-add-file').addEventListener('change', async e =>
  {
    const file = e.target.files[0];
    if (!file) return;
    try
    {
      state.addItems = await parseFile(file);
      msg('wl-add-msg', '已从文件读取 ' + state.addItems.length + ' 个词');
    }
    catch (err)
    {
      state.addItems = [];
      msg('wl-add-msg', '文件解析失败：' + err.message, true);
    }
  });

  $('act-wl-add').addEventListener('click', async () =>
  {
    const items = parseText($('wl-add-text').value).concat(state.addItems || []);
    if (!items.length) return msg('wl-add-msg', '请输入或上传至少一个单词', true);

    msg('wl-add-msg', '检查重复…');
    const join = await decideJoin(items, 'bulk');
    if (join === null) return msg('wl-add-msg', '已取消');

    const r = await api('/api/wordlists/' + state.addTargetId + '/items', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ items })
    });
    if (r.error) return msg('wl-add-msg', r.error, true);
    if (join) await startStudy(state.addTargetId);
    closeDrawer('modal-wl-add');
    await reloadAll();
    openWordlist(state.addTargetId);
  });

  // ---------- AI 生成 ----------

  function openAi()
  {
    $('ai-topic').value = '';
    $('ai-count').value = '20';
    $('ai-level').value = 'normal';
    msg('ai-msg', '');
    $('act-wl-ai').disabled = false;
    syncSwitch('ai-study', 'bulk');
    openDrawer('modal-wl-ai');
  }

  $('act-wl-ai').addEventListener('click', async () =>
  {
    const topic = $('ai-topic').value.trim();
    if (!topic) return msg('ai-msg', '请输入主题', true);
    const count = parseInt($('ai-count').value || '20');
    const level = $('ai-level').value;
    $('act-wl-ai').disabled = true;
    msg('ai-msg', '提交任务…');

    const r = await api('/api/wordlists/generate', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ topic, count, level })
    });
    if (!r.task_id)
    {
      $('act-wl-ai').disabled = false;
      return msg('ai-msg', r.error || '提交失败', true);
    }

    const taskId = r.task_id;
    for (let i = 0; i < 60; i++)
    {
      await new Promise(resolve => setTimeout(resolve, 2000));
      const t = await api('/api/ai-tasks/' + taskId);
      if (t.status === 'done' && t.result_ref)
      {
        const newId = parseInt(t.result_ref);
        $('act-wl-ai').disabled = false;
        msg('ai-msg', '生成完成，检查是否加入学习…');
        const join = await decideJoinByList(newId, 'bulk');
        if (join === null)
        {
          msg('ai-msg', '已生成，可在列表中打开');
          await reloadAll();
          return;
        }
        if (join) await startStudy(newId);
        closeDrawer('modal-wl-ai');
        await reloadAll();
        openWordlist(newId);
        return;
      }
      if (t.status === 'failed')
      {
        $('act-wl-ai').disabled = false;
        return msg('ai-msg', '生成失败：' + (t.error || '未知错误'), true);
      }
      msg('ai-msg', '生成中… ' + (t.done || 0) + '/' + (t.total || 1) + '（队列位置 ' + (t.queue_position || 0) + '）');
    }
    $('act-wl-ai').disabled = false;
    msg('ai-msg', '生成超时，请稍后在 AI 任务列表查看结果', true);
  });

  // ---------- 组合词表 ----------

  function openRange()
  {
    const byVol = {};
    state.builtin.forEach(list =>
    {
      const vol = list.volume || '未分册';
      (byVol[vol] = byVol[vol] || []).push(list);
    });
    $('range-units').innerHTML = Object.keys(byVol).map(vol => `
      <div class="wordlib-vol">${esc(vol)}</div>
      ${byVol[vol].map(list => `
        <label class="kp-picker__item">
          <input type="checkbox" data-range-unit data-textbook="${esc(list.textbook || '')}" data-volume="${esc(vol)}" data-unit="${esc(list.unit || '')}">
          <span>${esc(list.unit || list.title)}</span>
          <span class="pill">${list.item_count || 0} 词</span>
        </label>`).join('')}
    `).join('');
    msg('range-msg', '');
    openDrawer('modal-wl-range');
  }

  $('act-wl-range').addEventListener('click', async () =>
  {
    const checked = Array.from(document.querySelectorAll('[data-range-unit]:checked'));
    if (!checked.length) return msg('range-msg', '请至少勾选一个单元', true);
    const units = checked.map(cb => ({
      textbook: cb.dataset.textbook,
      volume: cb.dataset.volume,
      unit: cb.dataset.unit
    }));
    msg('range-msg', '生成中…');
    const r = await api('/api/wordlists/range-package', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ units })
    });
    if (!r.id) return msg('range-msg', r.error || '生成失败', true);
    closeDrawer('modal-wl-range');
    await reloadAll();
    openWordlist(r.id);
  });

  // ---------- 待整理生词导入 ----------

  async function openMyWordsImport()
  {
    const r = await api('/api/wordbook');
    const books = r.data || [];
    $('mywords-book').innerHTML = books.length
      ? books.map(b => `<option value="${b.id}">${esc(b.name)}（${b.entry_count || 0} 个词）</option>`).join('')
      : '<option value="0">还没有生词本，请先新建</option>';
    $('mywords-name').value = '我的生词合集';
    msg('mywords-msg', '');
    setMyWordsTarget('new');
    openDrawer('modal-mywords-import');
  }

  function setMyWordsTarget(target)
  {
    document.querySelectorAll('[data-mywords-target]').forEach(btn =>
      btn.classList.toggle('is-active', btn.dataset.mywordsTarget === target));
    document.querySelectorAll('[data-mywords-panel]').forEach(p =>
      p.hidden = p.dataset.mywordsPanel !== target);
  }

  document.querySelectorAll('[data-mywords-target]').forEach(btn =>
  {
    btn.addEventListener('click', () => setMyWordsTarget(btn.dataset.mywordsTarget));
  });

  $('act-mywords-import').addEventListener('click', async () =>
  {
    const useNew = document.querySelector('[data-mywords-target].is-active').dataset.mywordsTarget === 'new';
    let body;
    if (useNew)
    {
      const name = $('mywords-name').value.trim();
      if (!name) return msg('mywords-msg', '请输入生词本名称', true);
      body = { name };
    }
    else
    {
      const id = parseInt($('mywords-book').value || '0');
      if (!id) return msg('mywords-msg', '请选择一本已有生词本', true);
      body = { wordbook_id: id };
    }
    msg('mywords-msg', '导入中…');
    const r = await api('/api/wordlists/my-words/import', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    if (r.error) return msg('mywords-msg', r.error, true);
    closeDrawer('modal-mywords-import');
    alert('已把 ' + (r.added || 0) + ' 个生词放进「' + (r.name || '生词本') + '」，跳过 ' + (r.skipped || 0) + ' 个（已在其中）');
    await reloadAll();
  });

  // ---------- 生词本 ----------

  function openWbCreate()
  {
    $('create-name').value = '';
    $('create-desc').value = '';
    openDrawer('modal-wb-create');
  }

  $('act-create-wb').addEventListener('click', async () =>
  {
    const name = $('create-name').value.trim();
    const description = $('create-desc').value.trim();
    if (!name) return alert('请输入生词本名称');
    const r = await api('/api/wordbook', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, description })
    });
    if (r.error) return alert(r.error);
    closeDrawer('modal-wb-create');
    await reloadAll();
  });

  function openWbImport()
  {
    $('import-json').value = '';
    $('import-file-path').value = '';
    msg('import-msg', '');
    openDrawer('modal-wb-import');
  }

  document.querySelectorAll('[data-import-tab]').forEach(btn =>
  {
    btn.addEventListener('click', () =>
    {
      document.querySelectorAll('[data-import-tab]').forEach(b => b.classList.toggle('is-active', b === btn));
      document.querySelectorAll('[data-import-panel]').forEach(p => p.hidden = p.dataset.importPanel !== btn.dataset.importTab);
    });
  });

  $('act-wb-import').addEventListener('click', async () =>
  {
    const paste = !document.querySelector('[data-import-panel="paste"]').hidden;
    let body;
    if (paste)
    {
      const raw = $('import-json').value.trim();
      if (!raw) return msg('import-msg', '请粘贴 JSON 内容', true);
      try { body = { json_data: JSON.parse(raw) }; }
      catch (e) { return msg('import-msg', 'JSON 格式错误：' + e.message, true); }
    }
    else
    {
      const path = $('import-file-path').value.trim();
      if (!path) return msg('import-msg', '请输入文件路径', true);
      body = { file_path: path };
    }
    msg('import-msg', '导入中…');
    const r = await api('/api/wordbook/import', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body)
    });
    if (r.error) return msg('import-msg', r.error, true);
    msg('import-msg', '导入完成：新增 ' + (r.imported || 0) + ' 个词，合并重复 ' + (r.merged || 0) + ' 个');
    setTimeout(() => { closeDrawer('modal-wb-import'); reloadAll(); }, 800);
  });

  function openWbAdd()
  {
    state.kpSelected = new Set();
    $('kp-search').value = '';
    $('new-words').value = '';
    syncSwitch('wb-add-study', 'manual');
    openDrawer('modal-wb-add');
    loadAllKps().then(renderKpPicker);
  }

  document.querySelectorAll('[data-tab]').forEach(btn =>
  {
    btn.addEventListener('click', () =>
    {
      document.querySelectorAll('[data-tab]').forEach(b => b.classList.toggle('is-active', b === btn));
      document.querySelectorAll('[data-panel]').forEach(p => p.hidden = p.dataset.panel !== btn.dataset.tab);
    });
  });

  $('kp-search').addEventListener('input', renderKpPicker);

  async function loadAllKps()
  {
    const r = await api('/api/kp?page=1&size=1000');
    state.kps = r.data || [];
  }

  function renderKpPicker()
  {
    const q = ($('kp-search').value || '').toLowerCase();
    const list = (state.kps || []).filter(k =>
      !q || (k.standard_content || k.user_input_content || '').toLowerCase().includes(q));
    const el = $('kp-picker');
    if (!list.length) { el.innerHTML = '<p class="state-line">没有匹配的单词</p>'; return; }
    el.innerHTML = list.map(k => `
      <label class="kp-picker__item">
        <input type="checkbox" data-kp="${k.id}" ${state.kpSelected.has(k.id) ? 'checked' : ''}>
        <span>${esc(k.standard_content || k.user_input_content)}</span>
        ${k.is_solidified === 1 ? '<span class="pill pill--teal">已固化</span>' : '<span class="pill">待固化</span>'}
      </label>`).join('');
  }

  $('kp-picker').addEventListener('change', e =>
  {
    const cb = e.target.closest('[data-kp]');
    if (!cb) return;
    const id = parseInt(cb.dataset.kp);
    if (cb.checked) state.kpSelected.add(id); else state.kpSelected.delete(id);
  });

  $('act-add-existing').addEventListener('click', async () =>
  {
    if (!state.kpSelected.size) return alert('请选择至少一个知识点');
    const r = await api('/api/wordbook/' + state.detail.id + '/entries', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ knowledge_ids: Array.from(state.kpSelected) })
    });
    if (r.error) return alert(r.error);
    closeDrawer('modal-wb-add');
    await reloadAll();
    openWordbook(state.detail.id);
  });

  $('act-add-new').addEventListener('click', async () =>
  {
    const words = $('new-words').value.split(/\r?\n/).map(w => w.trim()).filter(Boolean);
    if (!words.length) return alert('请输入至少一个新词');

    const join = await decideJoin(words.map(w => ({ word: w })), 'manual');
    if (join === null) return;

    const ids = [];
    for (const word of words)
    {
      const r = await api('/api/kp', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ user_input_content: word, join_study: join })
      });
      if (r && r.id) ids.push(r.id);
    }
    if (!ids.length) return alert('录入失败，请重试');

    const r = await api('/api/wordbook/' + state.detail.id + '/entries', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ knowledge_ids: ids })
    });
    if (r.error) return alert(r.error);
    closeDrawer('modal-wb-add');
    await reloadAll();
    openWordbook(state.detail.id);
  });

  async function openWbEdit()
  {
    const r = await api('/api/wordbook/' + state.detail.id);
    if (r.error) return alert(r.error);
    $('edit-name').value = r.name || '';
    $('edit-desc').value = r.description || '';
    $('edit-public').checked = r.is_public === 1;
    openDrawer('modal-wb-edit');
  }

  $('act-save-wb-edit').addEventListener('click', async () =>
  {
    const name = $('edit-name').value.trim();
    if (!name) return alert('名称不能为空');
    const r = await api('/api/wordbook/' + state.detail.id, {
      method: 'PUT', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        name,
        description: $('edit-desc').value.trim(),
        is_public: $('edit-public').checked ? 1 : 0
      })
    });
    if (r.error) return alert(r.error);
    closeDrawer('modal-wb-edit');
    await reloadAll();
    openWordbook(state.detail.id);
  });

  // ---------- T007：Tab / 筛选 / 折叠 / 限量 ----------

  document.querySelectorAll('[data-lib-tab]').forEach(btn =>
  {
    btn.addEventListener('click', () => setTab(btn.dataset.libTab));
  });

  document.querySelectorAll('[data-shared-filter]').forEach(btn =>
  {
    btn.addEventListener('click', () =>
    {
      state.ui.sharedFilter = btn.dataset.sharedFilter;
      document.querySelectorAll('[data-shared-filter]').forEach(b =>
        b.classList.toggle('is-active', b === btn));
      renderShared();
    });
  });

  document.querySelectorAll('[data-private-filter]').forEach(btn =>
  {
    btn.addEventListener('click', () =>
    {
      state.ui.privateFilter = btn.dataset.privateFilter;
      document.querySelectorAll('[data-private-filter]').forEach(b =>
        b.classList.toggle('is-active', b === btn));
      renderPrivate();
    });
  });

  $('btn-shared-expand').addEventListener('click', () =>
  {
    state.ui.openVols = state.ui.openVols === 'all' ? [] : 'all';
    renderShared();
  });

  document.querySelectorAll('[data-more]').forEach(btn =>
  {
    btn.addEventListener('click', () =>
    {
      state.ui.more[btn.dataset.more] = true;
      renderPrivate();
    });
  });

  // ---------- T007：待整理 FAB 拖拽 ----------

  let fabSuppressClick = false;

  function fabKey()
  {
    return 'wordlibFab:' + ((state.me && state.me.user_id) || 0);
  }

  function applyFabPos(pos)
  {
    const fab = $('btn-mywords-fab');
    if (!fab || !pos) return;
    const maxX = window.innerWidth - fab.offsetWidth - 8;
    const maxY = window.innerHeight - fab.offsetHeight - 8;
    const x = Math.min(Math.max(8, pos.x), Math.max(8, maxX));
    const y = Math.min(Math.max(8, pos.y), Math.max(8, maxY));
    fab.style.left = x + 'px';
    fab.style.top = y + 'px';
    fab.style.right = 'auto';
  }

  function initFabDrag()
  {
    const fab = $('btn-mywords-fab');
    if (!fab) return;
    try
    {
      const raw = localStorage.getItem(fabKey());
      if (raw) applyFabPos(JSON.parse(raw));
    }
    catch (e) {}

    let startX = 0, startY = 0, baseX = 0, baseY = 0, moved = false, pid = null;

    fab.addEventListener('pointerdown', e =>
    {
      if (e.button !== 0) return;
      const r = fab.getBoundingClientRect();
      startX = e.clientX;
      startY = e.clientY;
      baseX = r.left;
      baseY = r.top;
      moved = false;
      pid = e.pointerId;
      try { fab.setPointerCapture(pid); } catch (err) {}
      fab.classList.add('is-dragging');
    });

    fab.addEventListener('pointermove', e =>
    {
      if (pid === null) return;
      const dx = e.clientX - startX;
      const dy = e.clientY - startY;
      if (!moved && Math.hypot(dx, dy) < 6) return;
      moved = true;
      applyFabPos({ x: baseX + dx, y: baseY + dy });
      e.preventDefault();
    });

    function endDrag()
    {
      if (pid === null) return;
      try { fab.releasePointerCapture(pid); } catch (err) {}
      pid = null;
      fab.classList.remove('is-dragging');
      if (!moved) return;
      fabSuppressClick = true;
      const r = fab.getBoundingClientRect();
      try
      {
        localStorage.setItem(fabKey(), JSON.stringify({ x: Math.round(r.left), y: Math.round(r.top) }));
      }
      catch (err) {}
      setTimeout(() => { fabSuppressClick = false; }, 0);
    }

    fab.addEventListener('pointerup', endDrag);
    fab.addEventListener('pointercancel', endDrag);
  }

  // ---------- T007：把别的整本单词本加入当前单词本（层级成员） ----------

  let importTargetId = 0;   // 当前打开的单词本 = 接收方
  let importTrailIndex = 0;

  function importCandidates()
  {
    return state.builtin.concat(state.user, state.range).filter(l => l.id !== importTargetId);
  }

  function candidateLabel(l)
  {
    if (l.source === 'builtin') return '共用';
    return l.kind === 'combo' ? '合并本' : '单词本';
  }

  function renderWlImportTargets()
  {
    const q = ($('wl-import-q').value || '').toLowerCase();
    const list = importCandidates()
      .filter(l => !q || String(l.title || '').toLowerCase().includes(q));
    $('wl-import-list').innerHTML = list.length
      ? list.map(l => `
          <button class="kp-chip" type="button" data-import-source="${l.id}">
            <span class="kp-chip__word">${esc(l.title)}</span>
            <span class="kp-chip__meta">${candidateLabel(l)} · ${l.item_count || 0} 词</span>
          </button>`).join('')
      : '<p class="state-line">没有可加入的其他单词本。</p>';
  }

  function openWlImport(targetId)
  {
    importTargetId = targetId;
    importTrailIndex = Math.max(0, state.trail.findIndex(t => t.id === targetId));
    $('wl-import-q').value = '';
    $('wl-import-msg').textContent = '';
    renderWlImportTargets();
    openDrawer('modal-wl-import');
  }

  $('wl-import-q').addEventListener('input', renderWlImportTargets);

  $('wl-import-list').addEventListener('click', async e =>
  {
    const btn = e.target.closest('[data-import-source]');
    if (!btn) return;
    const sourceId = parseInt(btn.dataset.importSource);
    const source = importCandidates().find(l => l.id === sourceId);
    const title = source ? source.title : '该单词本';
    const msgEl = $('wl-import-msg');
    msgEl.textContent = '正在加入…';
    const r = await api('/api/wordlists/' + sourceId + '/import-whole', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ target_id: importTargetId })
    });
    if (r.error) { msgEl.textContent = r.error; return; }
    msgEl.textContent = r.copied_title
      ? '共用词表已复制为私有本「' + r.copied_title + '」并整本加入，共 ' + (r.added || 0) + ' 个词。'
      : '已把「' + title + '」整本加入，共 ' + (r.added || 0) + ' 个词。';
    await reloadAll();
    // 原地刷新当前单词本，层级成员与词数立即可见
    setTimeout(async () =>
    {
      closeDrawer('modal-wl-import');
      await openWordlist(importTargetId, { crumbIndex: importTrailIndex });
    }, 600);
  });

  // ---------- T007：新建 / 管理组合 ----------

  function comboMsg(text, danger)
  {
    const el = $('combo-msg');
    el.className = 'state-line' + (danger ? ' state-line--danger' : '');
    el.textContent = text || '';
  }

  function memberKey(m) { return m.type + ':' + m.id; }

  function renderComboSelected()
  {
    const el = $('combo-selected');
    const list = state.combo.members;
    $('combo-selected-cnt').textContent = list.length;
    if (!list.length)
    {
      el.innerHTML = '<p class="state-line">还没有成员。可以从下方添加单词本、已有合并本或单个单词。</p>';
      return;
    }
    el.innerHTML = list.map((m, i) => `
      <div class="combo-chip">
        <span class="combo-chip__type">${m.type === 'word' ? '单词' : '词表'}</span>
        <span class="combo-chip__title">${esc(m.title || '')}</span>
        <button class="combo-chip__btn" type="button" data-sel-up="${i}" title="上移" ${i === 0 ? 'disabled' : ''}>↑</button>
        <button class="combo-chip__btn" type="button" data-sel-down="${i}" title="下移" ${i === list.length - 1 ? 'disabled' : ''}>↓</button>
        <button class="combo-chip__btn" type="button" data-sel-del="${i}" title="移除">×</button>
      </div>`).join('');
  }

  function candidateRow(m)
  {
    const on = state.combo.members.some(x => memberKey(x) === memberKey(m));
    return `
      <button class="kp-chip${on ? ' is-on' : ''}" type="button" data-combo-add="${memberKey(m)}">
        <span class="kp-chip__word">${esc(m.title || '')}</span>
        <span class="kp-chip__meta">${esc(m.meta || '')}</span>
        <span class="kp-chip__mark">${on ? '已选' : '+'}</span>
      </button>`;
  }

  function fillPool(list)
  {
    if (!state.combo.pool) state.combo.pool = {};
    list.forEach(m => { state.combo.pool[memberKey(m)] = m; });
  }

  function renderComboLists()
  {
    const q = ($('combo-q-list').value || '').toLowerCase();
    const list = [];
    state.builtin.forEach(l => list.push({
      type: 'list', id: l.id, title: l.title,
      meta: [l.textbook, l.volume, l.unit].filter(Boolean).join(' · ') || '共用单词本'
    }));
    state.user.forEach(l => list.push({ type: 'list', id: l.id, title: l.title, meta: '自建词表' }));
    const shown = list.filter(m => !q || (m.title + ' ' + m.meta).toLowerCase().includes(q));
    fillPool(shown);
    $('combo-list').innerHTML = shown.length
      ? shown.map(candidateRow).join('')
      : '<p class="state-line">没有匹配的单词本。</p>';
  }

  function renderComboCombos()
  {
    const q = ($('combo-q-combo').value || '').toLowerCase();
    const list = state.range
      .filter(l => state.combo.mode === 'create' || l.id !== state.combo.id)
      .map(l => ({ type: 'list', id: l.id, title: l.title, meta: '已有合并本' }))
      .filter(m => !q || m.title.toLowerCase().includes(q));
    fillPool(list);
    $('combo-combo').innerHTML = list.length
      ? list.map(candidateRow).join('')
      : '<p class="state-line">还没有其他合并本可加入。</p>';
  }

  function renderComboWords()
  {
    const q = ($('combo-q-word').value || '').toLowerCase();
    const list = state.combo.wordItems
      .filter(it => !q || (it.word || '').toLowerCase().includes(q))
      .map(it => ({
        type: 'word', id: it.item_id, title: it.word,
        meta: it.meaning || '释义待补充'
      }));
    fillPool(list);
    $('combo-word').innerHTML = list.length
      ? list.map(candidateRow).join('')
      : '<p class="state-line">没有匹配的单词。</p>';
  }

  async function loadComboWords()
  {
    const id = parseInt($('combo-word-src').value);
    state.combo.wordItems = [];
    if (!id)
    {
      $('combo-word').innerHTML = '<p class="state-line">请选择一本词表。</p>';
      return;
    }
    const r = await api('/api/wordlists/' + id + '/items?size=300');
    if (r.error) return comboMsg(r.error, true);
    state.combo.wordItems = r.data || [];
    renderComboWords();
  }

  function switchComboSrc(src)
  {
    state.combo.src = src;
    document.querySelectorAll('[data-combo-src]').forEach(b =>
      b.classList.toggle('is-active', b.dataset.comboSrc === src));
    document.querySelectorAll('[data-combo-panel]').forEach(p =>
    {
      p.hidden = p.dataset.comboPanel !== src;
    });
    if (src === 'list') renderComboLists();
    else if (src === 'combo') renderComboCombos();
    else renderComboWords();
  }

  function toggleComboMember(m)
  {
    const key = memberKey(m);
    const i = state.combo.members.findIndex(x => memberKey(x) === key);
    if (i >= 0) state.combo.members.splice(i, 1);
    else state.combo.members.push(m);
    renderComboSelected();
    if (state.combo.src === 'list') renderComboLists();
    else if (state.combo.src === 'combo') renderComboCombos();
    else renderComboWords();
  }

  function comboWordOptions()
  {
    const opts = state.user.concat(state.builtin);
    $('combo-word-src').innerHTML = opts.length
      ? opts.map(l => `<option value="${l.id}">${esc(l.title)}</option>`).join('')
      : '<option value="">暂无词表</option>';
  }

  async function openComboCreate()
  {
    state.combo = { mode: 'create', id: 0, members: [], original: [], wordItems: [], pool: {} };
    $('combo-title').textContent = '新建合并本';
    $('combo-meta').hidden = false;
    $('combo-name').value = '';
    $('combo-desc').value = '';
    $('act-combo-save').textContent = '保存';
    comboMsg('');
    comboWordOptions();
    switchComboSrc('list');
    renderComboSelected();
    openDrawer('modal-combo');
    await loadComboWords();
  }

  async function openComboManage(id)
  {
    const r = await api('/api/wordlists/' + id + '/children');
    if (r.error) return alert(r.error);
    const members = (r.data || []).map(m => ({
      type: m.child_type,
      id: m.child_id,
      memberId: m.member_id,
      title: m.title || '未命名',
      meta: m.child_type === 'list' ? (m.kind === 'combo' ? '已有合并本' : '单词本') : (m.meaning || '单词')
    }));
    state.combo = {
      mode: 'manage', id, members, wordItems: [], pool: {},
      original: members.map(m => ({ memberId: m.memberId, key: memberKey(m) }))
    };
    $('combo-title').textContent = '管理成员';
    $('combo-meta').hidden = true;
    $('act-combo-save').textContent = '保存成员';
    comboMsg('');
    comboWordOptions();
    switchComboSrc('list');
    renderComboSelected();
    openDrawer('modal-combo');
    await loadComboWords();
  }

  $('modal-combo').addEventListener('click', e =>
  {
    const add = e.target.closest('[data-combo-add]');
    if (add)
    {
      const m = state.combo.pool && state.combo.pool[add.dataset.comboAdd];
      if (m) toggleComboMember(m);
      return;
    }
    const srcBtn = e.target.closest('[data-combo-src]');
    if (srcBtn)
    {
      switchComboSrc(srcBtn.dataset.comboSrc);
      return;
    }
    const up = e.target.closest('[data-sel-up]');
    if (up)
    {
      const i = parseInt(up.dataset.selUp);
      if (i > 0)
      {
        const list = state.combo.members;
        [list[i - 1], list[i]] = [list[i], list[i - 1]];
        renderComboSelected();
      }
      return;
    }
    const down = e.target.closest('[data-sel-down]');
    if (down)
    {
      const i = parseInt(down.dataset.selDown);
      const list = state.combo.members;
      if (i >= 0 && i < list.length - 1)
      {
        [list[i + 1], list[i]] = [list[i], list[i + 1]];
        renderComboSelected();
      }
      return;
    }
    const del = e.target.closest('[data-sel-del]');
    if (del)
    {
      state.combo.members.splice(parseInt(del.dataset.selDel), 1);
      renderComboSelected();
      if (state.combo.src === 'list') renderComboLists();
      else if (state.combo.src === 'combo') renderComboCombos();
      else renderComboWords();
    }
  });

  $('combo-q-list').addEventListener('input', renderComboLists);
  $('combo-q-combo').addEventListener('input', renderComboCombos);
  $('combo-q-word').addEventListener('input', renderComboWords);
  $('combo-word-src').addEventListener('change', loadComboWords);

  $('act-combo-save').addEventListener('click', async () =>
  {
    const c = state.combo;
    if (!c.members.length) return comboMsg('至少添加一个成员。', true);

    if (c.mode === 'create')
    {
      const title = $('combo-name').value.trim();
      if (!title) return comboMsg('请填写名称。', true);
      comboMsg('正在保存…');
      const r = await api('/api/wordlists/compose', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          title,
          description: $('combo-desc').value.trim(),
          members: c.members.map(m => ({ type: m.type, id: m.id }))
        })
      });
      if (r.error) return comboMsg(r.error, true);
      closeDrawer('modal-combo');
      await reloadAll();
      setTab('private');
      openWordlist(r.id);
      return;
    }

    comboMsg('正在保存…');
    const desiredKeys = c.members.map(memberKey);
    for (const e of c.original)
    {
      if (desiredKeys.includes(e.key)) continue;
      const d = await api('/api/wordlists/' + c.id + '/children/' + e.memberId, { method: 'DELETE' });
      if (d.error) return comboMsg(d.error, true);
    }
    const addItems = c.members.filter(m => !c.original.some(e => e.key === memberKey(m)))
      .map(m => ({ type: m.type, id: m.id }));
    if (addItems.length)
    {
      const a = await api('/api/wordlists/' + c.id + '/children', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ items: addItems })
      });
      if (a.error) return comboMsg(a.error, true);
    }
    const rows = (await api('/api/wordlists/' + c.id + '/children')).data || [];
    const map = {};
    rows.forEach(m => { map[m.child_type + ':' + m.child_id] = m.member_id; });
    const ids = desiredKeys.map(k => map[k]).filter(Boolean);
    if (ids.length)
    {
      const o = await api('/api/wordlists/' + c.id + '/children/order', {
        method: 'PUT', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ids })
      });
      if (o.error) return comboMsg(o.error, true);
    }
    closeDrawer('modal-combo');
    await reloadAll();
    openWordlist(c.id);
  });

  // ---------- 启动 ----------

  loadMe().then(async () =>
  {
    await reloadAll();
    setTab(initialTab());
    initFabDrag();
  });
})();
