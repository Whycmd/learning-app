// v1.5.3-T003 首用提示（D5）：全站仅 2 条，本机 localStorage 记忆，各自仅出现一次。
// 用法：<div class="first-tip" data-tip-key="eng_tip_xxx" hidden>…<button data-tip-close>知道了</button></div>
// 点「知道了」或所在 section 内任意操作后永久消失；人工修改渠道：文案在各页面 HTML，记忆键在 data-tip-key。
(function ()
{
  'use strict';

  function init()
  {
    document.querySelectorAll('.first-tip[data-tip-key]').forEach(tip =>
    {
      const key = tip.getAttribute('data-tip-key');
      let read = true;
      try { read = localStorage.getItem(key) === '1'; } catch (e) {}
      if (!read) tip.hidden = false;
      const host = tip.closest('section') || document.body;
      const dismiss = () =>
      {
        tip.hidden = true;
        try { localStorage.setItem(key, '1'); } catch (e) {}
        host.removeEventListener('click', onClick);
        host.removeEventListener('change', dismiss);
      };
      const onClick = e => { if (!e.target.closest('.first-tip')) dismiss(); };
      const btn = tip.querySelector('[data-tip-close]');
      if (btn) btn.addEventListener('click', dismiss);
      host.addEventListener('click', onClick);
      host.addEventListener('change', dismiss);
    });
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})();
