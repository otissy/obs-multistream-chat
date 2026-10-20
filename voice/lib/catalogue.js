'use strict';
// The voice catalogue (voice/catalogue.json): which voice packs exist, which voices each one holds,
// and how to drive sherpa-onnx for a voice. A pack is a k2-fsa release archive unpacked into
// <packs dir>/<pack id>/; a voice is usable when its pack's files are there.
const fs = require('fs');
const path = require('path');

const ENGINES = new Set(['kokoro', 'vits']);
const ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$/;
const LOCALE_RE = /^[a-z]{2,3}(-[A-Z]{2}|-[0-9]{3})?$/;

function fail(msg) { throw new Error('catalogue: ' + msg); }

// a file name inside a pack: relative, no "..", no drive or root
function safeRel(p, what) {
  if (typeof p !== 'string' || !p || p.length > 200) fail(what + ' must be a file name');
  const norm = p.replace(/\\/g, '/');
  if (norm.startsWith('/') || /^[A-Za-z]:/.test(norm) || norm.split('/').includes('..')) fail(what + ' must stay inside its pack: ' + p);
  return norm;
}

function validate(raw) {
  if (!raw || typeof raw !== 'object' || raw.schema !== 1) fail('schema must be 1');
  if (!Array.isArray(raw.packs) || !Array.isArray(raw.voices)) fail('packs and voices must be arrays');
  const packs = new Map();
  for (const p of raw.packs) {
    if (!p || !ID_RE.test(p.id || '')) fail('bad pack id ' + JSON.stringify(p && p.id));
    if (packs.has(p.id)) fail('duplicate pack ' + p.id);
    if (!ENGINES.has(p.engine)) fail(p.id + ': engine must be kokoro or vits');
    if (typeof p.licence !== 'string' || !p.licence) fail(p.id + ': licence is required');
    const f = p.files || {};
    safeRel(f.model, p.id + ' files.model');
    safeRel(f.tokens, p.id + ' files.tokens');
    safeRel(f.dataDir, p.id + ' files.dataDir');
    if (p.engine === 'kokoro') {
      safeRel(f.voices, p.id + ' files.voices');
      for (const [k, v] of Object.entries(f.lexicons || {})) String(v).split(',').forEach(x => safeRel(x, p.id + ' lexicon ' + k));
    }
    packs.set(p.id, p);
  }
  const voices = new Map();
  for (const v of raw.voices) {
    if (!v || !ID_RE.test(v.id || '')) fail('bad voice id ' + JSON.stringify(v && v.id));
    if (voices.has(v.id)) fail('duplicate voice ' + v.id);
    const p = packs.get(v.pack);
    if (!p) fail(v.id + ': unknown pack ' + v.pack);
    if (!Number.isInteger(v.sid) || v.sid < 0) fail(v.id + ': sid must be a non-negative integer');
    if (typeof v.name !== 'string' || !v.name) fail(v.id + ': name is required');
    if (!LOCALE_RE.test(v.locale || '')) fail(v.id + ': locale must look like en-GB');
    if (typeof v.language !== 'string' || !v.language) fail(v.id + ': language is required');
    if (p.engine === 'kokoro') {
      if (typeof v.lang !== 'string' || !v.lang) fail(v.id + ': kokoro voices need lang');
      if (v.lexicon && !(p.files.lexicons || {})[v.lexicon]) fail(v.id + ': lexicon ' + v.lexicon + ' is not in pack ' + p.id);
    }
    voices.set(v.id, v);
  }
  return { packs, voices };
}

function loadCatalogue(file) {
  return validate(JSON.parse(fs.readFileSync(file, 'utf8')));
}

function packFiles(pack, packsDir) {
  const dir = path.join(packsDir, pack.id);
  const f = pack.files;
  const need = [f.model, f.tokens, f.dataDir];
  if (pack.engine === 'kokoro') need.push(f.voices);
  return { dir, need: need.map(x => path.join(dir, x)) };
}

function packInstalled(pack, packsDir) {
  return packFiles(pack, packsDir).need.every(p => fs.existsSync(p));
}

function installedVoices(cat, packsDir) {
  const ok = new Map();
  for (const p of cat.packs.values()) ok.set(p.id, packInstalled(p, packsDir));
  return [...cat.voices.values()].filter(v => ok.get(v.pack));
}

// How to run one voice: `key` names the loaded model it needs (voices with the same key share it),
// `config` is the sherpa-onnx OfflineTts config, `gen` the per-line generation options.
function engineSpec(cat, voiceId, packsDir, numThreads = 2) {
  const v = cat.voices.get(voiceId);
  if (!v) throw new Error('unknown voice ' + voiceId);
  const p = cat.packs.get(v.pack);
  const dir = path.join(packsDir, p.id);
  const at = rel => path.join(dir, rel);
  const common = { numThreads, provider: 'cpu', debug: false };
  if (p.engine === 'kokoro') {
    const lexicon = v.lexicon ? p.files.lexicons[v.lexicon].split(',').map(at).join(',') : '';
    return {
      key: p.id + '|' + (v.lexicon || 'lang=' + v.lang),
      config: {
        model: {
          kokoro: { model: at(p.files.model), voices: at(p.files.voices), tokens: at(p.files.tokens), dataDir: at(p.files.dataDir), lexicon, lang: v.lexicon ? '' : v.lang },
          ...common,
        },
        maxNumSentences: 1,
      },
      gen: { sid: v.sid, extra: { lang: v.lang } },
    };
  }
  return {
    key: p.id,
    config: {
      model: { vits: { model: at(p.files.model), tokens: at(p.files.tokens), dataDir: at(p.files.dataDir), lexicon: '' }, ...common },
      maxNumSentences: 1,
    },
    gen: { sid: v.sid },
  };
}

module.exports = { validate, loadCatalogue, packInstalled, installedVoices, engineSpec };
