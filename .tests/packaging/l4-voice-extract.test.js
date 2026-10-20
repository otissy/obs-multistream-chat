'use strict';
// L4: the installer's voice-pack unpacking (packaging/voice-extract.iss), compiled into a tiny Setup
// and run silently with PATH = System32 only, like a stock Windows PC: no bzip2 anywhere. Windows'
// tar.exe alone cannot read .tar.bz2 there; Setup's 7-Zip must undo the bzip2 layer first.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const { tarEnv } = require('../../scripts/fetch-voices');

const ROOT = path.resolve(__dirname, '..', '..');
const ISCC = [
  path.join(process.env.LOCALAPPDATA || '', 'Programs', 'Inno Setup 6', 'ISCC.exe'),
  path.join(process.env['ProgramFiles(x86)'] || '', 'Inno Setup 6', 'ISCC.exe'),
  path.join(process.env.ProgramFiles || '', 'Inno Setup 6', 'ISCC.exe'),
].find(p => fs.existsSync(p));
const SYS32 = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32');

function harness(dir) {
  const iss = path.join(dir, 'harness.iss');
  fs.writeFileSync(iss, [
    '[Setup]', 'AppName=voice-extract-test', 'AppVersion=1', 'DefaultDirName={tmp}\\x', 'CreateAppDir=no',
    'Uninstallable=no', 'PrivilegesRequired=lowest', 'ArchiveExtraction=full', 'OutputDir=' + dir, 'OutputBaseFilename=harness',
    // included after code, inside [Code], exactly as installer.iss does (a ';' comment there breaks)
    '[Code]', 'procedure Before; begin end;',
    '#include "' + path.join(ROOT, 'packaging', 'voice-extract.iss') + '"',
    '[Code]',
    'function InitializeSetup: Boolean;',
    'var Err: String;',
    'begin',
    "  if UnpackVoiceArchive(ExpandConstant('{param:archive}'), ExpandConstant('{param:dest}'), Err) then Log('RESULT ok')",
    "  else Log('RESULT failed: ' + Err);",
    '  Result := False;',
    'end;', '',
  ].join('\r\n'));
  const c = spawnSync(ISCC, ['/Q', iss], { encoding: 'utf8', windowsHide: true });
  assert.strictEqual(c.status, 0, c.stdout + c.stderr);
  return path.join(dir, 'harness.exe');
}

function runHarness(exe, archive, dest, log) {
  spawnSync(exe, ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/LOG=' + log, '/archive=' + archive, '/dest=' + dest],
    { windowsHide: true, env: { SystemRoot: process.env.SystemRoot, TEMP: os.tmpdir(), TMP: os.tmpdir(), PATH: SYS32 } });
  // the Setup stub can return before its child has written the log: wait for the RESULT line
  const until = Date.now() + 30000;
  while (Date.now() < until) {
    const t = fs.existsSync(log) ? fs.readFileSync(log, 'utf8') : '';
    const m = /RESULT (.*)/.exec(t);
    if (m) return m[1].trim();
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 250);
  }
  return 'no RESULT in ' + log;
}

const ready = process.platform === 'win32' && ISCC && (() => {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-bz2-'));
  fs.writeFileSync(path.join(d, 'f'), 'x');
  const r = spawnSync('tar', ['-cjf', path.join(d, 'f.tar.bz2'), '-C', d, 'f'], { env: tarEnv() });
  fs.rmSync(d, { recursive: true, force: true });
  return r.status === 0;
})();

test('Setup unpacks a .tar.bz2 voice pack on a PC with no bzip2 on PATH, and reports a broken one',
  { skip: !ready && 'needs Windows, Inno Setup 6.5+ and a bzip2 to build the fixture' }, () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-vx-'));
    try {
      const id = 'vits-piper-en_GB-test-medium';
      const src = path.join(dir, 'src', id, 'espeak-ng-data');
      fs.mkdirSync(src, { recursive: true });
      fs.writeFileSync(path.join(dir, 'src', id, 'en_GB-test-medium.onnx'), 'model');
      fs.writeFileSync(path.join(src, 'phontab'), 'data');
      const archive = path.join(dir, id + '.tar.bz2');
      assert.strictEqual(spawnSync('tar', ['-cjf', archive, '-C', path.join(dir, 'src'), id], { env: tarEnv() }).status, 0);

      // the premise: on this PATH, tar.exe by itself cannot read it
      const bare = spawnSync(path.join(SYS32, 'tar.exe'), ['-tjf', archive], { encoding: 'utf8', env: { SystemRoot: process.env.SystemRoot, PATH: SYS32 } });
      assert.notStrictEqual(bare.status, 0, 'tar.exe read bzip2 without help: ' + bare.stdout);

      const exe = harness(dir);
      const dest = path.join(dir, 'voices');
      assert.strictEqual(runHarness(exe, archive, dest, path.join(dir, 'ok.log')), 'ok');
      assert.strictEqual(fs.readFileSync(path.join(dest, id, 'en_GB-test-medium.onnx'), 'utf8'), 'model');
      assert.strictEqual(fs.readFileSync(path.join(dest, id, 'espeak-ng-data', 'phontab'), 'utf8'), 'data');
      assert.deepStrictEqual(fs.readdirSync(dir).filter(f => f.endsWith('.unpack')), [], 'the work folder is removed');

      const broken = path.join(dir, 'vits-piper-broken.tar.bz2');
      fs.writeFileSync(broken, 'BZh9 not really bzip2');
      assert.match(runHarness(exe, broken, path.join(dir, 'voices2'), path.join(dir, 'bad.log')), /^failed: /);
    } finally {
      fs.rmSync(dir, { recursive: true, force: true });
    }
  });

test('installer.iss unpacks voices through voice-extract.iss, never tar -xjf', () => {
  const iss = fs.readFileSync(path.join(ROOT, 'packaging', 'installer.iss'), 'utf8');
  assert.match(iss, /^ArchiveExtraction=full$/m);
  assert.match(iss, /#include "voice-extract\.iss"/);
  assert.ok(!/-xjf/.test(iss + fs.readFileSync(path.join(ROOT, 'packaging', 'voice-extract.iss'), 'utf8')));
});
