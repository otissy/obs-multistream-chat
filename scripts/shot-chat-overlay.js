'use strict';
// README picture of the on-stream chat: renders the real data/overlay/chat.html (static mode, sample lines
// through its own add()) at the Browser-source size the dock recommends, in headless Chrome/Chromium.
//   node scripts/shot-chat-overlay.js <out.png> [width] [height] [query]
const http = require('http');
const fs = require('fs');
const path = require('path');
const { Browser } = require('../.tests/overlay/helpers');

const out = process.argv[2];
const W = Number(process.argv[3] || 800), H = Number(process.argv[4] || 400);
const query = process.argv[5] || 'theme=dark&static=1';
const DIR = path.join(__dirname, '..', 'data', 'overlay');
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.svg': 'image/svg+xml' };

const LINES = [
  ['twitch', 'NightOwl', '#9b7bff', 'that save was insane'],
  ['youtube', 'Mara Visser', '', 'first time catching the stream live, hi everyone'],
  ['kick', 'KickFan', '#53fc18', 'W stream'],
  ['twitch', 'collab_mod', '#ff7aa2', 'gg, both chats in one place'],
  ['youtube', 'Jonas', '', 'what settings do you use?'],
  ['twitch', 'pixelpanda', '#4fc3f7', 'clip it!!'],
  ['kick', 'SampleUser', '', 'how long have you been playing?'],
];

(async () => {
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const f = path.join(DIR, u.pathname === '/' ? 'chat.html' : u.pathname.replace(/^\/+/, ''));
    if (!f.startsWith(DIR) || !fs.existsSync(f)) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] || 'application/octet-stream' });
    res.end(fs.readFileSync(f));
  });
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const b = await Browser.launch();
  try {
    const p = await b.page(W, H, true); // transparent, like an OBS Browser source
    await p.goto('http://127.0.0.1:' + server.address().port + '/chat.html?' + query);
    // a dark sample scene behind the transparent Browser source, so the picture reads on a white page too
    if (process.argv[6] === 'scene') await p.eval("document.documentElement.style.background = 'radial-gradient(circle at 30% 20%, #3a4a6b 0%, #1a2233 45%, #0d1018 100%)'; true");
    for (const [platform, author, color, text] of LINES) {
      await p.eval('window.__omsc.add(' + JSON.stringify({ platform, author, color, parts: [{ t: 'text', v: text }] }) + ')');
    }
    await new Promise(r => setTimeout(r, 700)); // the rise animation
    await p.shot(out);
    console.log('wrote ' + out);
  } finally {
    await b.close();
    server.close();
  }
})().catch(e => { console.error(e); process.exit(1); });
