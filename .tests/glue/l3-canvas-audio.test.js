'use strict';
// L3 (source level): the 0.3.0 glue uses the canvas / audio-track / delay API that 32.2.2 declares, holds what it
// must hold, shares no global between outputs, and only ever READS Aitum's file.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const { ROOT, sdk, walk } = require('./helpers');

const read = (...p) => fs.readFileSync(path.join(ROOT, ...p), 'utf8');
const strip = s => s.replace(/\/\/.*$/gm, '').replace(/\/\*[\s\S]*?\*\//g, '');
const mgr = strip(read('src', 'obs', 'output_manager.cpp'));
const fleet = strip(read('src', 'core', 'fleet.cpp'));
const aitum = strip(read('src', 'core', 'aitum.cpp'));

test('every canvas / encoder / track / delay function the glue needs is declared in the pinned headers', t => {
  const dir = sdk('.obs-sdk');
  if (!dir) return t.skip('UNVERIFIED: .obs-sdk not present');
  const decl = walk(dir, /\.h$/).map(p => fs.readFileSync(p, 'utf8')).join('\n');
  const need = ['obs_enum_canvases', 'obs_get_canvas_by_name', 'obs_get_main_canvas', 'obs_canvas_get_video', 'obs_canvas_get_video_info',
    'obs_canvas_has_video', 'obs_canvas_get_name', 'obs_canvas_get_flags', 'obs_canvas_removed', 'obs_canvas_release',
    'obs_audio_encoder_create', 'obs_encoder_get_mixer_index', 'obs_output_get_audio_encoder', 'obs_output_set_audio_encoder',
    'obs_encoder_set_video', 'obs_encoder_set_audio', 'obs_encoder_set_scaled_size', 'obs_encoder_set_gpu_scale_type',
    'obs_encoder_set_frame_rate_divisor', 'obs_output_set_delay', 'obs_enum_encoder_types', 'obs_get_encoder_type',
    'obs_get_encoder_codec', 'obs_get_encoder_caps', 'obs_enum_outputs', 'obs_output_get_service', 'obs_service_get_connect_info',
    'obs_get_module', 'obs_get_module_binary_path', 'obs_frontend_get_profile_config', 'obs_frontend_get_current_profile_path'];
  const missing = need.filter(n => !decl.includes(n + '(') && !decl.includes(n + ' ('));
  assert.deepStrictEqual(missing, []);
  const glueCode = mgr + strip(read('src', 'obs', 'dock.cpp'));
  for (const n of need) assert.ok(glueCode.includes(n + '('), n + ' is not used (stale list)');
  assert.match(decl, /OBS_OUTPUT_DELAY_PRESERVE/);
  assert.match(decl, /OBS_SERVICE_CONNECT_INFO_STREAM_KEY/);
  assert.match(decl, /OBS_ENCODER_CAP_DEPRECATED/);
  assert.match(decl, /MAIN = 1 << 0/);
});

test('mixer track is 1-based in config and 0-based in libobs, exactly once', () => {
  assert.match(mgr, /obs_audio_encoder_create\("ffmpeg_aac"[^;]*static_cast<size_t>\(plan\.audio_track - 1\)/);
  assert.match(mgr, /obs_encoder_get_mixer_index\(a\)\) \+ 1/);
});

test('an own-canvas output keeps its canvas alive for the attempt and releases it', () => {
  assert.match(mgr, /a\.canvas = canvas;/);
  assert.match(mgr, /obs_canvas_release\(a\.canvas\)/);
  assert.match(mgr, /obs_encoder_set_video\(venc, video\)/);
});

test('libobs reconnect stays off, delay is applied per output, key is wiped', () => {
  assert.match(mgr, /obs_output_set_reconnect_settings\(a\.out, 0, 0\)/);
  assert.match(mgr, /obs_output_set_delay\(a\.out/);
  assert.match(fleet, /secure_zero\(key\)/);
});

test('no mutable global or static is shared between outputs (the point: more than two at once)', () => {
  for (const [name, code] of [['output_manager.cpp', mgr], ['fleet.cpp', fleet]]) {
    // namespace-scope or function-local statics that are not const / constexpr / functions
    const bad = [...code.matchAll(/^\s*(?:static|thread_local)\s+(?!const\b|constexpr\b|void\b|bool\b\s+\w+\s*\(|[\w:<>]+\s*\*?\s*\w+\s*\()([^;{]*);/gm)];
    assert.deepStrictEqual(bad.map(m => m[0].trim()), [], name);
    assert.ok(!/\bg_\w+/.test(code), name + ' touches a g_ global');
  }
  // the only lock is the event queue, never held while calling the backend
  assert.strictEqual((fleet.match(/std::lock_guard/g) || []).length, 2);
  const lockedBlocks = [...fleet.matchAll(/std::lock_guard<std::mutex> lk\(ev_m_\);([^}]*)\}/g)].map(m => m[1]);
  for (const b of lockedBlocks) assert.ok(!/backend_/.test(b));
});

test('Aitum files are only read: no writer, no remove, no rename anywhere on the Aitum path', () => {
  for (const code of [aitum, fleet]) assert.ok(!/ofstream|fopen|write_file|remove\(|rename\(|obs_data_save|CreateFile/.test(code));
  assert.ok(/aitum\.json/.test(mgr));
  assert.ok(!/(ofstream|write_file|save_json|obs_data_save)[^;]*aitum/.test(mgr));
  // we never start or stop Aitum's outputs: only ever look at other outputs' service and active state
  assert.ok(!/obs_output_start\(o\)|obs_output_stop\(o\)|obs_output_force_stop\(o\)/.test(mgr));
});

test('no stream key or Aitum server ever reaches a log call or a message string', () => {
  for (const f of ['aitum.cpp', 'fleet.cpp']) {
    const code = strip(read('src', 'core', f));
    assert.ok(!/log_\([^;]*(\.key\b|\bkey\b|akey|own_key|\.server\b)/.test(code.replace(/"[^"]*"/g, '""')), f);
  }
  assert.ok(!/log_line\([^;]*(\bkey\b|\.server)/.test(mgr.replace(/"[^"]*"/g, '""')));
});
