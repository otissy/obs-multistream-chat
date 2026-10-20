#!/usr/bin/env node
'use strict';
// Downloads the voice packs that go into the installer, and checks each one twice before it ships:
//   1. the archive's SHA-256 must match voice/pins.json (a pack without a pin is refused, unless
//      --record-pins is given, which records it: do that once, review, commit the pins)
//   2. the pack's own MODEL_CARD / README / LICENSE must name a licence that allows shipping
//      (voice/lib/licence.js); a pack whose catalogue licence is "unverified" and has no such text
//      is refused
// Refused packs are listed and left out; the voices in them simply do not appear in the plugin.
//
//   node scripts/fetch-voices.js --out <dir> [--packs id,id | --all] [--record-pins]
//        [--keep-embedded] [--allow-lessac-derived] [--release <base url>]
// Voices fine-tuned from Lessac ship when the catalogue's policy.allowLessacDerived says so (it does: the
// maintainer's decision is recorded there) or --allow-lessac-derived is given. policy.shipAll ships every
// pack whatever its licence verdict (the verdict is still recorded in manifest.json and LICENCES.md);
// packs in policy.exclude never ship. The SHA-256 pin is enforced either way.
//
// Writes <out>/<pack id>/ (unpacked), <out>/manifest.json and <out>/LICENCES.md (for NOTICE).
// --keep-embedded: every pack is still checked, but only the ones the setup embeds stay unpacked
// (the others are downloaded by Setup itself); saves ~6 GB of disk on the build machine.
// Exit codes: 0 every requested pack shipped, 1 some were refused, 2 bad usage.
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');
const { loadCatalogue, packInstalled } = require('../voice/lib/catalogue');
const { classify } = require('../voice/lib/licence');

const ROOT = path.resolve(__dirname, '..');
const CATALOGUE = path.join(ROOT, 'voice', 'catalogue.json');
const PINS = path.join(ROOT, 'voice', 'pins.json');
const LICENCE_FILES = /^(MODEL_CARD|README(\.md|\.txt)?|LICEN[CS]E(\.md|\.txt)?)$/i;

function parseArgs(argv) {
  const o = { packs: null, recordPins: false, allowLessac: false, shipAll: false, exclude: [], keep: null, catalogue: CATALOGUE, pins: PINS };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--out') o.out = argv[++i];
    else if (a === '--packs') o.packs = argv[++i].split(',').filter(Boolean);
    else if (a === '--all') o.packs = null;
    else if (a === '--record-pins') o.recordPins = true;
    else if (a === '--allow-lessac-derived') o.allowLessac = true;
    else if (a === '--keep-embedded') o.keep = require('./voice-installer').DEFAULT_EMBED;
    else if (a === '--release') o.release = argv[++i];
    else if (a === '--catalogue') o.catalogue = argv[++i];
    else if (a === '--pins') o.pins = argv[++i];
    else throw new Error('unknown argument: ' + a);
  }
  if (!o.out) throw new Error('--out <dir> is required');
  if (o.release && !/^https:\/\/|^http:\/\/127\.0\.0\.1:\d+\//.test(o.release)) throw new Error('--release must be https (or http://127.0.0.1 in tests)');
  return o;
}

const DOWNLOAD_MS = 20 * 60 * 1000; // per pack: a stalled transfer fails that pack instead of the whole job
const UNPACK_MS = 20 * 60 * 1000;

async function download(url, file) {
  const res = await fetch(url, { redirect: 'follow', signal: AbortSignal.timeout(DOWNLOAD_MS) });
  if (!res.ok) throw new Error('HTTP ' + res.status + ' for ' + url);
  const hash = crypto.createHash('sha256');
  const out = fs.createWriteStream(file);
  let bytes = 0;
  for await (const chunk of res.body) {
    hash.update(chunk);
    bytes += chunk.length;
    if (!out.write(chunk)) await new Promise(r => out.once('drain', r));
  }
  await new Promise((r, j) => out.end(e => (e ? j(e) : r())));
  return { sha256: hash.digest('hex'), bytes };
}

// Windows' own tar.exe has no bzip2 inside: it runs "bzip2 -d" from PATH and fails on a stock PC.
// Git for Windows (here and on the GitHub runners) has one in usr\bin; it is appended, not prepended,
// so "tar" still resolves to the system tar.exe and not Git's.
function tarEnv(env = process.env) {
  if (process.platform !== 'win32') return env;
  const key = Object.keys(env).find(k => k.toUpperCase() === 'PATH') || 'PATH';
  const dirs = String(env[key] || '').split(';').filter(Boolean);
  const has = dirs.some(d => fs.existsSync(path.join(d, 'bzip2.exe')));
  const git = path.join(env.ProgramFiles || 'C:\\Program Files', 'Git', 'usr', 'bin');
  if (has || !fs.existsSync(path.join(git, 'bzip2.exe'))) return env;
  return { ...env, [key]: [...dirs, git].join(';') };
}

// "bzip2 -dc archive | tar -xf - -C dest", with both pipes driven from here. Not "tar -xjf": Windows'
// tar.exe runs bzip2 as a filter of its own and that pipe deadlocks once a real pack (67-350 MB) fills it,
// which hung CI for hours; small test archives never showed it. Resolves to "" or an error message.
function unpack(archive, dest, env = tarEnv()) {
  return new Promise(resolve => {
    let done = false;
    let err = '';
    const finish = msg => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      resolve(msg);
    };
    const bz = spawn('bzip2', ['-dc', archive], { env, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
    const tar = spawn('tar', ['-xf', '-', '-C', dest], { env, stdio: ['pipe', 'ignore', 'pipe'], windowsHide: true });
    const timer = setTimeout(() => { bz.kill(); tar.kill(); finish('timed out after ' + UNPACK_MS / 60000 + ' min'); }, UNPACK_MS);
    for (const p of [bz, tar]) {
      p.stderr.on('data', d => { if (err.length < 400) err += d; });
      p.on('error', e => { bz.kill(); tar.kill(); finish(e.code === 'ENOENT' ? e.path + ' was not found on PATH' : e.message); });
    }
    tar.stdin.on('error', () => {}); // tar quitting early surfaces as its exit code
    bz.stdout.pipe(tar.stdin);
    let left = 2;
    const codes = {};
    const exit = name => code => {
      codes[name] = code;
      if (--left === 0) finish(codes.bzip2 === 0 && codes.tar === 0 ? '' : `bzip2 exit ${codes.bzip2}, tar exit ${codes.tar}: ${err.trim()}`);
    };
    bz.on('close', exit('bzip2'));
    tar.on('close', exit('tar'));
  });
}

function sha256File(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function licenceText(dir) {
  return fs.readdirSync(dir).filter(f => LICENCE_FILES.test(f)).map(f => fs.readFileSync(path.join(dir, f), 'utf8')).join('\n\n');
}

// first lines of the model card, kept in LICENCES.md as the attribution
function attribution(text) {
  return text.split('\n').map(l => l.trim()).filter(Boolean).slice(0, 12).join('\n');
}

async function fetchPack(pack, o, release, pins, log) {
  if (o.exclude.includes(pack.id)) return { skipped: 'excluded by the catalogue policy' };
  const dl = path.join(o.out, '.download');
  const tmp = path.join(o.out, '.unpack', pack.id);
  fs.mkdirSync(dl, { recursive: true });
  const archive = path.join(dl, pack.id + '.tar.bz2');
  const pin = pins[pack.id];
  let got;
  if (pin && fs.existsSync(archive) && sha256File(archive) === pin.sha256) got = { sha256: pin.sha256, bytes: fs.statSync(archive).size };
  else got = await download(release + pack.id + '.tar.bz2', archive);
  if (!pin) {
    if (!o.recordPins) return { refused: 'no pin in voice/pins.json (run once with --record-pins, review, commit)' };
    pins[pack.id] = { sha256: got.sha256, bytes: got.bytes };
    log('pinned ' + pack.id + ' ' + got.sha256);
  } else if (got.sha256 !== pin.sha256) {
    return { refused: 'SHA-256 mismatch: got ' + got.sha256 + ', pinned ' + pin.sha256 };
  }
  fs.rmSync(tmp, { recursive: true, force: true });
  fs.mkdirSync(tmp, { recursive: true });
  const bad = await unpack(archive, tmp);
  if (bad) return { refused: 'could not unpack: ' + bad.slice(0, 300) };
  const top = path.join(tmp, pack.id);
  if (!fs.existsSync(top)) return { refused: 'archive does not contain the folder ' + pack.id };
  if (!packInstalled(pack, tmp)) return { refused: 'archive is missing files the catalogue names' };
  const text = licenceText(top);
  let lic = classify(text, pack.licenceEvidence);
  if (!lic.ok && !text.trim() && pack.licence !== 'unverified') lic = { ok: true, licence: pack.licence + ' (from the catalogue; the pack has no licence file)', warn: '' };
  if (!lic.ok && o.shipAll) lic = { ok: true, licence: 'not cleared: ' + lic.reason, warn: 'shipped by the maintainer\'s decision (catalogue policy shipAll)' };
  if (!lic.ok) return { refused: 'licence: ' + lic.reason };
  if (lic.warn && !o.allowLessac && !o.shipAll) return { refused: 'licence: ' + lic.warn };
  const dest = path.join(o.out, pack.id);
  fs.rmSync(dest, { recursive: true, force: true });
  if (!o.keep || o.keep.includes(pack.id)) fs.renameSync(top, dest);
  fs.rmSync(tmp, { recursive: true, force: true });
  const ev = pack.licenceEvidence;
  const extra = ev && lic.licence.includes('(looked up: ') ? '\n\nDataset terms (' + ev.source + '):\n' + ev.quote + (ev.attribution ? '\nAttribution: ' + ev.attribution : '') : '';
  return { licence: lic.licence, warn: lic.warn || '', sha256: got.sha256, bytes: got.bytes, attribution: attribution(text) + extra };
}

async function main(argv, log = m => process.stdout.write(m + '\n')) {
  const o = parseArgs(argv);
  const cat = loadCatalogue(o.catalogue);
  const raw = JSON.parse(fs.readFileSync(o.catalogue, 'utf8'));
  const release = o.release || raw.release;
  if (raw.policy && raw.policy.allowLessacDerived === true) o.allowLessac = true;
  if (raw.policy && raw.policy.shipAll === true) o.shipAll = true;
  if (raw.policy && Array.isArray(raw.policy.exclude)) o.exclude = raw.policy.exclude;
  const pins = fs.existsSync(o.pins) ? JSON.parse(fs.readFileSync(o.pins, 'utf8')) : {};
  const wanted = o.packs ? o.packs : [...cat.packs.keys()];
  for (const id of wanted) if (!cat.packs.has(id)) throw new Error('unknown pack ' + id);
  fs.mkdirSync(o.out, { recursive: true });
  const shipped = [];
  const refused = [];
  const skipped = []; // left out on purpose (policy.exclude): not a failure
  for (const id of wanted) {
    let r;
    try { r = await fetchPack(cat.packs.get(id), o, release, pins, log); } catch (e) { r = { refused: e.message }; }
    if (r.skipped) { skipped.push({ id, reason: r.skipped }); log('skipped ' + id + ': ' + r.skipped); }
    else if (r.refused) { refused.push({ id, reason: r.refused }); log('REFUSED ' + id + ': ' + r.refused); }
    else { shipped.push({ id, ...r }); log('ok ' + id + ' (' + r.licence + ')'); }
  }
  if (o.recordPins) fs.writeFileSync(o.pins, JSON.stringify(Object.fromEntries(Object.entries(pins).sort()), null, 1) + '\n');
  fs.writeFileSync(path.join(o.out, 'manifest.json'), JSON.stringify({ shipped: shipped.map(({ attribution, ...s }) => s), refused, skipped }, null, 1) + '\n');
  const md = ['# Voice pack licences', '', 'Each pack below ships unchanged from ' + release, ''];
  for (const s of shipped) md.push('## ' + s.id, '', 'Licence: ' + s.licence + (s.warn ? ' (' + s.warn + ')' : ''), '', '```', s.attribution || '(no model card)', '```', '');
  fs.writeFileSync(path.join(o.out, 'LICENCES.md'), md.join('\n'));
  fs.rmSync(path.join(o.out, '.unpack'), { recursive: true, force: true });
  log(shipped.length + ' shipped, ' + refused.length + ' refused, ' + skipped.length + ' skipped by policy');
  return refused.length ? 1 : 0;
}

if (require.main === module) {
  main(process.argv.slice(2)).then(code => process.exit(code), e => { process.stderr.write('fetch-voices: ' + e.message + '\n'); process.exit(2); });
}

module.exports = { parseArgs, main, tarEnv, unpack };
