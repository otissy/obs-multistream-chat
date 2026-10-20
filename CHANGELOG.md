# Changelog

## 0.4.2 (unreleased): neural chat voices, offline, in 43 languages

- New: the voice reader speaks with **neural voices that run on this PC** (sherpa-onnx with Kokoro v1.0 and
  Piper voices) instead of Windows voices. The catalogue (`voice/catalogue.json`) holds 185 voices in 43
  languages: all 54 Kokoro voices (American and British English, Spanish, French, Hindi, Italian, Japanese,
  Brazilian Portuguese, Mandarin) and 1-6 Piper voices per locale, with English accents (Scottish, Northern
  and Southern English, Welsh, South Asian) and regional ones (Flemish, Mexican and Argentinian Spanish,
  European Portuguese). Only installed packs are offered.
- New: the models run in a **voice helper** (`voice/index.js`, Node, like the chat reader) that the plugin
  starts on the first line, in a kill-on-close job, with stdin/stdout pipes. A model error or a clashing
  `onnxruntime.dll` from another plugin cannot take OBS down. Its stderr goes to `voice-helper.log`.
- New: `scripts/fetch-voices.js` downloads each voice pack, checks it against a pinned SHA-256
  (`voice/pins.json`) and ships it only if its own model card names a licence that allows it (no
  non-commercial, no-derivatives, research-only, bare links or missing licences). Voices fine-tuned from
  Piper's Lessac voice ship by the maintainer's decision, and so does every other catalogue voice
  (`policy` in the catalogue; see NOTICE); voices that did not clear the check are labelled
  [non-commercial] or [licence unclear] in the dock. The pin is enforced for every pack.
- Fixed: 0.4.x was silent. Each message was spoken with an async SAPI `Speak` on a short-lived thread that
  released the voice straight after, cancelling the speech. The 13 voice names in the list were never applied.
- Changed: chat text is cleaned before it is spoken (links become "a link"; emoji, spam runs and control
  characters are removed; 300 characters at most). At most 5 lines wait; Preview cuts off what is playing.
- Fixed: voice packs ticked in Setup could never be unpacked on a normal PC. Windows' `tar.exe` cannot read
  `.tar.bz2` without a `bzip2` on PATH; Setup now undoes the bzip2 layer with its own 7-Zip, then tar reads
  the plain `.tar`. The build stops if the included Kokoro pack did not ship, instead of making a setup
  without voices.
- Fixed: a setup build with voices failed to compile (the generated voice list began with a `;` comment
  inside `[Code]`).
- Verified on Windows: all 54 Kokoro voices speak through the real helper; the plugin compiles with MSVC and
  the core suite passes 150/150. Not verified: the plugin has not run in OBS; Piper voices await the licence review.

## 0.4.0 (2026-10-18): live chat dock, platform logos, zero-touch auto-connect, vertical canvas, separate audio tracks

- New: **In-dock Live Chat feed** in the Chat tab: the streamer can now read their merged chat (Twitch, YouTube, Kick, TikTok, X) directly inside the OBS Studio dock without opening the browser-source overlay.
- New: **Platform logos in chat**: chat messages and sources table now display crisp vector logos for Twitch, Kick, YouTube, TikTok, and X instead of text tags.
- New: **Zero-touch channel auto-connect**: enter channel handle once in "My Channel" to automatically configure and enable Twitch, Kick, and TikTok. Automatically auto-syncs from configured destinations.
- New: **Automatic live stream detection & resilient lifecycle**: chat reader starts automatically when OBS or any destination goes live; gently remains in background standby (`waiting`) for offline channels so it immediately connects when broadcast starts.
- New: **Windows setup installer executable** (`obs-multistream-chat-0.4.0-windows-x64-setup.exe`) powered by Inno Setup in addition to portable `.zip` archives.
- New: **Vertical scenes and canvas**: canvas picker displays resolution and vertical tag (e.g. `Vertical (1080x1920 - Vertical)`), auto-selects vertical canvas when TikTok is chosen, and locks encoder to "own" with vertical bitrate defaults.
- New: **Separate audio tracks**: mixer tracks 1-6 with profile-defined track names from OBS (`[AdvOut] Track<N>Name`), audio bitrate control, and clear routing guidance for Advanced Audio Properties.
- New: **Stream key reveal and verification**: clicking Reveal in the edit dialog decrypts and reveals the saved DPAPI stream key so the streamer can verify their key at any time.
- Fixed: Resolved Aitum Stream Suite duplicate DLL conflict and locale file layout bug.

## 0.3.1 (unreleased): the dock was never registered in OBS

- Fixed: 0.1.0 to 0.3.0 registered the dock from `obs_module_load` and ignored the return value; in the real
  OBS 32.2.2 log the plugin logged nothing about a dock while obs-track-monitor and casterview (which register in
  `obs_module_post_load`) logged "dock registered". The dock is now created in `obs_module_post_load`.
- New: every outcome is logged (`dock registered`, `dock registration returned false`, `frontend main window not
  ready`), and a failed registration is retried once on `OBS_FRONTEND_EVENT_FINISHED_LOADING`.
- New: **Tools > OBS Multistream Chat** (`obs_frontend_add_tools_menu_qaction`) always opens the panel; if OBS
  refused the dock it opens in a floating window of its own.
- New: at-a-glance strip in the panel: a big Start/Stop all button and a summary line (live / total, total kbps,
  destinations needing attention, chat reader and sources reading). The Start all / Stop all buttons were replaced by it.
- Changed: the 500 ms tick timer no longer hangs off the dock (it ran only if the dock existed).
- Checked: OBS 32.2.2 ships Qt6Core/Gui/Widgets 6.11.1.0, the exact version the plugin is built against: no mismatch.
- Not verified: still never loaded into a running OBS.

## 0.3.0 (unreleased): canvas, audio track and Aitum output settings per destination

- New: **canvas** per destination ("" = main canvas, or any named OBS canvas via `obs_enum_canvases` /
  `obs_get_canvas_by_name`). A non-main canvas always has its own video encoder (id, bitrate, keyframe
  interval, preset, profile, rate control, rescale, frame-rate divisor, extra settings). A missing canvas gives
  a clear status and a retry, never a crash. The output holds a reference to its canvas for as long as it runs.
- New: **audio track** per destination (as the main stream, or mixer track 1 to 6). Shared video reuses the
  main stream's audio encoder when it already carries the track, else an own AAC encoder
  (`obs_audio_encoder_create(..., track - 1)`) with a selectable bitrate. Track names come from the profile.
- New: **stream delay** per destination (`obs_output_set_delay`).
- New: **Aitum Stream Suite settings** per destination: *Copy once* and *Follow* (re-read at each start), read-only,
  keys never logged or stored (`src/core/aitum.*`, [docs/aitum.md](docs/aitum.md)). Layout checked against 1.2.4.
- New: **double-streaming warning** (same server and key active in two places) in the dock.
- New: the per-destination orchestration moved out of the OBS glue into `src/core/fleet.*` behind an
  `OutputBackend` interface, and the encoder/canvas/track decisions into `src/core/plan.*`; both are unit tested
  with mocks (eight and twelve simultaneous mock outputs, error isolation, start/stop ordering, concurrent events).
- Changed: config **schema 4** (3 -> 4 migrates: `encoder: "x264"` becomes `"own"` with `obs_x264`, `veryfast`,
  keyframe 2 s, i.e. what 0.2.0 did; saved as 4; older plugins treat 4 as too new and read it read-only).
  Tests that wrote `"schema":3` now write 4 and `"x264"` became `"own"` (named in the tests' commit).
- Changed: the dock's encoder choice is now *Shared* or *Own encoder*; the table has a *Canvas / track* column.
- Not verified: never loaded into a running OBS; hardware encoders on an Aitum canvas's video output are untested.

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
- New: `chat.user_agent` (`"chrome"` default | `"headless"`): the installed Chrome's own user agent minus `Headless` (YouTube serves an
  empty stub page to the headless agent); state `stub` for an empty page; `--diagnose` prints the agent mode and the stub guess.
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
