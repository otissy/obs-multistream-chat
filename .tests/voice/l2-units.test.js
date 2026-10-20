'use strict';
// L2: each piece alone, with hostile and malformed input: frames, chat-text cleaning, catalogue
// validation, and the engine (model cache, streaming, cancel) against the stand-in sherpa.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { CATALOGUE, FAKE_SHERPA, makePacks } = require('./helpers');
const { TYPE, HEADER, MAX_PAYLOAD, encode, pcm16, Decoder } = require('../../voice/lib/frames');
const { cleanText, MAX_CHARS } = require('../../voice/lib/text');
const { validate, loadCatalogue, installedVoices } = require('../../voice/lib/catalogue');
const { Engine } = require('../../voice/lib/engine');

const sherpa = require(path.join(FAKE_SHERPA, 'sherpa-onnx-node'));

// ---- frames -----------------------------------------------------------------------------------
test('frames: encode then decode gives the same frames, whatever the chunking', () => {
  const a = encode(TYPE.AUDIO, 7, 24000, pcm16(new Float32Array([0, 0.5, -0.5, 1])));
  const b = encode(TYPE.DONE, 7, 0);
  const c = encode(TYPE.ERROR, 0, 0, Buffer.from('bad', 'utf8'));
  const all = Buffer.concat([a, b, c]);
  for (const step of [1, 3, 19, 20, 21, all.length]) {
    const d = new Decoder();
    const got = [];
    for (let i = 0; i < all.length; i += step) got.push(...d.push(all.subarray(i, i + step)));
    assert.deepStrictEqual(got.map(f => [f.type, f.id, f.sampleRate, f.payload.length]),
      [[TYPE.AUDIO, 7, 24000, 8], [TYPE.DONE, 7, 0, 0], [TYPE.ERROR, 0, 0, 3]], 'step ' + step);
  }
  assert.strictEqual(a.length, HEADER + 8);
});

test('frames: PCM clips out-of-range samples and turns NaN into silence', () => {
  const p = pcm16(new Float32Array([2, -2, NaN, 0.25]));
  assert.deepStrictEqual([0, 2, 4, 6].map(o => p.readInt16LE(o)), [32767, -32767, 0, 8192]);
});

test('frames: a bad magic or an oversized length is refused', () => {
  assert.throws(() => new Decoder().push(Buffer.alloc(HEADER)), /magic/);
  const h = encode(TYPE.DONE, 1, 0);
  h.writeUInt32LE(MAX_PAYLOAD + 1, 16);
  assert.throws(() => new Decoder().push(h), /too large/);
  assert.throws(() => encode(TYPE.AUDIO, 1, 1, Buffer.alloc(MAX_PAYLOAD + 1)), /too large/);
});

// ---- chat text --------------------------------------------------------------------------------
test('text: links, emoji, spam runs and control characters do not reach the voice', () => {
  assert.strictEqual(cleanText('check https://evil.example/x?y=1 now'), 'check a link now');
  assert.strictEqual(cleanText('www.example.com'), 'a link');
  assert.strictEqual(cleanText('gg 🔥🔥🔥 👍🏽 🇳🇱'), 'gg');
  assert.strictEqual(cleanText('noooooooooo!!!!!!!!'), 'nooo!!!');
  assert.strictEqual(cleanText('lolololololololol'), 'lololol');
  assert.strictEqual(cleanText('a\u0000b\u202Ec\td'), 'a b c d');
  assert.strictEqual(cleanText('🔥🔥🔥'), '');
  assert.strictEqual(cleanText('...!!! ???'), '');
  for (const x of [null, undefined, 42, {}, []]) assert.strictEqual(cleanText(x), '');
});

test('text: long lines are cut near a word boundary and stay under the limit', () => {
  const long = ('word ').repeat(200);
  const out = cleanText(long);
  assert.ok([...out].length <= MAX_CHARS);
  assert.ok(out.endsWith('word'));
  assert.ok([...cleanText('x'.repeat(5000).replace(/x/g, 'ab'))].length <= MAX_CHARS);
});

test('text: other scripts survive (Cyrillic, CJK, Arabic, Devanagari)', () => {
  for (const s of ['Привет всем', '大家好', 'مرحبا بالجميع', 'सबको नमस्ते', 'こんにちは']) assert.strictEqual(cleanText(s), s);
});

// ---- catalogue --------------------------------------------------------------------------------
const base = () => JSON.parse(fs.readFileSync(CATALOGUE, 'utf8'));

test('catalogue: paths that leave the pack folder are refused', () => {
  for (const bad of ['../x.onnx', '/etc/passwd', 'C:\\x.onnx', 'a/../../b', '']) {
    const c = base();
    c.packs[1].files.model = bad;
    assert.throws(() => validate(c), /catalogue/, bad);
  }
});

test('catalogue: unknown packs, duplicate ids, bad sids and bad engines are refused', () => {
  const muts = [
    c => { c.voices[0].pack = 'nope'; },
    c => { c.voices.push(c.voices[0]); },
    c => { c.packs.push(c.packs[0]); },
    c => { c.voices[0].sid = -1; },
    c => { c.voices[0].sid = 1.5; },
    c => { c.packs[0].engine = 'sapi'; },
    c => { c.packs[0].licence = ''; },
    c => { c.voices[0].locale = 'english'; },
    c => { c.voices[0].lexicon = 'xx'; },
    c => { c.schema = 2; },
  ];
  for (const m of muts) { const c = base(); m(c); assert.throws(() => validate(c), /catalogue/); }
});

test('catalogue: only voices whose pack files are all present count as installed', () => {
  const cat = loadCatalogue(CATALOGUE);
  const dir = makePacks(cat, ['vits-piper-en_GB-alba-medium', 'vits-piper-de_DE-thorsten-high']);
  fs.rmSync(path.join(dir, 'vits-piper-de_DE-thorsten-high', 'tokens.txt'));
  assert.deepStrictEqual(installedVoices(cat, dir).map(v => v.id), ['piper.en_GB-alba-medium']);
  assert.deepStrictEqual(installedVoices(cat, path.join(os.tmpdir(), 'oms-voice-none')), []);
});

// ---- engine -----------------------------------------------------------------------------------
function engineWithLog(ids, opts = {}) {
  const cat = loadCatalogue(CATALOGUE);
  const dir = makePacks(cat, ids);
  const log = path.join(dir, 'fake.log');
  process.env.OMS_FAKE_SHERPA_LOG = log;
  const e = new Engine({ sherpa, catalogue: cat, packsDir: dir, ...opts });
  const read = () => (fs.existsSync(log) ? fs.readFileSync(log, 'utf8').trim().split('\n').map(JSON.parse) : []);
  return { e, read };
}

test('engine: streams one chunk per sentence, with the voice\'s speaker id and language', async () => {
  const { e, read } = engineWithLog(['kokoro-multi-lang-v1_0']);
  const chunks = [];
  const n = await e.speak({ voice: 'kokoro.bf_emma', text: 'One. Two! Three?', speed: 1 },
    { onAudio: (s, r) => chunks.push([s.length, r]), cancelled: () => false });
  assert.strictEqual(chunks.length, 3);
  assert.ok(chunks.every(c => c[1] === 24000));
  assert.strictEqual(n, chunks.reduce((a, c) => a + c[0], 0));
  const line = read().find(x => x.text);
  assert.strictEqual(line.sid, 21);
  assert.strictEqual(line.lang, 'en-GB-x-rp'); // espeak-ng has no 'en-gb' voice name
  assert.ok(read()[0].lexicon.endsWith('lexicon-gb-en.txt,' + path.join(path.dirname(read()[0].load), 'lexicon-zh.txt')));
});

test('engine: voices on one model load it once; a third model evicts the oldest', async () => {
  const { e, read } = engineWithLog(['kokoro-multi-lang-v1_0', 'vits-piper-en_GB-alba-medium', 'vits-piper-de_DE-thorsten-high'], { maxModels: 2 });
  const say = v => e.speak({ voice: v, text: 'Hi.' }, { onAudio() {}, cancelled: () => false });
  await say('kokoro.af_heart');
  await say('kokoro.am_michael');
  await say('kokoro.zf_xiaoxiao'); // Chinese runs on the same en-us + zh lexicon model
  assert.strictEqual(read().filter(x => x.load).length, 1);
  await say('piper.en_GB-alba-medium');
  await say('piper.de_DE-thorsten-high'); // evicts kokoro
  await say('kokoro.af_heart');
  assert.strictEqual(read().filter(x => x.load).length, 4);
});

test('engine: speed is clamped and cancel stops between chunks', async () => {
  const { e, read } = engineWithLog(['vits-piper-en_GB-alba-medium']);
  let got = 0;
  await e.speak({ voice: 'piper.en_GB-alba-medium', text: 'A. B. C. D. E.', speed: 9 },
    { onAudio: () => { got++; }, cancelled: () => got >= 2 });
  assert.strictEqual(got, 2);
  assert.strictEqual(read().find(x => x.text).speed, 2);
  await e.speak({ voice: 'piper.en_GB-alba-medium', text: 'A.', speed: 'fast' }, { onAudio() {}, cancelled: () => false });
  assert.strictEqual(read().filter(x => x.text)[1].speed, 1);
});

test('engine: a missing pack fails the line clearly, every time', async () => {
  const { e } = engineWithLog([]);
  const say = () => e.speak({ voice: 'piper.en_GB-alba-medium', text: 'Hi.' }, { onAudio() {}, cancelled: () => false });
  await assert.rejects(say, /voice pack not installed: vits-piper-en_GB-alba-medium/);
  await assert.rejects(say, /voice pack not installed/);
  await assert.rejects(() => e.speak({ voice: 'nope', text: 'x' }, { onAudio() {}, cancelled: () => false }), /unknown voice/);
});

// ---- licence gate -----------------------------------------------------------------------------
const { classify } = require('../../voice/lib/licence');

test('licence: permissive licences named in the card are accepted', () => {
  const ok = {
    '* License: CC0': 'CC0-1.0 / public domain',
    'Dataset: public domain (LibriVox)': 'CC0-1.0 / public domain',
    '* License: Attribution 4.0 International (CC BY 4.0)': 'CC-BY',
    'Creative Commons Attribution-ShareAlike 4.0': 'CC-BY-SA',
    'License: CC-BY-SA-4.0': 'CC-BY-SA',
    'Licensed under the Apache License, Version 2.0': 'Apache-2.0',
    'License: MIT': 'MIT',
    'Open Data Commons Attribution License (ODC-By)': 'CC-BY',
    '* License: https://creativecommons.org/licenses/by/4.0/': 'CC-BY',
    '* License: https://creativecommons.org/licenses/by-sa/4.0/deed.en': 'CC-BY-SA',
    'License: https://creativecommons.org/publicdomain/zero/1.0/': 'CC0-1.0 / public domain',
    'https://www.apache.org/licenses/LICENSE-2.0': 'Apache-2.0',
    '* License: http://unlicense.org': 'Unlicense',
  };
  for (const [t, name] of Object.entries(ok)) {
    const r = classify(t);
    assert.ok(r.ok, t);
    assert.strictEqual(r.licence, name, t);
  }
});

test('licence: non-commercial, no-derivatives, research-only, bare links and silence are refused', () => {
  for (const t of [
    'Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0)',
    'License: CC BY-NC 4.0',
    'CC BY-ND 4.0',
    '* License: https://creativecommons.org/licenses/by-nc-sa/4.0/',
    '* License: https://creativecommons.org/licenses/by-nd/4.0/',
    'Attribution-NoDerivatives 4.0',
    'This dataset may be used for research purposes only.',
    '* License: https://www.cstr.ed.ac.uk/projects/blizzard/2011/lessac_blizzard2011/license.html',
    'Trained at MIT. Speakers: 1',
    '* License: See URL',
    '* License: AGPLv3 See URL',
    '',
    '   ',
  ]) assert.strictEqual(classify(t).ok, false, JSON.stringify(t));
});

test('licence: a voice fine-tuned from Lessac is flagged even when its own data is permissive', () => {
  const r = classify('* License: CC BY 4.0\n\n## Training\n\nFine-tuned from English lessac medium');
  assert.match(classify('* License: https://creativecommons.org/licenses/by/4.0/\n\n## Training\n\nFinetuned from U.S. English lessac voice (medium quality).').warn, /Lessac/);
  assert.ok(r.ok);
  assert.match(r.warn, /Lessac/);
  assert.strictEqual(classify('* License: CC BY 4.0\n\nTrained from scratch').warn, '');
});
