# Security

## What is stored, and where

| Data | Location | Protection |
|---|---|---|
| Destinations, handles, URLs, settings | `%APPDATA%\obs-studio\plugin_config\obs-multistream-chat\config.json` | plain JSON, no secrets in it |
| Stream keys | the same file, field `key_enc` | Windows DPAPI (`CryptProtectData`), current-user scope, app-specific entropy, token `dpapi:v1:<base64>` |
| Overlay port | `config.json` (`server.port`, `server.last_port`) | none needed |
| Nothing else | no registry keys, no startup entries, no scheduled tasks, no cloud | n/a |

The plugin makes no outbound connections of its own. Network traffic is the RTMP(S) connections OBS's
own output code opens to the URLs you entered, and the loopback overlay server.

## How the key is protected

- Encrypted with DPAPI as soon as it is saved. A different Windows user, or the same file on another
  PC, cannot decrypt it; the dock then asks you to re-enter it.
- The plaintext exists only (a) in the edit dialog while you type it, and (b) briefly in memory while
  a connection is being created. Both buffers are overwritten afterwards (best effort).
- The config writer refuses to write if any plaintext key is still attached to a destination.
- An old plaintext `key` in a hand-written or schema-1 file is encrypted on load and rewritten.
  No plaintext backup is kept.
- Keys are never put in: the OBS log (every log line passes through a redactor that also masks
  Twitch-style `live_<digits>_<token>` and YouTube-style `xxxx-xxxx-xxxx-xxxx` patterns and any known
  key), error messages, the dock table, `/state.json`, the SSE stream or the overlay page.
  URLs are logged only as `scheme://host/first-segment/...`.
- **Not protected against:** malware running as your Windows user (DPAPI does not stop that), and
  anyone who can read OBS process memory while a stream is starting. OBS itself also keeps the key in
  the RTMP service object while the output exists, as it does for its own stream key.

## The local server

Serves the overlay to the OBS browser source.

- Binds `127.0.0.1` only, never `0.0.0.0` or `::`; the bound address is read back from the OS in
  tests, and every accepted peer address is checked again.
- Read-only: `GET` and `HEAD` only; a fixed allowlist of paths (`/`, `/overlay.html`, `/overlay.js`,
  `/icons/*.svg` for known icons, `/state.json`, `/events`). Nothing is read from disk by request path,
  so there is no traversal surface.
- DNS-rebinding defence: the `Host` header must be exactly `127.0.0.1:<port>` or `localhost:<port>`.
  A present `Origin` must be the same origin; cross-site `Sec-Fetch-Site` is refused on the data paths.
- No CORS headers are ever sent, so other web pages cannot read the feed even if they can reach it.
  `CSP default-src 'none'` with same-origin script/img/connect only, `nosniff`, `no-store`.
- Limits: request headers 8 KiB, 5 s read timeout, 8 connections, 4 event streams.
- Content: platform id, handle and a state word per enabled destination. No URLs, keys, names,
  counters or IP addresses.
- The handle is user text; the page shows it with `textContent`, never as HTML.
- Another program on the same PC running as you can still read that feed (and could read your config
  file). The feed was chosen to hold nothing sensitive for that reason.

## Reporting

Please report problems privately to the repository owner (otissy) rather than in public issues.
