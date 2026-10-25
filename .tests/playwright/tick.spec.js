'use strict';
// The chat overlay must TICK: each line keeps its natural height, and when the source is full the
// oldest WHOLE lines leave at the top. Regression of 2026-10-24: lines were squeezed (flex-shrink on
// an overflow:hidden flex item) instead.
//
// How "natural" is known without trusting the page: the same messages are replayed (backlog) into a
// second copy of the same page, same width, 16000 px tall, where nothing has to give way. Its line
// heights are the reference. The page under test must show exactly the newest lines whose
// reference heights fit its box, each at its reference height, flush with the bottom (align=bottom)
// or the top (align=top), none cut. Everything runs against the real C++ server and real page.
const { test, expect } = require('playwright/test');
const path = require('path');
const { startServer, routeEmotes, TW_EMOTE, PROOF_DIR } = require('./harness');

const SIZES = [[800, 400], [1920, 1080], [1280, 720], [390, 844]];
const ALIGNS = ['bottom', 'top'];
const H_TOL = 0.75; // px, line height vs reference
const P_TOL = 1.0;  // px, positions

const WORDS = 'that was a clean play how did you even do that first time here love the stream'.split(' ');
const short = n => [{ t: 'text', v: `#${n} gg` }];
const medium = n => [{ t: 'text', v: `#${n} ` + Array.from({ length: 14 }, (_, i) => WORDS[(n + i) % WORDS.length]).join(' ') }];
const veryLong = n => [{ t: 'text', v: (`#${n} ` + Array.from({ length: 120 }, (_, i) => WORDS[(n + i) % WORDS.length]).join(' ')).slice(0, 500) }];
const withEmote = n => [{ t: 'text', v: `#${n} nice ` }, { t: 'emote', v: 'Kappa', u: TW_EMOTE }, { t: 'text', v: ' gg' }];

function pageUrl(srv, align, extra = '') {
  return srv.keyed(`/chat.html?theme=dark&font=22&max=100&backlog=200&align=${align}${extra}`);
}

const MEASURE = `(() => {
  const chat = document.getElementById('chat'); const cs = getComputedStyle(chat); const box = chat.getBoundingClientRect();
  const innerTop = box.top + parseFloat(cs.paddingTop), innerBottom = box.bottom - parseFloat(cs.paddingBottom);
  const all = [...chat.querySelectorAll('.msg')];
  const idOf = e => (e.querySelector('.text').textContent.match(/#(\\d+)/) || [])[1] | 0;
  const ms = all.filter(e => getComputedStyle(e).display !== 'none').map(e => { const r = e.getBoundingClientRect();
    return { id: (e.querySelector('.text').textContent.match(/#(\\d+)/) || [])[1] | 0, top: r.top, bottom: r.bottom, h: r.height,
      cap: parseFloat(getComputedStyle(e).maxHeight), imgs: e.querySelectorAll('img').length }; });
  return { innerTop, innerBottom, room: innerBottom - innerTop, allIds: all.map(idOf), hiddenIds: all.filter(e => getComputedStyle(e).display === 'none').map(idOf), box: { x: box.x, y: box.y, w: box.width, h: box.height },
    bg: cs.backgroundColor, ms, vw: innerWidth, vh: innerHeight, sw: document.documentElement.scrollWidth, sh: document.documentElement.scrollHeight };
})()`;

// waits until the page shows message #lastId, every image has settled and no animation runs
async function settle(page, lastId) {
  await page.waitForFunction(id => {
    const ms = [...document.querySelectorAll('#chat .msg')];
    if (!ms.length || !ms[ms.length - 1].querySelector('.text').textContent.includes('#' + id + ' ')) return false;
    if ([...document.images].some(i => !i.complete)) return false;
    return document.getAnimations().length === 0;
  }, lastId, { timeout: 20000 });
  await page.waitForTimeout(150); // a load/error handler may still run trimToFit
}

async function reference(browser, srv, width, align, extra, sent, emoteOpts) {
  const ctx = await browser.newContext({ viewport: { width, height: 16000 } });
  const ref = await ctx.newPage();
  if (emoteOpts) await routeEmotes(ref, emoteOpts);
  await ref.goto(pageUrl(srv, align, extra));
  await settle(ref, sent[sent.length - 1]);
  const m = await ref.evaluate(MEASURE);
  await ctx.close();
  expect(m.ms.map(x => x.id), 'reference shows every message').toEqual(sent);
  return new Map(m.ms.map(x => [x.id, x.h]));
}

function expectedKept(sent, refH, room, maxLines) {
  let used = 0, k = 0;
  for (let i = sent.length - 1; i >= 0; i--) {
    if (k >= 1 && used + refH.get(sent[i]) > room + 0.5) break;
    used += refH.get(sent[i]); k++;
  }
  return sent.slice(sent.length - Math.min(k, maxLines));
}

function checkTicked(m, sent, refH, align, maxLines, label) {
  const want = expectedKept(sent, refH, m.room, maxLines);
  expect(m.ms.map(x => x.id), `${label}: exactly the newest lines that fit, oldest gone (room ${m.room.toFixed(1)} px)`).toEqual(want);
  const firstShown = want.length ? m.allIds.indexOf(want[0]) : m.allIds.length;
  expect(m.hiddenIds, `${label}: only the oldest lines are out of view, all of them before the first shown one`).toEqual(m.allIds.slice(0, firstShown));
  for (const x of m.ms) {
    expect(Math.abs(x.h - refH.get(x.id)), `${label}: line #${x.id} is ${x.h.toFixed(2)} px, natural ${refH.get(x.id).toFixed(2)} px`).toBeLessThanOrEqual(H_TOL);
  }
  if (m.ms.length) {
    const first = m.ms[0], last = m.ms[m.ms.length - 1];
    if (align === 'bottom') expect(Math.abs(last.bottom - m.innerBottom), `${label}: newest line sits on the bottom edge (${last.bottom.toFixed(1)} vs ${m.innerBottom.toFixed(1)})`).toBeLessThanOrEqual(P_TOL);
    else expect(Math.abs(first.top - m.innerTop), `${label}: oldest kept line sits on the top edge (${first.top.toFixed(1)} vs ${m.innerTop.toFixed(1)})`).toBeLessThanOrEqual(P_TOL);
    if (m.ms.length > 1 || refH.get(last.id) <= m.room) {
      expect(first.top, `${label}: no line cut at the top`).toBeGreaterThanOrEqual(m.innerTop - P_TOL);
      expect(last.bottom, `${label}: no line cut at the bottom`).toBeLessThanOrEqual(m.innerBottom + P_TOL);
    }
    for (let i = 1; i < m.ms.length; i++) expect(Math.abs(m.ms[i].top - m.ms[i - 1].bottom), `${label}: lines are stacked without overlap`).toBeLessThanOrEqual(P_TOL);
  }
  expect(m.sw, `${label}: no horizontal overflow`).toBeLessThanOrEqual(m.vw);
  expect(m.sh, `${label}: no vertical page overflow`).toBeLessThanOrEqual(m.vh);
  return want;
}

async function open(browser, srv, w, h, align, extra = '', emoteOpts) {
  const ctx = await browser.newContext({ viewport: { width: w, height: h } });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(String(e)));
  if (emoteOpts) await routeEmotes(page, emoteOpts);
  const resp = await page.goto(pageUrl(srv, align, extra));
  expect(resp.status()).toBe(200);
  await page.waitForTimeout(300); // the event stream is open before the first message
  return { ctx, page, errors };
}

const shot = (page, name) => page.screenshot({ path: path.join(PROOF_DIR, name + '.png') });

for (const [W, H] of SIZES) {
  for (const align of ALIGNS) {
    const tag = `${W}x${H} align=${align}`;
    const file = `tick-${W}x${H}-${align}`;

    test.describe(tag, () => {
      let srv;
      test.beforeEach(async () => { srv = await startServer(); });
      test.afterEach(async () => { if (srv) await srv.stop(); srv = null; });

      test('empty: no lines, the card covers the whole source', async ({ browser }) => {
        const { ctx, page, errors } = await open(browser, srv, W, H, align);
        const m = await page.evaluate(MEASURE);
        expect(m.ms).toEqual([]);
        expect(m.box).toEqual({ x: 0, y: 0, w: W, h: H });
        expect(m.bg).toBe('rgba(18, 18, 24, 0.82)');
        expect(errors).toEqual([]);
        await shot(page, `${file}-empty`);
        await ctx.close();
      });

      test('one message: natural height, on its edge', async ({ browser }) => {
        const { ctx, page, errors } = await open(browser, srv, W, H, align);
        await srv.ingest([{ parts: short(1) }]);
        await settle(page, 1);
        const refH = await reference(browser, srv, W, align, '', [1]);
        checkTicked(await page.evaluate(MEASURE), [1], refH, align, 100, tag + ' one');
        expect(errors).toEqual([]);
        await shot(page, `${file}-one`);
        await ctx.close();
      });

      test('a few messages: all shown, natural heights, stacked from the edge', async ({ browser }) => {
        const { ctx, page, errors } = await open(browser, srv, W, H, align);
        await srv.ingest([{ parts: short(1) }, { parts: medium(2) }, { parts: short(3) }]);
        await settle(page, 3);
        const refH = await reference(browser, srv, W, align, '', [1, 2, 3]);
        const kept = checkTicked(await page.evaluate(MEASURE), [1, 2, 3], refH, align, 100, tag + ' few');
        expect(kept).toEqual([1, 2, 3]);
        expect(errors).toEqual([]);
        await shot(page, `${file}-few`);
        await ctx.close();
      });

      test('exactly full: every line kept; one more pushes exactly the oldest out', async ({ browser }) => {
        const { ctx, page, errors } = await open(browser, srv, W, H, align);
        await srv.ingest([{ parts: short(1) }]);
        await settle(page, 1);
        const m1 = await page.evaluate(MEASURE);
        const h1 = m1.ms[0].h;
        const k = Math.floor((m1.room + 0.5) / h1);
        expect(k, 'room for at least two lines').toBeGreaterThanOrEqual(2);
        await srv.ingest(Array.from({ length: k - 1 }, (_, i) => ({ parts: short(i + 2) })));
        await settle(page, k);
        const full = await page.evaluate(MEASURE);
        expect(full.ms.map(x => x.id), `exactly full at ${k} lines of ${h1.toFixed(2)} px in ${full.room.toFixed(1)} px`).toEqual(Array.from({ length: k }, (_, i) => i + 1));
        for (const x of full.ms) expect(Math.abs(x.h - h1)).toBeLessThanOrEqual(H_TOL);
        await shot(page, `${file}-exactly-full`);
        await srv.ingest([{ parts: short(k + 1) }]);
        await settle(page, k + 1);
        const more = await page.evaluate(MEASURE);
        expect(more.ms.map(x => x.id), 'one in, the oldest whole line out').toEqual(Array.from({ length: k }, (_, i) => i + 2));
        for (const x of more.ms) expect(Math.abs(x.h - h1)).toBeLessThanOrEqual(H_TOL);
        if (align === 'bottom') expect(Math.abs(more.ms[k - 1].bottom - more.innerBottom)).toBeLessThanOrEqual(P_TOL);
        else expect(Math.abs(more.ms[0].top - more.innerTop)).toBeLessThanOrEqual(P_TOL);
        expect(errors).toEqual([]);
        await ctx.close();
      });

      test('overflowing (mixed one- and two-line messages): ticks, nothing squeezed', async ({ browser }) => {
        const { ctx, page, errors } = await open(browser, srv, W, H, align);
        const n = Math.ceil(H / 30) * 2 + 5;
        const sent = Array.from({ length: n }, (_, i) => i + 1);
        await srv.ingest(sent.map(i => ({ parts: i % 3 === 0 ? medium(i) : short(i) })));
        await settle(page, n);
        const refH = await reference(browser, srv, W, align, '', sent);
        const kept = checkTicked(await page.evaluate(MEASURE), sent, refH, align, 100, tag + ' overflow');
        expect(kept.length).toBeLessThan(n);
        expect(errors).toEqual([]);
        await shot(page, `${file}-overflow`);
        await ctx.close();
      });

      test('very long messages: each capped at 12em, never squeezed below it, oldest leave whole', async ({ browser }) => {
        const { ctx, page, errors } = await open(browser, srv, W, H, align);
        const sent = [1, 2, 3, 4, 5, 6, 7, 8];
        await srv.ingest(sent.map(i => ({ parts: i % 4 === 0 ? short(i) : veryLong(i) })));
        await settle(page, 8);
        const refH = await reference(browser, srv, W, align, '', sent);
        const m = await page.evaluate(MEASURE);
        checkTicked(m, sent, refH, align, 100, tag + ' very long');
        const capPx = 12 * 22 + 4; // max-height 12em on the content box + 2 px padding top and bottom
        for (const x of m.ms) if (x.id % 4 !== 0 && refH.get(x.id) >= capPx - 0.5) expect(Math.abs(x.h - capPx), `#${x.id} is the 12em cap exactly`).toBeLessThanOrEqual(H_TOL);
        expect(errors).toEqual([]);
        await shot(page, `${file}-very-long`);
        await ctx.close();
      });

      test('emote images present (loading late): lines grow when the image arrives, and the chat re-ticks', async ({ browser }) => {
        const emo = { delayMs: 400 };
        const { ctx, page, errors } = await open(browser, srv, W, H, align, '', emo);
        const n = Math.ceil(H / 34) + 6;
        const sent = Array.from({ length: n }, (_, i) => i + 1);
        await srv.ingest(sent.map(i => ({ parts: i % 2 ? withEmote(i) : short(i) })));
        await settle(page, n);
        const m = await page.evaluate(MEASURE);
        expect(m.ms.filter(x => x.imgs === 1).length, 'emotes render as images').toBeGreaterThan(0);
        const refH = await reference(browser, srv, W, align, '', sent, emo);
        checkTicked(m, sent, refH, align, 100, tag + ' emotes');
        expect(errors).toEqual([]);
        await shot(page, `${file}-emotes`);
        await ctx.close();
      });

      test('emote images absent (emotes=0): names as text, still ticks', async ({ browser }) => {
        const { ctx, page, errors } = await open(browser, srv, W, H, align, '&emotes=0', {});
        const n = Math.ceil(H / 34) + 6;
        const sent = Array.from({ length: n }, (_, i) => i + 1);
        await srv.ingest(sent.map(i => ({ parts: i % 2 ? withEmote(i) : short(i) })));
        await settle(page, n);
        const m = await page.evaluate(MEASURE);
        expect(m.ms.every(x => x.imgs === 0), 'no images with emotes=0').toBe(true);
        const refH = await reference(browser, srv, W, align, '&emotes=0', sent, {});
        checkTicked(m, sent, refH, align, 100, tag + ' emotes=0');
        expect(errors).toEqual([]);
        await shot(page, `${file}-emotes-off`);
        await ctx.close();
      });

      test('emote images failing to load: replaced by their names, still ticks', async ({ browser }) => {
        const emo = { delayMs: 200, fail: true };
        const { ctx, page, errors } = await open(browser, srv, W, H, align, '', emo);
        const n = Math.ceil(H / 34) + 6;
        const sent = Array.from({ length: n }, (_, i) => i + 1);
        await srv.ingest(sent.map(i => ({ parts: i % 2 ? withEmote(i) : short(i) })));
        await settle(page, n);
        const m = await page.evaluate(MEASURE);
        expect(m.ms.every(x => x.imgs === 0), 'broken images were replaced by text').toBe(true);
        expect(await page.locator('#chat .msg .text', { hasText: 'Kappa' }).count()).toBeGreaterThan(0);
        const refH = await reference(browser, srv, W, align, '', sent, emo);
        checkTicked(m, sent, refH, align, 100, tag + ' emotes failing');
        expect(errors).toEqual([]);
        await ctx.close();
      });

      test('after a reload: the backlog comes back in the same ticked state', async ({ browser }) => {
        const { ctx, page, errors } = await open(browser, srv, W, H, align);
        const n = Math.ceil(H / 30) + 10;
        const sent = Array.from({ length: n }, (_, i) => i + 1);
        await srv.ingest(sent.map(i => ({ parts: i % 4 === 0 ? medium(i) : short(i) })));
        await settle(page, n);
        const before = await page.evaluate(MEASURE);
        await page.reload();
        await settle(page, n);
        const after = await page.evaluate(MEASURE);
        const refH = await reference(browser, srv, W, align, '', sent);
        checkTicked(after, sent, refH, align, 100, tag + ' after reload');
        expect(after.ms.map(x => [x.id, Math.round(x.top * 10) / 10]), 'same lines at the same places as before the reload').toEqual(before.ms.map(x => [x.id, Math.round(x.top * 10) / 10]));
        expect(errors).toEqual([]);
        await shot(page, `${file}-reloaded`);
        await ctx.close();
      });
    });
  }
}

test.describe('the dock\'s own URL (font=22&max=30) at 1920x1080', () => {
  test('max=30 caps the lines before the height does; the cap also drops whole oldest lines', async ({ browser }) => {
    const srv = await startServer();
    try {
      const ctx = await browser.newContext({ viewport: { width: 1920, height: 1080 } });
      const page = await ctx.newPage();
      await page.goto(srv.keyed('/chat.html?theme=dark&font=22&max=30'));
      await page.waitForTimeout(300);
      const sent = Array.from({ length: 45 }, (_, i) => i + 1);
      await srv.ingest(sent.map(i => ({ parts: short(i) })));
      await settle(page, 45);
      const m = await page.evaluate(MEASURE);
      const refH = await reference(browser, srv, 1920, 'bottom', '', sent);
      const kept = checkTicked(m, sent, refH, 'bottom', 30, 'dock URL');
      expect(kept).toEqual(sent.slice(15));
      await page.screenshot({ path: path.join(PROOF_DIR, 'tick-1920x1080-dock-url.png') });
      await ctx.close();
    } finally { await srv.stop(); }
  });
});
