'use strict';
// Tiny file logger. Logs states and errors, never chat text, never the token: every line is
// passed through scrub(), which replaces any registered secret before it can be written.
const fs = require('fs');

const MAX_BYTES = 256 * 1024;

function makeLogger(file) {
  const secrets = [];
  let size = 0;
  try { size = file ? fs.statSync(file).size : 0; } catch (e) { size = 0; }
  function scrub(s) {
    let out = String(s);
    for (const sec of secrets) if (sec && sec.length >= 6) out = out.split(sec).join('[token]');
    return out;
  }
  function write(level, msg) {
    const line = new Date().toISOString() + ' ' + level + ' ' + scrub(msg).replace(/[\r\n]+/g, ' ').slice(0, 500) + '\n';
    if (!file) return;
    try {
      if (size + line.length > MAX_BYTES) {
        try { fs.renameSync(file, file + '.1'); } catch (e) { /* keep going */ }
        size = 0;
      }
      fs.appendFileSync(file, line);
      size += line.length;
    } catch (e) { /* logging must never break the reader */ }
  }
  return {
    addSecret(s) { secrets.push(s); },
    scrub,
    info: m => write('INFO', m),
    warn: m => write('WARN', m),
    error: m => write('ERROR', m),
  };
}

module.exports = { makeLogger };
