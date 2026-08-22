import {env} from "cloudflare:workers";
import {runInDurableObject} from "cloudflare:test";
import {describe, expect, it} from "vitest";
import {INTERNAL_API_HEADER} from "../src/internal-api";
import {TURN_TTL_MS, mintTurnIceServers, reserveTurnMint, roomIceServers,
  stunIceServers, turnConfigured} from "../src/turn";
import type {Env} from "../src/types";

const bindings = env as unknown as Env;
const turnEnv: Env = {...bindings,
  TURN_KEY_ID: "0123456789abcdef0123456789abcdef",
  TURN_API_TOKEN: "turn-api-fixture-token-0123456789abcdef"};

const stunEntries = [
  {urls: "stun:stun.cloudflare.com:3478"},
  {urls: "stun:stun.l.google.com:19302"},
];

/* The documented Cloudflare Realtime response: one uncredentialed STUN entry
 * plus one credentialed TURN entry whose urls include the port-53 variants
 * the provider itself warns are browser-blocked. */
const providerResponse = {iceServers: [
  {urls: ["stun:stun.cloudflare.com:3478", "stun:stun.cloudflare.com:53"]},
  {urls: ["turn:turn.cloudflare.com:3478?transport=udp",
    "turn:turn.cloudflare.com:53?transport=udp",
    "turns:turn.cloudflare.com:5349?transport=tcp"],
  username: "minted-user", credential: "minted-secret", future: "ignored"},
]};

const mintedEntries = [
  {urls: ["turn:turn.cloudflare.com:3478?transport=udp",
    "turns:turn.cloudflare.com:5349?transport=tcp"],
  username: "minted-user", credential: "minted-secret"},
];

function mintStub(status: number, body: unknown) {
  const calls: {url: string; init: RequestInit | undefined}[] = [];
  const fetcher = (async (input: RequestInfo | URL, init?: RequestInit) => {
    calls.push({url: String(input), init});
    return new Response(
      typeof body === "string" ? body : JSON.stringify(body), {status});
  }) as typeof fetch;
  return {calls, fetcher};
}

function refusingFetcher(calls: {url: string}[] = []) {
  return (async (input: RequestInfo | URL) => {
    calls.push({url: String(input)});
    throw new Error("no external fetch expected");
  }) as typeof fetch;
}

function scratchStorage<T>(name: string,
                           run: (storage: DurableObjectStorage) => Promise<T>) {
  const stub = bindings.PARTY_ROOMS.get(bindings.PARTY_ROOMS.idFromName(name));
  return runInDurableObject(stub, (_instance, state) => run(state.storage));
}

describe("zero-cost TURN credential minting", () => {
  it("always serves the two fixed STUN servers", () => {
    expect(stunIceServers()).toEqual(stunEntries);
  });

  it("recognises TURN as configured only with both well-formed secrets", () => {
    expect(turnConfigured(bindings)).toBe(false);
    expect(turnConfigured({...bindings,
      TURN_KEY_ID: turnEnv.TURN_KEY_ID} as Env)).toBe(false);
    expect(turnConfigured({...bindings,
      TURN_API_TOKEN: turnEnv.TURN_API_TOKEN} as Env)).toBe(false);
    expect(turnConfigured({...turnEnv,
      TURN_KEY_ID: "../../../escape"} as Env)).toBe(false);
    expect(turnConfigured({...turnEnv, TURN_API_TOKEN: "short"} as Env)).toBe(false);
    expect(turnConfigured(turnEnv)).toBe(true);
  });

  it("mints against the documented endpoint and rebuilds only known fields", async () => {
    const {calls, fetcher} = mintStub(201, providerResponse);
    const minted = await mintTurnIceServers(turnEnv, fetcher);
    expect(minted).toEqual(mintedEntries);
    expect(calls).toHaveLength(1);
    expect(calls[0]!.url).toBe("https://rtc.live.cloudflare.com/v1/turn/keys/" +
      `${turnEnv.TURN_KEY_ID}/credentials/generate-ice-servers`);
    expect(calls[0]!.init?.method).toBe("POST");
    const headers = new Headers(calls[0]!.init?.headers);
    expect(headers.get("authorization")).toBe(`Bearer ${turnEnv.TURN_API_TOKEN}`);
    expect(headers.get("content-type")).toBe("application/json");
    expect(JSON.parse(String(calls[0]!.init?.body))).toEqual({ttl: 14_400});
  });

  it("treats every mint failure as no-TURN, never an error", async () => {
    for (const [status, body] of [
      [500, providerResponse],
      [403, {error: "forbidden"}],
      [201, "not json {"],
      [201, {iceServers: "not-an-array"}],
      [201, {iceServers: []}],
      // No credentialed entry at all.
      [201, {iceServers: [{urls: ["stun:stun.cloudflare.com:3478"]}]}],
      // A credential without its username is malformed, not usable.
      [201, {iceServers: [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
        credential: "half"}]}],
      // Only browser-blocked port-53 urls survive validation: nothing usable.
      [201, {iceServers: [{urls: ["turn:turn.cloudflare.com:53?transport=udp"],
        username: "user", credential: "secret"}]}],
      [201, {iceServers: [{urls: ["http://turn.cloudflare.com:3478"],
        username: "user", credential: "secret"}]}],
    ] as const) {
      const {fetcher} = mintStub(status, body);
      expect(await mintTurnIceServers(turnEnv, fetcher)).toBeNull();
    }
    const failing = (async () => {
      throw new Error("network unreachable");
    }) as unknown as typeof fetch;
    expect(await mintTurnIceServers(turnEnv, failing)).toBeNull();
  });

  it("degrades to STUN-only when the secrets are absent, without any fetch", async () => {
    const calls: {url: string}[] = [];
    const served = await scratchStorage("turn-absent-unit", storage =>
      roomIceServers(bindings, storage, {reserve: async () => true,
        fetcher: refusingFetcher(calls)}));
    expect(served).toEqual(stunEntries);
    expect(calls).toHaveLength(0);
  });

  it("mints once per TTL window and serves later joins from the room cache", async () => {
    const {calls, fetcher} = mintStub(201, providerResponse);
    const now = Date.now();
    await scratchStorage("turn-cache-unit", async storage => {
      const first = await roomIceServers(turnEnv, storage,
        {reserve: async () => true, fetcher, now});
      expect(first).toEqual([...stunEntries, ...mintedEntries]);
      const cached = await storage.get<{iceServers: unknown; expiresAt: number}>(
        "turnIceServers");
      expect(cached).toEqual({iceServers: mintedEntries,
        expiresAt: now + TURN_TTL_MS});
      /* N joins must not equal N mints: the second consumer inside the TTL
       * window is answered from storage without a provider call. */
      const second = await roomIceServers(turnEnv, storage,
        {reserve: async () => {
          throw new Error("a cache hit must not charge the budget");
        }, fetcher: refusingFetcher(), now: now + 1_000});
      expect(second).toEqual([...stunEntries, ...mintedEntries]);
    });
    expect(calls).toHaveLength(1);
  });

  it("re-mints once less than half the TTL window remains", async () => {
    const {calls, fetcher} = mintStub(201, providerResponse);
    const now = Date.now();
    await scratchStorage("turn-refresh-unit", async storage => {
      await storage.put("turnIceServers", {iceServers: [{
        urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
        username: "stale-user", credential: "stale-secret",
      }], expiresAt: now + TURN_TTL_MS / 2 - 1});
      const served = await roomIceServers(turnEnv, storage,
        {reserve: async () => true, fetcher, now});
      expect(served).toEqual([...stunEntries, ...mintedEntries]);
    });
    expect(calls).toHaveLength(1);
  });

  it("degrades a refused or failed refresh to the best still-valid answer", async () => {
    const now = Date.now();
    await scratchStorage("turn-refusal-unit", async storage => {
      /* Budget refusal with nothing cached: STUN-only, and no provider call
       * because a refusal must charge and cost nothing. */
      const refused = await roomIceServers(turnEnv, storage,
        {reserve: async () => false, fetcher: refusingFetcher(), now});
      expect(refused).toEqual(stunEntries);
      /* Budget refusal with unexpired credentials cached: keep serving them. */
      const staleEntries = [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
        username: "stale-user", credential: "stale-secret"}];
      await storage.put("turnIceServers",
        {iceServers: staleEntries, expiresAt: now + 60_000});
      const refusedWithCache = await roomIceServers(turnEnv, storage,
        {reserve: async () => false, fetcher: refusingFetcher(), now});
      expect(refusedWithCache).toEqual([...stunEntries, ...staleEntries]);
      /* Provider failure during a refresh: same degradation ladder. */
      const {fetcher: failing} = mintStub(500, {error: "unavailable"});
      const failedRefresh = await roomIceServers(turnEnv, storage,
        {reserve: async () => true, fetcher: failing, now});
      expect(failedRefresh).toEqual([...stunEntries, ...staleEntries]);
      /* An expired or corrupt record is never served. */
      await storage.put("turnIceServers",
        {iceServers: staleEntries, expiresAt: now - 1});
      const expired = await roomIceServers(turnEnv, storage,
        {reserve: async () => false, fetcher: refusingFetcher(), now});
      expect(expired).toEqual(stunEntries);
      await storage.put("turnIceServers", {iceServers: "corrupt", expiresAt:
        now + TURN_TTL_MS});
      const corrupt = await roomIceServers(turnEnv, storage,
        {reserve: async () => false, fetcher: refusingFetcher(), now});
      expect(corrupt).toEqual(stunEntries);
    });
  });

  it("charges the mint through the fixed turnMint budget shape", async () => {
    expect(await reserveTurnMint(bindings)).toBe(true);
    const day = new Date().toISOString().slice(0, 10);
    const health = await bindings.PARTY_BUDGETS.get(
      bindings.PARTY_BUDGETS.idFromName(day)).fetch("https://budget/health",
      {headers: {[INTERNAL_API_HEADER]: "1"}});
    expect(await health.json()).toMatchObject({
      reservations: {turnMint: 1},
      admitted: {pairingUnits: 0, controlUnits: 2},
    });
  });
});
