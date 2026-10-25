'use strict';
// The local server answers every page and stream ONLY under /k/<key>/, against the real C++ server.
// HTTP level: missing, wrong, partial, too long, upper-case and empty keys get 401 and none of the
// route's content; the right key gets the real file / the real feed.
// Browser level: a keyed page loads its script, icons and live stream through the key; an old
// keyless URL (what an existing OBS browser source holds) shows a visible notice, never a blank page.
const { test, expect } = require('playwright/test');
const fs = require('fs');
const path = require('path');
const { startServer, request, streamFor, contrast, OVERLAY_DIR, PROOF_DIR } = require('./harness');

const PAGES = ['/', '/chat.html', '/overlay.html'];
const DATA = ['/chat.js', '/overlay.js', '/icons/twitch.svg', '/state.json', '/events', '/chat/events', '/chat/status.json'];
const FILES = { '/chat.html': 'chat.html', '/chat.js': 'chat.js', '/overlay.html': 'overlay.html', '/overlay.js': 'overlay.js', '/icons/twitch.svg': 'icons/twitch.svg' };

function badPrefixes(key) {
  const flip = key.slice(0, 63) + (key[63] === '0' ? '1' : '0');
  return {
    missing: '',
    'wrong (last digit changed)': `/k/${flip}`,
    'partial (first half)': `/k/${key.slice(0, 32)}`,
    'too long (one extra digit)': `/k/${key}0`,
    'upper case': `/k/${key.toUpperCase()}`,
    empty: '/k/',
    'key without its slash': `/k/${key}`, // "/k/<key>chat.html"
  };
}

test.describe('HTTP: without the right key nothing is served', () => {
  let srv;
  test.beforeEach(async () => { srv = await startServer(); await srv.ingest([{ author: 'SecretViewer', parts: [{ t: 'text', v: 'private line' }] }]); });
  test.afterEach(async () => { if (srv) await srv.stop(); srv = null; });

  for (const route of [...PAGES, ...DATA]) {
    test(`${route}: every wrong form of the key gets 401 and no content; the right key gets the real thing`, async () => {
      const isPage = PAGES.includes(route);
      const real = FILES[route === '/' ? '/overlay.html' : route] ? fs.readFileSync(path.join(OVERLAY_DIR, FILES[route === '/' ? '/overlay.html' : route]), 'utf8') : null;
      for (const [what, prefix] of Object.entries(badPrefixes(srv.key))) {
        const p = what === 'key without its slash' ? `${prefix}${route.slice(1)}` : prefix + route;
        const r = await request('GET', srv.port, p);
        expect(r.status, `${what}: ${p.replace(srv.key, '<key>')}`).toBe(401);
        expect(r.body).not.toContain('SecretViewer');
        expect(r.body).not.toContain('private line');
        if (real) expect(r.body).not.toBe(real);
        expect(r.body).toContain('access key');
        if (what !== 'key without its slash') { // that one is not a path the pages ever ask for; 401 and no content is all that matters
          expect(r.headers['content-type']).toMatch(isPage ? /^text\/html/ : /^text\/plain/);
          if (!isPage) expect(r.body).toBe('access key required\n');
        }
        const h = await request('HEAD', srv.port, p);
        expect(h.status, `HEAD ${what}`).toBe(401);
      }
      if (route.endsWith('events')) {
        const s = await streamFor(srv.port, `/k/${srv.key}${route}`, 1200);
        expect(s.status).toBe(200);
        expect(s.headers['content-type']).toMatch(/^text\/event-stream/);
        if (route === '/chat/events') expect(s.body).toMatch(/data: .*SecretViewer/);
        else expect(s.body).toMatch(/data: \{"v":1/);
        return;
      }
      const ok = await request('GET', srv.port, `/k/${srv.key}${route}`);
      expect(ok.status).toBe(200);
      if (real) expect(ok.body).toBe(real);
      if (route === '/chat/status.json') expect(JSON.parse(ok.body).counts.accepted).toBe(1);
      if (route === '/state.json') expect(JSON.parse(ok.body)).toHaveProperty('destinations');
    });
  }

  test('event streams without the key end at once and carry no data', async () => {
    for (const route of ['/events', '/chat/events']) {
      for (const [what, prefix] of Object.entries(badPrefixes(srv.key))) {
        if (what === 'key without its slash') continue;
        const s = await streamFor(srv.port, prefix + route + '?backlog=50', 1500);
        expect(s.status, `${route} ${what}`).toBe(401);
        expect(s.closedEarly, `${route} ${what}: the server hangs up instead of streaming`).toBe(true);
        expect(s.body).not.toMatch(/data:/);
      }
    }
  });

  test('the reader route keeps its own bearer token and ignores the access key', async () => {
    const body = JSON.stringify({ v: 1, sent: Date.now(), sources: [], messages: [] });
    const hdr = { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) };
    expect((await request('POST', srv.port, '/chat/ingest', body, hdr)).status, 'no bearer token').toBe(401);
    expect((await request('POST', srv.port, '/chat/ingest', body, Object.assign({ Authorization: 'Bearer ' + srv.key }, hdr))).status, 'the access key is not the reader token').toBe(401);
  });
});

test.describe('browser: keyed pages work end to end through the key', () => {
  let srv;
  test.beforeEach(async () => { srv = await startServer(); });
  test.afterEach(async () => { if (srv) await srv.stop(); srv = null; });

  test('chat page: script, icons and the live stream all load under /k/<key>/; a line posted after load appears', async ({ page }) => {
    const reqs = [];
    page.on('response', r => { if (r.url().startsWith(srv.base)) reqs.push([new URL(r.url()).pathname.replace(srv.key, '<key>'), r.status()]); });
    const errors = []; page.on('pageerror', e => errors.push(String(e)));
    await page.setViewportSize({ width: 800, height: 400 });
    const resp = await page.goto(srv.keyed('/chat.html?theme=dark&font=22&max=30'));
    expect(resp.status()).toBe(200);
    await page.waitForTimeout(300);
    const icon = page.waitForResponse(r => r.url().includes('/icons/kick.svg'));
    await srv.ingest([{ platform: 'kick', author: 'LiveViewer', parts: [{ t: 'text', v: 'arrived after load' }] }]);
    await expect(page.locator('#chat .msg')).toHaveCount(1);
    await expect(page.locator('#chat .msg .name')).toHaveText('LiveViewer');
    await expect(page.locator('#chat .msg .text')).toHaveText('arrived after load');
    const mask = await page.locator('#chat .msg .ico').evaluate(e => getComputedStyle(e).webkitMaskImage);
    expect(mask).toContain(`/k/${srv.key}/icons/kick.svg`);
    expect((await icon).status(), 'the icon itself loads through the key').toBe(200);
    const paths = reqs.map(r => r[0]);
    for (const [p, st] of reqs) { expect(p, 'every request carries the key').toMatch(/^\/k\/<key>\//); expect(st, p).toBe(200); }
    expect(paths).toEqual(expect.arrayContaining(['/k/<key>/chat.html', '/k/<key>/chat.js', '/k/<key>/chat/events']));
    expect(errors).toEqual([]);
    await page.screenshot({ path: path.join(PROOF_DIR, 'key-chat-keyed-800x400.png') });
  });

  test('platform-icons page: its script, state and stream load under /k/<key>/', async ({ page }) => {
    const reqs = [];
    page.on('response', r => { if (r.url().startsWith(srv.base)) reqs.push([new URL(r.url()).pathname.replace(srv.key, '<key>'), r.status()]); });
    await page.setViewportSize({ width: 800, height: 200 });
    const events = page.waitForResponse(r => new URL(r.url()).pathname.endsWith('/events'));
    expect((await page.goto(srv.keyed('/overlay.html?theme=dark'))).status()).toBe(200);
    expect((await events).status()).toBe(200);
    await page.waitForTimeout(300);
    const paths = reqs.map(r => r[0]);
    expect(paths).toEqual(expect.arrayContaining(['/k/<key>/overlay.html', '/k/<key>/overlay.js', '/k/<key>/events']));
    for (const [p, st] of reqs) { expect(p).toMatch(/^\/k\/<key>\//); expect(st, p).toBe(200); }
    expect(await page.evaluate(() => fetch('state.json').then(r => r.status))).toBe(200);
  });
});

const OLD_URLS = {
  'old keyless chat URL (what an existing browser source holds)': '/chat.html?theme=dark&font=22&max=30',
  'old keyless platform-icons URL': '/overlay.html?theme=transparent&layout=v&size=64',
  'old keyless root URL': '/',
};

test.describe('browser: an old or wrong URL fails visibly, never blank', () => {
  let srv;
  test.beforeEach(async () => { srv = await startServer(); await srv.ingest([{ author: 'SecretViewer', parts: [{ t: 'text', v: 'private line' }] }]); });
  test.afterEach(async () => { if (srv) await srv.stop(); srv = null; });

  const cases = Object.entries(OLD_URLS).map(([k, v]) => [k, s => s.plain(v)]).concat([
    ['wrong key', s => `${s.base}/k/${s.key.slice(0, 63)}${s.key[63] === 'a' ? 'b' : 'a'}/chat.html?theme=dark`],
    ['partial key', s => `${s.base}/k/${s.key.slice(0, 40)}/chat.html`],
  ]);
  for (const [what, url] of cases) {
    for (const [W, H] of [[800, 400], [390, 844], [1920, 1080]]) {
      test(`${what} at ${W}x${H}: a readable notice inside the source, no chat data`, async ({ page }) => {
        await page.setViewportSize({ width: W, height: H });
        const resp = await page.goto(url(srv));
        expect(resp.status()).toBe(401);
        const n = page.locator('#omsc-key-notice');
        await expect(n).toBeVisible();
        await expect(n).toContainText('access key');
        await expect(n).toContainText('Copy the browser source URL again');
        const g = await n.evaluate(e => { const r = e.getBoundingClientRect(); const cs = getComputedStyle(e); return { x: r.x, y: r.y, w: r.width, h: r.height, fg: cs.color, bg: cs.backgroundColor, fs: parseFloat(cs.fontSize), sw: document.documentElement.scrollWidth }; });
        expect(g.x).toBeGreaterThanOrEqual(0);
        expect(g.y).toBeGreaterThanOrEqual(0);
        expect(g.x + g.w).toBeLessThanOrEqual(W);
        expect(g.y + g.h).toBeLessThanOrEqual(H);
        expect(g.w, 'wide enough to read').toBeGreaterThanOrEqual(Math.min(300, W - 40));
        expect(g.fs, 'font size').toBeGreaterThanOrEqual(18);
        expect(g.bg, 'own opaque background: readable over any scene in OBS (the page itself is transparent)').toMatch(/^rgb\(/);
        expect(contrast(g.fg, g.bg), 'text contrast').toBeGreaterThanOrEqual(4.5);
        expect(g.sw).toBeLessThanOrEqual(W);
        const text = await page.evaluate(() => document.body.innerText);
        expect(text).not.toContain('SecretViewer');
        expect(await page.locator('script').count(), 'no script runs on the notice').toBe(0);
        await page.screenshot({ path: path.join(PROOF_DIR, `key-notice-${what.replace(/\W+/g, '-')}-${W}x${H}.png`) });
      });
    }
  }
});
