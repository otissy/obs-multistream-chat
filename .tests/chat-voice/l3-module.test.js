'use strict';
// L3: the pieces together. C++ socket tests (oms_tests L3 voice_*), then the real server + real page:
// one leader among several overlay pages, takeover, reload, flush, "also play locally" reaching the plugin.
const test = require('node:test');
const assert = require('node:assert');
const path = require('path');
const fs = require('fs');
const { spawnSync } = require('child_process');
const h = require('./helpers');

function oms() {
  const root = process.env.OMSC_BUILD_ROOT || 'D:/builds/obs-multistream-chat';
  return [path.join(root, 'windows-x64', 'Release', 'oms_tests.exe'), path.join(root, 'tests-vs', 'Release', 'oms_tests.exe')].find(p => fs.existsSync(p));
}

test('C++ L3 voice cases all pass', { skip: !oms() && 'oms_tests not built' }, () => {
  const r = spawnSync(oms(), ['L3', 'voice_'], { encoding: 'utf8', timeout: 120000 });
  const total = /# total (\d+) pass (\d+) fail (\d+)/.exec(r.stdout);
  assert.ok(total, r.stdout + r.stderr);
  assert.strictEqual(total[3], '0', r.stdout);
  assert.ok(Number(total[2]) >= 2);
});

test('several overlay pages: exactly one speaks, takeover on close, reload and program change', { skip: h.skipReason, timeout: 120000 }, async () => {
  const srv = await h.startServer();
  const browser = await h.launch('obs');
  const pages = [];
  try {
    const url = srv.base + 'chat.html?voice=1';
    const a = await h.openPage(browser, url, { mode: 'obs' }); pages.push(a);
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).role === 'leader'), 'first page leads');
    const b = await h.openPage(browser, url, { mode: 'obs' }); pages.push(b);
    assert.ok(await h.waitFor(async () => (await h.vstate(b.page)).role === 'leader'), 'newest OBS page leads (a reloaded source takes over)');
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).role === 'standby'));
    const c = await h.openPage(browser, url, { mode: 'browser' }); pages.push(c); // a preview in a normal browser
    assert.ok(await h.waitFor(async () => (await h.vstate(c.page)).role === 'standby'), 'a normal browser never steals from OBS');
    assert.deepStrictEqual((await srv.status()).listeners, 3);

    const id = await srv.tone('viewer', 500, 440);
    assert.ok(await h.waitFor(async () => (await h.vstate(b.page)).played === 1), 'leader played it');
    await h.sleep(300);
    assert.strictEqual((await h.vstate(a.page)).played, 0, 'standby stayed quiet');
    assert.strictEqual((await h.vstate(c.page)).played, 0);
    assert.ok(id > 0);

    // the speaking source leaves program: the one on program takes over
    await b.page.evaluate(() => window.dispatchEvent(new CustomEvent('obsSourceActiveChanged', { detail: { active: false } })));
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).role === 'leader'), 'shown-on-program page takes over');
    assert.strictEqual((await h.vstate(b.page)).role, 'standby');
    await b.page.evaluate(() => window.dispatchEvent(new CustomEvent('obsSourceActiveChanged', { detail: { active: true } })));
    assert.ok(await h.waitFor(async () => (await h.vstate(b.page)).role === 'leader'));

    // the leader closes: the next one takes over by itself
    await b.context.close(); pages.splice(pages.indexOf(b), 1);
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).role === 'leader', 4000), 'takeover after close');
    await srv.tone('viewer', 400, 440);
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).played === 1));

    // reload mid-line: the reloaded page leads again, does not replay the old line, speaks the next one
    const long = await srv.tone('viewer', 3000, 330);
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).playing === long));
    await a.page.reload();
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).role === 'leader'));
    await h.sleep(500);
    let s = await h.vstate(a.page);
    assert.strictEqual(s.playing, 0, 'the cut line is not replayed');
    assert.strictEqual(s.played, 0);
    await srv.tone('viewer', 400, 440);
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).played === 1));

    // the dock's interrupt (flush) stops the current line and clears what waits
    await srv.tone('viewer', 3000, 330);
    await srv.tone('viewer', 3000, 330);
    assert.ok(await h.waitFor(async () => (await h.vstate(a.page)).playing > 0));
    await srv.flush();
    assert.ok(await h.waitFor(async () => { const x = await h.vstate(a.page); return x.playing === 0 && x.queued.length === 0; }), 'flush stopped it');
  } finally {
    for (const p of pages) await p.context.close().catch(() => {});
    await browser.close();
    await srv.stop();
  }
});

test('"also play on this PC" reaches the plugin; no page at all means local playback as before', { skip: h.skipReason, timeout: 60000 }, async () => {
  const srv = await h.startServer();
  const browser = await h.launch('obs');
  try {
    let st = await srv.status();
    assert.strictEqual(st.listeners, 0);
    assert.strictEqual(st.local, true, 'no overlay listening: the plugin keeps the desktop voice');
    const p = await h.openPage(browser, srv.base + 'chat.html?voice=1', { mode: 'obs' });
    assert.ok(await h.waitFor(async () => (await srv.status()).listeners === 1));
    assert.strictEqual((await srv.status()).local, false, 'default: local copy off (no double audio)');
    const q = await h.openPage(browser, srv.base + 'chat.html?voice=1&local=1', { mode: 'obs' });
    assert.ok(await h.waitFor(async () => (await srv.status()).local === true), 'leader asked for the local copy');
    await q.context.close();
    assert.ok(await h.waitFor(async () => { const s = await srv.status(); return s.listeners === 1 && s.local === false; }, 4000));
    await p.context.close();
    assert.ok(await h.waitFor(async () => (await srv.status()).listeners === 0, 4000));
    assert.strictEqual((await srv.status()).local, true);
  } finally {
    await browser.close();
    await srv.stop();
  }
});

test('a flood of lines never grows the server past its limits', { skip: h.skipReason, timeout: 60000 }, async () => {
  const srv = await h.startServer();
  try {
    for (let i = 0; i < 120; i++) await srv.tone('spam' + i, 200, 500);
    const s = await srv.status();
    assert.ok(s.clips <= 32, 'held clips: ' + s.clips);
  } finally {
    await srv.stop();
  }
});
