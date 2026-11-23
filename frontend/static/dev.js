// 开发者看板（v1.5.2-T001 DEV-INTAKE）
// 数据接口：/api/dev/summary、/api/dev/backlog(/:id)、/api/dev/bugs(/:id)（仅 admin）
// 展示名映射：dev.html 顶部 DEV_LABELS（人工修改渠道）
(function () {
  "use strict";
  var L = window.DEV_LABELS || {};
  var tab = "backlog";

  function $(id) { return document.getElementById(id); }

  function api(path, opts) {
    return fetch(path, opts).then(function (res) {
      if (res.status === 401 || res.status === 403) return { denied: true };
      return res.json().catch(function () { return {}; }).then(function (j) {
        return { status: res.status, json: j };
      });
    });
  }

  function showDenied() {
    $("dv-main").hidden = true;
    $("dv-tabs").hidden = true;
    $("dv-filter").hidden = true;
    $("dv-denied").hidden = false;
  }

  // ---------- 徽标 ----------
  function loadBadges() {
    return api("/api/dev/summary").then(function (r) {
      if (r.denied) { showDenied(); return; }
      var s = r.json || {};
      $("dv-badge-backlog").textContent = s.backlog_total || 0;
      $("dv-badge-bugs").textContent = s.bugs_total || 0;
      $("dv-badge-triage").textContent = s.triage_total || 0;
    });
  }

  // ---------- 页签 ----------
  function bindTabs() {
    $("dv-tabs").addEventListener("click", function (e) {
      var btn = e.target.closest(".dv-tab");
      if (!btn || btn.dataset.tab === tab) return;
      tab = btn.dataset.tab;
      var all = document.querySelectorAll(".dv-tab");
      for (var i = 0; i < all.length; i++) all[i].classList.toggle("is-active", all[i] === btn);
      renderFilter();
      load();
    });
  }

  // ---------- 筛选 ----------
  function sel(label, id, opts) {
    var wrap = document.createElement("label");
    wrap.innerHTML = "<span>" + label + "</span>";
    var s = document.createElement("select");
    s.id = id;
    s.innerHTML = opts.map(function (o) {
      return '<option value="' + o[0] + '">' + o[1] + "</option>";
    }).join("");
    wrap.appendChild(s);
    return wrap;
  }

  function statusOpts() {
    var m = (L.status.backlog || {});
    var out = [["", tab === "backlog" ? "全部（不含已拒绝）" : "全部"]];
    if (tab === "backlog") {
      ["new", "triage", "accepted", "planned", "shipped", "rejected"].forEach(function (k) {
        out.push([k, m[k] || k]);
      });
    } else {
      var mb = L.status.bugs || {};
      ["new", "triage", "confirmed", "fixing", "fixed", "closed", "duplicate"].forEach(function (k) {
        out.push([k, mb[k] || k]);
      });
    }
    return out;
  }

  function renderFilter() {
    var f = $("dv-filter");
    f.innerHTML = "";
    if (tab === "triage") return; // 待分拣无筛选
    f.appendChild(sel("状态", "dv-f-status", statusOpts()));
    if (tab === "backlog") {
      f.appendChild(sel("优先级", "dv-f-priority", [["", "全部"], ["P0", "P0"], ["P1", "P1"], ["P2", "P2"]]));
    } else if (tab === "bugs") {
      f.appendChild(sel("严重度", "dv-f-severity", [["", "全部"], ["blocker", "阻断"], ["major", "严重"], ["minor", "轻微"]]));
      var cm = L.category.bugs || {};
      f.appendChild(sel("分类", "dv-f-category", [["", "全部"], ["ui", cm.ui || "ui"], ["api", cm.api || "api"],
        ["data", cm.data || "data"], ["export", cm.export || "export"], ["content", cm.content || "content"]]));
    }
  }

  // ---------- 列表 ----------
  function load() {
    var list = $("dv-list");
    list.innerHTML = "";
    if (tab === "triage") {
      Promise.all([
        api("/api/dev/backlog?status=triage"),
        api("/api/dev/bugs?status=triage")
      ]).then(function (rs) {
        if (rs[0].denied) { showDenied(); return; }
        var items = [];
        ((rs[0].json || {}).items || []).forEach(function (it) { it._src = "backlog"; items.push(it); });
        ((rs[1].json || {}).items || []).forEach(function (it) { it._src = "bugs"; items.push(it); });
        items.sort(function (a, b) { return (b.last_seen_at || "").localeCompare(a.last_seen_at || ""); });
        render(items);
      });
      return;
    }
    var path = tab === "backlog" ? "/api/dev/backlog?" : "/api/dev/bugs?";
    var p = [];
    var st = $("dv-f-status"), pr = $("dv-f-priority"), sv = $("dv-f-severity"), ct = $("dv-f-category");
    if (st && st.value) p.push("status=" + encodeURIComponent(st.value));
    if (pr && pr.value) p.push("priority=" + encodeURIComponent(pr.value));
    if (sv && sv.value) p.push("severity=" + encodeURIComponent(sv.value));
    if (ct && ct.value) p.push("category=" + encodeURIComponent(ct.value));
    api(path + p.join("&")).then(function (r) {
      if (r.denied) { showDenied(); return; }
      render(((r.json || {}).items || []).map(function (it) { it._src = tab; return it; }));
    });
  }

  function render(items) {
    var list = $("dv-list");
    list.innerHTML = "";
    $("dv-empty").hidden = items.length > 0;
    items.forEach(function (it) { list.appendChild(row(it)); });
  }

  function optList(map, keys, cur) {
    return keys.map(function (k) {
      return '<option value="' + k + '"' + (k === cur ? " selected" : "") + ">" + (map[k] || k) + "</option>";
    }).join("");
  }

  function row(it) {
    var isBug = it._src === "bugs";
    var el = document.createElement("div");
    el.className = "dv-item";

    var catMap = isBug ? (L.category.bugs || {}) : (L.category.backlog || {});
    var hits = document.createElement("span");
    hits.className = "dv-item__hits";
    hits.textContent = "被提 " + (it.occurrences || 1) + " 次";

    var head = document.createElement("div");
    head.className = "dv-item__head";
    head.innerHTML = '<span class="dv-item__no">#' + (isBug ? "G" : "B") + it.id + "</span>" +
      '<span class="dv-item__title"></span>' +
      (tab === "triage" ? '<span class="dv-item__tag">' + (isBug ? "问题" : "需求") + "</span>" : "") +
      '<span class="dv-item__tag">' + (catMap[it.category] || "") + "</span>";
    head.querySelector(".dv-item__title").textContent = it.title || "";
    head.appendChild(hits);
    el.appendChild(head);

    // 编辑行
    var edit = document.createElement("div");
    edit.className = "dv-item__edit";
    var stSel = document.createElement("select");
    stSel.innerHTML = optList(isBug ? (L.status.bugs || {}) : (L.status.backlog || {}),
      isBug ? ["new", "triage", "confirmed", "fixing", "fixed", "closed", "duplicate"]
            : ["new", "triage", "accepted", "planned", "shipped", "rejected"], it.status);
    edit.appendChild(stSel);

    var lvlSel = document.createElement("select");
    if (isBug) {
      lvlSel.innerHTML = optList(L.severity || {}, ["blocker", "major", "minor"], it.severity || "minor");
    } else {
      lvlSel.innerHTML = '<option value="">未设优先级</option>' +
        optList(L.priority || {}, ["P0", "P1", "P2"], it.priority || "");
    }
    edit.appendChild(lvlSel);

    var note = document.createElement("input");
    note.className = "dv-note";
    note.placeholder = "备注";
    note.value = it.admin_note || "";
    edit.appendChild(note);

    var dup = null;
    if (isBug) {
      dup = document.createElement("input");
      dup.className = "dv-dup";
      dup.type = "number";
      dup.placeholder = "重复于#";
      dup.value = it.duplicate_of || "";
      edit.appendChild(dup);
    }

    var save = document.createElement("button");
    save.className = "dv-item__save";
    save.type = "button";
    save.textContent = "保存";
    var savedTip = document.createElement("span");
    savedTip.className = "dv-item__saved";
    edit.appendChild(save);
    edit.appendChild(savedTip);
    el.appendChild(edit);

    // 元信息 + 展开
    var meta = document.createElement("div");
    meta.className = "dv-item__meta";
    meta.innerHTML = "<span>最近 " + (it.last_seen_at || "-") + "</span>" +
      "<span>首次 " + (it.created_at || "-") + "</span>" +
      "<span>用户 " + (it.user_id || 0) + "</span>";
    var toggle = document.createElement("button");
    toggle.className = "dv-item__toggle";
    toggle.type = "button";
    toggle.textContent = "展开";
    meta.appendChild(toggle);
    el.appendChild(meta);

    var detail = document.createElement("div");
    detail.className = "dv-item__detail";
    detail.hidden = true;
    detail.innerHTML = "<p></p><p>分类：" + (catMap[it.category] || "-") +
      " · 置信度 " + (typeof it.confidence === "number" ? it.confidence.toFixed(2) : "-") +
      (isBug && it.page_path ? " · 页面 " + it.page_path : "") + "</p>";
    detail.querySelector("p").textContent = it.detail || "（无详情）";
    el.appendChild(detail);

    toggle.addEventListener("click", function () {
      detail.hidden = !detail.hidden;
      toggle.textContent = detail.hidden ? "展开" : "收起";
    });

    save.addEventListener("click", function () {
      save.disabled = true;
      savedTip.textContent = "";
      var body = { status: stSel.value, admin_note: note.value };
      body[isBug ? "severity" : "priority"] = lvlSel.value;
      if (dup && dup.value) body.duplicate_of = Number(dup.value);
      api("/api/dev/" + (isBug ? "bugs" : "backlog") + "/" + it.id, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body)
      }).then(function (r) {
        save.disabled = false;
        if (r.denied) { showDenied(); return; }
        if (r.status === 200) {
          savedTip.textContent = "已保存";
          it.status = body.status;
          loadBadges();
        } else {
          savedTip.textContent = (r.json && r.json.error) || "保存失败";
        }
        setTimeout(function () { savedTip.textContent = ""; }, 2500);
      }).catch(function () { save.disabled = false; savedTip.textContent = "网络异常"; });
    });

    return el;
  }

  document.addEventListener("DOMContentLoaded", function () {
    $("dv-filter").addEventListener("change", load);
    loadBadges().then(function () {
      if (!$("dv-denied").hidden) return;
      bindTabs();
      renderFilter();
      load();
    });
  });
})();
