import {RelayServer} from "./server.js";

function integerSetting(name: string, fallback: number, minimum: number,
                        maximum: number): number {
  const raw = process.env[name];
  if (raw === undefined) return fallback;
  const value = Number(raw);
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    throw new Error(`${name} must be an integer from ${minimum} to ${maximum}`);
  }
  return value;
}

const port = integerSetting("PORT", 8080, 1, 65535);
const maxRooms = integerSetting("MDKR_RELAY_MAX_ROOMS", 1024, 1, 100_000);
const roomTtlMs = integerSetting(
  "MDKR_RELAY_ROOM_TTL_MS", 6 * 60 * 60_000, 60_000, 24 * 60 * 60_000);
const server = new RelayServer({
  host: process.env.HOST ?? "0.0.0.0",
  port,
  maxRooms,
  roomTtlMs,
});

const address = await server.start();
console.log(
  `[relay] listening host=${address.host} port=${address.port} protocol=1 maxPlayers=4`);

let stopping = false;
async function stop(signal: string): Promise<void> {
  if (stopping) return;
  stopping = true;
  console.log(`[relay] stopping signal=${signal}`);
  await server.stop();
}

process.once("SIGINT", () => void stop("SIGINT"));
process.once("SIGTERM", () => void stop("SIGTERM"));
