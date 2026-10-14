'use strict';
// HTML FIXTURES for the five platforms: the chat-list structure and one message row, as a page
// would render them. They are written from the selectors and structures found while reading
// Social Stream Ninja's sources (see NOTICE and PROJECT.md), NOT copied from a live page.
//
//   CHECKED AGAINST A LIVE PAGE:  none of them. No real platform page may be loaded in this
//   project's test runs. If a platform changes its markup, these fixtures stay green while the
//   real page breaks: the live checklist in README.md is the only real check.
//
// A segment is a string (text) or { emote: 'Name', url: 'https://...' }.
const esc = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');

const TW_EMOTE = 'https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/1.0';
const KICK_EMOTE = 'https://files.kick.com/emotes/37226/fullsize';
const YT_EMOJI = 'https://www.youtube.com/s/gaming/emoji/7ff574f2/emoji_u1f642.svg';
const X_EMOJI = 'https://abs-0.twimg.com/emoji/v2/svg/1f600.svg';
const TT_EMOTE = 'https://p16-webcast.tiktokcdn.com/webcast-va/emote_1.png~tplv-obj.image';

function twitch({ author, color = 'rgb(255, 69, 0)', segments, badges = [], login }) {
  const parts = segments.map(s => typeof s === 'string'
    ? `<span class="text-fragment" data-a-target="chat-message-text">${esc(s)}</span>`
    : `<div class="chat-line__message--emote-button" data-test-selector="emote-button"><span><img alt="${esc(s.emote)}" class="chat-image chat-line__message--emote" src="${esc(s.url)}"></span></div>`).join('');
  const b = badges.map(n => `<img alt="${esc(n)}" aria-label="${esc(n)} badge" class="chat-badge" src="https://static-cdn.jtvnw.net/badges/v1/3267646d-33f0-4b17-b3df-f923a41db1d0/1">`).join('');
  return `<div class="chat-line__message" data-a-target="chat-line-message" data-a-user="${esc(login || author.toLowerCase())}" tabindex="0">` +
    `<div class="chat-line__message-container"><div class="chat-line__username-container"><span class="chat-line__message--badges">${b}</span>` +
    `<span class="chat-line__username" role="button"><span><span class="chat-author__display-name" data-a-target="chat-message-username" style="color: ${color};">${esc(author)}</span></span></span></div>` +
    `<span aria-hidden="true">: </span><span data-a-target="chat-line-message-body" class="">${parts}</span></div></div>`;
}

function youtube({ id, author, segments, badges = [] }) {
  const parts = segments.map(s => typeof s === 'string' ? esc(s)
    : `<img class="emoji yt-formatted-string style-scope yt-live-chat-text-message-renderer" alt="${esc(s.emote)}" src="${esc(s.url)}" shared-tooltip-text=":x:">`).join('');
  const b = badges.map(t => `<yt-live-chat-author-badge-renderer class="style-scope yt-live-chat-author-chip" type="${esc(t)}"><div id="image"></div></yt-live-chat-author-badge-renderer>`).join('');
  return `<yt-live-chat-text-message-renderer class="style-scope yt-live-chat-item-list-renderer" id="${esc(id)}" author-type="">` +
    `<div id="author-photo"><img id="img" src="data:image/gif;base64,R0lGODlhAQABAAAAACw="></div>` +
    `<div id="content"><span id="timestamp">10:42 AM</span><yt-live-chat-author-chip><span id="author-name" dir="auto">${esc(author)}<span id="chip-badges">${b}</span></span></yt-live-chat-author-chip>` +
    `<span id="message" dir="auto">${parts}</span></div></yt-live-chat-text-message-renderer>`;
}

function kick({ id, author, color = 'rgb(0, 255, 163)', segments, badges = [] }) {
  const parts = segments.map(s => typeof s === 'string' ? esc(s)
    : `<div class="chat-emote-container"><img data-emote-name="${esc(s.emote)}" alt="${esc(s.emote)}" src="${esc(s.url)}"></div>`).join('');
  const b = badges.map(n => `<div class="badge-tooltip"><img src="https://files.kick.com/badges/${esc(n)}.svg" alt="${esc(n)}"></div>`).join('');
  return `<div data-chat-entry="${esc(id)}" data-index="1" class="mt-0.5"><div class="chat-entry"><div class="chat-message-identity flex">${b}` +
    `<button title="${esc(author)}" class="inline font-bold" data-prevent-expand style="color: ${color};">${esc(author)}</button></div>` +
    `<span class="font-bold text-white">: </span><span class="chat-entry-content font-normal">${parts}</span></div></div>`;
}

function tiktok({ author, segments, badges = [] }) {
  const parts = segments.map(s => typeof s === 'string' ? esc(s)
    : `<img class="ImgEmoteChatMessage" alt="${esc(s.emote)}" src="${esc(s.url)}">`).join('');
  const b = badges.map(() => '<img class="ImgBadgeChatMessage" alt="Moderator" src="https://p16-webcast.tiktokcdn.com/img/badge_mod.png">').join('');
  return `<div data-e2e="chat-message" class="css-1x2y3z-DivChatMessage e1abc"><div class="css-9q-DivUserInfo">${b}` +
    `<span data-e2e="message-owner-name">${esc(author)}</span></div><div class="css-7k-DivComment e2def"><span class="break-words align-middle">${parts}</span></div></div>`;
}

function x({ author, segments }) {
  const parts = segments.map(s => typeof s === 'string' ? esc(s)
    : `<img alt="${esc(s.emote)}" src="${esc(s.url)}">`).join('');
  return `<div class="css-175oi2r r-row"><div data-testid="UserAvatar-Container-${esc(author.toLowerCase())}" class="css-175oi2r"><img alt="" src="data:image/gif;base64,R0lGODlhAQABAAAAACw="></div>` +
    `<div class="css-175oi2r"><a href="/${esc(author.toLowerCase())}" role="link"><span>${esc(author)}</span></a> <span class="css-1jxf684">${parts}</span></div></div>`;
}

const BUILD = { twitch, youtube, kick, tiktok, x };

// Page skeletons. Each has two HISTORY rows that were on the page before the reader attached
// (the reader must NOT report them), and a feed script that appends rows the fake server queues.
function page(variant, key) {
  const feed = into => `<script>(function(){var n=0;function poll(){fetch('/__feed?key=${key}&since='+n,{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){var c=document.querySelector(${JSON.stringify(into)});if(c){j.items.forEach(function(h){c.insertAdjacentHTML('beforeend',h);});}n=j.next;}).catch(function(){});}setInterval(poll,80);})();</script>`;
  const hist = {
    twitch: [twitch({ author: 'OldTimer', segments: ['history one'] }), twitch({ author: 'OldTimer', segments: ['history two'] })],
    youtube: [youtube({ id: 'hist-1', author: 'OldTimer', segments: ['history one'] }), youtube({ id: 'hist-2', author: 'OldTimer', segments: ['history two'] })],
    kick: [kick({ id: 'hist-1', author: 'OldTimer', segments: ['history one'] }), kick({ id: 'hist-2', author: 'OldTimer', segments: ['history two'] })],
    tiktok: [tiktok({ author: 'oldtimer', segments: ['history one'] }), tiktok({ author: 'oldtimer', segments: ['history two'] })],
    x: [x({ author: 'OldTimer', segments: ['history one'] }), x({ author: 'OldTimer', segments: ['history two'] })],
  };
  const shell = (title, body) => `<!doctype html><html><head><meta charset="utf-8"><title>${title} (local fixture)</title></head><body>${body}</body></html>`;
  switch (variant) {
    case 'twitch':
      return shell('twitch popout fixture', `<div class="chat-room__content"><div class="chat-list--default"><div class="chat-scrollable-area__message-container" role="log">${hist.twitch.join('')}</div></div></div>${feed('.chat-scrollable-area__message-container')}`);
    case 'twitch-layout-changed': // same page after a redesign: none of the pack's selectors exist any more
      return shell('twitch layout changed', `<main class="stream-chat-v9"><ol class="msgs-v9" role="log"><li class="m9"><b class="u9">OldTimer</b><i class="t9">history</i></li></ol></main>`);
    case 'youtube':
      return shell('youtube live_chat fixture', `<yt-live-chat-app><yt-live-chat-renderer><yt-live-chat-item-list-renderer><div id="item-scroller"><div id="item-offset"><div id="items" class="style-scope yt-live-chat-item-list-renderer">${hist.youtube.join('')}</div></div></div></yt-live-chat-item-list-renderer></yt-live-chat-renderer></yt-live-chat-app>${feed('#items')}`);
    case 'kick':
      return shell('kick popout fixture', `<div id="chatroom"><div id="chatroom-messages" class="overflow-y-scroll">${hist.kick.join('')}</div></div>${feed('#chatroom-messages')}`);
    case 'kick-empty': // container present, no messages at all
      return shell('kick empty fixture', `<div id="chatroom"><div id="chatroom-messages" class="overflow-y-scroll"></div></div>`);
    case 'tiktok':
      return shell('tiktok live fixture', `<div data-e2e="live-chat-container"><div data-e2e="chat-room" class="css-1a-DivChatRoomContent e1">${hist.tiktok.join('')}</div></div>${feed('[data-e2e="chat-room"]')}`);
    case 'x':
      return shell('x broadcast fixture', `<div data-testid="chatContainer"><div class="css-175oi2r">${hist.x.join('')}</div></div>${feed('[data-testid="chatContainer"] > div')}`);
    case 'youtube-consent': // what a visitor in the EU may get instead of the chat
      return shell('youtube consent fixture', `<div><h1>Before you continue to YouTube</h1><form action="https://consent.youtube.com/save" method="post"><button type="button">Accept all</button></form></div>`);
    case 'youtube-disabled':
      return shell('youtube chat disabled fixture', `<yt-live-chat-app><yt-live-chat-renderer><div id="empty-state">Chat is disabled for this live stream.</div></yt-live-chat-renderer></yt-live-chat-app>`);
    case 'youtube-replay': // a replay renderer: rows exist but they are history
      return shell('youtube replay fixture', `<yt-live-chat-app><yt-live-chat-replay-renderer><yt-live-chat-item-list-renderer><div id="items">${hist.youtube.join('')}</div></yt-live-chat-item-list-renderer></yt-live-chat-replay-renderer></yt-live-chat-app>${feed('#items')}`);
    case 'kick-offline': // chat list present and empty, page says the channel is not live
      return shell('kick offline fixture', `<div id="chatroom"><div id="chatroom-messages" class="overflow-y-scroll"></div></div><aside><h2>The channel is offline</h2></aside>`);
    case 'kick-index': // rows that only carry data-index (virtualised list), no data-chat-entry
      return shell('kick data-index fixture', `<div id="chatroom"><div id="chatroom-messages" class="overflow-y-scroll">${hist.kick.map(h => h.replace(/data-chat-entry="[^"]*"/, '')).join('')}</div></div>${feed('#chatroom-messages')}`);
    case 'x-login': // what X shows without an account: no chat, a sign-in wall
      return shell('x login wall fixture', `<div><h1>Sign in to X</h1><a href="/i/flow/login" data-testid="loginButton">Log in</a></div>`);
    default:
      throw new Error('unknown fixture variant ' + variant);
  }
}

module.exports = { BUILD, page, esc, EMOTES: { TW_EMOTE, KICK_EMOTE, YT_EMOJI, X_EMOJI, TT_EMOTE } };
