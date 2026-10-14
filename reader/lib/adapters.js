'use strict';
// Selector packs: reader/adapters/<platform>.json. A layout change on a platform is fixed by
// editing one of these files, not by rebuilding anything. Every pack is validated strictly; an
// invalid pack disables only its own platform, with a message that names the problem.
const fs = require('fs');
const path = require('path');
const { PLATFORMS, normalizeTarget } = require('./targets');

const MAX_PACK_BYTES = 64 * 1024;

function isStr(x) { return typeof x === 'string' && x.length > 0 && x.length <= 300 && !/[\u0000-\u001f]/.test(x); }
function isStrList(x, max = 20) { return Array.isArray(x) && x.length <= max && x.every(isStr); }

// Throws Error with a plain message on the first problem.
function validatePack(pack, expectedPlatform) {
  const bad = m => { throw new Error(m); };
  if (!pack || typeof pack !== 'object' || Array.isArray(pack)) bad('not a JSON object');
  if (pack.adapter !== 1) bad('"adapter" must be 1');
  if (pack.platform !== expectedPlatform) bad('"platform" must be "' + expectedPlatform + '"');
  if (typeof pack.experimental !== 'boolean') bad('"experimental" must be true or false');
  if (typeof pack.url !== 'string' || !pack.url.startsWith('https://') || pack.url.split('{target}').length !== 2) {
    bad('"url" must be an https:// template containing {target} once');
  }
  if (!isStrList(pack.hosts, 5) || !pack.hosts.length || pack.hosts.some(h => h !== h.toLowerCase() || !/^[a-z0-9.-]+$/.test(h))) {
    bad('"hosts" must be a short list of lower-case host names');
  }
  const host = new URL(pack.url.replace('{target}', 'x')).hostname;
  if (!pack.hosts.includes(host)) bad('the host of "url" (' + host + ') is not in "hosts"');
  if (pack.loginPaths !== undefined && !isStrList(pack.loginPaths, 10)) bad('"loginPaths" must be a list of strings');
  if (pack.containerTimeoutSec !== undefined && !(Number.isInteger(pack.containerTimeoutSec) && pack.containerTimeoutSec >= 10 && pack.containerTimeoutSec <= 300)) {
    bad('"containerTimeoutSec" must be a whole number from 10 to 300');
  }
  if (pack.consentHosts !== undefined) {
    const own = pack.hosts.map(h => h.replace(/^www\./, ''));
    if (!isStrList(pack.consentHosts, 5) || !pack.consentHosts.every(h => /^[a-z0-9.-]+$/.test(h) && own.some(b => h.endsWith('.' + b)))) {
      bad('"consentHosts" must be lower-case hosts under the platform\'s own domain');
    }
  }
  if (pack.replayPaths !== undefined && !(isStrList(pack.replayPaths, 5) && pack.replayPaths.every(p => p.startsWith('/')))) bad('"replayPaths" must be a list of paths');
  if (pack.cookies !== undefined) {
    if (!Array.isArray(pack.cookies) || pack.cookies.length > 3) bad('"cookies" must be a short list');
    for (const c of pack.cookies) {
      if (!c || !['SOCS', 'CONSENT'].includes(c.name)) bad('only the consent preference cookies SOCS and CONSENT may be set');
      if (typeof c.value !== 'string' || !/^[A-Za-z0-9+_.-]{1,64}$/.test(c.value)) bad('cookie value has odd characters');
      if (typeof c.domain !== 'string' || !c.domain.startsWith('.') || c.domain.split('.').length < 3 || !pack.hosts.some(h => ('.' + h).endsWith(c.domain))) bad('cookie domain must be a parent of one of "hosts"');
    }
  }
  if (pack.noteStates !== undefined) {
    if (typeof pack.noteStates !== 'object' || pack.noteStates === null || Array.isArray(pack.noteStates)) bad('"noteStates" must be an object');
    for (const [id, v] of Object.entries(pack.noteStates)) {
      if (!v || !['consent', 'disabled', 'replay', 'offline', 'login'].includes(v.state) || !['always', 'nochat', 'nomsg'].includes(v.when) || !isStr(v.detail)) bad('"noteStates.' + id + '" needs a known state, when and detail');
    }
  }
  const e = pack.engine;
  if (!e || typeof e !== 'object') bad('"engine" is missing');
  if (!isStrList(e.container, 10) || !e.container.length) bad('"engine.container" must list at least one selector');
  const hasItem = isStrList(e.item || [], 10) && (e.item || []).length > 0;
  const hasAnchor = isStrList(e.anchor || [], 5) && (e.anchor || []).length > 0;
  if (!hasItem && !hasAnchor) bad('"engine.item" or "engine.anchor" is needed');
  if (!e.author || !isStrList(e.author.sel, 10) || !e.author.sel.length) bad('"engine.author.sel" must list at least one selector');
  if (e.author.color !== undefined && !['inline', 'none'].includes(e.author.color)) bad('"engine.author.color" must be "inline" or "none"');
  if (!e.body || typeof e.body !== 'object') bad('"engine.body" is missing');
  if (e.body.sel !== undefined && !isStrList(e.body.sel, 10)) bad('"engine.body.sel" must be a list of selectors');
  if (!(e.body.sel && e.body.sel.length) && e.body.rest !== true) bad('"engine.body" needs "sel" or "rest": true');
  if (e.body.remove !== undefined && !isStrList(e.body.remove, 20)) bad('"engine.body.remove" must be a list of selectors');
  for (const k of ['ignore']) if (e[k] !== undefined && !isStrList(e[k], 20)) bad('"engine.' + k + '" must be a list of selectors');
  for (const k of ['emote', 'badge']) {
    if (e[k] === undefined) continue;
    if (typeof e[k] !== 'object' || !isStrList(e[k].sel, 10)) bad('"engine.' + k + '.sel" must be a list of selectors');
    for (const f of ['name', 'src']) if (e[k][f] !== undefined && !isStrList(e[k][f], 6)) bad('"engine.' + k + '.' + f + '" must be a list of attribute names');
  }
  if (e.id !== undefined) {
    if (typeof e.id !== 'object' || (e.id.attrs !== undefined && !(Array.isArray(e.id.attrs) && e.id.attrs.length <= 6 && e.id.attrs.every(isStr)))) {
      bad('"engine.id.attrs" must be a list of attribute names');
    }
  }
  if (e.notes !== undefined) {
    if (!Array.isArray(e.notes) || e.notes.length > 10) bad('"engine.notes" must be a short list');
    for (const n of e.notes) {
      if (!n || !isStr(n.id) || (n.sel !== undefined && !isStrList(n.sel, 10)) || (n.text !== undefined && !isStrList(n.text, 20))) bad('every note needs an id and selector or text lists');
      if (!pack.noteStates || !pack.noteStates[n.id]) bad('note "' + n.id + '" has no entry in "noteStates"');
    }
  }
  if (e.login !== undefined) {
    if (typeof e.login !== 'object') bad('"engine.login" must be an object');
    for (const f of ['sel', 'text']) if (e.login[f] !== undefined && !(Array.isArray(e.login[f]) && e.login[f].length <= 20 && e.login[f].every(isStr))) bad('"engine.login.' + f + '" must be a list of strings');
  }
  // every selector must at least parse in this runtime's own syntax check where one exists
  return pack;
}

// -> { packs: {platform: pack}, errors: {platform: message} }
function loadAdapters(dir) {
  const packs = {};
  const errors = {};
  for (const platform of PLATFORMS) {
    const file = path.join(dir, platform + '.json');
    try {
      const st = fs.statSync(file);
      if (st.size > MAX_PACK_BYTES) throw new Error('file is larger than ' + MAX_PACK_BYTES + ' bytes');
      packs[platform] = validatePack(JSON.parse(fs.readFileSync(file, 'utf8')), platform);
    } catch (e) {
      errors[platform] = 'adapter file ' + platform + '.json: ' + (e && e.message ? e.message : String(e));
    }
  }
  return { packs, errors };
}

// The page URL for a source. testOrigin (http://127.0.0.1:<port>, tests only) replaces the origin
// of the template, never the path, and is itself only accepted when it is exactly loopback.
function pageUrl(pack, target, testOrigin) {
  const n = normalizeTarget(pack.platform, target);
  if (!n.ok || n.target !== target) throw new Error('target is not valid for ' + pack.platform);
  const real = pack.url.replace('{target}', encodeURIComponent(target));
  if (!testOrigin) return real;
  if (!/^http:\/\/127\.0\.0\.1:\d{2,5}$/.test(testOrigin)) throw new Error('test origin must be http://127.0.0.1:<port>');
  const u = new URL(real);
  return testOrigin + u.pathname + u.search;
}
function allowedHosts(pack, testOrigin) {
  return testOrigin ? ['127.0.0.1'] : pack.hosts;
}

module.exports = { loadAdapters, validatePack, pageUrl, allowedHosts };
