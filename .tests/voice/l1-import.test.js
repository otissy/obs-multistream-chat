'use strict';
// L1: the voice helper's modules load, the shipped catalogue is valid, and every voice in it
// resolves to a sherpa-onnx config whose paths stay inside its own pack.
const test = require('node:test');
const assert = require('node:assert');
const path = require('path');
const { CATALOGUE } = require('./helpers');
const { loadCatalogue, engineSpec } = require('../../voice/lib/catalogue');

test('modules load without sherpa-onnx installed', () => {
  for (const m of ['frames', 'text', 'catalogue', 'engine']) assert.ok(require('../../voice/lib/' + m));
  assert.strictEqual(typeof require('../../voice/index').serve, 'function');
});

test('the shipped catalogue is valid and covers the major languages', () => {
  const cat = loadCatalogue(CATALOGUE);
  const langs = new Set([...cat.voices.values()].map(v => v.language));
  for (const l of ['English', 'Spanish', 'French', 'German', 'Portuguese', 'Italian', 'Dutch', 'Russian', 'Polish',
    'Turkish', 'Arabic', 'Hindi', 'Chinese', 'Japanese', 'Ukrainian', 'Swedish', 'Vietnamese', 'Indonesian']) {
    assert.ok(langs.has(l), 'no voice for ' + l);
  }
  const accents = new Set([...cat.voices.values()].filter(v => v.language === 'English').map(v => v.accent));
  for (const a of ['American', 'British', 'Scottish', 'Northern English', 'Southern English']) assert.ok(accents.has(a), 'no ' + a + ' English');
});

test('every voice resolves inside its pack, and voices of one model share a load key', () => {
  const cat = loadCatalogue(CATALOGUE);
  const packs = path.resolve('/packs');
  const keys = new Map();
  for (const v of cat.voices.values()) {
    const s = engineSpec(cat, v.id, packs);
    const m = s.config.model.kokoro || s.config.model.vits;
    for (const p of [m.model, m.tokens, m.dataDir, ...(m.lexicon ? m.lexicon.split(',') : [])]) {
      assert.ok(p.startsWith(path.join(packs, v.pack) + path.sep), v.id + ': ' + p);
    }
    assert.strictEqual(s.gen.sid, v.sid);
    keys.set(s.key, (keys.get(s.key) || 0) + 1);
  }
  // all American and Chinese Kokoro voices run on one loaded model
  assert.ok(keys.get('kokoro-multi-lang-v1_0|en-us') >= 28);
});
