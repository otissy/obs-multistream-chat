(function () {
  'use strict';
  // Merged chat overlay. Only talks to this page's own origin (/chat/events). Every string that
  // came from a web page is untrusted: it only ever goes through textContent / createTextNode,
  // never innerHTML. The platform id is matched against a fixed list before it is used in a URL,
  // the name colour must be #rrggbb, and an emote image must be https (the server has already
  // checked the host against the platform's own CDN; the CSP of this page enforces it again).
  var PLATFORMS = ['twitch', 'youtube', 'kick', 'tiktok', 'x'];
  var params = new URLSearchParams(location.search);
  var body = document.body;
  var chat = document.getElementById('chat');

  function pick(v, allowed, fallback) { return allowed.indexOf(v) >= 0 ? v : fallback; }
  function num(v, lo, hi, def) {
    var n = parseInt(v, 10);
    return (n >= lo && n <= hi) ? n : def;
  }

  var theme = pick(params.get('theme'), ['dark', 'light', 'transparent'], 'dark');
  var font = num(params.get('font'), 10, 72, 20);
  // a small logo next to the name, not a badge: about the height of a lower-case letter's body
  var iconPx = num(params.get('size'), 8, 96, Math.max(10, Math.round(font * 0.8)));
  var maxLines = num(params.get('max'), 1, 100, 30);
  var fadeSec = num(params.get('fade'), 0, 600, 0);
  var colorMode = pick(params.get('colors'), ['platform', 'hash', 'off'], 'platform');
  var showEmotes = params.get('emotes') !== '0';
  var backlog = num(params.get('backlog'), 0, 200, maxLines);

  body.setAttribute('data-theme', theme);
  body.setAttribute('data-align', pick(params.get('align'), ['bottom', 'top'], 'bottom'));
  document.documentElement.style.setProperty('--font', font + 'px');
  document.documentElement.style.setProperty('--icon', iconPx + 'px');

  // contrast against the card colour of the theme, so a platform's dark blue is still readable
  var bgLuma = theme === 'light' ? 0.93 : (theme === 'transparent' ? 0.12 : 0.012);
  function lin(c) { c /= 255; return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); }
  function luma(r, g, b) { return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b); }
  function ratio(l1, l2) { var a = Math.max(l1, l2), b = Math.min(l1, l2); return (a + 0.05) / (b + 0.05); }
  function ensureContrast(hex) {
    var r = parseInt(hex.slice(1, 3), 16), g = parseInt(hex.slice(3, 5), 16), b = parseInt(hex.slice(5, 7), 16);
    for (var i = 0; i < 12 && ratio(luma(r, g, b), bgLuma) < 3; i++) {
      var toward = bgLuma > 0.5 ? 0 : 255;
      r = Math.round(r + (toward - r) * 0.18); g = Math.round(g + (toward - g) * 0.18); b = Math.round(b + (toward - b) * 0.18);
    }
    return 'rgb(' + r + ',' + g + ',' + b + ')';
  }
  function hashColor(name) {
    var h = 2166136261;
    for (var i = 0; i < name.length; i++) { h ^= name.charCodeAt(i); h = Math.imul(h, 16777619); }
    var hue = (h >>> 0) % 360;
    return 'hsl(' + hue + ',68%,' + (theme === 'light' ? '34%' : '66%') + ')';
  }
  function nameColor(m) {
    if (colorMode === 'off') return '';
    if (colorMode === 'platform' && typeof m.color === 'string' && /^#[0-9a-fA-F]{6}$/.test(m.color)) return ensureContrast(m.color);
    return hashColor(String(m.author || ''));
  }

  function emoteNode(p) {
    var url = typeof p.u === 'string' ? p.u : '';
    var name = String(p.v || '');
    if (!showEmotes || url.indexOf('https://') !== 0) return document.createTextNode(name);
    var img = document.createElement('img');
    img.className = 'emote';
    img.alt = name;
    img.title = name;
    img.draggable = false;
    img.referrerPolicy = 'no-referrer';
    img.addEventListener('load', function () { trimToFit(); }); // an emote can make its line taller
    img.addEventListener('error', function () { if (img.parentNode) img.parentNode.replaceChild(document.createTextNode(name), img); });
    img.src = url;
    return img;
  }

  var lastSeq = 0;
  var lines = [];

  // Drops the oldest lines until the rest fit the browser source, so the chat ticks: new lines
  // push old ones out, whole lines only, with align=bottom and align=top alike. The newest line
  // always stays, even when it alone is taller than the source. Offsets ignore the rise animation's
  // transform, so a line still sliding in is measured where it will end up.
  function trimToFit() {
    if (lines.length < 2) return;
    var cs = getComputedStyle(chat);
    var room = chat.clientHeight - (parseFloat(cs.paddingTop) || 0) - (parseFloat(cs.paddingBottom) || 0);
    if (!(room > 0)) return;
    var last = lines[lines.length - 1];
    while (lines.length > 1 && last.offsetTop + last.offsetHeight - lines[0].offsetTop > room + 0.5) {
      var old = lines.shift();
      if (old.parentNode) old.parentNode.removeChild(old);
    }
  }

  function add(m) {
    if (!m || typeof m !== 'object') return;
    var platform = pick(m.platform, PLATFORMS, null);
    if (!platform || !Array.isArray(m.parts)) return;
    var el = document.createElement('div');
    el.className = 'msg';
    el.setAttribute('data-platform', platform);
    var ico = document.createElement('span');
    ico.className = 'ico';
    ico.style.setProperty('--mask', 'url("/icons/' + platform + '.svg")');
    var name = document.createElement('span');
    name.className = 'name';
    name.textContent = String(m.author || '');
    var c = nameColor(m);
    if (c) name.style.color = c;
    var sep = document.createElement('span');
    sep.className = 'sep';
    sep.textContent = ':';
    var text = document.createElement('span');
    text.className = 'text';
    for (var i = 0; i < m.parts.length && i < 80; i++) {
      var p = m.parts[i];
      if (!p) continue;
      if (p.t === 'emote') text.appendChild(emoteNode(p));
      else text.appendChild(document.createTextNode(String(p.v == null ? '' : p.v)));
    }
    el.appendChild(ico);
    el.appendChild(name);
    el.appendChild(sep);
    el.appendChild(text);
    chat.appendChild(el);
    lines.push(el);
    while (lines.length > maxLines) {
      var old = lines.shift();
      if (old.parentNode) old.parentNode.removeChild(old);
    }
    trimToFit();
    if (fadeSec > 0) {
      setTimeout(function () { el.classList.add('fading'); }, fadeSec * 1000);
      setTimeout(function () {
        if (el.parentNode) el.parentNode.removeChild(el);
        var k = lines.indexOf(el);
        if (k >= 0) lines.splice(k, 1);
      }, fadeSec * 1000 + 1100);
    }
  }

  function connect() {
    if (typeof EventSource === 'undefined') return;
    var es = new EventSource('/chat/events?backlog=' + backlog);
    es.onmessage = function (ev) {
      var m;
      try { m = JSON.parse(ev.data); } catch (e) { return; }
      var seq = Number(m && m.seq);
      if (!(seq > lastSeq)) return; // a replayed line after a reconnect
      lastSeq = seq;
      add(m);
    };
    es.onerror = function () { /* EventSource reconnects by itself and sends Last-Event-ID */ };
  }

  window.addEventListener('resize', trimToFit);
  window.__omsc = { add: add };   // test hook: renders a message object, nothing more than the feed already allows
  if (params.get('static') !== '1') connect();
})();
