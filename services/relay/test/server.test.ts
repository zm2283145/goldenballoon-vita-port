import assert from "node:assert/strict";
import test, {afterEach} from "node:test";
import WebSocket, {type RawData} from "ws";
import {
  RELAY_CLIENT_SLOT,
  RELAY_FRAME_HEADER_BYTES,
  RELAY_MAGIC,
  RELAY_PROTOCOL_VERSION,
  RELAY_SUBPROTOCOL,
  RelayChannel,
  RelayFrameKind,
} from "../src/protocol.js";
import {RelayServer} from "../src/server.js";

const GAME_ID = "golden-balloon";
const GAME_VERSION = 1;

interface ServerMessage {
  binary: boolean;
  data: Buffer;
}

class TestClient {
  readonly socket: WebSocket;
  readonly #queued: ServerMessage[] = [];
  readonly #waiters: Array<(message: ServerMessage) => void> = [];

  private constructor(socket: WebSocket) {
    this.socket = socket;
    socket.on("message", (data: RawData, binary: boolean) => {
      let bytes: Buffer;
      if (Buffer.isBuffer(data)) bytes = data;
      else if (Array.isArray(data)) bytes = Buffer.concat(data);
      else bytes = Buffer.from(data);
      const message = {
        binary,
        data: bytes,
      };
      const waiter = this.#waiters.shift();
      if (waiter) waiter(message);
      else this.#queued.push(message);
    });
  }

  static async connect(url: string): Promise<TestClient> {
    const socket = new WebSocket(url, RELAY_SUBPROTOCOL);
    const client = new TestClient(socket);
    await new Promise<void>((resolve, reject) => {
      socket.once("open", resolve);
      socket.once("error", reject);
    });
    return client;
  }

  sendJson(value: object): void {
    this.socket.send(JSON.stringify(value));
  }

  sendBinary(value: Buffer): void {
    this.socket.send(value, {binary: true});
  }

  async next(): Promise<ServerMessage> {
    const queued = this.#queued.shift();
    if (queued) return queued;
    return await new Promise<ServerMessage>((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("message timeout")), 2000);
      this.#waiters.push(message => {
        clearTimeout(timer);
        resolve(message);
      });
    });
  }

  async nextJson(): Promise<Record<string, unknown>> {
    const message = await this.next();
    assert.equal(message.binary, false);
    return JSON.parse(message.data.toString()) as Record<string, unknown>;
  }

  async close(): Promise<void> {
    if (this.socket.readyState === WebSocket.CLOSED) return;
    await new Promise<void>(resolve => {
      this.socket.once("close", () => resolve());
      this.socket.close();
    });
  }
}

const servers: RelayServer[] = [];
const clients: TestClient[] = [];

afterEach(async () => {
  await Promise.all(clients.splice(0).map(client => client.close()));
  await Promise.all(servers.splice(0).map(server => server.stop()));
});

async function startServer(options: ConstructorParameters<typeof RelayServer>[0] = {}):
Promise<{server: RelayServer; url: string}> {
  const server = new RelayServer({
    host: "127.0.0.1",
    port: 0,
    heartbeatMs: 60_000,
    ...options,
  });
  servers.push(server);
  const address = await server.start();
  return {server, url: `ws://127.0.0.1:${address.port}/relay`};
}

async function connect(url: string): Promise<TestClient> {
  const client = await TestClient.connect(url);
  clients.push(client);
  return client;
}

async function createRoom(client: TestClient, maxPlayers = 4):
Promise<Record<string, unknown>> {
  client.sendJson({
    protocolVersion: 1,
    type: "create",
    gameId: GAME_ID,
    gameVersion: GAME_VERSION,
    playerName: "HOST",
    roomName: "TEST ROOM",
    maxPlayers,
  });
  return await client.nextJson();
}

async function joinRoom(client: TestClient, roomCode: string,
                        playerName: string): Promise<Record<string, unknown>> {
  client.sendJson({
    protocolVersion: 1,
    type: "join",
    gameId: GAME_ID,
    gameVersion: GAME_VERSION,
    playerName,
    roomCode,
  });
  return await client.nextJson();
}

function clientFrame(destinationMask: number, sequence: number,
                     senderSlot = RELAY_CLIENT_SLOT): Buffer {
  const payload = Buffer.from([0xde, 0xad, 0xbe, 0xef]);
  const frame = Buffer.alloc(RELAY_FRAME_HEADER_BYTES + payload.length);
  RELAY_MAGIC.copy(frame);
  frame[4] = RELAY_PROTOCOL_VERSION;
  frame[5] = RelayFrameKind.Data;
  frame[6] = senderSlot;
  frame[7] = destinationMask;
  frame[8] = RelayChannel.Input;
  frame.writeUInt32BE(sequence, 12);
  payload.copy(frame, RELAY_FRAME_HEADER_BYTES);
  return frame;
}

test("a room admits four assigned slots and refuses a fifth client", async () => {
  const {server, url} = await startServer();
  const host = await connect(url);
  const welcome = await createRoom(host);
  const roomCode = String(welcome.roomCode);
  assert.match(roomCode, /^[ABCDEFGHJKLMNPQRSTUVWXYZ23456789]{8}$/);
  assert.equal(welcome.slot, 0);
  assert.equal(welcome.host, true);

  for (let slot = 1; slot < 4; slot++) {
    const member = await connect(url);
    const joined = await joinRoom(member, roomCode, `P${slot + 1}`);
    assert.equal(joined.slot, slot);
    assert.equal(joined.host, false);
    const membership = await host.nextJson();
    assert.equal(membership.type, "membership");
    assert.equal((membership.members as unknown[]).length, slot + 1);
  }

  const fifth = await connect(url);
  const refusal = await joinRoom(fifth, roomCode, "P5");
  assert.deepEqual(refusal, {
    protocolVersion: 1,
    type: "error",
    code: "room_full",
  });
  assert.equal(server.roomCount, 1);
});

test("one browser sees multiple rooms and live room membership", async () => {
  const {server, url} = await startServer();
  const browser = await connect(url);
  browser.sendJson({
    protocolVersion: 1,
    type: "browse",
    gameId: GAME_ID,
    gameVersion: GAME_VERSION,
  });
  assert.deepEqual(await browser.nextJson(), {
    protocolVersion: 1,
    type: "room_list",
    rooms: [],
    truncated: false,
  });

  const firstHost = await connect(url);
  const first = await createRoom(firstHost, 4);
  const firstUpsert = await browser.nextJson();
  assert.equal((firstUpsert.room as Record<string, unknown>).roomCode,
    first.roomCode);
  assert.equal((firstUpsert.room as Record<string, unknown>).playerCount, 1);

  const secondHost = await connect(url);
  await createRoom(secondHost, 2);
  assert.equal((await browser.nextJson()).type, "room_upsert");
  assert.equal(server.roomCount, 2);

  const member = await connect(url);
  await joinRoom(member, String(first.roomCode), "P2");
  await firstHost.nextJson();
  const membershipUpsert = await browser.nextJson();
  assert.equal(
    (membershipUpsert.room as Record<string, unknown>).playerCount, 2);

  await firstHost.close();
  const removal = await browser.nextJson();
  assert.deepEqual(removal, {
    protocolVersion: 1,
    type: "room_remove",
    roomCode: first.roomCode,
  });
  assert.equal(server.roomCount, 1);
});

test("a browser can host on the same WebSocket connection", async () => {
  const {url} = await startServer();
  const browser = await connect(url);
  browser.sendJson({
    protocolVersion: 1,
    type: "browse",
    gameId: GAME_ID,
    gameVersion: GAME_VERSION,
  });
  await browser.nextJson();
  const welcome = await createRoom(browser, 4);
  assert.equal(welcome.type, "welcome");
  assert.equal(welcome.slot, 0);
});

test("room discovery and admission are isolated by game compatibility", async () => {
  const {url} = await startServer();
  const host = await connect(url);
  const welcome = await createRoom(host);

  const otherGameBrowser = await connect(url);
  otherGameBrowser.sendJson({
    protocolVersion: 1,
    type: "browse",
    gameId: "another-game",
    gameVersion: 1,
  });
  const otherRooms = await otherGameBrowser.nextJson();
  assert.deepEqual(otherRooms.rooms, []);

  otherGameBrowser.sendJson({
    protocolVersion: 1,
    type: "join",
    gameId: "another-game",
    gameVersion: 1,
    playerName: "OTHER",
    roomCode: welcome.roomCode,
  });
  assert.equal((await otherGameBrowser.nextJson()).code, "incompatible_game");

  otherGameBrowser.sendJson({
    protocolVersion: 1,
    type: "join",
    gameId: GAME_ID,
    gameVersion: GAME_VERSION,
    playerName: "OTHER",
    roomCode: welcome.roomCode,
  });
  assert.equal((await otherGameBrowser.nextJson()).type, "welcome");
});

test("binary data is destination-masked and sender identity is injected", async () => {
  const {url} = await startServer();
  const host = await connect(url);
  const welcome = await createRoom(host);
  const member = await connect(url);
  await joinRoom(member, String(welcome.roomCode), "P2");
  await host.nextJson();

  host.sendBinary(clientFrame(1 << 1, 1));
  const relayed = await member.next();
  assert.equal(relayed.binary, true);
  assert.equal(relayed.data[6], 0);
  assert.equal(relayed.data[7], 1 << 1);
  assert.equal(relayed.data.readUInt32BE(12), 1);
  assert.deepEqual(
    relayed.data.subarray(RELAY_FRAME_HEADER_BYTES),
    Buffer.from([0xde, 0xad, 0xbe, 0xef]));
});

test("sender spoofing and replayed sequences close the offender", async () => {
  const {url} = await startServer();
  const host = await connect(url);
  const welcome = await createRoom(host);
  const member = await connect(url);
  await joinRoom(member, String(welcome.roomCode), "P2");
  await host.nextJson();

  member.sendBinary(clientFrame(1, 1, 0));
  assert.equal((await member.nextJson()).code, "protocol");

  const replacement = await connect(url);
  await host.nextJson();
  await joinRoom(replacement, String(welcome.roomCode), "P2B");
  await host.nextJson();
  replacement.sendBinary(clientFrame(1, 10));
  await host.next();
  replacement.sendBinary(clientFrame(1, 10));
  assert.equal((await replacement.nextJson()).code, "protocol");
});

test("member departure updates membership and host departure closes the room", async () => {
  const {server, url} = await startServer();
  const host = await connect(url);
  const welcome = await createRoom(host);
  const member = await connect(url);
  await joinRoom(member, String(welcome.roomCode), "P2");
  await host.nextJson();

  await member.close();
  const membership = await host.nextJson();
  assert.equal((membership.members as unknown[]).length, 1);

  const replacement = await connect(url);
  await joinRoom(replacement, String(welcome.roomCode), "P2B");
  await host.nextJson();
  await host.close();
  assert.equal((await replacement.nextJson()).code, "room_closed");
  assert.equal(server.roomCount, 0);
});

test("expired rooms close their connected clients during admission sweeps", async () => {
  let now = 1_000;
  const {server, url} = await startServer({
    roomTtlMs: 100,
    now: () => now,
  });
  const host = await connect(url);
  await createRoom(host);
  now += 101;

  const nextHost = await connect(url);
  await createRoom(nextHost);
  assert.equal((await host.nextJson()).code, "room_closed");
  assert.equal(server.roomCount, 1);
});
