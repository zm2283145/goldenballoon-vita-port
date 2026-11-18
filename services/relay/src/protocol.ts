export const RELAY_PROTOCOL_VERSION = 1;
export const RELAY_SUBPROTOCOL = "mdkr-relay-v1";
export const RELAY_MAX_PLAYERS = 4;
export const RELAY_MIN_PLAYERS = 2;
export const RELAY_MAX_PAYLOAD_BYTES = 1024;
export const RELAY_FRAME_HEADER_BYTES = 16;
export const RELAY_CLIENT_SLOT = 0xff;
export const RELAY_MAGIC = Buffer.from("DKRR", "ascii");

export enum RelayFrameKind {
  Data = 1,
}

export enum RelayChannel {
  Session = 1,
  Input = 2,
  Control = 3,
  Repair = 4,
}

export interface RelayCreateRequest {
  protocolVersion: 1;
  type: "create";
  gameId: string;
  gameVersion: number;
  playerName: string;
  roomName: string;
  maxPlayers: number;
}

export interface RelayBrowseRequest {
  protocolVersion: 1;
  type: "browse";
  gameId: string;
  gameVersion: number;
}

export interface RelayJoinRequest {
  protocolVersion: 1;
  type: "join";
  gameId: string;
  gameVersion: number;
  playerName: string;
  roomCode: string;
}

export type RelayAdmissionRequest =
  | RelayBrowseRequest
  | RelayCreateRequest
  | RelayJoinRequest;

export interface RelayDataFrame {
  senderSlot: number;
  destinationMask: number;
  channel: RelayChannel;
  sequence: number;
  payload: Buffer;
}

export type RelayErrorCode =
  | "protocol"
  | "incompatible_game"
  | "room_not_found"
  | "room_full"
  | "room_closed"
  | "rate_limited"
  | "server_capacity";

const ROOM_CODE_PATTERN = /^[ABCDEFGHJKLMNPQRSTUVWXYZ23456789]{8}$/;
const GAME_ID_PATTERN = /^[a-z0-9](?:[a-z0-9.-]{0,30}[a-z0-9])?$/;
const PLAYER_NAME_PATTERN = /^[A-Za-z0-9 _-]{1,16}$/;
const ROOM_NAME_PATTERN = /^[A-Za-z0-9 _-]{1,20}$/;

function exactKeys(value: Record<string, unknown>, expected: readonly string[]): boolean {
  const actual = Object.keys(value).sort();
  const sortedExpected = [...expected].sort();
  return actual.length === sortedExpected.length &&
    actual.every((key, index) => key === sortedExpected[index]);
}

export function parseAdmissionRequest(text: string): RelayAdmissionRequest | null {
  let value: unknown;
  if (Buffer.byteLength(text, "utf8") > 512) return null;
  try {
    value = JSON.parse(text);
  } catch {
    return null;
  }
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const item = value as Record<string, unknown>;
  if (item.protocolVersion !== RELAY_PROTOCOL_VERSION ||
      typeof item.type !== "string") {
    return null;
  }
  if (typeof item.gameId !== "string" ||
      !GAME_ID_PATTERN.test(item.gameId) ||
      !Number.isInteger(item.gameVersion) ||
      Number(item.gameVersion) < 1 ||
      Number(item.gameVersion) > 0xffff_ffff) {
    return null;
  }
  if (item.type === "browse" &&
      exactKeys(item, ["protocolVersion", "type", "gameId", "gameVersion"])) {
    return {
      protocolVersion: 1,
      type: "browse",
      gameId: item.gameId,
      gameVersion: Number(item.gameVersion),
    };
  }
  if (typeof item.playerName !== "string" ||
      !PLAYER_NAME_PATTERN.test(item.playerName)) return null;
  if (item.type === "create" &&
      exactKeys(item, [
        "protocolVersion", "type", "gameId", "gameVersion",
        "playerName", "roomName", "maxPlayers",
      ]) &&
      typeof item.roomName === "string" &&
      ROOM_NAME_PATTERN.test(item.roomName) &&
      Number.isInteger(item.maxPlayers) &&
      Number(item.maxPlayers) >= RELAY_MIN_PLAYERS &&
      Number(item.maxPlayers) <= RELAY_MAX_PLAYERS) {
    return {
      protocolVersion: 1,
      type: "create",
      gameId: item.gameId,
      gameVersion: Number(item.gameVersion),
      playerName: item.playerName,
      roomName: item.roomName,
      maxPlayers: Number(item.maxPlayers),
    };
  }
  if (item.type === "join" &&
      exactKeys(item, [
        "protocolVersion", "type", "gameId", "gameVersion",
        "playerName", "roomCode",
      ]) &&
      typeof item.roomCode === "string" &&
      ROOM_CODE_PATTERN.test(item.roomCode)) {
    return {
      protocolVersion: 1,
      type: "join",
      gameId: item.gameId,
      gameVersion: Number(item.gameVersion),
      playerName: item.playerName,
      roomCode: item.roomCode,
    };
  }
  return null;
}

export function roomCodeValid(roomCode: string): boolean {
  return ROOM_CODE_PATTERN.test(roomCode);
}

export function decodeClientFrame(bytes: Buffer): RelayDataFrame | null {
  if (bytes.length < RELAY_FRAME_HEADER_BYTES ||
      bytes.length > RELAY_FRAME_HEADER_BYTES + RELAY_MAX_PAYLOAD_BYTES ||
      !bytes.subarray(0, 4).equals(RELAY_MAGIC) ||
      bytes[4] !== RELAY_PROTOCOL_VERSION ||
      bytes[5] !== RelayFrameKind.Data ||
      bytes[6] !== RELAY_CLIENT_SLOT ||
      bytes[9] !== 0 ||
      bytes.readUInt16BE(10) !== 0) {
    return null;
  }
  const destinationMask = bytes[7] ?? 0;
  const channel = bytes[8] ?? 0;
  const sequence = bytes.readUInt32BE(12);
  if (destinationMask === 0 || (destinationMask & 0xf0) !== 0 ||
      !Object.values(RelayChannel).includes(channel) ||
      sequence === 0) {
    return null;
  }
  return {
    senderSlot: RELAY_CLIENT_SLOT,
    destinationMask,
    channel,
    sequence,
    payload: bytes.subarray(RELAY_FRAME_HEADER_BYTES),
  };
}

export function encodeRelayFrame(frame: RelayDataFrame): Buffer {
  if (frame.senderSlot < 0 || frame.senderSlot >= RELAY_MAX_PLAYERS ||
      frame.destinationMask === 0 || (frame.destinationMask & 0xf0) !== 0 ||
      !Object.values(RelayChannel).includes(frame.channel) ||
      frame.sequence <= 0 || frame.sequence > 0xffff_ffff ||
      frame.payload.length > RELAY_MAX_PAYLOAD_BYTES) {
    throw new RangeError("invalid relay frame");
  }
  const output = Buffer.alloc(RELAY_FRAME_HEADER_BYTES + frame.payload.length);
  RELAY_MAGIC.copy(output, 0);
  output[4] = RELAY_PROTOCOL_VERSION;
  output[5] = RelayFrameKind.Data;
  output[6] = frame.senderSlot;
  output[7] = frame.destinationMask;
  output[8] = frame.channel;
  output.writeUInt32BE(frame.sequence, 12);
  frame.payload.copy(output, RELAY_FRAME_HEADER_BYTES);
  return output;
}

export function errorMessage(code: RelayErrorCode): string {
  return JSON.stringify({
    protocolVersion: RELAY_PROTOCOL_VERSION,
    type: "error",
    code,
  });
}
