// S1/S4 node harness for the Online Room page itself (online-room.js).
//
// S1 — fallback-only /state refresh: the room object pushes the post-command
// state over /connect BEFORE the command response returns, so the page must
// trust the push and fetch /state at most once, only when the push covering
// the accepted command's revision fails to arrive inside the fallback window.
// S4 — typed 4000-class closes (host_closed / room_expired) are terminal and
// must reach the terminal card immediately, never the reconnect ladder.
//
// The page is a DOM-coupled IIFE; like controller.js's exposeInternals
// precedent it installs test seams on globalThis when the loopback test
// config asks for them. This harness stubs the small DOM surface the module
// touches at load, uses the REAL presenter and live-state boundary modules,
// and drives the real sendLiveCommand/connectLiveSocket paths through fixture
// request/subscribe transports. The Wasm model is absent (api === null), so
// projection intentionally fails after state custody is updated — request
// counting and room custody, the contracts under test, are unaffected.

import assert from "node:assert/strict";
import {createRequire} from "node:module";
import {fileURLToPath} from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

const ORIGIN = "https://play.example";
const FALLBACK_MS = 150;
const roomName = "abcdefghijklmnopqrstuv";
const credential = "C".repeat(43);
const compatibility = Object.freeze({
  protocolVersion: 1,
  buildId: Object.freeze(Array.from({length: 16}, (_, index) => index + 1)),
  gameplayDigest: Object.freeze(
    Array.from({length: 32}, (_, index) => 128 + index)),
  romRevision: 1,
  cadenceHz: 30,
});

function member(endpointId, overrides = {}) {
  return {endpointId, seatCount: 1, connected: true, ready: false,
    loaded: false, ...overrides};
}

function seat(endpointId, overrides = {}) {
  return {endpointId, selectionRevision: 0, voteTrack: null, localIndex: 0,
    characterId: null, vehicleId: null, ...overrides};
}

function snapshot(overrides = {}) {
  const {lobby: lobbyOverrides = {}, ...rootOverrides} = overrides;
  const lobby = {protocolVersion: 1, revision: 1, matchEpoch: 0,
    leaderGeneration: 1, roomId: "42", leaderEndpointId: "100",
    phase: "lobby", compatibility: structuredClone(compatibility),
    members: [member("100")], seats: [seat("100")], selectedTrack: null,
    selectedVehicleMask: 0, ...lobbyOverrides};
  return {type: "match_state", schemaVersion: 1,
    expiresAt: 2_000_000_000_000, inviteExpiresAt: 1_999_999_000_000,
    inviteGeneration: 1, closedReason: null, lobby, controlTail: [],
    ...rootOverrides};
}

function createWire() {
  return {...snapshot(), roomId: roomName, endpointId: "100", credential,
    fallbackCode: "123456", inviteExpiresInMs: 600_000,
    inviteUrl: `${ORIGIN}/room/#match=${"A".repeat(43)}`};
}

/* The public push shape the object broadcasts: no identity keys, the lobby
 * advanced by one accepted set_character. */
function coveringPush(revision) {
  return snapshot({lobby: {revision,
    seats: [seat("100", {characterId: 3, selectionRevision: 1})]}});
}

// ---------------------------------------------------------------------------
// Minimal DOM/global surface for loading the page IIFE outside a browser.
// ---------------------------------------------------------------------------

function fakeElement() {
  return {
    hidden: false, disabled: false, open: false, textContent: "",
    dataset: {}, style: {}, value: "", src: "",
    addEventListener() {}, removeEventListener() {},
    append() {}, appendChild() {}, replaceChildren() {},
    setAttribute() {}, removeAttribute() {}, getAttribute() { return null; },
    focus() {}, click() {}, close() {}, showModal() {},
    closest() { return null; },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    getContext() { return null; },
  };
}

const elements = new Map();
globalThis.document = {
  getElementById(id) {
    if (!elements.has(id)) elements.set(id, fakeElement());
    return elements.get(id);
  },
  createElement() { return fakeElement(); },
  createTextNode(text) { return {textContent: text}; },
  querySelector() { return null; },
  head: {append() {}},
  body: {append() {}, appendChild() {}},
  currentScript: null,
  activeElement: null,
};
globalThis.location = {hash: "", pathname: "/", search: "",
  hostname: "127.0.0.1", origin: "http://127.0.0.1:8080",
  href: "http://127.0.0.1:8080/"};
globalThis.history = {replaceState() {}};
globalThis.requestAnimationFrame = (fn) => setTimeout(fn, 0);
if (typeof globalThis.addEventListener !== "function") {
  globalThis.addEventListener = () => {};
}

globalThis.__mdkrOnlineRoomTestConfig = {liveFixture: true,
  exposeInternals: true, refreshFallbackMs: FALLBACK_MS};
globalThis.__mdkrOnlineRoomSurfaceTest = true;

const liveState = require(path.join(
  root, "dist/web/online/online-room-live-state.js"));
require(path.join(root, "dist/web/online/online-room-presenter.js"));
require(path.join(root, "dist/web/online/online-room.js"));

const internals = globalThis.__mdkrOnlineRoomInternals;
const testState = globalThis.__mdkrOnlineRoomTestState;
assert.ok(internals, "online-room.js did not expose its loopback test seams");
assert.equal(internals.refreshFallbackMs, FALLBACK_MS,
  "test fallback window was not honored");

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const requests = () => testState.requests || [];
const stateRequests = () =>
  requests().filter((entry) => entry.path.endsWith("/state")).length;
const commandRequests = () =>
  requests().filter((entry) => entry.path.endsWith("/command")).length;

// ---------------------------------------------------------------------------
// Fixture transport: request() answers commands with the reducer's MatchStep
// shape; subscribe() hands the harness the push and close callbacks so it can
// play the room object's broadcast-before-response behavior — or suppress it.
// ---------------------------------------------------------------------------

const transport = {pushState: null, pushClose: null,
  deliverPush: true, pushDelayMs: 0, revision: 1};

const fixtureConfig = Object.freeze({
  origin: ORIGIN,
  compatibility,
  timeoutMs: 5000,
  fixture: true,
  request: async (requestPath, {body}) => {
    if (requestPath.endsWith("/command")) {
      const revision = Number(body.expectedRevision) + 1;
      transport.revision = revision;
      const respond = () => ({accepted: true, duplicate: false,
        leaderChanged: false, error: "ok", revision, matchEpoch: 0,
        leaderEndpointId: "100", selectedTrack: null, selectedVehicleMask: 0,
        previousRevision: Number(body.expectedRevision)});
      if (transport.deliverPush && transport.pushState) {
        if (transport.pushDelayMs > 0) {
          const handler = transport.pushState;
          setTimeout(() => handler(coveringPush(revision)),
            transport.pushDelayMs);
        } else {
          // The object broadcasts to the room's sockets before the HTTP
          // response is even returned (match-room.ts): push first.
          transport.pushState(coveringPush(revision));
        }
      }
      return respond();
    }
    if (requestPath.endsWith("/state")) {
      return coveringPush(transport.revision);
    }
    throw Object.assign(new Error("not_found"), {code: "not_found"});
  },
  subscribe: (_room, onState, onClose) => {
    transport.pushState = onState;
    transport.pushClose = onClose;
    return {close() {}};
  },
});

function adoptFreshRoom() {
  const ingested = liveState.ingest(createWire(), null, null, compatibility,
    ORIGIN, Date.now(), 0);
  assert.ok(ingested && ingested.state,
    "fixture wire state failed live-state ingest");
  transport.pushState = null;
  transport.pushClose = null;
  transport.deliverPush = true;
  transport.pushDelayMs = 0;
  transport.revision = 1;
  internals.adoptLiveFixture(fixtureConfig, ingested.state);
  assert.equal(internals.connectLiveSocket(), true,
    "fixture live socket did not connect");
  assert.ok(transport.pushState, "subscribe did not capture the push handler");
}

async function run() {
  // -------------------------------------------------------------------------
  // S1a: the covering push arrives promptly -> the accepted command performs
  // ZERO /state fetches, even after the fallback window has fully elapsed.
  // -------------------------------------------------------------------------
  adoptFreshRoom();
  {
    const statesBefore = stateRequests();
    const commandsBefore = commandRequests();
    const accepted = await internals.sendLiveCommand("set_character", 3, 0);
    assert.equal(accepted, true, "pushed coverage did not settle the command");
    await sleep(FALLBACK_MS + 120);
    assert.equal(commandRequests() - commandsBefore, 1,
      "expected exactly one command request");
    assert.equal(stateRequests() - statesBefore, 0,
      "a prompt push must suppress the /state refresh entirely");
  }

  // -------------------------------------------------------------------------
  // S1b: the push arrives late but inside the window -> still zero fetches.
  // -------------------------------------------------------------------------
  adoptFreshRoom();
  {
    transport.pushDelayMs = Math.floor(FALLBACK_MS / 3);
    const statesBefore = stateRequests();
    const accepted = await internals.sendLiveCommand("set_character", 3, 0);
    assert.equal(accepted, true,
      "an in-window push did not settle the command");
    await sleep(FALLBACK_MS + 120);
    assert.equal(stateRequests() - statesBefore, 0,
      "an in-window push must still suppress the /state refresh");
  }

  // -------------------------------------------------------------------------
  // S1c: the push is suppressed -> exactly ONE fallback /state fetch, and
  // only after the window has elapsed (never the old immediate refresh).
  // -------------------------------------------------------------------------
  adoptFreshRoom();
  {
    transport.deliverPush = false;
    const statesBefore = stateRequests();
    const startedAt = Date.now();
    await internals.sendLiveCommand("set_character", 3, 0);
    const elapsed = Date.now() - startedAt;
    assert.equal(stateRequests() - statesBefore, 1,
      "a suppressed push must cost exactly one fallback /state fetch");
    assert.ok(elapsed >= FALLBACK_MS - 25,
      `fallback fetch fired before the window elapsed (${elapsed}ms)`);
    await sleep(FALLBACK_MS + 120);
    assert.equal(stateRequests() - statesBefore, 1,
      "the fallback /state fetch must fire at most once");
  }

  // -------------------------------------------------------------------------
  // S4: typed 4000-class closes are terminal — the card the close named,
  // zero reconnect churn, room custody cleared. Mirrors party-host.js and
  // controller.js, which already discriminate exactly this pair.
  // -------------------------------------------------------------------------
  for (const [reason, presented] of [
    ["host_closed", "host_closed"],
    ["room_expired", "not_found"],
  ]) {
    adoptFreshRoom();
    const statesBefore = stateRequests();
    const errorsBefore = (testState.errors || []).length;
    transport.pushClose(4000, reason);
    assert.deepEqual(testState.errors.slice(errorsBefore), [presented],
      `4000 ${reason} must present the ${presented} terminal card at once`);
    assert.equal(internals.liveRoomSnapshot(), null,
      `4000 ${reason} must clear live room custody`);
    await sleep(400);
    assert.equal(stateRequests() - statesBefore, 0,
      `4000 ${reason} must never ride the reconnect ladder`);
  }

  // A generic transport loss still takes the ordinary reconnect ladder: the
  // terminal branch must not swallow recoverable closes.
  adoptFreshRoom();
  {
    const statesBefore = stateRequests();
    transport.pushClose(1006, "");
    await sleep(450);
    assert.ok(stateRequests() - statesBefore >= 1,
      "a generic close must still reconnect through the ladder");
    assert.ok(internals.liveRoomSnapshot(),
      "a generic close must not clear live room custody");
  }

  globalThis.MDKROnlineRoom.disable();
  console.log("online room client tests passed: fallback-only /state " +
    "refresh (prompt, in-window, suppressed) and typed 4000-class closes");
}

await run();
