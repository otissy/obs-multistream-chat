// Read-only page script. It is injected into ONE chat page, in an isolated JavaScript world, and
// driven entirely by a selector pack (reader/adapters/<platform>.json).
//
// What it does: find the chat list, watch it with a MutationObserver, and for every NEW message row
// read author, text, emote images and badges, then hand them to the reader through a binding that
// only this isolated world can call.
// What it never does: click, type, scroll, focus, submit, fetch, open sockets, touch storage or
// modify the page. (.tests/chat/l2 greps this file for every DOM-mutating API and fails on a hit.)
//
// The function is stringified by lib/source.js and called with (cfg, bindingName). It must stay
// self-contained: no closures over module variables, no require().
function omscEngine(cfg, bindingName) {
  'use strict';
  var seenEls = new WeakSet();
  var queue = [];
  var seq = 0;
  var seenCount = 0;
  var recent = Object.create(null); // synthetic-id guard: author|text -> time
  var container = null;
  var observer = null;
  var foundAt = 0;
  var scanTimer = null;

  function send(obj) {
    try { window[bindingName](JSON.stringify(obj)); } catch (e) { /* binding gone: page is closing */ }
  }
  function arr(x) { return Array.isArray(x) ? x : (x ? [x] : []); }
  function qs(root, sels) {
    var list = arr(sels);
    for (var i = 0; i < list.length; i++) {
      try { var el = root.querySelector(list[i]); if (el) return el; } catch (e) { /* bad selector in a pack */ }
    }
    return null;
  }
  function qsa(root, sels) {
    var out = [];
    var list = arr(sels);
    for (var i = 0; i < list.length; i++) {
      try { var nl = root.querySelectorAll(list[i]); for (var k = 0; k < nl.length; k++) if (out.indexOf(nl[k]) < 0) out.push(nl[k]); } catch (e) { /* ignore */ }
    }
    return out;
  }
  function matches(el, sels) {
    var list = arr(sels);
    for (var i = 0; i < list.length; i++) {
      try { if (el.matches(list[i])) return true; } catch (e) { /* ignore */ }
    }
    return false;
  }
  function norm(s) { return String(s == null ? '' : s).replace(/\s+/g, ' ').trim(); }
  function attrOf(el, names) {
    var list = arr(names);
    for (var i = 0; i < list.length; i++) {
      var v = el.getAttribute && el.getAttribute(list[i]);
      if (v) return v;
    }
    return '';
  }
  function hash(s) {
    var h = 2166136261;
    for (var i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); }
    return (h >>> 0).toString(16);
  }
  function toHex(css) {
    var m = /^rgba?\(\s*(\d+)[\s,]+(\d+)[\s,]+(\d+)/i.exec(css || '');
    if (m) {
      var h = '#';
      for (var i = 1; i <= 3; i++) h += ('0' + Math.min(255, parseInt(m[i], 10)).toString(16)).slice(-2);
      return h;
    }
    return /^#[0-9a-f]{6}$/i.test(css || '') ? css : '';
  }

  // --- finding rows -------------------------------------------------------------------------
  function rowsIn(node) {
    var out = [];
    if (cfg.anchor) {
      var anchors = [];
      if (node.matches && matches(node, cfg.anchor)) anchors.push(node);
      anchors = anchors.concat(qsa(node, cfg.anchor));
      for (var i = 0; i < anchors.length; i++) {
        var row = rowOfAnchor(anchors[i], node);
        if (row && out.indexOf(row) < 0) out.push(row);
      }
      return out;
    }
    if (node.matches && matches(node, cfg.item)) out.push(node);
    var inner = qsa(node, cfg.item);
    for (var k = 0; k < inner.length; k++) if (out.indexOf(inner[k]) < 0) out.push(inner[k]);
    return out;
  }
  // a row is the largest ancestor of an anchor that still contains exactly one anchor, and never
  // reaches above the node that was added (so the first row in an empty list is not mistaken for
  // the whole list)
  function rowOfAnchor(a, boundary) {
    var el = a;
    while (el !== boundary && el.parentElement && el.parentElement !== container && el.parentElement !== document.body) {
      var count = 0;
      try { count = el.parentElement.querySelectorAll(cfg.anchor).length; } catch (e) { count = 2; }
      if (count !== 1) break;
      el = el.parentElement;
    }
    return el;
  }

  // --- reading one row ----------------------------------------------------------------------
  function readParts(root, remove) {
    var parts = [];
    function pushText(t) {
      if (!t) return;
      var last = parts[parts.length - 1];
      if (last && !last.u && last.t === 'text') last.v += t; else parts.push({ t: 'text', v: t });
    }
    function walk(n) {
      if (n.nodeType === 3) { pushText(n.nodeValue); return; }
      if (n.nodeType !== 1) return;
      var tag = n.tagName;
      if (tag === 'SCRIPT' || tag === 'STYLE' || tag === 'SVG' || tag === 'svg' || tag === 'NOSCRIPT' || tag === 'BUTTON') return;
      if (remove && remove.length && matches(n, remove)) return;
      if (cfg.emote && matches(n, cfg.emote.sel)) {
        var name = norm(attrOf(n, cfg.emote.name || ['alt', 'title', 'aria-label']));
        var url = '';
        try { url = n.currentSrc || n.src || ''; } catch (e) { url = ''; }
        if (!url) url = attrOf(n, cfg.emote.src || ['data-src', 'src']);
        parts.push({ t: 'emote', v: name || 'emote', u: url });
        return;
      }
      if (tag === 'IMG') { pushText(norm(n.getAttribute('alt') || '')); return; }
      if (tag === 'BR') { pushText(' '); return; }
      var kids = n.childNodes;
      for (var i = 0; i < kids.length; i++) walk(kids[i]);
    }
    walk(root);
    return parts;
  }

  function extract(row) {
    if (cfg.ignore && matches(row, cfg.ignore)) return null;
    var authorEl = qs(row, cfg.author && cfg.author.sel);
    var author = authorEl ? norm(authorEl.textContent) : '';
    var color = '';
    if (authorEl && cfg.author && cfg.author.color === 'inline') {
      var el = authorEl;
      for (var up = 0; up < 3 && el && !color; up++, el = el.parentElement) color = toHex(el.style && el.style.color);
    }
    var bodyRoot = null;
    var remove = arr(cfg.body && cfg.body.remove);
    if (cfg.body && cfg.body.sel) bodyRoot = qs(row, cfg.body.sel);
    if (!bodyRoot && cfg.body && cfg.body.rest) bodyRoot = row; // everything except the removed pieces
    var parts = bodyRoot ? readParts(bodyRoot, remove.concat(arr(cfg.body && cfg.body.rest ? cfg.author && cfg.author.sel : []))) : [];
    if (parts.length && parts[0].t === 'text') parts[0].v = parts[0].v.replace(/^\s+/, '');
    if (parts.length && parts[parts.length - 1].t === 'text') parts[parts.length - 1].v = parts[parts.length - 1].v.replace(/\s+$/, '');
    var hasText = false;
    for (var i = 0; i < parts.length; i++) if (parts[i].t === 'emote' || norm(parts[i].v)) hasText = true;
    if (!author || !hasText) return null;
    var badges = [];
    if (cfg.badge && cfg.badge.sel) {
      var bl = qsa(row, cfg.badge.sel);
      for (var b = 0; b < bl.length && badges.length < 6; b++) {
        var bn = norm(attrOf(bl[b], cfg.badge.name || ['alt', 'aria-label', 'title', 'type']));
        if (bn) badges.push(bn);
      }
    }
    var id = '';
    if (cfg.id && cfg.id.attrs) id = attrOf(row, cfg.id.attrs);
    if (!id && cfg.id && cfg.id.child) {
      var ce = qs(row, cfg.id.child.sel);
      if (ce) id = attrOf(ce, cfg.id.child.attrs);
    }
    var plain = '';
    for (var p = 0; p < parts.length; p++) plain += parts[p].t === 'emote' ? '[' + parts[p].v + ']' : parts[p].v;
    plain = norm(plain);
    if (!id) {
      // no id on the page: synthesise one, and ignore the same author+text re-rendered within 1.5 s
      var key = author + '\u0001' + plain;
      var now = Date.now();
      if (recent[key] && now - recent[key] < 1500) return null;
      recent[key] = now;
      id = 's' + hash(key) + '-' + (++seq);
    }
    return { id: id, author: author, color: color, parts: parts, badges: badges, t: Date.now() };
  }

  function handleRow(row, tries) {
    if (seenEls.has(row)) return;
    var msg = null;
    try { msg = extract(row); } catch (e) { msg = null; }
    if (!msg) {
      // frameworks often fill a row a moment after inserting it: look again a few times
      if (tries < 6) setTimeout(function () { handleRow(row, tries + 1); }, 120);
      return;
    }
    seenEls.add(row);
    seenCount++;
    queue.push(msg);
    if (queue.length >= 20) flush();
  }
  function flush() {
    if (!queue.length) return;
    var items = queue.splice(0, 50);
    send({ t: 'msgs', items: items });
    if (queue.length) setTimeout(flush, 50);
  }

  // --- watching -----------------------------------------------------------------------------
  function onMutations(list) {
    for (var i = 0; i < list.length; i++) {
      var m = list[i];
      if (m.type === 'childList') {
        var added = m.addedNodes;
        for (var k = 0; k < added.length; k++) {
          var n = added[k];
          if (n.nodeType !== 1) continue;
          var rows = rowsIn(n);
          for (var r = 0; r < rows.length; r++) if (!seenEls.has(rows[r])) scheduleRow(rows[r]);
        }
      }
      // text filled into a row that was inserted empty
      var tgt = m.target && m.target.nodeType === 3 ? m.target.parentElement : m.target;
      if (tgt && tgt.nodeType === 1 && tgt !== container && !cfg.anchor && tgt.closest) {
        var host = null;
        try { host = tgt.closest(arr(cfg.item).join(',')); } catch (e) { host = null; }
        if (host && !seenEls.has(host)) scheduleRow(host);
      }
    }
  }
  var scheduled = new WeakSet();
  function scheduleRow(row) {
    if (scheduled.has(row)) return;
    scheduled.add(row);
    setTimeout(function () { scheduled.delete(row); handleRow(row, 0); }, 100);
  }

  function attach(el) {
    container = el;
    foundAt = Date.now();
    // rows already on the page are history: remember them, report none
    var existing = rowsIn(el);
    for (var i = 0; i < existing.length; i++) seenEls.add(existing[i]);
    observer = new MutationObserver(onMutations);
    observer.observe(el, { childList: true, subtree: true, characterData: true });
  }

  function loginSeen() {
    var l = cfg.login || {};
    if (l.sel && qs(document, l.sel)) return true;
    if (l.text && l.text.length) {
      var t = '';
      try { t = (document.body && document.body.innerText || '').slice(0, 4000).toLowerCase(); } catch (e) { t = ''; }
      for (var i = 0; i < l.text.length; i++) if (t.indexOf(String(l.text[i]).toLowerCase()) >= 0) return true;
    }
    return false;
  }

  function tick() {
    if (container && !container.isConnected) { // the page rebuilt its list: attach to the new one
      try { observer.disconnect(); } catch (e) { /* ignore */ }
      container = null;
    }
    if (!container) {
      var el = qs(document, cfg.container);
      if (el) attach(el);
    }
    send({ t: 'tick', container: !!container, login: loginSeen(), seen: seenCount, sinceFound: container ? Date.now() - foundAt : 0 });
  }

  send({ t: 'hello', href: String(location.href).slice(0, 200) });
  scanTimer = setInterval(tick, 2000);
  setInterval(flush, 250);
  setTimeout(tick, 300);
  window.addEventListener('pagehide', function () { clearInterval(scanTimer); });
}
module.exports = { omscEngine: omscEngine };
