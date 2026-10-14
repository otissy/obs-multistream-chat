'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..');
const read = f => fs.readFileSync(path.join(ROOT, f), 'utf8');

test('all documented files exist', () => {
  for (const f of ['README.md', 'CHANGELOG.md', 'PROJECT.md', 'NOTICE', 'LICENSE', 'docs/security.md', 'docs/limits.md',
    'scripts/build.ps1', 'scripts/install.ps1', 'scripts/package.ps1', 'scripts/fetch-sdks.ps1', 'scripts/make-import-lib.ps1', 'scripts/vsenv.ps1', 'CMakePresets.json'])
    assert.ok(fs.existsSync(path.join(ROOT, f)), f);
});

test('every PowerShell script parses', () => {
  for (const f of fs.readdirSync(path.join(ROOT, 'scripts')).filter(x => x.endsWith('.ps1'))) {
    const p = path.join(ROOT, 'scripts', f).replace(/'/g, "''");
    const cmd = `$e=$null; [void][System.Management.Automation.Language.Parser]::ParseFile('${p}',[ref]$null,[ref]$e); if($e){ $e | % { $_.Message }; exit 1 }`;
    const r = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', cmd], { encoding: 'utf8', windowsHide: true });
    assert.strictEqual(r.status, 0, f + '\n' + r.stdout + r.stderr);
  }
});

test('presets parse and name the documented preset', () => {
  const j = JSON.parse(read('CMakePresets.json'));
  assert.ok(j.configurePresets.some(p => p.name === 'windows-x64'));
  assert.ok(j.buildPresets.some(p => p.name === 'windows-x64'));
});

test('pinned hashes in fetch-sdks.ps1 match PROJECT.md', () => {
  const fetch = read('scripts/fetch-sdks.ps1'), proj = read('PROJECT.md');
  for (const h of fetch.match(/[0-9a-f]{64}/g)) assert.ok(proj.includes(h), h);
});

test('docs contain no real-looking stream key; only the documented fake one', () => {
  const files = [];
  (function walk(d) {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      if (['.git', 'build', 'dist', '.obs-sdk', '.qt-sdk', 'node_modules'].includes(e.name)) continue;
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p); else if (/\.(md|cpp|hpp|js|ps1|json|ini|html|txt)$/.test(e.name) || e.name === 'NOTICE') files.push(p);
    }
  })(ROOT);
  for (const f of files) {
    const t = fs.readFileSync(f, 'utf8');
    for (const m of t.matchAll(/live_\d+_[A-Za-z0-9]+/g)) assert.ok(/^live_(000000_test|123456_abcDEF123)$/.test(m[0]), `${f}: ${m[0]}`);
  }
});

test('build root: -BuildRoot beats OMSC_BUILD_ROOT beats the D: builds default; only that folder is created', () => {
  const os = require('os');
  const base = fs.mkdtempSync(path.join(os.tmpdir(), 'omsc-root-'));
  const a = path.join(base, 'a'), b = path.join(base, 'b');
  const run = (extraEnv, arg) => spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command',
    `. '${path.join(ROOT, 'scripts', 'vsenv.ps1')}'; Get-BuildRoot ${arg}`], { encoding: 'utf8', windowsHide: true, env: { ...process.env, ...extraEnv } });
  let r = run({ OMSC_BUILD_ROOT: a }, '');
  assert.strictEqual(r.stdout.trim(), a);
  r = run({ OMSC_BUILD_ROOT: a }, `'${b}'`);
  assert.strictEqual(r.stdout.trim(), b);
  assert.ok(fs.existsSync(a) && fs.existsSync(b));
  fs.rmSync(base, { recursive: true, force: true });
});

test('scripts and CMake default to the central folder and never to build/ in the repo', () => {
  for (const f of ['scripts/build.ps1', 'scripts/install.ps1', 'scripts/package.ps1', 'scripts/make-import-lib.ps1']) {
    const t = read(f);
    assert.ok(t.includes('BuildRoot'), f + ' has no -BuildRoot');
    assert.ok(!/Join-Path \$Root 'build'/.test(t) && !/OutDir '\.obs-sdk/.test(t), f + ' still writes into the repo');
  }
  assert.ok(read('scripts/vsenv.ps1').includes('D:\\builds\\obs-multistream-chat'));
  assert.ok(read('scripts/vsenv.ps1').includes('OMSC_BUILD_ROOT'));
  assert.ok(read('CMakePresets.json').includes('D:/builds/obs-multistream-chat/'));
  assert.ok(!/\$\{sourceDir\}\/build/.test(read('CMakePresets.json')));
});

test('README documents install, per-platform setup, troubleshooting and antivirus', () => {
  const r = read('README.md');
  for (const h of ['## Install', '## Setup', '## Troubleshooting', '### Antivirus', 'Twitch', 'YouTube', 'Kick', 'IDP.Generic', 'Restore & Exclude'])
    assert.ok(r.includes(h), h);
});

test('docs/limits.md covers the required topics', () => {
  const l = read('docs/limits.md');
  for (const w of ['viewer counts', 'chat', 'NVENC', 'One bitrate'])
    assert.ok(l.includes(w), w);
});
