'use strict';
// L2: each piece alone. C++: VoiceHub, WAV framing, routing, the server's voice routes (oms_tests L2 voice_*).
// Page: how the overlay reads its settings from the URL and from storage.
const test = require('node:test');
const assert = require('node:assert');
const path = require('path');
const { spawnSync } = require('child_process');
const h = require('./helpers');

function oms() {
  const fs = require('fs');
  const root = process.env.OMSC_BUILD_ROOT || 'D:/builds/obs-multistream-chat';
  return [path.join(root, 'windows-x64', 'Release', 'oms_tests.exe'), path.join(root, 'tests-vs', 'Release', 'oms_tests.exe')].find(p => fs.existsSync(p));
}

test('C++ L2 voice cases all pass', { skip: !oms() && 'oms_tests not built' }, () => {
  const r = spawnSync(oms(), ['L2', 'voice_'], { encoding: 'utf8', timeout: 120000 });
  const total = /# total (\d+) pass (\d+) fail (\d+)/.exec(r.stdout);
  assert.ok(total, r.stdout + r.stderr);
  assert.strictEqual(total[3], '0', r.stdout);
  assert.ok(Number(total[2]) >= 17, 'expected at least 17 voice cases, got ' + total[2]);
});

test('page settings: URL defaults, bounds, names, and storage overrides', { skip: h.skipReason }, async () => {
  const srv = await h.startServer();
  const browser = await h.launch('obs');
  try {
    const q = async (query, storage) => {
      const p = await h.openPage(browser, srv.base + 'chat.html?' + query, { mode: 'obs' });
      if (storage !== undefined) {
        await p.page.evaluate(s => { if (s === null) localStorage.removeItem('omsc.voice.v1'); else localStorage.setItem('omsc.voice.v1', s); }, storage);
        await p.page.reload();
      }
      const s = await h.waitFor(() => h.vstate(p.page));
      await p.context.close();
      return s.settings;
    };
    let s = await q('voice=1');
    assert.deepStrictEqual(s, { speak: true, vol: 100, queue: 5, drop: 'oldest', local: false, skip: '' });
    s = await q('voice=1&vol=40&queue=12&drop=newest&local=1&skip=%20Me%20,NightBot,,' + 'x'.repeat(65) + '&speak=0');
    assert.deepStrictEqual(s, { speak: false, vol: 40, queue: 12, drop: 'newest', local: true, skip: 'me, nightbot' });
    s = await q('voice=1&vol=150&queue=0&drop=sideways&local=yes');
    assert.deepStrictEqual(s, { speak: true, vol: 100, queue: 5, drop: 'oldest', local: false, skip: '' });
    s = await q('voice=1&vol=40', JSON.stringify({ vol: 70, queue: 3, drop: 'newest', local: true, skip: 'A,b', speak: false }));
    assert.deepStrictEqual(s, { speak: false, vol: 70, queue: 3, drop: 'newest', local: true, skip: 'a, b' });
    s = await q('voice=1&vol=40', '{not json');
    assert.strictEqual(s.vol, 40, 'broken storage falls back to the URL');
    s = await q('voice=1&vol=40', JSON.stringify({ vol: 900, queue: 'x', drop: 1 }));
    assert.strictEqual(s.vol, 40);
    assert.strictEqual(s.queue, 5);
    assert.strictEqual(s.drop, 'oldest');
  } finally {
    await browser.close();
    await srv.stop();
  }
});
