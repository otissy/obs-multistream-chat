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
const { main, tarEnv } = require('../../scripts/fetch-voices');

// the fixture needs a tar that can write bzip2: on Windows that is tar.exe plus a bzip2 that
// tarEnv() finds (Git's), exactly what fetch-voices itself unpacks with
const tarOk = spawnSync('tar', ['--version']).status === 0 && (() => {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-bz2-'));
  fs.writeFileSync(path.join(d, 'f'), 'x');
  const r = spawnSync('tar', ['-cjf', path.join(d, 'f.tar.bz2'), '-C', d, 'f'], { env: tarEnv() });
  fs.rmSync(d, { recursive: true, force: true });
  return r.status === 0;
})();

// a small catalogue of Piper packs: good licence, non-commercial, no licence at all (or the given ones)
function fixture(packs = [['vits-piper-en_GB-good-medium', '* License: CC BY 4.0\n'], ['vits-piper-en_GB-nc-medium', 'License: CC BY-NC-SA 4.0\n'], ['vits-piper-en_GB-none-medium', null]], policy) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-fetch-'));
  const cat = { schema: 1, release: 'https://unused.example/', packs: [], voices: [] };
  if (policy) cat.policy = policy;
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
    assert.strictEqual(spawnSync('tar', ['-cjf', archive, '-C', path.join(dir, 'src'), id], { env: tarEnv() }).status, 0);
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

test('fetch-voices: pins are required, recorded on request, and then enforced', { skip: !tarOk && 'no tar that can write bzip2' }, async () => {
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

test('fetch-voices: a Lessac-derived pack ships only when the catalogue policy allows it', { skip: !tarOk && 'no tar' }, async () => {
  const card = '* License: https://creativecommons.org/licenses/by/4.0/\n\n## Training\n\nFinetuned from U.S. English lessac voice (medium quality).\n';
  for (const [policy, want] of [[undefined, 1], [{ allowLessacDerived: true }, 0]]) {
    const { dir, served } = fixture([['vits-piper-en_GB-tuned-medium', card]], policy);
    const { server, base } = await serve(served);
    try {
      const out = path.join(dir, 'out');
      const code = await main(['--out', out, '--catalogue', path.join(dir, 'catalogue.json'), '--pins', path.join(dir, 'pins.json'), '--release', base, '--record-pins'], () => {});
      assert.strictEqual(code, want, JSON.stringify(policy));
      const m = JSON.parse(fs.readFileSync(path.join(out, 'manifest.json'), 'utf8'));
      if (want) assert.match(m.refused[0].reason, /Lessac/);
      else {
        assert.match(m.shipped[0].warn, /Lessac/);
        assert.match(fs.readFileSync(path.join(out, 'LICENCES.md'), 'utf8'), /Licence: CC-BY \(fine-tuned from Lessac/);
      }
    } finally {
      server.close();
    }
  }
});

test('fetch-voices: policy shipAll ships uncleared packs (verdict kept), exclude never ships, pins still hold', { skip: !tarOk && 'no tar' }, async () => {
  const { dir, served } = fixture(undefined, { shipAll: true, exclude: ['vits-piper-en_GB-none-medium'] });
  const { server, base } = await serve(served);
  try {
    const out = path.join(dir, 'out');
    const pins = path.join(dir, 'pins.json');
    const args = ['--out', out, '--catalogue', path.join(dir, 'catalogue.json'), '--pins', pins, '--release', base];
    assert.strictEqual(await main(args, () => {}), 1, 'no pins: nothing ships, shipAll or not');
    assert.strictEqual(await main([...args, '--record-pins'], () => {}), 0, 'a policy exclusion is not a failure');
    const m = JSON.parse(fs.readFileSync(path.join(out, 'manifest.json'), 'utf8'));
    assert.deepStrictEqual(m.shipped.map(x => x.id).sort(), ['vits-piper-en_GB-good-medium', 'vits-piper-en_GB-nc-medium']);
    const nc = m.shipped.find(x => x.id.includes('-nc-'));
    assert.match(nc.licence, /^not cleared: non-commercial/);
    assert.match(nc.warn, /shipAll/);
    assert.deepStrictEqual(m.refused, []);
    assert.deepStrictEqual(m.skipped, [{ id: 'vits-piper-en_GB-none-medium', reason: 'excluded by the catalogue policy' }]);
  } finally {
    server.close();
  }
});

test('the shipped catalogue ships every voice but Natia, and labels the uncleared ones', () => {
  const c = JSON.parse(fs.readFileSync(path.join(ROOT, 'voice', 'catalogue.json'), 'utf8'));
  assert.strictEqual(c.policy.shipAll, true);
  assert.deepStrictEqual(c.policy.exclude, ['vits-piper-ka_GE-natia-medium']);
  const labels = new Set(c.packs.map(p => p.usage).filter(Boolean));
  assert.deepStrictEqual([...labels].sort(), ['licence unclear', 'non-commercial']);
  assert.strictEqual(c.packs.find(p => p.id === 'vits-piper-en_US-ryan-high').usage, 'non-commercial');
});

test('the shipped catalogue records the maintainer\'s Lessac decision', () => {
  const c = JSON.parse(fs.readFileSync(path.join(ROOT, 'voice', 'catalogue.json'), 'utf8'));
  assert.strictEqual(c.policy.allowLessacDerived, true);
  assert.match(c.policy.decision, /otiyyy/);
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
  // #included inside [Code]: the first line must be a section header, never a ';' comment
  assert.strictEqual(t.split('\r\n')[0], '[Components]');
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

// ---- unpacking ----------------------------------------------------------------------------------
test('unpack: streams a multi-megabyte archive through bzip2 | tar, and reports corrupt input and a missing bzip2', { skip: !tarOk && 'no tar' }, async () => {
  const { unpack } = require('../../scripts/fetch-voices');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-unpack-'));
  const src = path.join(dir, 'src', 'pack');
  fs.mkdirSync(src, { recursive: true });
  const blob = crypto.randomBytes(6 * 1024 * 1024); // incompressible: fills any pipe many times over
  fs.writeFileSync(path.join(src, 'model.onnx'), blob);
  const archive = path.join(dir, 'pack.tar.bz2');
  assert.strictEqual(spawnSync('tar', ['-cjf', archive, '-C', path.join(dir, 'src'), 'pack']).status, 0);
  const out = path.join(dir, 'out');
  fs.mkdirSync(out);
  assert.strictEqual(await unpack(archive, out), '');
  assert.ok(fs.readFileSync(path.join(out, 'pack', 'model.onnx')).equals(blob));

  const bad = path.join(dir, 'bad.tar.bz2');
  fs.writeFileSync(bad, Buffer.concat([fs.readFileSync(archive).subarray(0, 4096), Buffer.alloc(4096, 7)]));
  assert.match(await unpack(bad, out), /bzip2 exit [1-9]/);

  assert.match(await unpack(archive, out, { PATH: path.join(dir, 'nowhere') }), /not found on PATH/);
});
