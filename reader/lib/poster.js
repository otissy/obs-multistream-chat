'use strict';
// Sends what the sources read to the plugin's merge server, over loopback only.
//   POST http://127.0.0.1:<port>/chat/ingest   Authorization: Bearer <token>
// The token comes from the environment (never the command line) and is never logged.
const { delayMs } = require('./backoff');

const BATCH = 25;
const MAX_MESSAGE_CHARS = 4000;

// Keeps one message small enough that a batch always fits the server's body cap.
function shrink(m) {
  let budget = MAX_MESSAGE_CHARS;
  const parts = [];
  for (const p of m.parts.slice(0, 64)) {
    const len = (p.v || '').length + (p.u || '').length;
    if (len > budget) {
      if (p.t === 'text' && budget > 0) parts.push({ t: 'text', v: p.v.slice(0, budget) });
      break;
    }
    budget -= len;
    parts.push(p);
  }
  return Object.assign({}, m, { parts, author: m.author.slice(0, 200) });
}

class Poster {
  // opts: { port, token, log, fetchFn, flushMs, timeoutMs, maxQueue, unreachableMs, onFatal }
  constructor(opts) {
    this.url = 'http://127.0.0.1:' + opts.port + '/chat/ingest';
    this.token = opts.token;
    this.log = opts.log;
    this.fetchFn = opts.fetchFn || fetch;
    this.flushMs = opts.flushMs || 300;
    this.timeoutMs = opts.timeoutMs || 5000;
    this.maxQueue = opts.maxQueue || 2000;
    this.unreachableMs = opts.unreachableMs || 90000;
    this.onFatal = opts.onFatal || (() => {});
    this.queue = [];
    this.statuses = new Map();
    this.statusDirty = false;
    this.lastStatusSent = 0;
    this.fails = 0;
    this.nextTry = 0;
    this.lastOk = Date.now();
    this.busy = false;
    this.dropped = 0;
    this.sent = 0;
    this.fatal = false;
  }
  start() { this.timer = setInterval(() => { this.flush().catch(() => {}); }, this.flushMs); }
  async stop() {
    clearInterval(this.timer);
    this.nextTry = 0;
    try { await this.flush(); } catch (e) { /* best effort */ }
  }
  enqueue(messages) {
    for (const m of messages) this.queue.push(shrink(m));
    if (this.queue.length > this.maxQueue) {
      const drop = this.queue.length - this.maxQueue;
      this.queue.splice(0, drop); // oldest first: a stuck server must not eat memory
      this.dropped += drop;
    }
  }
  setStatus(s) {
    this.statuses.set(s.id, { id: s.id, platform: s.platform, state: s.state, detail: String(s.detail || '').slice(0, 160) });
    this.statusDirty = true;
  }
  async flush() {
    if (this.busy || this.fatal) return;
    const now = Date.now();
    if (now < this.nextTry) return;
    const needStatus = this.statusDirty || now - this.lastStatusSent > 10000;
    if (!this.queue.length && !needStatus) return;
    this.busy = true;
    const batch = this.queue.slice(0, BATCH);
    const body = JSON.stringify({
      v: 1, sent: Date.now(),
      messages: batch.map(m => ({ platform: m.platform, source: m.source, id: m.id, author: m.author, color: m.color, parts: m.parts, badges: m.badges, t: m.t })),
      sources: Array.from(this.statuses.values()),
    }, (k, v) => (typeof v === 'string' ? v.toWellFormed() : v));
    try {
      const res = await this.fetchFn(this.url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + this.token },
        body,
        signal: AbortSignal.timeout(this.timeoutMs),
      });
      const st = res.status;
      try { await res.arrayBuffer(); } catch (e) { /* body not needed */ }
      if (st === 200) {
        this.queue.splice(0, batch.length);
        this.sent += batch.length;
        this.statusDirty = false;
        this.lastStatusSent = Date.now();
        this.fails = 0;
        this.lastOk = Date.now();
      } else if (st === 401 || st === 403) {
        this.fatal = true;
        this.log.error('the merge server refused this reader (HTTP ' + st + '): token or address is wrong, stopping');
        this.onFatal('refused', st);
      } else if (st === 400 || st === 413) {
        this.queue.splice(0, batch.length); // retrying the same bytes cannot help
        this.dropped += batch.length;
        this.log.warn('a batch of ' + batch.length + ' messages was rejected (HTTP ' + st + ') and dropped');
        this.lastOk = Date.now();
      } else {
        this._failed('HTTP ' + st);
      }
    } catch (e) {
      this._failed(e && e.name === 'TimeoutError' ? 'timeout' : 'connection failed');
    } finally {
      this.busy = false;
    }
  }
  _failed(why) {
    this.fails++;
    this.nextTry = Date.now() + delayMs(this.fails - 1, 1000, 30000);
    if (this.fails === 1 || this.fails % 10 === 0) this.log.warn('merge server not reachable (' + why + '), will retry');
    if (Date.now() - this.lastOk > this.unreachableMs) {
      this.fatal = true;
      this.log.error('merge server unreachable for ' + Math.round(this.unreachableMs / 1000) + ' s: OBS is probably gone, stopping');
      this.onFatal('unreachable', 0);
    }
  }
}

module.exports = { Poster, shrink };
