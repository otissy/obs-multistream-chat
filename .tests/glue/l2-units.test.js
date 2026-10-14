'use strict';
// L2: locale keys used by the dock exist (and none are dead); the plugin sources compile at /W4
// against the real 32.2.2 + Qt 6.11.1 headers (compile-only when import libs are absent).
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
const { ROOT, sdk } = require('./helpers');

const fwd = p => p.split(path.sep).join('/');
const ini = fs.readFileSync(path.join(ROOT, 'data', 'locale', 'en-US.ini'), 'utf8');
const keys = new Set([...ini.matchAll(/^([A-Za-z0-9_.]+)=/gm)].map(m => m[1]));

test('every T("key") / obs_module_text("key") in the glue has an en-US entry', () => {
  const used = new Set();
  for (const f of ['dock.cpp', 'plugin-main.cpp'])
    for (const m of fs.readFileSync(path.join(ROOT, 'src', 'obs', f), 'utf8').matchAll(/\b(?:T|obs_module_text)\("([A-Za-z0-9_.]+)"\)/g)) used.add(m[1]);
  assert.ok(used.size > 30);
  assert.deepStrictEqual([...used].filter(k => !keys.has(k)), []);
});

test('no dead locale keys', () => {
  const src = ['dock.cpp', 'plugin-main.cpp'].map(f => fs.readFileSync(path.join(ROOT, 'src', 'obs', f), 'utf8')).join('\n');
  assert.deepStrictEqual([...keys].filter(k => !src.includes('"' + k + '"')), []);
});

test('locale file is well formed', () => {
  for (const line of ini.split(/\r?\n/).filter(Boolean)) assert.match(line, /^[A-Za-z0-9_.]+=".*"$/, line);
});

test('plugin sources compile (needs MSVC + SDKs)', t => {
  const obs = sdk('.obs-sdk'), qt = sdk('.qt-sdk');
  if (!obs || !qt) return t.skip('UNVERIFIED: SDKs not present');
  const vswhere = 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe';
  if (!fs.existsSync(vswhere)) return t.skip('UNVERIFIED: no Visual Studio');
  const dir = path.join(ROOT, 'build', 'glue-compile');
  let r = spawnSync('cmake', ['-S', ROOT, '-B', dir, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DOMS_BUILD_TESTS=OFF',
    '-DOBS_SDK_DIR=' + fwd(obs), '-DQT_SDK_DIR=' + fwd(qt)], { encoding: 'utf8' });
  assert.strictEqual(r.status, 0, r.stdout + r.stderr);
  r = spawnSync('cmake', ['--build', dir, '--config', 'Release', '--target', 'obs-multistream-chat'], { encoding: 'utf8', maxBuffer: 1 << 26 });
  assert.strictEqual(r.status, 0, r.stdout.slice(-3000) + r.stderr.slice(-1000));
  assert.ok(!/warning C\d+/.test(r.stdout), 'warnings:\n' + (r.stdout.match(/.*warning C\d+.*/g) || []).slice(0, 5).join('\n'));
});
