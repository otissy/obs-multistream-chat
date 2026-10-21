'use strict';
// What a user may point the reader at: a bare channel / video / broadcast id, or a link on the
// platform's own host that reduces to one. Mirrors src/core/chat_targets.cpp rule for rule;
// .tests/chat/vectors/targets.json is checked against both implementations.

const PLATFORMS = ['twitch', 'youtube', 'kick', 'tiktok', 'x'];

function isAlnumAscii(c) { return (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122); }
function classOk(s, extra, lo, hi) {
  if (s.length < lo || s.length > hi) return false;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c >= 0x80) return false;
    if (!(isAlnumAscii(c) || extra.includes(s[i]))) return false;
  }
  return true;
}
const NAME_OK = {
  twitch: s => classOk(s, '_', 3, 25),
  kick: s => classOk(s, '_-', 3, 25),
  tiktok: s => classOk(s, '_.', 2, 24) && !s.endsWith('.'),
  youtube: s => classOk(s, '_-', 11, 11),
  x: s => classOk(s, '_-', 8, 40),
};
const HOSTS = {
  twitch: ['twitch.tv', 'www.twitch.tv', 'm.twitch.tv'],
  kick: ['kick.com', 'www.kick.com'],
  youtube: ['youtube.com', 'www.youtube.com', 'm.youtube.com'],
  tiktok: ['tiktok.com', 'www.tiktok.com'],
  x: ['x.com', 'www.x.com', 'twitter.com', 'www.twitter.com', 'mobile.twitter.com'],
};

function parseUrl(input) {
  let s = input;
  const ls = s.toLowerCase();
  if (ls.startsWith('https://')) s = s.slice(8);
  else if (ls.startsWith('http://')) return null;
  else if (s.includes('://')) return null;
  const hash = s.indexOf('#');
  if (hash >= 0) s = s.slice(0, hash);
  let query = '';
  const qm = s.indexOf('?');
  if (qm >= 0) { query = s.slice(qm + 1); s = s.slice(0, qm); }
  const slash = s.indexOf('/');
  const host = slash < 0 ? s : s.slice(0, slash);
  const path = slash < 0 ? '' : s.slice(slash);
  if (!host) return null;
  for (let i = 0; i < host.length; i++) {
    const c = host.charCodeAt(i);
    if (!(isAlnumAscii(c) || c === 46 || c === 45) || c >= 0x80) return null; // no userinfo, port, unicode
  }
  if (host.startsWith('.') || host.endsWith('.')) return null;
  for (let i = 0; i < path.length; i++) {
    const c = path.charCodeAt(i);
    if (c <= 0x20 || c === 0x7f || c === 0x5c || c === 0x25) return null;
  }
  return { host: host.toLowerCase(), segs: path.split('/').filter(x => x), query };
}
function queryParam(q, key) {
  for (const kv of q.split('&')) {
    const eq = kv.indexOf('=');
    if (eq >= 0 && kv.slice(0, eq) === key) return kv.slice(eq + 1);
  }
  return '';
}
// YouTube channel handle (without the @): 3-30 of a-z 0-9 _ - . , not starting or ending with a dot
function handleOk(h) { return classOk(h, '_-.', 3, 30) && !h.startsWith('.') && !h.endsWith('.'); }
// YouTube channel id: UC + 22 of A-Z a-z 0-9 _ -
function channelIdOk(s) { return s.length === 24 && s.startsWith('UC') && classOk(s, '_-', 24, 24); }
function looksLikeUrl(platform, s) {
  return s.includes('/') || (platform !== 'tiktok' && s.includes('.')) || s.includes(':') || s.includes('?');
}

// -> { ok:true, target } | { ok:false, error }
function normalizeTarget(platform, inputRaw) {
  if (!PLATFORMS.includes(platform)) return { ok: false, error: 'unknown chat platform' };
  const input = String(inputRaw == null ? '' : inputRaw).replace(/^[ \t]+|[ \t]+$/g, '');
  if (!input || input.length > 300) return { ok: false, error: 'enter a channel, video id or link' };
  for (let i = 0; i < input.length; i++) {
    const c = input.charCodeAt(i);
    if (c < 0x20 || c === 0x7f) return { ok: false, error: 'control characters are not allowed' };
  }
  let cand = '';
  // YouTube: a channel (@handle, stored as "@handle") is found live by the reader; an 11-character id is one video
  const ytChannel = h => (handleOk(h) ? { ok: true, target: '@' + h.toLowerCase() } : { ok: false, error: 'that is not a valid YouTube channel handle (@name)' });
  if (platform === 'youtube' && input.startsWith('@') && !input.includes('/')) return ytChannel(input.slice(1));
  if (!looksLikeUrl(platform, input)) {
    cand = input;
    if (platform === 'tiktok' && cand.startsWith('@')) cand = cand.slice(1);
    if (platform === 'youtube') {
      if (channelIdOk(cand)) return { ok: true, target: cand };
      // bare 11 characters with a digit, - or _ is a video id; anything else typed bare is a channel name
      // ("RoyalAscent" is 11 letters: a channel, not a video)
      if (!(NAME_OK.youtube(cand) && /[0-9_-]/.test(cand))) return ytChannel(cand);
    }
  } else {
    const u = parseUrl(input);
    if (!u) return { ok: false, error: 'that link is not acceptable (https links to the platform only)' };
    const hostOk = HOSTS[platform].includes(u.host);
    if (platform === 'twitch' || platform === 'kick') {
      if (!hostOk || !u.segs.length) return { ok: false, error: 'not a ' + platform + ' link' };
      cand = (u.segs[0] === 'popout' && u.segs.length >= 2) ? u.segs[1] : u.segs[0];
    } else if (platform === 'youtube') {
      if (u.host === 'youtu.be') {
        if (!u.segs.length) return { ok: false, error: 'no video id in that link' };
        cand = u.segs[0];
      } else if (hostOk && u.segs.length && u.segs[0].startsWith('@')) {
        return ytChannel(u.segs[0].slice(1)); // youtube.com/@name, /@name/live, /@name/streams ...
      } else if (hostOk && u.segs.length >= 2 && u.segs[0] === 'channel') {
        if (!channelIdOk(u.segs[1])) return { ok: false, error: 'that YouTube channel link has no valid channel id (UC...)' };
        return { ok: true, target: u.segs[1] }; // youtube.com/channel/UC...
      } else if (hostOk) {
        if (u.segs.length && (u.segs[0] === 'watch' || u.segs[0] === 'live_chat')) cand = queryParam(u.query, 'v');
        else if (u.segs.length >= 2 && (u.segs[0] === 'live' || u.segs[0] === 'embed')) cand = u.segs[1];
        if (!cand) return { ok: false, error: 'that YouTube link has no video id' };
      } else return { ok: false, error: 'not a youtube.com link' };
    } else if (platform === 'tiktok') {
      if (!hostOk || !u.segs.length || !u.segs[0].startsWith('@')) return { ok: false, error: 'not a tiktok.com/@user link' };
      cand = u.segs[0].slice(1);
    } else if (platform === 'x') {
      if (!hostOk || u.segs.length < 3 || u.segs[0] !== 'i' || u.segs[1] !== 'broadcasts') return { ok: false, error: 'not an x.com/i/broadcasts/... link' };
      cand = u.segs[2];
    }
  }
  if (!NAME_OK[platform](cand)) return { ok: false, error: 'that is not a valid ' + platform + ' name or id' };
  const lower = platform === 'twitch' || platform === 'kick' || platform === 'tiktok';
  return { ok: true, target: lower ? cand.toLowerCase() : cand };
}

// A YouTube target that names a channel (the live video is looked up), not one video.
function isChannelTarget(platform, target) {
  return platform === 'youtube' && typeof target === 'string' && (target.startsWith('@') || channelIdOk(target));
}

module.exports = { PLATFORMS, normalizeTarget, isChannelTarget };
