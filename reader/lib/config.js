'use strict';
// The reader's own config: written by the plugin next to config.json, never contains a secret.
//   { "v":1, "port":51734, "quiet_minutes":5, "sources":[{"id":"c1","platform":"twitch","target":"channel"}] }
const fs = require('fs');
const { normalizeTarget, PLATFORMS } = require('./targets');

function parseReaderConfig(text) {
  let c;
  try { c = JSON.parse(text); } catch (e) { throw new Error('reader config is not valid JSON'); }
  if (!c || typeof c !== 'object' || Array.isArray(c) || c.v !== 1) throw new Error('reader config: "v" must be 1');
  if (!Number.isInteger(c.port) || c.port < 1024 || c.port > 65535) throw new Error('reader config: "port" must be 1024-65535');
  const quiet = c.quiet_minutes === undefined ? 5 : c.quiet_minutes;
  if (!Number.isInteger(quiet) || quiet < 1 || quiet > 120) throw new Error('reader config: "quiet_minutes" must be 1-120');
  // 32 = the plugin's kMaxChatSources (it was 10 here, so 11+ enabled channels stopped the whole reader)
  if (!Array.isArray(c.sources) || c.sources.length > 32) throw new Error('reader config: "sources" must be a list of at most 32');
  const ids = new Set();
  const sources = c.sources.map((s, i) => {
    if (!s || typeof s !== 'object') throw new Error('reader config: source #' + (i + 1) + ' is not an object');
    if (typeof s.id !== 'string' || !/^[A-Za-z0-9_-]{1,32}$/.test(s.id) || ids.has(s.id)) throw new Error('reader config: source #' + (i + 1) + ' has a bad or repeated id');
    ids.add(s.id);
    if (!PLATFORMS.includes(s.platform)) throw new Error('reader config: source ' + s.id + ' has an unknown platform');
    const n = normalizeTarget(s.platform, s.target);
    if (!n.ok || n.target !== s.target) throw new Error('reader config: source ' + s.id + ': ' + (n.error || 'target is not normalized'));
    return { id: s.id, platform: s.platform, target: s.target };
  });
  const ua = c.user_agent === undefined ? 'chrome' : c.user_agent;
  if (ua !== 'chrome' && ua !== 'headless') throw new Error('reader config: "user_agent" must be "chrome" or "headless"');
  const lowMemory = c.low_memory === undefined ? undefined : c.low_memory === true;
  return { port: c.port, quietMs: quiet * 60000, images: c.images === true, userAgent: ua, lowMemory, sources };
}

function loadReaderConfig(file) {
  const st = fs.statSync(file);
  if (st.size > 64 * 1024) throw new Error('reader config is too large');
  return parseReaderConfig(fs.readFileSync(file, 'utf8'));
}

module.exports = { parseReaderConfig, loadReaderConfig };
