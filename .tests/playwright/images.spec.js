'use strict';
// Images in the ONE chat overlay after the squish fix and the access key: platform logos (served by
// the plugin under /k/<key>/icons/), emote images (from the platforms' CDNs, allowed by the page's
// CSP), badges (read by the reader as names; the overlay does not draw them), the live platform bar
// (platforms=top|bottom, the same bar overlay.html shows), and how a broken image degrades.
// Real C++ server, real page; only the CDN answers are local (Playwright route), never the page.
const { test, expect } = require('playwright/test');
const path = require('path');
const { startServer, routeEmotes, PROOF_DIR } = require('./harness');

const SIZES = [[800, 400], [1920, 1080], [1280, 720], [390, 844]];
const BRAND = { twitch: [0x91, 0x46, 0xff], youtube: [0xff, 0x4e, 0x45], kick: [0x53, 0xfc, 0x18], tiktok: [0x25, 0xf4, 0xee], x: [0xf2, 0xf2, 0xf5] };
const EMOTE = {
  twitch: 'https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/1.0',
  youtube: 'https://www.youtube.com/s/gaming/emoji/7ff574f2/emoji_u1f642.svg',
  kick: 'https://files.kick.com/emotes/37226/fullsize',
  tiktok: 'https://p16-webcast.tiktokcdn.com/img/emote/1.png',
  x: 'https://pbs.twimg.com/emoji/v2/72x72/1f642.png',
};
const BADGES = { mod: ['Moderator'], sub: ['Subscriber (6 months)'], vip: ['VIP'], broadcaster: ['Broadcaster'], verified: ['Verified'], unknown: ['Some New Badge'], none: [] };
const PLATS = Object.keys(EMOTE);
test.describe.configure({ timeout: 240000 }); // many real screenshots; the PC runs this at below-normal priority

// records CSP violations and failed responses of the page
async function watch(page, srv) {
  await page.addInitScript(() => { window.__csp = []; document.addEventListener('securitypolicyviolation', e => window.__csp.push(e.violatedDirective + ' ' + e.blockedURI)); });
  const bad = [], local = [], cons = [];
  page.on('response', r => { const u = r.url(); if (u.startsWith(srv.base)) local.push([new URL(u).pathname.replace(srv.key, '<key>'), r.status()]); if (r.status() >= 400) bad.push(r.status() + ' ' + u.replace(srv.key, '<key>')); });
  page.on('console', m => { if (m.type() === 'error') cons.push(m.text()); });
  page.on('pageerror', e => cons.push(String(e)));
  return { bad, local, cons, csp: () => page.evaluate(() => window.__csp) };
}

// share of the element's pixels close to an RGB colour, read from a real screenshot in a blank page
async function colourShare(page, locator, rgb) {
  const b64 = (await locator.screenshot()).toString('base64');
  const ctx = page.context();
  const probe = ctx.__probe || (ctx.__probe = await ctx.newPage()); // one blank page per context, reused
  const share = await probe.evaluate(async ([b, c]) => {
    const img = new Image(); img.src = 'data:image/png;base64,' + b; await img.decode();
    const cv = document.createElement('canvas'); cv.width = img.width; cv.height = img.height;
    const g = cv.getContext('2d'); g.drawImage(img, 0, 0);
    const d = g.getImageData(0, 0, cv.width, cv.height).data; let hit = 0;
    for (let i = 0; i < d.length; i += 4) if (Math.abs(d[i] - c[0]) + Math.abs(d[i + 1] - c[1]) + Math.abs(d[i + 2] - c[2]) < 90) hit++;
    return hit / (d.length / 4);
  }, [b64, rgb]);
  return share;
}

async function openChat(browser, srv, W, H, query, emoteOpts = {}) {
  const ctx = await browser.newContext({ viewport: { width: W, height: H } });
  const page = await ctx.newPage();
  const w = await watch(page, srv);
  const cdn = await routeEmotes(page, emoteOpts);
  const resp = await page.goto(srv.keyed('/chat.html?theme=dark&font=22&max=30' + (query || '')));
  expect(resp.status()).toBe(200);
  await page.waitForTimeout(300);
  return { ctx, page, w, cdn };
}

async function waitLines(page, n) {
  await page.waitForFunction(k => document.querySelectorAll('#chat .msg').length >= k && [...document.images].every(i => i.complete) && document.getAnimations().length === 0, n, { timeout: 20000 });
  await page.waitForTimeout(200);
}

for (const [W, H] of SIZES) {
  test.describe(`${W}x${H}`, () => {
    let srv;
    test.beforeEach(async () => { srv = await startServer(); });
    test.afterEach(async () => { if (srv) await srv.stop(); srv = null; });

    test('every platform\'s logo loads through the key and sits next to that platform\'s messages, with every badge type and long names', async ({ browser }) => {
      const { ctx, page, w } = await openChat(browser, srv, W, H);
      // 14 messages: every platform (i % 5) and every badge type (i % 7), one 40-character name
      const kinds = Object.keys(BADGES);
      const sent = Array.from({ length: 14 }, (_, i) => { const p = PLATS[i % 5], bk = kinds[i % 7];
        return { platform: p, badges: BADGES[bk], author: bk === 'unknown' ? ('AVeryLongDisplayNameThatGoesOnAndOn_' + p).slice(0, 40) : p + '_' + bk, parts: [{ t: 'text', v: `${p} ${bk}` }] }; });
      await srv.ingest(sent);
      await waitLines(page, 14);
      const lines = await page.evaluate(() => [...document.querySelectorAll('#chat .msg')].filter(e => getComputedStyle(e).display !== 'none').map(e => {
        const ico = e.querySelector('.ico'), name = e.querySelector('.name'), r = ico.getBoundingClientRect(), n = name.getClientRects()[0]; // first line of a wrapped name
        return { p: e.dataset.platform, mask: getComputedStyle(ico).webkitMaskImage, name: name.textContent, text: e.querySelector('.text').textContent, icoX: r.x, icoW: r.width, icoMid: r.y + r.height / 2, nameX: n.x, nameTop: n.top, nameLineH: parseFloat(getComputedStyle(name).lineHeight) || 30, imgs: e.querySelectorAll('img').length, kids: e.children.length };
      }));
      expect(lines.length).toBeGreaterThanOrEqual(5);
      expect(new Set(lines.map(l => l.p)), 'all five platforms on screen').toEqual(new Set(PLATS));
      const shownSent = sent.slice(sent.length - lines.length);
      lines.forEach((l, i) => {
        const m = shownSent[i];
        expect(l.p, 'line platform').toBe(m.platform);
        expect(l.mask, `logo of ${m.platform} line`).toContain(`/k/${srv.key}/icons/${m.platform}.svg`);
        expect(l.name, 'author shown whole, no badge text mixed in').toBe(m.author);
        expect(l.text).toBe(m.parts[0].v);
        for (const b of m.badges) { expect(l.name).not.toContain(b); expect(l.text).not.toContain(b); }
        expect(l.kids, 'logo, name, colon, text: nothing else (badges are not drawn)').toBe(4);
        expect(l.icoW).toBeGreaterThanOrEqual(16);
        expect(l.icoX + l.icoW, 'logo comes before the name').toBeLessThanOrEqual(l.nameX + 1);
        expect(Math.abs(l.icoMid - (l.nameTop + l.nameLineH / 2)), 'logo on the name\'s first line').toBeLessThanOrEqual(l.nameLineH / 2);
      });
      const msgEls = page.locator('#chat .msg:not([hidden])');
      for (let i = 0; i < lines.length; i++) {
        const share = await colourShare(page, msgEls.nth(i).locator('.ico'), BRAND[lines[i].p]);
        expect(share, `${lines[i].p} logo is drawn in its brand colour (share of brand pixels)`).toBeGreaterThan(0.04); // measured: X 0.088 (thin white glyph), others higher; a logo that failed to load measures 0
      }
      for (const p of new Set(lines.map(l => l.p))) expect(w.local, `${p} logo answered 200 under the key`).toContainEqual([`/k/<key>/icons/${p}.svg`, 200]);
      expect(w.bad, 'no failed request (no 401 from the key, no 404)').toEqual([]);
      expect(await w.csp(), 'no CSP violation').toEqual([]);
      expect(w.cons).toEqual([]);
      expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(W);
      await page.screenshot({ path: path.join(PROOF_DIR, `images-logos-badges-${W}x${H}.png`) });
      await ctx.close();
    });

    test('emote images from every platform\'s CDN load (naturalWidth > 0, allowed host, no CSP violation)', async ({ browser }) => {
      const { ctx, page, w, cdn } = await openChat(browser, srv, W, H, '', { delayMs: 150 });
      await srv.ingest(PLATS.map(p => ({ platform: p, parts: [{ t: 'text', v: p + ' says ' }, { t: 'emote', v: 'Emo_' + p, u: EMOTE[p] }, { t: 'text', v: ' ok' }] })));
      await waitLines(page, PLATS.length);
      const imgs = await page.evaluate(() => [...document.querySelectorAll('#chat .msg:not([hidden])')].map(e => { const i = e.querySelector('img.emote'); return { p: e.dataset.platform, src: i && i.src, nw: i ? i.naturalWidth : 0, h: i ? i.getBoundingClientRect().height : 0, alt: i && i.alt, font: parseFloat(getComputedStyle(e).fontSize) }; }));
      expect(imgs.map(i => i.p)).toEqual(PLATS);
      for (const i of imgs) {
        expect(i.src, `${i.p} emote`).toBe(EMOTE[i.p]);
        expect(i.nw, `${i.p} emote decoded`).toBe(112);
        expect(Math.abs(i.h - 1.5 * i.font), `${i.p} emote drawn 1.5em tall`).toBeLessThanOrEqual(1);
        expect(i.alt).toBe('Emo_' + i.p);
      }
      expect(new Set(cdn.map(u => new URL(u).host))).toEqual(new Set(PLATS.map(p => new URL(EMOTE[p]).host)));
      expect(await w.csp(), 'no CSP violation').toEqual([]);
      expect(w.bad).toEqual([]);
      expect(w.cons).toEqual([]);
      await page.screenshot({ path: path.join(PROOF_DIR, `images-emotes-${W}x${H}.png`) });
      await ctx.close();
    });

    test('emotes absent (emotes=0): the emote name is text, no image request is made', async ({ browser }) => {
      const { ctx, page, w, cdn } = await openChat(browser, srv, W, H, '&emotes=0');
      await srv.ingest(PLATS.map(p => ({ platform: p, parts: [{ t: 'text', v: p + ' says ' }, { t: 'emote', v: 'Emo_' + p, u: EMOTE[p] }] })));
      await waitLines(page, PLATS.length);
      const t = await page.evaluate(() => [...document.querySelectorAll('#chat .msg .text')].map(e => [e.textContent, e.querySelectorAll('img').length]));
      expect(t).toEqual(PLATS.map(p => [`${p} says Emo_${p}`, 0]));
      expect(cdn).toEqual([]);
      expect(w.bad).toEqual([]);
      await ctx.close();
    });

    test('a broken emote image degrades to its name: visible text, no empty gap, same size as the plain-text line', async ({ browser }) => {
      const { ctx, page, w } = await openChat(browser, srv, W, H, '', { delayMs: 150, fail: true });
      // each broken-emote line is followed by the same words as plain text: the two must look the same
      const msgs = [];
      for (const p of PLATS) { msgs.push({ platform: p, parts: [{ t: 'text', v: 'gg ' }, { t: 'emote', v: 'Kappa', u: EMOTE[p] }, { t: 'text', v: ' wp' }] }); msgs.push({ platform: p, parts: [{ t: 'text', v: 'gg Kappa wp' }] }); }
      await srv.ingest(msgs);
      await waitLines(page, msgs.length > 10 ? 10 : msgs.length);
      const rows = await page.evaluate(() => [...document.querySelectorAll('#chat .msg:not([hidden])')].map(e => { const t = e.querySelector('.text'); const r = e.getBoundingClientRect(); const tr = t.getBoundingClientRect(); return { text: t.textContent, imgs: e.querySelectorAll('img').length, h: r.height, tw: tr.width }; }));
      expect(rows.length % 2 === 0 ? rows.length : rows.length - 1).toBeGreaterThanOrEqual(2);
      const start = rows.length % 2; // the oldest line may have been pushed out
      for (let i = start; i + 1 < rows.length; i += 2) {
        expect(rows[i].imgs, 'no broken image left behind').toBe(0);
        expect(rows[i].text, 'the emote name is shown instead').toBe('gg Kappa wp');
        expect(Math.abs(rows[i].h - rows[i + 1].h), 'same height as the plain-text line (no gap, no shift)').toBeLessThanOrEqual(0.75);
        expect(Math.abs(rows[i].tw - rows[i + 1].tw), 'same width as the plain-text line').toBeLessThanOrEqual(1);
      }
      expect(await w.csp()).toEqual([]);
      expect(w.bad.every(b => b.startsWith('404 https://')), 'only the CDN answers failed: ' + w.bad.join()).toBe(true);
      await page.screenshot({ path: path.join(PROOF_DIR, `images-broken-emotes-${W}x${H}.png`) });
      await ctx.close();
    });

    test('one overlay: platforms=top puts the live platform bar above the chat; logos lit for live, chat starts below the bar and still ticks', async ({ browser }) => {
      for (const [p, h, s] of [['twitch', 'otiyyy', 'live'], ['youtube', 'otiyyy', 'idle'], ['kick', 'otiyyy', 'live'], ['tiktok', 'otiyyy', 'reconnecting']]) expect(await srv.cmd(`dest ${p} ${h} ${s}`)).toBe('OK');
      const { ctx, page, w } = await openChat(browser, srv, W, H, '&platforms=top&bar_size=40');
      await page.waitForFunction(() => document.querySelectorAll('#bar .dest').length === 4);
      await srv.ingest(Array.from({ length: 60 }, (_, i) => ({ platform: PLATS[i % 5], parts: [{ t: 'text', v: `#${i + 1} gg` }] })));
      await waitLines(page, 1);
      await page.waitForFunction(() => { const m = [...document.querySelectorAll('#chat .msg:not([hidden])')]; return m.length && m[m.length - 1].textContent.includes('#60 '); });
      await page.waitForTimeout(300);
      const g = await page.evaluate(() => { const b = document.getElementById('bar').getBoundingClientRect(); const c = document.getElementById('chat').getBoundingClientRect(); const ms = [...document.querySelectorAll('#chat .msg:not([hidden])')].map(e => e.getBoundingClientRect());
        return { barTop: b.top, barBottom: b.bottom, barH: b.height, chatTop: c.top, chatBottom: c.bottom, firstTop: ms[0].top, lastBottom: ms[ms.length - 1].bottom, n: ms.length, states: [...document.querySelectorAll('#bar .dest')].map(d => d.dataset.state), masks: [...document.querySelectorAll('#bar .glyph')].map(x => getComputedStyle(x).webkitMaskImage) }; });
      expect(g.states).toEqual(['live', 'idle', 'live', 'reconnecting']);
      expect(g.barTop).toBe(0);
      expect(g.barH).toBeGreaterThan(30);
      expect(Math.abs(g.chatTop - g.barBottom), 'the chat box starts right under the bar').toBeLessThanOrEqual(1);
      expect(g.firstTop, 'no chat line under the bar').toBeGreaterThanOrEqual(g.barBottom);
      expect(g.lastBottom).toBeLessThanOrEqual(H + 0.5);
      ['twitch', 'youtube', 'kick', 'tiktok'].forEach((p, i) => expect(g.masks[i]).toContain(`/k/${srv.key}/icons/${p}.svg`));
      const dests = page.locator('#bar .dest .glyph');
      expect(await colourShare(page, dests.nth(0), BRAND.twitch), 'live Twitch logo lit in its colour').toBeGreaterThan(0.12);
      expect(await colourShare(page, dests.nth(2), BRAND.kick), 'live Kick logo lit in its colour').toBeGreaterThan(0.12);
      expect(await colourShare(page, dests.nth(1), BRAND.youtube), 'idle YouTube logo stays grey').toBeLessThan(0.02);
      expect(w.bad).toEqual([]);
      expect(await w.csp()).toEqual([]);
      expect(w.local).toEqual(expect.arrayContaining([['/k/<key>/overlay.js', 200], ['/k/<key>/events', 200], ['/k/<key>/chat/events', 200]]));
      await page.screenshot({ path: path.join(PROOF_DIR, `images-one-overlay-bar-${W}x${H}.png`) });
      await ctx.close();
    });
  });
}

test.describe('the old platform-icons URL keeps working', () => {
  test('/k/<key>/overlay.html still shows the same live bar; chat.html?chat=0&platforms=top shows only the bar', async ({ browser }) => {
    const srv = await startServer();
    try {
      for (const [p, s] of [['twitch', 'live'], ['kick', 'idle']]) expect(await srv.cmd(`dest ${p} otiyyy ${s}`)).toBe('OK');
      const ctx = await browser.newContext({ viewport: { width: 800, height: 200 } });
      const page = await ctx.newPage();
      const w = await watch(page, srv);
      expect((await page.goto(srv.keyed('/overlay.html?theme=dark'))).status()).toBe(200);
      await page.waitForFunction(() => document.querySelectorAll('#bar .dest').length === 2);
      await page.waitForTimeout(700); // overlay.html fades the colour in over 0.3 s
      expect(await page.evaluate(() => [...document.querySelectorAll('#bar .dest')].map(d => d.dataset.state))).toEqual(['live', 'idle']);
      expect(await colourShare(page, page.locator('#bar .dest .glyph').first(), BRAND.twitch)).toBeGreaterThan(0.12);
      await page.screenshot({ path: path.join(PROOF_DIR, 'images-old-overlay-url-800x200.png') });
      expect((await page.goto(srv.keyed('/chat.html?theme=dark&chat=0&platforms=top'))).status()).toBe(200);
      await page.waitForFunction(() => document.querySelectorAll('#bar .dest').length === 2);
      expect(await page.evaluate(() => getComputedStyle(document.getElementById('chat')).display)).toBe('none');
      expect(w.local.filter(l => l[0].includes('chat/events')), 'chat=0 opens no chat stream').toEqual([]);
      expect(await colourShare(page, page.locator('#bar .dest .glyph').first(), BRAND.twitch)).toBeGreaterThan(0.12);
      expect(w.bad).toEqual([]);
      await page.screenshot({ path: path.join(PROOF_DIR, 'images-bar-only-800x200.png') });
      await ctx.close();
    } finally { await srv.stop(); }
  });
});
