'use strict';
// A small Chrome DevTools Protocol client over a plain WebSocket (Node's built-in WebSocket).
// Only talks to the browser this reader started, on 127.0.0.1.
class Cdp {
  static async connect(wsUrl, timeoutMs = 10000) {
    const u = new URL(wsUrl);
    if (u.hostname !== '127.0.0.1' && u.hostname !== 'localhost') throw new Error('DevTools endpoint is not loopback');
    const ws = new WebSocket(wsUrl);
    await new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error('DevTools connect timeout')), timeoutMs);
      ws.onopen = () => { clearTimeout(t); resolve(); };
      ws.onerror = () => { clearTimeout(t); reject(new Error('DevTools connect failed')); };
    });
    return new Cdp(ws);
  }
  constructor(ws) {
    this.ws = ws;
    this.id = 0;
    this.pending = new Map();
    this.handlers = []; // {method, sessionId, fn}
    this.closed = false;
    this.onclose = null;
    ws.onmessage = ev => {
      let m;
      try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.id && this.pending.has(m.id)) {
        const { res, rej, timer } = this.pending.get(m.id);
        this.pending.delete(m.id);
        clearTimeout(timer);
        if (m.error) rej(new Error(m.error.message || 'protocol error')); else res(m.result || {});
      } else if (m.method) {
        for (const h of this.handlers.slice()) {
          if (h.method === m.method && (h.sessionId === undefined || h.sessionId === m.sessionId)) {
            try { h.fn(m.params || {}, m); } catch (e) { /* a handler must not kill the client */ }
          }
        }
      }
    };
    ws.onclose = () => {
      this.closed = true;
      for (const { rej, timer } of this.pending.values()) { clearTimeout(timer); rej(new Error('DevTools connection closed')); }
      this.pending.clear();
      if (this.onclose) this.onclose();
    };
  }
  send(method, params = {}, sessionId, timeoutMs = 20000) {
    if (this.closed) return Promise.reject(new Error('DevTools connection closed'));
    const id = ++this.id;
    const msg = { id, method, params };
    if (sessionId) msg.sessionId = sessionId;
    return new Promise((res, rej) => {
      const timer = setTimeout(() => { this.pending.delete(id); rej(new Error(method + ' timed out')); }, timeoutMs);
      this.pending.set(id, { res, rej, timer });
      try { this.ws.send(JSON.stringify(msg)); } catch (e) { clearTimeout(timer); this.pending.delete(id); rej(e); }
    });
  }
  on(method, fn, sessionId) {
    const h = { method, fn, sessionId };
    this.handlers.push(h);
    return () => { const i = this.handlers.indexOf(h); if (i >= 0) this.handlers.splice(i, 1); };
  }
  close() { try { this.ws.close(); } catch (e) { /* already closed */ } }
}
module.exports = { Cdp };
