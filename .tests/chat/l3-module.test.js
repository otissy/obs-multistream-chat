'use strict';
// L3: the reader as a module (config -> Chrome -> sources -> poster) against a LOCAL fake platform
// and a stub merge server; plus the C++ server tests over real sockets.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { TOKEN, chromeAvailable, startFake, startStubMerge, runCpp, waitFor, sleep } = require('./helpers');
const { start } = require('../../reader/index');
const { PREFIX } = require('../../reader/lib/chrome');
const { BUILD, EMOTES } = require('./fixtures/dom');

function profileDirs() { return fs.readdirSync(os.tmpdir()).filter(n => n.startsWith(PREFIX)); }
function writeCfg(port, sources) {
  const f = path.join(os.tmpdir(), 'oms-l3-' + process.pid + '-' + Math.random().toString(16).slice(2) + '.json');
  fs.writeFileSync(f, JSON.stringify({ v: 1, port, quiet_minutes: 5, sources }));
  return f;
}

if (!chromeAvailable()) {
  test('reader module (needs Chrome)', t => t.skip('UNVERIFIED: Chrome is not installed'));
} else {
  test('reader: two sources, one browser, messages arrive with token, status and in order; profile is deleted', async () => {
    const fake = await startFake();
    const stub = await startStubMerge();
    const before = profileDirs().length;
    fake.register('twitch', 'chan_a', 'twitch');
    fake.register('youtube', 'dQw4w9WgXcQ', 'youtube');
    const cfg = writeCfg(stub.port, [{ id: 'c1', platform: 'twitch', target: 'chan_a' }, { id: 'c2', platform: 'youtube', target: 'dQw4w9WgXcQ' }]);
    const env = Object.assign({}, process.env, { OMSC_CHAT_TOKEN: TOKEN });
    const ctl = await start({ config: cfg, testOrigin: fake.origin }, env, { noStagger: true });
    try {
      await waitFor(() => ctl.sources.length === 2 && ctl.sources.every(s => s.state === 'reading'), 20000);
      for (let i = 1; i <= 3; i++) {
        fake.append('twitch', 'chan_a', BUILD.twitch({ author: 'TwA', segments: ['tw ' + i] }));
        await sleep(150);
        fake.append('youtube', 'dQw4w9WgXcQ', BUILD.youtube({ id: 'yt-' + i, author: 'YtA', segments: ['yt ' + i, { emote: 'x', url: EMOTES.YT_EMOJI }] }));
        await sleep(150);
      }
      await waitFor(() => stub.messages().length >= 6, 10000);
      const msgs = stub.messages();
      assert.strictEqual(msgs.length, 6);
      // two independent pages: order inside each platform is exact, across platforms the merge server orders by observation time
      assert.deepStrictEqual(msgs.filter(m => m.platform === 'twitch').map(m => m.parts[0].v), ['tw 1', 'tw 2', 'tw 3']);
      assert.deepStrictEqual(msgs.filter(m => m.platform === 'youtube').map(m => m.parts[0].v), ['yt 1', 'yt 2', 'yt 3']);
      assert.ok(msgs.every(m => typeof m.t === 'number' && m.t > 1e12), 'every message carries when the reader saw it');
      assert.deepStrictEqual(msgs.filter(m => m.platform === 'youtube').map(m => m.id), ['yt-1', 'yt-2', 'yt-3']);
      assert.strictEqual(stub.state.unauthorized, 0);
      assert.strictEqual(stub.status('c1').state, 'reading');
      assert.strictEqual(stub.status('c2').platform, 'youtube');
      assert.ok(stub.batches.every(b => b.v === 1 && typeof b.sent === 'number'));
      assert.ok(!JSON.stringify(stub.batches).includes(TOKEN));
      assert.strictEqual(fake.pageLoads('/popout/chan_a/chat'), 1);
      assert.strictEqual(fake.pageLoads('/live_chat'), 1);
    } finally {
      await ctl.shutdown(0);
      assert.strictEqual(await ctl.done, 0);
      fake.close(); stub.close(); fs.rmSync(cfg, { force: true });
    }
    assert.strictEqual(stub.status('c1').state, 'stopped', 'the final status tells the plugin the reader stopped');
    await waitFor(() => profileDirs().length <= before, 10000);
  });

  test('reader: a wrong token ends the reader with code 3 and nothing is retried', async () => {
    const fake = await startFake();
    const stub = await startStubMerge({ token: 'a-completely-different-token-value' });
    fake.register('kick', 'kickchan', 'kick');
    const cfg = writeCfg(stub.port, [{ id: 'c1', platform: 'kick', target: 'kickchan' }]);
    const ctl = await start({ config: cfg, testOrigin: fake.origin }, Object.assign({}, process.env, { OMSC_CHAT_TOKEN: TOKEN }), { noStagger: true });
    try {
      const code = await Promise.race([ctl.done, sleep(20000).then(() => 'timeout')]);
      assert.strictEqual(code, 3);
      assert.ok(stub.state.unauthorized >= 1 && stub.state.unauthorized <= 2, 'stopped after the refusal, no hammering: ' + stub.state.unauthorized);
    } finally { await ctl.shutdown(3); fake.close(); stub.close(); fs.rmSync(cfg, { force: true }); }
  });

  test('reader: while the server fails nothing is lost, and delivery resumes in order', async () => {
    const fake = await startFake();
    const stub = await startStubMerge();
    fake.register('kick', 'kickchan2', 'kick');
    const cfg = writeCfg(stub.port, [{ id: 'c1', platform: 'kick', target: 'kickchan2' }]);
    const ctl = await start({ config: cfg, testOrigin: fake.origin }, Object.assign({}, process.env, { OMSC_CHAT_TOKEN: TOKEN }), { noStagger: true });
    try {
      await waitFor(() => ctl.sources[0] && ctl.sources[0].state === 'reading', 20000);
      stub.state.mode = 'fail';
      for (let i = 1; i <= 4; i++) { fake.append('kick', 'kickchan2', BUILD.kick({ id: 'k-' + i, author: 'K', segments: ['m' + i] })); await sleep(100); }
      await sleep(1500);
      assert.strictEqual(stub.messages().length, 0);
      stub.state.mode = 'ok';
      await waitFor(() => stub.messages().length >= 4, 15000);
      assert.deepStrictEqual(stub.messages().map(m => m.id), ['k-1', 'k-2', 'k-3', 'k-4']);
    } finally { await ctl.shutdown(0); fake.close(); stub.close(); fs.rmSync(cfg, { force: true }); }
  });

  test('reader: an adapter file that is broken shows as an error status for that platform only', async () => {
    const fake = await startFake();
    const stub = await startStubMerge();
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-adapt3-'));
    for (const f of fs.readdirSync(path.join(__dirname, '..', '..', 'reader', 'adapters'))) fs.copyFileSync(path.join(__dirname, '..', '..', 'reader', 'adapters', f), path.join(dir, f));
    fs.writeFileSync(path.join(dir, 'tiktok.json'), '{"adapter":1}');
    fake.register('kick', 'kickchan3', 'kick');
    const cfg = writeCfg(stub.port, [{ id: 'c1', platform: 'kick', target: 'kickchan3' }, { id: 'c2', platform: 'tiktok', target: 'someone' }]);
    const ctl = await start({ config: cfg, adapters: dir, testOrigin: fake.origin }, Object.assign({}, process.env, { OMSC_CHAT_TOKEN: TOKEN }), { noStagger: true });
    try {
      await waitFor(() => stub.status('c2') && stub.status('c1'), 15000);
      assert.strictEqual(stub.status('c2').state, 'error');
      assert.match(stub.status('c2').detail, /tiktok\.json/);
      await waitFor(() => stub.status('c1').state === 'reading', 15000);
    } finally { await ctl.shutdown(0); fake.close(); stub.close(); fs.rmSync(cfg, { force: true }); fs.rmSync(dir, { recursive: true, force: true }); }
  });
}

const cpp = (() => { try { return runCpp('L3', 'chat_ingest|unauthenticated_post|slow_or_short|server_with_chat'); } catch (e) { return { error: e }; } })();
if (!cpp.cases) {
  test('C++ chat server over sockets', t => t.skip('UNVERIFIED: C++ tests not built'));
} else {
  test('C++ chat L3: all four socket tests ran', () => assert.strictEqual(cpp.cases.length, 4, cpp.out));
  for (const c of cpp.cases) test('C++ ' + c.name, () => assert.ok(c.ok, c.detail));
}
