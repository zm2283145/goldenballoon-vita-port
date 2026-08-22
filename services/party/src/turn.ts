import type {Env} from "./types";
import {internalRequest} from "./internal-api";

/**
 * Zero-cost TURN (Cloudflare Realtime) with STUN-only degradation.
 *
 * Doctrine: iceServers are connectivity hints, never authority. The two fixed
 * STUN servers are always served; TURN credentials are appended only when the
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
 * entries plus any minted TURN entries. */
export type DeliveredIceServer = {urls: string} | TurnIceServer;

interface StoredTurnServers {
  iceServers: TurnIceServer[];
  expiresAt: number;
}

export interface TurnSeams {
  reserve?: (env: Env) => Promise<boolean>;
  fetcher?: typeof fetch;
  now?: number;
}

export function stunIceServers(): DeliveredIceServer[] {
  return [
    {urls: "stun:stun.cloudflare.com:3478"},
    {urls: "stun:stun.l.google.com:19302"},
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
 * unconditionally. Null means "nothing usable", which the caller treats as a
 * mint failure. */
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
    const urls = rawUrls.filter(validIceUrl);
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
    const response = await env.PARTY_BUDGETS.get(id).fetch(
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

/** The room's full iceServers answer: fixed STUN, plus TURN credentials when
 * they can be had. Storage-backed cache, half-TTL refresh, and the
 * degradation ladder described in the module doctrine. Never throws. The
 * seams parameter exists for unit tests only. */
export async function roomIceServers(
    env: Env, storage: DurableObjectStorage,
    seams: TurnSeams = {}): Promise<DeliveredIceServer[]> {
  const stun = stunIceServers();
  if (!turnConfigured(env)) return stun;
  const reserve = seams.reserve || reserveTurnMint;
  const now = seams.now ?? Date.now();
  let cached: StoredTurnServers | null = null;
  try {
    const stored = await storage.get<unknown>(TURN_CACHE_KEY);
    if (validStoredTurnServers(stored, now)) cached = stored;
  } catch { /* Unreadable cache is the same as no cache. */ }
  if (cached && cached.expiresAt - now >= TURN_TTL_MS / 2) {
    return [...stun, ...cached.iceServers];
  }
  const degraded = () => cached ? [...stun, ...cached.iceServers] : stun;
  if (!(await reserve(env))) return degraded();
  const minted = await mintTurnIceServers(env, seams.fetcher);
  if (!minted) return degraded();
  try {
    await storage.put(TURN_CACHE_KEY,
      {iceServers: minted, expiresAt: now + TURN_TTL_MS} satisfies
        StoredTurnServers);
  } catch { /* The fresh set is still served; the next join re-mints. */ }
  return [...stun, ...minted];
}
