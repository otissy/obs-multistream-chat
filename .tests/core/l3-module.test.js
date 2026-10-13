'use strict';
const test = require('node:test');
const assert = require('node:assert');
const { ensureBuilt, runLayer } = require('./helpers');

const LAYER = 'L3';
const MIN = 8;
const b = ensureBuilt();
if (!b.ok && !b.broken) {
  test(`${LAYER} C++ socket tests`, t => t.skip('UNVERIFIED: ' + b.reason));
} else {
  test(`${LAYER} build available`, () => assert.ok(b.ok, b.reason));
  if (b.ok) {
    const res = runLayer(LAYER);
    test(`${LAYER} produced results`, () => assert.ok(res.cases.length >= MIN, `only ${res.cases.length} cases\n${res.out}`));
    for (const c of res.cases) test(c.name, () => assert.ok(c.ok, c.detail));
  }
}
