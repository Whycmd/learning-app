// 错题本（v1.5.1-T002 ERROR-RETRY）
(function ()
{
  'use strict';

  const TYPES = ['spelling', 'meaning', 'collocation', 'grammar', 'unknown'];
  const TYPE_LABEL = {
    spelling: '拼写', meaning: '词义', collocation: '搭配',
    grammar: '语法', unknown: '待判定'
  };
  const SOURCE_LABEL = { exercise: '练习', quiz: '每日小测', retry: '重练' };

  let archivedView = false;

  function $(id) { return document.getElementById(id); }
  function esc(s)
  {
    return String(s == null ? '' : s).replace(/[&<>"']/g, c =>
      ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  }
  function msg(t, cls)
  {
    const e = $('wb-msg');
    if (e) { e.className = 'state-line' + (cls ? ' ' + cls : ''); e.textContent = t || ''; }
  }
  async function api(url, opts)
  {
    const r = await fetch(url, Object.assign({ cache: 'no-store' }, opts || {}));
    let j = null;
    try { j = await r.json(); } catch (e) {}
    return { ok: r.ok, status: r.status, data: j };
  }
  function jsonBody(obj)
  {
    return { headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(obj) };
  }
  // 与后端 next_retry_time（YYYY-MM-DD HH:MM）同格式，可直接字符串比较
  function nowStr()
  {
    const d = new Date();
    const p = n => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) +
      ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
  }
  function isDue(it)
  {
    const r = it.retry || {};
    return r.status === 'queued' && !!r.next_retry_time && r.next_retry_time <= nowStr();
  }

  // ---------- 顶部摘要 ----------
  async function loadSummary()
  {
    const { ok, data } = await api('/api/wrongbook/summary');
    if (!ok)
    {
      $('wb-summary').textContent = '未登录';
      msg('请先登录后再查看错题本', 'state-line--danger');
      return;
    }
    const total = data.total_count || 0, due = data.due_count || 0;
    $('wb-summary').textContent = '错题 ' + total + ' 词 · 待重练 ' + due;
    $('wb-start').textContent = due > 0 ? '开始重练 ' + due : '开始重练';
  }

  // ---------- 列表（按错因折叠） ----------
  function rowHtml(it)
  {
    const kid = it.knowledge_id;
    const rid = (it.retry && it.retry.id) || 0;
    const retryBtn = it.archived ? ''
      : `<button class="btn btn-secondary u-btn-sm" data-act="start-one" data-kid="${kid}" data-rid="${rid}">立即再练</button>`;
    return `<div class="wrong-row">
        <div class="wrong-row__main">
          <span class="kp-pill">${esc(it.word || ('#' + kid))}</span>
          <span class="state-line">${esc(it.meaning || '')}</span>
          <span class="error-attr">错因：<strong>${esc(it.latest_error_label || '待判定')}</strong> · 共错 ${it.error_count} 次 · 最近 ${esc(it.last_error_at || '')}</span>
        </div>
        <div class="u-row-sm">
          ${retryBtn}
          <button class="btn btn-secondary u-btn-sm" data-act="detail" data-kid="${kid}">详情</button>
        </div>
      </div>
      <div class="wb-detail-wrap" id="wb-detail-${kid}" hidden></div>`;
  }

  async function loadWords()
  {
    const type = $('wb-type').value;
    const source = $('wb-source').value;
    const days = $('wb-days').value;
    const keyword = $('wb-keyword').value.trim();
    const qs = new URLSearchParams({ page: '1', size: '100' });
    if (type) qs.set('error_type', type);
    if (source) qs.set('source_type', source);
    if (days) qs.set('days', days);
    if (keyword) qs.set('keyword', keyword);
    if (archivedView) qs.set('include_archived', '1');

    $('wb-list').innerHTML = '<p class="state-line">加载中…</p>';
    const { ok, data } = await api('/api/wrongbook/words?' + qs.toString());
    if (!ok) { $('wb-list').innerHTML = '<p class="state-line state-line--danger">读取失败</p>'; return; }

    let items = data.items || [];
    if (archivedView) items = items.filter(x => x.archived);
    $('wb-count').textContent = '共 ' + (archivedView ? items.length : (data.total || 0)) + ' 词';
    if (!items.length)
    {
      $('wb-list').innerHTML = archivedView
        ? '<p class="state-line">归档区暂为空。</p>'
        : '<p class="state-line">暂无错题。答错的题目会自动进入这里。</p>';
      return;
    }

    const groups = TYPES.map(t => ({ t: t, list: [] }));
    for (const it of items)
    {
      const i = TYPES.indexOf(it.latest_error_type);
      groups[i >= 0 ? i : 4].list.push(it);
    }

    $('wb-list').innerHTML = groups.filter(g => g.list.length).map(g =>
    {
      const open = archivedView || g.list.some(isDue);
      return `<details class="wb-group"${open ? ' open' : ''}>
        <summary class="wb-group__head">
          <span>${TYPE_LABEL[g.t]}（${g.list.length}）</span>
        </summary>
        <div class="wb-group__body">${g.list.map(rowHtml).join('')}</div>
      </details>`;
    }).join('');
  }

  // ---------- 行内详情 ----------
  async function showDetail(kid, force)
  {
    const box = $('wb-detail-' + kid);
    if (!box) return;
    if (!force && !box.hidden) { box.hidden = true; return; }
    if (!force && box.dataset.loaded === '1') { box.hidden = false; return; }

    box.hidden = false;
    box.innerHTML = '<p class="state-line">加载中…</p>';
    const { ok, data } = await api('/api/wrongbook/words/' + kid);
    if (!ok) { box.innerHTML = '<p class="state-line state-line--danger">详情读取失败</p>'; return; }

    const items = data.items || [];
    const latest = items[0] || null;
    const retry = data.retry;
    const archived = !!retry && retry.status === 'archived';

    const hist = items.slice(0, 5).map(it =>
      `<div class="wrong-detail">
        <p class="state-line">${esc(SOURCE_LABEL[it.source_type] || it.source_type)} · 第 ${it.question_no} 题 · ${esc(it.created_at)}</p>
        <p>作答：${esc(it.user_answer || '—')} <span class="state-line">/ 正确：${esc(it.correct_answer || '—')}</span></p>
        <p class="error-attr">${esc(it.reason || '暂无归因')}</p>
      </div>`).join('');

    const attrSel = latest
      ? `<label class="u-row-sm u-mt-md">
          <span class="field-label">修正错因</span>
          <select class="input" data-attr-id="${latest.id}">
            ${TYPES.map(t => `<option value="${t}"${t === latest.error_type ? ' selected' : ''}>${TYPE_LABEL[t]}</option>`).join('')}
          </select>
        </label>`
      : '';

    const retryLine = retry
      ? `<p class="state-line">已重练 ${retry.retry_count} 次 · 连续答对 ${retry.consecutive_correct} 次 · 下次 ${esc(retry.next_retry_time || '—')}</p>`
      : '';

    box.innerHTML = retryLine + hist + attrSel +
      `<div class="u-row-sm u-mt-md">
        ${retry && !archived ? `<button class="btn btn-primary u-btn-sm" data-act="start-one" data-kid="${kid}" data-rid="${retry.id}">立即再练</button>` : ''}
        <button class="btn btn-secondary u-btn-sm" data-act="to-wordbook" data-kid="${kid}">加入单词本</button>
        <button class="btn btn-secondary u-btn-sm" data-act="mastered" data-kid="${kid}" data-rid="${retry ? retry.id : 0}">标记已掌握</button>
        <a class="btn btn-secondary u-btn-sm" href="/pages/kp-detail.html?id=${kid}">看精讲</a>
        ${retry ? (archived
          ? `<button class="btn btn-secondary u-btn-sm" data-act="restore" data-rid="${retry.id}">恢复</button>`
          : `<button class="btn btn-secondary u-btn-sm" data-act="archive" data-rid="${retry.id}">归档</button>`) : ''}
      </div>`;
    box.dataset.loaded = '1';
  }

  // ---------- 改归因 ----------
  document.addEventListener('change', async e =>
  {
    const sel = e.target.closest('select[data-attr-id]');
    if (!sel) return;
    const kid = sel.closest('.wb-detail-wrap')?.id.replace('wb-detail-', '');
    const { ok } = await api('/api/error-attributions/' + sel.dataset.attrId,
      Object.assign({ method: 'PUT' }, jsonBody({ error_type: sel.value })));
    msg(ok ? '错因已更新' : '错因更新失败', ok ? 'state-line--success' : 'state-line--danger');
    if (ok) { await loadWords(); loadSummary(); if (kid) showDetail(kid, true); }
  });

  // ---------- 加入单词本 ----------
  async function addToWordbook(kid)
  {
    const list = await api('/api/wordbook');
    let id = 0;
    const books = (list.ok && list.data && list.data.data) || [];
    if (books.length) id = books[0].id;
    if (!id)
    {
      const c = await api('/api/wordbook', Object.assign({ method: 'POST' }, jsonBody({ name: '我的单词本' })));
      if (c.ok && c.data) id = c.data.id;
    }
    if (!id) { msg('没有可用的单词本', 'state-line--danger'); return; }
    const r = await api('/api/wordbook/' + id + '/entries',
      Object.assign({ method: 'POST' }, jsonBody({ knowledge_ids: [Number(kid)] })));
    msg(r.ok ? '已加入单词本' : '加入单词本失败', r.ok ? 'state-line--success' : 'state-line--danger');
  }

  // ---------- 加入导出词表 ----------
  async function ensureExportList()
  {
    const { ok, data } = await api('/api/wordlists?source=user');
    if (!ok) return 0;
    const list = (data && data.data) || [];
    const found = list.find(x => x.title === '错题导出');
    if (found) return found.id;
    const c = await api('/api/wordlists',
      Object.assign({ method: 'POST' }, jsonBody({ title: '错题导出', description: '错题本自动维护的导出词表' })));
    return c.ok && c.data ? c.data.id : 0;
  }

  async function exportWords(words)
  {
    if (!words.length) { msg('没有可加入的错题', ''); return; }
    const id = await ensureExportList();
    if (!id) { msg('创建「错题导出」词表失败', 'state-line--danger'); return; }
    const { ok, data } = await api('/api/wordlists/' + id + '/items',
      Object.assign({ method: 'POST' }, jsonBody({ words: words })));
    if (ok) msg('已加入「错题导出」：新增 ' + (data.added || 0) + '，跳过 ' + (data.skipped || 0), 'state-line--success');
    else msg('加入词表失败', 'state-line--danger');
  }

  // ---------- 重练队列 ----------
  async function loadRetry()
  {
    const { ok, data } = await api('/api/retry-items');
    if (!ok) { $('wb-retry').innerHTML = '<p class="state-line state-line--danger">读取失败</p>'; return; }
    const items = Array.isArray(data) ? data : ((data && data.data) || []);
    $('wb-retry-count').textContent = '共 ' + items.length + ' 词';
    if (!items.length)
    {
      $('wb-retry').innerHTML = '<p class="state-line">暂无重练词。</p>';
      return;
    }
    $('wb-retry').innerHTML = items.map(it =>
    {
      const archived = it.status === 'archived';
      return `<div class="wrong-row">
        <div class="wrong-row__main">
          <span class="kp-pill">${esc(it.word || ('#' + it.knowledge_id))}</span>
          <span class="state-line">${esc(it.meaning || '')}</span>
          <span class="state-line">下次重练 ${esc(it.next_retry_time || '—')} · 已练 ${it.retry_count} 次 · 连对 ${it.consecutive_correct}/3</span>
        </div>
        <div class="u-row-sm">
          ${archived ? '' : `<button class="btn btn-secondary u-btn-sm" data-act="start-one" data-kid="${it.knowledge_id}" data-rid="${it.id}">立即练</button>
          <button class="btn btn-secondary u-btn-sm" data-act="skip" data-rid="${it.id}">跳过</button>
          <button class="btn btn-secondary u-btn-sm" data-act="archive" data-rid="${it.id}">归档</button>`}
          ${archived ? `<button class="btn btn-secondary u-btn-sm" data-act="restore" data-rid="${it.id}">恢复</button>` : ''}
        </div>
      </div>`;
    }).join('');
  }

  // ---------- 事件 ----------
  document.addEventListener('click', async e =>
  {
    const btn = e.target.closest('button[data-act]');
    if (!btn) return;
    const act = btn.dataset.act;
    const rid = btn.dataset.rid;
    const kid = btn.dataset.kid;

    if (act === 'detail') { showDetail(kid); return; }

    if (act === 'start-one')
    {
      btn.disabled = true;
      if (rid && rid !== '0') await api('/api/retry-items/' + rid + '/advance', { method: 'POST' });
      location.href = '/pages/review.html?retry=1&kid=' + encodeURIComponent(kid);
      return;
    }

    if (act === 'to-wordbook') { btn.disabled = true; await addToWordbook(kid); btn.disabled = false; return; }

    if (act === 'mastered')
    {
      btn.disabled = true;
      const r = await api('/api/learn/' + kid + '/proficiency',
        Object.assign({ method: 'PUT' }, jsonBody({ proficiency_level: 5 })));
      if (r.ok && rid && rid !== '0') await api('/api/retry-items/' + rid + '/archive', { method: 'POST' });
      btn.disabled = false;
      msg(r.ok ? '已标记掌握并归档' : '标记失败', r.ok ? 'state-line--success' : 'state-line--danger');
      if (r.ok) { loadWords(); loadSummary(); loadRetry(); }
      return;
    }

    if (!rid) return;
    btn.disabled = true;
    const { ok } = await api('/api/retry-items/' + rid + '/' + act, { method: 'POST' });
    btn.disabled = false;
    if (ok)
    {
      msg('操作完成', 'state-line--success');
      loadWords(); loadRetry(); loadSummary();
    }
    else msg('操作失败，请刷新后重试', 'state-line--danger');
  });

  $('btn-refresh').addEventListener('click', () => { loadWords(); loadSummary(); loadRetry(); });
  ['wb-type', 'wb-source', 'wb-days'].forEach(id => $(id).addEventListener('change', loadWords));
  $('btn-archived').addEventListener('click', function ()
  {
    archivedView = !archivedView;
    this.setAttribute('aria-pressed', archivedView ? 'true' : 'false');
    this.textContent = archivedView ? '返回错题' : '归档视图';
    loadWords();
  });
  $('btn-export').addEventListener('click', async () =>
  {
    const { ok, data } = await api('/api/wrongbook/words?page=1&size=100');
    if (!ok) { msg('错题读取失败', 'state-line--danger'); return; }
    const words = (data.items || []).filter(x => !x.archived).map(x => x.word).filter(Boolean);
    exportWords(words);
  });

  // v1.5.1-T003：支持周报按错因直达预筛选（?error_type=spelling&days=7）
  (function applyUrlFilters()
  {
    const q = new URLSearchParams(location.search);
    const type = q.get('error_type');
    const days = q.get('days');
    if (type && TYPES.indexOf(type) >= 0) $('wb-type').value = type;
    if (days && Array.from($('wb-days').options).some(o => o.value === days)) $('wb-days').value = days;
  })();

  loadSummary();
  loadWords();
  loadRetry();
})();
