# PROJECT.md - obs-multistream

The map of the ground. Read before any task; update at the end of every task.
Only verified facts go in "Limits"; everything else is marked as design or unverified.

## Current state (2026-10-13)

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
- C++ tests (core, no OBS needed): `powershell -File scripts/build.ps1 -Test`
  (needs MSVC; the ladder files under `.tests/core` call the same build and mark the layer
  SKIPPED, not passed, when no compiler is found)
- build plugin: `powershell -File scripts/build.ps1`  (preset `windows-x64`, VS 2022 generator)
- install (only when the user runs it): `powershell -File scripts/install.ps1`
- package: `powershell -File scripts/package.ps1` -> `dist/obs-multistream-<ver>-windows-x64.zip` + `.sha256`

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
  config.hpp/.cpp    schema v2, load/save, migration v1->v2, atomic write, DPAPI keys
  state_feed.hpp/.cpp  OverlayState snapshot -> JSON; StateHub (thread-safe, versioned)
  http_server.hpp/.cpp 127.0.0.1-only read-only server: static allowlist, state.json, SSE
src/obs/             OBS glue, links libobs, obs-frontend-api, Qt6
  plugin-main.cpp    module entry, wiring, frontend events
  output_manager.*   one obs_output per destination, encoder sharing, signals -> event queue
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

`%APPDATA%\obs-studio\plugin_config\obs-multistream\config.json` (OBS's own per-user plugin
config location, via `obs_module_get_config_path`). Schema version 2:

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
- HTTP server: binds `127.0.0.1` only (never `0.0.0.0`/`::`), peer address re-checked on accept,
  `Host` header must be `127.0.0.1:<port>` or `localhost:<port>` (DNS-rebinding defence), a present
  `Origin` must match, GET/HEAD only, fixed allowlist of paths (no file paths from the request, no
  directory traversal surface), no `Access-Control-Allow-Origin`, CSP + nosniff + no-store, header
  size and time limits, connection caps.
- Overlay carries only `{platform, handle, state}`. The handle is untrusted text: set with
  `textContent`, never `innerHTML`.
- No outbound network from the plugin except the RTMP(S) connections OBS itself makes.

## How it differs

Reference only; no code was copied from either.

| | obs-multi-rtmp (GPL-2.0) | Aitum Multistream | obs-multistream (this) |
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

## Limits (verified)

| Limit | What it means |
|---|---|
| Compiler (found later on 2026-10-13) | `vswhere` reports Visual Studio 2022 Community (and a "18" install) with the C++ tools; the VS 2022 generator works. At the start of the task there was none; the user installed it meanwhile. Core compiles at /W4 with zero warnings. |
| Norton IDP.Generic | Norton flags freshly built unsigned test executables (heuristic) and deletes them mid-run. Not evaded. All build output lives under ONE folder, `build/` (gitignored), so the user can exclude that folder. Test/DLL binaries carry PE version info. |
| OBS 32.2.2 (`obs64.exe` ProductVersion) | libobs API 32.2.2. Source tarball sha-256 `35d3cd0979d65664fada7119fdb612eca7c34b61a1623a330caec74bf72626c4` (GitHub archive of tag 32.2.2) |
| obs-deps prebuilt 2026-07-15 | pinned by 32.2.2's `CMakePresets.json`; windows-x64 sha-256 `6f90e9598fa10cff5ad23cdcfae49b87868c07bf896b02cd464582b4ce2f2ba9` (not needed to build this plugin; recorded for completeness) |
| Qt 6.11.1 (installed `Qt6Core.dll` 6.11.1.0) | obs-deps qt6 2026-07-15 windows-x64 zip contains Qt 6.11.1; sha-256 `7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579` **downloaded and verified equal to the pin** |
| Portable OBS for smoke test | `OBS-Studio-32.2.2-Windows-x64.zip` sha-256 `4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1` (GitHub release asset digest; not downloaded yet) |
| obs-plugintemplate (master, 3e7d7ac, 2025-12-09) | still pins OBS 31.1.1 / deps 2025-07-11, so it is stale for 32.x. Its layout idea (`libobs` + `obs-frontend-api` + Qt6 Widgets targets, `ENABLE_FRONTEND_API`, `ENABLE_QT`) is followed; the pins come from OBS 32.2.2's own `CMakePresets.json`. Its `buildspec.json` and cmake/ bootstrap are not vendored, because production-suite plugins build against a vendored SDK (below) and the user's other plugins already work that way. |
| SDK layout | like production-suite: `.obs-sdk/{libobs,frontend-api,lib}` and `.qt-sdk/{include,lib}` (gitignored). Headers are taken from the 32.2.2 tarball and the Qt zip above; import libs are generated from the installed DLLs by `scripts/make-import-lib.ps1` (dumpbin + lib, needs the Build Tools). `obsconfig.h` hand-written. |
| libobs version gate | one-directional: rejects plugins built against a newer libobs; 32.2.2 headers load on 32.2.2+. |
| Icons | simple-icons 16.35.0 (CC0-1.0 files): twitch, youtube, kick, facebook, tiktok. Trovo does not exist there (404) so Trovo/custom use the plugin's own neutral glyph. Brand names and marks remain their owners' trademarks (see NOTICE). |
| Platforms | no viewer counts, chat, titles, go-live buttons or health read-back from the platform: no API by design. "Live" means only "OBS has a working RTMP connection and is sending", not "the platform is showing you as live". |
| Shell/Windows | only `RocketLeague.exe` was running at the start of this task; OBS was not running. The plugin must never be loaded into the user's real OBS by an agent. |

## Sources of truth

- Overlay contract (`/state.json` shape): `src/core/state_feed.cpp`; consumed by `data/overlay/overlay.js`.
- Platform presets (default URLs, labels, icon names): `src/core/destination.cpp`; mirrored by the
  icon file names in `data/overlay/icons/` (a test checks every preset has an icon or the neutral glyph).
- Locale keys: `data/locale/en-US.ini`.
- Version: `CMakeLists.txt` project VERSION (also read by package script and `plugin-main.cpp`).

## Known problems

- **2026-10-13** - full `oms_tests all` run could not complete: Norton (IDP.Generic) removed the exe mid-run. Verified: L2 73/73 in one process; each L3 test passes alone except one that failed once (431 reset, fixed with a drain, not re-run after the fix) and the LAN-probe test was rewritten after a stall and not re-run. The user must exclude `build/` in Norton and run `scripts/build.ps1 -Test`.
- **2026-10-13** - real platforms and a real OBS load are unverified (not allowed to start OBS or stream).
- **2026-10-13** - recommended RTMP URLs in the docs come from memory of the platforms' public help pages and could not be re-fetched in this task (only OBS/obs-deps/Qt/icon sources were allowed). Treat them as defaults to double-check on the platform's dashboard.

- **2026-10-14** - overlay L2 has no hostile-handle / path-trick test: the Norton agent hook blocked writing the first version of that test file. Covered only by L1 static checks (no innerHTML, textContent, fixed platform list) and one benign markup-characters test. Not evaded.

- **2026-10-14** - plugin DLL not linked: generating `.obs-sdk/lib/obs.lib` and `obs-frontend-api.lib` needs `dumpbin /exports` on the installed obs.dll; the Norton agent hook blocked that command, so it was not retried or worked around. The plugin sources compile at /W4 with zero warnings against the real 32.2.2 and Qt 6.11.1 headers (object files only). To get the DLL: `powershell -File scripts/make-import-lib.ps1 ...` (see README), or let `scripts/build.ps1` do it.
- **2026-10-14** - glue logic against a real OBS is unverified (not allowed to load OBS). Reasoned from 32.2.2 source: rtmp stop can join the connect thread, so every output stop runs on a worker thread; one output object per connection attempt; own reconnect (libobs reconnect off).

## Dropped approaches

- **2026-10-13** - ffmpeg relay per destination (course C) - dropped at design time, reasons above.
- **2026-10-13** - nlohmann/json or cpp-httplib as dependencies - dropped: network is restricted to OBS, obs-deps, Qt, icon sources, and a ~400-line parser plus a ~500-line server is smaller than the audit burden.
