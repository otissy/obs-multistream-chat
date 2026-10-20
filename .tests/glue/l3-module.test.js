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

// ---- 0.3.1: the dock must always be reachable -------------------------------------------------
const main = strip(read('src', 'obs', 'plugin-main.cpp'));

test('the dock is registered in obs_module_post_load, never from obs_module_load, and the result is checked', () => {
  const load = main.slice(main.indexOf('bool obs_module_load(void)'), main.indexOf('void obs_module_post_load(void)'));
  assert.ok(!/add_dock_by_id|MultistreamDock\(/.test(load), 'obs_module_load must not register the dock');
  const post = main.slice(main.indexOf('void obs_module_post_load(void)'), main.indexOf('void obs_module_unload(void)'));
  assert.match(post, /create_dock\("post load"\)/);
  assert.match(post, /install_tools_entry\(\)/);
  assert.match(main, /bool ok = obs_frontend_add_dock_by_id\(/);
  assert.equal((main.match(/obs_frontend_add_dock_by_id\(/g) || []).length, 1);
});

test('every dock outcome is logged and one retry happens on FINISHED_LOADING', () => {
  const raw = read('src', 'obs', 'plugin-main.cpp');
  for (const msg of ['dock registered', 'dock registration returned false', 'frontend main window not ready']) assert.ok(raw.includes(msg), msg);
  assert.match(main, /case OBS_FRONTEND_EVENT_FINISHED_LOADING:[\s\S]{0,200}g_dock_retry_allowed = false;[\s\S]{0,60}create_dock\("finished loading"\)/);
});

test('Tools > OBS Multistream Chat always opens the panel, in a floating window if OBS refused the dock', t => {
  assert.match(main, /obs_frontend_add_tools_menu_qaction\(/);
  assert.match(main, /QAction::triggered, \[\]\(\) \{ show_panel\(\); \}/);
  assert.match(main, /g_fallback = new QDockWidget\(/);
  assert.match(main, /g_fallback->setFloating\(true\)/);
  const dir = sdk('.obs-sdk');
  if (dir) assert.ok(walk(dir, /\.h$/).map(p => fs.readFileSync(p, 'utf8')).join('\n').includes('obs_frontend_add_tools_menu_qaction('));
});

test('the tick timer does not depend on the dock existing', () => {
  assert.match(main, /st\.timer = new QTimer\(qApp\)/);
  assert.ok(!/new QTimer\(g_dock\)/.test(main));
});

test('the dock shows a big start/stop, a summary line and the chat reader state', () => {
  const dock = strip(read('src', 'obs', 'dock.cpp'));
  assert.match(dock, /big_btn_->setMinimumHeight\(40\)/);
  assert.match(dock, /update_glance\(views\)/);
  assert.match(dock, /plugin_reader_running\(\)/);
  for (const col of ['Col.Status', 'Col.Bitrate', 'Col.Dropped', 'Col.Uptime']) assert.ok(dock.includes(col), col);
});

// 0.5.0: one flat row of tabs, a "Get started" card, one way to add a chat channel (setup_flow.cpp)
test('the dock has one row of tabs and the chat panel has none of its own', () => {
  const dock = strip(read('src', 'obs', 'dock.cpp'));
  const chat = strip(read('src', 'obs', 'chat_panel.cpp'));
  assert.ok(!/new QTabWidget/.test(chat), 'chat_panel.cpp must not build its own tab bar');
  for (const t of ['Tab.Chat', 'Tab.Streams', 'Tab.Sources', 'Tab.Overlays', 'Tab.Voice']) assert.ok(dock.includes('T("' + t + '")'), t);
  assert.match(dock, /chat_panel_->attach\(tabs_, tab_chat_/);
});

test('the Get started card has three steps driven by setup_progress', () => {
  const dock = strip(read('src', 'obs', 'dock.cpp'));
  assert.match(dock, /setup_progress\(cfg, ui_flag\("overlay_copied"\)\)/);
  for (const k of ['Setup.Step1', 'Setup.Step2', 'Setup.Step3', 'Setup.Btn1', 'Setup.Btn2', 'Setup.Btn3']) assert.ok(dock.includes(k), k);
  assert.match(dock, /obs_module_config_path\("ui-state\.ini"\)/); // not in config.json
});

test('chat channels are added only through setup_flow (no guessing, no overwriting)', () => {
  const chat = strip(read('src', 'obs', 'chat_panel.cpp'));
  assert.match(chat, /add_chat_source\(st\.cfg\.chat\.sources/);
  assert.match(chat, /add_sources_from_destinations\(st\.cfg\.destinations, st\.cfg\.chat\.sources\)/);
  assert.match(chat, /set_reader_start_mode\(/);
  assert.ok(!/auto_connect_channel|sync_from_destinations|add_collaborator_dialog/.test(chat));
  assert.ok(!/"c" \+ std::to_string\(st\.cfg\.chat\.sources\.size\(\)/.test(chat), 'ids come from next_chat_source_id');
});

test('screenshots run only when OMSC_SHOTS is set', () => {
  const main = strip(read('src', 'obs', 'plugin-main.cpp'));
  assert.match(main, /getenv\("OMSC_SHOTS"\)/);
  assert.match(main, /if \(!dir \|\| !\*dir\)\s*return;/);
});

test('every README picture exists and every picture is used', () => {
  const readme = read('README.md');
  const used = [...readme.matchAll(/\]\((docs\/images\/[^)]+\.png)\)/g)].map(m => m[1]);
  assert.ok(used.length >= 9, 'README shows the step pictures');
  for (const u of used) assert.ok(fs.existsSync(path.join(ROOT, u)), u);
  const files = fs.readdirSync(path.join(ROOT, 'docs', 'images')).map(f => 'docs/images/' + f);
  assert.deepStrictEqual(files.filter(f => !used.includes(f)), []);
});
