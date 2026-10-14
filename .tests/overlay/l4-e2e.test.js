'use strict';
// L4: the real overlay files, served by the feed, rendered at true size in headless Chrome.
// Writes proof screenshots to OMS_PROOF_DIR (default: <repo>/../overlay-proof) and checks layout.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const { Browser, startMockFeed, chromeAvailable, PROOF_DIR } = require('./helpers');

if (!chromeAvailable()) {
  test('L4 overlay screenshots (needs Chrome)', t => t.skip('UNVERIFIED: Chrome not installed'));
} else {
  const ALL = ['idle', 'connecting', 'live', 'degraded', 'reconnecting', 'failed'];
  const five = st => ['twitch', 'youtube', 'kick', 'facebook', 'tiktok'].map((p, i) => ({ platform: p, handle: 'otiyyy', state: st[i] }));
  let feed, browser;
  test.before(async () => { feed = await startMockFeed(); browser = await Browser.launch(); fs.mkdirSync(PROOF_DIR, { recursive: true }); });
  test.after(async () => { await browser.close(); feed.close(); });

  async function shot(name, w, h, query, dests, transparentBg) {
    feed.set(dests);
    const page = await browser.page(w, h, true);
    await page.goto(feed.url('/overlay.html' + query));
    await page.eval('new Promise(r => setTimeout(r, 400))');
    if (transparentBg) await page.eval(`document.body.style.background = '${transparentBg}'; true`);
    const file = await page.shot(path.join(PROOF_DIR, name + '.png'));
    const box = await page.eval('(() => { const r = document.getElementById("bar").getBoundingClientRect(); return { w: r.width, h: r.height, right: r.right, bottom: r.bottom, sw: document.documentElement.scrollWidth, sh: document.documentElement.scrollHeight }; })()');
    await page.close();
    assert.ok(fs.statSync(file).size > 500, name);
    return box;
  }

  test('horizontal dark: every state side by side (1000x120)', async () => {
    const dests = ALL.map((s, i) => ({ platform: ['twitch', 'youtube', 'kick', 'facebook', 'tiktok', 'custom'][i], handle: 'otiyyy', state: s }));
    const b = await shot('h-dark-all-states', 1000, 120, '', dests, '#1b2a3a');
    assert.ok(b.right <= 1000 && b.sw <= 1000, JSON.stringify(b));
  });

  test('horizontal light and transparent themes', async () => {
    await shot('h-light', 700, 100, '?theme=light', five(['live', 'live', 'reconnecting', 'idle', 'degraded']), '#d9e2ec');
    await shot('h-transparent', 700, 100, '?theme=transparent', five(['live', 'live', 'reconnecting', 'idle', 'degraded']), '#2c7a4b');
  });

  test('vertical, large, custom accent', async () => {
    const b = await shot('v-dark-size72', 360, 560, '?layout=v&size=72&accent=00e0b0', five(['live', 'live', 'reconnecting', 'failed', 'connecting']), '#33263d');
    assert.ok(b.bottom <= 560 && b.sh <= 560, JSON.stringify(b));
  });

  test('real stream frame 1920x1080 with five destinations stays inside the canvas', async () => {
    const b = await shot('frame-1920x1080', 1920, 1080, '?size=56', five(['live', 'live', 'live', 'reconnecting', 'live']), '#14161c');
    assert.ok(b.right <= 1920 && b.bottom <= 1080 && b.sw <= 1920 && b.sh <= 1080, JSON.stringify(b));
  });

  test('long handles are truncated, not overflowing', async () => {
    const d = five(['live', 'live', 'live', 'live', 'live']).map(x => ({ ...x, handle: 'a-very-long-streamer-handle-that-keeps-going-and-going' }));
    const b = await shot('long-handles', 1920, 120, '?size=48', d);
    assert.ok(b.sw <= 1920, JSON.stringify(b));
  });
}
