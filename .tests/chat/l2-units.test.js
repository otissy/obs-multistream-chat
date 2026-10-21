'use strict';
// L2: each function alone, with hostile and malformed input. JS units, then every platform
// adapter driven in headless Chrome against its local fixture, then the C++ units (chat hub,
// sanitiser, routes) through oms_tests.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { READER, TOKEN, chromeAvailable, startFake, runCpp, sleep, waitFor } = require('./helpers');
const { normalizeTarget } = require('../../reader/lib/targets');
const { loadAdapters, validatePack, pageUrl, allowedHosts } = require('../../reader/lib/adapters');
const { evaluateHealth, Source } = require('../../reader/lib/source');
const { delayMs } = require('../../reader/lib/backoff');
const { Poster, shrink } = require('../../reader/lib/poster');
const { parseReaderConfig } = require('../../reader/lib/config');
const { makeLogger } = require('../../reader/lib/log');
const { parseArgs, start } = require('../../reader/index');
const { BUILD, EMOTES } = require('./fixtures/dom');

const { packs } = loadAdapters(path.join(READER, 'adapters'));
const clone = o => JSON.parse(JSON.stringify(o));

// ---- targets ----------------------------------------------------------------------------------
test('targets: every shared vector gives the same answer as the C++ implementation expects', () => {
  const vec = JSON.parse(fs.readFileSync(path.join(__dirname, 'vectors', 'targets.json'), 'utf8'));
  for (const v of vec) {
    const r = normalizeTarget(v.platform, v.input);
    assert.strictEqual(r.ok, v.ok, `[${v.platform}] ${JSON.stringify(v.input)} expected ok=${v.ok}`);
    if (v.ok) {
      assert.strictEqual(r.target, v.target, v.input);
      assert.strictEqual(pageUrl(packs[v.platform], r.target, ''), v.url, v.input);
    }
  }
});

test('targets: non-strings, control characters and huge input are refused, not thrown on', () => {
  for (const x of [null, undefined, 42, {}, [], 'a\u0000b', 'twitch\n', 'x'.repeat(5000)]) {
    assert.doesNotThrow(() => normalizeTarget('twitch', x));
  }
  assert.strictEqual(normalizeTarget('twitch', 'a\u0000bcd').ok, false);
  assert.strictEqual(normalizeTarget('twitch', 'x'.repeat(5000)).ok, false);
  assert.strictEqual(normalizeTarget('twitch', 'cafés').ok, false);
});

// ---- adapters ---------------------------------------------------------------------------------
test('adapters: a pack that points at another host, a non-https url or no {target} is refused', () => {
  const good = clone(packs.twitch);
  assert.doesNotThrow(() => validatePack(clone(good), 'twitch'));
  const bad = (mut, re) => { const p = clone(good); mut(p); assert.throws(() => validatePack(p, 'twitch'), re); };
  bad(p => { p.url = 'http://www.twitch.tv/popout/{target}/chat'; }, /https/);
  bad(p => { p.url = 'https://www.twitch.tv/popout/chat'; }, /target/);
  bad(p => { p.url = 'https://www.twitch.tv/{target}/{target}'; }, /target/);
  bad(p => { p.url = 'https://evil.example/popout/{target}/chat'; }, /not in "hosts"/);
  bad(p => { p.hosts = ['WWW.TWITCH.TV']; }, /hosts/);
  bad(p => { p.hosts = []; }, /hosts/);
  bad(p => { p.platform = 'kick'; }, /platform/);
  bad(p => { p.adapter = 2; }, /adapter/);
  bad(p => { p.experimental = 'yes'; }, /experimental/);
  bad(p => { p.containerTimeoutSec = 5; }, /containerTimeoutSec/);
  bad(p => { delete p.engine; }, /engine/);
  bad(p => { p.engine.container = []; }, /container/);
  bad(p => { p.engine.container = 'div'; }, /container/);
  bad(p => { delete p.engine.item; }, /item/);
  bad(p => { p.engine.author.color = 'red'; }, /color/);
  bad(p => { p.engine.body = {}; }, /body/);
  bad(p => { p.engine.emote.sel = 5; }, /emote/);
  bad(p => { p.engine.id.attrs = [1]; }, /attrs/);
  bad(p => { p.engine.container = ['a\nb']; }, /container/);
  assert.throws(() => validatePack(null, 'twitch'), /object/);
  assert.throws(() => validatePack([], 'twitch'), /object/);
});

test('adapters: a broken or oversized file disables only its own platform', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-adapt-'));
  try {
    for (const f of fs.readdirSync(path.join(READER, 'adapters'))) fs.copyFileSync(path.join(READER, 'adapters', f), path.join(dir, f));
    fs.writeFileSync(path.join(dir, 'kick.json'), '{ not json');
    fs.writeFileSync(path.join(dir, 'x.json'), JSON.stringify({ pad: 'x'.repeat(70000) }));
    fs.unlinkSync(path.join(dir, 'tiktok.json'));
    const r = loadAdapters(dir);
    assert.deepStrictEqual(Object.keys(r.packs).sort(), ['twitch', 'youtube']);
    assert.match(r.errors.kick, /kick\.json/);
    assert.match(r.errors.x, /larger than/);
    assert.match(r.errors.tiktok, /tiktok\.json/);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test('adapters: the test origin is loopback-only and keeps the real path; no origin means the real url', () => {
  const p = packs.youtube;
  assert.strictEqual(pageUrl(p, 'dQw4w9WgXcQ', ''), 'https://www.youtube.com/live_chat?is_popout=1&v=dQw4w9WgXcQ');
  assert.strictEqual(pageUrl(p, 'dQw4w9WgXcQ', 'http://127.0.0.1:4555'), 'http://127.0.0.1:4555/live_chat?is_popout=1&v=dQw4w9WgXcQ');
  for (const o of ['http://localhost:4555', 'http://127.0.0.1', 'https://127.0.0.1:4555', 'http://127.0.0.1:4555/x', 'http://evil.example:80', 'http://127.0.0.1.evil.example:4555']) {
    assert.throws(() => pageUrl(p, 'dQw4w9WgXcQ', o), /test origin/, o);
  }
  assert.throws(() => pageUrl(p, 'not valid', ''), /target/);
  assert.throws(() => pageUrl(packs.twitch, 'Shroud', ''), /target/); // must already be normalized
  assert.deepStrictEqual(allowedHosts(p, ''), ['www.youtube.com']);
  assert.deepStrictEqual(allowedHosts(p, 'http://127.0.0.1:4555'), ['127.0.0.1']);
});

// ---- health decision --------------------------------------------------------------------------
test('health: states follow the evidence', () => {
  const base = { now: 100000, loadedAt: 90000, containerTimeoutMs: 45000, quietMs: 300000, seenTotal: 0, loginRedirect: false };
  const ev = o => evaluateHealth(Object.assign({}, base, o));
  assert.strictEqual(ev({ loadedAt: 0 }).state, 'starting');
  assert.strictEqual(ev({ tick: null }).state, 'starting');
  assert.strictEqual(ev({ tick: { container: false, login: false, seen: 0, sinceFound: 0 } }).state, 'starting');
  const layout = ev({ now: 90000 + 46000, tick: { container: false, login: false, seen: 0, sinceFound: 0 } });
  assert.strictEqual(layout.state, 'layout');
  assert.match(layout.detail, /page layout changed or login needed/);
  assert.strictEqual(ev({ now: 90000 + 9000, tick: { container: false, login: true, seen: 0, sinceFound: 0 } }).state, 'login');
  assert.strictEqual(ev({ now: 90000 + 3000, tick: { container: false, login: true, seen: 0, sinceFound: 0 } }).state, 'starting'); // give the page a moment
  assert.strictEqual(ev({ loginRedirect: true }).state, 'login');
  assert.strictEqual(ev({ loadedAt: 0, loginRedirect: true }).state, 'login');
  assert.strictEqual(ev({ tick: { container: true, login: false, seen: 0, sinceFound: 1000 } }).state, 'reading');
  const quiet = ev({ tick: { container: true, login: false, seen: 0, sinceFound: 301000 } });
  assert.strictEqual(quiet.state, 'quiet');
  assert.match(quiet.detail, /layout changed or login is needed/);
  assert.strictEqual(ev({ seenTotal: 3, tick: { container: true, login: false, seen: 3, sinceFound: 999999 } }).state, 'reading'); // silence after messages is fine
  assert.strictEqual(ev({ seenTotal: 3, tick: { container: false, login: false, seen: 3, sinceFound: 0 }, now: 90000 + 1000 }).state, 'starting');
});

test('backoff: grows, stays capped, never below one second, jitter stays inside 25 percent', () => {
  const mid = () => 0.5;
  assert.strictEqual(delayMs(0, 5000, 300000, mid), 5000);
  assert.strictEqual(delayMs(1, 5000, 300000, mid), 10000);
  assert.strictEqual(delayMs(20, 5000, 300000, mid), 300000);
  for (let a = 0; a < 12; a++) {
    for (const r of [0, 0.25, 0.999]) {
      const d = delayMs(a, 5000, 300000, () => r);
      assert.ok(d >= 1000 && d <= 300000, `attempt ${a} gave ${d}`);
    }
  }
  assert.ok(delayMs(-3, 5000, 300000, mid) >= 1000);
});

// ---- poster -----------------------------------------------------------------------------------
function fakeFetch(plan) {
  const calls = [];
  const fn = async (url, opts) => {
    calls.push({ url, opts, body: JSON.parse(opts.body) });
    const step = plan.length > 1 ? plan.shift() : plan[0];
    if (step === 'throw') throw new Error('ECONNREFUSED');
    return { status: step, arrayBuffer: async () => new ArrayBuffer(0) };
  };
  fn.calls = calls;
  return fn;
}
const logSink = () => { const lines = []; return { lines, info: m => lines.push(m), warn: m => lines.push(m), error: m => lines.push(m) }; };
const msg = (id, extra) => Object.assign({ source: 'c1', platform: 'twitch', id, author: 'A', color: '', parts: [{ t: 'text', v: 'hi ' + id }], badges: [], t: Date.now() }, extra);

test('poster: posts to loopback with the bearer token, removes what was accepted, sends status', async () => {
  const f = fakeFetch([200]);
  const log = logSink();
  const p = new Poster({ port: 4999, token: TOKEN, log, fetchFn: f });
  p.enqueue([msg('1'), msg('2')]);
  p.setStatus({ id: 'c1', platform: 'twitch', state: 'reading', detail: '' });
  await p.flush();
  assert.strictEqual(f.calls.length, 1);
  assert.strictEqual(f.calls[0].url, 'http://127.0.0.1:4999/chat/ingest');
  assert.strictEqual(f.calls[0].opts.headers.Authorization, 'Bearer ' + TOKEN);
  assert.strictEqual(f.calls[0].opts.headers['Content-Type'], 'application/json');
  assert.strictEqual(f.calls[0].body.v, 1);
  assert.strictEqual(f.calls[0].body.messages.length, 2);
  assert.strictEqual(f.calls[0].body.sources[0].state, 'reading');
  assert.ok(!JSON.stringify(f.calls[0].body).includes(TOKEN), 'the token is a header, never part of the body');
  assert.strictEqual(p.queue.length, 0);
  await p.flush(); // nothing to say and status was just sent: no request
  assert.strictEqual(f.calls.length, 1);
});

test('poster: batches of 25, and a failed post keeps the messages and backs off', async () => {
  const f = fakeFetch([503, 200]);
  const p = new Poster({ port: 4999, token: TOKEN, log: logSink(), fetchFn: f });
  p.enqueue(Array.from({ length: 60 }, (_, i) => msg('m' + i)));
  await p.flush();
  assert.strictEqual(f.calls[0].body.messages.length, 25);
  assert.strictEqual(p.queue.length, 60); // 503: nothing removed
  await p.flush();
  assert.strictEqual(f.calls.length, 1, 'inside the backoff window no new request is made');
  p.nextTry = 0;
  await p.flush();
  assert.strictEqual(p.queue.length, 35);
  p.nextTry = 0; await p.flush(); p.nextTry = 0; await p.flush();
  assert.strictEqual(p.queue.length, 0);
});

test('poster: 401 is fatal and logged without the token; 400 drops the batch instead of looping', async () => {
  let fatal = null;
  const log = logSink();
  const p = new Poster({ port: 4999, token: TOKEN, log, fetchFn: fakeFetch([401]), onFatal: (w, s) => { fatal = [w, s]; } });
  p.enqueue([msg('1')]);
  await p.flush();
  assert.deepStrictEqual(fatal, ['refused', 401]);
  assert.ok(log.lines.every(l => !l.includes(TOKEN)));
  const f2 = fakeFetch([400]);
  const p2 = new Poster({ port: 4999, token: TOKEN, log: logSink(), fetchFn: f2 });
  p2.enqueue([msg('1'), msg('2')]);
  await p2.flush();
  assert.strictEqual(p2.queue.length, 0);
  assert.strictEqual(p2.dropped, 2);
  await p2.flush(); // only the pending status may follow; the dropped messages are never resent
  assert.ok(f2.calls.length <= 2 && f2.calls.every((c, i) => i === 0 || c.body.messages.length === 0));
});

test('poster: a server that stays away for too long ends the reader instead of retrying forever', async () => {
  let fatal = null;
  const p = new Poster({ port: 4999, token: TOKEN, log: logSink(), fetchFn: fakeFetch(['throw']), unreachableMs: 50, onFatal: w => { fatal = w; } });
  p.enqueue([msg('1')]);
  await p.flush();
  assert.strictEqual(fatal, null);
  await sleep(80);
  p.nextTry = 0;
  await p.flush();
  assert.strictEqual(fatal, 'unreachable');
});

test('poster: the queue is bounded, oversized messages shrink, lone surrogates become U+FFFD', async () => {
  const f = fakeFetch([200]);
  const p = new Poster({ port: 4999, token: TOKEN, log: logSink(), fetchFn: f, maxQueue: 50 });
  p.enqueue(Array.from({ length: 80 }, (_, i) => msg('m' + i)));
  assert.strictEqual(p.queue.length, 50);
  assert.strictEqual(p.queue[0].id, 'm30'); // oldest dropped first
  assert.strictEqual(p.dropped, 30);
  const big = shrink(msg('b', { parts: [{ t: 'text', v: 'x'.repeat(10000) }, { t: 'text', v: 'never' }], author: 'n'.repeat(900) }));
  assert.ok(JSON.stringify(big).length < 4600);
  assert.strictEqual(big.author.length, 200);
  assert.strictEqual(shrink(msg('c', { parts: Array.from({ length: 200 }, () => ({ t: 'text', v: 'a' })) })).parts.length, 64);
  p.queue.length = 0;
  p.enqueue([msg('s', { parts: [{ t: 'text', v: 'bad \ud800 half' }], author: 'A\udc00' })]);
  await p.flush();
  const sent = f.calls[0].body.messages[0];
  assert.ok(!/[\ud800-\udfff]/.test(sent.parts[0].v) && sent.parts[0].v.includes('�'));
  assert.ok(!/[\ud800-\udfff]/.test(sent.author));
});

// ---- logger, config, argv ---------------------------------------------------------------------
test('log: the token never reaches the file, even inside an error message', () => {
  const f = path.join(os.tmpdir(), 'oms-log-' + process.pid + '.log');
  try {
    const log = makeLogger(f);
    log.addSecret(TOKEN);
    log.error('request failed with Authorization: Bearer ' + TOKEN + ' and again ' + TOKEN);
    log.info('line\r\nbreak');
    const text = fs.readFileSync(f, 'utf8');
    assert.ok(!text.includes(TOKEN));
    assert.ok(text.includes('[token]'));
    assert.strictEqual(text.split('\n').filter(Boolean).length, 2, 'newlines in a message cannot forge log lines');
  } finally { fs.rmSync(f, { force: true }); fs.rmSync(f + '.1', { force: true }); }
});

test('log: the file is capped and rotated', () => {
  const f = path.join(os.tmpdir(), 'oms-log2-' + process.pid + '.log');
  try {
    const log = makeLogger(f);
    for (let i = 0; i < 800; i++) log.info('x'.repeat(400));
    assert.ok(fs.statSync(f).size <= 256 * 1024 + 600);
    assert.ok(fs.existsSync(f + '.1'));
  } finally { fs.rmSync(f, { force: true }); fs.rmSync(f + '.1', { force: true }); }
});

test('reader config: validation of ports, ids, platforms and targets', () => {
  const ok = { v: 1, port: 51734, quiet_minutes: 7, sources: [{ id: 'c1', platform: 'twitch', target: 'shroud' }] };
  const r = parseReaderConfig(JSON.stringify(ok));
  assert.strictEqual(r.port, 51734);
  assert.strictEqual(r.quietMs, 7 * 60000);
  assert.strictEqual(r.sources.length, 1);
  const bad = (mut, re) => { const c = clone(ok); mut(c); assert.throws(() => parseReaderConfig(JSON.stringify(c)), re); };
  bad(c => { c.v = 2; }, /"v"/);
  bad(c => { c.port = 80; }, /port/);
  bad(c => { c.port = 'x'; }, /port/);
  bad(c => { c.quiet_minutes = 0; }, /quiet/);
  bad(c => { c.sources = {}; }, /sources/);
  // the plugin allows 32 chat channels; 11 used to stop the whole reader
  bad(c => { c.sources = Array.from({ length: 33 }, (_, i) => ({ id: 'c' + i, platform: 'twitch', target: 'shroud' })); }, /at most 32/);
  const many = clone(ok);
  many.sources = Array.from({ length: 32 }, (_, i) => ({ id: 'c' + i, platform: 'twitch', target: 'chan' + (100 + i) }));
  assert.strictEqual(parseReaderConfig(JSON.stringify(many)).sources.length, 32);
  // YouTube channel targets pass the reader's own check (the plugin writes them as "@name")
  const yt = clone(ok);
  yt.sources = [{ id: 'c1', platform: 'youtube', target: '@royalascent' }, { id: 'c2', platform: 'youtube', target: 'UCabcdefghijklmnopqrstuv' }];
  assert.strictEqual(parseReaderConfig(JSON.stringify(yt)).sources.length, 2);
  bad(c => { c.sources[0].id = 'bad id'; }, /id/);
  bad(c => { c.sources.push({ id: 'c1', platform: 'kick', target: 'xqc' }); }, /repeated/);
  bad(c => { c.sources[0].platform = 'myspace'; }, /platform/);
  bad(c => { c.sources[0].target = 'https://evil.example/'; }, /target|link|valid/);
  bad(c => { c.sources[0].target = 'Shroud'; }, /normalized/);
  assert.throws(() => parseReaderConfig('{nope'), /JSON/);
  assert.throws(() => parseReaderConfig('null'), /"v"/);
});

test('argv and token: bad usage and a missing token stop the reader before anything starts', async () => {
  assert.throws(() => parseArgs([]), /--config/);
  assert.throws(() => parseArgs(['--config', 'x', '--evil']), /unknown/);
  assert.throws(() => parseArgs(['--config']), /--config/);
  assert.deepStrictEqual(parseArgs(['--config', 'c.json', '--exit-on-stdin-close']), { exitOnStdinClose: true, config: 'c.json' });
  const cfg = path.join(os.tmpdir(), 'oms-rc-' + process.pid + '.json');
  fs.writeFileSync(cfg, JSON.stringify({ v: 1, port: 51734, sources: [] }));
  try {
    await assert.rejects(() => start({ config: cfg }, {}), e => e.exitCode === 2 && /OMSC_CHAT_TOKEN/.test(e.message));
    await assert.rejects(() => start({ config: cfg }, { OMSC_CHAT_TOKEN: 'short' }), e => e.exitCode === 2);
    await assert.rejects(() => start({ config: cfg, testOrigin: 'http://example.com' }, { OMSC_CHAT_TOKEN: TOKEN }), e => e.exitCode === 2 && /test-origin/.test(e.message));
  } finally { fs.rmSync(cfg, { force: true }); }
});

// ---- every platform adapter against its fixture, in headless Chrome ----------------------------
if (!chromeAvailable()) {
  test('adapters in Chrome', t => t.skip('UNVERIFIED: Chrome is not installed'));
} else {
  const { launch } = require('../../reader/lib/chrome');
  let browser, fake;
  test.before(async () => { fake = await startFake(); browser = await launch({}); });
  test.after(async () => { if (browser) await browser.close(); if (fake) fake.close(); });

  const log = logSink();
  async function readFrom(platform, target, variant, feed, expectCount, settle = 400) {
    fake.register(platform, target, variant);
    const src = new Source({ cdp: browser.cdp, pack: packs[platform], src: { id: 'c1', platform, target }, testOrigin: fake.origin, quietMs: 600000, log });
    const got = [];
    src.on('messages', m => got.push(...m));
    await src.start();
    await waitFor(() => src.state === 'reading', 15000);
    for (const html of feed) { fake.append(platform, target, html); await sleep(60); }
    await waitFor(() => got.length >= expectCount, 8000).catch(() => {});
    await sleep(settle); // anything extra (history, duplicates) would show up now
    await src.stop();
    return got;
  }
  const flat = m => m.parts.map(p => (p.t === 'emote' ? `[${p.v}]` : p.v)).join('');

  test('adapter twitch: author, colour, text, emote image and badge are read; history is not', async () => {
    const got = await readFrom('twitch', 'chan_one', 'twitch', [
      BUILD.twitch({ author: 'Alice', color: 'rgb(255, 69, 0)', segments: ['hello ', { emote: 'Kappa', url: EMOTES.TW_EMOTE }, ' world'], badges: ['Moderator'] }),
      BUILD.twitch({ author: 'Mallory', segments: ['<script>alert(1)</script> <img src=x onerror=alert(2)>'] }),
      BUILD.twitch({ author: 'Dup', segments: ['same text'] }), BUILD.twitch({ author: 'Dup', segments: ['same text'] }),
      BUILD.twitch({ author: 'Empty', segments: [''] }),
    ], 3);
    assert.strictEqual(got.length, 3, JSON.stringify(got.map(flat)));
    assert.strictEqual(got[0].author, 'Alice');
    assert.strictEqual(got[0].color, '#ff4500');
    assert.deepStrictEqual(got[0].parts, [{ t: 'text', v: 'hello ' }, { t: 'emote', v: 'Kappa', u: EMOTES.TW_EMOTE }, { t: 'text', v: ' world' }]);
    assert.deepStrictEqual(got[0].badges, ['Moderator']);
    assert.match(got[0].id, /^s[0-9a-f]+-\d+$/); // twitch rows carry no id: synthesised
    assert.strictEqual(got[0].platform, 'twitch');
    assert.strictEqual(got[0].source, 'c1');
    assert.strictEqual(flat(got[1]), '<script>alert(1)</script> <img src=x onerror=alert(2)>', 'hostile markup arrives as literal text, never as elements');
    assert.strictEqual(got.filter(m => m.author === 'Dup').length, 1, 'the same row re-rendered within 1.5 s is reported once');
    assert.ok(!got.some(m => /history/.test(flat(m))), 'rows that were on the page before attaching are history');
    assert.strictEqual(fake.pageLoads('/popout/chan_one/chat'), 1, 'exactly one page load');
  });

  test('adapter youtube: the renderer id is the message id; emoji image and badge type are read', async () => {
    const got = await readFrom('youtube', 'dQw4w9WgXcQ', 'youtube', [
      BUILD.youtube({ id: 'ChwKGkNNLXlfS-1', author: 'Yuki', segments: ['nice ', { emote: '\u{1F642}', url: EMOTES.YT_EMOJI }], badges: ['moderator'] }),
      BUILD.youtube({ id: 'ChwKGkNNLXlfS-2', author: 'Pat', segments: ['plain'] }),
      BUILD.youtube({ id: 'ChwKGkNNLXlfS-3', author: 'Nobody', segments: [''] }),
    ], 2);
    assert.strictEqual(got.length, 2, JSON.stringify(got.map(flat)));
    assert.strictEqual(got[0].id, 'ChwKGkNNLXlfS-1');
    assert.strictEqual(got[0].author, 'Yuki');
    assert.strictEqual(got[0].color, '');
    assert.deepStrictEqual(got[0].parts, [{ t: 'text', v: 'nice ' }, { t: 'emote', v: '\u{1F642}', u: EMOTES.YT_EMOJI }]);
    assert.deepStrictEqual(got[0].badges, ['moderator']);
    assert.ok(!got.some(m => /history/.test(flat(m))));
    assert.strictEqual(fake.pageLoads('/live_chat'), 1);
  });

  test('adapter kick: data-chat-entry is the id; username colour, emote and badge are read', async () => {
    const got = await readFrom('kick', 'kickchan', 'kick', [
      BUILD.kick({ id: 'e-1111', author: 'Kicker', color: 'rgb(0, 255, 163)', segments: ['lol ', { emote: 'KEKW', url: EMOTES.KICK_EMOTE }], badges: ['moderator'] }),
      BUILD.kick({ id: 'e-2222', author: 'Other', segments: ['second'] }),
    ], 2);
    assert.strictEqual(got.length, 2, JSON.stringify(got.map(flat)));
    assert.strictEqual(got[0].id, 'e-1111');
    assert.strictEqual(got[0].author, 'Kicker');
    assert.strictEqual(got[0].color, '#00ffa3');
    assert.deepStrictEqual(got[0].parts.map(p => p.t + ':' + p.v), ['text:lol ', 'emote:KEKW']);
    assert.strictEqual(got[0].parts[1].u, EMOTES.KICK_EMOTE);
    assert.deepStrictEqual(got[0].badges, ['moderator']);
    assert.strictEqual(flat(got[1]), 'second');
    assert.ok(!got.some(m => /history/.test(flat(m))));
  });

  test('adapter tiktok (experimental): author, text and badge from the data-e2e structure', async () => {
    const got = await readFrom('tiktok', 'tiktoker', 'tiktok', [
      BUILD.tiktok({ author: 'viewer1', segments: ['gm chat'], badges: ['mod'] }),
      BUILD.tiktok({ author: 'viewer2', segments: ['hi ', { emote: 'smile', url: EMOTES.TT_EMOTE }] }),
    ], 2);
    assert.strictEqual(got.length, 2, JSON.stringify(got.map(flat)));
    assert.strictEqual(got[0].author, 'viewer1');
    assert.strictEqual(flat(got[0]), 'gm chat');
    assert.deepStrictEqual(got[0].badges, ['Moderator']);
    assert.strictEqual(flat(got[1]), 'hi [smile]');
    assert.ok(!got.some(m => /history/.test(flat(m))));
  });

  test('adapter x (experimental): rows found from the avatar anchor; author and text separated', async () => {
    const got = await readFrom('x', 'xcast12345', 'x', [
      BUILD.x({ author: 'Xavier', segments: ['hello from x'] }),
      BUILD.x({ author: 'Yara', segments: ['second ', { emote: '\u{1F600}', url: EMOTES.X_EMOJI }] }),
    ], 2);
    assert.strictEqual(got.length, 2, JSON.stringify(got.map(flat)));
    assert.strictEqual(got[0].author, 'Xavier');
    assert.strictEqual(flat(got[0]), 'hello from x');
    assert.strictEqual(got[1].author, 'Yara');
    assert.ok(!got.some(m => /history/.test(flat(m))));
  });

  test('adapter health: a redesigned page is reported as layout changed, a login wall as login (never silent)', async () => {
    const fastTwitch = clone(packs.twitch); fastTwitch.containerTimeoutSec = 10;
    fake.register('twitch', 'changed_chan', 'twitch-layout-changed');
    fake.register('x', 'wall1234567', 'x-login');
    const a = new Source({ cdp: browser.cdp, pack: fastTwitch, src: { id: 'c1', platform: 'twitch', target: 'changed_chan' }, testOrigin: fake.origin, quietMs: 600000, log });
    const b = new Source({ cdp: browser.cdp, pack: packs.x, src: { id: 'c2', platform: 'x', target: 'wall1234567' }, testOrigin: fake.origin, quietMs: 600000, log });
    await a.start(); await b.start();
    await waitFor(() => a.state === 'layout', 25000);
    await waitFor(() => b.state === 'login', 25000);
    assert.match(a.detail, /page layout changed or login needed/);
    assert.match(b.detail, /login|human check/);
    await a.stop(); await b.stop();
  });

  test('adapter safety: a page that redirects to a foreign host is left, not read', async () => {
    // a second local origin plays "another site": the host is 127.0.0.1 too, so use the pack's host list instead
    const pack = clone(packs.kick); // real hosts: ['kick.com'], so a local page on 127.0.0.1 is "foreign" when no testOrigin is set
    fake.register('kick', 'foreignchan', 'kick');
    const src = new Source({ cdp: browser.cdp, pack, src: { id: 'c1', platform: 'kick', target: 'foreignchan' }, testOrigin: fake.origin, quietMs: 600000, log });
    src.hosts = ['kick.com']; // as in production: only the platform's own host is acceptable
    await src.start();
    await waitFor(() => src.state === 'error' && /not on the platform allow-list/.test(src.detail), 15000);
    await src.stop();
  });
}

// ---- page states: consent, disabled, replay, offline; consent cookie; diagnose -----------------------
const { classifyNavigation } = require('../../reader/lib/source');
const { diagnose } = require('../../reader/lib/diagnose');

test('states: notes decide consent / disabled / replay / offline, and only under their own conditions', () => {
  const base = { now: 100000, loadedAt: 90000, containerTimeoutMs: 45000, quietMs: 300000, seenTotal: 0, loginRedirect: false };
  const ev = o => evaluateHealth(Object.assign({}, base, o));
  const none = { container: false, login: false, seen: 0, sinceFound: 0 };
  const consent = { state: 'consent', when: 'nochat', detail: 'consent page' };
  assert.strictEqual(ev({ now: 90000 + 2000, tick: none, notes: [consent] }).state, 'starting', 'give the page a moment first');
  assert.strictEqual(ev({ now: 90000 + 5000, tick: none, notes: [consent] }).state, 'consent');
  assert.strictEqual(ev({ now: 90000 + 5000, tick: { container: true, login: false, seen: 0, sinceFound: 1000 }, notes: [consent] }).state, 'reading', 'a nochat note is moot once the chat list exists');
  const replay = { state: 'replay', when: 'always', detail: 'replay' };
  assert.strictEqual(ev({ now: 90000 + 5000, tick: { container: true, login: false, seen: 4, sinceFound: 9000 }, seenTotal: 4, notes: [replay] }).state, 'replay');
  const offline = { state: 'offline', when: 'nomsg', detail: 'offline' };
  const there = s => ({ container: true, login: false, seen: 0, sinceFound: s });
  assert.strictEqual(ev({ tick: there(3000), notes: [offline] }).state, 'reading');
  assert.strictEqual(ev({ tick: there(7000), notes: [offline] }).state, 'offline');
  assert.strictEqual(ev({ tick: there(7000), seenTotal: 2, notes: [offline] }).state, 'reading', 'a channel that has spoken is not offline');
  assert.strictEqual(ev({ tick: there(400000), notes: [] }).state, 'quiet', 'without the offline words it stays the old quiet state');
  assert.strictEqual(ev({ forced: { state: 'consent', detail: 'x' }, tick: there(1000) }).state, 'consent');
  assert.strictEqual(ev({ loadedAt: 0, forced: { state: 'replay', detail: 'x' } }).state, 'replay');
});

test('states: navigation to the consent host is its own kind, an unknown host is still foreign', () => {
  const p = packs.youtube;
  assert.strictEqual(classifyNavigation(p, 'www.youtube.com', ['www.youtube.com']), 'ok');
  assert.strictEqual(classifyNavigation(p, 'consent.youtube.com', ['www.youtube.com']), 'consent');
  assert.strictEqual(classifyNavigation(p, 'consent.youtube.com.evil.example', ['www.youtube.com']), 'foreign');
  assert.strictEqual(classifyNavigation(p, 'evil.example', ['www.youtube.com']), 'foreign');
  assert.strictEqual(classifyNavigation(packs.kick, 'consent.youtube.com', ['kick.com']), 'foreign');
});

test('states: cookies, consent hosts and notes in a pack are validated tightly', () => {
  const good = clone(packs.youtube);
  assert.doesNotThrow(() => validatePack(clone(good), 'youtube'));
  const bad = (mut, re) => { const p = clone(good); mut(p); assert.throws(() => validatePack(p, 'youtube'), re); };
  bad(p => { p.cookies = [{ name: 'LOGIN_INFO', value: 'abc', domain: '.youtube.com' }]; }, /consent preference cookies/);
  bad(p => { p.cookies = [{ name: 'SOCS', value: 'a;b', domain: '.youtube.com' }]; }, /odd characters/);
  bad(p => { p.cookies = [{ name: 'SOCS', value: 'CAI', domain: '.evil.example' }]; }, /parent/);
  bad(p => { p.cookies = [{ name: 'SOCS', value: 'CAI', domain: '.com' }]; }, /parent|domain/);
  bad(p => { p.consentHosts = ['consent.evil.example']; }, /own domain/);
  bad(p => { p.noteStates.consent.state = 'hacked'; }, /noteStates/);
  bad(p => { p.noteStates.consent.when = 'sometimes'; }, /noteStates/);
  bad(p => { delete p.noteStates.disabled; }, /no entry in "noteStates"/);
  bad(p => { p.replayPaths = ['live_chat_replay']; }, /replayPaths/);
  assert.deepStrictEqual(packs.youtube.cookies, [{ name: 'SOCS', value: 'CAI', domain: '.youtube.com' }]);
  assert.ok(packs.youtube.url.includes('live_chat?is_popout=1&v='), 'the popout form');
  assert.ok(!packs.youtube.url.includes('embed_domain'), 'embed_domain belongs to the embedded iframe, not the popout');
});

if (chromeAvailable()) {
  const { launch: launch2 } = require('../../reader/lib/chrome');
  let b2, f2;
  test.before(async () => { f2 = await startFake(); b2 = await launch2({}); });
  test.after(async () => { if (b2) await b2.close(); if (f2) f2.close(); });
  const log2 = { info() {}, warn() {}, error() {} };
  async function stateOf(platform, target, variant, want, ms = 20000, feed = []) {
    f2.register(platform, target, variant);
    const src = new Source({ cdp: b2.cdp, pack: packs[platform], src: { id: 'c1', platform, target }, testOrigin: f2.origin, quietMs: 600000, log: log2 });
    const got = [];
    src.on('messages', m => got.push(...m));
    await src.start();
    try {
      await waitFor(() => src.state === want, ms);
      for (const h of feed) { f2.append(platform, target, h); await sleep(80); }
      await sleep(800);
      return { src, got, detail: src.detail, state: src.state };
    } finally { await src.stop(); }
  }

  test('states in Chrome: YouTube consent page, disabled chat and replay each get their own state', async () => {
    assert.match((await stateOf('youtube', 'consent1111', 'youtube-consent', 'consent')).detail, /cookie-consent page/);
    assert.match((await stateOf('youtube', 'disabled111', 'youtube-disabled', 'disabled')).detail, /disabled or unavailable/);
    const r = await stateOf('youtube', 'replay11111', 'youtube-replay', 'replay', 20000, [BUILD.youtube({ id: 'rp-1', author: 'Replayer', segments: ['old line'] })]);
    assert.strictEqual(r.got.length, 0, 'replay lines are never forwarded as live chat');
  });

  test('states in Chrome: Kick chat list present but channel offline -> offline; an empty list without those words stays reading', async () => {
    const r = await stateOf('kick', 'offlinechan', 'kick-offline', 'offline');
    assert.match(r.detail, /channel is offline/);
    const q = await stateOf('kick', 'emptychan', 'kick-empty', 'reading');
    assert.strictEqual(q.state === 'offline', false);
  });

  test('states in Chrome: Kick rows that only carry data-index are read, with synthesised ids', async () => {
    const row = BUILD.kick({ id: 'x', author: 'IdxUser', segments: ['via data-index'] }).replace(/data-chat-entry="[^"]*"/, '');
    const r = await stateOf('kick', 'indexchan', 'kick-index', 'reading', 20000, [row]);
    assert.strictEqual(r.got.length, 1);
    assert.strictEqual(r.got[0].author, 'IdxUser');
    assert.match(r.got[0].id, /^s[0-9a-f]+-\d+$/);
  });

  test('consent cookie: SOCS is set through CDP before the page loads, secure, scoped to .youtube.com, only in that tab context', async () => {
    f2.register('youtube', 'cookie11111', 'youtube');
    const src = new Source({ cdp: b2.cdp, pack: packs.youtube, src: { id: 'c1', platform: 'youtube', target: 'cookie11111' }, testOrigin: f2.origin, quietMs: 600000, log: log2 });
    await src.start();
    try {
      await waitFor(() => src.sessionId, 5000);
      const r = await b2.cdp.send('Network.getCookies', { urls: ['https://www.youtube.com/'] }, src.sessionId);
      const c = r.cookies.find(x => x.name === 'SOCS');
      assert.ok(c, JSON.stringify(r.cookies.map(x => x.name)));
      assert.strictEqual(c.value, 'CAI');
      assert.strictEqual(c.domain, '.youtube.com');
      assert.strictEqual(c.secure, true);
      assert.deepStrictEqual(r.cookies.map(x => x.name), ['SOCS'], 'nothing else is set');
      f2.register('kick', 'cookie22222', 'kick');
      const k = new Source({ cdp: b2.cdp, pack: packs.kick, src: { id: 'c2', platform: 'kick', target: 'cookie22222' }, testOrigin: f2.origin, quietMs: 600000, log: log2 });
      await k.start();
      try {
        await waitFor(() => k.sessionId, 5000);
        const kc = await b2.cdp.send('Network.getCookies', { urls: ['https://www.youtube.com/'] }, k.sessionId);
        assert.strictEqual(kc.cookies.length, 0);
      } finally { await k.stop(); }
    } finally { await src.stop(); }
  });

  test('diagnose prints structure only: counts, tags, class names, attribute NAMES; never text, names or values', async () => {
    f2.register('kick', 'diagchan', 'kick');
    f2.append('kick', 'diagchan', BUILD.kick({ id: 'secret-id-9f8e7d', author: 'SecretAuthorXYZ', color: 'rgb(1, 2, 3)', segments: ['SecretTextQWE ', { emote: 'SecretEmote', url: EMOTES.KICK_EMOTE }] }));
    f2.register('youtube', 'diagyt11111', 'youtube-consent');
    const lines = [];
    await diagnose({ sources: [{ id: 'c1', platform: 'kick', target: 'diagchan' }, { id: 'c2', platform: 'youtube', target: 'diagyt11111' }] }, packs, {},
      { testOrigin: f2.origin, waitSec: 5, env: Object.assign({}, process.env) }, o => lines.push(o));
    const text = JSON.stringify(lines);
    for (const secret of ['SecretAuthorXYZ', 'SecretTextQWE', 'SecretEmote', 'secret-id-9f8e7d', 'rgb(1, 2, 3)', 'history one', 'OldTimer', 'hist-1']) assert.ok(!text.includes(secret), 'leaked: ' + secret);
    const kick = lines.find(l => l.platform === 'kick');
    const snap = kick.snapshots[kick.snapshots.length - 1].facts;
    assert.strictEqual(snap.container.found, true);
    assert.ok(snap.container.path[0].startsWith('div#chatroom-messages'));
    assert.ok(snap.counts.item.find(c => c.sel === '[data-chat-entry]').n >= 3, JSON.stringify(snap.counts.item));
    assert.ok(snap.counts.author.some(c => c.n >= 1));
    assert.ok(snap.first_item.attribute_names.includes('data-chat-entry'));
    assert.ok(snap.first_item.shape.kids.length >= 1);
    assert.strictEqual(snap.notes[0].id, 'offline');
    const yt = lines.find(l => l.platform === 'youtube');
    const ysnap = yt.snapshots[yt.snapshots.length - 1].facts;
    assert.strictEqual(ysnap.container.found, false);
    assert.strictEqual(ysnap.notes.find(n => n.id === 'consent').words_hit[0], true, 'which state words appear is reported as yes/no');
    assert.deepStrictEqual(yt.cookies_set, [{ name: 'SOCS', accepted: true }]);
  });
}

// ---- user agent compatibility setting and the stub page state -----------------------------------------
const { computeUserAgent } = require('../../reader/lib/useragent');

test('user agent: version comes from the browser, only the Headless token goes, mode headless changes nothing', () => {
  const win = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) HeadlessChrome/126.0.6478.127 Safari/537.36';
  const r = computeUserAgent({ product: 'HeadlessChrome/126.0.6478.127', userAgent: win }, 'chrome');
  assert.strictEqual(r.userAgent, win.replace('HeadlessChrome/', 'Chrome/'));
  assert.strictEqual(r.userAgent.replace('Chrome/', 'HeadlessChrome/'), win, 'nothing but the token differs');
  assert.ok(!/headless/i.test(r.userAgent));
  assert.strictEqual(r.version, '126.0.6478.127');
  assert.strictEqual(r.major, '126');
  const md = r.userAgentMetadata;
  assert.ok(md.brands.some(b => b.brand === 'Google Chrome' && b.version === '126'));
  assert.ok(md.brands.some(b => b.brand === 'Chromium' && b.version === '126'));
  assert.ok(!JSON.stringify(md).toLowerCase().includes('headless'));
  assert.strictEqual(md.fullVersionList.find(b => b.brand === 'Google Chrome').version, '126.0.6478.127');
  assert.strictEqual(md.platform, 'Windows');
  assert.strictEqual(md.platformVersion, '10.0.0');
  assert.strictEqual(md.mobile, false);
  // another version number is followed, not remembered
  const other = computeUserAgent({ userAgent: win.replace(/126\.0\.6478\.127/, '141.0.7390.55') }, 'chrome');
  assert.strictEqual(other.version, '141.0.7390.55');
  assert.ok(other.userAgent.includes('Chrome/141.0.7390.55 '));
  // an agent that is already a normal Chrome is left byte for byte as it is
  const normal = win.replace('HeadlessChrome/', 'Chrome/');
  assert.strictEqual(computeUserAgent({ userAgent: normal }, 'chrome').userAgent, normal);
  assert.strictEqual(computeUserAgent({ userAgent: win }, 'headless'), null);
  for (const junk of [null, undefined, {}, { userAgent: '' }, { userAgent: 'curl/8.0' }, { userAgent: 42 }]) assert.strictEqual(computeUserAgent(junk, 'chrome'), null, JSON.stringify(junk));
  assert.strictEqual(computeUserAgent({ userAgent: 'Mozilla/5.0 (X11; Linux x86_64) HeadlessChrome/120.0.1.2 Safari/537.36' }, 'chrome').userAgentMetadata.platform, 'Linux');
});

test('user agent: the reader config accepts only chrome or headless, default chrome', () => {
  const base = { v: 1, port: 51734, sources: [] };
  assert.strictEqual(parseReaderConfig(JSON.stringify(base)).userAgent, 'chrome');
  assert.strictEqual(parseReaderConfig(JSON.stringify({ ...base, user_agent: 'headless' })).userAgent, 'headless');
  for (const bad of ['firefox', '', 'Mozilla/5.0 custom', 1, null]) assert.throws(() => parseReaderConfig(JSON.stringify({ ...base, user_agent: bad })), /user_agent/);
});

test('states: an almost empty page with no chat list is "stub", but a login wall or a redesigned page is not', () => {
  const base = { now: 100000, loadedAt: 90000, containerTimeoutMs: 45000, quietMs: 300000, seenTotal: 0, loginRedirect: false };
  const ev = o => evaluateHealth(Object.assign({}, base, o));
  const t = (elements, login) => ({ container: false, login: !!login, elements, seen: 0, sinceFound: 0 });
  assert.strictEqual(ev({ now: 90000 + 3000, tick: t(11) }).state, 'starting', 'give the page time first');
  const s = ev({ now: 90000 + 9000, tick: t(11) });
  assert.strictEqual(s.state, 'stub');
  assert.match(s.detail, /platform served an empty page: likely automation detection or an unavailable stream/);
  assert.strictEqual(ev({ now: 90000 + 9000, tick: t(19) }).state, 'stub');
  assert.strictEqual(ev({ now: 90000 + 9000, tick: t(20) }).state, 'starting', 'a real page that is simply still loading');
  assert.strictEqual(ev({ now: 90000 + 9000, tick: t(11, true) }).state, 'login');
  assert.strictEqual(ev({ now: 90000 + 46000, tick: t(300) }).state, 'layout');
  assert.strictEqual(ev({ now: 90000 + 9000, tick: { container: true, login: false, elements: 11, seen: 0, sinceFound: 100 } }).state, 'reading', 'a chat list wins');
});

if (chromeAvailable()) {
  const { launch: launch3 } = require('../../reader/lib/chrome');
  let b3, f3;
  test.before(async () => { f3 = await startFake(); b3 = await launch3({}); });
  test.after(async () => { if (b3) await b3.close(); if (f3) f3.close(); });
  const log3 = { info() {}, warn() {}, error() {} };

  test('user agent in Chrome: the request carries the installed Chrome\'s own agent minus Headless; headless mode leaves it alone', async () => {
    const ver = await b3.cdp.send('Browser.getVersion');
    const ua = computeUserAgent(ver, 'chrome');
    assert.ok(ver.userAgent.includes('HeadlessChrome/'), 'this Chrome really advertises Headless by default');
    f3.register('youtube', 'uatest11111', 'youtube');
    const a = new Source({ cdp: b3.cdp, pack: packs.youtube, src: { id: 'c1', platform: 'youtube', target: 'uatest11111' }, testOrigin: f3.origin, quietMs: 600000, log: log3, userAgent: ua });
    await a.start();
    await waitFor(() => a.state === 'reading', 20000);
    await a.stop();
    const seen = f3.uas.filter(u => u.path === '/live_chat').pop();
    assert.strictEqual(seen.ua, ver.userAgent.replace('HeadlessChrome/', 'Chrome/'));
    assert.ok(seen.ua.includes('Chrome/' + ver.product.split('/')[1].split('.')[0] + '.'), 'major version taken from the browser');
    assert.strictEqual(ua.version, ver.product.split('/')[1], 'full version for the client hints comes from the browser');
    assert.ok(!/headless/i.test(seen.ua + seen.chua), 'no Headless token in the agent or the client hints: ' + seen.chua);
    assert.ok(/Google Chrome/.test(seen.chua) || seen.chua === '', 'brands follow the installed Chrome: ' + seen.chua);
    f3.register('youtube', 'uatest22222', 'youtube');
    const b = new Source({ cdp: b3.cdp, pack: packs.youtube, src: { id: 'c2', platform: 'youtube', target: 'uatest22222' }, testOrigin: f3.origin, quietMs: 600000, log: log3, userAgent: computeUserAgent(ver, 'headless') });
    await b.start();
    await waitFor(() => b.state === 'reading', 20000);
    await b.stop();
    assert.ok(f3.uas.filter(u => u.path === '/live_chat').pop().ua.includes('HeadlessChrome/'), 'headless mode: Chrome is not touched');
  });

  test('YouTube adapter: with the Chrome agent and a chat list present the state is reading, and lines flow', async () => {
    const ver = await b3.cdp.send('Browser.getVersion');
    f3.register('youtube', 'yt-read1111', 'youtube');
    const src = new Source({ cdp: b3.cdp, pack: packs.youtube, src: { id: 'c1', platform: 'youtube', target: 'yt-read1111' }, testOrigin: f3.origin, quietMs: 600000, log: log3, userAgent: computeUserAgent(ver, 'chrome') });
    const got = [];
    src.on('messages', m => got.push(...m));
    await src.start();
    try {
      await waitFor(() => src.state === 'reading', 20000);
      assert.strictEqual(src.state, 'reading');
      f3.append('youtube', 'yt-read1111', BUILD.youtube({ id: 'ua-1', author: 'Yui', segments: ['hello'] }));
      await waitFor(() => got.length === 1, 8000);
      assert.strictEqual(got[0].id, 'ua-1');
    } finally { await src.stop(); }
  });

  test('stub page in Chrome: an empty document is reported as stub, not as layout changed', async () => {
    f3.register('youtube', 'stubpage111', 'youtube-stub');
    const src = new Source({ cdp: b3.cdp, pack: packs.youtube, src: { id: 'c1', platform: 'youtube', target: 'stubpage111' }, testOrigin: f3.origin, quietMs: 600000, log: log3 });
    await src.start();
    try {
      await waitFor(() => src.state === 'stub', 25000);
      assert.match(src.detail, /platform served an empty page/);
    } finally { await src.stop(); }
  });

  test('diagnose reports the user agent mode and flags a stub page', async () => {
    f3.register('youtube', 'diagstub111', 'youtube-stub');
    f3.register('youtube', 'diagfull111', 'youtube');
    const lines = [];
    await diagnose({ sources: [{ id: 'c1', platform: 'youtube', target: 'diagstub111' }, { id: 'c2', platform: 'youtube', target: 'diagfull111' }] }, packs, {},
      { testOrigin: f3.origin, waitSec: 5, env: Object.assign({}, process.env), userAgentMode: 'chrome' }, o => lines.push(o));
    const head = lines.find(l => l.user_agent_mode);
    assert.strictEqual(head.user_agent_mode, 'chrome');
    assert.strictEqual(head.user_agent_applied, true);
    assert.match(head.chrome_version, /^\d+\.\d+\.\d+\.\d+$/);
    const last = l => l.snapshots[l.snapshots.length - 1].facts;
    const stub = last(lines.find(l => l.source === 'c1'));
    assert.strictEqual(stub.stub, true);
    assert.match(stub.state_guess, /^stub: platform served an empty page/);
    const full = last(lines.find(l => l.source === 'c2'));
    assert.strictEqual(full.stub, false);
    assert.strictEqual(full.container.found, true);
    const off = [];
    await diagnose({ sources: [{ id: 'c1', platform: 'youtube', target: 'diagfull111' }] }, packs, {}, { testOrigin: f3.origin, waitSec: 5, env: Object.assign({}, process.env), userAgentMode: 'headless' }, o => off.push(o));
    assert.strictEqual(off[0].user_agent_applied, false);
  });
}

// ---- the C++ units: sanitiser, targets, hub, config, routes ------------------------------------
const CPP_L2 = 'chat_|emote_|hub_|ingest_|reader_config|config_v2_migrates|config_chat|config_node';
const cpp = runCppSafe('L2', CPP_L2);
function runCppSafe(layer, filter) {
  try { return runCpp(layer, filter); } catch (e) { return { error: e }; }
}
if (!cpp.cases) {
  test('C++ chat units', t => t.skip('UNVERIFIED: C++ tests not built (' + (cpp.error && cpp.error.message) + ')'));
} else {
  test('C++ chat L2: enough cases ran', () => assert.ok(cpp.cases.length >= 28, `only ${cpp.cases.length}\n${cpp.out}`));
  for (const c of cpp.cases) test('C++ ' + c.name, () => assert.ok(c.ok, c.detail));
}

test('browser search: Chrome first, then Chromium (per user, then system), Edge last; OMSC_CHROME wins', () => {
  const { candidates } = require('../../reader/lib/chrome');
  const local = String.raw`C:\Users\u\AppData\Local`, own = String.raw`D:\x\chrome.exe`;
  const list = candidates({ LOCALAPPDATA: local, OMSC_CHROME: own });
  assert.strictEqual(list[0], own);
  const idx = s => list.findIndex(p => p.includes(s));
  assert.ok(idx('Google') > 0 && idx('Google') < idx('Chromium'), list.join('\n'));
  assert.ok(list[idx('Chromium')].startsWith(local), 'per-user Chromium before the system one');
  assert.ok(list.includes(String.raw`C:\Program Files\Chromium\Application\chrome.exe`));
  assert.ok(list[list.length - 1].endsWith('msedge.exe'));
  assert.ok(!candidates({}).some(p => p.includes('undefined')), 'no LOCALAPPDATA: no broken entries');
});
