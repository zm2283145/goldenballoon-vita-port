// ROM/browser-free fixture for the exact canonical shell diagnostic block.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const source = readFileSync(new URL("../dist/web/mdkr64-shell.js", import.meta.url), "utf8");
const begin = source.indexOf("// BEGIN bounded startup diagnostics");
const end = source.indexOf("// END bounded startup diagnostics.", begin);
assert.ok(begin >= 0 && end > begin, "canonical diagnostic block remains extractable");
const block = source.slice(begin, end) + "\nglobalThis.controls = startupDiagnostics;";

function fixture(testConfig) {
  let now = 0;
  let nextId = 0;
  const pending = new Map();
  const listeners = new Map();
  const context = vm.createContext({
    testConfig,
    performance: { now: () => now },
    document: {
      visibilityState: "visible", hasFocus: () => false,
      addEventListener(name, callback) { listeners.set(`document:${name}`, callback); },
      removeEventListener(name) { listeners.delete(`document:${name}`); },
    },
    requestAnimationFrame(callback) { pending.set(++nextId, callback); return nextId; },
    cancelAnimationFrame(id) { pending.delete(id); },
    addEventListener(name, callback) { listeners.set(name, callback); },
  });
  vm.runInContext(block, context);
  return {
    context, pending, listeners,
    advance(timestamp) {
      now = timestamp;
      const callbacks = [...pending.values()];
      pending.clear();
      for (const callback of callbacks) callback(timestamp);
    },
  };
}

for (const config of [null, {}, { startupDiagnostics: false }, { startupDiagnostics: "true" }]) {
  const off = fixture(config);
  assert.equal(off.context.controls, null);
  assert.equal(off.context.__mdkrStartupTrace, undefined);
  assert.equal(off.context.__mdkrStartupDiagnosticsSnapshot, undefined);
  assert.equal(off.pending.size, 0);
  assert.equal(off.listeners.size, 0);
}

const f = fixture({ startupDiagnostics: true });
const { controls } = f.context;
assert.equal(f.pending.size, 0, "installing the hook does not start observation");
controls.start();
assert.equal(f.pending.size, 1);
f.advance(16);
let snapshot = controls.snapshot();
assert.equal(snapshot.heartbeatCallbacks, 1);
assert.equal(snapshot.engineRafRequested, 0, "heartbeat is not an engine wait");
assert.equal(snapshot.engineRafResolved, 0);
controls.trace("raf-requested");
f.advance(32);
snapshot = controls.snapshot();
assert.equal(snapshot.heartbeatCallbacks, 2, "browser progresses during unresolved engine wait");
assert.equal(snapshot.engineRafRequested, 1);
assert.equal(snapshot.engineRafResolved, 0);
controls.trace("raf-resolved");
assert.equal(controls.snapshot().engineRafResolved, 1);
for (let i = 0; i < 80; i++) controls.trace(`stage-${i}`);
snapshot = controls.snapshot();
assert.equal(snapshot.events.length, 32);
assert.equal(snapshot.events[0].phase, "stage-48");
controls.trace("x".repeat(65));
controls.trace(null);
assert.equal(controls.snapshot().lastPhase, "stage-79");
snapshot.events[0].phase = "mutated-copy";
assert.equal(controls.snapshot().events[0].phase, "stage-48");
assert.equal(snapshot.hasFocus, false, "lack of focus does not suppress observation");
controls.stop("exited");
assert.equal(f.pending.size, 0);
assert.equal(controls.snapshot().active, false);
assert.equal(controls.snapshot().stoppedReason, "exited");
controls.start();
assert.equal(controls.snapshot().generation, 2);
assert.equal(controls.snapshot().events.length, 0);
assert.equal(controls.snapshot().engineRafRequested, 0);
assert.equal(f.pending.size, 1, "restart never duplicates the heartbeat");
controls.start();
assert.equal(f.pending.size, 1);
f.listeners.get("pagehide")();
assert.equal(f.pending.size, 0);
assert.equal(controls.snapshot().stoppedReason, "pagehide");
controls.start();
f.advance(600033);
assert.equal(f.pending.size, 0);
assert.equal(controls.snapshot().stoppedReason, "observation-limit");

// Pin the real synthetic helper's observation order without changing its rAF.
const waitBegin = source.indexOf("globalThis.__mdkrWaitAnimationFrame = async function () {");
const waitEnd = source.indexOf("\n};", waitBegin) + 3;
assert.ok(waitBegin >= 0 && waitEnd > waitBegin);
vm.runInContext(`
  const testRafDeltas = null;
  const testRafDeltasNs = null;
  let testActualRafLast = null;
  ${source.slice(waitBegin, waitEnd)}
`, f.context);
controls.start();
const wait = f.context.__mdkrWaitAnimationFrame();
assert.equal(controls.snapshot().engineRafRequested, 1);
assert.equal(controls.snapshot().engineRafResolved, 0);
assert.equal(f.pending.size, 2, "engine and observer own independent callbacks");
f.advance(600049);
assert.equal(await wait, 600049);
assert.equal(controls.snapshot().engineRafResolved, 1);
assert.equal(controls.snapshot().heartbeatCallbacks, 1);
controls.stop("exited");

controls.start();
f.context.document.visibilityState = "hidden";
const hiddenWait = f.context.__mdkrWaitAnimationFrame();
assert.equal(controls.snapshot().lastPhase, "raf-visibility-wait");
assert.equal(controls.snapshot().engineRafRequested, 0);
assert.equal(f.pending.size, 1, "hidden engine waits for visibility, not a new rAF");
f.context.document.visibilityState = "visible";
f.listeners.get("document:visibilitychange")();
await Promise.resolve();
assert.equal(f.listeners.has("document:visibilitychange"), false);
assert.equal(controls.snapshot().engineRafRequested, 1);
f.advance(600065);
assert.equal(await hiddenWait, 600065);
assert.equal(controls.snapshot().engineRafResolved, 1);
controls.stop("exited");

// Integration remains opt-in and lifecycle-owned, not a second engine loop.
assert.match(source, /if \(phase === "main-started"\) startupDiagnostics\.start\(\)/);
assert.match(source, /phase === "exited"[\s\S]*phase === "aborted"[\s\S]*startupDiagnostics\.stop\(phase\)/);
assert.match(source, /startupDiagnostics: startupDiagnostics \? startupDiagnostics\.snapshot\(\) : null/);
console.log("browser startup diagnostics fixture passed");
