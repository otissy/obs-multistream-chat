'use strict';
// L4: scripts/fetch-voices.js end to end against a local "release" server: pins, licence gate,
// unpacking, manifest and LICENCES.md. Real archives are built with the system tar (bzip2), the
// same tool the build uses on Windows (tar.exe).
const test = require('node:test');
const assert = require('node:assert');
const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const { ROOT } = require('./helpers');
const { main } = require('../../scripts/fetch-voices');

const tarOk = spawnSync('tar', ['--version']).status === 0;

// a small catalogue with three Piper packs: good licence, non-commercial, no licence at all
function fixture() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-fetch-'));
  const packs = [['vits-piper-en_GB-good-medium', '* License: CC BY 4.0\n'], ['vits-piper-en_GB-nc-medium', 'License: CC BY-NC-SA 4.0\n'], ['vits-piper-en_GB-none-medium', null]];
  const cat = { schema: 1, release: 'https://unused.example/', packs: [], voices: [] };
  const served = new Map();
  for (const [id, card] of packs) {
    const name = id.replace('vits-piper-', '');
    cat.packs.push({ id, engine: 'vits', licence: 'unverified', files: { model: name + '.onnx', tokens: 'tokens.txt', dataDir: 'espeak-ng-data' } });
    cat.voices.push({ id: 'piper.' + name, name, locale: 'en-GB', language: 'English', pack: id, sid: 0 });
    const src = path.join(dir, 'src', id);
    fs.mkdirSync(path.join(src, 'espeak-ng-data'), { recursive: true });
    fs.writeFileSync(path.join(src, name + '.onnx'), 'model ' + id);
    fs.writeFileSync(path.join(src, 'tokens.txt'), 'a 1');
    if (card) fs.writeFileSync(path.join(src, 'MODEL_CARD'), '# Model card for ' + name + '\n\n' + card);
    const archive = path.join(dir, id + '.tar.bz2');
    assert.strictEqual(spawnSync('tar', ['-cjf', archive, '-C', path.join(dir, 'src'), id]).status, 0);
    served.set('/' + id + '.tar.bz2', fs.readFileSync(archive));
  }
  fs.writeFileSync(path.join(dir, 'catalogue.json'), JSON.stringify(cat));
  return { dir, served };
}

function serve(served) {
  const hits = [];
  const server = http.createServer((req, res) => {
    hits.push(req.url);
    const body = served.get(req.url);
    if (!body) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'Content-Type': 'application/x-bzip2' });
    res.end(body);
  });
  return new Promise(r => server.listen(0, '127.0.0.1', () => r({ server, hits, base: 'http://127.0.0.1:' + server.address().port + '/' })));
}

test('fetch-voices: pins are required, recorded on request, and then enforced', { skip: !tarOk && 'no tar' }, async () => {
  const { dir, served } = fixture();
  const { server, hits, base } = await serve(served);
  const out = path.join(dir, 'out');
  const pins = path.join(dir, 'pins.json');
  const args = ['--out', out, '--catalogue', path.join(dir, 'catalogue.json'), '--pins', pins, '--release', base];
  const quiet = () => {};
  try {
    // no pins yet: everything refused, nothing unpacked
    assert.strictEqual(await main(args, quiet), 1);
    assert.ok(!fs.existsSync(path.join(out, 'vits-piper-en_GB-good-medium')));
    // record pins: the good pack ships; the NC and the unlicensed pack are refused by the gate
    assert.strictEqual(await main([...args, '--record-pins'], quiet), 1);
    const m = JSON.parse(fs.readFileSync(path.join(out, 'manifest.json'), 'utf8'));
    assert.deepStrictEqual(m.shipped.map(s => s.id), ['vits-piper-en_GB-good-medium']);
    assert.deepStrictEqual(m.refused.map(r => r.id).sort(), ['vits-piper-en_GB-nc-medium', 'vits-piper-en_GB-none-medium']);
    assert.match(m.refused.find(r => r.id.includes('-nc-')).reason, /non-commercial/);
    assert.match(m.refused.find(r => r.id.includes('-none-')).reason, /no licence text/);
    assert.ok(fs.existsSync(path.join(out, 'vits-piper-en_GB-good-medium', 'en_GB-good-medium.onnx')));
    assert.match(fs.readFileSync(path.join(out, 'LICENCES.md'), 'utf8'), /Model card for en_GB-good-medium/);
    const recorded = JSON.parse(fs.readFileSync(pins, 'utf8'));
    assert.strictEqual(recorded['vits-piper-en_GB-good-medium'].sha256,
      crypto.createHash('sha256').update(served.get('/vits-piper-en_GB-good-medium.tar.bz2')).digest('hex'));
    // a pinned, already-downloaded archive is not fetched again
    const before = hits.length;
    assert.strictEqual(await main([...args, '--packs', 'vits-piper-en_GB-good-medium'], quiet), 0);
    assert.strictEqual(hits.length, before);
    // the server's archive changes under the pin: refused
    served.set('/vits-piper-en_GB-good-medium.tar.bz2', Buffer.concat([served.get('/vits-piper-en_GB-good-medium.tar.bz2'), Buffer.from('x')]));
    fs.rmSync(path.join(out, '.download'), { recursive: true, force: true });
    assert.strictEqual(await main([...args, '--packs', 'vits-piper-en_GB-good-medium'], quiet), 1);
    assert.match(JSON.parse(fs.readFileSync(path.join(out, 'manifest.json'), 'utf8')).refused[0].reason, /SHA-256 mismatch/);
  } finally {
    server.close();
  }
});

test('fetch-voices: bad usage is refused before anything is downloaded', async () => {
  const { parseArgs } = require('../../scripts/fetch-voices');
  assert.throws(() => parseArgs([]), /--out/);
  assert.throws(() => parseArgs(['--out', 'x', '--release', 'http://example.com/']), /https/);
  assert.throws(() => parseArgs(['--out', 'x', '--bogus']), /unknown/);
  await assert.rejects(() => main(['--out', path.join(os.tmpdir(), 'oms-x'), '--packs', 'no-such-pack'], () => {}), /unknown pack/);
});

test('the real catalogue release URL is the k2-fsa tts-models release', () => {
  const c = JSON.parse(fs.readFileSync(path.join(ROOT, 'voice', 'catalogue.json'), 'utf8'));
  assert.strictEqual(c.release, 'https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/');
});

// ---- installer voice components -----------------------------------------------------------------
const { generate, DEFAULT_EMBED } = require('../../scripts/voice-installer');
const { loadCatalogue } = require('../../voice/lib/catalogue');

test('voice-installer: shipped packs become unticked download components per language; embedded ones do not', () => {
  const cat = loadCatalogue(path.join(ROOT, 'voice', 'catalogue.json'));
  const sha = c => c.repeat(64);
  const manifest = {
    shipped: [
      { id: 'kokoro-multi-lang-v1_0', sha256: sha('a'), bytes: 330e6 },
      { id: 'vits-piper-en_GB-alba-medium', sha256: sha('b'), bytes: 60e6 },
      { id: 'vits-piper-de_DE-thorsten-high', sha256: sha('c'), bytes: 110e6 },
      { id: 'vits-piper-de_DE-kerstin-low', sha256: sha('d'), bytes: 60e6 },
      { id: 'vits-piper-nl_BE-nathalie-medium', sha256: sha('e'), bytes: 62e6 },
      { id: 'vits-piper-pt_PT-tugao-medium', sha256: sha('f'), bytes: 62e6 },
    ],
    refused: [{ id: 'vits-piper-en_US-lessac-high', reason: 'licence' }],
  };
  const g = generate(cat, manifest, 'https://example.test/r/', DEFAULT_EMBED);
  assert.deepStrictEqual(g.embedded, ['kokoro-multi-lang-v1_0']);
  assert.deepStrictEqual(g.languages, ['Dutch', 'English', 'German', 'Portuguese']);
  const t = g.text;
  assert.match(t, /Name: "voices\\included"; Description: "Included: Chinese, English, French, Hindi, Italian, Japanese, Portuguese, Spanish"/);
  // download components carry no Types, so they start unticked
  const german = t.split('\r\n').find(l => l.startsWith('Name: "voices\\german"'));
  assert.ok(german && !/Types:/.test(german), german);
  assert.match(german, /German \(Germany\): download, 170 MB/);
  assert.match(t, /AddVoiceDownload\('https:\/\/example\.test\/r\/vits-piper-de_DE-thorsten-high\.tar\.bz2', 'vits-piper-de_DE-thorsten-high\.tar\.bz2', 'c{64}'\)/);
  assert.match(t, /Flemish \(Belgium\)/);
  assert.match(t, /Portuguese \(Portugal\): download, 62 MB/);
  assert.match(t, /English \(Scottish\): download, 60 MB/);
  assert.ok(!/kokoro-multi-lang-v1_0\.tar\.bz2/.test(t), 'an embedded pack must not be downloaded');
  assert.ok(!/lessac/.test(t), 'a refused pack must not appear');
  // Pascal: no semicolon before else
  assert.ok(!/\);\r\n\s+else/.test(t));
});

test('voice-installer: a manifest pack without a SHA-256, or unknown to the catalogue, stops the build', () => {
  const { main } = require('../../scripts/voice-installer');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-iss-'));
  const write = m => { fs.writeFileSync(path.join(dir, 'm.json'), JSON.stringify(m)); return ['--manifest', path.join(dir, 'm.json'), '--out', path.join(dir, 'v.iss')]; };
  assert.throws(() => main(write({ shipped: [{ id: 'vits-piper-de_DE-thorsten-high', sha256: '' }], refused: [] })), /SHA-256/);
  assert.throws(() => main(write({ shipped: [{ id: 'nope', sha256: 'a'.repeat(64) }], refused: [] })), /catalogue/);
});
