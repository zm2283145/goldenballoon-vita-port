import {beforeAll, describe, expect, it, vi} from "vitest";

/*
 * S5 (review F6): party-host.js re-rendered the whole roster — including the
 * pending-approval cards — on every 5-second control-channel RTT pong,
 * destroying the slot picker's selection and focus exactly while the host was
 * approving a phone. The remedy makes the pong path surgical: it rewrites the
 * pong'd seat tile's status text only, and a pending card's DOM node persists
 * across pongs unless its own data changed.
 *
 * The page is a DOM-coupled IIFE, so this suite runs it over a small fake DOM
 * (memoized fake elements, a fake RTCPeerConnection) and drives the REAL pong
 * bookkeeping through the testConfig-gated MDKRPartyHost.testControlPong hook,
 * which stamps an outstanding ping and calls the exact handler the control
 * channel's pong message invokes. The focus half of the assertion — a real
 * <select> keeping selection and focus in a real browser — is the extended
 * scenario in tests/check_party_host.py (browser lane, merge time).
 */

type Handler = (event: unknown) => void;

class FakeNode {
  tagName: string;
  children: FakeNode[] = [];
  dataset: Record<string, string> = {};
  style: Record<string, string> = {};
  attributes = new Map<string, string>();
  listeners = new Map<string, Handler[]>();
  selectorMap: Record<string, FakeNode | null> = {};
  hidden = false;
  disabled = false;
  open = false;
  selected = false;
  value = "";
  textContent = "";
  className = "";
  type = "";
  id = "";
  width = 0;
  height = 0;
  onclick: unknown = null;

  constructor(tagName = "div") { this.tagName = tagName; }
  addEventListener(name: string, handler: Handler): void {
    const handlers = this.listeners.get(name) || [];
    handlers.push(handler);
    this.listeners.set(name, handlers);
  }
  removeEventListener(): void {}
  setAttribute(name: string, value: string): void {
    this.attributes.set(name, String(value));
  }
  getAttribute(name: string): string | null {
    return this.attributes.get(name) ?? null;
  }
  private adopt(node: FakeNode): void {
    this.children.push(node);
    if (this.tagName === "select" && node.tagName === "option" &&
        (this.children.length === 1 || node.selected)) {
      this.value = node.value;
    }
  }
  append(...nodes: (FakeNode | string)[]): void {
    for (const node of nodes) {
      if (node instanceof FakeNode) this.adopt(node);
    }
  }
  appendChild(node: FakeNode): FakeNode { this.adopt(node); return node; }
  replaceChildren(...nodes: FakeNode[]): void { this.children = [...nodes]; }
  insertBefore(node: FakeNode, reference: FakeNode | null): FakeNode {
    const index = reference ? this.children.indexOf(reference) : -1;
    this.children.splice(index < 0 ? this.children.length : index, 0, node);
    return node;
  }
  remove(): void {}
  focus(): void {}
  click(): void {}
  closest(): FakeNode | null { return null; }
  querySelector(selector: string): FakeNode | null {
    return this.selectorMap[selector] ?? null;
  }
  querySelectorAll(): FakeNode[] { return []; }
  showModal(): void { this.open = true; }
  close(): void { this.open = false; }
  getContext(): null { return null; }
}

function seatTile(seat: number): FakeNode {
  const tile = new FakeNode("li");
  tile.dataset.seat = String(seat);
  tile.selectorMap = {
    "strong": new FakeNode("strong"),
    "small": new FakeNode("small"),
    ".party-seat-remove": new FakeNode("button"),
    ".party-seat-confirm": null,
  };
  return tile;
}

class FakeRTCPeerConnection {
  connectionState = "new";
  signalingState = "stable";
  localDescription: unknown = null;
  addEventListener(): void {}
  createDataChannel(label: string) {
    return {label, readyState: "connecting", binaryType: "",
      addEventListener() {}, send() {}, close() {}};
  }
  async createOffer() { return {type: "offer", sdp: "v=0"}; }
  async setLocalDescription(description: unknown) {
    this.localDescription = description;
  }
  async setRemoteDescription(): Promise<void> {}
  async addIceCandidate(): Promise<void> {}
  close(): void {}
  restartIce(): void {}
}

interface PartyHostApi {
  open(): void;
  applyRoomState(value: Record<string, unknown>): void;
  receiveSignal(value: Record<string, unknown>): void;
  state(): {room: Record<string, unknown> | null};
  remotePads(): Array<{active: boolean; provisional: boolean; reserved: boolean}>;
  testControlPong(controllerId: string, rttMs?: number): boolean;
}

const elements = new Map<string, FakeNode>();
function byId(id: string): FakeNode {
  let node = elements.get(id);
  if (!node) {
    node = new FakeNode();
    node.id = id;
    elements.set(id, node);
  }
  return node;
}

let host: PartyHostApi;
let testState: {controlRtts: number[]; controlPongs: number};

const PENDING_ID = "pending-controller-0001";
const LIVE_ID = "live-controller-000001";

beforeAll(async () => {
  const seats = byId("party-seats");
  seats.children = [seatTile(1), seatTile(2), seatTile(3), seatTile(4)];
  const stage = byId("stage");
  stage.hidden = true;
  (globalThis as Record<string, unknown>).document = {
    getElementById: byId,
    createElement: (tag: string) => new FakeNode(tag),
    createTextNode: (text: string) => {
      const node = new FakeNode("#text");
      node.textContent = text;
      return node;
    },
    querySelector: () => null,
    body: new FakeNode("body"),
    head: new FakeNode("head"),
    activeElement: null,
  };
  vi.stubGlobal("location", {hostname: "127.0.0.1",
    origin: "http://127.0.0.1:8080", href: "http://127.0.0.1:8080/"});
  vi.stubGlobal("RTCPeerConnection", FakeRTCPeerConnection);
  vi.stubGlobal("MDKRPartySas", {
    createIdentity: async () => ({publicKey: "H".repeat(87), privateKey: {}}),
    sdpFingerprint: () => "",
    phrase: async () => "",
  });
  (globalThis as Record<string, unknown>).__mdkrPartyHostTestConfig = {
    async request(path: string) {
      if (path === "/api/party/create") {
        return {roomId: "abcdefghijklmnopqrstuv",
          hostCredential: "H".repeat(43), fallbackCode: "123456",
          inviteGeneration: 1, inviteExpiresInMs: 120_000,
          controllerUrl: `http://127.0.0.1:8080/controller/#${"A".repeat(43)}`};
      }
      return {ok: true};
    },
  };
  await import("../../../dist/web/party/party-host.js");
  host = (globalThis as Record<string, unknown>)
    .MDKRPartyHost as unknown as PartyHostApi;
  testState = (globalThis as Record<string, unknown>)
    .__mdkrPartyHostTestState as typeof testState;
  host.open();
  await vi.waitFor(() => {
    if (!host.state().room) throw new Error("room not created yet");
  });
});

function findSelect(node: FakeNode): FakeNode | null {
  if (node.tagName === "select") return node;
  for (const child of node.children) {
    const found = findSelect(child);
    if (found) return found;
  }
  return null;
}

describe("pending-approval card DOM stability across RTT pongs", () => {
  it("keeps the pending card node and its slot choice across a pong, while " +
     "the pong'd seat repaints only its RTT text", async () => {
    host.applyRoomState({type: "room_state", transitionId: 1,
      inviteExpiresAt: Date.now() + 120_000, inviteGeneration: 1,
      phase: "open", controllers: [
        {controllerId: LIVE_ID, name: "Live phone",
          controllerPublicKey: "K".repeat(87), phase: "connected", seat: 1,
          leaseGeneration: 1, connectionSequence: 1},
        {controllerId: PENDING_ID, name: "Waiting phone", phase: "pending",
          seat: null, leaseGeneration: 0, connectionSequence: 1},
      ]});
    /* The phone announced itself over signaling, so a live peer exists. */
    host.receiveSignal({type: "controller_hello", controllerId: LIVE_ID});
    await vi.waitFor(() => {
      if (!host.testControlPong(LIVE_ID, 23)) {
        throw new Error("live peer not established yet");
      }
    });
    /* Model the confirmed direct connection the RTT belongs to. */
    host.remotePads()[0]!.active = true;

    const pendingList = byId("party-pending-list");
    expect(pendingList.children).toHaveLength(1);
    const cardBefore = pendingList.children[0]!;
    const select = findSelect(cardBefore)!;
    expect(select).not.toBeNull();
    /* The host picks a non-default slot for the waiting phone... */
    select.value = "3";

    /* ...and a routine pong from the already-connected phone arrives. */
    const pongsBefore = testState.controlPongs;
    expect(host.testControlPong(LIVE_ID, 23)).toBe(true);
    expect(testState.controlPongs).toBe(pongsBefore + 1);
    const sampled = testState.controlRtts.at(-1)!;
    expect(sampled).toBeGreaterThanOrEqual(23);
    expect(sampled).toBeLessThan(200);

    /* The pending card is the same DOM node with the same slot choice. */
    expect(pendingList.children).toHaveLength(1);
    expect(pendingList.children[0]).toBe(cardBefore);
    expect(findSelect(pendingList.children[0]!)).toBe(select);
    expect(select.value).toBe("3");

    /* The pong'd seat tile shows the fresh sample — the surgical repaint
     * reached exactly the one text node the pong changed. */
    const tile = byId("party-seats").children[0]!;
    expect(tile.selectorMap["small"]!.textContent)
      .toBe(`Phone connected · ${sampled} ms · direct`);
  });

  it("repaints nothing for a pong on a seat that is not an active direct " +
     "connection, and still never rebuilds the roster", () => {
    host.remotePads()[0]!.active = false;
    const pendingList = byId("party-pending-list");
    const cardBefore = pendingList.children[0]!;
    const tileText = byId("party-seats").children[0]!
      .selectorMap["small"]!.textContent;
    expect(host.testControlPong(LIVE_ID, 31)).toBe(true);
    expect(pendingList.children[0]).toBe(cardBefore);
    expect(byId("party-seats").children[0]!.selectorMap["small"]!.textContent)
      .toBe(tileText);
  });

  it("still rebuilds when the pending set's own data changes", () => {
    host.applyRoomState({type: "room_state", transitionId: 2,
      inviteExpiresAt: Date.now() + 120_000, inviteGeneration: 1,
      phase: "open", controllers: [
        {controllerId: LIVE_ID, name: "Live phone",
          controllerPublicKey: "K".repeat(87), phase: "connected", seat: 1,
          leaseGeneration: 1, connectionSequence: 1},
        {controllerId: PENDING_ID, name: "Renamed phone", phase: "pending",
          seat: null, leaseGeneration: 0, connectionSequence: 1},
      ]});
    const pendingList = byId("party-pending-list");
    expect(pendingList.children).toHaveLength(1);
    const strong = pendingList.children[0]!.children[0]!.children[0]!;
    expect(strong.textContent).toBe("Renamed phone");
  });
});
