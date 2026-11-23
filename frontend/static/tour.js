// tour.js — v1.5.3-T003 页面内一次性小导览（复用 components.css 的 .onboard-* 样式）
// 用法：Tour.init({ key: 'eng_tour_xxx_v1', steps: [{ target: '#id', title, text }] })
// 规则：仅首次进入自动播放；URL 加 ?tour=1 可重放；Esc 或「跳过」关闭；记忆存本机 localStorage。
(function ()
{
  'use strict';

  var state = { root: null, steps: [], i: 0, key: '', replay: false, timer: 0 };

  function read(key)
  {
    try { return localStorage.getItem(key) === '1'; } catch (e) { return false; }
  }

  function save(key)
  {
    try { localStorage.setItem(key, '1'); } catch (e) {}
  }

  // 目标不可见（隐藏面板 / 尚未渲染）时按缺失处理，卡片居中全屏
  function targetEl(sel)
  {
    var el = sel ? document.querySelector(sel) : null;
    if (!el) return null;
    var r = el.getBoundingClientRect();
    if (!r.width || !r.height) return null;
    return el;
  }

  function start(replay)
  {
    state.replay = !!replay;
    state.i = 0;
    var root = document.createElement('div');
    root.className = 'onboard';
    root.setAttribute('role', 'dialog');
    root.setAttribute('aria-modal', 'true');
    root.setAttribute('aria-label', '页面导览');
    root.innerHTML = '<div class="onboard-spot"></div>' +
      '<div class="onboard-card">' +
        '<div class="onboard-card__head"><span class="onboard-card__count"></span><span class="onboard-card__dots"></span></div>' +
        '<div class="onboard-card__title"></div>' +
        '<p class="onboard-card__text"></p>' +
        '<div class="onboard-card__acts"></div>' +
      '</div>';
    document.body.appendChild(root);
    state.root = root;

    root.addEventListener('click', function (e)
    {
      var b = e.target.closest('[data-tour-act]');
      if (!b) return;
      var act = b.getAttribute('data-tour-act');
      if (act === 'close') return close();
      if (act === 'prev' && state.i > 0) { state.i--; render(); }
      if (act === 'next' && state.i < state.steps.length - 1) { state.i++; render(); }
    });
    document.addEventListener('keydown', onKey);
    window.addEventListener('resize', layout);
    window.addEventListener('scroll', layout, true);
    // 目标内容异步渲染（骨架 → 列表）后自动重新定位，5 秒后停止
    state.timer = setInterval(layout, 300);
    setTimeout(function () { if (state.timer) { clearInterval(state.timer); state.timer = 0; } }, 5000);
    render();
  }

  function onKey(e) { if (e.key === 'Escape') close(); }

  function close()
  {
    if (!state.root) return;
    if (state.timer) { clearInterval(state.timer); state.timer = 0; }
    state.root.remove();
    state.root = null;
    document.removeEventListener('keydown', onKey);
    window.removeEventListener('resize', layout);
    window.removeEventListener('scroll', layout, true);
    if (!state.replay) save(state.key);
  }

  function render()
  {
    var s = state.steps[state.i];
    var last = state.i === state.steps.length - 1;
    var root = state.root;
    root.querySelector('.onboard-card__count').textContent = (state.i + 1) + ' / ' + state.steps.length;
    root.querySelector('.onboard-card__dots').innerHTML = state.steps.map(function (_, i)
    {
      return '<span class="onboard-dot' + (i <= state.i ? ' is-on' : '') + '"></span>';
    }).join('');
    root.querySelector('.onboard-card__title').textContent = s.title;
    root.querySelector('.onboard-card__text').textContent = s.text;
    var acts = [];
    if (state.i > 0) acts.push('<button class="btn btn-secondary" type="button" data-tour-act="prev">上一步</button>');
    acts.push(last
      ? '<button class="btn btn-primary" type="button" data-tour-act="close">知道了</button>'
      : '<button class="btn btn-primary" type="button" data-tour-act="next">下一步</button>' +
        '<button class="btn btn-secondary" type="button" data-tour-act="close">跳过</button>');
    root.querySelector('.onboard-card__acts').innerHTML = acts.join('');
    layout();
  }

  function layout()
  {
    if (!state.root) return;
    var mobile = window.matchMedia('(max-width: 640px)').matches;
    var spot = state.root.querySelector('.onboard-spot');
    var card = state.root.querySelector('.onboard-card');
    var el = targetEl(state.steps[state.i].target);

    if (!el)   // 目标缺失：全屏遮罩，卡片居中（桌面）/ 贴底（移动）
    {
      spot.classList.add('is-full');
      spot.style.left = '0'; spot.style.top = '0';
      spot.style.width = '100vw'; spot.style.height = '100vh';
      if (!mobile)
      {
        card.style.transform = '';
        card.style.left = '50%'; card.style.top = '50%';
        card.style.transform = 'translate(-50%,-50%)';
      }
      return;
    }

    spot.classList.remove('is-full');
    var r0 = el.getBoundingClientRect();
    if (r0.top < 0 || r0.bottom > window.innerHeight - 60)
      window.scrollTo(0, Math.max(0, window.scrollY + r0.top - window.innerHeight * 0.4 + r0.height / 2));

    var r = el.getBoundingClientRect();
    var pad = 6;
    spot.style.left = (r.left - pad) + 'px';
    spot.style.top = (r.top - pad) + 'px';
    spot.style.width = (r.width + pad * 2) + 'px';
    spot.style.height = (r.height + pad * 2) + 'px';

    if (mobile) { card.style.left = ''; card.style.top = ''; card.style.transform = ''; return; }

    var cw = card.offsetWidth, ch = card.offsetHeight;
    var left = r.right + pad + 16;
    if (left + cw > window.innerWidth - 16) left = r.left - cw - pad - 16;
    if (left < 16) left = Math.max(16, Math.min((window.innerWidth - cw) / 2, window.innerWidth - cw - 16));
    var top = r.top + r.height / 2 - ch / 2;
    top = Math.max(16, Math.min(top, window.innerHeight - ch - 16));
    card.style.transform = '';
    card.style.left = left + 'px';
    card.style.top = top + 'px';
  }

  function init(cfg)
  {
    if (!cfg || !cfg.key || !cfg.steps || !cfg.steps.length) return;
    state.key = cfg.key;
    state.steps = cfg.steps;
    var replay = new URLSearchParams(location.search).get('tour') === '1';
    if (!replay && read(cfg.key)) return;
    if (replay) { try { history.replaceState(null, '', location.pathname); } catch (e) {} }
    start(replay);
  }

  window.Tour = { init: init };
})();
