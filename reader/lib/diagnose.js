'use strict';
// `node reader/index.js --diagnose --config <file>`: loads each configured chat page ONCE in the same
// kind of headless Chrome and prints only STRUCTURAL facts as JSON lines, so a layout problem can be
// found without sharing chat: selector match counts, the tag/class path of the chat container, the
// tag/class shape of the first message row, data-attribute NAMES (never values), iframe hostnames,
// and which of the pack's state words appear (yes/no). Never prints message text, author names,
// attribute values, ids with digits, URLs with queries or anything typed by a viewer.
// It uses no token and posts nothing anywhere.
const { launch } = require('./chrome');
const { pageUrl, allowedHosts } = require('./adapters');
const { classifyNavigation } = require('./source');
const { computeUserAgent } = require('./useragent');

// Runs inside the page (read-only). cfg = the pack's engine block.
function probe(cfg) {
  var arr = function (x) { return Array.isArray(x) ? x : (x ? [x] : []); };
  function count(sels) {
    return arr(sels).map(function (s) { try { return { sel: s, n: document.querySelectorAll(s).length }; } catch (e) { return { sel: s, n: 'invalid selector' }; } });
  }
  function safeId(id) { return /^[A-Za-z][A-Za-z0-9_-]{0,30}$/.test(id) && !/\d{4,}/.test(id) ? id : undefined; }
  function cls(el) {
    var c = el.getAttribute && el.getAttribute('class');
    return c ? c.split(/\s+/).filter(function (x) { return x && !/\d{5,}/.test(x); }).slice(0, 5).join('.') : '';
  }
  function shape(el, depth) {
    if (!el) return null;
    var o = { tag: el.tagName.toLowerCase(), id: safeId(el.id || ''), cls: cls(el), children: el.children.length };
    if (depth > 0) o.kids = Array.prototype.slice.call(el.children, 0, 4).map(function (c) { return shape(c, depth - 1); });
    return o;
  }
  function path(el) {
    var out = [];
    for (var i = 0; el && el.tagName && i < 5; i++, el = el.parentElement) out.push(el.tagName.toLowerCase() + (safeId(el.id || '') ? '#' + el.id : '') + (cls(el) ? '.' + cls(el) : ''));
    return out;
  }
  function first(sels) {
    var l = arr(sels);
    for (var i = 0; i < l.length; i++) { try { var e = document.querySelector(l[i]); if (e) return e; } catch (x) { /* skip */ } }
    return null;
  }
  var container = first(cfg.container);
  var itemSel = cfg.item || cfg.anchor;
  var item = first(itemSel);
  var bodyText = '';
  try { bodyText = (document.body && document.body.innerText || '').slice(0, 6000).toLowerCase(); } catch (e) { bodyText = ''; }
  var notes = arr(cfg.notes).map(function (n) {
    return { id: n.id, selector_hit: !!(n.sel && n.sel.length && first(n.sel)), words_hit: arr(n.text).map(function (t) { return bodyText.indexOf(String(t).toLowerCase()) >= 0; }) };
  });
  var frames = [];
  Array.prototype.forEach.call(document.querySelectorAll('iframe'), function (f) { try { frames.push(new URL(f.src, location.href).hostname); } catch (e) { frames.push('(none)'); } });
  var attrNames = item ? Array.prototype.map.call(item.attributes, function (a) { return a.name; }) : [];
  return {
    ready: document.readyState, host: location.hostname, path: location.pathname.replace(/\/[^/]*\d{4,}[^/]*/g, '/{id}').slice(0, 80),
    elements: document.getElementsByTagName('*').length, body_text_chars: bodyText.length, iframes: frames.slice(0, 10),
    container: { found: !!container, path: container ? path(container) : null, children: container ? container.children.length : 0, shape: shape(container, 2) },
    counts: {
      container: count(cfg.container), item: count(cfg.item), anchor: count(cfg.anchor),
      author: count(cfg.author && cfg.author.sel), body: count(cfg.body && cfg.body.sel), emote: count(cfg.emote && cfg.emote.sel),
      badge: count(cfg.badge && cfg.badge.sel), ignore: count(cfg.ignore), login: count(cfg.login && cfg.login.sel),
    },
    first_item: item ? { shape: shape(item, 4), attribute_names: attrNames.slice(0, 20) } : null,
    notes: notes,
  };
}

async function diagnose(cfg, packs, errors, opts, print) {
  const sources = cfg.sources.filter(s => packs[s.platform]);
  for (const s of cfg.sources) if (!packs[s.platform]) print({ source: s.id, platform: s.platform, error: errors[s.platform] || 'no adapter' });
  if (!sources.length) return;
  const browser = await launch({ chrome: opts.chrome, images: false, env: opts.env });
  const mode = opts.userAgentMode || 'chrome';
  let ua = null;
  try { ua = computeUserAgent(await browser.cdp.send('Browser.getVersion'), mode); } catch (e) { ua = null; }
  print({ user_agent_mode: mode, user_agent_applied: !!ua, chrome_version: ua ? ua.version : undefined });
  const wait = Math.max(5, Math.min(120, opts.waitSec || 25)) * 1000;
  try {
    for (const s of sources) {
      const pack = packs[s.platform];
      const cdp = browser.cdp;
      const ctx = await cdp.send('Target.createBrowserContext', {});
      const t = await cdp.send('Target.createTarget', { url: 'about:blank', browserContextId: ctx.browserContextId });
      const a = await cdp.send('Target.attachToTarget', { targetId: t.targetId, flatten: true });
      const sid = a.sessionId;
      const c = (m, p) => cdp.send(m, p || {}, sid);
      const hosts = allowedHosts(pack, opts.testOrigin);
      const nav = [];
      cdp.on('Page.frameNavigated', p => {
        if (p.frame && !p.frame.parentId) {
          let u; try { u = new URL(p.frame.url); } catch (e) { return; }
          if (u.protocol !== 'about:') nav.push({ host: u.hostname, path: u.pathname.slice(0, 60), kind: classifyNavigation(pack, u.hostname, hosts) });
        }
      }, sid);
      try {
        await c('Page.enable'); await c('Runtime.enable'); await c('Network.enable');
        await c('Emulation.setDeviceMetricsOverride', { width: 900, height: 1000, deviceScaleFactor: 1, mobile: false });
        if (ua) await c('Emulation.setUserAgentOverride', { userAgent: ua.userAgent, userAgentMetadata: ua.userAgentMetadata });
        const cookies = [];
        for (const ck of pack.cookies || []) {
          const r = await c('Network.setCookie', { name: ck.name, value: ck.value, domain: ck.domain, path: '/', secure: true, sameSite: 'None' });
          cookies.push({ name: ck.name, accepted: !!r.success });
        }
        const url = pageUrl(pack, s.target, opts.testOrigin);
        const t0 = Date.now();
        const r = await c('Page.navigate', { url });
        const snaps = [];
        for (const at of [Math.round(wait * 0.4), wait]) {
          await new Promise(res => setTimeout(res, Math.max(0, at - (Date.now() - t0))));
          const ev = await c('Runtime.evaluate', { expression: '(' + probe.toString() + ')(' + JSON.stringify(pack.engine) + ')', returnByValue: true });
          const facts = ev.result && ev.result.value;
          if (facts) {
            facts.stub = facts.elements < 20 && !facts.container.found; // an empty page: see docs/limits.md
            if (facts.stub) facts.state_guess = 'stub: platform served an empty page: likely automation detection or an unavailable stream';
          }
          snaps.push({ after_s: Math.round((Date.now() - t0) / 1000), facts, error: ev.exceptionDetails ? 'probe failed' : undefined });
        }
        print({ source: s.id, platform: s.platform, navigate_error: r.errorText || null, cookies_set: cookies, main_frame_navigations: nav, snapshots: snaps });
      } catch (e) {
        print({ source: s.id, platform: s.platform, error: String(e && e.message || e).slice(0, 200) });
      }
      try { await cdp.send('Target.closeTarget', { targetId: t.targetId }); await cdp.send('Target.disposeBrowserContext', { browserContextId: ctx.browserContextId }); } catch (e) { /* ignore */ }
    }
  } finally { await browser.close(); }
}

module.exports = { diagnose, probe };
