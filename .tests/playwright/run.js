'use strict';
// Runs the Playwright suite with Playwright's own Chromium (headless). Playwright is not a project
// dependency: set PLAYWRIGHT_DIR to a folder holding node_modules/playwright (1.64.x), or it is looked
// up in the npx cache. Extra arguments go to `playwright test` (e.g. a spec file or --grep).
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function findPlaywright() {
  const tries = [];
  if (process.env.PLAYWRIGHT_DIR) tries.push(path.join(process.env.PLAYWRIGHT_DIR, 'node_modules'));
  const npx = path.join(process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local'), 'npm-cache', '_npx');
  if (fs.existsSync(npx)) for (const d of fs.readdirSync(npx)) tries.push(path.join(npx, d, 'node_modules'));
  return tries.find(nm => { try { return /^1\.6[4-9]\./.test(JSON.parse(fs.readFileSync(path.join(nm, 'playwright', 'package.json'), 'utf8')).version); } catch (e) { return false; } });
}

const nm = findPlaywright();
if (!nm) { console.error('Playwright 1.64.x not found: set PLAYWRIGHT_DIR (folder with node_modules/playwright)'); process.exit(2); }
const env = Object.assign({}, process.env, { NODE_PATH: nm });
for (const k of Object.keys(env)) if (k.startsWith('NODE_TEST')) delete env[k];
const r = spawnSync(process.execPath, [path.join(nm, 'playwright', 'cli.js'), 'test', '--config', path.join(__dirname, 'playwright.config.js'), ...process.argv.slice(2)], { stdio: 'inherit', env });
process.exit(r.status == null ? 1 : r.status);
