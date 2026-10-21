'use strict';
// Reader benchmark (Windows): boot time, YouTube live detection, memory and CPU of the reader's
// browser, with 1, 3 and 5 chat sources against the local test platform (no internet), and
// optionally against real public pages (--live).
//   node scripts/bench-reader.js [--live @channel] [--twitch channel] [--seconds 30] [--out file.json]
// Nothing here logs in or posts anything; real pages are only opened, like the reader does.
const { execFileSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const h = require('../.tests/chat/helpers');
const { BUILD } = require('../.tests/chat/fixtures/dom');
const { start } = require('../reader/index');


const args = process.argv.slice(2);
const arg = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const SECONDS = Number(arg('--seconds', 30));

// The browser processes THIS benchmark started: descendants of this node process only. (Matching the
// profile prefix alone also catches a reader that a running OBS started, e.g. the user's real one.)
function browserProcs() {
  const ps = 'Get-CimInstance Win32_Process | ForEach-Object { "$($_.ProcessId) $($_.ParentProcessId) $($_.WorkingSetSize) ' +
    '$([long]$_.KernelModeTime + [long]$_.UserModeTime) $($_.Name)" }';
  const out = execFileSync('powershell', ['-NoProfile', '-Command', ps], { encoding: 'utf8', maxBuffer: 1 << 24 });
  const rows = out.trim().split(/\r?\n/).filter(Boolean).map(l => { const p = l.split(' '); return { pid: +p[0], ppid: +p[1], ws: +p[2], cpu100ns: +p[3], name: p.slice(4).join(' ') }; });
  const mine = new Set([process.pid]);
  for (let grew = true; grew;) {
    grew = false;
    for (const r of rows) if (!mine.has(r.pid) && mine.has(r.ppid)) { mine.add(r.pid); grew = true; }
  }
  return rows.filter(r => r.pid !== process.pid && mine.has(r.pid) && /^(chrome|msedge)\.exe$/i.test(r.name));
}
function snapshot() {
  const b = browserProcs();
  const self = process.memoryUsage().rss;
  const cpuSelf = process.cpuUsage();
  return {
    t: Date.now(), procs: b.length,
    browserMB: b.reduce((a, p) => a + p.ws, 0) / 1048576,
    readerMB: self / 1048576,
    browserCpuMs: b.reduce((a, p) => a + p.cpu100ns, 0) / 10000,
    readerCpuMs: (cpuSelf.user + cpuSelf.system) / 1000,
  };
}
function cpuPct(a, b) {
  const wall = (b.t - a.t) * os.cpus().length;
  return { browser: +(100 * (b.browserCpuMs - a.browserCpuMs) / wall).toFixed(2), reader: +(100 * (b.readerCpuMs - a.readerCpuMs) / wall).toFixed(2) };
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
function cfgFile(port, sources) {
  const f = path.join(os.tmpdir(), 'oms-bench-' + process.pid + '-' + Math.random().toString(16).slice(2) + '.json');
  fs.writeFileSync(f, JSON.stringify({ v: 1, port, quiet_minutes: 5, sources }));
  return f;
}

async function localRun(n) {
  const fake = await h.startFake();
  const stub = await h.startStubMerge();
  const plats = ['twitch', 'kick', 'youtube'];
  const sources = [];
  for (let i = 0; i < n; i++) {
    const p = plats[i % plats.length];
    const target = p === 'youtube' ? '@benchchan' + i : 'chan' + i;
    if (p === 'youtube') { const vid = ('Bench' + i + 'Vid000').slice(0, 11); fake.setChannel('benchchan' + i, vid); fake.register('youtube', vid, 'youtube'); sources.push({ id: 'c' + (i + 1), platform: p, target, vid }); }
    else { fake.register(p, target, p); sources.push({ id: 'c' + (i + 1), platform: p, target }); }
  }
  const cfg = cfgFile(stub.port, sources.map(({ vid, ...s }) => s));
  const t0 = Date.now();
  const ctl = await start({ config: cfg, testOrigin: fake.origin }, Object.assign({}, process.env, { OMSC_CHAT_TOKEN: h.TOKEN }), {});
  const r = { sources: n };
  try {
    await h.waitFor(() => sources.every(s => stub.status(s.id) && stub.status(s.id).state === 'reading'), 60000, 50);
    r.bootMs = Date.now() - t0;
    const yt = sources.find(s => s.platform === 'youtube');
    if (yt) r.youtubeChannelToReadingMs = r.bootMs; // includes the /live lookup
    await sleep(3000);
    const idleA = snapshot(); await sleep(SECONDS * 1000); const idleB = snapshot();
    r.idle = { browserMB: +idleB.browserMB.toFixed(1), readerMB: +idleB.readerMB.toFixed(1), processes: idleB.procs, cpuPct: cpuPct(idleA, idleB) };
    // busy: 10 chat lines per second across the sources
    const busyA = snapshot();
    const end = Date.now() + SECONDS * 1000;
    let k = 0;
    while (Date.now() < end) {
      const s = sources[k % sources.length];
      const target = s.platform === 'youtube' ? s.vid : s.target;
      fake.append(s.platform, target, BUILD[s.platform]({ id: 'b' + k, author: 'User' + (k % 50), segments: ['message number ' + k + ' with some words in it'] }));
      k++; await sleep(100);
    }
    await sleep(1500);
    const busyB = snapshot();
    r.busy = { linesSent: k, linesMerged: stub.messages().length, browserMB: +busyB.browserMB.toFixed(1), readerMB: +busyB.readerMB.toFixed(1), cpuPct: cpuPct(busyA, busyB) };
  } finally {
    await ctl.shutdown(0); await ctl.done; fake.close(); stub.close(); fs.rmSync(cfg, { force: true });
  }
  return r;
}

async function liveRun(channel, twitch) {
  const stub = await h.startStubMerge();
  const sources = [{ id: 'c1', platform: 'youtube', target: channel }];
  if (twitch) sources.push({ id: 'c2', platform: 'twitch', target: twitch });
  const cfg = cfgFile(stub.port, sources);
  const t0 = Date.now();
  const ctl = await start({ config: cfg }, Object.assign({}, process.env, { OMSC_CHAT_TOKEN: h.TOKEN }), {});
  const r = { channel, twitch: twitch || null };
  try {
    await h.waitFor(() => { const s = stub.status('c1'); return s && ['reading', 'offline', 'consent', 'layout', 'stub', 'disabled', 'error'].includes(s.state); }, 120000, 100);
    r.youtubeFirstVerdictMs = Date.now() - t0;
    r.youtubeState = stub.status('c1').state;
    if (twitch) { await h.waitFor(() => stub.status('c2') && stub.status('c2').state !== 'starting', 120000, 100); r.twitchFirstVerdictMs = Date.now() - t0; r.twitchState = stub.status('c2').state; }
    await sleep(3000);
    const a = snapshot(); await sleep(SECONDS * 1000); const b = snapshot();
    r.steady = { browserMB: +b.browserMB.toFixed(1), readerMB: +b.readerMB.toFixed(1), processes: b.procs, cpuPct: cpuPct(a, b), linesMerged: stub.messages().length };
  } finally {
    await ctl.shutdown(0); await ctl.done; stub.close(); fs.rmSync(cfg, { force: true });
  }
  return r;
}

(async () => {
  const out = { when: new Date().toISOString(), cpu: os.cpus()[0].model, cores: os.cpus().length, ramGB: +(os.totalmem() / 1073741824).toFixed(1), node: process.version, seconds: SECONDS, local: [] };
  for (const n of (arg('--counts') || '1,3,5').split(',').map(Number)) { const r = await localRun(n); out.local.push(r); console.log(JSON.stringify(r)); }
  if (arg('--live')) { out.live = await liveRun(arg('--live'), arg('--twitch')); console.log(JSON.stringify(out.live)); }
  if (arg('--out')) fs.writeFileSync(arg('--out'), JSON.stringify(out, null, 1));
  console.log('done');
})().catch(e => { console.error(e); process.exit(1); });
