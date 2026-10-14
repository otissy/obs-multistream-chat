'use strict';
// L4: run package.ps1 and install.ps1 for real, but against scratch folders only.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..');
const ps = (script, args) => spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', path.join(ROOT, 'scripts', script), ...args],
  { encoding: 'utf8', windowsHide: true });

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-pkg-'));
const fakeDll = path.join(scratch, 'obs-multistream-chat.dll');
fs.writeFileSync(fakeDll, 'MZ not a real dll');

test('package.ps1 makes a zip with the OBS plugin layout and a matching .sha256', () => {
  const out = path.join(scratch, 'dist');
  const r = ps('package.ps1', ['-Dll', fakeDll, '-OutDir', out]);
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  const zips = fs.readdirSync(out).filter(f => f.endsWith('.zip'));
  assert.strictEqual(zips.length, 1);
  assert.match(zips[0], /^obs-multistream-chat-\d+\.\d+\.\d+-windows-x64\.zip$/);
  const zipPath = path.join(out, zips[0]);
  const want = fs.readFileSync(zipPath + '.sha256', 'utf8').trim();
  const got = crypto.createHash('sha256').update(fs.readFileSync(zipPath)).digest('hex');
  assert.strictEqual(want, `${got} *${zips[0]}`);
  const extract = path.join(scratch, 'x');
  const e = spawnSync('powershell.exe', ['-NoProfile', '-Command', `Expand-Archive -LiteralPath '${zipPath}' -DestinationPath '${extract}'`], { encoding: 'utf8', windowsHide: true });
  assert.strictEqual(e.status, 0, e.stderr);
  for (const f of ['bin/64bit/obs-multistream-chat.dll', 'data/locale/en-US.ini', 'data/overlay/overlay.html', 'data/overlay/overlay.js', 'data/overlay/icons/twitch.svg', 'data/overlay/icons/x.svg', 'data/overlay/chat.html', 'data/overlay/chat.js', 'data/reader/index.js', 'data/reader/lib/inject.js', 'data/reader/adapters/twitch.json', 'data/reader/adapters/x.json', 'NOTICE', 'LICENSE', 'README.md', 'docs/security.md'])
    assert.ok(fs.existsSync(path.join(extract, 'obs-multistream-chat', f)), f);
  assert.ok(!fs.existsSync(path.join(extract, 'obs-multistream-chat', '.obs-sdk')));
});

test('install.ps1 installs into the given folder with the ProgramData layout, then uninstalls', () => {
  const dest = path.join(scratch, 'plugins', 'obs-multistream-chat');
  let r = ps('install.ps1', ['-Dll', fakeDll, '-Dest', dest, '-IgnoreRunningObs']);
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  assert.ok(fs.existsSync(path.join(dest, 'bin', '64bit', 'obs-multistream-chat.dll')));
  assert.ok(fs.existsSync(path.join(dest, 'data', 'locale', 'en-US.ini')));
  assert.ok(fs.existsSync(path.join(dest, 'data', 'overlay', 'overlay.html')));
  assert.ok(fs.existsSync(path.join(dest, 'data', 'reader', 'index.js')));
  assert.ok(fs.existsSync(path.join(dest, 'data', 'reader', 'adapters', 'kick.json')));
  r = ps('install.ps1', ['-Dest', dest, '-Uninstall', '-IgnoreRunningObs']);
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  assert.ok(!fs.existsSync(dest));
});

test('install.ps1 offers to remove an old obs-multistream folder (-RemoveOld deletes it)', () => {
  const plugins = path.join(scratch, 'plugins2');
  const old = path.join(plugins, 'obs-multistream');
  fs.mkdirSync(old, { recursive: true });
  fs.writeFileSync(path.join(old, 'marker.txt'), 'x');
  const dest = path.join(plugins, 'obs-multistream-chat');
  let r = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', path.join(ROOT, 'scripts', 'install.ps1'), '-Dll', fakeDll, '-Dest', dest, '-IgnoreRunningObs'], { encoding: 'utf8', windowsHide: true, input: '' });
  assert.ok(fs.existsSync(old), 'must not delete without consent');
  assert.match(r.stdout, /old install/);
  r = ps('install.ps1', ['-Dll', fakeDll, '-Dest', dest, '-IgnoreRunningObs', '-RemoveOld']);
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  assert.ok(!fs.existsSync(old));
  assert.ok(fs.existsSync(path.join(dest, 'bin', '64bit', 'obs-multistream-chat.dll')));
});

test('install.ps1 without a DLL fails clearly and writes nothing', () => {
  const dest = path.join(scratch, 'nothing');
  const r = ps('install.ps1', ['-Dll', path.join(scratch, 'missing.dll'), '-Dest', dest, '-IgnoreRunningObs']);
  assert.notStrictEqual(r.status, 0);
  assert.ok(!fs.existsSync(dest));
});

test('install.ps1 never touches the real ProgramData plugin folder during tests', () => {
  const real = path.join(process.env.ProgramData || 'C:/ProgramData', 'obs-studio', 'plugins', 'obs-multistream-chat');
  assert.ok(!fs.existsSync(real) || fs.statSync(real).mtimeMs < Date.now() - 3600 * 1000, 'real install folder changed recently');
});

test.after(() => fs.rmSync(scratch, { recursive: true, force: true }));
