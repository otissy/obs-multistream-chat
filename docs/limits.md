# Limits

## By design: no platform APIs

There is no OAuth, no platform web API and no relay server, so the plugin **cannot** know:

- viewer counts, followers, chat or alerts;
- whether the platform shows you as live, or accepted the key beyond "the RTMP connection is up";
- titles, categories or go-live buttons.

"Live" in the dock and the overlay means: *OBS has an RTMP connection to that server and is sending
frames*. A platform can still reject or delay the stream after that point.

## Shared encoders (the default)

Verified in the OBS 32.2.2 source (see PROJECT.md): several outputs can attach to one running encoder.
Consequences:

- **One bitrate, resolution, frame rate, codec and keyframe interval for every shared output.** The
  per-destination bitrate field is ignored and greyed out in shared mode. Choose a main bitrate every
  platform accepts (Twitch's recommended range is lower than YouTube's, for example).
- A destination started or reconnected mid-stream starts delivering at the next keyframe (your keyframe
  interval, usually 2 s).
- Shared outputs only start while the main stream is live; they stop first when it stops.
- You cannot change encoder settings while any output is attached.
- OBS's stream delay applies to the main output only; shared outputs are not delayed.
- If you use Enhanced Broadcasting (multitrack video), only the first video encoder and the first audio
  track are shared.

## NVENC session limits

NVIDIA limits simultaneous NVENC sessions on some GPUs (the cap depends on the driver and card;
consumer cards historically allowed only a small number, raised in newer drivers). Shared mode adds
**zero** sessions, which is why it is the default on a GPU that is already busy with a game and OBS.
Each x264-mode destination adds a CPU encode and no NVENC session, but costs CPU and a raw-frame copy.

## Other

- Windows x64 and OBS 32.x only; built against libobs 32.2.2 (loads on equal or newer libobs of the
  same major API).
- At most 16 destinations.
- RTMP(S) only: no SRT, WHIP or HLS ingest.
- Keys are tied to your Windows user (DPAPI).
- Not tested yet against a real OBS or real platforms (see CHANGELOG).
