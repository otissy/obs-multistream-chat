# Aitum Stream Suite settings (since 0.3.0)

Aitum Stream Suite gives OBS extra canvases (for example a vertical 1080x1920 one) with their own scenes
and a long list of output options. In the author's use it crashes when it hosts more than two streams at
the same time. So this plugin can run the outputs (any number) while taking the *configuration* from
Aitum's output settings. Aitum itself is never asked to start, stop or change anything.

## What it does

Per destination, under **Aitum Stream Suite settings** in the add/edit dialog:

| Mode | Behaviour |
|---|---|
| **No** (default) | Everything comes from the destination's own fields. |
| **Copy once** | Reads the chosen Aitum output now and fills this destination's fields with it (canvas, encoder, bitrate, keyframes, preset, profile, rate control, every other encoder setting, rescale, audio bitrate and track, delay, and server and key if Aitum has them). After that the settings are yours; later changes in Aitum do not follow. |
| **Follow** | Re-reads Aitum's file **every time the destination starts** and uses those settings for that start. Nothing is written into this plugin's `config.json` except the output's name. The server and key Aitum supplies (if any) are held in memory only for the start, then wiped. |

**Import now** runs the copy immediately so you can review the result before pressing OK.

If Aitum's file is missing, unreadable, too big (over 4 MB), not JSON, or does not have the layout this
plugin was written against, the destination still starts with its own settings and the dock says why
(hover the status cell). It never fails the start because of Aitum.

## Where Aitum keeps its settings (inspected on Aitum Stream Suite 1.2.4)

Not in `plugin_config\aitum-stream-suite` (that folder does not exist). The file is per OBS profile:

```
%APPDATA%\obs-studio\basic\profiles\<profile folder>\aitum.json     (plus aitum.json.bak)
```

The plugin reads the file of the *currently active* profile (`obs_frontend_get_current_profile_path`).
The OBS log line `[Aitum Stream Suite] Loaded configuration file` is this file. The file has no version
or schema field, so this plugin checks the layout itself (a top-level `outputs` list and `canvas` list)
and also compares the loaded Aitum DLL's file version with `1.2.4`; another version shows a note in the
dialog and the mapping is still attempted, but unknown keys are ignored.

Keys read (keys only; values are never documented here):

- top level: `canvas[]` (`name`, `width`, `height`), `outputs[]`. Everything else in the file is dock layout
  and hotkey state and is ignored.
- each output: `type` (only `stream` is used; `backtrack`, `virtual_cam` and others are skipped),
  `name`, `enabled`, `canvas` (the canvas **name**), `audio_encoder`, `audio_encoder_settings.bitrate`,
  `audio_tracks` (bit mask, bit 0 = track 1), `vod_track` (not used), `video_encoders[0]`.
- `video_encoders[0]`: `video_encoder` (an OBS encoder id such as `obs_nvenc_h264_tex`),
  `video_encoder_settings` (a flat map: `bitrate`, `max_bitrate`, `keyint_sec`, `rate_control`, `preset`,
  `profile`, `tune`, `multipass`, `bf`, `lookahead`, `adaptive_quantization`, `cqp`, `target_quality`,
  `device`, ...), `scale`, `width`, `height`, `scale_type`, `output_video_encoder` (names another Aitum
  output whose encoder is shared).

**Unverified keys.** The strings `stream_server`, `stream_key`, `custom_delay`, `delay_sec`,
`delay_preserve` and `audio_track` are present in the Aitum binary but were not in any file inspected
(the author had not set a custom server in Aitum). The importer reads the first five if they exist (on the
output or on its first video encoder entry) and says "unverified" in the notes when it used them. A single
`audio_track` number is not mapped, because its base (0 or 1) is not known.

An Aitum stream output *without* its own server presumably falls back to the profile's main OBS service,
which is the main stream's destination. This plugin never reads `service.json` and never copies the main
stream's key: such an output imports encoder, canvas and track settings only, and you enter URL and key
in this plugin.

## What is not mapped

`vod_track`, hotkeys, `output_video_encoder` (sharing another Aitum output's encoder: here the destination
gets its own encoder with the same settings), every non-`stream` output type, nested settings values.

## Double streaming

If the same server **and** key are active in two places (two of this plugin's destinations, a destination
and an enabled Aitum output that has a server and key in the file, or a destination and any other output
that is currently streaming in OBS, including the main stream and Aitum's own outputs), the dock shows a
red warning naming the two places. It checks every five seconds and never stops anything on its own.
A warning needs both a server and a key on both sides; an Aitum output without a server cannot be
compared (nothing to compare), so be careful not to also start it in Aitum.

## What this plugin never does with Aitum

- never writes, renames or deletes `aitum.json` or any Aitum file;
- never starts, stops, enables or edits an Aitum output (it only inspects other outputs' state and service
  while checking for double streaming);
- never logs, prints or stores Aitum's key or server (log lines pass through the redactor; a test feeds a
  fake key through every message path and checks it never appears);
- in **Copy once** the key, if Aitum has one, is encrypted with Windows DPAPI immediately, like a key you
  type; in **Follow** it never touches disk.

## Not verified in a running OBS

All of this was developed without loading the plugin into a running OBS or opening Aitum. The importer is
verified against synthetic files (including malformed, unknown-layout and hostile ones) and read-only
against the structure of the author's real `aitum.json` (counts and mapped values, no key or server
printed). The libobs calls (canvas video, per-canvas encoders, audio encoders on a chosen mixer track) are
checked against the pinned 32.2.2 headers and link against the installed `obs.dll`, but whether OBS
accepts an encoder attached to an Aitum canvas's video output is only known once it runs.
