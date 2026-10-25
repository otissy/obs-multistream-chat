'use strict';
// Playwright suite for the web-facing overlay pages (chat tick-upward, access key, input).
// Run: node .tests/playwright/run.js   (finds Playwright 1.64.x, runs headless, one worker)
const path = require('path');
module.exports = {
  testDir: __dirname,
  testMatch: /.*\.spec\.js$/,
  timeout: 90000,
  workers: 1,               // one browser at a time: the owner's PC may be gaming or streaming
  retries: 0,
  reporter: [['list']],
  outputDir: process.env.OMS_PW_OUT || path.join(__dirname, '..', '..', '..', 'playwright-output'),
  use: { headless: true, browserName: 'chromium', deviceScaleFactor: 1, trace: 'off', screenshot: 'off' },
};
