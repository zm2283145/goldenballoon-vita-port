import {singletonLocationHint, type Env} from "./types";
import {internalRequest} from "./internal-api";

/**
 * Zero-cost TURN (Cloudflare Realtime) with STUN-only degradation.
 *
 * Doctrine: iceServers are connectivity hints, never authority. The one
 * fixed STUN server is always served — Cloudflare only, matching the
 * clients' baked-in fallback and disclosing traffic to no third party —
 * and TURN credentials are appended only when the
 * owner has provisioned BOTH secrets and a mint succeeds. Every failure —
 * unprovisioned secrets, budget refusal, provider outage, malformed response —
 * degrades to the best still-valid answer (cached credentials, then
 * STUN-only) and is never surfaced to a client as an error: pairing without a
 * relay is exactly yesterday's behavior.
 *
 * Cost control: a mint is one external HTTPS call, so it is charged through
 * the fixed turnMint budget shape and cached in the room object's storage
 * (hibernation wipes in-memory state) for the credential TTL. N joins to one
 * room therefore cost at most one mint per TTL window; the set is refreshed
 * once less than half the window remains so a long-lived room never hands out
 * about-to-expire credentials.
 */

export const TURN_TTL_SECONDS = 14_400;
export const TURN_TTL_MS = TURN_TTL_SECONDS * 1_000;

const TURN_CACHE_KEY = "turnIceServers";
const TURN_MINT_TIMEOUT_MS = 5_000;
const MAX_MINT_RESPONSE_BYTES = 16 * 1024;
const MAX_ICE_ENTRIES = 8;
const MAX_ICE_URLS = 8;
const MAX_ICE_ENTRY_JSON_BYTES = 2_048;
const MAX_ICE_SECRET_BYTES = 512;

/* stun/turn/turns URI with an explicit port and at most a transport query, the
 * only shapes the provider mints and the clients hand to their ICE agents. */
const ICE_URL_PATTERN =
  /^(stun|turn|turns):[A-Za-z0-9.-]{1,128}:(\d{1,5})(\?transport=(?:udp|tcp))?$/;

export interface TurnIceServer {
  urls: string[];
  username: string;
  credential: string;
}

/** What a response's iceServers array holds: the fixed single-url STUN
 * entry plus any minted TURN entries. */
export type DeliveredIceServer = {urls: string} | TurnIceServer;

interface StoredTurnServers {
  iceServers: TurnIceServer[];
  expiresAt: number;
}

export interface TurnSeams {
  reserve?: (env: Env) => Promise<boolean>;
  fetcher?: typeof fetch;
  now?: number;
  /** Unit tests observe the floating background refresh through this hook;
   * production callers leave it unset and let the task run detached (a
   * Durable Object stays alive while async work is pending). */
  background?: (task: Promise<boolean>) => void;
}

export function stunIceServers(): DeliveredIceServer[] {
  return [
    {urls: "stun:stun.cloudflare.com:3478"},
  ];
}

/** Both secrets, each shaped like a value that can be safely interpolated
 * into the key-id URL path and the bearer header. Anything else reads as
 * unconfigured, which fails safe to STUN-only. */
export function turnConfigured(env: Env): boolean {
  return typeof env.TURN_KEY_ID === "string" &&
    /^[A-Za-z0-9_-]{8,128}$/.test(env.TURN_KEY_ID) &&
    typeof env.TURN_API_TOKEN === "string" &&
    /^[\x21-\x7e]{16,256}$/.test(env.TURN_API_TOKEN);
}

function validIceUrl(value: unknown): value is string {
  if (typeof value !== "string") return false;
  const matched = ICE_URL_PATTERN.exec(value);
  /* The provider's port-53 variants are documented as browser-blocked and
   * would only slow candidate gathering everywhere else. */
  return matched !== null && matched[2] !== "53";
}

function validIceSecret(value: unknown): value is string {
  return typeof value === "string" && value.length >= 1 &&
    value.length <= MAX_ICE_SECRET_BYTES && /^[\x21-\x7e]+$/.test(value);
}

/** Rebuild the provider's credentialed entries from known fields only, so
 * nothing unvalidated is ever forwarded to a client. Uncredentialed entries
 * (the provider repeats its STUN urls) are dropped — STUN is already served
 * unconditionally — and stun urls inside a credentialed entry are stripped,
 * because credentials ride on turn/turns urls only; an entry with no
 * turn/turns url left is dropped. Null means "nothing usable", which the
 * caller treats as a mint failure. */
function normalizedTurnEntries(value: unknown): TurnIceServer[] | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const listed = (value as Record<string, unknown>).iceServers;
  if (!Array.isArray(listed) || listed.length < 1 ||
      listed.length > MAX_ICE_ENTRIES) return null;
  const entries: TurnIceServer[] = [];
  for (const item of listed) {
    if (!item || typeof item !== "object" || Array.isArray(item)) return null;
    const entry = item as Record<string, unknown>;
    const hasUsername = entry.username !== undefined;
    const hasCredential = entry.credential !== undefined;
    if (hasUsername !== hasCredential) return null;
    const rawUrls = Array.isArray(entry.urls) ? entry.urls : [entry.urls];
    if (rawUrls.length < 1 || rawUrls.length > MAX_ICE_URLS) return null;
    if (!hasUsername) continue;
    if (!validIceSecret(entry.username) || !validIceSecret(entry.credential)) {
      return null;
    }
    const urls = rawUrls.filter(validIceUrl)
      .filter(url => /^turns?:/.test(url));
    if (urls.length === 0) continue;
    entries.push({urls, username: entry.username, credential: entry.credential});
  }
  if (entries.length === 0 ||
      JSON.stringify(entries).length > MAX_ICE_ENTRY_JSON_BYTES) return null;
  return entries;
}

/** One credential mint against the documented Cloudflare Realtime endpoint.
 * Null on any failure — non-2xx, network, timeout, malformed body — never an
 * exception. The fetcher parameter exists for unit tests only; production
 * callers use the platform default. */
export async function mintTurnIceServers(
    env: Env, fetcher: typeof fetch = fetch): Promise<TurnIceServer[] | null> {
  if (!turnConfigured(env)) return null;
  try {
    const response = await fetcher(
      "https://rtc.live.cloudflare.com/v1/turn/keys/" +
      `${env.TURN_KEY_ID}/credentials/generate-ice-servers`, {
        method: "POST",
        headers: {"content-type": "application/json",
          authorization: `Bearer ${env.TURN_API_TOKEN}`},
        body: JSON.stringify({ttl: TURN_TTL_SECONDS}),
        signal: AbortSignal.timeout(TURN_MINT_TIMEOUT_MS),
      });
    if (!response.ok) return null;
    const text = await response.text();
    if (text.length > MAX_MINT_RESPONSE_BYTES) return null;
    return normalizedTurnEntries(JSON.parse(text));
  } catch {
    return null;
  }
}

/** Admit one mint against the day's control reserve — the same in-object
 * budget idiom as PartyRoom.reserveNativeCommand. A refusal (or an
 * unreachable budget object) charges nothing and reads as "no mint". */
export async function reserveTurnMint(env: Env): Promise<boolean> {
  try {
    const day = new Date().toISOString().slice(0, 10);
    const id = env.PARTY_BUDGETS.idFromName(day);
    const response = await env.PARTY_BUDGETS.get(id,
      {locationHint: singletonLocationHint(env)}).fetch(
      "https://budget/admit?kind=control&units=2&operation=turnMint",
      internalRequest({method: "POST"}));
    return response.ok;
  } catch {
    return false;
  }
}

function validStoredTurnServers(value: unknown,
                                now: number): value is StoredTurnServers {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const record = value as Record<string, unknown>;
  if (!Number.isSafeInteger(record.expiresAt) ||
      Number(record.expiresAt) <= now ||
      Number(record.expiresAt) > now + TURN_TTL_MS) return false;
  return normalizedTurnEntries({iceServers: record.iceServers}) !== null;
}

/* F2: the half-TTL refresh must never hold a room object's input gate on the
 * external mint (a slow provider stalls every join/redeem in the room for up
 * to the 5 s cap). Cached credentials at the refresh point still carry at
 * least TTL/2 validity (≥2 h at the 4 h default), so they are always served
 * immediately and the re-mint runs as a floating background task. Single
 * flight: the in-memory per-storage flag stops concurrent tasks inside one
 * live object; the stored start stamp is what survives hibernation, so a
 * rehydrated object (flag lost) cannot stampede — the worst case is exactly
 * one duplicate charged mint, and a refusal still charges nothing. */
const TURN_REFRESH_KEY = "turnRefreshStartedAt";
export const TURN_REFRESH_LOCKOUT_MS = 30_000;
const refreshingStorages = new WeakSet<DurableObjectStorage>();

function startTurnRefresh(env: Env, storage: DurableObjectStorage,
                          seams: TurnSeams, now: number): void {
  if (refreshingStorages.has(storage)) return;
  refreshingStorages.add(storage);
  const task = (async () => {
    try {
      /* Stamp compare-and-set. Durable Object events are single-threaded and
       * this object's tasks are already serialized by the in-memory flag, so
       * read-check-write cannot race with itself; the stamp's job is to gate
       * the NEXT object incarnation while this mint is still in flight. A
       * malformed or future stamp reads as absent, which fails open to one
       * charged re-mint rather than locking refresh out forever. */
      const stamped = await storage.get<unknown>(TURN_REFRESH_KEY);
      if (typeof stamped === "number" && Number.isSafeInteger(stamped) &&
          stamped <= now && now - stamped < TURN_REFRESH_LOCKOUT_MS) {
        return false;
      }
      await storage.put(TURN_REFRESH_KEY, now);
      const reserve = seams.reserve || reserveTurnMint;
      if (!(await reserve(env))) return false;
      const minted = await mintTurnIceServers(env, seams.fetcher);
      if (!minted) return false;
      await storage.put(TURN_CACHE_KEY,
        {iceServers: minted,
          expiresAt: (seams.now ?? Date.now()) + TURN_TTL_MS} satisfies
          StoredTurnServers);
      return true;
    } catch {
      /* Every failure leaves the still-valid cache in place; the next caller
       * past the lockout tries again. Never surfaces to any response. */
      return false;
    } finally {
      refreshingStorages.delete(storage);
    }
  })();
  seams.background?.(task);
}

/** The room's full iceServers answer: fixed STUN, plus TURN credentials when
 * they can be had. Storage-backed cache, non-blocking half-TTL background
 * refresh, and the degradation ladder described in the module doctrine.
 * Never throws. The seams parameter exists for unit tests only. */
export async function roomIceServers(
    env: Env, storage: DurableObjectStorage,
    seams: TurnSeams = {}): Promise<DeliveredIceServer[]> {
  const stun = stunIceServers();
  if (!turnConfigured(env)) return stun;
  const now = seams.now ?? Date.now();
  let cached: StoredTurnServers | null = null;
  try {
    const stored = await storage.get<unknown>(TURN_CACHE_KEY);
    if (validStoredTurnServers(stored, now)) cached = stored;
  } catch { /* Unreadable cache is the same as no cache. */ }
  if (cached) {
    /* Still-valid credentials are served immediately, always. Less than half
     * the window remaining only means the refresh starts in the background —
     * no joiner ever waits on the provider for credentials that exist. */
    if (cached.expiresAt - now < TURN_TTL_MS / 2) {
      startTurnRefresh(env, storage, seams, now);
    }
    return [...stun, ...cached.iceServers];
  }
  /* No valid cache — the very first mint (room create), or a set that fully
   * expired while the room slept. This one stays synchronous so the wave-0
   * payloads still carry TURN whenever it can be had at all. */
  const reserve = seams.reserve || reserveTurnMint;
  if (!(await reserve(env))) return stun;
  const minted = await mintTurnIceServers(env, seams.fetcher);
  if (!minted) return stun;
  try {
    await storage.put(TURN_CACHE_KEY,
      {iceServers: minted, expiresAt: now + TURN_TTL_MS} satisfies
        StoredTurnServers);
  } catch { /* The fresh set is still served; the next join re-mints. */ }
  return [...stun, ...minted];
}
