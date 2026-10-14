# OBS Multistream Chat

Plugin id / folder / DLL name: `obs-multistream-chat`. (Renamed from `obs-multistream`; there is no chat feature, the name is only a name.)

Send the one stream OBS already encodes to several RTMP(S) destinations at once, with a dock for you
and a browser-source overlay for your viewers. **No platform APIs, no OAuth, no relay service:** each
destination is a plain RTMP(S) URL plus that platform's stream key. Native C++ plugin for OBS Studio
32.2.2 (Windows x64). Licence GPL-2.0-or-later.

- **Dock (for you):** a table of destinations with a status dot (green live, amber degraded or
  reconnecting, red failed, grey idle), bitrate, dropped %, uptime, per-row start/stop, enable
  toggles, an add/edit dialog (key masked, reveal button), and a banner when your encoder or network
  is the bottleneck.
- **Overlay (on stream):** a browser source showing each platform's icon with your handle next to it.
  An icon lights up only while that destination is genuinely connected and sending, dims when idle,
  and pulses while reconnecting.
- **Shared encoders by default:** extra destinations reuse the main stream's encoders, so no extra
  NVENC session and no extra GPU load. A separate x264 encode per destination is optional.

Status: see [CHANGELOG.md](CHANGELOG.md). It has **not** been loaded into a real OBS or streamed to a
real platform yet; read [docs/limits.md](docs/limits.md) first.

## Install

You need either a release zip or a build from source.

**Release zip:** close OBS, extract `obs-multistream-chat-<version>-windows-x64.zip` so that the
`obs-multistream-chat` folder ends up in `C:\ProgramData\obs-studio\plugins\`. Check the zip against its
`.sha256` file first. Start OBS, then **View > Docks > OBS Multistream Chat**.

**From source** (needs Visual Studio Build Tools 2022 with *Desktop development with C++*; CMake is
already enough, you do not need the CMake component of the installer):

```
powershell -File scripts\build.ps1 -Test      # fetches pinned SDKs, builds, runs the core tests
powershell -File scripts\install.ps1          # only when YOU run it; refuses while OBS is open
powershell -File scripts\package.ps1          # dist\obs-multistream-chat-<ver>-windows-x64.zip + .sha256
```

`scripts\install.ps1 -Uninstall` removes it again. Nothing is installed, scheduled or added to startup
by anything else.

## Setup

1. Dock **OBS Multistream Chat** > **Add...**
2. Pick the platform. The URL is filled in where the platform has one fixed ingest; otherwise paste
   the URL from the platform's dashboard.
3. Paste the stream key (masked; **Reveal** shows what you typed). Type the handle you want shown on
   stream.
4. Leave **Encoder** on *Shared* unless you know you want a separate x264 encode.
5. Start your normal OBS stream. Destinations with *Start with the main stream* go live with it.

| Platform | Where to find the key | Default URL in the dialog |
|---|---|---|
| Twitch | Creator Dashboard > Settings > Stream > Primary Stream key | `rtmp://live.twitch.tv/app` (RTMPS also works) |
| YouTube | YouTube Studio > Create > Go live > Stream > Stream key | `rtmp://a.rtmp.youtube.com/live2` (or `rtmps://a.rtmps.youtube.com:443/live2`) |
| Facebook | Live Producer > Streaming software > Stream key (make it persistent) | `rtmps://live-api-s.facebook.com:443/rtmp/` |
| Kick | Creator Dashboard > Stream: copy the Stream URL **and** the key | none: account specific |
| TikTok | TikTok LIVE access / LIVE Studio: copy the Server URL **and** the key | none: account specific |
| Trovo / other | the provider's RTMP server and key | none |

These defaults come from the platforms' public help pages as remembered when this was written and could
not be re-checked offline: **confirm them on your dashboard.** Platforms change keys and ingests.

### Add the overlay to OBS

1. Dock > **Copy overlay URL**.
2. OBS: **Sources > + > Browser**, paste the URL, width 900 and height 120 (vertical: see below),
   untick "Shutdown source when not visible" and "Refresh browser when scene becomes active".
3. Tune it with query options appended to the URL:

| Option | Values | Default |
|---|---|---|
| `theme` | `dark`, `light`, `transparent` | `dark` |
| `layout` | `h` (row), `v` (column) | `h` |
| `size` | icon size in px, 16 to 256 | 48 |
| `accent` | `RRGGBB` accent for platforms without a brand colour | `7c5cff` |
| `names` | `1` show handles, `0` icons only | 1 |
| `hide_idle` | `1` hide anything not connected | 0 |

Example: `http://127.0.0.1:51734/overlay.html?theme=transparent&layout=v&size=64`. The port is chosen
once and remembered; if it is taken later a new one is used and the dock says so. If the plugin stops
answering, the overlay dims everything instead of showing a stale "live".

## Troubleshooting

- **Dock missing:** View > Docks > OBS Multistream Chat. Check OBS's log (Help > Log Files) for lines starting
  `[obs-multistream-chat]`. Keys never appear there.
- **Start is greyed out:** in *Shared* mode a destination can only start while the main stream is
  live. Start the main stream, or switch that destination to the x264 encoder.
- **Stays "Connecting" then "Reconnecting":** wrong URL, wrong key, blocked port (RTMP uses 1935,
  RTMPS 443), or the platform is not accepting the key yet. The tooltip on the status shows the reason.
  Retries back off from 1 s up to 60 s and stop after 8 failed tries ("Failed"); press Start to retry.
- **Amber "Degraded":** the output is connected but dropping at least 3 % of frames or sending under
  half the expected bitrate for 5 s. The banner says whether the network or the encoder/GPU is the cause.
- **Platform says offline though the icon is lit:** the icon means "OBS is connected and sending",
  nothing more. Platforms take a few seconds to show a stream and some need the stream key's
  "go live" step. No API means no confirmation from the platform (see docs/limits.md).
- **"Stream key cannot be decrypted":** keys are tied to your Windows user. Re-enter the key if the
  config was copied from another account or PC.
- **Keys with an older config:** a `config.json` from a newer version is opened read-only.
- **Broken config:** a damaged `config.json` is renamed `config.json.bad-<n>` and you start empty.

### Antivirus (Norton and others)

Freshly compiled, unsigned DLLs and test executables are commonly flagged by heuristic scanners
(Norton reports them as `IDP.Generic`) and may be removed mid-run. That is a false positive of the
"new unknown file" kind; nothing in this project is evading or hiding anything. All build output lives
under one folder, `build\`, so you can exclude just that folder. This is your decision and your action:

- To restore a blocked file in Norton: open Norton, **Security > History**, show the protection or
  quarantine history, select the item, and choose **Restore & Exclude** (menu names vary by version).
- To exclude a folder without a detection: Norton **Settings > Antivirus > Scans and Risks > Items to
  Exclude from Scans** (and *Exclude from Auto-Protect*), then add `...\obs-multistream-chat\build`.
- The built binaries carry version information (company, product, description, version) to make them
  easier to recognise. A release zip is not code-signed.

## More

- [docs/security.md](docs/security.md): what is stored where, how keys are protected, the local server.
- [docs/limits.md](docs/limits.md): what this deliberately does not do.
- [PROJECT.md](PROJECT.md): design, threading, config format, decisions, known problems.
- [NOTICE](NOTICE): third-party icons and trademarks.
