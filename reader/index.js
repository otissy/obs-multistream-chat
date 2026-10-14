#!/usr/bin/env node
'use strict';
// OBS Multistream Chat reader.
//
// Reads the PUBLIC chat page of each configured channel in a headless Chrome of its own (throw-away
// profile, no login, no extension), and posts the messages to the plugin's local merge server.
// No platform API, no OAuth, no token of any platform, nothing is ever sent to a platform.
//
//   node reader/index.js --config <reader-config.json> [--exit-on-stdin-close]
//   env OMSC_CHAT_TOKEN   per-session token for the merge server (never on the command line)
//
// Exit codes: 0 normal, 2 bad usage or config, 3 merge server refused the token, 4 merge server
// unreachable, 5 browser could not be (re)started.
const path = require('path');
const { loadReaderConfig } = require('./lib/config');
const { loadAdapters } = require('./lib/adapters');
const { launch, cleanStaleProfiles } = require('./lib/chrome');
const { Source } = require('./lib/source');
const { Poster } = require('./lib/poster');
const { makeLogger } = require('./lib/log');
const { delayMs } = require('./lib/backoff');

function parseArgs(argv) {
  const o = { exitOnStdinClose: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--config') o.config = argv[++i];
    else if (a === '--adapters') o.adapters = argv[++i];
    else if (a === '--log') o.log = argv[++i];
    else if (a === '--chrome') o.chrome = argv[++i];
    else if (a === '--test-origin') o.testOrigin = argv[++i]; // tests only: http://127.0.0.1:<port>
    else if (a === '--exit-on-stdin-close') o.exitOnStdinClose = true;
    else if (a === '--diagnose') o.diagnose = true; // prints structural facts about each page, then exits
    else if (a === '--wait') o.waitSec = parseInt(argv[++i], 10);
    else throw new Error('unknown argument: ' + a);
  }
  if (!o.config) throw new Error('--config <file> is required');
  return o;
}

// Starts the reader; resolves to a controller { shutdown(code), done } once running.
async function start(opts, env, hooks = {}) {
  const log = makeLogger(opts.log);
  const token = env.OMSC_CHAT_TOKEN || '';
  if (token.length < 16) throw Object.assign(new Error('OMSC_CHAT_TOKEN is missing or too short'), { exitCode: 2 });
  log.addSecret(token);
  const cfg = loadReaderConfig(opts.config);
  const adaptersDir = opts.adapters || path.join(__dirname, 'adapters');
  const { packs, errors } = loadAdapters(adaptersDir);
  if (opts.testOrigin && !/^http:\/\/127\.0\.0\.1:\d{2,5}$/.test(opts.testOrigin)) throw Object.assign(new Error('--test-origin must be http://127.0.0.1:<port>'), { exitCode: 2 });

  let finish;
  const done = new Promise(r => { finish = r; });
  let shuttingDown = false;
  const sources = [];
  let browser = null;

  const poster = new Poster({
    port: cfg.port, token, log,
    onFatal: why => { shutdown(why === 'refused' ? 3 : 4); },
  });
  poster.start();
  log.info('reader started: ' + cfg.sources.length + ' source(s)');

  async function shutdown(code) {
    if (shuttingDown) return;
    shuttingDown = true;
    log.info('reader stopping (code ' + code + ')');
    for (const s of sources) { try { await s.stop(); } catch (e) { /* ignore */ } poster.setStatus({ id: s.src.id, platform: s.src.platform, state: 'stopped', detail: '' }); }
    try { await poster.stop(); } catch (e) { /* ignore */ }
    if (browser) { try { await browser.close(); } catch (e) { /* ignore */ } }
    finish(code);
  }

  // Chrome gets the environment WITHOUT the token.
  const browserEnv = Object.assign({}, env);
  delete browserEnv.OMSC_CHAT_TOKEN;

  const wanted = cfg.sources.filter(s => packs[s.platform]);
  for (const s of cfg.sources) {
    if (!packs[s.platform]) poster.setStatus({ id: s.id, platform: s.platform, state: 'error', detail: errors[s.platform] || 'no adapter' });
  }

  async function openBrowser() {
    cleanStaleProfiles();
    const b = await launch({ chrome: opts.chrome, images: cfg.images, env: browserEnv });
    if (shuttingDown) { await b.close(); return false; } // asked to stop while Chrome was starting
    browser = b;
    browser.cdp.onclose = () => { if (!shuttingDown) onBrowserLost(); };
    log.info('browser started');
    return true;
  }
  let relaunches = 0;
  async function onBrowserLost() {
    log.error('the browser went away');
    for (const s of sources) { try { await s.stop(); } catch (e) { /* ignore */ } }
    sources.length = 0;
    for (const s of wanted) poster.setStatus({ id: s.id, platform: s.platform, state: 'error', detail: 'the browser closed unexpectedly' });
    if (++relaunches > 3) { poster.setStatus({ id: 'reader', platform: 'twitch', state: 'error', detail: 'browser keeps failing' }); shutdown(5); return; }
    await new Promise(r => setTimeout(r, delayMs(relaunches - 1, 3000, 60000)));
    if (shuttingDown) return;
    try { if (await openBrowser()) await startSources(); } catch (e) { log.error('could not restart the browser: ' + e.message); shutdown(5); }
  }
  async function startSources() {
    for (const s of wanted) {
      if (shuttingDown) return;
      const src = new Source({ cdp: browser.cdp, pack: packs[s.platform], src: s, testOrigin: opts.testOrigin, quietMs: cfg.quietMs, log });
      src.on('status', st => poster.setStatus(st));
      src.on('messages', msgs => poster.enqueue(msgs));
      sources.push(src);
      poster.setStatus(src.status());
      await src.start();
      if (hooks.noStagger !== true) await new Promise(r => setTimeout(r, 1500)); // one page at a time
    }
  }

  if (wanted.length) {
    try { if (!(await openBrowser())) return { shutdown, done, sources, poster }; } catch (e) {
      log.error('browser start failed: ' + e.message);
      for (const s of wanted) poster.setStatus({ id: s.id, platform: s.platform, state: 'error', detail: 'could not start Chrome: ' + e.message });
      await new Promise(r => setTimeout(r, 1200)); // let the statuses reach the plugin
      await shutdown(5);
      return { shutdown, done, sources, poster };
    }
    startSources().catch(e => log.error('start failed: ' + e.message));
  }
  return { shutdown, done, sources, poster };
}

async function runDiagnose(opts) {
  const { diagnose } = require('./lib/diagnose');
  const cfg = loadReaderConfig(opts.config);
  const { packs, errors } = loadAdapters(opts.adapters || path.join(__dirname, 'adapters'));
  const env = Object.assign({}, process.env);
  delete env.OMSC_CHAT_TOKEN;
  await diagnose(cfg, packs, errors, { chrome: opts.chrome, testOrigin: opts.testOrigin, waitSec: opts.waitSec, env }, o => process.stdout.write(JSON.stringify(o) + '\n'));
}

async function main() {
  let opts;
  try { opts = parseArgs(process.argv.slice(2)); } catch (e) { process.stderr.write(e.message + '\n'); process.exit(2); }
  if (opts.diagnose) {
    try { await runDiagnose(opts); process.exit(0); } catch (e) { process.stderr.write(String(e.message) + '\n'); process.exit(2); }
  }
  let ctl;
  try { ctl = await start(opts, process.env); } catch (e) { process.stderr.write(e.message + '\n'); process.exit(e.exitCode || 2); }
  delete process.env.OMSC_CHAT_TOKEN;
  process.on('SIGINT', () => ctl.shutdown(0));
  process.on('SIGTERM', () => ctl.shutdown(0));
  if (opts.exitOnStdinClose) {
    process.stdin.on('end', () => ctl.shutdown(0));
    process.stdin.on('error', () => ctl.shutdown(0));
    process.stdin.resume();
  }
  process.exit(await ctl.done);
}

if (require.main === module) main();
module.exports = { start, parseArgs, runDiagnose };
