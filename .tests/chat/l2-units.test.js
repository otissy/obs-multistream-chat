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
  bad(c => { c.sources = Array.from({ length: 11 }, (_, i) => ({ id: 'c' + i, platform: 'twitch', target: 'shroud' })); }, /at most 10/);
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
