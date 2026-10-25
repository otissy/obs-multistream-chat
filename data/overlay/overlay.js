(function () {
  'use strict';
  // Only talks to this page's own origin: events (SSE) with state.json polling as fallback.
  // Two homes, one script: overlay.html (the platform bar on its own, query names as below) and
  // chat.html with platforms=top|bottom (the same bar above or below the chat, so ONE browser
  // source shows both; there bar_size / bar_layout replace size / layout, which the chat uses).
  // Platform ids are matched against a fixed list before they are used in a URL; handles are
  // untrusted text and only ever go through textContent.
  var BRANDS = {
    twitch: '#9146ff', youtube: '#ff2a2a', kick: '#53fc18', facebook: '#1877f2', tiktok: '#25f4ee', generic: ''
  };
  var STATES = ['idle', 'connecting', 'live', 'degraded', 'reconnecting', 'failed'];
  var STALE_MS = 6000;

  var params = new URLSearchParams(location.search);
  var body = document.body;
  var bar = document.getElementById('bar');
  var chatBox = document.getElementById('chat');
  var embedded = !!chatBox;
  var where = embedded ? pick(params.get('platforms'), ['top', 'bottom'], null) : 'page';

  function pick(value, allowed, fallback) { return allowed.indexOf(value) >= 0 ? value : fallback; }
  function applyParams() {
    body.setAttribute('data-theme', pick(params.get('theme'), ['dark', 'light', 'transparent'], 'dark'));
    body.setAttribute('data-layout', pick(params.get(embedded ? 'bar_layout' : 'layout'), ['h', 'v'], 'h'));
    if (embedded) body.setAttribute('data-platforms', where);
    body.setAttribute('data-names', params.get('names') === '0' ? '0' : '1');
    body.setAttribute('data-hide-idle', params.get('hide_idle') === '1' ? '1' : '0');
    var size = parseInt(params.get(embedded ? 'bar_size' : 'size'), 10);
    if (!(size >= 16 && size <= 256)) size = 48;
    document.documentElement.style.setProperty('--size', size + 'px');
    var accent = params.get('accent');
    if (accent && /^[0-9a-fA-F]{6}$/.test(accent)) document.documentElement.style.setProperty('--accent', '#' + accent);
  }

  function platformOf(p) { return Object.prototype.hasOwnProperty.call(BRANDS, p) ? p : 'generic'; }

  function build(d) {
    var platform = platformOf(d.platform);
    var el = document.createElement('div');
    el.className = 'dest';
    el.setAttribute('data-id', String(d.id));
    var icon = document.createElement('div');
    icon.className = 'icon';
    var glyph = document.createElement('div');
    glyph.className = 'glyph';
    glyph.style.setProperty('--mask', 'url("icons/' + platform + '.svg")');
    var dot = document.createElement('div');
    dot.className = 'dot';
    icon.appendChild(glyph);
    icon.appendChild(dot);
    var name = document.createElement('div');
    name.className = 'name';
    el.appendChild(icon);
    el.appendChild(name);
    el._platform = platform;
    if (BRANDS[platform]) el.style.setProperty('--brand', BRANDS[platform]);
    return el;
  }

  var lastSeen = 0;
  function render(state) {
    lastSeen = Date.now();
    body.removeAttribute('data-stale');
    var list = (state && Array.isArray(state.destinations)) ? state.destinations : [];
    var existing = {};
    Array.prototype.forEach.call(bar.children, function (c) { existing[c.getAttribute('data-id')] = c; });
    var keep = {};
    list.forEach(function (d, i) {
      var id = String(d.id);
      var el = existing[id];
      if (el && el._platform !== platformOf(d.platform)) { bar.removeChild(el); el = null; }
      if (!el) el = build(d);
      keep[id] = true;
      var st = pick(d.state, STATES, 'idle');
      el.setAttribute('data-state', st);
      el.title = (d.handle ? d.handle + ' - ' : '') + platformOf(d.platform) + ': ' + st;
      el.querySelector('.name').textContent = d.handle ? String(d.handle) : '';
      if (bar.children[i] !== el) bar.insertBefore(el, bar.children[i] || null);
    });
    Object.keys(existing).forEach(function (id) { if (!keep[id] && existing[id].parentNode) bar.removeChild(existing[id]); });
    if (embedded) makeRoom();
  }

  // In chat.html the chat box gives up the bar's height, then re-fits its lines (chat.js listens to resize).
  var barH = -1;
  function makeRoom() {
    var h = Math.ceil(bar.getBoundingClientRect().height);
    if (h === barH) return;
    barH = h;
    document.documentElement.style.setProperty('--bar-h', h + 'px');
    window.dispatchEvent(new Event('resize'));
  }

  function markStaleIfSilent() {
    if (lastSeen && Date.now() - lastSeen > STALE_MS) body.setAttribute('data-stale', '1');
  }

  function poll() {
    fetch('state.json', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(render)
      .catch(function () { /* stale timer handles it */ });
  }

  function connect() {
    if (typeof EventSource === 'undefined') return;
    var es = new EventSource('events');
    es.onmessage = function (ev) { try { render(JSON.parse(ev.data)); } catch (e) { /* ignore a bad frame */ } };
    es.onerror = function () { /* EventSource reconnects by itself (retry: 2000) */ };
  }

  if (embedded && !where) return;      // chat.html without platforms=: no bar, no requests
  applyParams();
  window.__oms = { render: render };    // test hook; no capability beyond what /state.json already gives
  if (params.get('static') !== '1') {
    poll();
    connect();
    setInterval(poll, 3000);            // safety net if the event stream is blocked
    setInterval(markStaleIfSilent, 1000);
  }
})();
