'use strict';
// One chat source = one browser tab (in its own cookie-less browser context) on one chat page.
// It loads the page ONCE, injects the read-only engine, and turns what the engine reports into
// messages and a health state. It never reloads in a loop: a reload happens only after a load
// failure or crash, with backoff and a hard cap.
const crypto = require('crypto');
const { EventEmitter } = require('events');
const { omscEngine } = require('./inject');
const { pageUrl, allowedHosts } = require('./adapters');
const { delayMs } = require('./backoff');

const MAX_ATTEMPTS = 4;
const LOAD_TIMEOUT_MS = 45000;
const MEDIA_BLOCK = ['*.mp4*', '*.m3u8*', '*.webm*', '*.mpd*', '*.flv*', '*.ts?*'];

// Pure: what state does the evidence say? (unit tested without a browser)
//   e = { now, loadedAt, tick:{container,login,seen,sinceFound}|null, containerTimeoutMs, quietMs,
//         seenTotal, loginRedirect }
function evaluateHealth(e) {
  if (e.loginRedirect) return { state: 'login', detail: 'the platform sent the page to a login; nothing can be read without one' };
  if (!e.loadedAt) return { state: 'starting', detail: 'loading the page' };
  const elapsed = e.now - e.loadedAt;
  const t = e.tick;
  if (!t || !t.container) {
    if (t && t.login && elapsed >= 8000) return { state: 'login', detail: 'the page asks for a login or a human check; the reader never logs in' };
    if (elapsed >= e.containerTimeoutMs) {
      return { state: 'layout', detail: 'page layout changed or login needed: the chat list was not found after ' + Math.round(elapsed / 1000) + ' s' };
    }
    return { state: 'starting', detail: 'waiting for the chat list to appear' };
  }
  if (e.seenTotal > 0) return { state: 'reading', detail: '' };
  if (t.sinceFound >= e.quietMs) {
    return { state: 'quiet', detail: 'no messages for ' + Math.round(t.sinceFound / 60000) + ' min: chat may just be empty, or the page layout changed or login is needed' };
  }
  return { state: 'reading', detail: 'chat list found, waiting for the first message' };
}

class Source extends EventEmitter {
  // opts: { cdp, pack, src:{id,platform,target}, testOrigin, quietMs, log, bindingName? }
  constructor(opts) {
    super();
    this.cdp = opts.cdp;
    this.pack = opts.pack;
    this.src = opts.src;
    this.testOrigin = opts.testOrigin || '';
    this.quietMs = opts.quietMs || 5 * 60000;
    this.log = opts.log;
    this.binding = 'omsc_' + crypto.randomBytes(6).toString('hex');
    this.sessionId = null;
    this.targetId = null;
    this.contextId = null;
    this.unsubs = [];
    this.loadedAt = 0;
    this.tick = null;
    this.seenTotal = 0;
    this.loginRedirect = false;
    this.attempts = 0;
    this.stopped = false;
    this.state = 'starting';
    this.detail = 'starting';
    this.timers = new Set();
    this.url = pageUrl(this.pack, this.src.target, this.testOrigin);
    this.hosts = allowedHosts(this.pack, this.testOrigin);
  }

  status() { return { id: this.src.id, platform: this.src.platform, state: this.state, detail: this.detail }; }

  _set(state, detail) {
    if (state === this.state && detail === this.detail) return;
    this.state = state;
    this.detail = detail;
    this.log.info('source ' + this.src.id + ' (' + this.src.platform + '): ' + state + (detail ? ' - ' + detail : ''));
    this.emit('status', this.status());
  }
  _timer(fn, ms) {
    const t = setTimeout(() => { this.timers.delete(t); fn(); }, ms);
    this.timers.add(t);
    return t;
  }

  async start() {
    this.stopped = false;
    this._set('starting', 'opening the page');
    await this._open();
    this.healthTimer = setInterval(() => this._evaluate(), 2000);
  }

  async _open() {
    if (this.stopped) return;
    const cdp = this.cdp;
    try {
      const ctx = await cdp.send('Target.createBrowserContext', {});
      this.contextId = ctx.browserContextId;
      try { await cdp.send('Browser.setDownloadBehavior', { behavior: 'deny', browserContextId: this.contextId }); } catch (e) { /* older Chrome */ }
      const t = await cdp.send('Target.createTarget', { url: 'about:blank', browserContextId: this.contextId });
      this.targetId = t.targetId;
      const a = await cdp.send('Target.attachToTarget', { targetId: this.targetId, flatten: true });
      this.sessionId = a.sessionId;
      const sid = this.sessionId;
      const s = (m, p) => cdp.send(m, p || {}, sid);
      this.unsubs.push(cdp.on('Runtime.bindingCalled', p => this._onBinding(p), sid));
      this.unsubs.push(cdp.on('Page.loadEventFired', () => this._onLoad(), sid));
      this.unsubs.push(cdp.on('Page.frameNavigated', p => this._onNavigated(p), sid));
      this.unsubs.push(cdp.on('Inspector.targetCrashed', () => this._fail('the page crashed'), sid));
      this.unsubs.push(cdp.on('Page.javascriptDialogOpening', () => { s('Page.handleJavaScriptDialog', { accept: false }).catch(() => {}); }, sid));
      this.unsubs.push(cdp.on('Target.targetDestroyed', p => { if (p.targetId === this.targetId && !this.stopped && !this._reopening) this._fail('the tab was closed'); }));
      this.unsubs.push(cdp.on('Target.targetCreated', p => {
        // a page opening a popup: close it, we only ever read this one tab
        const ti = p.targetInfo || {};
        if (ti.type === 'page' && ti.openerId === this.targetId) cdp.send('Target.closeTarget', { targetId: ti.targetId }).catch(() => {});
      }));
      await s('Page.enable');
      await s('Runtime.enable');
      await s('Network.enable');
      await s('Network.setBlockedURLs', { urls: MEDIA_BLOCK });
      await s('Emulation.setDeviceMetricsOverride', { width: 900, height: 1000, deviceScaleFactor: 1, mobile: false });
      await s('Runtime.addBinding', { name: this.binding, executionContextName: 'omsc' });
      const engineCfg = Object.assign({}, this.pack.engine);
      const source = '(' + omscEngine.toString() + ')(' + JSON.stringify(engineCfg) + ',' + JSON.stringify(this.binding) + ');';
      await s('Page.addScriptToEvaluateOnNewDocument', { source: guardTop(source), worldName: 'omsc' });
      this.loadedAt = 0;
      this.tick = null;
      this._loadTimer = this._timer(() => this._fail('the page did not finish loading in ' + LOAD_TIMEOUT_MS / 1000 + ' s'), LOAD_TIMEOUT_MS);
      const nav = await s('Page.navigate', { url: this.url });
      if (nav.errorText) this._fail('could not load the page (' + nav.errorText + ')');
    } catch (e) {
      this._fail('could not open the page (' + (e && e.message ? e.message : e) + ')');
    }
  }

  _onLoad() {
    if (this.stopped) return;
    clearTimeout(this._loadTimer);
    this.loadedAt = Date.now();
    this.attempts = 0; // loaded fine: the failure counter starts over
    this._evaluate();
  }

  _onNavigated(p) {
    const f = p.frame || {};
    if (f.parentId || this.stopped) return; // only the main frame matters
    let u;
    try { u = new URL(f.url); } catch (e) { return; }
    if (u.protocol === 'about:' || u.protocol === 'chrome-error:') return;
    if (!this.hosts.includes(u.hostname)) {
      this.log.warn('source ' + this.src.id + ' was redirected to ' + u.hostname + ' (not allowed): stopped');
      this._closeTab();
      this.stopped = true;
      this._set('error', 'the page redirected to ' + u.hostname + ', which is not on the platform allow-list; stopped');
      return;
    }
    const lp = this.pack.loginPaths || [];
    if (!this.testOrigin && lp.some(x => u.pathname.startsWith(x))) {
      this.loginRedirect = true;
      this._evaluate();
    }
  }

  _onBinding(p) {
    if (p.name !== this.binding || this.stopped) return;
    let m;
    try {
      if (typeof p.payload !== 'string' || p.payload.length > 600000) return;
      m = JSON.parse(p.payload);
    } catch (e) { return; }
    if (m.t === 'tick') {
      this.tick = { container: !!m.container, login: !!m.login, seen: Number(m.seen) || 0, sinceFound: Number(m.sinceFound) || 0 };
      this._evaluate();
    } else if (m.t === 'msgs' && Array.isArray(m.items)) {
      const out = [];
      for (const it of m.items.slice(0, 50)) {
        if (!it || typeof it !== 'object' || !Array.isArray(it.parts)) continue;
        out.push({
          source: this.src.id, platform: this.src.platform,
          id: String(it.id || '').slice(0, 128), author: String(it.author || '').slice(0, 200),
          color: typeof it.color === 'string' ? it.color.slice(0, 7) : '',
          parts: it.parts.slice(0, 80).map(x => (x && x.t === 'emote' ? { t: 'emote', v: String(x.v || '').slice(0, 200), u: String(x.u || '').slice(0, 600) } : { t: 'text', v: String(x && x.v || '').slice(0, 2000) })),
          badges: Array.isArray(it.badges) ? it.badges.slice(0, 6).map(b => String(b).slice(0, 40)) : [],
          t: Number(it.t) || Date.now(),
        });
      }
      if (out.length) {
        this.seenTotal += out.length;
        this.emit('messages', out);
        this._evaluate();
      }
    }
  }

  _evaluate() {
    if (this.stopped && this.state === 'error') return;
    if (this._failed) return;
    const r = evaluateHealth({
      now: Date.now(), loadedAt: this.loadedAt, tick: this.tick,
      containerTimeoutMs: (this.pack.containerTimeoutSec || 45) * 1000, quietMs: this.quietMs,
      seenTotal: this.seenTotal, loginRedirect: this.loginRedirect,
    });
    this._set(r.state, r.detail);
  }

  _fail(reason) {
    if (this.stopped || this._failed) return;
    this._failed = true;
    clearTimeout(this._loadTimer);
    this._closeTab();
    this.attempts++;
    if (this.attempts > MAX_ATTEMPTS) {
      this.stopped = true;
      this._set('error', 'gave up after ' + MAX_ATTEMPTS + ' tries: ' + reason);
      return;
    }
    const wait = delayMs(this.attempts - 1);
    this._set('error', reason + '; trying again in ' + Math.round(wait / 1000) + ' s (try ' + this.attempts + ' of ' + MAX_ATTEMPTS + ')');
    this._timer(async () => {
      this._failed = false;
      this._reopening = true;
      this._set('starting', 'opening the page again');
      await this._open();
      this._reopening = false;
    }, wait);
  }

  _closeTab() {
    for (const u of this.unsubs.splice(0)) { try { u(); } catch (e) { /* ignore */ } }
    const { cdp, targetId, contextId } = this;
    this.targetId = null;
    this.sessionId = null;
    this.contextId = null;
    if (cdp && !cdp.closed) {
      if (targetId) cdp.send('Target.closeTarget', { targetId }).catch(() => {});
      if (contextId) cdp.send('Target.disposeBrowserContext', { browserContextId: contextId }).catch(() => {});
    }
  }

  async stop() {
    this.stopped = true;
    clearInterval(this.healthTimer);
    clearTimeout(this._loadTimer);
    for (const t of this.timers) clearTimeout(t);
    this.timers.clear();
    this._closeTab();
    this.state = 'stopped';
    this.detail = '';
  }
}

// The engine runs only in the top frame of the page (ad and tracker iframes get nothing).
function guardTop(src) {
  return 'if (window.top === window) { ' + src + ' }';
}

module.exports = { Source, evaluateHealth, MAX_ATTEMPTS };
