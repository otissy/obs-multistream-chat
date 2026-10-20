'use strict';
// L3: voice/index.js as a process, driven the way the plugin drives it: JSON lines on stdin,
// frames on stdout, exit when stdin closes. sherpa-onnx is the stand-in (NODE_PATH).
const test = require('node:test');
const assert = require('node:assert');
const { CATALOGUE, makePacks, runHelper, killAll, done } = require('./helpers');
const { TYPE } = require('../../voice/lib/frames');
const { loadCatalogue } = require('../../voice/lib/catalogue');

const cat = loadCatalogue(CATALOGUE);
const PACKS = makePacks(cat, ['kokoro-multi-lang-v1_0', 'vits-piper-en_GB-alba-medium']);
test.afterEach(killAll);

test('helper: READY counts the installed voices, lines come back in order, stdin close exits 0', async () => {
  const h = runHelper(['--packs', PACKS]);
  await h.until(fs => fs.some(f => f.type === TYPE.READY));
  const ready = JSON.parse(h.frames.find(f => f.type === TYPE.READY).payload);
  assert.strictEqual(ready.voices, 55); // 54 Kokoro + Alba
  h.send({ op: 'say', id: 1, voice: 'kokoro.af_heart', text: 'Hello there. How are you?' });
  h.send({ op: 'say', id: 2, voice: 'piper.en_GB-alba-medium', text: 'Hi from Scotland.' });
  await h.until(done(2));
  const audio = h.frames.filter(f => f.type === TYPE.AUDIO);
  assert.deepStrictEqual(audio.map(f => f.id), [1, 1, 2]);
  assert.deepStrictEqual(audio.map(f => f.sampleRate), [24000, 24000, 22050]);
  const order = h.frames.filter(f => f.type !== TYPE.READY).map(f => [f.type, f.id]);
  assert.deepStrictEqual(order.at(-1), [TYPE.DONE, 2]);
  assert.ok(order.findIndex(x => x[0] === TYPE.DONE && x[1] === 1) < order.findIndex(x => x[1] === 2));
  h.close();
  assert.strictEqual(await h.exited, 0);
});

test('helper: flush cuts the line being spoken and drops the waiting ones', async () => {
  const h = runHelper(['--packs', PACKS], { OMS_FAKE_SHERPA_DELAY_MS: '60' });
  h.send({ op: 'say', id: 1, voice: 'kokoro.af_heart', text: 'A. B. C. D. E. F. G. H.' });
  h.send({ op: 'say', id: 2, voice: 'kokoro.af_heart', text: 'Waiting.' });
  await h.until(fs => fs.some(f => f.type === TYPE.AUDIO && f.id === 1));
  h.send({ op: 'flush' });
  h.send({ op: 'say', id: 3, voice: 'kokoro.af_heart', text: 'After the flush.' });
  await h.until(done(3));
  assert.ok(h.frames.filter(f => f.type === TYPE.AUDIO && f.id === 1).length < 8);
  assert.strictEqual(h.frames.filter(f => f.type === TYPE.AUDIO && f.id === 2).length, 0);
  assert.ok(h.frames.some(f => f.type === TYPE.DONE && f.id === 2));
  h.close();
  assert.strictEqual(await h.exited, 0);
});

test('helper: when chat outruns the voice only the newest lines are kept', async () => {
  const h = runHelper(['--packs', PACKS], { OMS_FAKE_SHERPA_DELAY_MS: '40' });
  for (let i = 1; i <= 12; i++) h.send({ op: 'say', id: i, voice: 'kokoro.af_heart', text: 'Line ' + i + '.' });
  await h.until(done(12));
  const spoken = [...new Set(h.frames.filter(f => f.type === TYPE.AUDIO).map(f => f.id))];
  assert.ok(spoken.length <= 7, 'spoke ' + spoken.join(','));
  assert.deepStrictEqual(spoken.slice(-5), [8, 9, 10, 11, 12]);
  for (let i = 1; i <= 12; i++) assert.ok(h.frames.some(f => f.type === TYPE.DONE && f.id === i), 'no DONE for ' + i);
  h.close();
  await h.exited;
});

test('helper: junk requests get an ERROR frame, never a crash, and never echo the text', async () => {
  const h = runHelper(['--packs', PACKS]);
  h.sendRaw('not json\n');
  h.sendRaw('x'.repeat(70000) + '\n');
  h.send({ op: 'dance' });
  h.send({ op: 'say', voice: 'kokoro.af_heart', text: 'no id' });
  h.send({ op: 'say', id: 5, voice: 'no.such.voice', text: 'secret chat text' });
  h.send({ op: 'say', id: 6, voice: 'piper.de_DE-thorsten-high', text: 'zebra crossing' });
  h.send({ op: 'say', id: 7, voice: 'kokoro.af_heart', text: '🔥🔥🔥' });
  h.send({ op: 'say', id: 8, voice: 'kokoro.af_heart', text: 'still alive.' });
  await h.until(done(8));
  const errors = h.frames.filter(f => f.type === TYPE.ERROR);
  assert.ok(errors.length >= 6);
  assert.ok(errors.every(f => !f.payload.toString().includes('secret') && !f.payload.toString().includes('zebra')));
  assert.ok(h.frames.some(f => f.type === TYPE.DONE && f.id === 7));
  assert.ok(!h.frames.some(f => f.type === TYPE.AUDIO && f.id === 7));
  assert.ok(h.frames.some(f => f.type === TYPE.AUDIO && f.id === 8));
  h.close();
  assert.strictEqual(await h.exited, 0);
});

test('helper: bad usage exits 2, a missing sherpa-onnx exits 3 with an ERROR frame', async () => {
  const a = runHelper([]);
  assert.strictEqual(await a.exited, 2);
  const b = runHelper(['--packs', PACKS], { NODE_PATH: '' });
  assert.strictEqual(await b.exited, 3);
  assert.ok(b.frames.some(f => f.type === TYPE.ERROR && /sherpa-onnx is missing/.test(f.payload.toString())));
});
