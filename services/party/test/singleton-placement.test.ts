import {env} from "cloudflare:workers";
import {describe, expect, it} from "vitest";
import {reserveTurnMint} from "../src/turn";
import {DEFAULT_SINGLETON_LOCATION_HINT, SINGLETON_LOCATION_HINTS,
  singletonLocationHint, type Env} from "../src/types";

const bindings = env as unknown as Env;

/* S3a: the per-day budget object and the code directories are global
 * singletons whose placement is wherever the day's (or deployment's) first
 * request happened to land — potentially another continent from the whole
 * player base, taxing every serialized hop on every join step all day. Every
 * singleton get() now pins creation near the primary audience region via a
 * validated location hint; room objects stay creator-local and unhinted. */
describe("singleton Durable Object placement", () => {
  it("defaults the location hint to the primary audience region", () => {
    expect(DEFAULT_SINGLETON_LOCATION_HINT).toBe("wnam");
    expect(singletonLocationHint(bindings)).toBe("wnam");
  });

  it("honors only documented hints and fails junk back to the default", () => {
    for (const hint of SINGLETON_LOCATION_HINTS) {
      expect(singletonLocationHint(
        {...bindings, SINGLETON_LOCATION_HINT: hint} as Env)).toBe(hint);
    }
    for (const junk of ["", "moon", "WNAM", " wnam", 7, null, {}]) {
      expect(singletonLocationHint(
        {...bindings, SINGLETON_LOCATION_HINT: junk} as unknown as Env))
        .toBe(DEFAULT_SINGLETON_LOCATION_HINT);
    }
  });

  it("passes the hint to the budget singleton get() on the turn-mint path", async () => {
    const options: unknown[] = [];
    const spyEnv = {...bindings, PARTY_BUDGETS: {
      idFromName: (name: string) => bindings.PARTY_BUDGETS.idFromName(name),
      get: (id: DurableObjectId, getOptions?: unknown) => {
        options.push(getOptions);
        return bindings.PARTY_BUDGETS.get(id);
      },
    }} as unknown as Env;
    expect(await reserveTurnMint(spyEnv)).toBe(true);
    expect(options).toEqual([{locationHint: DEFAULT_SINGLETON_LOCATION_HINT}]);
  });
});
