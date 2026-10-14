'use strict';
// L3: page + feed together: SSE updates, polling fallback, stale detection, CSP respected.
const test = require('node:test');
const assert = require('node:assert');
const { Browser, startMockFeed, chromeAvailable } = require('./helpers');

if (!chromeAvailable()) {
  test('L3 overlay (needs Chrome)', t => t.skip('UNVERIFIED: Chrome not installed'));
} else {
  let feed, browser, page;
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const states = () => page.eval('[...document.querySelectorAll(".dest")].map(e => e.dataset.state).join()');
  async function waitFor(fn, want, ms = 4000) { const t0 = Date.now(); let v; while (Date.now() - t0 < ms) { v = await fn(); if (v === want) return v; await sleep(100); } return v; }

  test.before(async () => {
    feed = await startMockFeed();
    browser = await Browser.launch();
    page = await browser.page(800, 200, true);
  });
  test.after(async () => { await browser.close(); feed.close(); });

  test('page follows live pushes: idle -> connecting -> live -> reconnecting -> live', async () => {
    feed.set([{ platform: 'twitch', handle: 'otiyyy', state: 'idle' }, { platform: 'youtube', handle: 'otiyyy', state: 'idle' }], false);
    await page.goto(feed.url('/overlay.html'));
    assert.strictEqual(await waitFor(states, 'idle,idle'), 'idle,idle');
    for (const [a, b] of [['connecting', 'connecting'], ['live', 'connecting'], ['live', 'live'], ['live', 'reconnecting'], ['live', 'live']]) {
      feed.set([{ platform: 'twitch', handle: 'otiyyy', state: a }, { platform: 'youtube', handle: 'otiyyy', state: b }]);
      assert.strictEqual(await waitFor(states, a + ',' + b), a + ',' + b);
    }
  });

  test('CSP is honoured: no console errors or violations while rendering', async () => {
    const errs = browser.events.filter(e => e.method === 'Runtime.exceptionThrown' || (e.method === 'Log.entryAdded' && e.params.entry.level === 'error'));
    assert.strictEqual(errs.length, 0, JSON.stringify(errs.slice(0, 2)));
  });

  test('feed silence marks the overlay stale (dim) instead of keeping a lie lit', async () => {
    feed.set([{ platform: 'twitch', handle: 'otiyyy', state: 'live' }]);
    assert.strictEqual(await waitFor(states, 'live'), 'live');
    feed.close();               // plugin gone: SSE and polling both fail
    assert.strictEqual(await waitFor(() => page.eval('document.body.dataset.stale || ""'), '1', 12000), '1');
    await sleep(600); // let the opacity transition finish
    assert.ok(+(await page.eval('getComputedStyle(document.querySelector(".dest .glyph")).opacity')) < 0.5);
  });
}
