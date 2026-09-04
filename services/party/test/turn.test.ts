import {env} from "cloudflare:workers";
import {runInDurableObject} from "cloudflare:test";
import {describe, expect, it} from "vitest";
import {INTERNAL_API_HEADER} from "../src/internal-api";
import {TURN_REFRESH_LOCKOUT_MS, TURN_TTL_MS, mintTurnIceServers,
  reserveTurnMint, roomIceServers, stunIceServers, turnConfigured}
  from "../src/turn";
import type {Env} from "../src/types";

const bindings = env as unknown as Env;
const turnEnv: Env = {...bindings,
  TURN_KEY_ID: "0123456789abcdef0123456789abcdef",
  TURN_API_TOKEN: "turn-api-fixture-token-0123456789abcdef"};

const stunEntries = [
  {urls: "stun:stun.cloudflare.com:3478"},
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
  it("always serves the fixed Cloudflare STUN server, and only that one", () => {
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

  it("keeps credentials on turn/turns urls only, stripping stun from a mixed entry", async () => {
    /* The provider may repeat its stun urls inside the credentialed entry;
     * forwarding one would hand relay credentials to a non-TURN server. */
    const {fetcher} = mintStub(201, {iceServers: [
      {urls: ["stun:stun.cloudflare.com:3478",
        "turn:turn.cloudflare.com:3478?transport=udp",
        "turns:turn.cloudflare.com:5349?transport=tcp"],
      username: "minted-user", credential: "minted-secret"},
    ]});
    expect(await mintTurnIceServers(turnEnv, fetcher)).toEqual(mintedEntries);
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
      // Credentials never ride on a stun-only entry: nothing usable.
      [201, {iceServers: [{urls: ["stun:stun.cloudflare.com:3478"],
        username: "user", credential: "secret"}]}],
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

  /* F2 remedy contract: the half-TTL refresh serves the still-valid cached
   * credentials immediately (≥ TTL/2 validity remains at the refresh point)
   * and re-mints in the background — never inside the caller's join/redeem
   * response. The seams.background hook is how these tests observe the
   * floating task the production DO simply lets run. */
  it("re-mints once less than half the TTL window remains, in the background",
      async () => {
    const {calls, fetcher} = mintStub(201, providerResponse);
    const now = Date.now();
    const staleEntries = [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
      username: "stale-user", credential: "stale-secret"}];
    const tasks: Promise<boolean>[] = [];
    await scratchStorage("turn-refresh-unit", async storage => {
      await storage.put("turnIceServers",
        {iceServers: staleEntries, expiresAt: now + TURN_TTL_MS / 2 - 1});
      const served = await roomIceServers(turnEnv, storage,
        {reserve: async () => true, fetcher, now,
          background: task => tasks.push(task)});
      /* The caller gets the still-valid cached set, not the refresh. */
      expect(served).toEqual([...stunEntries, ...staleEntries]);
      expect(tasks).toHaveLength(1);
      expect(await tasks[0]!).toBe(true);
      /* The background refresh landed the fresh set for later joins. */
      const refreshed = await roomIceServers(turnEnv, storage,
        {reserve: async () => {
          throw new Error("a refreshed cache hit must not charge the budget");
        }, fetcher: refusingFetcher(), now: now + 1_000});
      expect(refreshed).toEqual([...stunEntries, ...mintedEntries]);
    });
    expect(calls).toHaveLength(1);
  });

  it("never blocks a join/redeem response on a slow refresh mint", async () => {
    const now = Date.now();
    const staleEntries = [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
      username: "stale-user", credential: "stale-secret"}];
    let releaseMint = () => {};
    const mintGate = new Promise<void>(resolve => { releaseMint = resolve; });
    const calls: {url: string}[] = [];
    const fetcher = (async (input: RequestInfo | URL) => {
      calls.push({url: String(input)});
      await mintGate;
      return new Response(JSON.stringify(providerResponse), {status: 201});
    }) as typeof fetch;
    const tasks: Promise<boolean>[] = [];
    await scratchStorage("turn-nonblocking-unit", async storage => {
      await storage.put("turnIceServers",
        {iceServers: staleEntries, expiresAt: now + 60_000});
      /* The provider is still hanging (up to its 5 s cap in production); the
       * room's answer must win this race with the cached credentials. */
      const raced = await Promise.race([
        roomIceServers(turnEnv, storage, {reserve: async () => true, fetcher,
          now, background: task => tasks.push(task)}),
        new Promise(resolve =>
          setTimeout(() => resolve("mint_blocked_the_response"), 250)),
      ]);
      expect(raced).toEqual([...stunEntries, ...staleEntries]);
      releaseMint();
      expect(tasks).toHaveLength(1);
      expect(await tasks[0]!).toBe(true);
      expect(await storage.get("turnIceServers")).toEqual(
        {iceServers: mintedEntries, expiresAt: now + TURN_TTL_MS});
    });
    expect(calls).toHaveLength(1);
  });

  it("keeps the background refresh single-flight inside one live object",
      async () => {
    const now = Date.now();
    const staleEntries = [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
      username: "stale-user", credential: "stale-secret"}];
    const {calls, fetcher} = mintStub(201, providerResponse);
    let reserves = 0;
    const tasks: Promise<boolean>[] = [];
    await scratchStorage("turn-single-flight-unit", async storage => {
      await storage.put("turnIceServers",
        {iceServers: staleEntries, expiresAt: now + 60_000});
      const seams = {reserve: async () => { reserves++; return true; }, fetcher,
        now, background: (task: Promise<boolean>) => tasks.push(task)};
      const [first, second] = await Promise.all([
        roomIceServers(turnEnv, storage, seams),
        roomIceServers(turnEnv, storage, seams),
      ]);
      expect(first).toEqual([...stunEntries, ...staleEntries]);
      expect(second).toEqual([...stunEntries, ...staleEntries]);
      await Promise.all(tasks);
    });
    /* Two concurrent stale serves, ONE charged mint. */
    expect(reserves).toBe(1);
    expect(calls).toHaveLength(1);
  });

  it("survives hibernation losing the in-memory flag: the stored refresh " +
     "stamp still gates a rehydrated object", async () => {
    const now = Date.now();
    const staleEntries = [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
      username: "stale-user", credential: "stale-secret"}];
    let releaseMint = () => {};
    const mintGate = new Promise<void>(resolve => { releaseMint = resolve; });
    let reserves = 0;
    const calls: {url: string}[] = [];
    const fetcher = (async (input: RequestInfo | URL) => {
      calls.push({url: String(input)});
      await mintGate;
      return new Response(JSON.stringify(providerResponse), {status: 201});
    }) as typeof fetch;
    await scratchStorage("turn-hibernation-unit", async storage => {
      await storage.put("turnIceServers",
        {iceServers: staleEntries, expiresAt: now + 60_000});
      const tasks: Promise<boolean>[] = [];
      let noteReserveCharged = () => {};
      const reserveCharged = new Promise<void>(resolve => {
        noteReserveCharged = resolve;
      });
      const first = await roomIceServers(turnEnv, storage,
        {reserve: async () => {
          reserves++;
          noteReserveCharged();
          return true;
        }, fetcher, now, background: task => tasks.push(task)});
      expect(first).toEqual([...stunEntries, ...staleEntries]);
      /* The reserve runs only after the refresh stamp is durable, so a
       * rehydrated object (fresh in-memory state over the same storage — the
       * facade's new identity models the post-hibernation object) must fall
       * back to the stamp and start no second mint while one is in flight. */
      await reserveCharged;
      expect(reserves).toBe(1);
      const rehydrated = {
        get: (key: string) => storage.get(key),
        put: (key: string, value: unknown) => storage.put(key, value),
      } as unknown as DurableObjectStorage;
      const tasksAfterWake: Promise<boolean>[] = [];
      const second = await roomIceServers(turnEnv, rehydrated,
        {reserve: async () => { reserves++; return true; },
          fetcher: refusingFetcher(calls),
          now: now + TURN_REFRESH_LOCKOUT_MS - 1,
          background: task => tasksAfterWake.push(task)});
      expect(second).toEqual([...stunEntries, ...staleEntries]);
      for (const task of tasksAfterWake) expect(await task).toBe(false);
      releaseMint();
      await Promise.all(tasks);
      expect(await storage.get("turnIceServers")).toEqual(
        {iceServers: mintedEntries, expiresAt: now + TURN_TTL_MS});
    });
    expect(reserves).toBe(1);
    expect(calls).toHaveLength(1);
  });

  it("degrades a refused or failed refresh to the best still-valid answer", async () => {
    const now = Date.now();
    const staleEntries = [{urls: ["turn:turn.cloudflare.com:3478?transport=udp"],
      username: "stale-user", credential: "stale-secret"}];
    await scratchStorage("turn-refusal-unit", async storage => {
      /* Budget refusal with nothing cached: STUN-only, and no provider call
       * because a refusal must charge and cost nothing. */
      const refused = await roomIceServers(turnEnv, storage,
        {reserve: async () => false, fetcher: refusingFetcher(), now});
      expect(refused).toEqual(stunEntries);
      /* Budget refusal with unexpired credentials cached: keep serving them;
       * the refused background refresh charges nothing and mints nothing. */
      await storage.put("turnIceServers",
        {iceServers: staleEntries, expiresAt: now + 60_000});
      const tasks: Promise<boolean>[] = [];
      const refusedWithCache = await roomIceServers(turnEnv, storage,
        {reserve: async () => false, fetcher: refusingFetcher(), now,
          background: task => tasks.push(task)});
      expect(refusedWithCache).toEqual([...stunEntries, ...staleEntries]);
      for (const task of tasks) expect(await task).toBe(false);
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
    /* Provider failure during a background refresh: the caller already got
     * the cached set; the failed task leaves it in place for the next join. */
    await scratchStorage("turn-failed-refresh-unit", async storage => {
      await storage.put("turnIceServers",
        {iceServers: staleEntries, expiresAt: now + 60_000});
      const {fetcher: failing} = mintStub(500, {error: "unavailable"});
      const tasks: Promise<boolean>[] = [];
      const failedRefresh = await roomIceServers(turnEnv, storage,
        {reserve: async () => true, fetcher: failing, now,
          background: task => tasks.push(task)});
      expect(failedRefresh).toEqual([...stunEntries, ...staleEntries]);
      expect(tasks).toHaveLength(1);
      expect(await tasks[0]!).toBe(false);
      expect(await storage.get("turnIceServers")).toEqual(
        {iceServers: staleEntries, expiresAt: now + 60_000});
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
