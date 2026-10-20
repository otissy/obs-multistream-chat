#!/usr/bin/env node
'use strict';
// OBS Multistream Chat voice helper: speaks chat lines with neural voices (sherpa-onnx: Kokoro and
// Piper), entirely on this PC. The plugin starts it as a hidden child process; running the models
// outside OBS means a model error or a clashing onnxruntime.dll from another plugin cannot take OBS
// down.
//
//   node voice/index.js --catalogue <catalogue.json> --packs <voice packs dir> [--threads N]
//
// stdin: one JSON request per line
//   {"op":"say","id":7,"voice":"kokoro.bf_emma","text":"...","speed":1.0}
//   {"op":"flush"}     drop the waiting lines and cut the one being spoken
// stdout: frames (lib/frames.js). stderr: diagnostics, never chat text.
// Exits when stdin closes. Exit codes: 0 normal, 2 bad usage or catalogue, 3 sherpa-onnx missing.
const path = require('path');
const readline = require('readline');
const { TYPE, encode, pcm16 } = require('./lib/frames');
const { cleanText } = require('./lib/text');
const { loadCatalogue, installedVoices } = require('./lib/catalogue');
const { Engine } = require('./lib/engine');

const MAX_WAITING = 5;
const MAX_LINE = 64 * 1024;

function parseArgs(argv) {
  const o = { threads: 2 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--catalogue') o.catalogue = argv[++i];
    else if (a === '--packs') o.packs = argv[++i];
    else if (a === '--threads') o.threads = parseInt(argv[++i], 10);
    else throw new Error('unknown argument: ' + a);
  }
  if (!o.catalogue) o.catalogue = path.join(__dirname, 'catalogue.json');
  if (!o.packs) throw new Error('--packs <dir> is required');
  if (!Number.isInteger(o.threads) || o.threads < 1 || o.threads > 8) throw new Error('--threads must be 1..8');
  return o;
}

// The request loop. One line is spoken at a time, in order; at most MAX_WAITING wait behind it
// (the oldest is dropped, it is already stale chat). Resolves when input ends and the queue drains.
function serve({ input, output, engine, catalogue, log = () => {} }) {
  const send = (type, id, rate, payload) => output.write(encode(type, id, rate, payload));
  const waiting = [];
  let current = null; // { id, cancelled }
  let ended = false;
  let finish;
  const done = new Promise(r => { finish = r; });

  async function pump() {
    if (current) return;
    while (waiting.length) {
      const req = waiting.shift();
      current = { id: req.id, cancelled: false };
      const me = current;
      try {
        await engine.speak(req, {
          onAudio: (samples, rate) => { if (!me.cancelled) send(TYPE.AUDIO, me.id, rate, pcm16(samples)); },
          cancelled: () => me.cancelled,
        });
      } catch (e) {
        log('line ' + me.id + ' failed: ' + (e && e.message));
        send(TYPE.ERROR, me.id, 0, Buffer.from(String((e && e.message) || e).slice(0, 500), 'utf8'));
      }
      send(TYPE.DONE, me.id, 0);
      current = null;
    }
    if (ended) finish();
  }

  function handle(line) {
    if (line.length > MAX_LINE) return send(TYPE.ERROR, 0, 0, Buffer.from('request too long', 'utf8'));
    let req;
    try { req = JSON.parse(line); } catch (_) { return send(TYPE.ERROR, 0, 0, Buffer.from('request is not JSON', 'utf8')); }
    if (!req || typeof req !== 'object') return;
    if (req.op === 'flush') {
      for (const w of waiting.splice(0)) send(TYPE.DONE, w.id, 0);
      if (current) current.cancelled = true;
      return;
    }
    if (req.op !== 'say') return send(TYPE.ERROR, 0, 0, Buffer.from('unknown op', 'utf8'));
    const id = Number.isInteger(req.id) && req.id > 0 && req.id <= 0xffffffff ? req.id : 0;
    if (!id) return send(TYPE.ERROR, 0, 0, Buffer.from('say needs an id', 'utf8'));
    if (typeof req.voice !== 'string' || !catalogue.voices.has(req.voice)) {
      send(TYPE.ERROR, id, 0, Buffer.from('unknown voice', 'utf8'));
      return send(TYPE.DONE, id, 0);
    }
    const text = cleanText(req.text);
    if (!text) return send(TYPE.DONE, id, 0);
    waiting.push({ id, voice: req.voice, text, speed: Number(req.speed) || 1.0 });
    while (waiting.length > MAX_WAITING) send(TYPE.DONE, waiting.shift().id, 0);
    pump();
  }

  const rl = readline.createInterface({ input, crlfDelay: Infinity });
  rl.on('line', handle);
  rl.on('close', () => {
    ended = true;
    for (const w of waiting.splice(0)) send(TYPE.DONE, w.id, 0);
    if (current) current.cancelled = true;
    else finish();
  });
  return { done };
}

async function main() {
  let opts;
  let catalogue;
  try {
    opts = parseArgs(process.argv.slice(2));
    catalogue = loadCatalogue(opts.catalogue);
  } catch (e) {
    process.stderr.write('voice: ' + e.message + '\n');
    process.exit(2);
  }
  let sherpa;
  try {
    sherpa = require('sherpa-onnx-node');
  } catch (e) {
    process.stdout.write(encode(TYPE.ERROR, 0, 0, Buffer.from('sherpa-onnx is missing: ' + e.message.split('\n')[0], 'utf8')));
    process.exit(3);
  }
  const voices = installedVoices(catalogue, opts.packs);
  process.stdout.write(encode(TYPE.READY, 0, 0, Buffer.from(JSON.stringify({ voices: voices.length }), 'utf8')));
  const engine = new Engine({ sherpa, catalogue, packsDir: opts.packs, numThreads: opts.threads });
  const log = m => process.stderr.write('voice: ' + m + '\n');
  await serve({ input: process.stdin, output: process.stdout, engine, catalogue, log }).done;
  process.stdout.write('', () => process.exit(0)); // let the last frames reach the pipe
}

if (require.main === module) main();

module.exports = { parseArgs, serve, MAX_WAITING };
