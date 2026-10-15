'use strict';
// L1: glue sources exist, and every libobs / frontend / Qt entry point they call is declared in the
// pinned 32.2.2 headers (the API audit, as a test).
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const { ROOT, sdk, walk } = require('./helpers');

const OBS = path.join(ROOT, 'src', 'obs');
const sources = fs.readdirSync(OBS).filter(f => /\.(cpp|hpp)$/.test(f));

test('glue files exist', () => {
  for (const f of ['plugin-main.cpp', 'output_manager.cpp', 'output_manager.hpp', 'dock.cpp', 'dock.hpp', 'plugin.hpp', 'CMakeLists.txt'])
    assert.ok(fs.existsSync(path.join(OBS, f)), f);
  assert.ok(fs.existsSync(path.join(ROOT, 'data', 'locale', 'en-US.ini')));
});

test('every obs_*/signal_*/calldata_*/video_output_* call is declared in the 32.2.2 headers', t => {
  const sdkDir = sdk('.obs-sdk');
  if (!sdkDir) return t.skip('UNVERIFIED: .obs-sdk not present (run scripts/fetch-sdks.ps1)');
  const decl = walk(sdkDir, /\.h$/).map(p => fs.readFileSync(p, 'utf8')).join('\n');
  const used = new Set();
  for (const f of sources) {
    const code = fs.readFileSync(path.join(OBS, f), 'utf8').replace(/\/\/.*$/gm, '').replace(/\/\*[\s\S]*?\*\//g, '');
    for (const m of code.matchAll(/\b((?:obs|signal_handler|calldata|video_output|os_sleep|bfree|blog)_?[a-z0-9_]*)\s*\(/g)) used.add(m[1]);
  }
  used.delete('obs_module_text'); // defined by OBS_MODULE_USE_DEFAULT_LOCALE
  for (const own of ['obs_module_load', 'obs_module_unload', 'obs_module_name', 'obs_module_description']) used.delete(own);
  assert.ok(used.size > 25, 'parsed too few calls: ' + [...used].join());
  const missing = [...used].filter(n => !decl.includes(n + '(') && !decl.includes(n + ' ('));
  assert.deepStrictEqual(missing, [], 'not declared in the pinned headers: ' + missing.join());
});

test('plugin never logs or formats key fields', () => {
  for (const f of sources) {
    const code = fs.readFileSync(path.join(OBS, f), 'utf8');
    assert.ok(!/blog\([^;]*(\.key\b|key_token|\bkey\b)/.test(code.replace(/"[^"]*"/g, '""')), f);
  }
});

test('output manager never frees what libobs owns and never leaves rtmp reconnect on', () => {
  const src = fs.readFileSync(path.join(OBS, 'output_manager.cpp'), 'utf8');
  const fleet = fs.readFileSync(path.join(ROOT, 'src', 'core', 'fleet.cpp'), 'utf8'); // 0.3.0: the orchestration moved to the core
  assert.match(src, /obs_output_set_reconnect_settings\(a\.out, 0, 0\)/);
  assert.match(fleet, /secure_zero\(key\)/);
  assert.ok(!/obs_output_release\(main\);[\s\S]{0,80}obs_output_get_video_encoder\(main\)/.test(src), 'encoders read after releasing main');
});
