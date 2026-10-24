# Research: a native Windows camera relay (one camera, many consumers)

Task (2026-10-24, research only, nothing built): "make r relay camera app native windows so we can use
multiple cameras everywhere". No relay camera app exists in this repo (searched for relay, camera,
webcam and dshow; the hits are about RTMP relays). "r relay" is read here as "a relay": one process owns
each physical camera and every other app reads copies of its frames.

**Ground truth for this PC:** Windows 10 Pro 22H2 (build 19045). That rules out the two newest Windows
routes below, which both need Windows 11.

## Options

Effort figures are rough estimates for one developer, not measured.

| Option | What it is | Effort | Risks |
|---|---|---|---|
| A. OBS as the relay (no new code) | One OBS source owns the camera; reuse it across scenes; OBS Virtual Camera exposes one picture (the program, or one chosen scene/source) to other apps | none | OBS's virtual camera is **one** device and shows one picture, so it does not give "many cameras" outside OBS; OBS must be running |
| B. Windows 11 multi-app camera | OS setting "Allow multiple apps to use camera at the same time" (from build 26100.3321, per a third-party guide) | none | **not available on Windows 10**; per the same guide, camera settings then move to Windows Settings only; unknown whether OBS's capture uses the shared stream |
| C. Native relay with Media Foundation virtual cameras | Small Windows service/app opens each camera once (Media Foundation source reader) and publishes one virtual camera per physical camera with `MFCreateVirtualCamera` | medium–large: about 2–4 weeks for a solid C++ app (capture, frame sharing, a COM media source DLL, installer) | **needs Windows 11 build 22000+** (Microsoft docs); the media source must be a registered COM DLL that runs inside Frame Server; signing and AV (Norton already flags unsigned builds here) |
| D. Native relay with DirectShow virtual cameras | Same as C but each output is a DirectShow source filter (how OBS's own virtual camera and older tools work) | large: about 3–5 weeks; one filter CLSID per output camera, shared-memory ring per camera | works on Windows 10, but DirectShow-only: apps that use only Media Foundation/WinRT (Windows Camera, newer Teams/browser builds) may not see it; per-camera registration and cleanup |
| E. Network relay (NDI via DistroAV, or Spout2) | Sender per camera, receivers in each app | low (install plugins) | NDI: extra CPU/GPU per stream (one forum report), LAN traffic, licence terms; Spout2: GPU textures on one PC only and only apps with Spout support; neither shows up as a "camera" for ordinary apps |

## Recommendation

1. Decide the scope first (open questions below); for OBS-only use, option A already covers it.
2. For "every app sees every camera" **on this Windows 10 PC**, only D works, and it is the most work.
   If moving to Windows 11 is acceptable, C is the cleaner modern route, and B may make a relay
   unnecessary. Test B first on a Windows 11 machine: it costs nothing.
3. Whatever is built: the relay must own the camera exclusively and hand out frames through shared
   memory (no re-encode), cap resolution/fps per output, and never be able to grab a camera that the
   live OBS is using (start order matters: whoever opens first wins on Windows 10).

## Open questions for otiyyy

- Which apps need the extra cameras: OBS only, Grace 11, browsers, Discord/Teams?
- Same PC only, or other PCs too? Other PCs means NDI (E).
- Is upgrading the streaming PC to Windows 11 an option? It decides between C and D.
- What is "r relay"? If it names an existing app (for example a phone-camera relay), its source and
  licence change the plan.

## Sources (read 2026-10-24; not tested on this PC)

- Microsoft Learn, `MFCreateVirtualCamera` / `IMFVirtualCamera` (minimum client Windows build 22000;
  only `MFVirtualCameraType_SoftwareCameraSource` supported):
  https://learn.microsoft.com/en-us/windows/win32/api/mfvirtualcamera/nf-mfvirtualcamera-mfcreatevirtualcamera
- Multi-app camera on Windows 11 (third-party guide, build 26100.3321 / KB5052093):
  https://pureinfotech.com/allow-multiple-apps-use-camera-windows-11/
- DistroAV (NDI for OBS): https://github.com/DistroAV/DistroAV
- obs-spout2-plugin: https://github.com/Off-World-Live/obs-spout2-plugin
- Earlier research notes on the Notion task page (2026-10-20).
