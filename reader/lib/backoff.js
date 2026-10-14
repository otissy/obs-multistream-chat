'use strict';
// Capped exponential backoff with jitter. attempt starts at 0.
function delayMs(attempt, baseMs = 5000, capMs = 300000, rnd = Math.random) {
  const exp = Math.min(capMs, baseMs * Math.pow(2, Math.max(0, attempt)));
  const jitter = exp * 0.25 * (rnd() * 2 - 1); // +/- 25 %
  return Math.max(1000, Math.round(Math.min(capMs, exp + jitter)));
}
module.exports = { delayMs };
