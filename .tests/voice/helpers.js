'use strict';
// Shared plumbing for the voice helper tests: a throw-away packs folder with placeholder files, the
// stand-in sherpa module, and a way to run voice/index.js as the plugin does (stdin lines in, frames out).
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { Decoder, TYPE } = require('../../voice/lib/frames');

const ROOT = path.resolve(__dirname, '..', '..');
const VOICE = path.join(ROOT, 'voice');
const CATALOGUE = path.join(VOICE, 'catalogue.json');
const FAKE_SHERPA = path.join(__dirname, 'fake-sherpa');

// Lays out the named packs (all of them when ids is omitted) with empty placeholder files.
function makePacks(cat, ids) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-voice-'));
  for (const p of cat.packs.values()) {
    if (ids && !ids.includes(p.id)) continue;
    const at = rel => path.join(dir, p.id, rel);
    fs.mkdirSync(at(p.files.dataDir), { recursive: true });
    const files = [p.files.model, p.files.tokens];
    if (p.engine === 'kokoro') {
      files.push(p.files.voices);
      for (const l of Object.values(p.files.lexicons || {})) files.push(...l.split(','));
    }
    for (const f of files) fs.writeFileSync(at(f), '');
  }
  return dir;
}

const live = new Set();
// kills whatever a failed test left running, so the runner can exit
function killAll() {
  for (const c of live) c.kill();
  live.clear();
}

function runHelper(args, env = {}) {
  const child = spawn(process.execPath, [path.join(VOICE, 'index.js'), ...args], {
    env: { ...process.env, NODE_PATH: FAKE_SHERPA, ...env },
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const dec = new Decoder();
  const frames = [];
  const waiters = [];
  let stderr = '';
  child.stdout.on('data', d => {
    for (const f of dec.push(d)) frames.push(f);
    for (const w of waiters.splice(0)) w();
  });
  child.stderr.on('data', d => { stderr += d; });
  live.add(child);
  const exited = new Promise(r => child.on('exit', code => { live.delete(child); r(code); }));
  return {
    child, frames, exited,
    get stderr() { return stderr; },
    send(o) { child.stdin.write(JSON.stringify(o) + '\n'); },
    sendRaw(s) { child.stdin.write(s); },
    close() { child.stdin.end(); },
    // resolves once pred(frames) is true, or rejects after ms
    async until(pred, ms = 5000) {
      const end = Date.now() + ms;
      while (!pred(frames)) {
        if (Date.now() > end) throw new Error('timed out; frames: ' + JSON.stringify(frames.map(f => [f.type, f.id])));
        await new Promise(r => { waiters.push(r); setTimeout(r, 50); });
      }
    },
  };
}

const done = id => fs => fs.some(f => f.type === TYPE.DONE && f.id === id);

module.exports = { ROOT, VOICE, CATALOGUE, FAKE_SHERPA, makePacks, runHelper, killAll, done };
