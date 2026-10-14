'use strict';
// Starts the user's installed Chrome headless, with a throw-away profile, and returns a DevTools
// connection to it. No extension, no login, no cookies kept: the profile folder is deleted on exit.
const { spawn } = require('child_process');
const fs = require('fs');
const http = require('http');
const os = require('os');
const path = require('path');
const { Cdp } = require('./cdp');

const PREFIX = 'oms-chat-profile-';
const MARKER = '.oms-chat-profile';

function candidates(env = process.env) {
  const list = [];
  if (env.OMSC_CHROME) list.push(env.OMSC_CHROME);
  list.push('C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe');
  list.push('C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe');
  if (env.LOCALAPPDATA) list.push(path.join(env.LOCALAPPDATA, 'Google', 'Chrome', 'Application', 'chrome.exe'));
  list.push('C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe'); // same engine, last resort
  return list;
}
function findChrome(env = process.env) {
  for (const c of candidates(env)) { try { if (fs.statSync(c).isFile()) return c; } catch (e) { /* next */ } }
  return null;
}

// Removes profile folders that an earlier run left behind (crash, power cut). Only folders with
// our own prefix AND marker file, older than 6 hours.
function cleanStaleProfiles(tmp = os.tmpdir(), maxAgeMs = 6 * 3600 * 1000) {
  let n = 0;
  let names = [];
  try { names = fs.readdirSync(tmp); } catch (e) { return 0; }
  for (const name of names) {
    if (!name.startsWith(PREFIX)) continue;
    const dir = path.join(tmp, name);
    try {
      if (!fs.existsSync(path.join(dir, MARKER))) continue;
      if (Date.now() - fs.statSync(dir).mtimeMs < maxAgeMs) continue;
      fs.rmSync(dir, { recursive: true, force: true, maxRetries: 3 });
      n++;
    } catch (e) { /* in use or locked: leave it */ }
  }
  return n;
}

function chromeArgs(dir, opts) {
  const a = [
    '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--remote-debugging-port=0', '--remote-allow-origins=http://127.0.0.1',
    '--user-data-dir=' + dir,
    '--disable-extensions', '--disable-sync', '--disable-background-networking', '--disable-component-update',
    '--disable-default-apps', '--disable-breakpad', '--no-service-autorun', '--disable-features=Translate,MediaRouter',
    '--deny-permission-prompts', '--autoplay-policy=user-gesture-required', '--password-store=basic',
    '--hide-scrollbars',
  ];
  if (!opts.images) a.push('--blink-settings=imagesEnabled=false');
  a.push('about:blank');
  return a;
}

function getJson(port, p) {
  return new Promise((resolve, reject) => {
    const req = http.get({ host: '127.0.0.1', port, path: p, timeout: 5000 }, res => {
      let b = '';
      res.on('data', d => { b += d; });
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } });
    });
    req.on('error', reject);
    req.on('timeout', () => { req.destroy(new Error('timeout')); });
  });
}

// -> { cdp, proc, dir, close() }
async function launch(opts = {}) {
  const exe = opts.chrome || findChrome();
  if (!exe) throw new Error('Google Chrome was not found (looked in Program Files and LocalAppData)');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), PREFIX));
  fs.writeFileSync(path.join(dir, MARKER), 'oms chat reader throw-away profile\n');
  const proc = spawn(exe, chromeArgs(dir, opts), { stdio: 'ignore', windowsHide: true, env: opts.env || process.env });
  let exited = false;
  proc.on('exit', () => { exited = true; });
  const portFile = path.join(dir, 'DevToolsActivePort');
  const t0 = Date.now();
  while (!fs.existsSync(portFile) && !exited && Date.now() - t0 < 20000) await new Promise(r => setTimeout(r, 100));
  const cleanup = () => { try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch (e) { /* retried at next start */ } };
  if (!fs.existsSync(portFile)) {
    try { proc.kill(); } catch (e) { /* gone */ }
    await new Promise(r => setTimeout(r, 300));
    cleanup();
    throw new Error('Chrome did not start');
  }
  const [port] = fs.readFileSync(portFile, 'utf8').split(/\r?\n/);
  let cdp;
  try {
    const ver = await getJson(Number(port), '/json/version');
    cdp = await Cdp.connect(ver.webSocketDebuggerUrl);
  } catch (e) {
    try { proc.kill(); } catch (e2) { /* gone */ }
    await new Promise(r => setTimeout(r, 300));
    cleanup();
    throw e;
  }
  let closing = null;
  const close = () => {
    if (closing) return closing;
    closing = (async () => {
      try { await cdp.send('Browser.close', {}, undefined, 3000); } catch (e) { /* closing anyway */ }
      cdp.close();
      for (let i = 0; i < 30 && !exited; i++) await new Promise(r => setTimeout(r, 100));
      if (!exited) { try { proc.kill(); } catch (e) { /* gone */ } await new Promise(r => setTimeout(r, 500)); }
      cleanup();
    })();
    return closing;
  };
  return { cdp, proc, dir, close, hasExited: () => exited };
}

module.exports = { findChrome, launch, cleanStaleProfiles, PREFIX, MARKER };
