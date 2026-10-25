'use strict';
// L4: OBS 33 plugin layout (https://obsproject.com/kb/legacy-plugin-locations). install.ps1 -Layout
// both|legacy|modern against scratch folders only, plus the setup script carrying the top-level DLL.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync, spawn } = require('child_process');

const ROOT = path.resolve(__dirname, '..', '..');
const install = (args) => spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
  path.join(ROOT, 'scripts', 'install.ps1'), '-IgnoreRunningObs', ...args], { encoding: 'utf8', windowsHide: true });

function fx(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-l33-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const dll = path.join(dir, 'obs-multistream-chat.dll');
  fs.writeFileSync(dll, `MZ fake ${Math.random()}`);
  const dest = path.join(dir, 'plugins', 'obs-multistream-chat');
  return { dir, dll, dest, legacy: path.join(dest, 'bin', '64bit', 'obs-multistream-chat.dll'), modern: path.join(dest, 'obs-multistream-chat.dll') };
}

test('default layout: both DLL copies, byte-identical, data beside them', (t) => {
  const f = fx(t);
  const r = install(['-Dll', f.dll, '-Dest', f.dest]);
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  assert.deepStrictEqual(fs.readFileSync(f.legacy), fs.readFileSync(f.dll));
  assert.deepStrictEqual(fs.readFileSync(f.modern), fs.readFileSync(f.dll));
  assert.ok(fs.existsSync(path.join(f.dest, 'data', 'overlay', 'chat.html')));
  assert.match(r.stdout, /both layout/);
});

test('-Layout legacy: only bin\\64bit, a stale top-level copy is removed', (t) => {
  const f = fx(t);
  fs.mkdirSync(f.dest, { recursive: true }); fs.writeFileSync(f.modern, 'stale');
  const r = install(['-Dll', f.dll, '-Dest', f.dest, '-Layout', 'legacy']);
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  assert.ok(fs.existsSync(f.legacy));
  assert.ok(!fs.existsSync(f.modern));
});

test('-Layout modern: only the top-level DLL, stale legacy copy and empty bin\\ removed', (t) => {
  const f = fx(t);
  fs.mkdirSync(path.dirname(f.legacy), { recursive: true }); fs.writeFileSync(f.legacy, 'stale');
  const r = install(['-Dll', f.dll, '-Dest', f.dest, '-Layout', 'modern']);
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  assert.deepStrictEqual(fs.readFileSync(f.modern), fs.readFileSync(f.dll));
  assert.ok(!fs.existsSync(path.join(f.dest, 'bin')));
});

test('-Layout with an unknown value fails and writes nothing', (t) => {
  const f = fx(t);
  const r = install(['-Dll', f.dll, '-Dest', f.dest, '-Layout', 'flat']);
  assert.notStrictEqual(r.status, 0);
  assert.ok(!fs.existsSync(f.dest));
});

test('a locked top-level DLL (OBS 33 running) fails loudly instead of reporting success', async (t) => {
  const f = fx(t);
  assert.strictEqual(install(['-Dll', f.dll, '-Dest', f.dest]).status, 0);
  const ps = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command',
    `$h=[IO.File]::Open('${f.modern}','Open','ReadWrite','None'); 'LOCKED'; [Console]::In.ReadLine() | Out-Null; $h.Close()`],
    { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  await new Promise((res, rej) => { let b = ''; ps.stdout.on('data', (d) => { b += d; if (b.includes('LOCKED')) res(); }); ps.on('exit', () => rej(new Error('lock helper exited'))); });
  try {
    const r = install(['-Dll', f.dll, '-Dest', f.dest]);
    assert.notStrictEqual(r.status, 0, r.stdout);
    assert.doesNotMatch(r.stdout, /installed to/);
  } finally { await new Promise((r) => { ps.on('exit', r); ps.stdin.end('\n'); }); }
});

test('Uninstall removes both layouts', (t) => {
  const f = fx(t);
  assert.strictEqual(install(['-Dll', f.dll, '-Dest', f.dest]).status, 0);
  const r = install(['-Dest', f.dest, '-Uninstall']);
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  assert.ok(!fs.existsSync(f.dest));
});

test('installer.iss installs the top-level DLL to {app} as well as bin\\64bit', () => {
  const iss = fs.readFileSync(path.join(ROOT, 'packaging', 'installer.iss'), 'utf8');
  assert.match(iss, /^Source: "\{#SourceTree\}\\bin\\64bit\\obs-multistream-chat\.dll"; DestDir: "\{app\}\\bin\\64bit"/m);
  assert.match(iss, /^Source: "\{#SourceTree\}\\obs-multistream-chat\.dll"; DestDir: "\{app\}"; Flags: ignoreversion restartreplace/m);
});
