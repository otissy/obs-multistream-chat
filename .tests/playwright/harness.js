'use strict';
// Shared harness for the Playwright suite: starts the REAL C++ HttpServer + ChatHub
// (oms_chat_fixture, built by the core ladder) serving the REAL data/overlay files, with a fresh
// random access key and reader token per test, and feeds it through the reader's own route
// (POST /chat/ingest, bearer token). Nothing in the page or the server is replaced.
//
// Environment (all optional):
//   OMSC_BUILD_ROOT     where the core ladder built oms_chat_fixture (default D:/builds/obs-multistream-chat)
//   OMS_FIXTURE_EXE     a specific fixture binary (used to prove a test fails on an older build)
//   OMS_OVERLAY_DIR     a specific overlay folder (used to prove a test fails on a broken page)
//   OMS_BROKEN_NO_KEY=1 start the server WITHOUT the key while the tests still use it: the
//                       "key check removed" variant, only for proving the key tests fail
const { spawn } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');
const OVERLAY_DIR = path.resolve(process.env.OMS_OVERLAY_DIR || path.join(ROOT, 'data', 'overlay'));
const BUILD_ROOT = path.resolve(process.env.OMSC_BUILD_ROOT || 'D:/builds/obs-multistream-chat');
const TW_EMOTE = 'https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/1.0';

function fixtureExe() {
  if (process.env.OMS_FIXTURE_EXE) return process.env.OMS_FIXTURE_EXE;
  const c = [path.join(BUILD_ROOT, 'tests-vs', 'Release', 'oms_chat_fixture.exe'), path.join(BUILD_ROOT, 'windows-x64', 'Release', 'oms_chat_fixture.exe'), path.join(BUILD_ROOT, 'tests', 'oms_chat_fixture')];
  const f = c.find(p => fs.existsSync(p));
  if (!f) throw new Error('oms_chat_fixture is not built: run the core ladder first (OMSC_BUILD_ROOT=' + BUILD_ROOT + ')');
  return f;
}

async function startServer() {
  const key = crypto.randomBytes(32).toString('hex');
  const token = crypto.randomBytes(24).toString('hex');
  const env = Object.assign({}, process.env, { OMSC_CHAT_TOKEN: token });
  if (process.env.OMS_BROKEN_NO_KEY === '1') delete env.OMSC_ACCESS_KEY; else env.OMSC_ACCESS_KEY = key;
  const proc = spawn(fixtureExe(), [OVERLAY_DIR], { stdio: ['pipe', 'pipe', 'pipe'], env, windowsHide: true });
  let err = '';
  let out = '';
  const waiters = [];
  proc.stdout.on('data', d => { out += d; let i; while ((i = out.indexOf('\n')) >= 0 && waiters.length) { const line = out.slice(0, i).trim(); out = out.slice(i + 1); waiters.shift()(line); } });
  proc.stderr.on('data', d => { err += d; });
  const port = await new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error('fixture silent: ' + err)), 30000); // below-normal priority on a busy PC
    waiters.push(line => { clearTimeout(t); const m = /PORT (\d+)/.exec(line); m ? res(+m[1]) : rej(new Error('fixture said ' + line)); });
    proc.on('exit', c => rej(new Error('fixture exited ' + c + ': ' + err)));
  });
  let seq = 0;
  const tag = crypto.randomBytes(3).toString('hex');
  const base = `http://127.0.0.1:${port}`;
  const srv = {
    port, key, base,
    keyed: p => `${base}/k/${key}${p}`,
    plain: p => `${base}${p}`,
    // posts messages through the reader's route; spreads them over sources to stay under the per-source burst
    async ingest(msgs) {
      const out = msgs.map((m, i) => Object.assign({ platform: 'twitch', source: 'c' + (1 + ((seq + i) % 8)), id: `${tag}-${seq + i}`, author: 'viewer' + (seq + i + 1), color: '', badges: [], t: Date.now() }, m));
      seq += msgs.length;
      for (let i = 0; i < out.length; i += 100) {
        const body = JSON.stringify({ v: 1, sent: Date.now(), sources: [], messages: out.slice(i, i + 100) });
        const r = await request('POST', port, '/chat/ingest', body, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body), Authorization: 'Bearer ' + token });
        if (r.status !== 200) throw new Error('ingest ' + r.status + ' ' + r.body);
        const j = JSON.parse(r.body);
        if (j.accepted !== Math.min(100, out.length - i)) throw new Error('ingest accepted ' + r.body);
      }
      await new Promise(r => setTimeout(r, 450)); // the hub's 300 ms reordering window
      return out;
    },
    // fixture command (dest <platform> <handle> <state> | dests-clear), waits for its OK
    cmd(c) { return new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('fixture silent on ' + c)), 10000); waiters.push(l => { clearTimeout(t); res(l); }); proc.stdin.write(c + '\n'); }); },
    async stop() {
      try { proc.stdin.write('quit\n'); } catch (e) { /* gone */ }
      await new Promise(r => { const t = setTimeout(() => { proc.kill(); r(); }, 3000); proc.on('exit', () => { clearTimeout(t); r(); }); });
    },
  };
  return srv;
}

// Raw HTTP, so headers and the Host line are exactly what we say (no browser in between).
function request(method, port, p, body, headers = {}, maxBytes = 65536) {
  return new Promise((res, rej) => {
    const r = http.request({ host: '127.0.0.1', port, path: p, method, headers: Object.assign({ Host: '127.0.0.1:' + port }, headers) }, x => {
      const chunks = [];
      let n = 0;
      x.on('data', d => { chunks.push(d); n += d.length; if (n > maxBytes) x.destroy(); });
      const done = () => res({ status: x.statusCode, headers: x.headers, body: Buffer.concat(chunks).toString('utf8') });
      x.on('end', done); x.on('close', done);
    });
    r.on('error', rej);
    if (body) r.write(body);
    r.end();
  });
}

// Opens an event stream and collects text for `ms`, then closes it.
function streamFor(port, p, ms) {
  return new Promise((res, rej) => {
    const r = http.request({ host: '127.0.0.1', port, path: p, headers: { Host: '127.0.0.1:' + port } }, x => {
      let b = '';
      x.on('data', d => { b += d; });
      const t = setTimeout(() => { x.destroy(); res({ status: x.statusCode, headers: x.headers, body: b, closedEarly: false }); }, ms);
      x.on('end', () => { clearTimeout(t); res({ status: x.statusCode, headers: x.headers, body: b, closedEarly: true }); });
    });
    r.on('error', rej);
    r.end();
  });
}

// a 112x112 PNG (the size Twitch serves for 3.0 emotes), solid green
const EMOTE_PNG = (() => {
  const zlib = require('zlib');
  const w = 112, h = 112;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; for (let x = 0; x < w; x++) { const o = y * (w * 3 + 1) + 1 + x * 3; raw[o] = 30; raw[o + 1] = 160; raw[o + 2] = 40; } }
  const crcT = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crcT[n] = c >>> 0; }
  const crc = b => { let c = 0xffffffff; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const td = Buffer.concat([Buffer.from(type), data]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([len, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
})();

// Every https request of the page is answered here, never sent: emote images get the PNG after
// `delayMs` (so the line grows AFTER it was laid out), or fail when `fail` is set.
async function routeEmotes(page, { delayMs = 0, fail = false } = {}) {
  const seen = [];
  await page.route(/^https:\/\//, async route => {
    seen.push(route.request().url());
    if (delayMs) await new Promise(r => setTimeout(r, delayMs));
    if (fail) return route.fulfill({ status: 404, body: 'gone' });
    return route.fulfill({ status: 200, contentType: 'image/png', body: EMOTE_PNG });
  });
  return seen;
}

// WCAG relative-luminance contrast of two "rgb(a)(r, g, b[, a])" strings
function contrast(fg, bg) {
  const p = s => s.match(/[\d.]+/g).slice(0, 3).map(Number);
  const lin = c => { c /= 255; return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
  const L = ([r, g, b]) => 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
  const a = L(p(fg)), b = L(p(bg));
  return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
}

const PROOF_DIR = process.env.OMS_PROOF_DIR || path.join(ROOT, '..', 'playwright-proof');

module.exports = { ROOT, OVERLAY_DIR, TW_EMOTE, EMOTE_PNG, PROOF_DIR, startServer, request, streamFor, routeEmotes, contrast };
