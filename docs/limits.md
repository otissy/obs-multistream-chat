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

## Chat reading (since 0.2.0)

- **Not an API.** The chat reader reads what a platform's public chat *web page* shows, in a hidden
  Chrome. That is the opposite of a supported integration: platforms may forbid automated reading in
  their terms of service, may block automated browsers, add captchas, require a login, or change
  their markup at any time. The author has not checked any platform's current terms. **You decide**
  whether to use it, per channel. The plugin sends nothing to any platform, logs in nowhere, stores no
  cookie, and only opens the chat pages of channels you add.
- **Never verified against a real page by the author.** The adapters (`reader\adapters\*.json`) were
  written from reading the open-source Social Stream Ninja sources (see NOTICE) and tested against
  local fixture pages. Fixtures prove the code reads the structure it was told about; they cannot
  prove a platform still renders that structure. Use the live checklist in the README.
- **Per platform:** Twitch (popout chat), YouTube (`live_chat` popout of a *live* video) and Kick
  (popout chat) are expected to work without an account. TikTok (live page) and X (broadcast page) are
  **experimental**: TikTok may need a login or show a human check; X usually needs a login and then
  shows *Needs login*. When the reader cannot read, the dock says so; it never pretends.
- **Order is approximate across platforms.** Lines are ordered by when the reader saw them, inside a
  300 ms window; pages that deliver in bursts can interleave slightly differently from what viewers
  saw. History on the page when the reader attaches is skipped on purpose.
- **Emotes** are shown only from each platform's own image host (Twitch, Kick, YouTube emoji, X emoji,
  TikTok); anything else becomes its name as text. Third-party emotes (7TV, BTTV, FFZ) are not read:
  the Chrome has no extensions.
- **No viewer counts, no sending, no moderation, no usernames or avatars:** only the display name shown
  on the page, the line, and a platform icon.
- **Needs** Node.js 22+ and Google Chrome (or Edge as a last resort). An unsigned compiled helper was
  avoided on purpose (antivirus), so this is plain `node` running JavaScript you can read.
- **Cost:** one headless Chrome (a few hundred MB while open) plus one tab per source, each loaded
  once. If you stream a heavy game, start the reader only when you need chat.
- **A reload happens only after a failed load or a crash**, with backoff (5 s, 10 s, 20 s, 40 s) and
  at most 4 tries per source. A page that loaded but shows no chat is reported, not reloaded.
- **Restart OBS or the browser source** after restarting the plugin: the page numbers its lines per
  plugin run.
- **YouTube in the EU:** a visitor may get a cookie-consent page before the chat. The reader presets the consent preference cookie
  (`SOCS=CAI` on `.youtube.com`, a value used by other open-source tools; if YouTube stops honouring it the state is
  *Blocked by the platform's cookie-consent page*) and does not click anything. The popout form
  `live_chat?is_popout=1&v=<id>` is used; `embed_domain` is only needed by the embedded chat iframe and is not sent.
  A video with chat off, no longer live, or members-only shows *Chat is disabled*; a finished video's chat replay shows *Replay only*.
- **Kick:** when the chat list is there but no row ever matches and the page says the channel is offline, the state is *Channel
  offline*; if the channel is live and the state is still *Quiet*, the row selectors are wrong: run `--diagnose` and fix
  `reader\adapters\kick.json`.

- **User agent (compatibility setting).** Chrome in headless mode announces itself as `HeadlessChrome/<version>`, and YouTube's
  `live_chat` popout answers that with an almost empty stub page (found in a live check: 11 elements, 103 characters; the same URL with a
  normal Chrome agent returns the full chat page). The setting `chat.user_agent` in the plugin's `config.json` (the reader sees it as
  `reader.userAgent`) is `"chrome"` (the default) or `"headless"`. `"chrome"` sends the installed Chrome's **own** user agent and client
  hints, with the version read from the browser (`Browser.getVersion`) and **only the word `Headless` removed**; `"headless"` leaves
  Chrome untouched. It applies to every platform. **That is all it does**: no `navigator.webdriver` patch, no plugin, WebGL, canvas or
  font spoofing, no other fingerprint change. It is a compatibility measure, not a disguise, and a site can still recognise automated
  reading. Platforms may treat automated reading of their pages as against their terms of service whatever the agent says; that was true
  before and this setting does not change it. You decide whether to use the reader at all.
- **Empty pages are reported, not guessed.** When a page loads but has fewer than 20 elements and no chat list the state is *Empty page
  served (automation detection or unavailable stream)*: the platform served an empty page, which can mean it dislikes the browser or that
  the stream does not exist or is not available to you.
