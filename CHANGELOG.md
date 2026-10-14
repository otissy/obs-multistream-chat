# Changelog

## 0.2.0 (unreleased): merged chat without any platform API

- New: **Chat tab** in the dock, **/chat.html** browser source and a Node **reader** (`reader/`) that
  reads the public chat pages of Twitch, YouTube, Kick, TikTok (experimental) and X (experimental) in a
  headless Chrome of its own, no API, OAuth, token or login.
- New: selector packs (`reader/adapters/*.json`) so a layout change is a file edit, plus a health check
  per source (*Reading*, *Quiet*, *Page layout changed or login needed*, *Needs login*, *Error*).
- New: chat merge in the local server (`/chat/ingest`, `/chat/events`, `/chat/status.json`): token,
  sanitising, emote host allow-list, dedupe, ordering window, blocklist, bot/command filter, rate limits.
- New: icons for X (`x.svg`); config schema 3 (adds `chat`, migrated from 2 on first load).
- Changed: the existing config tests that parse `"schema":2` text now use `"schema":3` (a schema-2 file
  is `Migrated`, not `Ok`); a dedicated test covers the 2 -> 3 migration.
- New: page states *consent*, *disabled*, *replay* and *offline* (selector-pack `notes` + `noteStates`), YouTube consent cookie
  `SOCS` preset through CDP, `replayPaths`, Kick rows also matched by `[data-index]`, and `node reader\index.js --diagnose`
  (structure-only facts about a page).
- Not verified: any real platform page (not allowed while building); see README "Live checklist".

## Central build folder (unreleased)

All compiled output (CMake trees, test executables, the DLL, generated import libs, the package zip) now goes to `D:\builds\obs-multistream-chat\` by default, overridable with `-BuildRoot` or `OMSC_BUILD_ROOT`, so one antivirus exclusion covers it. `scripts/build.ps1`, `install.ps1`, `package.ps1`, `make-import-lib.ps1`, the CMake presets and the tests were updated.

## Rename (unreleased)

Project renamed from `obs-multistream` to **OBS Multistream Chat** (`obs-multistream-chat`): plugin id, DLL, dock id, install and config folders, CMake project/target and zip name. Rename only: there is no chat feature. `install.ps1` detects an old `obs-multistream` plugin folder and offers to remove it (`-RemoveOld`).

## 0.1.0 (unreleased)

First complete draft.

- Core (portable C++17): destination model and validation, DPAPI key sealing, redaction, stats ring
  buffers, health state machine (idle, connecting, live, degraded, reconnecting with capped
  exponential backoff and jitter, failed), config schema 2 with migration from schema 1, atomic
  saves, overlay state hub, loopback-only read-only HTTP/SSE server with Host/Origin checks.
- OBS glue: one `rtmp_output` per destination, shared main-stream encoders or own x264/AAC, libobs
  reconnect disabled in favour of the state machine, frontend start/stop events, Qt6 dock with
  add/edit dialog, bottleneck banner and overlay URL copy.
- Overlay: browser-source page with platform icons (Simple Icons, CC0) and handles; themes, layout,
  size, accent via URL query; dims when the feed goes silent.
- Scripts: `fetch-sdks`, `make-import-lib`, `build`, `install`, `package`.
- Tests: core C++ (L2 75, L3 12 incl. real sockets, L4 against the real server), overlay in headless
  Chrome (L1 8, L2 9, L3 3, L4 5 with true-size screenshots), glue static/compile checks, packaging.

Known gaps: not loaded into a real OBS; not streamed to any real platform; the plugin DLL links and packages but has
not been loaded into OBS; no overlay test for hostile handles
(see PROJECT.md).
