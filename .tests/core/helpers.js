'use strict';
// Finds or builds the C++ core tests. Returns null when no C++ compiler exists, so the layers
// report SKIPPED (= unverified) instead of passing.
const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..', '..');

function vsInstalled() {
  const vswhere = 'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe';
  if (!fs.existsSync(vswhere)) return false;
  const r = spawnSync(vswhere, ['-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], { encoding: 'utf8' });
  return r.status === 0 && r.stdout.trim().length > 0;
}
function onPath(cmd) {
  const r = spawnSync(process.platform === 'win32' ? 'where' : 'which', [cmd], { encoding: 'utf8' });
  return r.status === 0;
}
function candidates(name) {
  const exe = process.platform === 'win32' ? name + '.exe' : name;
  return [
    path.join(ROOT, 'build', 'tests-vs', 'Release', exe),
    path.join(ROOT, 'build', 'tests-vs', 'Debug', exe),
    path.join(ROOT, 'build', 'windows-x64', 'Release', exe),
    path.join(ROOT, 'build', 'tests', exe),
  ];
}
function findExe(name) { return candidates(name).find(p => fs.existsSync(p)) || null; }

let attempted = false, reason = '';
function ensureBuilt() {
  if (findExe('oms_tests')) return { ok: true };
  if (attempted) return { ok: false, reason };
  attempted = true;
  let cfg;
  if (vsInstalled()) cfg = ['--preset', 'tests-vs2022'];
  else if (onPath('g++') || onPath('clang++')) cfg = ['--preset', 'tests'];
  else { reason = 'no C++ compiler found (install VS Build Tools 2022, Desktop development with C++)'; return { ok: false, reason }; }
  let r = spawnSync('cmake', ['-S', ROOT, ...cfg], { encoding: 'utf8' });
  if (r.status !== 0) { reason = 'cmake configure failed:\n' + r.stdout + r.stderr; return { ok: false, reason, broken: true }; }
  const build = cfg[1] === 'tests-vs2022'
    ? ['--build', path.join(ROOT, 'build', 'tests-vs'), '--config', 'Release', '--parallel']
    : ['--build', path.join(ROOT, 'build', 'tests'), '--parallel'];
  r = spawnSync('cmake', build, { encoding: 'utf8', maxBuffer: 1 << 26 });
  if (r.status !== 0) { reason = 'build failed:\n' + r.stdout.slice(-4000) + r.stderr.slice(-2000); return { ok: false, reason, broken: true }; }
  return findExe('oms_tests') ? { ok: true } : { ok: false, reason: 'built but exe not found', broken: true };
}

// Runs oms_tests for a layer; returns [{ok,name}] and raw output.
function runLayer(layer) {
  const exe = findExe('oms_tests');
  const r = spawnSync(exe, [layer], { encoding: 'utf8', timeout: 120000, cwd: path.dirname(exe) });
  const cases = [];
  for (const line of (r.stdout || '').split(/\r?\n/)) {
    const m = line.match(/^(not ok|ok) \d+ - (.*)$/);
    if (m) cases.push({ ok: m[1] === 'ok', name: m[2], detail: '' });
    else if (line.startsWith('# ') && cases.length && !cases[cases.length - 1].ok) cases[cases.length - 1].detail += line + '\n';
  }
  return { cases, status: r.status, out: (r.stdout || '') + (r.stderr || '') };
}

module.exports = { ROOT, ensureBuilt, findExe, runLayer, vsInstalled };
