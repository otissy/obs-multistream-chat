(function () {
  'use strict';
  // Chat voice for the chat overlay (the same browser source that shows the chat). Off unless the URL
  // has voice=1: then this page plays the lines the plugin's voice reader speaks, so OBS hears them as
  // this source's audio ("Control audio via OBS" on the source puts them on the mixer and the stream
  // tracks). Talks only to its own origin with relative URLs (voice/events, voice/clip/<id>.wav), so
  // the access key in the page URL covers them. Author names are untrusted text: textContent only.
  //
  // Several pages may listen (the same URL in two browser sources, a preview in a browser); the
  // server picks ONE leader (shown on program > inside OBS > newest) and only the leader speaks.
  var params = new URLSearchParams(location.search);
  if (params.get('voice') !== '1') { window.__omscVoice = null; return; }

  var inObs = typeof window.obsstudio !== 'undefined';
  var STORE = 'omsc.voice.v1';

  function num(v, lo, hi, def) { var n = parseInt(v, 10); return (n >= lo && n <= hi) ? n : def; }
  function names(s) {
    return String(s || '').split(',').map(function (x) { return x.trim().toLowerCase(); })
      .filter(function (x) { return x.length > 0 && x.length <= 64; }).slice(0, 50);
  }
  // URL = the defaults the streamer put in the browser source; the panel's changes are kept in this
  // browser profile (OBS keeps localStorage) and win over the URL until "Reset to the URL settings".
  var urlDefaults = {
    speak: params.get('speak') !== '0',
    vol: num(params.get('vol'), 0, 100, 100),
    queue: num(params.get('queue'), 1, 50, 5),
    drop: params.get('drop') === 'newest' ? 'newest' : 'oldest',
    local: params.get('local') === '1',
    skip: names(params.get('skip')).join(', ')
  };
  function load() {
    var s = {};
    for (var k in urlDefaults) s[k] = urlDefaults[k];
    try {
      var raw = localStorage.getItem(STORE);
      var o = raw ? JSON.parse(raw) : null;
      if (o && typeof o === 'object') {
        if (typeof o.speak === 'boolean') s.speak = o.speak;
        if (typeof o.vol === 'number') s.vol = num(o.vol, 0, 100, s.vol);
        if (typeof o.queue === 'number') s.queue = num(o.queue, 1, 50, s.queue);
        if (o.drop === 'oldest' || o.drop === 'newest') s.drop = o.drop;
        if (typeof o.local === 'boolean') s.local = o.local;
        if (typeof o.skip === 'string') s.skip = names(o.skip).join(', ');
      }
    } catch (e) { /* no storage (private window): URL settings only */ }
    return s;
  }
  function save() { try { localStorage.setItem(STORE, JSON.stringify(settings)); } catch (e) { /* ignore */ } }
  var settings = load();

  // ---- state ------------------------------------------------------------------------------------
  var st = { conn: 'connecting', role: '', blocked: false, playing: 0, played: 0, errors: 0, skipped: 0, dropped: 0, lastError: '' };
  var queue = [];          // clip announcements waiting: {id, author, platform, ms}
  var active = true;       // OBS: the source is on program (unknown = true)
  var es = null;

  // ---- audio: one media element -> Web Audio (volume, level meter) -> speakers / OBS -------------
  var audio = new Audio();
  audio.preload = 'auto';
  var ctx = null, gain = null, analyser = null, tap = null;
  function graph() {
    if (ctx) return;
    var AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    try {
      ctx = new AC();
      var src = ctx.createMediaElementSource(audio);
      gain = ctx.createGain();
      analyser = ctx.createAnalyser();
      analyser.fftSize = 1024;
      src.connect(gain);
      gain.connect(analyser);
      gain.connect(ctx.destination);
      // wake the graph whenever OBS / the browser suspends it
      ctx.onstatechange = function () { render(); };
    } catch (e) { ctx = null; gain = null; analyser = null; }
  }
  function applyVolume() {
    if (gain) { gain.gain.value = settings.vol / 100; audio.volume = 1; }
    else audio.volume = settings.vol / 100;
  }
  graph();
  applyVolume();

  function stopCurrent() {
    try { audio.pause(); } catch (e) { /* ignore */ }
    audio.removeAttribute('src');
    try { audio.load(); } catch (e) { /* ignore */ }
    st.playing = 0;
  }

  function pump() {
    if (st.playing || st.blocked || !queue.length) { render(); return; }
    var c = queue.shift();
    st.playing = c.id;
    audio.src = 'voice/clip/' + c.id + '.wav';
    var p;
    try { p = audio.play(); } catch (e) { p = Promise.reject(e); }
    Promise.resolve(p).then(function () {
      if (ctx && ctx.state === 'suspended') {
        // the element may play while the audio graph is still held back (no click yet): that is
        // silence, so treat it like a refused play and ask for the click
        ctx.resume().catch(function () {});
        setTimeout(function () {
          if (st.playing === c.id && ctx.state === 'suspended') {
            stopCurrent();
            st.blocked = true;
            queue.unshift(c);
            render();
          }
        }, 300);
      }
      render();
    }, function (err) {
      if (st.playing !== c.id) return; // flushed or replaced meanwhile
      if (err && err.name === 'NotAllowedError') {
        // a normal browser without a click yet: keep the line and show the button
        st.blocked = true;
        st.playing = 0;
        queue.unshift(c);
      } else if (err && err.name === 'AbortError') {
        st.playing = 0;
      } else {
        fail(c, err && err.name ? err.name : 'play failed');
        return;
      }
      render();
    });
    render();
  }
  function fail(c, why) {
    st.errors++;
    st.lastError = why;
    if (st.playing === c.id) st.playing = 0;
    pump();
  }
  audio.addEventListener('ended', function () { if (st.playing) { st.played++; st.playing = 0; } pump(); });
  audio.addEventListener('error', function () {
    if (!st.playing || !audio.getAttribute('src')) return; // our own removeAttribute
    var code = audio.error ? audio.error.code : 0;
    fail({ id: st.playing }, code === 4 ? 'could not decode the line' : 'could not load the line');
  });

  function unlock() {
    graph();
    if (ctx && ctx.state !== 'running') ctx.resume().catch(function () {});
    st.blocked = false;
    pump();
  }

  // ---- what the server says --------------------------------------------------------------------
  function onClip(m) {
    if (!m || typeof m !== 'object') return;
    var id = Number(m.id);
    if (!(id > 0) || id !== Math.floor(id)) return;
    if (st.role !== 'leader' || !settings.speak) return;
    var author = String(m.author || '').trim().toLowerCase();
    if (author && names(settings.skip).indexOf(author) >= 0) { st.skipped++; render(); return; }
    var waiting = queue.length + (st.playing ? 1 : 0);
    if (waiting >= settings.queue) {
      st.dropped++;
      if (settings.drop === 'newest' || !queue.length) { render(); return; }
      queue.shift(); // drop the oldest waiting line, keep the one being spoken
    }
    queue.push({ id: id, author: String(m.author || ''), platform: String(m.platform || ''), ms: Number(m.ms) || 0 });
    pump();
  }
  function flushAll() { queue = []; stopCurrent(); render(); }

  function connect() {
    if (es) { es.close(); es = null; }
    if (typeof EventSource === 'undefined') { st.conn = 'offline'; render(); return; }
    st.conn = 'connecting';
    es = new EventSource('voice/events?obs=' + (inObs ? 1 : 0) + '&active=' + (active ? 1 : 0) + '&local=' + (settings.local ? 1 : 0));
    es.onopen = function () { st.conn = 'live'; render(); };
    es.onerror = function () {
      st.conn = 'offline'; // EventSource retries by itself (retry: 2000)
      if (st.role === 'leader') st.role = '';
      render();
    };
    es.addEventListener('role', function (ev) {
      var m; try { m = JSON.parse(ev.data); } catch (e) { return; }
      st.conn = 'live';
      var role = m && m.role === 'leader' ? 'leader' : 'standby';
      if (role !== 'leader' && st.role === 'leader') flushAll(); // another overlay speaks now
      st.role = role;
      render();
    });
    es.addEventListener('clip', function (ev) { var m; try { m = JSON.parse(ev.data); } catch (e) { return; } onClip(m); });
    es.addEventListener('flush', function () { flushAll(); });
  }
  window.addEventListener('obsSourceActiveChanged', function (ev) {
    var a = !!(ev && ev.detail && ev.detail.active);
    if (a !== active) { active = a; connect(); }
  });

  // ---- the controls (never part of the chat layout: fixed, on top) -------------------------------
  var showControls = !inObs || params.get('controls') === '1';
  var css = document.createElement('style');
  css.textContent =
    '#omsc-voice{position:fixed;top:8px;right:8px;z-index:10;display:flex;flex-direction:column;align-items:flex-end;gap:6px;' +
    'font:14px/1.3 "Segoe UI",system-ui,sans-serif;color:#f2f2f5;max-width:calc(100vw - 16px)}' +
    '#omsc-voice button,#omsc-voice input,#omsc-voice select{font:inherit}' +
    '#omsc-voice .bar{display:flex;gap:6px;align-items:center}' +
    '#omsc-voice-status{background:rgba(18,18,24,.85);border-radius:999px;padding:4px 10px;font-size:12px}' +
    '#omsc-voice-status[data-conn="offline"]{background:#7f1d1d}' +
    '#omsc-voice-gear,#omsc-voice-unlock{background:#5865F2;color:#fff;border:0;border-radius:6px;padding:6px 12px;cursor:pointer;min-height:32px}' +
    '#omsc-voice-unlock{background:#16a34a;font-weight:700;padding:10px 16px}' +
    '#omsc-voice button:focus-visible,#omsc-voice input:focus-visible,#omsc-voice select:focus-visible{outline:3px solid #facc15;outline-offset:2px}' +
    '#omsc-voice-panel{background:rgba(18,18,24,.96);border:1px solid #3a3a48;border-radius:8px;padding:12px;width:300px;max-width:calc(100vw - 16px);box-sizing:border-box}' +
    '#omsc-voice-panel label{display:block;margin:8px 0 2px}' +
    '#omsc-voice-panel label.row{display:flex;gap:8px;align-items:center}' +
    '#omsc-voice-panel input[type=text],#omsc-voice-panel input[type=number],#omsc-voice-panel select{width:100%;box-sizing:border-box;padding:4px}' +
    '#omsc-voice-panel input[type=range]{width:100%}' +
    '#omsc-voice-panel .acts{display:flex;gap:8px;justify-content:flex-end;margin-top:10px}' +
    '#omsc-voice-panel .acts button{background:#2a2a36;color:#f2f2f5;border:1px solid #4a4a5a;border-radius:6px;padding:6px 10px;cursor:pointer}' +
    '#omsc-voice-meter{width:100%;height:8px}' +
    '#omsc-voice [hidden]{display:none!important}';
  document.head.appendChild(css);

  function el(tag, attrs, text) {
    var e = document.createElement(tag);
    for (var k in attrs) e.setAttribute(k, attrs[k]);
    if (text != null) e.textContent = text;
    return e;
  }
  var root = el('div', { id: 'omsc-voice' });
  var unlockBtn = el('button', { id: 'omsc-voice-unlock', type: 'button', hidden: '' }, 'Click to enable sound');
  var bar = el('div', { class: 'bar' });
  var statusEl = el('span', { id: 'omsc-voice-status', role: 'status', 'aria-live': 'polite' });
  var gear = el('button', { id: 'omsc-voice-gear', type: 'button', 'aria-expanded': 'false', 'aria-controls': 'omsc-voice-panel' }, 'Voice settings');
  bar.appendChild(statusEl);
  bar.appendChild(gear);
  if (!showControls) bar.hidden = true;
  var panel = el('div', { id: 'omsc-voice-panel', role: 'dialog', 'aria-label': 'Chat voice settings', hidden: '' });
  function field(labelText, input, row) {
    var l = el('label', row ? { class: 'row' } : {});
    if (row) { l.appendChild(input); l.appendChild(document.createTextNode(labelText)); }
    else { l.appendChild(document.createTextNode(labelText)); l.appendChild(input); }
    panel.appendChild(l);
  }
  var inSpeak = el('input', { id: 'omsc-voice-speak', type: 'checkbox' });
  var inVol = el('input', { id: 'omsc-voice-vol', type: 'range', min: '0', max: '100', step: '1' });
  var volOut = el('output', { id: 'omsc-voice-vol-out', for: 'omsc-voice-vol' });
  var inQueue = el('input', { id: 'omsc-voice-queue', type: 'number', min: '1', max: '50', step: '1' });
  var inDrop = el('select', { id: 'omsc-voice-drop' });
  inDrop.appendChild(el('option', { value: 'oldest' }, 'Drop the oldest waiting line'));
  inDrop.appendChild(el('option', { value: 'newest' }, 'Skip the new line'));
  var inLocal = el('input', { id: 'omsc-voice-local', type: 'checkbox' });
  var inSkip = el('input', { id: 'omsc-voice-skip', type: 'text', maxlength: '600', placeholder: 'yourname, nightbot', autocomplete: 'off' });
  var meter = el('meter', { id: 'omsc-voice-meter', min: '0', max: '1', value: '0', 'aria-label': 'Voice level' });
  field('Speak chat on stream', inSpeak, true);
  field('Volume ', inVol);
  panel.lastChild.insertBefore(volOut, inVol);
  field('Lines waiting at most', inQueue);
  field('When too many lines wait', inDrop);
  field('Also play on this PC’s speakers', inLocal, true);
  field('Never read out lines from (comma separated)', inSkip);
  field('Level', meter);
  var acts = el('div', { class: 'acts' });
  var resetBtn = el('button', { id: 'omsc-voice-reset', type: 'button' }, 'Reset to the URL settings');
  var closeBtn = el('button', { id: 'omsc-voice-close', type: 'button' }, 'Close');
  acts.appendChild(resetBtn);
  acts.appendChild(closeBtn);
  panel.appendChild(acts);
  root.appendChild(unlockBtn);
  root.appendChild(bar);
  root.appendChild(panel);
  document.body.appendChild(root);

  function fillInputs() {
    inSpeak.checked = settings.speak;
    inVol.value = String(settings.vol);
    volOut.textContent = settings.vol + '%';
    inQueue.value = String(settings.queue);
    inDrop.value = settings.drop;
    inLocal.checked = settings.local;
    if (document.activeElement !== inSkip) inSkip.value = settings.skip;
  }
  function statusText() {
    if (st.conn === 'offline') return 'Voice: offline (plugin not reachable)';
    if (st.conn === 'connecting') return 'Voice: connecting';
    if (st.role === 'standby') return 'Voice: standby (another overlay speaks)';
    if (!settings.speak) return 'Voice: muted';
    if (st.blocked) return 'Voice: waiting for a click';
    if (st.playing) return 'Voice: speaking';
    return 'Voice: ready';
  }
  function render() {
    unlockBtn.hidden = !st.blocked;
    statusEl.textContent = statusText();
    statusEl.setAttribute('data-conn', st.conn);
    root.setAttribute('data-role', st.role || 'none');
    root.setAttribute('data-conn', st.conn);
  }
  function openPanel(open) {
    panel.hidden = !open;
    gear.setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) { bar.hidden = false; fillInputs(); inSpeak.focus(); }
    else { if (!showControls) bar.hidden = true; if (showControls) gear.focus(); }
  }
  function changed(reconnect, keepStorage) {
    if (!keepStorage) save();
    applyVolume();
    if (!settings.speak) flushAll();
    if (reconnect) connect();
    render();
  }
  gear.addEventListener('click', function () { openPanel(panel.hidden); });
  closeBtn.addEventListener('click', function () { openPanel(false); });
  unlockBtn.addEventListener('click', unlock);
  inSpeak.addEventListener('change', function () { settings.speak = inSpeak.checked; changed(false); });
  inVol.addEventListener('input', function () { settings.vol = num(inVol.value, 0, 100, settings.vol); volOut.textContent = settings.vol + '%'; changed(false); });
  inQueue.addEventListener('change', function () { settings.queue = num(inQueue.value, 1, 50, settings.queue); inQueue.value = String(settings.queue); changed(false); });
  inDrop.addEventListener('change', function () { settings.drop = inDrop.value === 'newest' ? 'newest' : 'oldest'; changed(false); });
  inLocal.addEventListener('change', function () { settings.local = inLocal.checked; changed(true); });
  inSkip.addEventListener('change', function () { settings.skip = names(inSkip.value).join(', '); inSkip.value = settings.skip; changed(false); });
  resetBtn.addEventListener('click', function () {
    try { localStorage.removeItem(STORE); } catch (e) { /* ignore */ }
    var wasLocal = settings.local;
    settings = load();
    fillInputs();
    changed(settings.local !== wasLocal, true); // back to the URL: nothing saved until the next change
  });
  document.addEventListener('keydown', function (ev) {
    // Alt+Shift+V opens the settings, also inside OBS (Interact window) where the button is hidden
    if (ev.altKey && ev.shiftKey && (ev.key === 'V' || ev.key === 'v' || ev.code === 'KeyV')) { ev.preventDefault(); openPanel(panel.hidden); return; }
    if (ev.key === 'Escape' && !panel.hidden) { ev.preventDefault(); openPanel(false); }
  });
  // any click or key on the page counts as the click a normal browser waits for
  document.addEventListener('pointerdown', function (ev) { if (st.blocked && ev.target !== unlockBtn) unlock(); }, true);

  // level meter, only while the panel is open
  var lvl = new Float32Array(1024);
  setInterval(function () {
    if (panel.hidden || !analyser) return;
    analyser.getFloatTimeDomainData(lvl);
    var peak = 0;
    for (var i = 0; i < lvl.length; i++) { var a = Math.abs(lvl[i]); if (a > peak) peak = a; }
    meter.value = Math.min(1, peak);
  }, 100);

  // test hooks: read-only state, and a tap on the real output (after the volume) for recordings
  window.__omscVoice = {
    state: function () {
      return { conn: st.conn, role: st.role, blocked: st.blocked, playing: st.playing, played: st.played, errors: st.errors,
        skipped: st.skipped, dropped: st.dropped, lastError: st.lastError, queued: queue.map(function (c) { return c.id; }),
        settings: JSON.parse(JSON.stringify(settings)), ctx: ctx ? ctx.state : 'none', sampleRate: ctx ? ctx.sampleRate : 0,
        duration: isFinite(audio.duration) ? audio.duration : 0, inObs: inObs };
    },
    // starts collecting the mono output samples; stop() returns them as base64 16-bit little endian
    capture: function () {
      if (!ctx || tap) return false;
      var chunks = [];
      tap = ctx.createScriptProcessor(4096, 1, 1);
      var mute = ctx.createGain();
      mute.gain.value = 0;
      tap.onaudioprocess = function (e) { chunks.push(new Float32Array(e.inputBuffer.getChannelData(0))); };
      gain.connect(tap);
      tap.connect(mute);
      mute.connect(ctx.destination);
      tap.stop = function () {
        gain.disconnect(tap); tap.disconnect(); mute.disconnect();
        var n = 0; chunks.forEach(function (c) { n += c.length; });
        var bytes = new Uint8Array(n * 2), o = 0;
        chunks.forEach(function (c) {
          for (var i = 0; i < c.length; i++) {
            var v = Math.max(-1, Math.min(1, c[i])); var s = Math.round(v * 32767);
            bytes[o++] = s & 0xff; bytes[o++] = (s >> 8) & 0xff;
          }
        });
        var bin = ''; for (var j = 0; j < bytes.length; j += 8192) bin += String.fromCharCode.apply(null, bytes.subarray(j, j + 8192));
        tap = null;
        return { rate: ctx.sampleRate, b64: btoa(bin) };
      };
      return true;
    },
    stopCapture: function () { return tap ? tap.stop() : null; }
  };

  fillInputs();
  render();
  connect();
})();
