import {beforeAll, describe, expect, it, vi} from "vitest";

/*
 * Server-delivered iceServers on the controller page. The page is a
 * DOM-coupled IIFE, so — like party-controller-lan.test.ts — these tests
 * reach its pure hoisted seams through the exposeInternals flag: the strict
 * iceServers validator and the redeem-response normalizer that carries the
 * validated array into controllerInfo. The selection rule under test is the
 * degradation doctrine: a validated server list is preferred, and absent OR
 * malformed config silently falls back to the built-in STUN (never an error).
 */

interface IceInternals {
  normalizedIceServers(value: unknown): readonly Record<string, unknown>[] | null;
  normalizedControllerInfo(value: unknown): Record<string, unknown> | null;
}

let internals: IceInternals;

beforeAll(async () => {
  vi.stubGlobal("location", {
    hash: "", pathname: "/controller/", search: "",
    href: "https://play.example.com/controller/", replace() {},
  });
  vi.stubGlobal("history", {replaceState() {}});
  (globalThis as Record<string, unknown>).__mdkrControllerTestConfig =
    {exposeInternals: true};
  await import("../../../dist/web/controller/controller.js");
  internals = (globalThis as Record<string, unknown>)
    .__mdkrControllerInternals as unknown as IceInternals;
  vi.unstubAllGlobals();
});

const stunEntry = {urls: "stun:stun.cloudflare.com:3478"};
const turnEntry = {
  urls: ["turn:turn.cloudflare.com:3478?transport=udp",
    "turns:turn.cloudflare.com:5349?transport=tcp"],
  username: "minted-user", credential: "minted-secret",
};

const redeemBase = {
  controllerId: "A".repeat(22), credential: "B".repeat(43),
  roomId: "C".repeat(22), protocol: 2, hostPublicKey: "D".repeat(87),
};

describe("controller-page iceServers validation", () => {
  it("accepts the worker's STUN + TURN shapes and freezes them", () => {
    const servers = internals.normalizedIceServers([stunEntry, turnEntry]);
    expect(servers).toEqual([
      {urls: ["stun:stun.cloudflare.com:3478"]},
      turnEntry,
    ]);
    expect(Object.isFrozen(servers)).toBe(true);
    expect(Object.isFrozen(servers![1])).toBe(true);
  });

  it("refuses anything that is not a strict iceServers array", () => {
    for (const bad of [
      undefined, null, "stun:stun.cloudflare.com:3478", {}, [],
      [{...stunEntry, unknownField: true}],
      [{urls: "http://stun.cloudflare.com:3478"}],
      [{urls: []}],
      [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
        username: "user-without-credential"}],
      [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
        username: "", credential: "secret"}],
      [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
        username: "x".repeat(600), credential: "secret"}],
      // Credentials are TURN-scoped: a credentialed entry naming any
      // stun url — alone or mixed in — refuses the whole list.
      [{urls: ["stun:stun.cloudflare.com:3478"],
        username: "minted-user", credential: "minted-secret"}],
      [{urls: ["stun:stun.cloudflare.com:3478",
        "turn:turn.cloudflare.com:3478?transport=udp"],
        username: "minted-user", credential: "minted-secret"}],
      Array.from({length: 9}, () => stunEntry),
    ]) {
      expect(internals.normalizedIceServers(bad)).toBeNull();
    }
  });
});

describe("controller-page redeem response iceServers", () => {
  it("keeps a validated server list on the controller info", () => {
    const info = internals.normalizedControllerInfo(
      {...redeemBase, iceServers: [stunEntry, turnEntry]});
    expect(info).not.toBeNull();
    expect(info!.iceServers).toEqual([
      {urls: ["stun:stun.cloudflare.com:3478"]},
      turnEntry,
    ]);
  });

  it("degrades absent or malformed iceServers to no list, never a refusal", () => {
    const absent = internals.normalizedControllerInfo({...redeemBase});
    expect(absent).not.toBeNull();
    expect(absent!.iceServers).toBeUndefined();
    const malformed = internals.normalizedControllerInfo(
      {...redeemBase, iceServers: [{urls: "http://not-ice"}]});
    expect(malformed).not.toBeNull();
    expect(malformed!.iceServers).toBeUndefined();
  });

  it("still refuses a response whose required identity fields are wrong", () => {
    expect(internals.normalizedControllerInfo(
      {...redeemBase, protocol: 1, iceServers: [stunEntry]})).toBeNull();
    expect(internals.normalizedControllerInfo(
      {...redeemBase, iceServers: [stunEntry], extra: true})).toBeNull();
  });
});
