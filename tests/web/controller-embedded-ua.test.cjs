"use strict";

// F13: pin the in-app webview User-Agent heuristic that feeds the controller
// page's "Continue in Safari or Chrome" card. The card is a HINT, so the
// heuristic must be precise: every known in-app browser where the phone's
// WebRTC pairing tends to fail is flagged, and every real Safari/Chrome (and
// the other real mobile browsers, which are full browsers even when they wrap
// WKWebView) must NOT be flagged -- a false positive would strand a browser
// that pairs fine on the recovery card.

const assert = require("node:assert/strict");
const path = require("node:path");

// Reach the DOM-free internals: an empty hash keeps the entry code from
// touching history, and exposeInternals returns before the first getElementById.
global.location = {hash: ""};
globalThis.__mdkrControllerTestConfig = {exposeInternals: true};
require(path.join(__dirname, "..", "..", "dist", "web", "controller",
  "controller.js"));
const internals = globalThis.__mdkrControllerInternals;
assert.ok(internals && typeof internals.embeddedWebviewUa === "function",
  "controller internals must expose embeddedWebviewUa");
const {embeddedWebviewUa} = internals;

// Positive cases: real in-app webview UA strings (the token each app injects).
const inApp = [
  // Facebook iOS in-app browser.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/21E236 [FBAN/FBIOS;FBAV/456.0.0.34.108;FBBV/...]",
  // Facebook Android in-app browser.
  "Mozilla/5.0 (Linux; Android 13; SM-S911B Build/TP1A) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/120.0.0.0 Mobile Safari/537.36 [FB_IAB/FB4A;FBAV/456.0.0.28.107;]",
  // Instagram iOS in-app browser.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/21E236 Instagram 322.0.0.29.111 (iPhone14,3; iOS 17_4; en_US)",
  // Generic Android System WebView (the "; wv" token real Chrome never has).
  "Mozilla/5.0 (Linux; Android 12; Pixel 6 Build/SP2A; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/119.0.0.0 Mobile Safari/537.36",
  // LINE in-app browser.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148 Safari/604.1 Line/13.13.0",
  // WeChat in-app browser.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148 MicroMessenger/8.0.40(0x18002832) NetType/WIFI Language/en",
  // Snapchat in-app browser.
  "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36 Snapchat/12.60.0.36",
  // TikTok in-app browser.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148 musical_ly_31.5.0 JsSdk/2.0 BytedanceWebview/d8a21c6",
  // Twitter/X in-app browser.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148 Twitter for iPhone",
  // Pinterest in-app browser.
  "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36 Pinterest/11.9",
  // LinkedIn in-app browser.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148 LinkedInApp",
  // KakaoTalk in-app browser.
  "Mozilla/5.0 (Linux; Android 12; SM-G991N) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36 KAKAOTALK 10.4.3",
];

// Negative cases: full browsers, including the ones that ride WKWebView but are
// real browsers (CriOS, FxiOS, EdgiOS) and desktop browsers. None may match.
const realBrowsers = [
  // iOS Safari.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Mobile/15E148 Safari/604.1",
  // Android Chrome (mobile).
  "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36",
  // Desktop Chrome.
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
  // Desktop Safari (macOS).
  "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Safari/605.1.15",
  // Chrome on iOS (CriOS) -- a real browser on WKWebView; WebRTC works.
  "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/120.0.6099.119 Mobile/15E148 Safari/604.1",
  // Firefox on iOS (FxiOS).
  "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) FxiOS/121.0 Mobile/15E148 Safari/605.1.15",
  // Firefox on Android.
  "Mozilla/5.0 (Android 13; Mobile; rv:121.0) Gecko/121.0 Firefox/121.0",
  // Edge on Android (EdgA).
  "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36 EdgA/120.0.0.0",
  // Samsung Internet.
  "Mozilla/5.0 (Linux; Android 13; SM-S911B) AppleWebKit/537.36 (KHTML, like Gecko) SamsungBrowser/23.0 Chrome/115.0.0.0 Mobile Safari/537.36",
  // Opera mobile.
  "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36 OPR/78.0.0.0",
  // Empty / missing UA must never be flagged.
  "",
];

for (const ua of inApp) {
  assert.equal(embeddedWebviewUa(ua), true,
    `expected in-app webview to be flagged: ${ua}`);
}
for (const ua of realBrowsers) {
  assert.equal(embeddedWebviewUa(ua), false,
    `real browser must not be flagged: ${ua}`);
}

console.log("controller_embedded_ua: PASS");
