'use strict';
// Headless Chrome over the DevTools protocol, plus a mock of the plugin's local feed.
// Chrome runs headless (no window), with its own throw-away profile, and only talks to 127.0.0.1.
const { spawn } = require('child_process');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');
const OVERLAY_DIR = path.join(ROOT, 'data', 'overlay');
// the same search as the chat reader: Chrome, then Chromium, then Edge (OMSC_CHROME overrides)
const CHROME = require('../../reader/lib/chrome').findChrome() || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const PROOF_DIR = process.env.OMS_PROOF_DIR || path.join(ROOT, '..', 'overlay-proof');

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.svg': 'image/svg+xml' };

// Mirrors the contract of src/core/http_server.cpp for the overlay (static allowlist, state.json,
// SSE). The C++ server itself is tested in .tests/core; this exists so the page can be driven
// without compiling anything.
function startMockFeed() {
  let state = { v: 1, seq: 0, main_live: false, destinations: [] };
  const clients = new Set();
  const files = {};
  files['/overlay.html'] = path.join(OVERLAY_DIR, 'overlay.html');
  files['/overlay.js'] = path.join(OVERLAY_DIR, 'overlay.js');
  for (const f of fs.readdirSync(path.join(OVERLAY_DIR, 'icons'))) files['/icons/' + f] = path.join(OVERLAY_DIR, 'icons', f);
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const port = server.address().port;
    if (!['127.0.0.1:' + port, 'localhost:' + port].includes(req.headers.host || '')) { res.writeHead(403); return res.end('host'); }
    const p = u.pathname === '/' ? '/overlay.html' : u.pathname;
    if (p === '/state.json') {
      res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      return res.end(JSON.stringify(state));
    }
    if (p === '/events') {
      res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store' });
      res.write('retry: 2000\n\ndata: ' + JSON.stringify(state) + '\n\n');
      clients.add(res);
      req.on('close', () => clients.delete(res));
      return;
    }
    if (files[p]) {
      res.writeHead(200, {
        'Content-Type': TYPES[path.extname(p)] || 'application/octet-stream',
        'Cache-Control': 'no-store',
        'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; base-uri 'none'",
      });
      return res.end(fs.readFileSync(files[p]));
    }
    res.writeHead(404); res.end('nf');
  });
  return new Promise(resolve => server.listen(0, '127.0.0.1', () => resolve({
    port: server.address().port,
    url: path => `http://127.0.0.1:${server.address().port}${path || '/'}`,
    set(dests, mainLive = true) {
      state = { v: 1, seq: state.seq + 1, main_live: mainLive, destinations: dests.map((d, i) => ({ id: 'd' + (i + 1), live: d.state === 'live', ...d })) };
      for (const c of clients) c.write('data: ' + JSON.stringify(state) + '\n\n');
    },
    silenceEvents() { for (const c of clients) c.destroy(); clients.clear(); },
    close() { for (const c of clients) c.destroy(); server.close(); server.closeAllConnections(); },
  })));
}

class Browser {
  static async launch() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-chrome-'));
    const proc = spawn(CHROME, ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
      '--hide-scrollbars', '--mute-audio', '--remote-debugging-port=0', '--remote-allow-origins=http://127.0.0.1',
      '--user-data-dir=' + dir, 'about:blank'], { stdio: 'ignore' });
    const portFile = path.join(dir, 'DevToolsActivePort');
    for (let i = 0; i < 100 && !fs.existsSync(portFile); i++) await new Promise(r => setTimeout(r, 100));
    if (!fs.existsSync(portFile)) { proc.kill(); throw new Error('chrome did not start'); }
    const [port] = fs.readFileSync(portFile, 'utf8').split(/\r?\n/);
    const ver = await new Promise((res, rej) => http.get({ host: '127.0.0.1', port, path: '/json/version' }, r => { let b = ''; r.on('data', d => b += d); r.on('end', () => res(JSON.parse(b))); }).on('error', rej));
    const ws = new WebSocket(ver.webSocketDebuggerUrl);
    await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
    const b = new Browser(proc, ws, dir);
    ws.onmessage = ev => b._onmsg(JSON.parse(ev.data));
    return b;
  }
  constructor(proc, ws, dir) { this.proc = proc; this.ws = ws; this.dir = dir; this.id = 0; this.pending = new Map(); this.events = []; }
  _onmsg(m) {
    if (m.id && this.pending.has(m.id)) { const { res, rej } = this.pending.get(m.id); this.pending.delete(m.id); m.error ? rej(new Error(JSON.stringify(m.error))) : res(m.result); }
    else if (m.method) this.events.push(m);
  }
  send(method, params = {}, sessionId) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params, sessionId }));
    return new Promise((res, rej) => this.pending.set(id, { res, rej }));
  }
  async page(width, height, transparent = false) {
    const { targetId } = await this.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await this.send('Target.attachToTarget', { targetId, flatten: true });
    const s = (m, p) => this.send(m, p, sessionId);
    await s('Page.enable'); await s('Runtime.enable');
    await s('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
    if (transparent) await s('Emulation.setDefaultBackgroundColorOverride', { color: { r: 0, g: 0, b: 0, a: 0 } });
    const self = this;
    return {
      async goto(url) { await s('Page.navigate', { url }); await new Promise(r => setTimeout(r, 700)); },
      async eval(expr) { const r = await s('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }); if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails)); return r.result.value; },
      async shot(file) { const r = await s('Page.captureScreenshot', { format: 'png' }); fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, Buffer.from(r.data, 'base64')); return file; },
      async close() { await self.send('Target.closeTarget', { targetId }); },
    };
  }
  async close() { try { this.ws.close(); } catch (e) { /* ignore */ } this.proc.kill(); await new Promise(r => setTimeout(r, 300)); try { fs.rmSync(this.dir, { recursive: true, force: true }); } catch (e) { /* profile dir may still be locked */ } }
}

module.exports = { ROOT, OVERLAY_DIR, PROOF_DIR, startMockFeed, Browser, chromeAvailable: () => fs.existsSync(CHROME) };
