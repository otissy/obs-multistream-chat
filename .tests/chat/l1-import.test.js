'use strict';
// L1: every new file loads or parses on its own, without side effects.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { ROOT, READER, OVERLAY_DIR } = require('./helpers');

const LIBS = ['adapters', 'backoff', 'cdp', 'chrome', 'config', 'inject', 'log', 'poster', 'source', 'targets'];

test('reader modules load without starting anything', () => {
  for (const m of LIBS) assert.doesNotThrow(() => require(path.join(READER, 'lib', m + '.js')), m);
  const idx = require(path.join(READER, 'index.js'));
  assert.strictEqual(typeof idx.start, 'function');
  assert.strictEqual(typeof idx.parseArgs, 'function');
});

test('reader has no dependencies and no package scripts', () => {
  const pkgPath = path.join(READER, 'package.json');
  assert.ok(fs.existsSync(pkgPath));
  const pkg = JSON.parse(fs.readFileSync(pkgPath, 'utf8'));
  assert.ok(!pkg.dependencies || Object.keys(pkg.dependencies).length === 0);
  assert.ok(!pkg.devDependencies || Object.keys(pkg.devDependencies).length === 0);
  assert.ok(!pkg.scripts || !Object.keys(pkg.scripts).some(k => /install/.test(k)), 'no install scripts');
  for (const f of fs.readdirSync(path.join(READER, 'lib')).concat(['index.js'])) {
    const src = fs.readFileSync(path.join(READER, f === 'index.js' ? 'index.js' : 'lib/' + f), 'utf8');
    for (const m of src.matchAll(/require\('([^']+)'\)/g)) {
      assert.ok(m[1].startsWith('./') || ['fs', 'path', 'os', 'http', 'crypto', 'events', 'child_process'].includes(m[1]), f + ' requires ' + m[1]);
    }
  }
});

test('the injected engine is one self-contained function that touches nothing on the page', () => {
  const { omscEngine } = require(path.join(READER, 'lib', 'inject.js'));
  assert.strictEqual(typeof omscEngine, 'function');
  const src = omscEngine.toString();
  assert.doesNotThrow(() => new vm.Script('(' + src + ')'));
  const code = src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/(^|[^:'"\\])\/\/.*$/gm, '$1');
  const forbidden = [
    [/\.click\s*\(/, 'click'], [/\.focus\s*\(/, 'focus'], [/\.blur\s*\(/, 'blur'], [/dispatchEvent/, 'dispatchEvent'],
    [/innerHTML|outerHTML|insertAdjacent/, 'html writes'], [/appendChild|removeChild|replaceChild|insertBefore|\.remove\s*\(|\.append\s*\(|\.prepend\s*\(/, 'DOM edits'],
    [/setAttribute|removeAttribute|classList\s*\.\s*(add|remove|toggle)/, 'attribute edits'], [/\.style\s*\.\s*\w+\s*=|\.style\s*\[/, 'style edits'],
    [/scrollTo|scrollBy|scrollIntoView|scrollTop\s*=|\.scroll\s*\(/, 'scrolling'], [/fetch\s*\(|XMLHttpRequest|WebSocket|EventSource|sendBeacon/, 'network'],
    [/localStorage|sessionStorage|indexedDB|document\s*\.\s*cookie/, 'storage'], [/\.submit\s*\(|\.value\s*=|execCommand|\.select\s*\(/, 'form input'],
    [/window\s*\.\s*open|location\s*=[^=]|location\s*\.\s*(href\s*=[^=]|assign|replace|reload)/, 'navigation'], [/\beval\s*\(|new\s+Function|document\s*\.\s*write/, 'dynamic code'],
    [/\.innerText\s*=|\.textContent\s*=|\.nodeValue\s*=/, 'text writes'], [/KeyboardEvent|MouseEvent|InputEvent|PointerEvent/, 'synthetic events'],
  ];
  for (const [re, what] of forbidden) assert.ok(!re.test(code), 'engine must not use: ' + what);
  assert.ok(/window\[bindingName\]/.test(code), 'engine reports only through the binding');
});

test('all five adapter packs are present and valid', () => {
  const { loadAdapters } = require(path.join(READER, 'lib', 'adapters.js'));
  const { packs, errors } = loadAdapters(path.join(READER, 'adapters'));
  assert.deepStrictEqual(errors, {});
  assert.deepStrictEqual(Object.keys(packs).sort(), ['kick', 'tiktok', 'twitch', 'x', 'youtube']);
  assert.strictEqual(packs.tiktok.experimental, true);
  assert.strictEqual(packs.x.experimental, true);
  for (const p of ['twitch', 'youtube', 'kick']) assert.strictEqual(packs[p].experimental, false, p);
  for (const p of Object.values(packs)) assert.ok(p.url.startsWith('https://'));
});

test('overlay page and script exist, parse, and avoid HTML injection sinks', () => {
  const html = fs.readFileSync(path.join(OVERLAY_DIR, 'chat.html'), 'utf8');
  const js = fs.readFileSync(path.join(OVERLAY_DIR, 'chat.js'), 'utf8');
  assert.doesNotThrow(() => new vm.Script(js));
  const jsCode = js.replace(/\/\*[\s\S]*?\*\//g, '').replace(/(^|[^:'"\\])\/\/.*$/gm, '$1');
  assert.ok(!/innerHTML|outerHTML|insertAdjacentHTML|document\.write|\beval\s*\(|new\s+Function/.test(jsCode));
  assert.ok(/textContent/.test(js) && /createTextNode/.test(js));
  assert.ok(!/<script(?![^>]*\bsrc=)/i.test(html), 'no inline script');
  assert.ok(!/\son\w+\s*=/i.test(html), 'no inline event handlers');
  assert.ok(/<script src="chat\.js">/.test(html)); // relative: keeps the /k/<key>/ prefix
  assert.ok(!/['"(]\/(chat|icons|events|state)/.test(js.replace(/\/\/.*$/gm, '')), 'chat.js uses only relative URLs');
});

test('platform icons for all five chat platforms are bundled', () => {
  for (const p of ['twitch', 'youtube', 'kick', 'tiktok', 'x']) {
    const f = path.join(OVERLAY_DIR, 'icons', p + '.svg');
    assert.ok(fs.existsSync(f), p);
    const s = fs.readFileSync(f, 'utf8');
    assert.ok(s.startsWith('<svg') && s.includes('<path d="') && s.includes('<title>'), p);
    assert.ok(!/<script|onload|href=/i.test(s), p + ' svg has no active content');
  }
});

test('fixtures build for every variant and the vectors file parses', () => {
  const { page, BUILD } = require('./fixtures/dom');
  for (const v of ['twitch', 'twitch-layout-changed', 'youtube', 'kick', 'kick-empty', 'tiktok', 'x', 'x-login']) {
    assert.ok(page(v, 'k1').includes('<!doctype html>'), v);
  }
  assert.deepStrictEqual(Object.keys(BUILD).sort(), ['kick', 'tiktok', 'twitch', 'x', 'youtube']);
  const vec = JSON.parse(fs.readFileSync(path.join(__dirname, 'vectors', 'targets.json'), 'utf8'));
  assert.ok(Array.isArray(vec) && vec.length >= 40);
});

test('no file in the reader or overlay reaches for a platform API, token or login', () => {
  const files = [];
  const walk = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (/\.(js|json|html)$/.test(e.name)) files.push(p); } };
  walk(READER);
  files.push(path.join(OVERLAY_DIR, 'chat.js'), path.join(OVERLAY_DIR, 'chat.html'));
  const apiHosts = /(api\.twitch\.tv|id\.twitch\.tv|googleapis\.com|api\.kick\.com|open\.tiktokapis|api\.x\.com|api\.twitter\.com|client_secret|client_id|access_token)/i;
  for (const f of files) {
    const s = fs.readFileSync(f, 'utf8');
    assert.ok(!apiHosts.test(s), path.relative(ROOT, f) + ' mentions an API or OAuth');
  }
});

test('plugin glue: the reader gets its token from the environment only, lives in a kill-on-close job, and is never started on load by default', () => {
  const rp = fs.readFileSync(path.join(ROOT, 'src', 'obs', 'reader_process.cpp'), 'utf8');
  const cmdLine = /std::wstring cmd = ([\s\S]*?);\n/.exec(rp)[1];
  assert.ok(!/token/i.test(cmdLine), 'the command line must not mention the token: ' + cmdLine);
  assert.ok(/OMSC_CHAT_TOKEN=/.test(rp) && /CREATE_UNICODE_ENVIRONMENT/.test(rp));
  assert.ok(/JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE/.test(rp) && /CREATE_NO_WINDOW/.test(rp) && /SW_HIDE/.test(rp));
  assert.ok(/PROC_THREAD_ATTRIBUTE_HANDLE_LIST/.test(rp), 'only the handles the child needs are inherited');
  const pm = fs.readFileSync(path.join(ROOT, 'src', 'obs', 'plugin-main.cpp'), 'utf8');
  assert.ok(/if \(st\.cfg\.chat\.start_with_obs && !st\.read_only\)/.test(pm), 'auto start only when the user ticked it');
  assert.ok(/set_token\(""\)/.test(pm), 'the token is revoked when the reader ends');
  assert.ok(!/0\.0\.0\.0|INADDR_ANY/.test(pm));
  const cfg = fs.readFileSync(path.join(ROOT, 'src', 'core', 'config.cpp'), 'utf8');
  assert.ok(!/token|cookie|password/i.test(/json::Value chat = [\s\S]*?root\.set\("chat"/.exec(cfg)[0]), 'no login material is ever saved in the chat block');
});
