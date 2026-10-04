// 全局主题切换（v1.3.0-T102 导航接管版）
// 用法：在 <head> 中、CSS 之后引入。
//   ① 立即应用 localStorage 已保存主题，避免暗色用户白屏闪烁（同步执行，不依赖 DOM）；
//   ② 导出 initThemeButton() 供 nav.js 在注入 topbar 后调用（nav.js 负责写按钮 HTML）。
// 人工修改渠道：改 KEY 换存储键；改 .theme-toggle 样式（components.css）换按钮外观。
(function ()
{
  const KEY = 'elfl-theme';
  const root = document.documentElement;

  const ICON_SUN =
    '<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="5"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>';
  const ICON_MOON =
    '<svg viewBox="0 0 24 24"><path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/></svg>';

  function current()
  {
    return root.getAttribute('data-theme') === 'dark' ? 'dark' : 'light';
  }

  function apply(theme)
  {
    if (theme === 'dark') root.setAttribute('data-theme', 'dark');
    else root.removeAttribute('data-theme');

    const btn = document.getElementById('theme-toggle');
    if (btn)
    {
      btn.innerHTML = theme === 'dark' ? ICON_SUN : ICON_MOON;
      btn.title = theme === 'dark' ? '切换为亮色' : '切换为暗色';
    }
  }

  // ① 立即应用（同步）
  let saved = null;
  try { saved = localStorage.getItem(KEY); } catch (e) {}
  apply(saved === 'dark' ? 'dark' : 'light');

  // ② 暴露给 nav.js：挂载 click 事件 + 首次刷新图标
  window.initThemeButton = function ()
  {
    const btn = document.getElementById('theme-toggle');
    if (!btn) return;
    btn.addEventListener('click', function ()
    {
      const next = current() === 'dark' ? 'light' : 'dark';
      try { localStorage.setItem(KEY, next); } catch (e) {}
      apply(next);
    });
    apply(current());
  };
})();
