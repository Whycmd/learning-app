/* v1.5.0-T009 T6：导出前本地估算（页数 / 字数）与确认弹层
 * 不调 AI、不扣额度；估算仅为打印预估，实际以生成文件为准。
 */
(function ()
{
  'use strict';

  // A4 小四、1.5 倍行距的粗略排版容量（字/页）
  const CHAR_PER_PAGE = 900;

  function num(v) { return Math.max(0, Number(v) || 0); }

  function fmtChars(n)
  {
    n = num(n);
    if (n < 10000) return String(n);
    return (n / 10000).toFixed(1).replace(/\.0$/, '') + ' 万';
  }

  // 精讲讲义：每词另页 + 该词自测答案独立页
  function explanation(count, chars)
  {
    count = num(count); chars = num(chars);
    if (!count) return { pages: 0, chars: 0 };
    const avg = chars / count;
    const contentPages = Math.max(1, Math.ceil(avg / CHAR_PER_PAGE));
    return {
      pages: count * (contentPages + 1),
      chars: Math.round((chars + count * 120) / 10) * 10
    };
  }

  // 练习卷：正文 + 答案页
  function paper(chars, itemCount)
  {
    chars = num(chars); itemCount = num(itemCount);
    if (!chars && !itemCount) return { pages: 0, chars: 0 };
    const body = Math.max(1, Math.ceil(chars / 1100));
    // 主观题留白更多，题量超 15 时加半页量
    const extra = itemCount > 15 ? 1 : 0;
    return { pages: body + 1 + extra, chars: chars };
  }

  // 单词卡：每页 6 张
  function cards(count, chars)
  {
    count = num(count); chars = num(chars);
    if (!count) return { pages: 0, chars: 0 };
    return {
      pages: Math.ceil(count / 6),
      chars: chars || count * 16
    };
  }

  let activeResolve = null;

  function close(result)
  {
    const box = document.getElementById('est-dialog');
    if (box) box.remove();
    document.removeEventListener('keydown', onKey);
    const r = activeResolve;
    activeResolve = null;
    if (r) r(result);
  }

  function onKey(e) { if (e.key === 'Escape') close(false); }

  // 返回 Promise<boolean>：true 继续导出，false 取消
  function confirm(opts)
  {
    opts = opts || {};
    if (activeResolve) close(false);

    const mask = document.createElement('div');
    mask.className = 'est-overlay';
    mask.id = 'est-dialog';

    const card = document.createElement('div');
    card.className = 'est-card';
    card.setAttribute('role', 'dialog');
    card.setAttribute('aria-modal', 'true');

    const title = document.createElement('h3');
    title.className = 'est-card__title';
    title.textContent = opts.title || '确认导出';

    const main = document.createElement('p');
    main.className = 'est-card__main';
    const pages = num(opts.pages);
    const chars = num(opts.chars);
    main.innerHTML = '预计 <strong>' + pages + '</strong> 页 / 约 <strong>' + fmtChars(chars) + '</strong> 字';

    const note = document.createElement('p');
    note.className = 'state-line';
    note.textContent = opts.note || '本地估算，实际页数以生成文件为准；导出不扣 AI 额度。';

    const row = document.createElement('div');
    row.className = 'u-row-sm u-mt-md';

    const ok = document.createElement('button');
    ok.type = 'button';
    ok.className = 'btn btn-primary';
    ok.textContent = opts.confirmText || '继续导出';
    ok.addEventListener('click', () => close(true));

    const cancel = document.createElement('button');
    cancel.type = 'button';
    cancel.className = 'btn btn-secondary';
    cancel.textContent = '取消';
    cancel.addEventListener('click', () => close(false));

    row.appendChild(ok);
    row.appendChild(cancel);
    card.appendChild(title);
    card.appendChild(main);
    card.appendChild(note);
    card.appendChild(row);
    mask.appendChild(card);
    mask.addEventListener('click', e => { if (e.target === mask) close(false); });
    document.body.appendChild(mask);
    document.addEventListener('keydown', onKey);

    return new Promise(resolve => { activeResolve = resolve; });
  }

  window.ExportEstimator = {
    explanation: explanation,
    paper: paper,
    cards: cards,
    confirm: confirm,
    fmtChars: fmtChars
  };
})();
