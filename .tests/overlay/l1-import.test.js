'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { ROOT, OVERLAY_DIR } = require('./helpers');

const read = f => fs.readFileSync(path.join(OVERLAY_DIR, f), 'utf8');

test('overlay files exist', () => {
  for (const f of ['overlay.html', 'overlay.js', 'icons/generic.svg']) assert.ok(fs.existsSync(path.join(OVERLAY_DIR, f)), f);
});

test('overlay.js parses', () => { new Function(read('overlay.js')); });

test('page contains no inline script and loads only same-origin script', () => {
  const html = read('overlay.html');
  const scripts = [...html.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/gi)];
  assert.strictEqual(scripts.length, 1);
  assert.match(scripts[0][1], /src="\/overlay\.js"/);
  assert.strictEqual(scripts[0][2].trim(), '');
  assert.ok(!/\son\w+\s*=/i.test(html), 'inline event handler');
});

test('no external network reference anywhere in the overlay', () => {
  for (const f of ['overlay.html', 'overlay.js']) {
    const src = read(f).replace(/<!--[\s\S]*?-->/g, '').replace(/\/\*[\s\S]*?\*\//g, '');
    assert.ok(!/(https?:)?\/\/[a-z0-9.-]+\.[a-z]{2,}/i.test(src.replace(/xmlns="[^"]*"/g, '')), f + ' references a remote host');
    assert.ok(!/@import|<link\b[^>]*href=["']?http/i.test(src), f);
  }
});

test('overlay.js never uses dangerous sinks', () => {
  const src = read('overlay.js');
  for (const bad of ['innerHTML', 'outerHTML', 'insertAdjacentHTML', 'document.write', 'eval(', 'new Function', 'setTimeout("', 'XMLHttpRequest', 'WebSocket', 'localStorage'])
    assert.ok(!src.includes(bad), bad);
  // only same-origin endpoints
  const urls = [...src.matchAll(/(?:fetch|EventSource)\(\s*'([^']+)'/g)].map(m => m[1]);
  assert.deepStrictEqual(urls.sort(), ['/events', '/state.json']);
});

test('every platform preset has an icon file (or maps to generic)', () => {
  const dest = fs.readFileSync(path.join(ROOT, 'src', 'core', 'destination.cpp'), 'utf8');
  const icons = [...dest.matchAll(/\{"(\w+)", "[^"]*", "[^"]*", "(\w+)"/g)].map(m => m[2]);
  assert.ok(icons.length >= 6, 'parsed presets: ' + icons.join());
  for (const i of icons) assert.ok(fs.existsSync(path.join(OVERLAY_DIR, 'icons', i + '.svg')), i);
});

test('icons are plain SVG with no script and match NOTICE hashes', () => {
  const notice = fs.readFileSync(path.join(ROOT, 'NOTICE'), 'utf8');
  for (const f of fs.readdirSync(path.join(OVERLAY_DIR, 'icons'))) {
    const buf = fs.readFileSync(path.join(OVERLAY_DIR, 'icons', f));
    assert.ok(!/<script|onload|<foreignObject|href=/i.test(buf.toString()), f);
    if (f === 'generic.svg') continue;
    const h = crypto.createHash('sha256').update(buf).digest('hex');
    assert.ok(notice.includes(h), `NOTICE lacks hash of ${f}`);
  }
});

test('NOTICE records source and licence of the icons', () => {
  const n = fs.readFileSync(path.join(ROOT, 'NOTICE'), 'utf8');
  assert.match(n, /Simple Icons 16\.35\.0/);
  assert.match(n, /CC0 1\.0/);
  assert.ok(fs.existsSync(path.join(ROOT, 'packaging', 'simple-icons-LICENSE.md')));
});
