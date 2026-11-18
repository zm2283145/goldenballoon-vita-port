import assert from "node:assert/strict";
import test from "node:test";
import {
  decodeClientFrame,
  encodeRelayFrame,
  parseAdmissionRequest,
  RELAY_CLIENT_SLOT,
  RELAY_FRAME_HEADER_BYTES,
  RELAY_MAGIC,
  RELAY_PROTOCOL_VERSION,
  RelayChannel,
  RelayFrameKind,
} from "../src/protocol.js";

function clientFrame(options: {
  senderSlot?: number;
  destinationMask?: number;
  channel?: number;
  sequence?: number;
  payload?: Buffer;
} = {}): Buffer {
  const payload = options.payload ?? Buffer.from("payload");
  const frame = Buffer.alloc(RELAY_FRAME_HEADER_BYTES + payload.length);
  RELAY_MAGIC.copy(frame);
  frame[4] = RELAY_PROTOCOL_VERSION;
  frame[5] = RelayFrameKind.Data;
  frame[6] = options.senderSlot ?? RELAY_CLIENT_SLOT;
  frame[7] = options.destinationMask ?? 2;
  frame[8] = options.channel ?? RelayChannel.Input;
  frame.writeUInt32BE(options.sequence ?? 1, 12);
  payload.copy(frame, RELAY_FRAME_HEADER_BYTES);
  return frame;
}

test("admission messages require exact bounded schemas", () => {
  assert.deepEqual(parseAdmissionRequest(JSON.stringify({
    protocolVersion: 1,
    type: "create",
    gameId: "golden-balloon",
    gameVersion: 1,
    playerName: "PLAYER 1",
    roomName: "ZACH RACE",
    maxPlayers: 4,
  })), {
    protocolVersion: 1,
    type: "create",
    gameId: "golden-balloon",
    gameVersion: 1,
    playerName: "PLAYER 1",
    roomName: "ZACH RACE",
    maxPlayers: 4,
  });
  assert.equal(parseAdmissionRequest(JSON.stringify({
    protocolVersion: 1,
    type: "join",
    gameId: "golden-balloon",
    gameVersion: 1,
    playerName: "PLAYER 2",
    roomCode: "ABCDEFGH",
    senderSlot: 0,
  })), null);
  assert.equal(parseAdmissionRequest(JSON.stringify({
    protocolVersion: 1,
    type: "create",
    gameId: "golden-balloon",
    gameVersion: 1,
    playerName: "<script>",
    roomName: "ZACH RACE",
    maxPlayers: 4,
  })), null);
  assert.equal(parseAdmissionRequest(JSON.stringify({
    protocolVersion: 1,
    type: "browse",
    gameId: "INVALID GAME",
    gameVersion: 1,
  })), null);
});

test("client frames require the untrusted sender sentinel", () => {
  const frame = clientFrame({payload: Buffer.from([1, 2, 3])});
  assert.deepEqual(decodeClientFrame(frame), {
    senderSlot: RELAY_CLIENT_SLOT,
    destinationMask: 2,
    channel: RelayChannel.Input,
    sequence: 1,
    payload: frame.subarray(RELAY_FRAME_HEADER_BYTES),
  });
  assert.equal(decodeClientFrame(clientFrame({senderSlot: 0})), null);
  assert.equal(decodeClientFrame(clientFrame({destinationMask: 0})), null);
  assert.equal(decodeClientFrame(clientFrame({channel: 99})), null);
  assert.equal(decodeClientFrame(clientFrame({sequence: 0})), null);
});

test("outbound frames carry the server-provided sender", () => {
  const output = encodeRelayFrame({
    senderSlot: 3,
    destinationMask: 5,
    channel: RelayChannel.Control,
    sequence: 0xffff_ffff,
    payload: Buffer.from("race"),
  });
  assert.equal(output[6], 3);
  assert.equal(output[7], 5);
  assert.equal(output[8], RelayChannel.Control);
  assert.equal(output.readUInt32BE(12), 0xffff_ffff);
  assert.equal(output.subarray(RELAY_FRAME_HEADER_BYTES).toString(), "race");
});
