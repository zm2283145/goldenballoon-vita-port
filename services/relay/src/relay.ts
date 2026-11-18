import {randomBytes} from "node:crypto";
import type {WebSocket} from "ws";
import {
  decodeClientFrame,
  encodeRelayFrame,
  errorMessage,
  RELAY_MAX_PLAYERS,
  RELAY_PROTOCOL_VERSION,
  type RelayAdmissionRequest,
  type RelayErrorCode,
} from "./protocol.js";

const ROOM_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const MAX_PACKETS_PER_WINDOW = 600;
const MAX_BYTES_PER_WINDOW = 512 * 1024;
const RATE_WINDOW_MS = 1000;
const ROOM_LIST_LIMIT = 128;

interface RelayMember {
  socket: WebSocket;
  endpointId: string;
  playerName: string;
  slot: number;
  lastSequence: number;
  rateWindowStartedAt: number;
  ratePackets: number;
  rateBytes: number;
  alive: boolean;
}

interface RelayRoom {
  code: string;
  name: string;
  gameId: string;
  gameVersion: number;
  revision: number;
  maxPlayers: number;
  createdAt: number;
  lastActivityAt: number;
  members: Map<number, RelayMember>;
}

interface RelayBrowser {
  alive: boolean;
  gameId: string;
  gameVersion: number;
}

export interface RelayManagerOptions {
  roomTtlMs?: number;
  maxRooms?: number;
  now?: () => number;
}

export class RelayManager {
  readonly #rooms = new Map<string, RelayRoom>();
  readonly #roomBySocket = new Map<WebSocket, RelayRoom>();
  readonly #memberBySocket = new Map<WebSocket, RelayMember>();
  readonly #browsers = new Map<WebSocket, RelayBrowser>();
  readonly #roomTtlMs: number;
  readonly #maxRooms: number;
  readonly #now: () => number;

  constructor(options: RelayManagerOptions = {}) {
    this.#roomTtlMs = options.roomTtlMs ?? 6 * 60 * 60_000;
    this.#maxRooms = options.maxRooms ?? 1024;
    this.#now = options.now ?? Date.now;
  }

  get roomCount(): number {
    return this.#rooms.size;
  }

  admit(socket: WebSocket, request: RelayAdmissionRequest): RelayErrorCode | null {
    if (this.#memberBySocket.has(socket)) return "protocol";
    this.sweepExpired();
    if (request.type === "browse") {
      this.#browsers.set(socket, {
        alive: true,
        gameId: request.gameId,
        gameVersion: request.gameVersion,
      });
      this.#sendRoomList(socket, request.gameId, request.gameVersion);
      return null;
    }
    if (request.type === "create") {
      if (this.#rooms.size >= this.#maxRooms) return "server_capacity";
      const room = this.#createRoom(
        request.gameId, request.gameVersion, request.roomName,
        request.maxPlayers);
      this.#attach(room, socket, request.playerName, 0);
      return null;
    }
    const room = this.#rooms.get(request.roomCode);
    if (!room) return "room_not_found";
    if (room.gameId !== request.gameId ||
        room.gameVersion !== request.gameVersion) {
      return "incompatible_game";
    }
    if (room.members.size >= room.maxPlayers) return "room_full";
    const slot = this.#firstFreeSlot(room);
    if (slot < 0) return "room_full";
    this.#attach(room, socket, request.playerName, slot);
    return null;
  }

  receiveBinary(socket: WebSocket, bytes: Buffer): RelayErrorCode | null {
    const room = this.#roomBySocket.get(socket);
    const member = this.#memberBySocket.get(socket);
    const frame = decodeClientFrame(bytes);
    if (!room || !member || !frame) return "protocol";
    if (!this.#admitRate(member, bytes.length)) return "rate_limited";
    if (frame.sequence <= member.lastSequence ||
        (frame.destinationMask & (1 << member.slot)) !== 0) {
      return "protocol";
    }
    member.lastSequence = frame.sequence;
    room.lastActivityAt = this.#now();
    const output = encodeRelayFrame({...frame, senderSlot: member.slot});
    for (const [slot, recipient] of room.members) {
      if ((frame.destinationMask & (1 << slot)) === 0) continue;
      this.#sendBinary(recipient.socket, output);
    }
    return null;
  }

  remove(socket: WebSocket): void {
    this.#browsers.delete(socket);
    const room = this.#roomBySocket.get(socket);
    const member = this.#memberBySocket.get(socket);
    if (!room || !member) return;
    this.#roomBySocket.delete(socket);
    this.#memberBySocket.delete(socket);
    room.members.delete(member.slot);
    room.revision++;
    room.lastActivityAt = this.#now();
    if (member.slot === 0) {
      this.#closeRoom(room, "room_closed");
    } else if (room.members.size === 0) {
      this.#rooms.delete(room.code);
    } else {
      this.#publishMembership(room);
      this.#publishRoom(room);
    }
  }

  markAlive(socket: WebSocket): void {
    const member = this.#memberBySocket.get(socket);
    if (member) member.alive = true;
    const browser = this.#browsers.get(socket);
    if (browser) browser.alive = true;
  }

  heartbeat(): void {
    for (const [socket, member] of this.#memberBySocket) {
      if (!member.alive) {
        socket.terminate();
        this.remove(socket);
        continue;
      }
      member.alive = false;
      socket.ping();
    }
    for (const [socket, browser] of this.#browsers) {
      if (!browser.alive) {
        socket.terminate();
        this.#browsers.delete(socket);
        continue;
      }
      browser.alive = false;
      socket.ping();
    }
    this.sweepExpired();
  }

  sweepExpired(): void {
    const now = this.#now();
    for (const room of [...this.#rooms.values()]) {
      if (now - room.lastActivityAt >= this.#roomTtlMs) {
        this.#closeRoom(room, "room_closed");
      }
    }
  }

  close(): void {
    for (const room of [...this.#rooms.values()]) {
      this.#closeRoom(room, "room_closed");
    }
    for (const socket of this.#browsers.keys()) {
      socket.close(4000, "server_stopping");
    }
    this.#browsers.clear();
  }

  #createRoom(gameId: string, gameVersion: number, name: string,
              maxPlayers: number): RelayRoom {
    let code: string;
    do {
      code = this.#newRoomCode();
    } while (this.#rooms.has(code));
    const now = this.#now();
    const room: RelayRoom = {
      code,
      name,
      gameId,
      gameVersion,
      revision: 1,
      maxPlayers,
      createdAt: now,
      lastActivityAt: now,
      members: new Map(),
    };
    this.#rooms.set(code, room);
    return room;
  }

  #attach(room: RelayRoom, socket: WebSocket, playerName: string, slot: number): void {
    this.#browsers.delete(socket);
    const member: RelayMember = {
      socket,
      endpointId: this.#newEndpointId(),
      playerName,
      slot,
      lastSequence: 0,
      rateWindowStartedAt: this.#now(),
      ratePackets: 0,
      rateBytes: 0,
      alive: true,
    };
    room.members.set(slot, member);
    room.revision++;
    room.lastActivityAt = this.#now();
    this.#roomBySocket.set(socket, room);
    this.#memberBySocket.set(socket, member);
    this.#sendJson(socket, {
      protocolVersion: RELAY_PROTOCOL_VERSION,
      type: "welcome",
      roomCode: room.code,
      endpointId: member.endpointId,
      slot,
      host: slot === 0,
      maxPlayers: room.maxPlayers,
      revision: room.revision,
      members: this.#memberList(room),
    });
    this.#publishMembership(room, socket);
    this.#publishRoom(room);
  }

  #publishMembership(room: RelayRoom, except?: WebSocket): void {
    const message = JSON.stringify({
      protocolVersion: RELAY_PROTOCOL_VERSION,
      type: "membership",
      revision: room.revision,
      members: this.#memberList(room),
    });
    for (const member of room.members.values()) {
      if (member.socket !== except) this.#sendText(member.socket, message);
    }
  }

  #closeRoom(room: RelayRoom, code: RelayErrorCode): void {
    this.#rooms.delete(room.code);
    this.#publishRoomRemoval(room.code, room.gameId, room.gameVersion);
    for (const member of room.members.values()) {
      this.#roomBySocket.delete(member.socket);
      this.#memberBySocket.delete(member.socket);
      this.#sendText(member.socket, errorMessage(code));
      member.socket.close(4000, code);
    }
    room.members.clear();
  }

  #memberList(room: RelayRoom): object[] {
    return [...room.members.values()]
      .sort((left, right) => left.slot - right.slot)
      .map(member => ({
        endpointId: member.endpointId,
        playerName: member.playerName,
        slot: member.slot,
        host: member.slot === 0,
      }));
  }

  #roomSummary(room: RelayRoom): object {
    const host = room.members.get(0);
    return {
      roomCode: room.code,
      roomName: room.name,
      gameId: room.gameId,
      gameVersion: room.gameVersion,
      hostName: host?.playerName ?? "",
      playerCount: room.members.size,
      maxPlayers: room.maxPlayers,
    };
  }

  #sendRoomList(socket: WebSocket, gameId: string, gameVersion: number): void {
    const matchingRooms = [...this.#rooms.values()]
      .filter(room =>
        room.gameId === gameId && room.gameVersion === gameVersion);
    const rooms = matchingRooms
      .sort((left, right) => right.createdAt - left.createdAt)
      .slice(0, ROOM_LIST_LIMIT)
      .map(room => this.#roomSummary(room));
    this.#sendJson(socket, {
      protocolVersion: RELAY_PROTOCOL_VERSION,
      type: "room_list",
      rooms,
      truncated: matchingRooms.length > ROOM_LIST_LIMIT,
    });
  }

  #publishRoom(room: RelayRoom): void {
    const message = JSON.stringify({
      protocolVersion: RELAY_PROTOCOL_VERSION,
      type: "room_upsert",
      room: this.#roomSummary(room),
    });
    for (const [socket, browser] of this.#browsers) {
      if (browser.gameId === room.gameId &&
          browser.gameVersion === room.gameVersion) {
        this.#sendText(socket, message);
      }
    }
  }

  #publishRoomRemoval(roomCode: string, gameId: string,
                      gameVersion: number): void {
    const message = JSON.stringify({
      protocolVersion: RELAY_PROTOCOL_VERSION,
      type: "room_remove",
      roomCode,
    });
    for (const [socket, browser] of this.#browsers) {
      if (browser.gameId === gameId && browser.gameVersion === gameVersion) {
        this.#sendText(socket, message);
      }
    }
  }

  #admitRate(member: RelayMember, bytes: number): boolean {
    const now = this.#now();
    if (now < member.rateWindowStartedAt) return false;
    if (now - member.rateWindowStartedAt >= RATE_WINDOW_MS) {
      member.rateWindowStartedAt = now;
      member.ratePackets = 0;
      member.rateBytes = 0;
    }
    if (member.ratePackets >= MAX_PACKETS_PER_WINDOW ||
        member.rateBytes + bytes > MAX_BYTES_PER_WINDOW) {
      return false;
    }
    member.ratePackets++;
    member.rateBytes += bytes;
    return true;
  }

  #firstFreeSlot(room: RelayRoom): number {
    for (let slot = 0; slot < room.maxPlayers; slot++) {
      if (!room.members.has(slot)) return slot;
    }
    return -1;
  }

  #newRoomCode(): string {
    const bytes = randomBytes(8);
    let output = "";
    for (const byte of bytes) output += ROOM_ALPHABET.charAt(byte & 31);
    return output;
  }

  #newEndpointId(): string {
    const bytes = randomBytes(8);
    if (bytes.every(byte => byte === 0)) bytes[7] = 1;
    return bytes.readBigUInt64BE().toString(10);
  }

  #sendJson(socket: WebSocket, value: object): void {
    this.#sendText(socket, JSON.stringify(value));
  }

  #sendText(socket: WebSocket, text: string): void {
    if (socket.readyState === socket.OPEN) socket.send(text);
  }

  #sendBinary(socket: WebSocket, bytes: Buffer): void {
    if (socket.readyState === socket.OPEN) socket.send(bytes, {binary: true});
  }
}
