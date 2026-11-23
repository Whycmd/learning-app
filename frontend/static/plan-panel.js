// 计划面板 · 共享模块（v1.5.1-T001）
// 单源双形态：桌面右侧抽屉 / 移动端全屏上滑；plan.html 以整页形态挂载
// 依赖接口：GET /api/goals、GET /api/treasury、POST /api/treasury/claim-week、
//           POST /api/treasury/weekly-goal、POST /api/treasury/next-week-tasks、
//           POST /api/goals、PUT /api/goals/:id、DELETE /api/goals/:id
(function () {
  'use strict';

  var state = {
    goals: [],
    plans: [],
    week: null,
    metrics: [],
    suggestions: [],
    newStep: 1,
    editingId: 0,
    targetEdit: false,
    targetTouched: false,
    nextOpen: false,
    newOpen: false,
    parsed: null,
    form: {},
    msg: '',
    msgType: ''
  };
  var root = null;
  var isPage = false;

  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
    });
  }

  function api(path, method, body) {
    var opts = { method: method || 'GET', cache: 'no-store', headers: {} };
    if (body != null) {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(body);
    }
    return fetch(path, opts).then(function (r) {
      return r.json().then(function (j) {
        if (!r.ok || j.ok === false) throw new Error(j.error || ('请求失败 ' + r.status));
        return j;
      });
    });
  }

  function goalById(id) {
    for (var i = 0; i < state.goals.length; i++) {
      if (state.goals[i].id === id) return state.goals[i];
    }
    return null;
  }

  function metricName(key) {
    for (var i = 0; i < state.metrics.length; i++) {
      if (state.metrics[i].metric_key === key) return state.metrics[i].name;
    }
    var fallback = { review_items: '复习', new_words: '学习新词', quiz_count: '完成小测' };
    return fallback[key] || key;
  }

  function metricUnit(key) {
    for (var i = 0; i < state.metrics.length; i++) {
      if (state.metrics[i].metric_key === key) return state.metrics[i].unit || '';
    }
    return '';
  }

  function suggestedTarget(key) {
    for (var i = 0; i < state.suggestions.length; i++) {
      if (state.suggestions[i].metric_key === key) return state.suggestions[i].suggested || 0;
    }
    return 0;
  }

  function statusBadge(goal) {
    if (goal.is_current) return '<span class="plan-badge plan-badge--current">正在执行</span>';
    if (goal.expired_days > 0) {
      return '<span class="plan-badge plan-badge--expired">已到期 ' + goal.expired_days + ' 天</span>';
    }
    return '';
  }

  function metaLine(goal) {
    var wl = goal.wordlist_id > 0 ? ('词表 #' + goal.wordlist_id) : '未绑定词表';
    var parts = [
      '有效期至 ' + esc(goal.deadline),
      '每天 ' + goal.daily_minutes + ' 分钟',
      '每天 ' + goal.daily_new_words + ' 个新词',
      wl
    ];
    return parts.join(' · ');
  }

  function currentPlan() {
    for (var i = 0; i < state.plans.length; i++) {
      if (state.plans[i].is_current) return state.plans[i];
    }
    return state.plans.length ? state.plans[0] : null;
  }

  function planByGoal(goalId) {
    for (var i = 0; i < state.plans.length; i++) {
      if (state.plans[i].goal_id === goalId) return state.plans[i];
    }
    return null;
  }

  function metricDesc(key) {
    var map = {
      review_items: '巩固已学知识点',
      new_words: '每天认识新词',
      quiz_count: '用小测检验掌握'
    };
    return map[key] || '按真实学习事件累计';
  }

  function metricIcon(key) {
    if (key === 'new_words') {
      return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 5.5A2.5 2.5 0 0 1 6.5 3H20v15H6.5A2.5 2.5 0 0 0 4 20.5z"/><path d="M8 7h8M8 11h5"/></svg>';
    }
    if (key === 'quiz_count') {
      return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="4" y="3" width="16" height="18" rx="2"/><path d="M8 11l2.5 2.5L16 8"/><path d="M8 17h6"/></svg>';
    }
    return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 12a8 8 0 1 1-2.3-5.6"/><path d="M20 4v4h-4"/></svg>';
  }

  function renderTasks(plan, editable) {
    if (!plan || !plan.tasks || !plan.tasks.length) return '';
    var html = '';
    for (var i = 0; i < plan.tasks.length; i++) {
      var t = plan.tasks[i];
      var target = Math.max(1, t.target || 1);
      var value = Math.min(t.progress || 0, target);
      var foot = (t.achieved ? '<span>' + (t.claimed ? '已领取' : '待领取') + '</span>' : '') +
        (editable && state.targetEdit
          ? '<label class="plan-task__num">目标 <input type="number" min="1" data-target="' +
            esc(t.metric_key) + '" value="' + target + '"></label>'
          : '');
      html += '<div class="plan-task">' +
        '<div class="plan-task__top">' +
          '<span class="plan-task__name">' + esc(t.metric_name) + '</span>' +
          '<span class="plan-task__num' + (t.achieved ? ' is-done' : '') + '">' +
            (t.progress || 0) + ' / ' + target + ' ' + esc(t.unit || '') +
          '</span>' +
        '</div>' +
        '<progress class="plan-bar" value="' + value + '" max="' + target + '"></progress>' +
        (foot ? '<div class="plan-task__foot">' + foot + '</div>' : '') +
      '</div>';
    }
    return html;
  }

  function renderCurrent() {
    var plan = currentPlan();
    var goal = plan ? goalById(plan.goal_id) : null;
    if (!goal) {
      return '<p class="plan-empty">尚未设定课规。设定后，圣殿会为你订下每周课规。</p>' +
             '<div class="plan-actions"><button class="btn btn-primary" type="button" data-new-toggle>设定课规</button></div>';
    }
    var allDone = plan.all_done;
    var html = '<div class="plan-card">' +
      '<div class="plan-card__top">' +
        '<span class="plan-card__name">' + esc(goal.name) + '</span>' +
        statusBadge(goal) +
        (allDone ? '<span class="plan-badge">本周已达成</span>' : '') +
      '</div>' +
      '<div class="plan-card__meta">' + metaLine(goal) + '</div>' +
      '<div class="plan-section-title">本周进度</div>' +
      renderTasks(plan, true) +
      '<div class="plan-actions">' +
        (state.targetEdit
          ? '<button class="btn btn-primary" type="button" data-task-save="' + plan.goal_id + '">保存目标</button>' +
            '<button class="btn btn-secondary" type="button" data-target-cancel>取消修改</button>'
          : '<button class="btn btn-secondary" type="button" data-target-edit>调整目标</button>' +
            '<button class="btn btn-secondary" type="button" data-next-toggle>提前定下周</button>') +
      '</div>' +
      '<p class="plan-msg">圣殿为你订下每周课规，钟响达成，即落印为证。未达成不惩罚。</p>' +
    '</div>';

    if (state.nextOpen) html += renderNextWeekForm(plan);
    return html;
  }

  function renderNextWeekForm(plan) {
    var rows = '';
    for (var i = 0; i < plan.tasks.length; i++) {
      var t = plan.tasks[i];
      rows += '<label class="plan-field"><span>' + esc(t.metric_name) + '（' + esc(t.unit || '') + '）</span>' +
        '<input type="number" min="1" data-next-target="' + esc(t.metric_key) + '" value="' + Math.max(1, t.target) + '"></label>';
    }
    return '<div class="plan-form">' +
      '<h4 class="plan-form__title">提前定下周</h4>' +
      '<div class="plan-grid">' + rows + '</div>' +
      '<div class="plan-actions">' +
        '<button class="btn btn-primary" type="button" data-next-save="' + esc(plan.goal_id) + '">保存下周目标</button>' +
        '<button class="btn btn-secondary" type="button" data-next-toggle>取消</button>' +
      '</div>' +
      '<p class="plan-msg">下周一生效，本周进度不受影响。</p>' +
    '</div>';
  }

  function renderOtherGoals() {
    var html = '<div class="plan-section-title">其他课规</div>';
    var list = [];
    for (var i = 0; i < state.goals.length; i++) {
      if (!state.goals[i].is_current) list.push(state.goals[i]);
    }
    if (!list.length) {
      html += '<p class="plan-empty">暂无其他课规。</p>';
    }
    for (var j = 0; j < list.length; j++) {
      var g = list[j];
      var p = planByGoal(g.id);
      html += '<div class="plan-other">' +
        '<div class="plan-other__head">' +
          '<span class="plan-other__name">' + esc(g.name) + '</span>' +
          statusBadge(g) +
        '</div>' +
        '<div class="plan-other__meta">' + metaLine(g) + '</div>' +
        (p
          ? renderTasks(p, false) + '<p class="plan-other__reward">本周已得 ' + (p.reward ? p.reward.earned : 0) + ' 金桂' +
              (p.reward && p.reward.claimable > 0 ? ' · 可领 ' + p.reward.claimable + ' 金桂' : '') + '</p>'
          : '<p class="plan-other__idle">本周未启用，暂不记进度</p>') +
        '<div class="plan-ops">' +
          '<button class="btn btn-secondary u-btn-sm" type="button" data-goal-activate="' + g.id + '">设为正在执行</button>' +
          '<button class="btn btn-secondary u-btn-sm" type="button" data-goal-edit="' + g.id + '">修改</button>' +
          '<button class="btn btn-secondary u-btn-sm plan-del" type="button" data-goal-del="' + g.id + '">删除</button>' +
        '</div>' +
        (state.editingId === g.id ? renderEditForm(g) : '') +
      '</div>';
    }
    return html;
  }

  function renderEditForm(g) {
    return '<div class="plan-form">' +
      '<h4 class="plan-form__title">修改课规</h4>' +
      '<div class="plan-grid">' +
        '<label class="plan-field"><span>课规名称</span><input data-edit="name" value="' + esc(g.name) + '"></label>' +
        '<label class="plan-field"><span>有效期至</span><input type="date" data-edit="deadline" value="' + esc(g.deadline) + '"></label>' +
        '<label class="plan-field"><span>每天可用分钟</span><input type="number" min="1" data-edit="daily_minutes" value="' + g.daily_minutes + '"></label>' +
        '<label class="plan-field"><span>每天学习新词个数</span><input type="number" min="0" data-edit="daily_new_words" value="' + g.daily_new_words + '"></label>' +
        '<label class="plan-field"><span>绑定词表编号（0=不绑定）</span><input type="number" min="0" data-edit="wordlist_id" value="' + g.wordlist_id + '"></label>' +
      '</div>' +
      '<div class="plan-actions">' +
        '<button class="btn btn-primary" type="button" data-goal-save-edit="' + g.id + '">保存修改</button>' +
        '<button class="btn btn-secondary" type="button" data-goal-edit="' + g.id + '">取消</button>' +
      '</div>' +
    '</div>';
  }

  function renderNewForm() {
    var p = state.form || {};
    var metrics = state.metrics.length ? state.metrics
      : [{ metric_key: 'review_items', name: '复习' }, { metric_key: 'new_words', name: '学习新词' }, { metric_key: 'quiz_count', name: '完成小测' }];
    var picked = p.goal_type || 'review_items';
    var suggested = suggestedTarget(picked);
    var picks = '';
    for (var i = 0; i < metrics.length; i++) {
      var mk = metrics[i].metric_key;
      picks += '<button type="button" class="plan-pick' + (mk === picked ? ' is-on' : '') +
        '" data-new-pick="' + esc(mk) + '">' + metricIcon(mk) +
        '<span class="plan-pick__name">' + esc(metrics[i].name) + '</span>' +
        '<span class="plan-pick__desc">' + esc(metricDesc(mk)) + '</span>' +
      '</button>';
    }
    var labels = ['选方向', '定目标', '排节奏'];
    var steps = '';
    for (var s = 1; s <= 3; s++) {
      steps += '<div class="plan-steps__item' +
        (state.newStep === s ? ' is-on' : (state.newStep > s ? ' is-done' : '')) + '">' +
        '<span class="plan-steps__no">' + s + '</span>' +
        '<span class="plan-steps__label">' + labels[s - 1] + '</span>' +
      '</div>';
    }
    return '<div class="plan-form plan-wizard">' +
      '<h4 class="plan-form__title">设定新课规</h4>' +
      '<div class="plan-steps">' + steps + '</div>' +
      '<div class="plan-step' + (state.newStep === 1 ? ' active' : '') + '">' +
        '<p class="plan-step__no">第 1 步 / 共 3 步 · 你主要想练什么？</p>' +
        '<div class="plan-picks">' + picks + '</div>' +
        '<label class="plan-field plan-field--wide"><span>也可以直接写一句（例如：两周内复习 60 条）</span>' +
          '<textarea data-new="raw_text" placeholder="两周内复习 60 条">' + esc(p.raw_text || '') + '</textarea></label>' +
        '<div class="plan-actions"><button class="btn btn-secondary u-btn-sm" type="button" data-parse>解析这句话</button></div>' +
      '</div>' +
      '<div class="plan-step' + (state.newStep === 2 ? ' active' : '') + '">' +
        '<p class="plan-step__no">第 2 步 / 共 3 步 · 想在多久内达成？</p>' +
        '<div class="plan-grid">' +
          '<label class="plan-field"><span>课规期限（天）</span><input type="number" min="1" data-new="period_days" value="' + (p.period_days || 7) + '"></label>' +
          '<label class="plan-field"><span>目标数量' + (suggested ? '（推荐 ' + suggested + '）' : '') + '</span><input type="number" min="1" data-new="target" value="' + (p.target || suggested || 30) + '"></label>' +
        '</div>' +
        '<p class="plan-form__hint">推荐值来自你近 4 周的实际完成，可继续调整。</p>' +
      '</div>' +
      '<div class="plan-step' + (state.newStep === 3 ? ' active' : '') + '">' +
        '<p class="plan-step__no">第 3 步 / 共 3 步 · 每天能投入多少时间与新词？</p>' +
        '<div class="plan-grid">' +
          '<label class="plan-field"><span>课规名称</span><input data-new="name" value="' + esc(p.name || '我的课规') + '"></label>' +
          '<label class="plan-field"><span>每天可用分钟</span><input type="number" min="1" data-new="daily_minutes" value="' + (p.daily_minutes || 20) + '"></label>' +
          '<label class="plan-field"><span>每天学习新词个数</span><input type="number" min="0" data-new="daily_new_words" value="' + (p.daily_new_words || 10) + '"></label>' +
          '<label class="plan-field"><span>绑定词表编号（0=不绑定）</span><input type="number" min="0" data-new="wordlist_id" value="' + (p.wordlist_id || 0) + '"></label>' +
        '</div>' +
      '</div>' +
      '<div class="plan-actions plan-actions--foot">' +
        (state.newStep > 1 ? '<button class="btn btn-secondary" type="button" data-step-prev>上一步</button>' : '') +
        (state.newStep < 3 ? '<button class="btn btn-primary" type="button" data-step-next>下一步</button>'
                           : '<button class="btn btn-primary" type="button" data-goal-create>保存课规</button>') +
        '<button class="btn btn-secondary" type="button" data-new-toggle>取消</button>' +
      '</div>' +
    '</div>';
  }

  function render() {
    if (!root) return;
    var cur = currentPlan();
    var claimable = 0;
    if (state.week) claimable = state.week.claimable || 0;
    var earned = state.week ? (state.week.earned || 0) : 0;

    var html = '';
    if (earned > 0 || claimable > 0) {
      html += '<p class="plan-claim">本周已领取 ' + earned + ' 金桂' +
        (claimable > 0 ? ' · 可领 ' + claimable + ' 金桂' : '') + '</p>';
    }
    if (state.msg) {
      html += '<p class="plan-msg' + (state.msgType ? ' plan-msg--' + state.msgType : '') + '">' + esc(state.msg) + '</p>';
    }
    html += renderCurrent();
    if (state.newOpen) html += renderNewForm();
    html += renderOtherGoals();
    if (cur) {
      html += '<div class="plan-actions"><button class="btn btn-secondary" type="button" data-new-toggle>设定新课规</button></div>';
    }
    root.querySelector('.plan-body').innerHTML = html;
  }

  function refresh() {
    return Promise.all([api('/api/goals'), api('/api/treasury')]).then(function (res) {
      state.goals = res[0].goals || [];
      state.plans = res[1].weekly_plans || [];
      state.week = res[1].week_reward || null;
      state.suggestions = res[1].suggestions || [];
      render();
    }).catch(function (e) {
      state.msg = '读取失败：' + e.message;
      state.msgType = 'danger';
      render();
    });
  }

  function loadMetrics() {
    return api('/api/metrics').then(function (r) {
      state.metrics = r.metrics || [];
    }).catch(function () { state.metrics = []; });
  }

  function ensureRoot() {
    if (root) return root;
    var host = document.getElementById('plan-panel-host');
    isPage = host && host.getAttribute('data-plan-page') === '1';
    root = document.createElement('div');
    root.className = 'plan-overlay' + (isPage ? ' is-page' : '');
    root.id = 'plan-panel';
    root.innerHTML =
      '<div class="plan-scrim" data-plan-close></div>' +
      '<aside class="plan-panel" role="dialog" aria-modal="true" aria-labelledby="plan-panel-title">' +
        '<div class="plan-head">' +
          '<h3 id="plan-panel-title">本周课规</h3>' +
          '<button class="plan-close" type="button" data-plan-close aria-label="关闭">×</button>' +
        '</div>' +
        '<div class="plan-body"></div>' +
      '</aside>';
    (host || document.body).appendChild(root);
    if (isPage) root.classList.add('open');
    root.addEventListener('click', onClick);
    root.addEventListener('input', function (e) {
      var el = e.target.closest('[data-new]');
      if (el && state.form) {
        var key = el.getAttribute('data-new');
        state.form[key] = el.value;
        if (key === 'target') state.targetTouched = true;
      }
    });
    return root;
  }

  function setMsg(text, type) {
    state.msg = text || '';
    state.msgType = type || '';
    render();
  }

  function valueOf(selector) {
    var el = root.querySelector(selector);
    return el ? el.value : '';
  }

  function onClick(e) {
    var t = e.target.closest('[data-plan-close],[data-goal-activate],[data-goal-edit],[data-goal-del],[data-goal-save-edit],[data-task-save],[data-target-edit],[data-target-cancel],[data-next-toggle],[data-next-save],[data-new-toggle],[data-step-next],[data-step-prev],[data-goal-create],[data-parse],[data-new-pick]');
    if (!t) return;

    if (t.hasAttribute('data-plan-close')) { close(); return; }

    if (t.hasAttribute('data-target-edit')) {
      state.targetEdit = true;
      state.msg = '';
      render();
      return;
    }

    if (t.hasAttribute('data-target-cancel')) {
      state.targetEdit = false;
      state.msg = '';
      render();
      return;
    }

    if (t.hasAttribute('data-new-pick')) {
      var pickedKey = t.getAttribute('data-new-pick');
      state.form = state.form || {};
      state.form.goal_type = pickedKey;
      if (!state.targetTouched) state.form.target = suggestedTarget(pickedKey) || '';
      render();
      return;
    }

    if (t.hasAttribute('data-goal-activate')) {
      var aid = parseInt(t.getAttribute('data-goal-activate'), 10);
      api('/api/goals/' + aid, 'PUT', { is_current: true }).then(function () {
        state.editingId = 0;
        setMsg('已设为正在执行。', 'success');
        return refresh();
      }).catch(function (err) { setMsg(err.message, 'danger'); });
      return;
    }

    if (t.hasAttribute('data-goal-edit')) {
      var eid = parseInt(t.getAttribute('data-goal-edit'), 10);
      state.editingId = state.editingId === eid ? 0 : eid;
      render();
      return;
    }

    if (t.hasAttribute('data-goal-del')) {
      var did = parseInt(t.getAttribute('data-goal-del'), 10);
      if (!window.confirm('删除后不可恢复，确认删除？')) return;
      api('/api/goals/' + did, 'DELETE').then(function () {
        setMsg('课规已删除。', 'success');
        return refresh();
      }).catch(function (err) { setMsg(err.message, 'danger'); });
      return;
    }

    if (t.hasAttribute('data-goal-save-edit')) {
      var sid = parseInt(t.getAttribute('data-goal-save-edit'), 10);
      var body = {
        name: valueOf('[data-edit="name"]'),
        deadline: valueOf('[data-edit="deadline"]'),
        daily_minutes: parseInt(valueOf('[data-edit="daily_minutes"]'), 10) || 20,
        daily_new_words: parseInt(valueOf('[data-edit="daily_new_words"]'), 10) || 0,
        wordlist_id: parseInt(valueOf('[data-edit="wordlist_id"]'), 10) || 0
      };
      api('/api/goals/' + sid, 'PUT', body).then(function () {
        state.editingId = 0;
        setMsg('课规已更新。', 'success');
        return refresh();
      }).catch(function (err) { setMsg(err.message, 'danger'); });
      return;
    }

    if (t.hasAttribute('data-task-save')) {
      var gid = parseInt(t.getAttribute('data-task-save'), 10);
      var curPlan = currentPlan();
      var inputs = root.querySelectorAll('[data-target]');
      var changes = [];
      var invalid = '';
      inputs.forEach(function (inp) {
        var metric = inp.getAttribute('data-target');
        var target = parseInt(inp.value, 10);
        if (!target || target < 1) { invalid = '目标数量需大于 0'; return; }
        var old = 0;
        if (curPlan && curPlan.tasks) {
          for (var i = 0; i < curPlan.tasks.length; i++) {
            if (curPlan.tasks[i].metric_key === metric) old = curPlan.tasks[i].target;
          }
        }
        if (target !== old) {
          changes.push(metricName(metric) + ' ' + old + ' → ' + target + ' ' + metricUnit(metric));
        }
      });
      if (invalid) { setMsg(invalid, 'danger'); return; }
      if (!changes.length) { setMsg('目标数量没有变化，无需保存。'); return; }
      if (!window.confirm('确认修改本周目标？\n\n' + changes.join('\n'))) return;

      var seq = Promise.resolve();
      inputs.forEach(function (inp) {
        var metric = inp.getAttribute('data-target');
        var target = parseInt(inp.value, 10);
        seq = seq.then(function () {
          return api('/api/treasury/weekly-goal', 'POST', { goal_id: gid, metric_key: metric, target: target });
        });
      });
      seq.then(function () {
        state.targetEdit = false;
        setMsg('目标已保存。', 'success');
        return refresh();
      }).catch(function (err) { setMsg(err.message, 'danger'); });
      return;
    }

    if (t.hasAttribute('data-next-toggle')) {
      state.nextOpen = !state.nextOpen;
      render();
      return;
    }

    if (t.hasAttribute('data-next-save')) {
      var ngid = parseInt(t.getAttribute('data-next-save'), 10);
      var basePlan = currentPlan();
      var tasks = [];
      var nextChanges = [];
      var badTarget = false;
      root.querySelectorAll('[data-next-target]').forEach(function (inp) {
        var metric = inp.getAttribute('data-next-target');
        var target = parseInt(inp.value, 10);
        if (!target || target < 1) { badTarget = true; return; }
        tasks.push({ metric_key: metric, target: target });
        var old = 0;
        if (basePlan && basePlan.tasks) {
          for (var i = 0; i < basePlan.tasks.length; i++) {
            if (basePlan.tasks[i].metric_key === metric) old = basePlan.tasks[i].target;
          }
        }
        if (target !== old) {
          nextChanges.push(metricName(metric) + ' ' + old + ' → ' + target + ' ' + metricUnit(metric));
        }
      });
      if (badTarget) { setMsg('目标数量需大于 0', 'danger'); return; }
      if (!nextChanges.length) { setMsg('下周目标与本周一致，无需保存。'); return; }
      if (!window.confirm('确认保存下周目标？\n\n' + nextChanges.join('\n'))) return;

      api('/api/treasury/next-week-tasks', 'POST', { goal_id: ngid, tasks: tasks }).then(function () {
        state.nextOpen = false;
        setMsg('下周目标已保存，下周一生效。', 'success');
        return refresh();
      }).catch(function (err) { setMsg(err.message, 'danger'); });
      return;
    }

    if (t.hasAttribute('data-new-toggle')) {
      state.newOpen = !state.newOpen;
      state.newStep = 1;
      state.parsed = null;
      state.targetTouched = false;
      state.form = state.newOpen
        ? { goal_type: 'review_items', target: suggestedTarget('review_items') || '' }
        : {};
      state.msg = '';
      render();
      return;
    }

    if (t.hasAttribute('data-step-next')) {
      if (state.newStep < 3) state.newStep++;
      render();
      return;
    }

    if (t.hasAttribute('data-step-prev')) {
      if (state.newStep > 1) state.newStep--;
      render();
      return;
    }

    if (t.hasAttribute('data-parse')) {
      var text = valueOf('[data-new="raw_text"]');
      if (!text) { setMsg('先写一句你的课规。', 'danger'); return; }
      api('/api/goals/parse', 'POST', { text: text }).then(function (p) {
        state.parsed = p;
        state.form = {
          raw_text: text,
          goal_type: p.goal_type,
          name: p.name,
          period_days: p.period_days,
          target: p.target,
          daily_minutes: p.daily_minutes,
          daily_new_words: p.daily_new_words,
          wordlist_id: 0
        };
        state.newStep = 2;
        render();
        setMsg('已按这句话填好，可继续调整。', 'success');
      }).catch(function (err) { setMsg(err.message, 'danger'); });
      return;
    }

    if (t.hasAttribute('data-goal-create')) {
      var f = state.form || {};
      var body = {
        name: f.name || '我的课规',
        goal_type: f.goal_type || (state.parsed && state.parsed.goal_type) || 'review_items',
        period_days: parseInt(f.period_days, 10) || 7,
        target: parseInt(f.target, 10) || 30,
        daily_minutes: parseInt(f.daily_minutes, 10) || 20,
        daily_new_words: parseInt(f.daily_new_words, 10) || 0,
        wordlist_id: parseInt(f.wordlist_id, 10) || 0,
        target_desc: '',
        parse_source: state.parsed ? 'template' : 'manual'
      };
      api('/api/goals', 'POST', body).then(function () {
        state.newOpen = false;
        state.newStep = 1;
        state.parsed = null;
        state.targetEdit = false;
        state.form = {};
        setMsg('课规已设定。', 'success');
        return refresh();
      }).catch(function (err) { setMsg(err.message, 'danger'); });
      return;
    }
  }

  function open() {
    ensureRoot();
    if (!isPage) root.classList.add('open');
    document.documentElement.classList.add('plan-lock');
    loadMetrics().then(function () {
      return refresh();
    }).then(function () {
      // 打开面板时兑现未领取的每项 5 金桂（未领取不清零）
      return api('/api/treasury/claim-week', 'POST', { trigger: 'panel' });
    }).then(function () {
      return refresh();
    }).catch(function () { /* 领取失败不阻塞面板 */ });
  }

  function close() {
    if (!root || isPage) return;
    root.classList.remove('open');
    document.documentElement.classList.remove('plan-lock');
    if (window.PlanPanel && typeof window.PlanPanel.onClose === 'function') window.PlanPanel.onClose();
  }

  function mountPage() {
    ensureRoot();
    loadMetrics().then(function () {
      return refresh();
    }).then(function () {
      return api('/api/treasury/claim-week', 'POST', { trigger: 'panel' });
    }).then(function () {
      return refresh();
    }).catch(function () {});
  }

  window.PlanPanel = {
    open: open,
    close: close,
    refresh: refresh,
    onClose: null
  };

  document.addEventListener('click', function (e) {
    if (e.target.closest('[data-goal-open]')) { e.preventDefault(); open(); }
  });

  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') close();
  });

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', function () {
      var host = document.getElementById('plan-panel-host');
      if (host && host.getAttribute('data-plan-page') === '1') mountPage();
    });
  } else {
    var host = document.getElementById('plan-panel-host');
    if (host && host.getAttribute('data-plan-page') === '1') mountPage();
  }
})();
