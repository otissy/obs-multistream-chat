'use strict';
// L2: the page's render logic, driven through window.__oms.render in headless Chrome (?static=1).
// NOTE: hostile-handle / path-tricks tests were NOT written: the Norton agent hook blocked the
// first version of this file. They are listed as a gap in PROJECT.md; static checks in L1 (no
// innerHTML, textContent only, fixed platform list) stand in for them.
const test = require('node:test');
const assert = require('node:assert');
const { Browser, startMockFeed, chromeAvailable } = require('./helpers');

if (!chromeAvailable()) {
  test('L2 overlay (needs Chrome)', t => t.skip('UNVERIFIED: Chrome not installed'));
} else {
  let feed, browser, page;
  test.before(async () => {
    feed = await startMockFeed();
    browser = await Browser.launch();
    page = await browser.page(800, 200, true);
    await page.goto(feed.url('/overlay.html?static=1'));
  });
  test.after(async () => { await browser.close(); feed.close(); });

  const render = d => page.eval(`window.__oms.render(${JSON.stringify({ destinations: d })}); true`);
  const q = js => page.eval(js);

  test('renders one element per destination with state and handle', async () => {
    await render([{ id: 'a', platform: 'twitch', handle: 'otiyyy', state: 'live' }, { id: 'b', platform: 'youtube', handle: 'otiyyy', state: 'idle' }]);
    assert.strictEqual(await q('document.querySelectorAll(".dest").length'), 2);
    assert.deepStrictEqual(await q('[...document.querySelectorAll(".dest")].map(e => e.dataset.state)'), ['live', 'idle']);
    assert.deepStrictEqual(await q('[...document.querySelectorAll(".name")].map(e => e.textContent)'), ['otiyyy', 'otiyyy']);
  });

  test('handle text is shown literally, markup characters included', async () => {
    await render([{ id: 'a', platform: 'twitch', handle: '<b>bold</b> & more', state: 'live' }]);
    assert.strictEqual(await q('document.querySelectorAll("#bar b").length'), 0);
    assert.strictEqual(await q('document.querySelector(".name").textContent'), '<b>bold</b> & more');
  });

  test('unknown platform falls back to the generic icon', async () => {
    await render([{ id: 'a', platform: 'nosuchsite', handle: 'x', state: 'live' }]);
    assert.strictEqual(await q('document.querySelector(".glyph").style.getPropertyValue("--mask")'), 'url("/icons/generic.svg")');
  });

  test('unknown state is shown as idle (dim), not lit', async () => {
    await render([{ id: 'a', platform: 'twitch', handle: 'x', state: 'mystery' }]);
    assert.strictEqual(await q('document.querySelector(".dest").dataset.state'), 'idle');
  });

  test('lit only when live or degraded; dim otherwise; pulse only when reconnecting', async () => {
    const states = ['idle', 'connecting', 'live', 'degraded', 'reconnecting', 'failed'];
    await render(states.map((s, i) => ({ id: 'd' + i, platform: 'twitch', handle: 'h', state: s })));
    const info = await q(`[...document.querySelectorAll(".dest")].map(e => { const g = getComputedStyle(e.querySelector(".glyph")); return { s: e.dataset.state, op: +g.opacity, filter: g.filter, anim: g.animationName, bg: g.backgroundColor }; })`);
    const by = Object.fromEntries(info.map(i => [i.s, i]));
    assert.strictEqual(by.live.op, 1); assert.strictEqual(by.live.filter, 'none'); assert.strictEqual(by.live.anim, 'none');
    assert.strictEqual(by.degraded.op, 1); assert.strictEqual(by.degraded.anim, 'none');
    assert.ok(by.idle.op < 0.6 && by.idle.filter.includes('grayscale'));
    assert.ok(by.failed.op < 0.6 && by.failed.filter.includes('grayscale'));
    assert.ok(by.connecting.op < 0.6);
    assert.strictEqual(by.reconnecting.anim, 'pulse');
    assert.notStrictEqual(by.idle.anim, 'pulse');
    assert.notStrictEqual(by.live.bg, by.idle.bg);
  });

  test('destinations removed from the feed disappear; order follows the feed', async () => {
    await render([{ id: 'a', platform: 'twitch', handle: '1', state: 'live' }, { id: 'b', platform: 'kick', handle: '2', state: 'live' }, { id: 'c', platform: 'tiktok', handle: '3', state: 'live' }]);
    await render([{ id: 'c', platform: 'tiktok', handle: '3', state: 'live' }, { id: 'a', platform: 'twitch', handle: '1', state: 'idle' }]);
    assert.deepStrictEqual(await q('[...document.querySelectorAll(".dest")].map(e => e.dataset.id)'), ['c', 'a']);
  });

  test('empty or odd feeds render without error', async () => {
    await render([]);
    assert.strictEqual(await q('document.querySelectorAll(".dest").length'), 0);
    assert.strictEqual(await q('(() => { try { window.__oms.render(null); window.__oms.render({}); return true; } catch (e) { return String(e); } })()'), true);
  });

  const prop = n => `getComputedStyle(document.documentElement).getPropertyValue("${n}").trim()`;
  test('query: theme, layout, size clamp, accent validation, names', async () => {
    await page.goto(feed.url('/overlay.html?static=1&theme=light&layout=v&size=64&accent=00ff88&names=0'));
    assert.strictEqual(await q('document.body.dataset.theme + document.body.dataset.layout + document.body.dataset.names'), 'lightv0');
    assert.strictEqual(await q(prop('--size') + ' + ' + prop('--accent')), '64px#00ff88');
    await page.goto(feed.url('/overlay.html?static=1&theme=bogus&layout=zz&size=99999&accent=notacolor'));
    assert.strictEqual(await q('document.body.dataset.theme + document.body.dataset.layout'), 'darkh');
    assert.strictEqual(await q(prop('--size') + ' + ' + prop('--accent')), '48px#7c5cff');
  });

  test('hide_idle hides everything that is not connected', async () => {
    await page.goto(feed.url('/overlay.html?static=1&hide_idle=1'));
    await render([{ id: 'a', platform: 'twitch', handle: 'x', state: 'idle' }, { id: 'b', platform: 'kick', handle: 'y', state: 'live' }]);
    assert.deepStrictEqual(await q('[...document.querySelectorAll(".dest")].filter(e => getComputedStyle(e).display !== "none").map(e => e.dataset.id)'), ['b']);
  });
}
