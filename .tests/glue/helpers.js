'use strict';
const fs = require('fs');
const path = require('path');
const ROOT = path.resolve(__dirname, '..', '..');
// Sandboxes do not carry the gitignored SDK dirs: fall back to the real repo's.
function sdk(name) {
  for (const base of [ROOT, path.join(ROOT, '..', '..', 'obs-multistream')]) {
    const p = path.join(base, name);
    if (fs.existsSync(p)) return p;
  }
  return null;
}
function walk(dir, ext, out = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, ext, out); else if (ext.test(e.name)) out.push(p);
  }
  return out;
}
module.exports = { ROOT, sdk, walk };
