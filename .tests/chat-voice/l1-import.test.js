'use strict';
// L1: the new files load alone and keep the overlay's rules (no innerHTML, own origin only, relative URLs).
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { ROOT } = require('./helpers');

const JS = path.join(ROOT, 'data', 'overlay', 'chat-voice.js');

test('chat-voice.js parses and does nothing at all without voice=1', () => {
  const src = fs.readFileSync(JS, 'utf8');
  new vm.Script(src, { filename: 'chat-voice.js' });
  const touched = [];
  const sandbox = {
    location: { search: '?theme=dark' },
    URLSearchParams,
    window: {},
    document: new Proxy({}, { get(t, k) { touched.push(String(k)); return undefined; } }),
  };
  vm.runInNewContext(src, sandbox);
  assert.strictEqual(sandbox.window.__omscVoice, null);
  assert.deepStrictEqual(touched, [], 'an existing overlay URL must not get any element, style or request');
});

test('chat-voice.js keeps the overlay rules: no innerHTML/eval, relative own-origin URLs only', () => {
  const src = fs.readFileSync(JS, 'utf8');
  for (const bad of ['innerHTML', 'outerHTML', 'insertAdjacentHTML', 'eval(', 'new Function', 'document.write', 'http://', 'https://', 'ws://', 'wss://', '/k/']) {
    assert.ok(!src.includes(bad), 'chat-voice.js must not contain ' + bad);
  }
  assert.ok(src.includes("'voice/events?"), 'voice stream is relative');
  assert.ok(src.includes("'voice/clip/'"), 'clips are relative');
  assert.ok(/localStorage\.(get|set|remove)Item[\s\S]*catch/.test(src), 'storage access is guarded');
});

test('chat.html loads chat-voice.js after chat.js and documents the voice options', () => {
  const html = fs.readFileSync(path.join(ROOT, 'data', 'overlay', 'chat.html'), 'utf8');
  const a = html.indexOf('<script src="chat.js"></script>');
  const b = html.indexOf('<script src="chat-voice.js"></script>');
  assert.ok(a > 0 && b > a);
  for (const opt of ['voice=1', 'vol=', 'queue=', 'drop=', 'local=', 'skip=', 'controls=1']) assert.ok(html.includes(opt), opt);
});

test('the plugin and the test fixture both serve chat-voice.js', () => {
  const main = fs.readFileSync(path.join(ROOT, 'src', 'obs', 'plugin-main.cpp'), 'utf8');
  assert.ok(main.includes('{"/chat-voice.js", "overlay/chat-voice.js", "text/javascript; charset=utf-8"}'));
  assert.ok(main.includes('set_voice_hub(&st.voice_hub)'));
  const fx = fs.readFileSync(path.join(ROOT, '.tests', 'core', 'cpp', 'chat_fixture.cpp'), 'utf8');
  assert.ok(fx.includes('"/chat-voice.js", "chat-voice.js"'));
});
