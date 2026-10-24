# Security

## What is stored, and where

| Data | Location | Protection |
|---|---|---|
| Destinations, handles, URLs, settings | `%APPDATA%\obs-studio\plugin_config\obs-multistream-chat\config.json` | plain JSON, no secrets in it |
| Stream keys | the same file, field `key_enc` | Windows DPAPI (`CryptProtectData`), current-user scope, app-specific entropy, token `dpapi:v1:<base64>` |
| Overlay port | `config.json` (`server.port`, `server.last_port`) | none needed |
| Overlay access key | `config.json` (`server.access_key`, 64 hex = 32 random bytes) and every browser source URL | plain text, like the URL in OBS's own scene file; readable by programs running as your Windows user |
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
- **Access key (unreleased, after 0.5.1).** Every page and stream is served only under `/k/<key>/` (for example
  `http://127.0.0.1:<port>/k/<key>/chat.html`). The key is 32 bytes from the Windows CSPRNG
  (`BCryptGenRandom`), made on first start and kept in `config.json`; the dock's *Copy URL* buttons include
  it. The key is compared in constant time. A request without it, with a wrong one, or with a prefix of it
  gets `401` and no data, before any route runs. The pages use relative URLs, so their script, icons and
  event stream carry the key too. `POST /chat/ingest` keeps its own per-run bearer token.
- What the key is and is not: it is an **access key, not encryption.** Traffic stays plain HTTP over the
  loopback interface; there is no TLS. Loopback traffic never leaves the PC, so nobody on the network can
  read it, and nobody on the network can connect at all (127.0.0.1 bind). The key stops other local
  programs and web pages that do not know it from reading the chat, the overlay state and the reader status.
  It does **not** stop a program running as your Windows user: it can read `config.json`, OBS's scene file or
  the process memory and so learn the key.
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
- Another program on the same PC running as you can still read that feed if it reads the key from your
  config file first. The feed was chosen to hold nothing sensitive for that reason.
- Not covered by the key: the chat reader's headless Chrome listens on a random loopback DevTools port while
  it runs (see below); any local program can find and drive that throw-away browser.

## Aitum Stream Suite import (since 0.3.0)

- The plugin only **reads** `aitum.json` of the active OBS profile; it never writes, renames or deletes it
  and never touches Aitum's outputs. See [aitum.md](aitum.md).
- An Aitum stream key (if the file holds one) is handled like a typed key: in *Copy once* it is encrypted with
  DPAPI at once; in *Follow* it is held in memory for the start only and wiped, and is never written to
  `config.json`, the log, the dock or an error message. The same applies to Aitum's server address.
- The double-streaming check compares key strings in memory (ours, Aitum's file, other active outputs'
  services) and reports only labels such as `"Twitch" (this plugin) and OBS main stream`.
- Files over 4 MB, non-JSON files and unexpected layouts are rejected with a generic message that contains
  no value from the file.

## Reporting

Please report problems privately to the repository owner (otissy) rather than in public issues.

## The chat reader (since 0.2.0)

**What the reader can do.** It runs as your Windows user, as `node reader\index.js`, started only by
you (the Start button, or the "Start it" choice on the Channels tab: when OBS starts, or when you go live). It starts a headless Chrome with a fresh
throw-away profile, opens the chat page of each channel you added (the URL is built from a fixed
template per platform; a link you type must be on that platform's own host, or it is refused), and
reads the chat list with a script that cannot click, type, scroll, navigate or send anything (a test
searches the script for every such API). It posts what it read to `127.0.0.1` only.

**Low-memory mode (default since 0.5.0).** All chat pages share the browser's one throw-away context and
its renderer processes, and Chrome's site isolation between them is off (`--disable-site-isolation-trials`,
no `site-per-process`). That cuts the browser from about 1.2 GB to about 0.4 GB for five channels
(docs/benchmarks.md). The tradeoff: a page could in theory read another chat page's memory through a
side channel. Every page is a public chat page in a logged-out profile, so there is nothing private to
read; set the environment variable `OMSC_READER_LOWMEM=0` before OBS starts for the old one context
and renderer per channel. A YouTube channel (`@name`) is looked up on `youtube.com/@name/live`, the same
host list as its chat page.

**What it cannot do.** It has no login, cookie, platform token or API key, and the plugin never stores
one. It cannot open a page outside the platform's host list: if a page redirects elsewhere, the tab is
closed and the source stops. Downloads, permission prompts, popups and dialogs are denied or closed.
It is not a sandbox: a malicious page that exploits Chrome could act as that Chrome; use a Chrome that is
kept up to date, and note that the reader opens only the channels you chose.

**Why loopback only.** Everything the reader sends goes to the plugin's server on `127.0.0.1`
(never `0.0.0.0`). The server keeps its existing protections (peer address re-checked, `Host` header must
be `127.0.0.1:<port>` or `localhost:<port>`, a present `Origin` must match) and adds:

- `POST /chat/ingest` needs `Authorization: Bearer <token>`. The token is 32 random bytes from the
  operating system, created each time you press Start, handed to the reader **through its environment
  only** (never the command line, so it is not visible in the process list), removed from the
  environment given to Chrome, its launch buffers are wiped after the process starts, scrubbed from the reader's
  log, and invalid as soon as the reader stops. Comparison is constant-time. A request without the
  right token is answered `401` **before its body is read**.
- Any `Origin` or `Sec-Fetch-Site` on an ingest request is refused: a web page, even with the token,
  cannot post. Only `application/json` with a `Content-Length` is accepted (no chunked uploads); the
  body cap is 256 KiB, 100 messages per batch.
- The reader's Chrome listens on a random loopback DevTools port for the lifetime of the reader (that
  is how it is driven). Another program running as you could connect to it; such a program could already
  do far more as you. The Chrome has no login to steal.

**Sanitising.** Everything from a page is untrusted text. The server reduces it to plain text: invalid
UTF-8 is replaced, tag-like text (`<script>`, `<img ...>`) is removed, control characters,
bidirectional overrides, zero-width and tag characters are dropped, runs of combining marks are
capped, whitespace is collapsed and length is capped (500 characters of text, 40 of name). Colours
must be `#rrggbb`. An emote is an image only if its URL is `https`, has no credentials or port, and its
host is on the allow-list of **that platform's** own image hosts; otherwise it is shown as its name. The
overlay page then sets everything with `textContent` (it contains no `innerHTML`), its Content-Security-
Policy only allows images from `'self'` and those hosts, and a test feeds it hostile messages directly
as a second layer.

**Limits and duplicates.** Per source: 20 lines/s sustained, burst 60; duplicates are dropped by
(platform, id); at most 16 sources; 4096 ids remembered; 500 lines kept for new overlays.

**Process ownership.** The reader and its Chrome are placed in a Windows job object that kills them
when OBS exits or crashes, and the plugin stops them when you press Stop. `chat-reader.log` holds states
and errors only, never chat text and never the token.

**The consent cookie.** For YouTube the adapter sets one cookie before the page loads, through the DevTools protocol
(`Network.setCookie`) in that source's own cookie-less browser context: `SOCS=CAI`, domain `.youtube.com`, secure. It is a *consent
preference* cookie (it records that the cookie dialog was answered), **not a login, session or identifier of you**. The pack
validator only accepts the names `SOCS` and `CONSENT`, values of letters, digits and `+_.-`, and a domain that is a parent of
the platform's own host; nothing else can be set. The context is discarded with the tab. The reader still never clicks.

**Diagnose mode.** `--diagnose` needs no token, posts nothing, and its output is structural by construction (a test plants a fake
author, text, emote and message id in a fixture and asserts none of them appears in the output).

**User agent.** With `chat.user_agent = "chrome"` (default) the reader tells the sites it reads the installed Chrome's own version
instead of `HeadlessChrome`: `Emulation.setUserAgentOverride` with the agent string and client-hint brands taken from
`Browser.getVersion`, only the `Headless` token removed, per source (per tab). It exists because YouTube serves an empty page to the
headless agent. Nothing else is altered (no `navigator.webdriver`, plugin, WebGL or fingerprint changes), the reader still has no login and
sends nothing, and a site can still detect automation. It is a compatibility setting that you can switch to `"headless"`; it is not
meant to evade a block, and the reader does not try to get past captchas, logins or consent pages beyond presetting the consent
preference cookie described above. Terms of service: see docs/limits.md.
