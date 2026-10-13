'use strict';
// L4: the real C++ HttpServer (via oms_fixture_server) answering real HTTP clients.
const test = require('node:test');
const assert = require('node:assert');
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { ensureBuilt, findExe } = require('./helpers');

const b = ensureBuilt();
function rawGet(port, hostHeader, p = '/state.json', extra = {}) {
  return new Promise((resolve, reject) => {
    const req = http.request({ host: '127.0.0.1', port, path: p, method: 'GET', headers: { Host: hostHeader, ...extra } }, res => {
      let body = '';
      res.on('data', d => (body += d));
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body }));
    });
    req.on('error', reject);
    req.end();
  });
}

if (!b.ok && !b.broken) {
  test('L4 fixture server end to end', t => t.skip('UNVERIFIED: ' + b.reason));
} else {
  test('L4 end to end', async t => {
    assert.ok(b.ok, b.reason);
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'oms-l4-'));
    fs.mkdirSync(path.join(dir, 'icons'));
    fs.writeFileSync(path.join(dir, 'overlay.html'), '<!doctype html><title>t</title>');
    fs.writeFileSync(path.join(dir, 'overlay.js'), '1');
    for (const n of ['twitch', 'youtube', 'kick', 'facebook', 'tiktok', 'generic']) fs.writeFileSync(path.join(dir, 'icons', n + '.svg'), '<svg/>');
    const child = spawn(findExe('oms_fixture_server'), [dir], { stdio: ['pipe', 'pipe', 'inherit'] });
    let buf = '';
    const waiters = [];
    child.stdout.on('data', d => {
      buf += d;
      for (const w of waiters.splice(0)) w();
    });
    const until = re =>
      new Promise((res, rej) => {
        const timer = setTimeout(() => rej(new Error('timeout ' + re)), 8000);
        const chk = () => {
          const m = buf.match(re);
          if (m) { clearTimeout(timer); buf = buf.slice(m.index + m[0].length); res(m); } else waiters.push(chk);
        };
        chk();
      });
    try {
      const port = +(await until(/PORT (\d+)/))[1];
      const host = `127.0.0.1:${port}`;
      await t.test('serves state and assets', async () => {
        child.stdin.write('set twitch otiyyy live;youtube otiyyy reconnecting\n');
        await until(/OK/);
        const r = await rawGet(port, host);
        assert.strictEqual(r.status, 200);
        const j = JSON.parse(r.body);
        assert.deepStrictEqual(j.destinations.map(d => d.state), ['live', 'reconnecting']);
        assert.strictEqual(j.destinations[0].live, true);
        assert.ok(!/rtmp|key/i.test(r.body));
        assert.ok(!Object.keys(r.headers).some(h => h.startsWith('access-control')));
        assert.strictEqual((await rawGet(port, host, '/icons/twitch.svg')).status, 200);
      });
      await t.test('DNS rebinding Host is refused', async () => {
        for (const h of ['evil.example:' + port, '127.0.0.1', 'localhost:1', host + '.evil.example']) {
          const r = await rawGet(port, h);
          assert.strictEqual(r.status, 403, h);
          assert.ok(!r.body.includes('otiyyy'));
        }
        assert.strictEqual((await rawGet(port, host, '/state.json', { Origin: 'http://evil.example' })).status, 403);
        assert.strictEqual((await rawGet(port, 'localhost:' + port)).status, 200);
      });
      await t.test('only loopback answers', async () => {
        for (const ifs of Object.values(os.networkInterfaces()))
          for (const a of ifs)
            if (a.family === 'IPv4' && !a.internal)
              await assert.rejects(
                () => new Promise((res, rej) => {
                  const q = http.get({ host: a.address, port, path: '/state.json', timeout: 2000 }, res);
                  q.on('error', rej);
                  q.on('timeout', () => q.destroy(new Error('timeout')));
                }));
      });
      await t.test('SSE pushes a change', async () => {
        const events = [];
        const req = http.get({ host: '127.0.0.1', port, path: '/events', headers: { Host: host } }, res => {
          assert.match(res.headers['content-type'], /text\/event-stream/);
          res.on('data', d => events.push(String(d)));
        });
        await new Promise(r => setTimeout(r, 400));
        child.stdin.write('set twitch otiyyy failed\n');
        await new Promise(r => setTimeout(r, 600));
        req.destroy();
        const all = events.join('');
        assert.ok(all.includes('"state":"live"') && all.includes('"state":"failed"'), all);
      });
    } finally {
      child.stdin.write('quit\n');
      setTimeout(() => child.kill(), 3000).unref();
    }
  });
}
