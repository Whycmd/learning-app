// ============================================================================
// md-render.js — 本地 Markdown 渲染（marked 解析 → 白名单消毒）
// 依赖：/static/marked.min.js（marked v12.0.2，MIT）
// 降级：marked 不可用时按纯文本换行渲染，不白屏
// ============================================================================
(function ()
{
  'use strict';

  var ALLOWED_TAGS = {
    P: 1, BR: 1, H1: 1, H2: 1, H3: 1, H4: 1, H5: 1, H6: 1,
    STRONG: 1, B: 1, EM: 1, I: 1, U: 1, DEL: 1, S: 1,
    UL: 1, OL: 1, LI: 1, BLOCKQUOTE: 1, CODE: 1, PRE: 1, HR: 1,
    A: 1, TABLE: 1, THEAD: 1, TBODY: 1, TR: 1, TH: 1, TD: 1
  };

  // 危险标签整段移除（含其文本内容）
  var DROP_TAGS = {
    SCRIPT: 1, STYLE: 1, IFRAME: 1, OBJECT: 1, EMBED: 1, LINK: 1, META: 1,
    FORM: 1, INPUT: 1, BUTTON: 1, TEXTAREA: 1, SELECT: 1, OPTION: 1,
    IMG: 1, SVG: 1, MATH: 1, VIDEO: 1, AUDIO: 1, SOURCE: 1, CANVAS: 1
  };

  function escapeHtml(s)
  {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function (c)
    {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c];
    });
  }

  function safeHref(raw)
  {
    var v = String(raw || '').replace(/[\u0000-\u0020]/g, '');
    if (!v) return false;
    if (/^(https?:|mailto:|#|\/|\.{0,2}\/)/i.test(v)) return true;
    // 无协议相对路径（如 pages/x.html）允许；其余自定义协议（javascript:/data: 等）拒绝
    return !/^[a-z][a-z0-9+.-]*:/i.test(v);
  }

  function sanitize(html)
  {
    var tpl = document.createElement('template');
    tpl.innerHTML = html;

    function clean(parent)
    {
      var children = Array.prototype.slice.call(parent.childNodes);
      for (var i = 0; i < children.length; i++)
      {
        var node = children[i];
        if (node.nodeType !== 1) continue;

        var tag = node.tagName;
        if (DROP_TAGS[tag])
        {
          parent.removeChild(node);
          continue;
        }
        if (!ALLOWED_TAGS[tag])
        {
          // 未知标签：先清理其子树，再解包保留文本
          clean(node);
          while (node.firstChild) parent.insertBefore(node.firstChild, node);
          parent.removeChild(node);
          continue;
        }

        // 白名单标签：仅保留安全属性，移除全部 on* 事件
        var attrs = Array.prototype.slice.call(node.attributes);
        for (var j = 0; j < attrs.length; j++)
        {
          var name = attrs[j].name.toLowerCase();
          var keep = false;
          if (tag === 'A') keep = (name === 'href' || name === 'title');
          else if (tag === 'TH' || tag === 'TD') keep = (name === 'colspan' || name === 'rowspan');
          if (!keep || name.indexOf('on') === 0) node.removeAttribute(attrs[j].name);
        }

        if (tag === 'A')
        {
          var href = node.getAttribute('href');
          if (!href || !safeHref(href)) node.removeAttribute('href');
          else
          {
            node.setAttribute('target', '_blank');
            node.setAttribute('rel', 'noopener noreferrer');
          }
        }
        clean(node);
      }
    }

    clean(tpl.content);
    var holder = document.createElement('div');
    holder.appendChild(tpl.content);
    return holder.innerHTML;
  }

  // 渲染入口：返回已消毒的 HTML 字符串
  window.renderMarkdown = function (md)
  {
    if (!md) return '';
    var src = String(md);
    if (!window.marked || typeof window.marked.parse !== 'function')
      return escapeHtml(src).replace(/\n/g, '<br>');
    try
    {
      return sanitize(window.marked.parse(src));
    }
    catch (e)
    {
      return escapeHtml(src).replace(/\n/g, '<br>');
    }
  };
})();
