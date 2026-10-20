'use strict';
// Shared test plumbing for the chat feature. Everything runs on 127.0.0.1 only: a LOCAL fake
// "platform" that serves the fixtures at the real URL paths, a stub merge server, the C++ fixture
// server (real HttpServer + ChatHub), and headless Chrome.
const fs = require('fs');
const http = require('http');
const path = require('path');
const { spawn, spawnSync } = require('child_process');
const { page } = require('./fixtures/dom');

const ROOT = path.resolve(__dirname, '..', '..');
const READER = path.join(ROOT, 'reader');
const OVERLAY_DIR = path.join(ROOT, 'data', 'overlay');
const PROOF_DIR = process.env.OMS_PROOF_DIR || path.join(ROOT, '..', 'chat-proof');
const TOKEN = 'test-token-0123456789abcdef0123456789abcdef';

function chromeAvailable() { return require('../overlay/helpers').chromeAvailable(); }

function listen(server) {
  return new Promise(resolve => server.listen(0, '127.0.0.1', () => resolve(server.address().port)));
}

// ---- a local fake platform ------------------------------------------------------------------
async function startFake() {
  const routes = new Map(); // "platform:target" -> {variant, key}
  const channels = new Map(); // YouTube handle (no @) -> live video id, or '' when the channel is not live
  const feeds = new Map();  // key -> {items:[]}
  const requests = [];
  const uas = [];
  let seq = 0;
  const server = http.createServer((req, res) => {
    const port = server.address().port;
    const u = new URL(req.url, 'http://x');
    requests.push(u.pathname + u.search);
    uas.push({ path: u.pathname, ua: req.headers['user-agent'] || '', chua: req.headers['sec-ch-ua'] || '', platform: req.headers['sec-ch-ua-platform'] || '' });
    if (req.headers.host !== '127.0.0.1:' + port) { res.writeHead(403); return res.end('host'); }
    if (u.pathname === '/__feed') {
      const f = feeds.get(u.searchParams.get('key'));
      const since = parseInt(u.searchParams.get('since'), 10) || 0;
      res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      return res.end(JSON.stringify({ items: f ? f.items.slice(since) : [], next: f ? f.items.length : 0 }));
    }
    let hit = null;
    let m;
    if ((m = /^\/popout\/([^/]+)\/chat$/.exec(u.pathname))) hit = routes.get('twitch:' + m[1]) || routes.get('kick:' + m[1]);
    else if (u.pathname === '/live_chat') hit = routes.get('youtube:' + u.searchParams.get('v'));
    else if ((m = /^\/i\/broadcasts\/([^/]+)$/.exec(u.pathname))) hit = routes.get('x:' + m[1]);
    else if ((m = /^\/@([^/]+)\/live$/.exec(u.pathname)) && channels.has(m[1])) {
      // a YouTube channel's /live page: points at the live video when there is one, else the channel itself
      const vid = channels.get(m[1]);
      const canon = vid ? 'https://www.youtube.com/watch?v=' + vid : 'https://www.youtube.com/@' + m[1];
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
      return res.end('<!doctype html><html><head><link rel="canonical" href="' + canon + '"><title>channel</title></head><body>' +
        '<div>' + 'x '.repeat(40) + '</div></body></html>');
    } else if ((m = /^\/@([^/]+)\/live$/.exec(u.pathname))) hit = routes.get('tiktok:' + m[1]);
    if (!hit) { res.writeHead(404, { 'Content-Type': 'text/plain' }); return res.end('not found'); }
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(page(hit.variant, hit.key));
  });
  const port = await listen(server);
  return {
    port, origin: 'http://127.0.0.1:' + port, requests, uas,
    register(platform, target, variant = platform) {
      const key = 'k' + (++seq);
      routes.set(platform + ':' + target, { variant, key });
      feeds.set(key, { items: [] });
      return key;
    },
    // YouTube channel page: setChannel('mychan', 'dQw4w9WgXcQ') = live on that video; setChannel('mychan', '') = offline
    setChannel(handle, videoId) { channels.set(handle, videoId || ''); },
    append(platform, target, html) { feeds.get(routes.get(platform + ':' + target).key).items.push(html); },
    pageLoads(pathPrefix) { return requests.filter(r => r.startsWith(pathPrefix)).length; },
    close() { server.closeAllConnections(); server.close(); },
  };
}

// ---- a stub of the plugin's merge server (contract of POST /chat/ingest) --------------------
async function startStubMerge(opts = {}) {
  const batches = [];
  const state = { mode: 'ok', token: opts.token || TOKEN, statuses: new Map(), unauthorized: 0 };
  const server = http.createServer((req, res) => {
    const port = server.address().port;
    if (req.method !== 'POST' || req.url !== '/chat/ingest') { res.writeHead(404); return res.end(); }
    if (req.headers.host !== '127.0.0.1:' + port) { res.writeHead(403); return res.end(); }
    if (req.headers.authorization !== 'Bearer ' + state.token) { state.unauthorized++; res.writeHead(401); return res.end('{}'); }
    let b = '';
    req.on('data', d => { b += d; });
    req.on('end', () => {
      if (state.mode === 'fail') { res.writeHead(503); return res.end('{}'); }
      if (state.mode === 'bad') { res.writeHead(400); return res.end('{}'); }
      let j;
      try { j = JSON.parse(b); } catch (e) { res.writeHead(400); return res.end('{}'); }
      batches.push(j);
      for (const s of j.sources || []) state.statuses.set(s.id, s);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end('{"ok":true}');
    });
  });
  const port = await listen(server);
  return {
    port, batches, state,
    messages: () => batches.flatMap(b => b.messages || []),
    status: id => state.statuses.get(id),
    close() { server.closeAllConnections(); server.close(); },
  };
}

// ---- the C++ fixture server: real HttpServer + ChatHub ---------------------------------------
const BUILD_ROOT = path.resolve(process.env.OMSC_BUILD_ROOT || 'D:/builds/obs-multistream-chat');
function findFixtureExe() {
  const c = [path.join(BUILD_ROOT, 'tests-vs', 'Release', 'oms_chat_fixture.exe'), path.join(BUILD_ROOT, 'windows-x64', 'Release', 'oms_chat_fixture.exe'), path.join(BUILD_ROOT, 'tests', 'oms_chat_fixture')];
  return c.find(p => fs.existsSync(p)) || null;
}
async function startCppChat(token = TOKEN) {
  const exe = findFixtureExe();
  if (!exe) throw new Error('oms_chat_fixture not built');
  const proc = spawn(exe, [OVERLAY_DIR], { stdio: ['pipe', 'pipe', 'pipe'], env: Object.assign({}, process.env, { OMSC_CHAT_TOKEN: token }), windowsHide: true });
  let buf = '';
  let err = '';
  const waiters = [];
  proc.stdout.on('data', d => { buf += d; flushWaiters(); });
  proc.stderr.on('data', d => { err += d; });
  function flushWaiters() {
    for (const w of waiters.slice()) {
      const i = buf.indexOf('\n');
      if (i >= 0) { const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1); waiters.splice(waiters.indexOf(w), 1); w(line); }
    }
  }
  const nextLine = () => new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('fixture silent: ' + err)), 10000); waiters.push(l => { clearTimeout(t); res(l); }); flushWaiters(); });
  const first = await nextLine();
  const port = parseInt(first.replace('PORT ', ''), 10);
  return {
    port, proc,
    url: p => `http://127.0.0.1:${port}${p || '/'}`,
    async cmd(c) { proc.stdin.write(c + '\n'); return nextLine(); },
    async status() { return JSON.parse(await this.cmd('status')); },
    async stop() { try { proc.stdin.write('quit\n'); } catch (e) { /* gone */ } await new Promise(r => { const t = setTimeout(() => { proc.kill(); r(); }, 3000); proc.on('exit', () => { clearTimeout(t); r(); }); }); },
  };
}

function cppExe() { const h = require('../core/helpers'); return h.findExe('oms_tests'); }
// Runs oms_tests for a layer with a name filter; returns the parsed TAP-like cases.
function runCpp(layer, filter) {
  const h = require('../core/helpers');
  const exe = h.findExe('oms_tests');
  const r = spawnSync(exe, [layer, filter], { encoding: 'utf8', timeout: 180000, cwd: path.dirname(exe) });
  const cases = [];
  for (const line of (r.stdout || '').split(/\r?\n/)) {
    const m = line.match(/^(not ok|ok) \d+ - (.*)$/);
    if (m) cases.push({ ok: m[1] === 'ok', name: m[2], detail: '' });
    else if (line.startsWith('# ') && cases.length && !cases[cases.length - 1].ok) cases[cases.length - 1].detail += line + '\n';
  }
  return { cases, status: r.status, out: (r.stdout || '') + (r.stderr || '') };
}

const sleep = ms => new Promise(r => setTimeout(r, ms));
async function waitFor(fn, ms = 15000, step = 100) {
  const t0 = Date.now();
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() - t0 > ms) throw new Error('timed out waiting for condition');
    await sleep(step);
  }
}

module.exports = { ROOT, READER, OVERLAY_DIR, PROOF_DIR, TOKEN, chromeAvailable, startFake, startStubMerge, startCppChat, findFixtureExe, runCpp, cppExe, sleep, waitFor, BUILD_ROOT };
