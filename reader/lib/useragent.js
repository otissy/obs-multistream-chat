'use strict';
// User-agent compatibility setting: reader.userAgent = "chrome" (default) | "headless".
//
// Why: with Chrome's default headless user agent ("HeadlessChrome/<ver>") YouTube's live_chat popout
// answers with an empty stub page. "chrome" sends the installed Chrome's OWN user agent, taken from
// Browser.getVersion, with only the word "Headless" removed. Nothing else is changed: no
// navigator.webdriver patch, no plugin / WebGL / canvas spoofing, no fingerprint games. A site can
// still tell this is automation, and may treat automated reading as against its terms (docs/limits.md).

// -> null for mode "headless" (leave Chrome as it is), else
//    { userAgent, userAgentMetadata, version, major } for Emulation.setUserAgentOverride.
// versionInfo is the result of Browser.getVersion: { product: 'HeadlessChrome/126.0.6478.127', userAgent: '...' }.
function computeUserAgent(versionInfo, mode) {
  if (mode === 'headless') return null;
  const ua = String((versionInfo && versionInfo.userAgent) || '');
  const m = /HeadlessChrome\/(\d+)\.(\d+\.\d+\.\d+)/.exec(ua) || /Chrome\/(\d+)\.(\d+\.\d+\.\d+)/.exec(ua);
  if (!ua || !m) return null; // unknown shape: do not guess
  const major = m[1];
  // Chrome reduces the version in its user agent string (141.0.0.0); the real full version is in `product`
  const pm = /Chrome\/(\d+\.\d+\.\d+\.\d+)/.exec(String((versionInfo && versionInfo.product) || ''));
  const full = pm && pm[1].split('.')[0] === major ? pm[1] : m[1] + '.' + m[2];
  const userAgent = ua.replace('HeadlessChrome/', 'Chrome/');
  const win = /Windows NT (\d+)\.(\d+)/.exec(ua);
  const platform = win ? 'Windows' : /Macintosh/.test(ua) ? 'macOS' : /Linux/.test(ua) ? 'Linux' : 'Windows';
  const brands = [{ brand: 'Not.A/Brand', version: '99' }, { brand: 'Chromium', version: major }, { brand: 'Google Chrome', version: major }];
  return {
    userAgent, version: full, major,
    userAgentMetadata: {
      brands,
      fullVersionList: [{ brand: 'Not.A/Brand', version: '99.0.0.0' }, { brand: 'Chromium', version: full }, { brand: 'Google Chrome', version: full }],
      platform, platformVersion: win ? win[1] + '.' + win[2] + '.0' : '0.0.0',
      architecture: 'x86', model: '', mobile: false, bitness: '64', wow64: false,
    },
  };
}

module.exports = { computeUserAgent };
