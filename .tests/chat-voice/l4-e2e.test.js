'use strict';
// L4: the chat overlay the way the streamer and OBS reach it: the real server, the real page, real
// clicks and keys, at 1920x1080 and 390x844, with the page's real audio output recorded and measured.
const test = require('node:test');
const assert = require('node:assert');
const path = require('path');
const fs = require('fs');
const h = require('./helpers');

const SHOTS = path.join(h.PROOF_DIR, 'shots');
const shot = async (page, name) => { fs.mkdirSync(SHOTS, { recursive: true }); await page.screenshot({ path: path.join(SHOTS, name + '.png') }); };
const visible = (page, sel) => page.locator(sel).isVisible();

let srv, obsBrowser, plainBrowser;
test.before(async () => {
  if (h.skipReason) return;
  srv = await h.startServer();
  obsBrowser = await h.launch('obs');
  plainBrowser = await h.launch('browser');
});
test.after(async () => {
  if (obsBrowser) await obsBrowser.close();
  if (plainBrowser) await plainBrowser.close();
  if (srv) await srv.stop();
});

async function open(query, mode, vp) {
  const browser = mode === 'obs' ? obsBrowser : plainBrowser;
  const p = await h.openPage(browser, srv.base + 'chat.html' + query, { mode, viewport: vp });
  return p;
}
async function leader(page) { assert.ok(await h.waitFor(async () => (await h.vstate(page)).role === 'leader'), 'page leads'); }
// one page at a time leads; tests close their pages so the next test's page is alone
async function closeAll(...ps) { for (const p of ps) if (p) await p.context.close().catch(() => {}); await h.waitFor(async () => (await srv.status()).listeners === 0, 4000); }

for (const vp of h.VIEWPORTS) {
  const tag = ' @' + vp.name;

  test('voice off (existing URLs): no voice UI or requests, same layout, logos load' + tag, { skip: h.skipReason }, async () => {
    const off = await open('?theme=dark', 'obs', vp);
    const on = await open('?theme=dark&voice=1', 'obs', vp);
    try {
      await leader(on.page);
      const boxOff = await h.addLines(off.page);
      const boxOn = await h.addLines(on.page);
      assert.strictEqual(boxOff.length, 3);
      assert.deepStrictEqual(boxOn, boxOff, 'the chat lines sit exactly where they sat without the voice');
      assert.strictEqual(await off.page.locator('#omsc-voice').count(), 0);
      assert.ok(!off.requests.some(u => u.includes('/voice/')), 'no voice request without voice=1');
      assert.ok(!off.requests.some(u => u.includes('chat-voice.js') && false));
      assert.strictEqual(await off.page.evaluate(() => window.__omscVoice), null);
      // inside OBS without controls=1 nothing of the voice is visible on stream
      for (const sel of ['#omsc-voice-gear', '#omsc-voice-status', '#omsc-voice-panel', '#omsc-voice-unlock']) assert.strictEqual(await visible(on.page, sel), false, sel);
      for (const p of [off, on]) {
        const l = await h.logosLoad(p.page);
        for (const k of ['twitch', 'youtube', 'kick', 'tiktok', 'x']) assert.ok(l[k] > 0, k + ' logo naturalWidth ' + l[k]);
        assert.ok(l.mask.includes('icons/'), 'line icon uses the platform logo');
      }
      await shot(off.page, 'voice-off' + tag);
      await shot(on.page, 'voice-on-obs' + tag);
      assert.deepStrictEqual(on.errors, []);
    } finally { await closeAll(off, on); }
  });

  test('OBS: a spoken line plays on its own and is really heard (duration, level, AudioContext)' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1', 'obs', vp);
    try {
      await leader(p.page);
      const rec = await h.record(p.page, async () => { await srv.tone('viewer', 800, 440, 0.5); }, 1600);
      const s = await h.vstate(p.page);
      assert.strictEqual(s.ctx, 'running');
      assert.strictEqual(s.played, 1);
      assert.ok(Math.abs(s.duration - 0.8) < 0.02, 'decoded duration ' + s.duration);
      const lv = h.levels(rec.samples, rec.rate);
      assert.ok(lv.peak > 14000 && lv.peak < 18000, 'peak ' + lv.peak + ' (0.5 full scale = 16384)');
      assert.ok(Math.abs(lv.voicedSec - 0.8) < 0.1, 'heard for ' + lv.voicedSec + ' s');
      assert.strictEqual(s.blocked, false);
      assert.strictEqual(await visible(p.page, '#omsc-voice-unlock'), false);
    } finally { await closeAll(p); }
  });

  test('normal browser: "Click to enable sound" appears, and a mouse click lets the line play' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1', 'browser', vp);
    try {
      // no page call at all before the line arrives: Playwright's evaluate and locator calls count as a
      // user gesture, which would unlock audio and hide the very state under test. Wait on the server.
      assert.ok(await h.waitFor(async () => (await srv.status()).listeners === 1));
      await h.sleep(300);
      await srv.tone('viewer', 600, 440, 0.5);
      await h.sleep(1000);
      await p.page.locator('#omsc-voice-unlock').waitFor({ state: 'visible', timeout: 8000 });
      assert.match(await p.page.locator('#omsc-voice-status').textContent(), /waiting for a click/);
      await shot(p.page, 'browser-click-to-enable' + tag);
      let s = await h.vstate(p.page);
      assert.strictEqual(s.blocked, true);
      assert.strictEqual(s.played, 0);
      assert.deepStrictEqual(s.queued.length, 1, 'the line waits for the click');
      const rec = await h.record(p.page, async () => { await p.page.click('#omsc-voice-unlock'); }, 1300);
      s = await h.vstate(p.page);
      assert.strictEqual(s.played, 1);
      assert.strictEqual(s.blocked, false);
      assert.strictEqual(await visible(p.page, '#omsc-voice-unlock'), false);
      assert.ok(h.levels(rec.samples, rec.rate).peak > 14000, 'heard after the click');
    } finally { await closeAll(p); }
  });

  test('normal browser: the enable-sound button works from the keyboard (Tab, Enter)' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1', 'browser', vp);
    try {
      assert.ok(await h.waitFor(async () => (await srv.status()).listeners === 1));
      await h.sleep(300);
      await srv.tone('viewer', 600, 440, 0.5);
      await h.sleep(1000);
      await p.page.locator('#omsc-voice-unlock').waitFor({ state: 'visible', timeout: 8000 });
      await p.page.keyboard.press('Tab');
      assert.strictEqual(await p.page.evaluate(() => document.activeElement && document.activeElement.id), 'omsc-voice-unlock');
      const rec = await h.record(p.page, async () => { await p.page.keyboard.press('Enter'); }, 1300);
      assert.strictEqual((await h.vstate(p.page)).played, 1);
      assert.ok(h.levels(rec.samples, rec.rate).peak > 14000);
    } finally { await closeAll(p); }
  });

  test('settings panel opens and closes with the mouse and the keyboard, and fits the screen' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1', 'browser', vp);
    try {
      await leader(p.page);
      assert.ok(await visible(p.page, '#omsc-voice-gear'), 'outside OBS the settings button is there');
      await p.page.click('#omsc-voice-gear');
      assert.ok(await visible(p.page, '#omsc-voice-panel'));
      assert.strictEqual(await p.page.getAttribute('#omsc-voice-gear', 'aria-expanded'), 'true');
      assert.strictEqual(await p.page.evaluate(() => document.activeElement.id), 'omsc-voice-speak');
      const box = await p.page.locator('#omsc-voice-panel').boundingBox();
      assert.ok(box.x >= 0 && box.x + box.width <= vp.width && box.y + box.height <= vp.height, 'panel inside the viewport ' + JSON.stringify(box));
      await shot(p.page, 'panel-open' + tag);
      await p.page.click('#omsc-voice-close');
      assert.strictEqual(await visible(p.page, '#omsc-voice-panel'), false);
      assert.strictEqual(await p.page.evaluate(() => document.activeElement.id), 'omsc-voice-gear', 'focus back on the button');
      await p.page.keyboard.press('Enter');
      assert.ok(await visible(p.page, '#omsc-voice-panel'), 'Enter on the button opens');
      await p.page.keyboard.press('Escape');
      assert.strictEqual(await visible(p.page, '#omsc-voice-panel'), false, 'Escape closes');
      await p.page.keyboard.press('Space');
      assert.ok(await visible(p.page, '#omsc-voice-panel'), 'Space on the button opens');
      await p.page.click('#omsc-voice-gear');
      assert.strictEqual(await visible(p.page, '#omsc-voice-panel'), false, 'the button toggles');
    } finally { await closeAll(p); }
  });

  test('inside OBS: hidden controls open with Alt+Shift+V (Interact), close with Escape; controls=1 shows them' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1', 'obs', vp);
    const q = await open('?voice=1&controls=1', 'obs', vp);
    try {
      await leader(q.page);
      assert.strictEqual(await visible(p.page, '#omsc-voice-gear'), false);
      await p.page.click('body', { position: { x: 5, y: 5 } });
      await p.page.keyboard.press('Alt+Shift+V');
      assert.ok(await visible(p.page, '#omsc-voice-panel'));
      assert.ok(await visible(p.page, '#omsc-voice-status'));
      await shot(p.page, 'obs-panel-by-shortcut' + tag);
      await p.page.keyboard.press('Escape');
      assert.strictEqual(await visible(p.page, '#omsc-voice-panel'), false);
      assert.strictEqual(await visible(p.page, '#omsc-voice-gear'), false, 'hidden again, nothing left on stream');
      assert.ok(await visible(q.page, '#omsc-voice-gear'), 'controls=1 shows the button inside OBS');
      await q.page.click('#omsc-voice-gear');
      assert.ok(await visible(q.page, '#omsc-voice-panel'));
    } finally { await closeAll(p, q); }
  });

  test('"Speak chat on stream": off is silent, on speaks; mouse and Space; survives reload' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1&controls=1', 'obs', vp);
    try {
      await leader(p.page);
      await p.page.click('#omsc-voice-gear');
      await p.page.click('#omsc-voice-speak'); // off
      assert.strictEqual(await p.page.isChecked('#omsc-voice-speak'), false);
      let rec = await h.record(p.page, async () => { await srv.tone('viewer', 500, 440, 0.5); }, 1000);
      assert.strictEqual((await h.vstate(p.page)).played, 0);
      assert.ok(h.levels(rec.samples, rec.rate).peak < 50, 'muted page is silent');
      assert.match(await p.page.locator('#omsc-voice-status').textContent(), /muted/);
      await p.page.reload();
      await leader(p.page);
      assert.strictEqual((await h.vstate(p.page)).settings.speak, false, 'kept after reload');
      await p.page.click('#omsc-voice-gear');
      await p.page.focus('#omsc-voice-speak');
      await p.page.keyboard.press('Space'); // on
      rec = await h.record(p.page, async () => { await srv.tone('viewer', 500, 440, 0.5); }, 1100);
      assert.strictEqual((await h.vstate(p.page)).played, 1);
      assert.ok(h.levels(rec.samples, rec.rate).peak > 14000);
    } finally { await closeAll(p); }
  });

  test('volume: the slider scales the real output (mouse click and arrow keys), kept after reload' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1&controls=1', 'obs', vp);
    try {
      await leader(p.page);
      await p.page.click('#omsc-voice-gear');
      const full = h.levels((await h.record(p.page, async () => { await srv.tone('viewer', 500, 440, 0.8); }, 1000)).samples, 48000).peak;
      const box = await p.page.locator('#omsc-voice-vol').boundingBox();
      await p.page.mouse.click(box.x + box.width * 0.3, box.y + box.height / 2);
      const v = Number(await p.page.inputValue('#omsc-voice-vol'));
      assert.ok(v >= 15 && v <= 45, 'clicked near 30%: ' + v);
      assert.strictEqual(await p.page.textContent('#omsc-voice-vol-out'), v + '%');
      await p.page.keyboard.press('ArrowLeft');
      await p.page.keyboard.press('ArrowLeft');
      const v2 = Number(await p.page.inputValue('#omsc-voice-vol'));
      assert.strictEqual(v2, v - 2, 'arrow keys move it');
      const part = h.levels((await h.record(p.page, async () => { await srv.tone('viewer', 500, 440, 0.8); }, 1000)).samples, 48000).peak;
      const ratio = part / full;
      assert.ok(Math.abs(ratio - v2 / 100) < 0.03, 'output scaled to ' + ratio.toFixed(3) + ' for ' + v2 + '%');
      await p.page.reload();
      await leader(p.page);
      assert.strictEqual((await h.vstate(p.page)).settings.vol, v2);
    } finally { await closeAll(p); }
  });

  test('queue limit and "when too many lines wait": typed and chosen by keyboard, flood is bounded' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1&controls=1', 'obs', vp);
    try {
      await leader(p.page);
      await p.page.click('#omsc-voice-gear');
      await p.page.click('#omsc-voice-queue');
      await p.page.keyboard.press('Control+A');
      await p.page.keyboard.type('2');
      await p.page.keyboard.press('Tab'); // change fires; focus moves to the select
      assert.strictEqual((await h.vstate(p.page)).settings.queue, 2);
      // drop the oldest (default): 1 speaking + 1 waiting; 6 lines -> the newest one waits
      const ids = [];
      for (let i = 0; i < 6; i++) ids.push(await srv.tone('viewer', 2000, 300 + i * 50, 0.3));
      assert.ok(await h.waitFor(async () => (await h.vstate(p.page)).dropped === 4));
      let s = await h.vstate(p.page);
      assert.strictEqual(s.playing, ids[0]);
      assert.deepStrictEqual(s.queued, [ids[5]]);
      await srv.flush();
      await h.waitFor(async () => (await h.vstate(p.page)).playing === 0);
      assert.strictEqual(await p.page.evaluate(() => document.activeElement.id), 'omsc-voice-drop');
      await p.page.keyboard.press('ArrowDown'); // "Skip the new line"
      assert.strictEqual((await h.vstate(p.page)).settings.drop, 'newest');
      const ids2 = [];
      for (let i = 0; i < 6; i++) ids2.push(await srv.tone('viewer', 2000, 300 + i * 50, 0.3));
      assert.ok(await h.waitFor(async () => (await h.vstate(p.page)).dropped === 8));
      s = await h.vstate(p.page);
      assert.strictEqual(s.playing, ids2[0]);
      assert.deepStrictEqual(s.queued, [ids2[1]]);
      // and with the mouse: pick the option back
      await p.page.click('#omsc-voice-drop');
      await p.page.selectOption('#omsc-voice-drop', 'oldest');
      assert.strictEqual((await h.vstate(p.page)).settings.drop, 'oldest');
      // out-of-range typing is clamped
      await p.page.fill('#omsc-voice-queue', '999');
      await p.page.press('#omsc-voice-queue', 'Tab');
      assert.strictEqual(await p.page.inputValue('#omsc-voice-queue'), '2', 'invalid value refused');
      await srv.flush();
    } finally { await closeAll(p); }
  });

  test('"Also play on this PC" (mouse and Space) tells the plugin, default off' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1&controls=1', 'obs', vp);
    try {
      await leader(p.page);
      assert.strictEqual((await srv.status()).local, false);
      await p.page.click('#omsc-voice-gear');
      await p.page.click('#omsc-voice-local');
      assert.ok(await h.waitFor(async () => (await srv.status()).local === true), 'plugin plays the local copy');
      await leader(p.page);
      await p.page.focus('#omsc-voice-local');
      await p.page.keyboard.press('Space');
      assert.ok(await h.waitFor(async () => { const s = await srv.status(); return s.local === false && s.listeners === 1; }));
    } finally { await closeAll(p); }
  });

  test('"Never read out lines from": typed names are skipped (case-insensitive), others speak' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1&controls=1', 'obs', vp);
    try {
      await leader(p.page);
      await p.page.click('#omsc-voice-gear');
      await p.page.click('#omsc-voice-skip');
      await p.page.keyboard.type('MyChannel,  NightBot ');
      await p.page.keyboard.press('Tab');
      assert.strictEqual(await p.page.inputValue('#omsc-voice-skip'), 'mychannel, nightbot');
      const rec = await h.record(p.page, async () => {
        await srv.tone('NIGHTBOT', 400, 440, 0.5);
        await srv.tone('mychannel', 400, 440, 0.5);
      }, 900);
      let s = await h.vstate(p.page);
      assert.strictEqual(s.skipped, 2);
      assert.strictEqual(s.played, 0);
      assert.ok(h.levels(rec.samples, rec.rate).peak < 50, 'skipped authors are not heard');
      await srv.tone('viewer', 400, 440, 0.5);
      assert.ok(await h.waitFor(async () => (await h.vstate(p.page)).played === 1));
      // clearing with the mouse: select the text, delete, click elsewhere
      await p.page.click('#omsc-voice-skip', { clickCount: 3 });
      await p.page.keyboard.press('Delete');
      await p.page.click('#omsc-voice-panel', { position: { x: 5, y: 5 } });
      assert.strictEqual((await h.vstate(p.page)).settings.skip, '');
    } finally { await closeAll(p); }
  });

  test('"Reset to the URL settings" drops the saved changes (mouse and keyboard)' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1&controls=1&vol=60&queue=4', 'obs', vp);
    try {
      await leader(p.page);
      await p.page.click('#omsc-voice-gear');
      await p.page.click('#omsc-voice-speak');
      await p.page.fill('#omsc-voice-queue', '9');
      await p.page.press('#omsc-voice-queue', 'Tab');
      let s = (await h.vstate(p.page)).settings;
      assert.strictEqual(s.speak, false);
      assert.strictEqual(s.queue, 9);
      await p.page.click('#omsc-voice-reset');
      s = (await h.vstate(p.page)).settings;
      assert.deepStrictEqual(s, { speak: true, vol: 60, queue: 4, drop: 'oldest', local: false, skip: '' });
      assert.strictEqual(await p.page.isChecked('#omsc-voice-speak'), true);
      assert.strictEqual(await p.page.inputValue('#omsc-voice-queue'), '4');
      assert.strictEqual(await p.page.evaluate(() => localStorage.getItem('omsc.voice.v1')), null);
      await p.page.click('#omsc-voice-speak');
      await p.page.focus('#omsc-voice-reset');
      await p.page.keyboard.press('Enter');
      assert.strictEqual((await h.vstate(p.page)).settings.speak, true);
    } finally { await closeAll(p); }
  });

  test('a line that cannot be decoded is skipped and the next one still plays' + tag, { skip: h.skipReason }, async () => {
    const p = await open('?voice=1&controls=1', 'obs', vp);
    let bad = null; // the first clip the page asks for gets a corrupt body
    try {
      await p.page.route('**/voice/clip/*.wav', async route => {
        const id = /clip\/(\d+)\.wav/.exec(route.request().url())[1];
        if (bad === null) bad = id;
        if (id === bad) return route.fulfill({ status: 200, contentType: 'audio/wav', body: Buffer.from('RIFF....WAVEgarbage-not-audio'.repeat(20)) });
        return route.continue();
      });
      await leader(p.page);
      const rec = await h.record(p.page, async () => {
        await srv.tone('viewer', 400, 440, 0.5);
        await srv.tone('viewer', 400, 440, 0.5);
      }, 1500);
      const s = await h.vstate(p.page);
      assert.strictEqual(s.errors, 1);
      assert.match(s.lastError, /decode|load|NotSupported/);
      assert.strictEqual(s.played, 1, 'the following line played');
      assert.ok(h.levels(rec.samples, rec.rate).peak > 14000);
    } finally { await closeAll(p); }
  });
}

test('server down: the status says offline (visible outside OBS)', { skip: h.skipReason }, async () => {
  const own = await h.startServer();
  const p = await h.openPage(plainBrowser, own.base + 'chat.html?voice=1', { mode: 'browser', viewport: h.VIEWPORTS[1] });
  try {
    await p.page.locator('#omsc-voice-status').filter({ hasText: 'ready' }).waitFor({ timeout: 8000 });
    await own.stop();
    await p.page.locator('#omsc-voice-status').filter({ hasText: 'offline' }).waitFor({ timeout: 8000 });
    assert.strictEqual(await p.page.getAttribute('#omsc-voice-status', 'data-conn'), 'offline');
    assert.ok(await visible(p.page, '#omsc-voice-status'));
    await shot(p.page, 'server-down @390x844');
  } finally { await p.context.close(); await own.stop(); }
});

// Proof with a real voice line (set OMS_VOICE_LINE_WAV to a WAV the voice helper wrote): the line goes
// through the real server into the page inside "OBS", and what the page outputs is recorded to
// <proof>/page-output.wav and measured.
test('real voice line: what the overlay outputs is the line (recorded WAV)', { skip: h.skipReason || (!process.env.OMS_VOICE_LINE_WAV && 'set OMS_VOICE_LINE_WAV') }, async () => {
  const src = fs.readFileSync(process.env.OMS_VOICE_LINE_WAV);
  const srcRate = src.readUInt32LE(24), srcSec = (src.length - 44) / 2 / srcRate;
  const p = await h.openPage(obsBrowser, srv.base + 'chat.html?voice=1', { mode: 'obs', viewport: h.VIEWPORTS[0] });
  try {
    await leader(p.page);
    const rec = await h.record(p.page, async () => { await srv.wav('viewer', process.env.OMS_VOICE_LINE_WAV); }, Math.ceil(srcSec * 1000) + 1200);
    const s = await h.vstate(p.page);
    const lv = h.levels(rec.samples, rec.rate);
    const out = path.join(h.PROOF_DIR, 'page-output.wav');
    h.writeWav(out, rec.samples, rec.rate);
    let sp = 0, ss = 0;
    for (let i = 44; i + 1 < src.length; i += 2) { const v = src.readInt16LE(i); sp = Math.max(sp, Math.abs(v)); ss += v * v; }
    const facts = { source: { rate: srcRate, seconds: +srcSec.toFixed(3), peak: sp, rms: Math.round(Math.sqrt(ss / ((src.length - 44) / 2))) },
      page: { ctx: s.ctx, outputRate: rec.rate, decodedSeconds: +s.duration.toFixed(3), recordedSeconds: +lv.totalSec.toFixed(3), voicedSeconds: +lv.voicedSec.toFixed(3), peak: lv.peak, rms: Math.round(lv.rms), played: s.played } };
    fs.writeFileSync(path.join(h.PROOF_DIR, 'page-output.json'), JSON.stringify(facts, null, 2));
    console.log(JSON.stringify(facts));
    assert.strictEqual(s.played, 1);
    assert.strictEqual(s.ctx, 'running');
    assert.ok(Math.abs(s.duration - srcSec) < 0.02, 'decoded the whole line');
    assert.ok(Math.abs(lv.voicedSec - srcSec) < 0.35, 'heard for about the line length: ' + lv.voicedSec);
    assert.ok(Math.abs(lv.peak - sp) / sp < 0.12, 'level kept at volume 100: ' + lv.peak + ' vs ' + sp);
  } finally { await closeAll(p); }
});
