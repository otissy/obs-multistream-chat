'use strict';
// L1: every core file is present, self-contained, and (when a compiler exists) compiles alone.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const path = require('path');
const { ROOT, ensureBuilt } = require('./helpers');

const CORE = path.join(ROOT, 'src', 'core');
const files = fs.readdirSync(CORE).filter(f => /\.(hpp|cpp)$/.test(f));

test('core has the expected modules', () => {
  for (const m of ['json', 'secret', 'redact', 'destination', 'stats', 'health', 'config', 'state_feed', 'http_server'])
    assert.ok(files.includes(m + '.cpp') && files.includes(m + '.hpp'), m);
});

for (const f of files) {
  test(`static: ${f} includes resolve, braces balance, header guarded`, () => {
    const src = fs.readFileSync(path.join(CORE, f), 'utf8');
    for (const m of src.matchAll(/#include "([^"]+)"/g))
      assert.ok(fs.existsSync(path.join(CORE, m[1])), `missing include ${m[1]}`);
    // one pass, so a "//" inside a string or a quote inside a comment cannot confuse the scan
    const re = /\/\/[^\n]*|\/\*[\s\S]*?\*\/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'/g;
    const code = src.replace(re, m => (m[0] === '/' ? ' ' : '""'));
    const count = c => code.split(c).length - 1;
    assert.strictEqual(count('{'), count('}'), 'braces');
    assert.strictEqual(count('('), count(')'), 'parens');
    if (f.endsWith('.hpp')) assert.match(src, /#pragma once/);
  });
}

test('security: server never binds a wildcard address and never sends CORS allow headers', () => {
  const src = fs.readFileSync(path.join(CORE, 'http_server.cpp'), 'utf8').replace(/\/\/.*$/gm, '');
  assert.ok(!/INADDR_ANY|in6addr_any|"0\.0\.0\.0"/.test(src));
  assert.ok(/INADDR_LOOPBACK/.test(src));
  assert.ok(!/Access-Control-Allow/.test(src));
});

test('security: no log or printf of key fields in core', () => {
  for (const f of files.filter(x => x.endsWith('.cpp'))) {
    const src = fs.readFileSync(path.join(CORE, f), 'utf8');
    assert.ok(!/printf\([^)]*\.key\b/.test(src), f);
  }
});

test('C++ compiles and links (needs a compiler)', t => {
  const b = ensureBuilt();
  if (!b.ok && !b.broken) return t.skip('UNVERIFIED: ' + b.reason);
  assert.ok(b.ok, b.reason);
});
