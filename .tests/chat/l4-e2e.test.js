'use strict';
// L4: the whole chain, the way a streamer gets it, with nothing real on the network:
//   local fake platform pages --(reader process: headless Chrome + selector packs)--> POST /chat/ingest
//   --> the REAL C++ HttpServer + ChatHub (oms_chat_fixture) --> SSE --> /chat.html in headless Chrome.
// Proof screenshots go to OMS_PROOF_DIR (default <repo>/../chat-proof) and are checked by assertions.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { TOKEN, READER, PROOF_DIR, chromeAvailable, startFake, startCppChat, findFixtureExe, waitFor, sleep } = require('./helpers');
const { launch } = require('../../reader/lib/chrome');
const { BUILD, EMOTES } = require('./fixtures/dom');

if (!chromeAvailable() || !findFixtureExe()) {
  test('chat end to end', t => t.skip('UNVERIFIED: ' + (chromeAvailable() ? 'oms_chat_fixture is not built' : 'Chrome is not installed')));
} else {
  // a 1x1 green PNG: stands in for any emote image so nothing leaves the machine
  const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==', 'base64');
  let chrome, cppChat, fake;
  const requestedOutside = [];

  async function overlayPage(url, w, h, transparentBackdrop) {
    const cdp = chrome.cdp;
    const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' });
    const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
    const s = (m, p) => cdp.send(m, p || {}, sessionId);
    await s('Page.enable'); await s('Runtime.enable'); await s('Network.enable');
    // every https request is answered locally (never sent): the emote hosts are real names in the CSP
    await s('Fetch.enable', { patterns: [{ urlPattern: 'https://*' }] });
    cdp.on('Fetch.requestPaused', p => {
      requestedOutside.push(p.request.url);
      s('Fetch.fulfillRequest', { requestId: p.requestId, responseCode: 200, responseHeaders: [{ name: 'Content-Type', value: 'image/png' }], body: PNG.toString('base64') }).catch(() => {});
    }, sessionId);
    const consoleErrors = [];
    cdp.on('Runtime.exceptionThrown', p => consoleErrors.push(JSON.stringify(p.exceptionDetails.text)), sessionId);
    await s('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false });
    if (transparentBackdrop) await s('Emulation.setDefaultBackgroundColorOverride', { color: { r: 0, g: 0, b: 0, a: 0 } });
    await s('Page.navigate', { url });
    await sleep(800);
    return {
      consoleErrors,
      async eval(expr) { const r = await s('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true }); if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails)); return r.result.value; },
      async shot(file) { const r = await s('Page.captureScreenshot', { format: 'png' }); fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, Buffer.from(r.data, 'base64')); return file; },
      async close() { await cdp.send('Target.closeTarget', { targetId }); },
    };
  }

  test.before(async () => {
    chrome = await launch({ images: true });
    fs.mkdirSync(PROOF_DIR, { recursive: true });
  });
  test.after(async () => { if (chrome) await chrome.close(); });

  const LINES = '[...document.querySelectorAll("#chat .msg")].map(m => ({ p: m.dataset.platform, a: m.querySelector(".name").textContent, t: m.querySelector(".text").textContent, mask: m.querySelector(".ico").style.getPropertyValue("--mask"), imgs: m.querySelectorAll("img").length }))';

  test('e2e: five platforms, messages arrive once, in order, with the right icons, hostile text stays text', async () => {
    cppChat = await startCppChat();
    fake = await startFake();
    fake.register('twitch', 'twchan', 'twitch');
    fake.register('kick', 'kkchan', 'kick');
    fake.register('youtube', 'dQw4w9WgXcQ', 'youtube');
    fake.register('tiktok', 'ttuser', 'tiktok');
    fake.register('x', 'xbroad1234', 'x');
    const cfg = path.join(os.tmpdir(), 'oms-l4-' + process.pid + '.json');
    fs.writeFileSync(cfg, JSON.stringify({ v: 1, port: cppChat.port, quiet_minutes: 5, sources: [
      { id: 'c1', platform: 'twitch', target: 'twchan' }, { id: 'c2', platform: 'kick', target: 'kkchan' }, { id: 'c3', platform: 'youtube', target: 'dQw4w9WgXcQ' },
      { id: 'c4', platform: 'tiktok', target: 'ttuser' }, { id: 'c5', platform: 'x', target: 'xbroad1234' }] }));
    // the real entry point, as a child process with the token in its environment only
    const reader = spawn(process.execPath, [path.join(READER, 'index.js'), '--config', cfg, '--test-origin', fake.origin, '--exit-on-stdin-close'],
      { stdio: ['pipe', 'ignore', 'pipe'], env: Object.assign({}, process.env, { OMSC_CHAT_TOKEN: TOKEN }), windowsHide: true });
    let rerr = '';
    reader.stderr.on('data', d => { rerr += d; });
    let page;
    try {
      // sources start one by one (1.5 s apart): wait until the plugin side sees all five reading
      await waitFor(async () => { const st = await cppChat.status(); return st.sources.length === 5 && st.sources.every(s => s.state === 'reading'); }, 40000, 500);
      page = await overlayPage(cppChat.url('/chat.html?max=40&font=22&backlog=0'), 460, 720);
      await sleep(500);
      const plan = [
        ['twitch', 'twchan', BUILD.twitch({ author: 'TwitchAnna', color: 'rgb(255, 69, 0)', segments: ['hello from twitch ', { emote: 'Kappa', url: EMOTES.TW_EMOTE }], badges: ['Moderator'] })],
        ['kick', 'kkchan', BUILD.kick({ id: 'kk-1', author: 'KickKai', segments: ['gg ', { emote: 'KEKW', url: EMOTES.KICK_EMOTE }] })],
        ['youtube', 'dQw4w9WgXcQ', BUILD.youtube({ id: 'yt-1', author: 'YtYuki', segments: ['nice stream ', { emote: 'smile', url: EMOTES.YT_EMOJI }] })],
        ['tiktok', 'ttuser', BUILD.tiktok({ author: 'tiktoker1', segments: ['hi tiktok'] })],
        ['x', 'xbroad1234', BUILD.x({ author: 'XavierX', segments: ['hello x'] })],
        ['twitch', 'twchan', BUILD.twitch({ author: 'Mallory', segments: ['<script>window.__pwned=1</script><img src=x onerror="window.__pwned=2"> javascript:alert(3)'] })],
        ['kick', 'kkchan', BUILD.kick({ id: 'kk-2', author: 'KickKai', segments: [{ emote: 'Evil', url: 'https://evil.example/x.png' }, ' evil emote host'] })],
        ['youtube', 'dQw4w9WgXcQ', BUILD.youtube({ id: 'yt-1', author: 'YtYuki', segments: ['nice stream (same id, must show once)'] })],
        ['twitch', 'twchan', BUILD.twitch({ author: 'RtlTroll', segments: ['‮evil‬ reversed ​zero​width'] })],
        ['tiktok', 'ttuser', BUILD.tiktok({ author: 'tiktoker1', segments: ['last one'] })],
      ];
      for (const [p, t, html] of plan) { fake.append(p, t, html); await sleep(450); }
      await waitFor(async () => (await page.eval(LINES)).length >= 9, 15000);
      await sleep(1500); // anything doubled would have arrived by now
      const lines = await page.eval(LINES);
      assert.strictEqual(lines.length, 9, JSON.stringify(lines.map(l => l.p + ':' + l.t)));
      assert.deepStrictEqual(lines.map(l => l.p), ['twitch', 'kick', 'youtube', 'tiktok', 'x', 'twitch', 'kick', 'twitch', 'tiktok'], 'platform order = order the messages were sent');
      assert.deepStrictEqual(lines.map(l => l.a), ['TwitchAnna', 'KickKai', 'YtYuki', 'tiktoker1', 'XavierX', 'Mallory', 'KickKai', 'RtlTroll', 'tiktoker1']);
      for (const l of lines) assert.strictEqual(l.mask, `url("/icons/${l.p}.svg")`, 'icon matches platform: ' + l.p);
      assert.strictEqual(lines[0].t.trim(), 'hello from twitch', 'the emote image is an <img>, its name is not text');
      assert.strictEqual(lines[0].imgs, 1);
      assert.strictEqual(lines[1].imgs, 1);
      assert.strictEqual(lines[2].t.trim(), 'nice stream');
      assert.strictEqual(lines[5].t, 'window.__pwned=1 javascript:alert(3)', 'markup removed by the hub, the rest is inert text');
      assert.strictEqual(lines[5].imgs, 0);
      assert.strictEqual(lines[6].imgs, 0, 'an emote on a non-allow-listed host is shown as text, not an image');
      assert.ok(lines[6].t.includes('Evil'));
      assert.strictEqual(lines[7].t, 'evil reversed zerowidth', 'bidi override and zero width characters removed');
      assert.strictEqual(await page.eval('typeof window.__pwned'), 'undefined');
      assert.strictEqual(await page.eval('document.querySelectorAll("#chat script, #chat iframe, #chat a, #chat [onerror]").length'), 0);
      assert.deepStrictEqual(page.consoleErrors, []);
      // the only https requests the overlay made are allow-listed emote hosts (and each was answered locally)
      for (const u of requestedOutside) assert.match(new URL(u).hostname, /^(static-cdn\.jtvnw\.net|files\.kick\.com|www\.youtube\.com)$/, u);
      assert.ok(!requestedOutside.some(u => /evil\.example/.test(u)));
      const st = await cppChat.status();
      assert.strictEqual(st.counts.accepted, 9);
      assert.ok(st.counts.duplicates >= 1, 'the repeated YouTube id was dropped by the hub');
      assert.strictEqual(st.counts.rate_limited, 0);
      await page.shot(path.join(PROOF_DIR, 'e2e-merged-chat-460x720.png'));
      // the blocklist applied on the live feed
      await cppChat.cmd('filter words=blockedword users=troll hide=0');
      fake.append('twitch', 'twchan', BUILD.twitch({ author: 'Someone', segments: ['this has BlockedWord in it'] }));
      fake.append('twitch', 'twchan', BUILD.twitch({ author: 'Troll', segments: ['harmless'] }));
      fake.append('twitch', 'twchan', BUILD.twitch({ author: 'Fine', segments: ['visible'] }));
      await waitFor(async () => (await page.eval(LINES)).some(l => l.t === 'visible'), 8000);
      const after = await page.eval(LINES);
      assert.ok(!after.some(l => /BlockedWord/i.test(l.t) || l.a === 'Troll'));
      // a second overlay opened later gets the recent backlog, in the same order
      const page2 = await overlayPage(cppChat.url('/chat.html?max=40'), 460, 720);
      await sleep(800);
      const replay = await page2.eval(LINES);
      assert.deepStrictEqual(replay.map(l => l.a + l.t), after.map(l => l.a + l.t));
      await page2.close();
    } finally {
      if (page) await page.close().catch(() => {});
      try { reader.stdin.end(); } catch (e) { /* gone */ }
      await new Promise(r => { const t = setTimeout(() => { reader.kill(); r(); }, 15000); reader.on('exit', () => { clearTimeout(t); r(); }); });
      assert.ok(!/token/i.test(rerr) || !rerr.includes(TOKEN), 'stderr never contains the token');
      await cppChat.stop(); fake.close(); fs.rmSync(cfg, { force: true });
    }
  });

  test('overlay themes and icons at true size (static page, local data)', async () => {
    const srv = await startCppChat(); // serves the real assets; no reader involved
    try {
      const samples = [
        { platform: 'twitch', author: 'TwitchAnna', color: '#ff4500', parts: [{ t: 'text', v: 'hello from twitch ' }, { t: 'emote', v: 'Kappa', u: EMOTES.TW_EMOTE }] },
        { platform: 'youtube', author: 'YtYuki', color: '', parts: [{ t: 'text', v: 'nice stream ' }, { t: 'emote', v: 'smile', u: EMOTES.YT_EMOJI }] },
        { platform: 'kick', author: 'KickKai', color: '#00ffa3', parts: [{ t: 'text', v: 'gg ' }, { t: 'emote', v: 'KEKW', u: EMOTES.KICK_EMOTE }] },
        { platform: 'tiktok', author: 'tiktoker1', color: '', parts: [{ t: 'text', v: 'a rather long message that has to wrap onto a second line without ever overflowing the width of the browser source' }] },
        { platform: 'x', author: 'XavierX', color: '', parts: [{ t: 'text', v: 'hello x' }] },
      ];
      const bgs = { dark: '#20242c', light: '#cfd8e3', transparent: '#2c7a4b' };
      for (const theme of ['dark', 'light', 'transparent']) {
        const page = await overlayPage(srv.url(`/chat.html?static=1&theme=${theme}&font=22`), 420, 520, theme === 'transparent');
        await page.eval(`document.documentElement.style.background = '${bgs[theme]}'; true`);
        await page.eval(`${JSON.stringify(samples)}.forEach(m => window.__omsc.add(m)); true`);
        await sleep(500);
        const info = await page.eval(`(() => { const out = {}; for (const m of document.querySelectorAll('.msg')) { const i = m.querySelector('.ico'); const cs = getComputedStyle(i); out[m.dataset.platform] = { w: i.getBoundingClientRect().width, bg: cs.backgroundColor, mask: cs.webkitMaskImage || cs.maskImage }; } return { out, sw: document.documentElement.scrollWidth, sh: document.documentElement.scrollHeight, bottom: Math.max(...[...document.querySelectorAll('.msg')].map(e => e.getBoundingClientRect().bottom)) }; })()`);
        assert.deepStrictEqual(Object.keys(info.out).sort(), ['kick', 'tiktok', 'twitch', 'x', 'youtube']);
        for (const [p, v] of Object.entries(info.out)) {
          assert.ok(v.mask.includes(`/icons/${p}.svg`), `${theme} ${p}: ${v.mask}`);
          // 0.5.0: a small logo next to the name, 0.8 x the font (otiyyy: "just a tiny logo next to the person"); was 1.1 x
          assert.ok(v.w >= 16 && v.w <= 19, `${theme} ${p} icon width ${v.w} (font 22: about 18)`);
          assert.notStrictEqual(v.bg, 'rgba(0, 0, 0, 0)', `${theme} ${p} icon has a colour`);
        }
        assert.ok(info.sw <= 420 && info.sh <= 520, `no overflow in ${theme}: ${JSON.stringify(info)}`);
        assert.ok(info.bottom <= 520);
        await page.shot(path.join(PROOF_DIR, `chat-${theme}-420x520.png`));
        await page.close();
      }
    } finally { await srv.stop(); }
  });

  test('overlay is XSS-proof when fed hostile messages directly (defence in depth behind the hub)', async () => {
    const srv = await startCppChat();
    try {
      const page = await overlayPage(srv.url('/chat.html?static=1&fade=0&max=5'), 420, 400);
      const hostile = [
        { platform: 'twitch', author: '<img src=x onerror=window.__x=1>', color: 'red;background:url(//evil)', parts: [{ t: 'text', v: '<script>window.__x=2</script>' }] },
        { platform: 'myspace', author: 'a', parts: [{ t: 'text', v: 'unknown platform ignored' }] },
        { platform: 'twitch/../../evil', author: 'a', parts: [{ t: 'text', v: 'path trick ignored' }] },
        { platform: 'kick', author: 'k', parts: [{ t: 'emote', v: 'bad', u: 'javascript:window.__x=3' }, { t: 'emote', v: 'bad2', u: 'http://evil.example/x.png' }, { t: 'text', v: 'x' }] },
        { platform: 'x', author: 'x', parts: 'not an array' },
        null, 42, 'str',
      ];
      await page.eval(`${JSON.stringify(hostile)}.forEach(m => window.__omsc.add(m)); true`);
      assert.strictEqual(await page.eval('typeof window.__x'), 'undefined');
      const lines = await page.eval(LINES);
      assert.strictEqual(lines.length, 2);
      assert.strictEqual(lines[0].a, '<img src=x onerror=window.__x=1>');
      assert.strictEqual(lines[0].t, '<script>window.__x=2</script>');
      assert.ok(!/red/.test(await page.eval('document.querySelector("#chat .name").style.color')), 'a colour that is not #rrggbb is never applied (a hash colour is used instead)');
      assert.strictEqual(await page.eval('document.querySelector("#chat .name").style.background'), '');
      assert.strictEqual(lines[1].imgs, 0);
      assert.strictEqual(await page.eval('document.querySelectorAll("#chat img, #chat script").length'), 0);
      await page.close();
    } finally { await srv.stop(); }
  });

  // 2026-10-24 regression: with more lines than fit, every line was squeezed (flex-shrink on an
  // overflow:hidden flex item) instead of the chat ticking upward. README size: 800 x 400.
  test('a full chat ticks: lines keep their height, the oldest drop out, the newest is fully visible', async () => {
    const srv = await startCppChat();
    try {
      for (const align of ['bottom', 'top']) {
        const page = await overlayPage(srv.url(`/chat.html?static=1&align=${align}`), 800, 400);
        const said = ['gg', 'a much longer message that has to wrap onto a second line in an 800 pixel wide browser source, so it is taller than the rest', 'lol', 'nice'];
        for (let i = 1; i <= 26; i++) await page.eval(`window.__omsc.add(${JSON.stringify({ platform: 'twitch', author: 'v' + i, parts: [{ t: 'text', v: '#' + i + ' ' + said[i % 4] }] })}); true`);
        await sleep(400); // the rise animation
        const m = await page.eval(`(() => { const box = document.getElementById('chat').getBoundingClientRect(); const ms = [...document.querySelectorAll('#chat .msg')];
          return { n: ms.length, boxTop: box.top, boxBottom: box.bottom,
            rows: ms.map(e => { const r = e.getBoundingClientRect(); return { h: r.height, nat: e.scrollHeight, top: r.top, bottom: r.bottom, t: e.querySelector('.text').textContent }; }) }; })()`);
        for (const r of m.rows) assert.ok(r.h >= r.nat - 1, `${align}: line "${r.t}" squeezed to ${r.h}px (needs ${r.nat}px)`);
        const minH = Math.min(...m.rows.map(r => r.h));
        const wrapped = m.rows.filter(r => / much longer /.test(r.t));
        assert.ok(wrapped.length >= 1 && wrapped.every(r => r.h >= minH * 1.6), `${align}: a wrapped line keeps both of its lines`);
        const last = m.rows[m.rows.length - 1];
        assert.match(last.t, /^#26 /, `${align}: newest line is last`);
        assert.ok(last.bottom <= m.boxBottom + 0.5, `${align}: newest line fully inside the source (${last.bottom} > ${m.boxBottom})`);
        assert.ok(m.rows[0].top >= m.boxTop - 0.5, `${align}: no line cut off at the top`);
        assert.ok(m.n < 26 && m.n >= 8, `${align}: oldest lines dropped, the rest kept (${m.n} lines)`);
        assert.deepStrictEqual(m.rows.map(r => +r.t.match(/^#(\d+)/)[1]), Array.from({ length: m.n }, (_, i) => 27 - m.n + i), `${align}: consecutive, newest last`);
        await page.shot(path.join(PROOF_DIR, `chat-full-${align}-800x400.png`));
        await page.close();
      }
      // one line taller than the whole source is kept (clipped by its 12em cap), never removed
      const page = await overlayPage(srv.url('/chat.html?static=1&font=40'), 300, 120);
      await page.eval(`window.__omsc.add({ platform: 'kick', author: 'k', parts: [{ t: 'text', v: 'word '.repeat(80) }] }); true`);
      assert.strictEqual(await page.eval('document.querySelectorAll("#chat .msg").length'), 1);
      await page.close();
    } finally { await srv.stop(); }
  });

  test('overlay options: max lines and fade work from the URL', async () => {
    const srv = await startCppChat();
    try {
      const page = await overlayPage(srv.url('/chat.html?static=1&max=3&fade=1'), 420, 300);
      await page.eval('for (let i = 1; i <= 6; i++) window.__omsc.add({ platform: "kick", author: "k" + i, parts: [{ t: "text", v: "line " + i }] }); true');
      const lines = await page.eval(LINES);
      assert.deepStrictEqual(lines.map(l => l.t), ['line 4', 'line 5', 'line 6']);
      await sleep(2500);
      assert.strictEqual((await page.eval(LINES)).length, 0, 'lines fade away after 1 s');
      await page.close();
    } finally { await srv.stop(); }
  });
}
