'use strict';
// The overlay pages have no controls (no button, link or field): they are OBS browser sources. OBS
// can still forward keyboard and mouse input to them ("Interact"). That input must not scroll,
// select or otherwise move the ticked chat, and must not reveal the hidden older lines.
// Both the chat page (full, align bottom and top) and the access-key notice are covered.
const { test, expect } = require('playwright/test');
const { startServer } = require('./harness');

const LINES = `[...document.querySelectorAll('#chat .msg')].filter(e => getComputedStyle(e).display !== 'none').map(e => { const r = e.getBoundingClientRect(); return e.querySelector('.text').textContent.split(' ')[0] + '@' + r.top.toFixed(1) + '/' + r.height.toFixed(1); })`;

async function fullChat(browser, srv, align) {
  const ctx = await browser.newContext({ viewport: { width: 800, height: 400 } });
  const page = await ctx.newPage();
  await page.goto(srv.keyed(`/chat.html?theme=dark&font=22&max=100&backlog=200&align=${align}`));
  await page.waitForTimeout(300);
  await srv.ingest(Array.from({ length: 40 }, (_, i) => ({ parts: [{ t: 'text', v: `#${i + 1} ${i % 3 ? 'gg' : 'a longer line that still fits on one row'}` }] })));
  await page.waitForFunction(() => { const m = document.querySelectorAll('#chat .msg'); return m.length && m[m.length - 1].textContent.includes('#40 ') && document.getAnimations().length === 0; });
  await page.waitForTimeout(150);
  return { ctx, page };
}

for (const align of ['bottom', 'top']) {
  test.describe(`chat page, full, align=${align}`, () => {
    let srv;
    test.beforeEach(async () => { srv = await startServer(); });
    test.afterEach(async () => { if (srv) await srv.stop(); srv = null; });

    test('there is nothing to focus: Tab never lands on an element of the page', async ({ browser }) => {
      const { ctx, page } = await fullChat(browser, srv, align);
      expect(await page.evaluate(() => document.querySelectorAll('a[href], button, input, select, textarea, [tabindex], [contenteditable]').length)).toBe(0);
      for (let i = 0; i < 4; i++) {
        await page.keyboard.press('Tab');
        expect(await page.evaluate(() => document.activeElement === document.body || document.activeElement === null)).toBe(true);
      }
      await ctx.close();
    });

    test('keyboard: Tab, arrows, Page Up/Down, Home, End, Space and Ctrl+A leave every line where it was', async ({ browser }) => {
      const { ctx, page } = await fullChat(browser, srv, align);
      const before = await page.evaluate(LINES);
      expect(before.length).toBeGreaterThan(5);
      expect(before.length).toBeLessThan(40);
      for (const k of ['Tab', 'ArrowUp', 'ArrowDown', 'PageUp', 'PageDown', 'Home', 'End', 'Space', 'Shift+Tab']) {
        await page.keyboard.press(k);
        expect(await page.evaluate(LINES), `after ${k}`).toEqual(before);
      }
      await page.keyboard.press('Control+A');
      expect(await page.evaluate(LINES)).toEqual(before);
      expect(await page.evaluate(() => document.getElementById('chat').scrollTop)).toBe(0);
      await ctx.close();
    });

    test('mouse: wheel up/down, clicks and a drag across the lines leave every line where it was', async ({ browser }) => {
      const { ctx, page } = await fullChat(browser, srv, align);
      const before = await page.evaluate(LINES);
      await page.mouse.move(400, 200);
      for (const dy of [-600, -600, 600, -2000]) {
        await page.mouse.wheel(0, dy);
        await page.waitForTimeout(120);
        expect(await page.evaluate(LINES), `after wheel ${dy}`).toEqual(before);
      }
      await page.mouse.click(400, 200);
      await page.mouse.dblclick(200, 100);
      await page.mouse.move(20, 20); await page.mouse.down(); await page.mouse.move(700, 380, { steps: 8 }); await page.mouse.up();
      expect(await page.evaluate(LINES), 'after clicks and a drag').toEqual(before);
      expect(await page.evaluate(() => document.getElementById('chat').scrollTop)).toBe(0);
      await ctx.close();
    });
  });
}

test.describe('access-key notice', () => {
  test('keyboard and mouse change nothing on the notice; it stays fully inside the source', async ({ browser }) => {
    const srv = await startServer();
    try {
      const ctx = await browser.newContext({ viewport: { width: 800, height: 400 } });
      const page = await ctx.newPage();
      expect((await page.goto(srv.plain('/chat.html?theme=dark'))).status()).toBe(401);
      const box = () => page.locator('#omsc-key-notice').boundingBox();
      const b0 = await box();
      expect(b0.y + b0.height).toBeLessThanOrEqual(400);
      for (const k of ['Tab', 'End', 'PageDown', 'Space']) { await page.keyboard.press(k); expect(await box(), k).toEqual(b0); }
      await page.mouse.wheel(0, 800); await page.mouse.click(400, 50);
      expect(await box()).toEqual(b0);
      expect(await page.evaluate(() => document.querySelectorAll('a[href], button, input, [tabindex]').length)).toBe(0);
      await ctx.close();
    } finally { await srv.stop(); }
  });
});
