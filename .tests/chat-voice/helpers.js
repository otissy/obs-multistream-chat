'use strict';
// Plumbing for the chat voice tests: the REAL C++ server (oms_chat_fixture: HttpServer + ChatHub +
// VoiceHub serving the real data/overlay files) and Playwright's headless Chromium. Everything is
// 127.0.0.1 only; nothing is played out loud (Chromium runs with a fake audio sink, see launch; the
// page's own output is tapped and recorded instead).
//
// OBS's browser source is CEF; a plain Chromium has no window.obsstudio. The "obs" mode therefore adds
// a stand-in window.obsstudio before the page loads (the only thing the page reads from it is that it
// exists, plus the obsSourceActiveChanged event, which the tests dispatch the way obs-browser does)
// and launches with --autoplay-policy=no-user-gesture-required like OBS. The "browser" mode is an
// ordinary browser: --autoplay-policy=user-gesture-required, no obsstudio.
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..');
const OVERLAY_DIR = process.env.OMSC_OVERLAY_DIR || path.join(ROOT, 'data', 'overlay');
const BUILD_ROOT = process.env.OMSC_BUILD_ROOT || 'D:/builds/obs-multistream-chat';
const PROOF_DIR = process.env.OMS_PROOF_DIR || path.join(ROOT, '..', 'chat-voice-proof');
const KEY = 'a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90';
const VIEWPORTS = [{ name: '1920x1080', width: 1920, height: 1080 }, { name: '390x844', width: 390, height: 844 }];

function loadPlaywright() {
  const tries = [process.env.PLAYWRIGHT_MODULE, 'playwright'];
  const cache = path.join(process.env.LOCALAPPDATA || '', 'npm-cache', '_npx');
  try {
    for (const d of fs.readdirSync(cache)) tries.push(path.join(cache, d, 'node_modules', 'playwright'));
  } catch (e) { /* no npx cache */ }
  for (const t of tries) {
    if (!t) continue;
    try { return require(t); } catch (e) { /* next */ }
  }
  return null;
}
const pw = loadPlaywright();

function findFixture() {
  const c = [path.join(BUILD_ROOT, 'windows-x64', 'Release', 'oms_chat_fixture.exe'), path.join(BUILD_ROOT, 'tests-vs', 'Release', 'oms_chat_fixture.exe')];
  return c.find(p => fs.existsSync(p)) || null;
}

const skipReason = !pw ? 'Playwright not found (set PLAYWRIGHT_MODULE)' : (!findFixture() ? 'oms_chat_fixture not built' : false);

// The real server. cmd(line) sends one stdin command and resolves with the next stdout line.
async function startServer() {
  const exe = findFixture();
  const env = Object.assign({}, process.env, { OMSC_ACCESS_KEY: KEY, OMSC_CHAT_TOKEN: 'voice-test-token-0123456789abcdef' });
  const proc = spawn(exe, [OVERLAY_DIR], { stdio: ['pipe', 'pipe', 'pipe'], env, windowsHide: true });
  let buf = '';
  const waiters = [];
  const lines = [];
  proc.stdout.on('data', d => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i).replace(/\r$/, '');
      buf = buf.slice(i + 1);
      if (waiters.length) waiters.shift()(line); else lines.push(line);
    }
  });
  let err = '';
  proc.stderr.on('data', d => { err += d.toString(); });
  const next = () => new Promise((resolve, reject) => {
    if (lines.length) return resolve(lines.shift());
    const t = setTimeout(() => reject(new Error('fixture silent; stderr: ' + err)), 10000);
    waiters.push(l => { clearTimeout(t); resolve(l); });
  });
  const first = await next();
  const m = /^PORT (\d+)$/.exec(first);
  if (!m) throw new Error('fixture did not start: ' + first + err);
  const port = Number(m[1]);
  let alive = true;
  proc.on('exit', () => { alive = false; });
  const cmd = async line => { proc.stdin.write(line + '\n'); return next(); };
  return {
    port, proc,
    base: 'http://127.0.0.1:' + port + '/k/' + KEY + '/',
    async tone(author, ms = 600, hz = 440, amp = 0.5, platform = 'twitch', rate = 24000) {
      const l = await cmd(['tone', author, platform, rate, ms, hz, amp].join(' '));
      return Number(/^CLIP (\d+)$/.exec(l)[1]);
    },
    async wav(author, file, platform = 'twitch') {
      const l = await cmd(['wav', author, platform, file].join(' '));
      return Number(/^CLIP (\d+)$/.exec(l)[1]);
    },
    async flush() { return cmd('vflush'); },
    async status() { return JSON.parse(await cmd('vstatus')); },
    get alive() { return alive; },
    async stop() {
      if (!alive) return;
      const done = new Promise(r => proc.once('exit', r));
      try { proc.stdin.write('quit\n'); } catch (e) { /* gone */ }
      const t = setTimeout(() => { try { proc.kill(); } catch (e) { /* gone */ } }, 3000);
      await done;
      clearTimeout(t);
    },
  };
}

async function launch(mode) {
  // --disable-audio-output: a fake sink that still renders in real time, so nothing can ever reach this
  // PC's speakers (headless Chromium otherwise opens the real default device); --mute-audio as a second guard.
  const args = [mode === 'obs' ? '--autoplay-policy=no-user-gesture-required' : '--autoplay-policy=user-gesture-required',
    '--disable-audio-output', '--mute-audio'];
  return pw.chromium.launch({ headless: true, args });
}

// One page of the chat overlay. mode 'obs' adds the stand-in window.obsstudio.
async function openPage(browser, url, { mode = 'obs', viewport = VIEWPORTS[0] } = {}) {
  const context = await browser.newContext({ viewport: { width: viewport.width, height: viewport.height } });
  if (mode === 'obs') await context.addInitScript(() => { window.obsstudio = { pluginVersion: 'test-stand-in' }; });
  const page = await context.newPage();
  const requests = [];
  page.on('request', r => requests.push(r.url()));
  const errors = [];
  page.on('pageerror', e => errors.push(String(e)));
  await page.goto(url);
  return { page, context, requests, errors };
}

const sleep = ms => new Promise(r => setTimeout(r, ms));
async function waitFor(fn, ms = 8000, step = 50) {
  const end = Date.now() + ms;
  let last;
  while (Date.now() < end) {
    last = await fn();
    if (last) return last;
    await sleep(step);
  }
  return last;
}
const vstate = page => page.evaluate(() => window.__omscVoice && window.__omscVoice.state());

// Records the page's real output (after its volume) for ms milliseconds while fn runs.
async function record(page, fn, ms) {
  await page.evaluate(() => window.__omscVoice.capture());
  if (fn) await fn();
  await sleep(ms);
  const r = await page.evaluate(() => window.__omscVoice.stopCapture());
  const b = Buffer.from(r.b64, 'base64');
  const samples = new Int16Array(b.buffer, b.byteOffset, b.length / 2);
  return { rate: r.rate, samples };
}
function levels(samples, rate) {
  let peak = 0, sum = 0, first = -1, last = -1;
  for (let i = 0; i < samples.length; i++) {
    const a = Math.abs(samples[i]);
    if (a > peak) peak = a;
    sum += samples[i] * samples[i];
    if (a > 300) { if (first < 0) first = i; last = i; }
  }
  const rms = samples.length ? Math.sqrt(sum / samples.length) : 0;
  return { peak, rms, voicedSec: first < 0 ? 0 : (last - first + 1) / rate, totalSec: samples.length / rate };
}
function writeWav(file, samples, rate) {
  const data = Buffer.from(samples.buffer, samples.byteOffset, samples.length * 2);
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVEfmt ', 8); h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22); h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * 2, 28);
  h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.concat([h, data]));
}

// Three chat lines through the page's own renderer, so the layout can be compared voice on / off.
async function addLines(page) {
  await page.evaluate(() => {
    const parts = t => [{ t: 'text', v: t }];
    window.__omsc.add({ platform: 'twitch', author: 'alice', color: '#9146ff', parts: parts('first line') });
    window.__omsc.add({ platform: 'youtube', author: 'bob', parts: parts('second line, a bit longer so it may wrap on a phone sized source') });
    window.__omsc.add({ platform: 'kick', author: 'carol', parts: parts('third') });
  });
  await sleep(400); // rise animation
  return page.evaluate(() => Array.from(document.querySelectorAll('#chat .msg')).map(e => {
    const r = e.getBoundingClientRect();
    return [Math.round(r.x), Math.round(r.y), Math.round(r.width), Math.round(r.height)].join(',');
  }));
}
// The platform logos (CSS mask icons) still load: each icon file decodes to a real image.
async function logosLoad(page) {
  return page.evaluate(async () => {
    const out = {};
    for (const p of ['twitch', 'youtube', 'kick', 'tiktok', 'x']) {
      out[p] = await new Promise(res => { const i = new Image(); i.onload = () => res(i.naturalWidth); i.onerror = () => res(0); i.src = 'icons/' + p + '.svg'; });
    }
    const ico = document.querySelector('#chat .msg .ico');
    out.mask = ico ? getComputedStyle(ico).getPropertyValue('--mask') : '';
    return out;
  });
}

module.exports = { ROOT, OVERLAY_DIR, PROOF_DIR, KEY, VIEWPORTS, pw, skipReason, startServer, launch, openPage, sleep, waitFor, vstate, record, levels, writeWav, addLines, logosLoad };
