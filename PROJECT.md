# PROJECT.md - obs-multistream-chat

The map of the ground. Read before any task; update at the end of every task.
Only verified facts go in "Limits"; everything else is marked as design or unverified.

> **Build output (2026-10-14):** all compiled output is under one central folder, default
> `D:\builds\obs-multistream-chat\`, chosen by `-BuildRoot`, else env `OMSC_BUILD_ROOT` (`Get-BuildRoot` in
> `scripts/vsenv.ps1`, mirrored in `.tests/core/helpers.js` and CMake `OMS_IMPORT_LIB_DIR`).

> **Rename (2026-10-14):** formerly `obs-multistream`; now display name "OBS Multistream Chat", slug
> `obs-multistream-chat`. Rename only, no chat feature. C++ namespace `oms`, `OMS_*` CMake options and
> test helper names keep the short form on purpose. Older log lines/commits mention the old name.

## Unreleased (2026-10-24): chat overlay ticks, overlay access key

- **Chat overlay squish fixed.** Cause (measured, 800x400, 26 lines): `#chat` is a flex column with
  `overflow: hidden`, and each `.msg` had the default `flex-shrink: 1` plus `overflow: hidden`, which drops a flex
  item's automatic minimum height; so instead of overflowing, all lines shrank (31 px -> 15 px). Fix: `.msg { flex:
  none }` and `trimToFit()` in chat.js drops the oldest whole lines that no longer fit (align bottom and top; re-run
  on emote load and resize; the newest line always stays). Test: chat L4 "a full chat ticks".
- **Overlay access key.** Audit before the change: one HTTP server, bound to 127.0.0.1 only (peer re-checked),
  Host/Origin/Sec-Fetch-Site checks, `POST /chat/ingest` behind a per-run bearer token, but every GET (`/`,
  overlay.html/js, chat.html/js, icons, `/state.json`, `/events`, `/chat/events`, `/chat/status.json`) open to any
  local process. No WebSocket on this server (the reader's headless Chrome has its own loopback DevTools port).
  Now: per-install key `server.access_key` (32 bytes, `random_token_hex`, kept in config.json), every GET/HEAD must
  be under `/k/<key>/` (`split_access_key`, `secret_equal` constant-time), else 401 before routing. Pages use
  relative URLs so subresources keep the prefix. The plugin refuses to start the server without a key. Not TLS.
  Existing browser sources must re-copy the URL after updating.
- Lesson (2026-10-24): `D:` was full (0 bytes free, not caused by this work); a sandbox write failed with "No space
  left on device". The work moved to a C: copy. Check free space before building under `D:\builds`.

## 0.5.0 (2026-10-20): dock overhaul, first load in a real OBS

- **First time the plugin ran inside OBS**: a throw-away portable OBS 32.2.2 (`D:\builds\obs-portable`, zip sha-256 as
  in Limits) loaded the DLL under the module name `obs-multistream-chat-dev`; log: config loaded, overlay server on
  127.0.0.1, `dock registered (post load)`, Tools menu entry added. The user's real OBS was never touched: the portable
  copy's `config\obs-studio\plugin_manager\modules.json` (a JSON array of `{module_name, enabled:false, ...}`) switched
  off all 10 ProgramData plugins, and the log's "Loaded Modules" lists only bundled modules + the dev build. Not yet
  run in it: streaming, the chat reader, the voice helper.
- Dock: one row of tabs (Chat, Streams, Chat sources, Overlays, Voice), a "Get started" card (state in `ui-state.ini`,
  not config.json), Overlays tab, Add-destination basic/advanced split. Chat sources: one add path +
  add-only "from Streams" (`src/core/setup_flow.*`, 7 core tests). Fixed: Auto-Connect added a name on 4 platforms,
  Auto-sync overwrote a collaborator, `c<count+1>` ids repeated after a removal.
- README pictures: `OMSC_SHOTS=<dir>` (+ `OMSC_SHOTS_DEMO=1`, `OMSC_SHOTS_QUIT=1`) renders each page and dialog
  offscreen with `QWidget::grab` (WA_DontShowOnScreen) and the OBS window with the dock docked; sample data in memory
  only. Run with `--portable --multi --disable-updater --minimize-to-tray` so no window shows.
- Chat reader: Chromium (`%LOCALAPPDATA%\Chromium\...`, `Program Files\Chromium\...`) is searched after Google Chrome
  and before Edge (the author's PC has Chromium only).
- Gotchas found by looking at the pictures, not by tests: Qt does not size a table column by its cell widget
  (fixed width for the Start/Stop column); QTextBrowser ignores `margin-right` on spans; long combo items widen a
  scroll page past the dock (AdjustToMinimumContentsLengthWithIcon).

## 0.5.0, part 2 (2026-10-20, same release): YouTube by channel, Audio tab, voice per platform, low-memory reader

- **YouTube by channel**: target `@handle` (lower case) or a `UC...` channel id. The reader (source.js `_resolveLive`)
  opens `youtube.com/@handle/live` (pack `channelUrl`, `channelRecheckSec` 120) before the engine is installed, reads
  the canonical `watch?v=<id>`, then opens that video's popout chat. No id: state `offline`, look again after 120 s.
  disabled/replay/stub on a channel source: look again after 120 s. Rules mirrored in reader/lib/targets.js and
  core/chat_targets.cpp, checked by `.tests/chat/vectors/targets.json` (75 vectors). Bare 11-character input is a
  video id only when it has a digit, `-` or `_` ("RoyalAscent" = channel). Verified on a real channel (@lofigirl): reading in 2.5 s.
- **Audio tab** (src/obs/audio_page.*): destination -> track, meters from `obs_add_raw_audio_callback` (planar float, peak per
  tick, only while the tab is visible), main-stream track from the profile (`[Output] Mode`, `[AdvOut] TrackIndex`, else 1),
  "silent" after ~5 s below -55 dBFS. The Streams table keeps its track column hidden (column indices unchanged).
- **Voice per platform** + every Voice-tab setting persisted in `ui-state.ini` (QSettings group `voice`); core
  `voice_for_platform` falls back to the main voice when a platform's voice is not installed.
- **Low-memory reader (default)**: all sources share the default browser context, site isolation and per-site renderers
  off, background throttling off. Benchmarks in docs/benchmarks.md (5 chats 1,243 -> 410 MB; real pages 1,215 -> 829 MB,
  CPU 2.2 % -> 0.4 %). Off: env `OMSC_READER_LOWMEM=0` (the plugin rewrites chat-reader.json, so a file setting would not stick).
- **Fixed**: reader config allowed at most 10 sources while the plugin allows 32 (11+ enabled channels = no reader at all);
  "from Streams" fell back to the destination name, which 0.5.0 fills with the platform label.
- **Setup**: `docs/quickstart.html` (offline, the README pictures) in the Start menu and offered at the end of Setup.
- Lesson (2026-10-20): the reader's Chrome profile prefix `oms-chat-profile-` is shared with the user's real OBS reader. A
  benchmark that filtered by it counted (and a cleanup killed) the user's live reader. Measure and stop only descendants
  of your own process.

## 0.4.0 (2026-10-17): Live in-dock chat, vertical scenes & canvas integration, audio tracks, key reveal

- **In-dock Live Chat feed**: the streamer no longer needs to use a browser source overlay to read their own chat;
  merged chat from all enabled sources (Twitch, YouTube, Kick, TikTok, X) renders in real-time in the dock with
  platform brand badges, timestamps, high-contrast username colors, emote images, font size scaling, clear button,
  and smart auto-scroll with pause-on-scroll-up and resume.
- **Chat tab sub-tabs**: "Live Chat" (full height live stream) and "Sources & Settings" (channels table, filters,
  blocked words/users, and overlay URL copy for viewers), with an unread badge on Live Chat when on settings.
- **Vertical scenes & canvas**: canvas picker displays resolution and vertical tag (e.g. `Vertical (1080x1920 - Vertical)`),
  auto-suggests vertical canvas for TikTok destinations, and locks encoder to "own" with vertical bitrate defaults.
- **Separate audio tracks**: mixer tracks 1-6 with profile-defined track names from OBS (`[AdvOut] Track<N>Name`),
  own AAC encoder bitrate control, and clear routing guidance for OBS Advanced Audio Properties.
- **Stream key reveal & verification**: Reveal button in the destination dialog decrypts and displays the saved DPAPI key;
  `OBS_FRONTEND_EVENT_PROFILE_CHANGED` listener keeps configuration and canvas/track lists in sync when changing profiles.
- Built `obs-multistream-chat.dll` (692,224 bytes), package `dist\obs-multistream-chat-0.4.0-windows-x64.zip`
  (sha-256 `f478e75b8cd500447d511f37720f7d22acea5c34e2f744b3bd9224bce99e417b`). Core suite 145/145, glue/packaging 35/35, chat 98/98.

## 0.3.1 (2026-10-15): dock registration fix

Root cause of "no dock": registration happened in `obs_module_load` with the result ignored (silent when refused).
Evidence: the real OBS 32.2.2 log shows obs-track-monitor and casterview (both register in `obs_module_post_load`, which
obs-module.h documents as "called when all modules have finished loading") logging "dock registered" after the module list,
while this plugin logged only config/server/version lines. The exact reason OBS refuses an early registration was not proven
(OBS source is not vendored); the fix follows the working plugins and logs every outcome. Qt check: installed
Qt6Core/Gui/Widgets are 6.11.1.0 = headers/libs used (QT_VERSION_STR 6.11.1): no mismatch, no rebuild against other Qt.
Fallbacks: one retry on FINISHED_LOADING, Tools > OBS Multistream Chat (floating QDockWidget if not registered).

## Current state (2026-10-15)

Version 0.3.0 adds per-destination canvas, audio track, own-encoder options and optional Aitum Stream Suite
output settings (spec below, docs/aitum.md). Core logic is tested with mocks (fleet of 8 and 12 mock outputs);
the glue compiles and links against the pinned 32.2.2 headers and the installed obs.dll but has NEVER been
loaded into OBS. Orchestration now lives in `src/core/fleet.*` (backend interface), decisions in `plan.*`, Aitum
mapping in `aitum.*`; `src/obs/output_manager.*` is only the libobs backend.

## State of 0.2.0 (2026-10-14)

Version 0.2.0 adds the chat half (see "Chat (0.2.0)" below): reader, adapters, merge server, overlay
page, dock tab. Built and tested offline; no real platform page was ever loaded.

## 0.3.0 spec (2026-10-15): audio track, canvas, Aitum settings (SPEC FIRST, then built)

Why: Aitum Stream Suite provides the vertical canvas and scenes but crashes when it hosts more than 2
simultaneous streams. So THIS plugin runs the outputs (any number), optionally taking the *configuration*
from Aitum's output entries. Win conditions (each becomes a test):

1. A destination picks mixer track 1-6 (0 = "as the main stream"). Shared video + a track the main stream
   already encodes -> the main audio encoder is reused; any other track -> an own AAC encoder
   (`obs_audio_encoder_create(..., mixer_idx)`), selectable bitrate. Tracks are 1-based in the UI and config,
   0-based in libobs (`mixer_idx = track - 1`). Names come from `[AdvOut] Track<N>Name` of the profile.
2. A destination picks `canvas` ("" = main canvas, else the canvas NAME). A non-main canvas always uses an own
   video encoder (resolution differs), chosen by encoder id (default `obs_x264`; any installed h264/hevc/av1
   video encoder id), bitrate, keyframe interval, preset, profile, rate control, optional rescale, fps divisor.
   Canvas missing -> clear status "canvas X is not available", never a crash, retried at next start.
3. "Use Aitum Stream Suite output settings": per destination `aitum.mode` = `off | copy | follow` and
   `aitum.output` = the Aitum output's name. `copy` imports once (key sealed with DPAPI at once). `follow`
   re-reads the ACTIVE profile's `aitum.json` at every start; URL/key from Aitum are held in memory only for the
   launch, then wiped, never written to our config, log or dock. Unknown/missing/malformed file or a shape that
   is not the inspected one -> soft failure with a clear message, the destination keeps its own settings.
   Aitum's own outputs are never started, stopped or modified; our plugin only READS `aitum.json`.
4. Double-streaming warning: the same server+key enabled in two places (our destinations, Aitum's file, any
   other active libobs output incl. Aitum's own and the main stream) shows a dock warning. Never auto-stops.
5. More than 2 outputs: per-destination state only (no shared global), tested with 8 mock outputs through the
   output backend abstraction (`src/core/fleet.*`), including start/stop ordering and error isolation.

Config schema 4 (3 -> 4 is a migration, saved as 4; an older plugin reads 4 as "too new", read-only):
per destination `encoder` `"shared"|"own"` (`"x264"` -> `"own"`, `video_encoder:"obs_x264"`, preset
`veryfast`, keyint 2, i.e. exactly what 0.2.0 did), `video_encoder`, `bitrate_kbps`, `keyint_sec`, `preset`,
`profile`, `rate_control`, `out_width`, `out_height` (0 = canvas size), `scale_type`, `fps_div`, `venc_extra`
(flat list of encoder settings imported from Aitum), `canvas`, `audio_track`, `audio_bitrate_kbps`, `delay_sec`,
`delay_preserve`, `aitum:{mode,output}`.

### Aitum Stream Suite 1.2.4: where its settings live (inspected read-only, keys only)

- Not in `plugin_config\aitum-stream-suite` (does not exist). The file is `<profile folder>\aitum.json`, one
  per OBS profile (`%APPDATA%\obs-studio\basic\profiles\<ProfileDir>\aitum.json`, plus `.bak`). The plugin binary
  is under `C:\ProgramData\obs-studio\plugins\aitum-stream-suite\` and has no data folder of its own. The log line
  "[Aitum Stream Suite] Loaded configuration file" is that file. The file has NO version or schema field, so the
  gate is structural plus the plugin version (1.2.4).
- Top level keys of interest: `canvas[]` ({name, width, height, color, uuid, ...ui state}) and `outputs[]`.
  Everything else is UI state (dock layouts, hotkeys) and ignored.
- `outputs[]` common: `enabled`, `type` (`stream`, `backtrack`, `virtual_cam`; `record` expected), `name`, `canvas`
  (canvas NAME), `start_hotkey`, `stop_hotkey`, `expanded`, `audio_encoder` (`ffmpeg_aac`),
  `audio_encoder_settings.bitrate`, `audio_tracks` (bit mask, bit 0 = track 1; seen on backtrack/record),
  `vod_track` (-1), `video_encoders[]`.
- `video_encoders[0]`: `enabled`, `type`, `name`, `canvas`, `video_encoder` (encoder id, e.g. `obs_nvenc_h264_tex`),
  `video_encoder_settings` (flat map of that encoder's settings: `bitrate`, `max_bitrate`, `keyint_sec`, `rate_control`,
  `preset`, `profile`, `tune`, `multipass`, `bf`, `lookahead`, `adaptive_quantization`, `cqp`, `target_quality`, `device`,
  ...), `scale` (bool), `width`, `height`, `scale_type` (obs_scale_type number), `output_video_encoder` (name of another
  Aitum output whose encoder to share; "" = none).
- Seen as strings in `aitum-stream-suite.dll` but NOT present in any inspected file (the user never set them):
  `stream_server`, `stream_key`, `custom_delay`, `delay_sec`, `delay_preserve`, `audio_track`. The importer reads
  them if present (looked up on the output and on `video_encoders[0]`) and marks the mapping UNVERIFIED; an
  `audio_track` single number is NOT mapped (base unknown). The DLL also imports `obs_output_set_delay` and
  `obs_frontend_get_streaming_service`: a stream output without `stream_server` presumably falls back to the
  profile's OBS service, which is the MAIN stream's destination. We never read `service.json`; such an output
  imports encoder/canvas/track settings only and the user supplies URL + key in our dock.
- Inspected (counts only): 9 profiles, 2 `stream` outputs in 2 profiles (one `Vertical Stream` on canvas `Vertical`
  1080x1920, one `GAMEPLAY ONLY` 1920x1080), 0 with `stream_server`/`stream_key`.

Mapping (Aitum -> ours): `name`->name hint, `canvas`->canvas (the NAME; resolved at launch, missing = status), `video_encoder`->video_encoder, settings `bitrate`->bitrate_kbps (`max_bitrate` kept as
extra), `keyint_sec`->keyint_sec (0 = encoder default -> 2 s for x264), `preset`/`profile`/`rate_control`, every other
scalar setting -> `venc_extra`, `scale`+`width`+`height`->out_width/out_height, `scale_type`, `audio_encoder_settings.
bitrate`->audio_bitrate_kbps, lowest set bit of `audio_tracks`->audio_track, delay keys->delay_sec/delay_preserve,
`stream_server`/`stream_key`->url/key (follow: memory only; copy: key sealed). Not mapped, said in the UI:
`vod_track`, hotkeys, `output_video_encoder` sharing (becomes an own encoder), non-`stream` types.

Red team (hits): stream key in aitum.json is plaintext (never printed, tested with a fake key through every
message path); file missing / empty / huge (size cap 4 MB) / not JSON / `outputs` not an array / output not an
object / unknown `type` / negative or fractional numbers / hostile strings (control chars, 10 kB names) -> soft
fail with notes, no crash; Aitum rewrites the file while we read (read whole file once, parse from memory); canvas
renamed or gone; audio track outside 1-6; track name empty; encoder id not installed (checked at launch with
`obs_get_encoder_codec`); 8 outputs failing together (isolated per output); main stream stops while own-encoder
outputs on a non-main canvas run (they keep running, only shared ones stop).

## Earlier state (2026-10-13)

A native C++ plugin for OBS Studio 32.2.2 (Windows x64) that sends the one stream OBS already
encodes to several RTMP(S) destinations at once, with no platform API, no OAuth, no relay
server: each destination is a plain RTMP(S) URL plus the platform's stream key.

Two surfaces:

- **Dock** (for the streamer): destinations table with status dots, bitrate, dropped %, uptime,
  per-row start/stop, add/edit dialog, bottleneck banner, copy-overlay-URL button.
- **Overlay** (on stream): a browser-source page served from a loopback-only HTTP server inside
  the plugin. Shows each platform's icon plus the streamer handle; the icon is lit only while
  that destination is really connected and sending.

Status: see CHANGELOG.md. The core compiles. Norton deletes the freshly built test exe during
long runs (see Limits), so the C++ suite was verified in pieces, not as one green run.

## How to run and test

- JS tests (overlay, packaging): `node ~/.claude/skills/plan/ladder.js .tests/<feature>`
- C++ tests (core, no OBS needed): `powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Test`
  (needs MSVC; the ladder files under `.tests/core` call the same build and mark the layer
  SKIPPED, not passed, when no compiler is found)
- build plugin: `powershell -ExecutionPolicy Bypass -File scripts/build.ps1`  (VS 2022 generator; tree `<build root>\windows-x64`, import libs `<build root>\import-libs`; build root default `D:\builds\obs-multistream-chat`)
- install (only when the user runs it): `powershell -ExecutionPolicy Bypass -File scripts/install.ps1`
- package: `powershell -ExecutionPolicy Bypass -File scripts/package.ps1` -> `dist/obs-multistream-chat-<ver>-windows-x64.zip` + `.sha256`

## Modules

```
src/core/            portable C++17, no OBS, no Qt. Builds into static lib oms_core.
  clock.hpp          Clock interface, SystemClock, MockClock (tests)
  json.hpp/.cpp      small strict JSON value/parser/writer (depth + size limited)
  base64.hpp         base64 encode/decode
  secret.hpp/.cpp    SecretProtector interface; DPAPI impl (Windows); test impl (tests only)
  redact.hpp/.cpp    key redaction for any string that may reach a log, URL sanitising
  destination.hpp/.cpp  Destination model, platform presets, validation
  ring.hpp           fixed-capacity ring buffer
  stats.hpp/.cpp     StatsTracker: samples -> bitrate, dropped %, uptime
  health.hpp/.cpp    HealthMachine: idle/connecting/live/degraded/reconnecting/failed
  config.hpp/.cpp    schema v4 (3 -> 4 migrates), load/save, migrations, atomic write, DPAPI keys
  aitum.hpp/.cpp     read-only parser + mapper for Aitum's aitum.json, double-stream detection (0.3.0)
  plan.hpp/.cpp      shared/own video + audio-track + canvas decisions from an EnvSnapshot (0.3.0)
  fleet.hpp/.cpp     per-destination runtimes behind OutputBackend, events, ordering, Aitum follow (0.3.0)
  state_feed.hpp/.cpp  OverlayState snapshot -> JSON; StateHub (thread-safe, versioned)
  http_server.hpp/.cpp 127.0.0.1-only read-only server: static allowlist, state.json, SSE
src/obs/             OBS glue, links libobs, obs-frontend-api, Qt6
  plugin-main.cpp    module entry, wiring, frontend events
  output_manager.*   libobs OutputBackend: one obs_output per destination, canvases, encoders, signals -> Fleet
  dock.*             Qt6 dock + add/edit dialog (no Q_OBJECT; functor connects, like the rest
                     of production-suite, so there is no moc step)
data/locale/en-US.ini
data/overlay/        overlay.html, overlay.js, icons/*.svg   (served by http_server, allowlisted)
scripts/             build.ps1, install.ps1, package.ps1, fetch-sdks.ps1, make-import-lib.ps1
docs/                security.md, limits.md
.tests/<feature>/    ladder layers (node:test); C++ tests live in .tests/core/cpp/
```

## Data flow

```
 OBS main stream  --(frontend event STREAMING_STARTED)-->  OutputManager (UI thread)
 encoders (video+audio, already running)                         |
        |  obs_output_set_video_encoder / set_audio_encoder       |  per enabled destination:
        +-----------------------------------------------------+   |  rtmp_output + rtmp_custom service
                                                              v   v
                                         obs_output  --RTMP(S)-->  platform ingest
                                              |
              libobs threads: signals "start" "stop" -> lock-free-ish event queue (mutex+vector)
                                              |
 Qt timer (500 ms, UI thread): drain queue -> HealthMachine.on_*; sample stats
   (get_total_bytes, get_frames_dropped, get_total_frames) -> StatsTracker -> HealthMachine.on_sample
   Command{Start|Stop} returned by the machine -> OutputManager performs it
                                              |
                          StateHub.publish(OverlayState)   <-- only platform, handle, state
                                              |
              HttpServer thread(s): /state.json, /events (SSE)  --> OBS browser source (overlay.js)
                                              +--> dock refresh (reads the same snapshot + stats)
```

## Threading

- **UI thread** owns every `Runtime` (destination + HealthMachine + StatsTracker + obs handles)
  and the config. No locks inside the core state machines.
- **libobs signal threads** only push `OutputEvent{dest_id, kind, code, error}` onto a
  mutex-guarded vector. They never touch a Runtime.
- **HTTP threads** (accept thread + one short-lived thread per connection, max 8 connections,
  max 4 SSE) read only `StateHub` (mutex + condition variable, immutable JSON string per version).
- Frontend events (STREAMING_STARTED / STOPPING) arrive on the UI thread.
- Stream keys are decrypted only on the UI thread, at output-create time, and held in an
  `obs_data_t` owned by the service; the plain copy in `Destination::key` is wiped (`secure_zero`)
  after the service is built.

## Config file format

`%APPDATA%\obs-studio\plugin_config\obs-multistream-chat\config.json` (OBS's own per-user plugin
config location, via `obs_module_get_config_path`). **Current schema is 4** (see the 0.3.0 spec for the added
per-destination fields; `encoder` is `shared|own`). The example below shows the original schema 2 fields:

```json
{
  "schema": 2,
  "server": { "port": 0, "last_port": 51734 },
  "auto_start_with_stream": true,
  "destinations": [
    {
      "id": "d1", "name": "Twitch", "platform": "twitch",
      "url": "rtmps://ingest.global-contribute.live-video.net/app",
      "key_enc": "dpapi:v1:<base64 DPAPI blob>",
      "handle": "otiyyy", "enabled": true,
      "encoder": "shared", "bitrate_kbps": 0, "auto_start": true
    }
  ]
}
```

- `port` 0 = pick a random free port once and remember it in `last_port`; non-zero = use it,
  falling back to a random free port (and recording that) if it is taken.
- Schema 1 (hand-authored import format, may hold plaintext `key`): migrated on load to 2, plaintext
  keys encrypted, file rewritten in place (atomic rename). No plaintext backup is kept.
- Newer schema than this build knows: loaded read-only, never overwritten.
- Malformed file: moved to `config.json.bad-<ms>`, empty defaults used, the dock shows a notice.
- Writes are atomic (write temp, flush, rename over).

## Security notes (full text: docs/security.md)

- Keys at rest: Windows DPAPI (`CryptProtectData`, current-user scope, fixed app entropy).
  A different Windows user or another machine cannot decrypt them. Malware running as the same
  user can; DPAPI does not defend against that, and the doc says so.
- Keys never in: logs (all log lines go through `redact`), the overlay page, `/state.json`, SSE,
  error strings, the dock table (masked, reveal button only in the edit dialog, blank-to-keep).
- HTTP server: every GET/HEAD needs the per-install access key as `/k/<key>/` (since 2026-10-24, constant-time
  compare, 401 otherwise; an access key, not encryption: plain HTTP on loopback);
  binds `127.0.0.1` only (never `0.0.0.0`/`::`), peer address re-checked on accept,
  `Host` header must be `127.0.0.1:<port>` or `localhost:<port>` (DNS-rebinding defence), a present
  `Origin` must match, GET/HEAD only, fixed allowlist of paths (no file paths from the request, no
  directory traversal surface), no `Access-Control-Allow-Origin`, CSP + nosniff + no-store, header
  size and time limits, connection caps.
- Overlay carries only `{platform, handle, state}`. The handle is untrusted text: set with
  `textContent`, never `innerHTML`.
- No outbound network from the plugin except the RTMP(S) connections OBS itself makes.

## How it differs

Reference only; no code was copied from either.

| | obs-multi-rtmp (GPL-2.0) | Aitum Multistream | obs-multistream-chat (this) |
|---|---|---|---|
| Overlay for viewers | none | none known | browser source with icons and handles, lit only while genuinely sending |
| Stream keys at rest | plain text in OBS profile JSON | stored by the plugin/account (not verified here) | DPAPI-encrypted, per Windows user |
| Account / cloud | none | optional Aitum account features (not verified here) | none, by design |
| Encoder default | per target settings or "same as main" | its own mechanisms (not verified here) | shared main encoders by default (no extra NVENC sessions) |
| Health model | basic start/stop + retry | not verified here | explicit state machine: degraded, backoff + jitter, failed |
| Local server | none | not verified here | loopback-only, read-only, rebinding-checked |

obs-multi-rtmp and Aitum claims were not checked against their current releases in this task
(the project forbids copying; behaviour was not re-tested). Treat the table as design intent.

## Encoder sharing: verified answer for 32.2.2

Read from the 32.2.2 source (`libobs/obs-encoder.c`, `obs-output.c`):

- An encoder keeps a **list of callbacks** (`encoder->callbacks`); `obs_encoder_start_internal`
  only wires the video/audio source on the *first* callback. A second output attaching to an
  already-running encoder just joins the list. So sharing is real: `obs_output_set_video_encoder`
  / `obs_output_set_audio_encoder(out, enc, 0)` with the main output's encoders, then
  `obs_output_start`. No second NVENC session, no second encode.
- `obs_output_initialize_encoders` -> `obs_encoder_initialize` returns true for an already
  active encoder.
- A late joiner discards packets until the first video keyframe (`obs-output.c`, "if first video
  frame is not a keyframe, discard until received"), so a destination started or reconnected
  mid-stream goes live after at most one GOP (the keyframe interval, 2 s by default on most setups).
- Consequences: **one bitrate, resolution, fps, keyframe interval and codec for all shared
  outputs.** A per-destination bitrate cap is impossible in shared mode (the field is ignored and
  greyed out). Encoder settings can't change while any output is attached.
- Shared outputs can only start while the main stream is running (the frontend creates and
  owns the encoders at stream start). Per-destination Start in shared mode is therefore enabled
  only while the main stream is live; Stop works any time.
- OBS's own stream delay applies to the main output only; shared outputs are undelayed.
- x264 fallback mode: own `obs_x264` video encoder (`obs_video_encoder_create`) + own `ffmpeg_aac`
  audio encoder on mixer track 0, attached with `obs_encoder_set_video(obs_get_video())`; it can
  run without the main stream. Costs CPU, not GPU. Uses one extra raw-video scaler context per
  destination.

## Win conditions

1. Core logic proven by tests with a mock clock: transitions, backoff (cap + jitter bounds),
   key redaction, migration, malformed config, port collision, loopback-only server (rebinding
   attempt and non-loopback peer refused).
2. Overlay shows exactly the state the feed says: lit only on `live`, dim on `idle`/`failed`,
   pulse on `reconnecting`, `degraded` lit with warning; theme parameters work. Screenshot at
   true size per state.
3. Glue uses only verified 32.2.2 API (every called function grep-checked against the headers).
4. The project is "ready to build": a person with the Build Tools runs one command.

## Courses of action

- A (direct): reuse obs-frontend streaming output's encoders, one rtmp_output per destination.
- B (defensive): A + health machine + own reconnect (OBS's built-in is disabled per output so two
  retry loops never fight).
- C (asymmetric): spawn ffmpeg per destination from a local relay. Rejected: extra process, needs
  an RTMP/flv source, doubles memory copies, key on a command line (visible in process list).
- Chosen: B. Fallback if sharing misbehaves on real OBS: x264 mode only.

## Red team (hits and where they land)

| Hit | Mitigation / test |
|---|---|
| Key leaks into logs through an error string or URL | `redact` on every log call; test with fake keys, also keys embedded in URL paths |
| DNS rebinding to the local server | Host allowlist; test |
| Another local web page reading state | no CORS, Origin check, data is handles + state only |
| Non-loopback peer | bind 127.0.0.1 + `is_loopback_peer` check; test |
| Handle contains `<script>` | textContent; overlay test with hostile handle |
| Config corrupt / truncated mid-write | atomic rename; malformed test |
| Config from a newer version overwritten | read-only mode; test |
| Port taken | fallback to random free port, recorded; test |
| Reconnect storm hammering an ingest | capped exponential backoff + jitter, max attempts -> failed; test |
| Main stream stops while shared outputs run | stop ours on STREAMING_STOPPING first; hold our own encoder refs until released |
| User stops one destination, machine retries it | `stop requested` is terminal until Start; test |
| Time going backwards / clock jumps | monotonic clock only (`steady_clock`), MockClock tests |
| GPU saturated | shared encoders default; banner on skipped/lagged frames |
| OBS closes while threads run | server + manager stopped in `obs_module_unload` before Qt tears down |

## Chat (0.2.0): merged live chat without any platform API

```
 reader/index.js (node, hidden child of OBS, only started by the user)
   headless Chrome (throw-away profile, no login) --CDP/WebSocket--> one tab per source
   injected read-only engine (reader/lib/inject.js, isolated world) + selector pack (reader/adapters/<p>.json)
        | Runtime binding (page can not call it)           health: Source (reader/lib/source.js)
        v
   Poster (reader/lib/poster.js) --POST /chat/ingest, Bearer token from env--> HttpServer (127.0.0.1)
                                                                   ChatHub (src/core/chat_hub.*)
   sanitise -> dedupe (platform,id) -> filters -> rate limit -> reorder window (300 ms) -> ring (500)
                                                                        | SSE /chat/events (Last-Event-ID)
                                         data/overlay/chat.html + chat.js (browser source), dock Chat tab
```

- Core (C++, portable, tested): `chat_targets` (platforms, target validation, emote host allow-list),
  `chat_hub` (sanitiser, hub), `random` (token), config schema 3 (`chat` block, `build_reader_config`),
  http_server routes. Glue: `src/obs/reader_process.*` (CreateProcess hidden, job object kill-on-close,
  stdin pipe = stop signal, token in env only), `src/obs/chat_panel.*` (dock tab), plugin-main wiring.
- Reader (Node 22+, no dependencies): see the file list in `reader/`. Adapters are JSON packs validated at
  load; an invalid pack disables only its platform with a message.
- Config schema 3 adds `chat`: `start_with_obs`, `node_path`, `quiet_minutes`, `hide_commands`,
  `hide_bots`, `blocked_words`, `blocked_users`, `sources[{id,platform,target,enabled}]`. 2 -> 3 is a
  migration (status Migrated, saved as 3); an older plugin reading 3 treats it as too new (read-only).
  No login, cookie or token is ever stored; the reader config `chat-reader.json` has none either.
- Tests: `.tests/chat/` (L1 JS+fixtures, L2 units + each adapter in headless Chrome + C++ units via
  oms_tests, L3 reader module vs stub merge server + C++ socket tests, L4 reader process -> real C++
  server -> overlay in Chrome, screenshots). `.tests/chat/vectors/targets.json` is shared by the C++ and
  JS target validators. Fixtures are in `.tests/chat/fixtures/dom.js` and are NOT checked against live pages.

Deviations from the brief and why:

| Brief | Done | Why |
|---|---|---|
| message `{platform,id,author,text,emotes[],badges[],time}` | `parts[]` of text and emote items instead of `text` + `emotes[]`; `t` = when the reader saw it | keeps each emote where it stands in the sentence; the server derives plain text |
| reorder "by arrival with a small window" | arrival minus the reader-reported age (cap 5 s) inside a 300 ms hold | pages deliver in bursts; arrival alone would reorder them |
| ONE pinned archive, sha-256 in NOTICE | the transfer hit the 400 MB cap I set: the archive is a truncated prefix; selector files were extracted from it and pinned per file | repo is >400 MB (voice models); only per-file pins are real |
| SSN is MIT | its LICENSE is GPL-3.0 | read from the archive; nothing copied, recorded in NOTICE |
| simple-icons X in NOTICE with source | `x.svg` written from the known geometry, not downloaded | no network allowed; flagged in NOTICE to verify |
| headless Chrome "via plain WebSocket" | yes, `--remote-debugging-port=0` on loopback | per brief; the DevTools pipe would avoid a port and is a possible later hardening |
| `node reader\index.js` | resolved through `obs_module_file("reader/index.js")` = `<plugin>\data
eader\index.js` | the plugin data folder is where OBS finds module files; zip and install.ps1 copy `reader/` there |
| per-source status "needs login" etc. | states `starting reading quiet layout login error stopped` | `quiet` = container found, zero messages for `quiet_minutes` (default 5) |

Live check (coordinator, 2026-10-14, counts only): Twitch worked (79 lines, 19 authors, 0 duplicates in 150 s); Kick found the chat
list but no lines in 150 s (maybe offline, maybe selectors); YouTube (EU) stayed on `layout`, likely the consent page. Reaction: states
consent/disabled/replay/offline (pack `notes`/`noteStates`), consent cookie via CDP, `--diagnose`. Kick still needs a live look.

Live finding (coordinator): with the HeadlessChrome agent YouTube's `live_chat` popout returns a stub (11 elements); with Chrome's own
agent (no `Headless`) the full page. Reaction: `chat.user_agent` (default "chrome", `reader/lib/useragent.js`), state `stub`.

Known weak spots (also in docs/limits.md): all five platform packs unverified live; X rows are found by
avatar anchor (a guess); YouTube members-only/ad rows are not special-cased; Kick virtualised lists may
re-create rows (ids from `data-chat-entry` guard that; Twitch/TikTok/X ids are synthetic, so a row that
is re-rendered after 1.5 s would show twice); emote images load from the platforms' CDNs inside the
OBS browser source.

## Limits (verified)

| Limit | What it means |
|---|---|
| Compiler (found later on 2026-10-13) | `vswhere` reports Visual Studio 2022 Community (and a "18" install) with the C++ tools; the VS 2022 generator works. At the start of the task there was none; the user installed it meanwhile. Core compiles at /W4 with zero warnings. |
| Norton IDP.Generic | Norton flags freshly built unsigned test executables (heuristic) and deletes them mid-run. Not evaded. All build output lives under ONE central folder, `D:\builds\obs-multistream-chat\` (override `-BuildRoot` / `OMSC_BUILD_ROOT`; the repo `build/` only keeps the SDK download cache, archives only), so the user can exclude that one folder. Tradeoff: excluded means unscanned, so build only reviewed code there. Never read or list the rest of `D:\builds`. Test/DLL binaries carry PE version info. |
| OBS 32.2.2 (`obs64.exe` ProductVersion) | libobs API 32.2.2. Source tarball sha-256 `35d3cd0979d65664fada7119fdb612eca7c34b61a1623a330caec74bf72626c4` (GitHub archive of tag 32.2.2) |
| obs-deps prebuilt 2026-07-15 | pinned by 32.2.2's `CMakePresets.json`; windows-x64 sha-256 `6f90e9598fa10cff5ad23cdcfae49b87868c07bf896b02cd464582b4ce2f2ba9` (not needed to build this plugin; recorded for completeness) |
| Qt 6.11.1 (installed `Qt6Core.dll` 6.11.1.0) | obs-deps qt6 2026-07-15 windows-x64 zip contains Qt 6.11.1; sha-256 `7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579` **downloaded and verified equal to the pin** |
| Portable OBS for smoke test | `OBS-Studio-32.2.2-Windows-x64.zip` sha-256 `4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1` (GitHub release asset digest; not downloaded yet) |
| obs-plugintemplate (master, 3e7d7ac, 2025-12-09) | still pins OBS 31.1.1 / deps 2025-07-11, so it is stale for 32.x. Its layout idea (`libobs` + `obs-frontend-api` + Qt6 Widgets targets, `ENABLE_FRONTEND_API`, `ENABLE_QT`) is followed; the pins come from OBS 32.2.2's own `CMakePresets.json`. Its `buildspec.json` and cmake/ bootstrap are not vendored, because production-suite plugins build against a vendored SDK (below) and the user's other plugins already work that way. |
| SDK layout | like production-suite: `.obs-sdk/{libobs,frontend-api,lib}` and `.qt-sdk/{include,lib}` (gitignored). Headers are taken from the 32.2.2 tarball and the Qt zip above; import libs are generated from the installed DLLs by `scripts/make-import-lib.ps1` (dumpbin + lib, needs the Build Tools). `obsconfig.h` hand-written. |
| libobs version gate | one-directional: rejects plugins built against a newer libobs; 32.2.2 headers load on 32.2.2+. |
| Icons | simple-icons 16.35.0 (CC0-1.0 files): twitch, youtube, kick, facebook, tiktok. Trovo does not exist there (404) so Trovo/custom use the plugin's own neutral glyph. Brand names and marks remain their owners' trademarks (see NOTICE). |
| Platforms | no viewer counts, chat, titles, go-live buttons or health read-back from the platform: no API by design. "Live" means only "OBS has a working RTMP connection and is sending", not "the platform is showing you as live". |
| Windows tar.exe and bzip2 (2026-10-20) | `C:\Windows\System32\tar.exe` (bsdtar 3.5.2, Windows 10 19045) has no bzip2 inside: it runs `bzip2 -d` from PATH. With PATH = System32 only it fails ("Can't initialize filter; unable to run program bzip2 -d"), for reading and writing. GitHub runners and dev shells hide this (Git's `usr\bin\bzip2.exe`). Voice packs are `.tar.bz2`, so: `fetch-voices.js` appends Git's `usr\bin` to tar's PATH (`tarEnv`), and Setup undoes bzip2 with its own 7-Zip (`ArchiveExtraction=full`, Inno Setup 6.5+, `packaging/voice-extract.iss`) before tar.exe reads the plain `.tar`. Both verified with PATH = System32 only. |
| Windows PowerShell from pwsh (2026-10-20) | `powershell.exe` started from PowerShell 7 inherits pwsh's `PSModulePath` and loses `Get-FileHash` (fetch-sdks.ps1 fails). Clear `PSModulePath` before starting it. |
| Shell/Windows | only `RocketLeague.exe` was running at the start of this task; OBS was not running. The plugin must never be loaded into the user's real OBS by an agent. |

## Sources of truth

- Overlay contract (`/state.json` shape): `src/core/state_feed.cpp`; consumed by `data/overlay/overlay.js`.
- Platform presets (default URLs, labels, icon names): `src/core/destination.cpp`; mirrored by the
  icon file names in `data/overlay/icons/` (a test checks every preset has an icon or the neutral glyph).
- Locale keys: `data/locale/en-US.ini`.
- Version: `CMakeLists.txt` project VERSION (also read by package script and `plugin-main.cpp`).

## Known problems

- **2026-10-15** - 0.3.0 was never loaded into a running OBS (forbidden: OBS was live). Unverified in OBS: an encoder attached to an Aitum canvas's `video_t` (`obs_encoder_set_video(venc, obs_canvas_get_video(c))`), `obs_encoder_set_gpu_scale_type` on non-texture encoders, an extra AAC encoder on another mixer track next to a shared video encoder (A/V offset), hardware encoder ids on a second canvas, `obs_get_module("aitum-stream-suite")` matching Aitum's module name, the double-stream check seeing Aitum's outputs.
- **2026-10-15** - Aitum keys `stream_server`, `stream_key`, `custom_delay`, `delay_sec`, `delay_preserve`, `audio_track` exist as strings in aitum-stream-suite.dll 1.2.4 but in none of the author's 9 profile files, so their placement and base are unverified (importer reads them, marks them unverified). Only 1 stream output exists in the active profile (Vertical Stream, no server).
- **2026-10-15** - a canvas removed while a destination streams from it is not detected (the output keeps its own canvas reference); a renamed canvas counts as missing.
- **2026-10-15** - existing weakness kept: `Ctx` objects handed to libobs signals are intentionally leaked (a few dozen bytes per attempt).

- **2026-10-13** - full `oms_tests all` run could not complete: Norton (IDP.Generic) removed the exe mid-run. Verified: L2 73/73 in one process; each L3 test passes alone except one that failed once (431 reset, fixed with a drain, not re-run after the fix) and the LAN-probe test was rewritten after a stall and not re-run. The user must exclude `build/` in Norton and run `scripts/build.ps1 -Test`.
- **2026-10-13** - real platforms and a real OBS load are unverified (not allowed to start OBS or stream).
- **2026-10-13** - recommended RTMP URLs in the docs come from memory of the platforms' public help pages and could not be re-fetched in this task (only OBS/obs-deps/Qt/icon sources were allowed). Treat them as defaults to double-check on the platform's dashboard.

- **2026-10-14** - overlay L2 has no hostile-handle / path-trick test: the Norton agent hook blocked writing the first version of that test file. Covered only by L1 static checks (no innerHTML, textContent, fixed platform list) and one benign markup-characters test. Not evaded.

- **2026-10-14** - resolved: the DLL links. `scripts/build.ps1 -Test` built obs-multistream-chat.dll (350,720 bytes, sha-256 e558dae2e162b57fe5d618a8a1087317bee059942f805fb95d80c625988eb35c) into D:\builds\obs-multistream-chat\windows-x64; core suite 83/83 in one process. Still never loaded into OBS.
- **2026-10-14** - glue logic against a real OBS is unverified (not allowed to load OBS). Reasoned from 32.2.2 source: rtmp stop can join the connect thread, so every output stop runs on a worker thread; one output object per connection attempt; own reconnect (libobs reconnect off).

- **2026-10-14** - smoke test (portable OBS 32.2.2 + two local RTMP sinks) not done: it needs a visible OBS window and a built DLL. Needs the user's go-ahead and the import libs. Plan is in docs/limits.md-adjacent notes: portable zip sha-256 above, `--portable`, throwaway profile, `ffmpeg -listen 1` sinks on 127.0.0.1 only, closed afterwards.
- **2026-10-14** - chat (0.2.0): none of the five platform adapters has been run against a live page (forbidden while building); fixtures only. TikTok and X are experimental by design. Live checklist in README.
- **2026-10-14** - chat: the overlay's emote images are fetched by the OBS browser source from the platforms' CDNs (allow-listed by CSP and by the hub); this is the only network traffic the overlay itself makes.
- **2026-10-14** - chat: the reader's Chrome exposes a loopback DevTools port while it runs (brief asked for WebSocket CDP).
- **2026-10-14** - 0.2.0 built with `scripts\build.ps1 -Test`: core suite 122/122 in one process; obs-multistream-chat.dll 537,088 bytes, sha-256 b5344f902067e7fc2a572c9c9380c52983b8eee5ecc35a3f382036b456bc82aa; zip dist\obs-multistream-chat-0.2.0-windows-x64.zip sha-256 e6c79fc231e3255fc2acd0488e9513b2614658eab21bc9a34e55bc1cea5358c1. Still never loaded into OBS; the dock Chat tab and the child-process code compile and link but were not run.

- **2026-10-15** - 0.3.0 built with `scripts\build.ps1 -Test` (idle priority, 2 jobs): core suite 145/145 in one process (was 122); ladders core 190, glue 15, packaging 14 green; obs-multistream-chat.dll 670,208 bytes, sha-256 9679d68d1889cf16f052ebf055b24b2f2c92b9338a8bc6fa5ea713774c24b68f; zip dist\obs-multistream-chat-0.3.0-windows-x64.zip sha-256 e5742b6856bdc39f21e64d04c23f174128fa2d49c9d2093a8e85abbb24100703 (contains data
eader). Chat/overlay ladders were not re-run (untouched; they start headless Chrome). Gotcha: the ladder's `findExe` reuses a stale `tests-vs\Release\oms_tests.exe`; delete it after core changes.
- **2026-10-15** - read-only probe `oms_aitum_probe` run on the real active profile's aitum.json: status Ok, 2 canvases, 1 stream output mapped (encoder unset -> obs_x264/4500 default notes, canvas Vertical, no server/key); no key or server printed.

- **2026-10-15** - 0.3.1 built (idle, 2 jobs): core 145/145, glue ladder 20, packaging green; obs-multistream-chat.dll 679936 bytes, sha-256 a88afbc6dea2a387eecc3782d214ecd6f43b984164b2ba4e12a1c914ddef4423; zip dist\obs-multistream-chat-0.3.1-windows-x64.zip sha-256 99b741e3cedb7630bc043b126cbea55c26364ed15fb80c3996a14fc30d6a80c8. Exports obs_module_post_load (checked in the DLL). Still never loaded into OBS.

- **2026-10-20** - voice branch first run on Windows (MSVC, VS 2026): core 150/150; voice ladder 30 green incl. all 54 real Kokoro voices through `voice/index.js` + sherpa-onnx-node 1.13.8 (~0.8 s per line, 2.7-4.6 s of audio each, spectrogram checked); packaging, glue green. Package with Kokoro: zip 338 MB, setup 323 MB (contains `data\voices\kokoro-multi-lang-v1_0`). Still never loaded into OBS.
- **2026-10-20** - known: unpacking Kokoro in `fetch-voices.js` takes ~15 min on Windows (tar.exe pipes through an external bzip2 slowly). Works; slow on every cache-cold CI run.
- **2026-10-20** - known: `package.ps1` under Windows PowerShell 5.1 writes zip entries with backslashes (`obs-multistream-chat\data\...`). Windows Explorer copes; some other unzip tools do not. Pre-existing, not fixed.
- **2026-10-20** - fixed: `voices.generated.iss` started with a `;` comment and is #included inside `[Code]`, so every setup build with voices failed ("'BEGIN' expected", line 1). Same trap guarded in `voice-extract.iss` and by tests.
- **2026-10-20** - fixed: dead locale key `Col.Source` (unused since 0.4.1) failed the glue ladder.

- **2026-10-20** - known: `.tests/voice/l4-e2e.test.js` "unpack ... missing bzip2" fails when the ladder is started from
  PowerShell (no bzip2 on PATH there) and passes from Git Bash and on CI. Not changed in 0.5.0.
- **2026-10-20** - known: `.tests/packaging` "install.ps1 never touches the real ProgramData plugin folder" fails for an hour
  after a real install into ProgramData (it compares the folder's mtime with now). Expected after an install, not a regression.
- **2026-10-20** - unverified: the Audio tab's meters with real audio (the portable OBS had no sources routed), and
  `peak_to_db` / `db_to_bar` have no unit test (they live in the OBS glue).

- **2026-10-24** - known (pre-existing, not fixed): Chrome-based ladders are flaky on this PC under load. Seen on
  unchanged code: `reader/lib/chrome.js` `launch()` reads `DevToolsActivePort` while Chrome still holds it open
  (`EBUSY`), failing the chat L2 `before` hook and so every L2 case; the overlay helper's `Browser.close()` kills
  only the launcher process, so headless Chromium processes outlive the test and the node test process can hang
  idle. Workaround used: run layer files with `node --test --test-timeout`, TEMP pointed at a private folder, then
  stop only the Chromes whose profile lives there.
- **2026-10-24** - known: in read-only mode (config.json from a newer build) a missing access key cannot be saved,
  so a new one is made on every OBS start and the browser source URLs change each time.
- **2026-10-24** - known: the access key is visible in the dock's overlay URL fields and in OMSC_SHOTS pictures of
  a real install; anyone who sees the dock (for example a screen share) learns it.
- **2026-10-24** - known: an invalid `server.access_key` warning goes through the generic "destination(s) were
  skipped" notice path in plugin-main (pre-existing wording for all load warnings).

## Dropped approaches

- **2026-10-13** - ffmpeg relay per destination (course C) - dropped at design time, reasons above.
- **2026-10-13** - nlohmann/json or cpp-httplib as dependencies - dropped: network is restricted to OBS, obs-deps, Qt, icon sources, and a ~400-line parser plus a ~500-line server is smaller than the audit burden.

