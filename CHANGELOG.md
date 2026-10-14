# Changelog

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

Known gaps: not loaded into a real OBS; not streamed to any real platform; the plugin DLL is not
linked on the development machine yet (import libraries pending); no overlay test for hostile handles
(see PROJECT.md).
