# Benchmarks (0.5.0)

Measured 2026-10-20 on one PC: AMD Ryzen 7 7800X3D (8 cores), 47.6 GB RAM, Windows 10 19045, Chromium 14x,
Node 26.11.1, OBS 32.2.2. CPU percentages are of the whole machine (all 8 cores = 100 %). Re-run them with
`node scripts/bench-reader.js` (local test pages, no internet) and add `--live @channel --twitch name` for real pages.

## OBS start-up

| | |
|---|---|
| `obs_init_module(obs-multistream-chat)` | **1.4 ms** of OBS's 1,067 ms `OBSInit` (portable OBS 32.2.2 start-up profiler) |
| Dock refresh | one 500 ms timer; the chat view polls the hub every 250 ms |
| Audio meters | only while the Audio tab is visible (`obs_add_raw_audio_callback` is removed when it hides) |

## Chat reader: low-memory mode (default since 0.5.0)

The reader opens each channel's public chat page in a hidden browser. Before 0.5.0 every channel had its own
browser context, so every page had its own renderer processes. Since 0.5.0 the pages share the browser's
(throw-away, logged-out) default context and renderers, and background throttling is off so no chat line is
late. Off with the environment variable `OMSC_READER_LOWMEM=0` set before OBS starts.

Local test pages, 20 s idle then 20 s at 10 chat lines per second, every line counted:

| Channels | Mode | Ready in | Browser memory | Processes | CPU idle / busy | Lines merged |
|---|---|---|---|---|---|---|
| 1 | old | 1.0 s | 576 MB | 10 | 2.6 % / 0.35 % | 185 / 185 |
| 1 | **low memory** | 0.9 s | **364 MB** | 7 | 0.16 % / 0.53 % | 185 / 185 |
| 3 | old | 4.1 s | 911 MB | 14 | 3.2 % / 0.55 % | 186 / 186 |
| 3 | **low memory** | 4.0 s | **386 MB** | 7 | 0.63 % / 0.93 % | 186 / 186 |
| 5 | old | 7.1 s | 1,243 MB | 18 | 3.9 % / 1.2 % | 185 / 185 |
| 5 | **low memory** | 6.8 s | **410 MB** | 8 | 0.98 % / 1.0 % | 187 / 187 |

The reader itself (Node) stays at 68-86 MB. "Ready in" grows ~1.5 s per channel on purpose: pages open one at a time.

Real pages (YouTube `@lofigirl`, live; Twitch `shroud`), 30 s steady:

| Mode | YouTube channel -> reading | Twitch -> reading | Browser memory | Processes | CPU |
|---|---|---|---|---|---|
| old | 4.1 s | 5.6 s | 1,215 MB | 15 | 2.22 % |
| **low memory** | **2.5 s** | 5.3 s | **829 MB** | 11 | **0.39 %** |

## Detecting that you are live

- **YouTube by channel (`@name`)**: the reader opens `youtube.com/@name/live` and reads the live video's id;
  2.5 s on a real channel above. While the channel is not live it looks again every **120 s**
  (`channelRecheckSec` in `reader/adapters/youtube.json`), so a stream that starts is picked up within ~2 minutes
  plus one page load. After a stream ends (chat off, replay or an empty page) it looks for the next one the same way.
- **Twitch, Kick**: the channel's popout chat works whether or not the channel is live; lines show up as soon as
  people type.
- **Start the reader "When I go live"** (Channels tab): nothing runs until your OBS stream starts.

## What was not measured

A low-end PC (no machine here); OBS's own CPU while streaming; the voice helper under load (one line takes about
0.8 s to synthesise on this PC, see the voice tests).
