import {describe, expect, it} from "vitest";
import {blankCompatibility, inviteRemainingMs, MATCH_LIMITS,
  MATCH_NO_CUP, MATCH_NO_PLACEMENT, MATCH_NO_TRACK,
  type MatchCommandType, type MatchCommandV1,
  type MatchCompatibilityV1, validMatchControlLog} from "../src/match/protocol";
import {createMatchLobby, dispatchMatchCommand, matchCommandFingerprint,
  validMatchLobby}
  from "../src/match/reducer";
import parityTrace from "../../../tests/fixtures/online_lobby_reducer_v1.tsv?raw";

const compatibility: MatchCompatibilityV1 = {protocolVersion: 1,
  buildId: Array.from({length: 16}, (_, i) => i + 1),
  gameplayDigest: Array.from({length: 32}, (_, i) => 128 + i),
  romRevision: 1, cadenceHz: 30};

function command(type: MatchCommandType, actor: string, id: number,
                 revision: number, value = 0, target = "0",
                 compat = blankCompatibility()): MatchCommandV1 {
  return {protocolVersion: 1, expectedRevision: revision, commandId: String(id),
    actorEndpointId: actor, type, value, targetEndpointId: target,
    compatibility: compat};
}

function canonicalParityState(lobby: NonNullable<ReturnType<typeof createMatchLobby>>,
                              result: ReturnType<typeof dispatchMatchCommand>): string {
  const members = lobby.members.map(item => [item.endpointId, Number(item.connected),
    Number(item.ready), Number(item.loaded), item.seatCount, item.lastCommandId].join(":"))
    .join(";");
  const seats = lobby.seats.map(item => [item.endpointId, item.localIndex,
    item.selectionRevision, item.voteTrack ?? "-", item.characterId ?? "-",
    item.vehicleId ?? "-"].join(":"))
    .join(";");
  const session = [lobby.mode,
    lobby.configuredTrack === MATCH_NO_TRACK ? "-" : lobby.configuredTrack,
    lobby.cupId === MATCH_NO_CUP ? "-" : lobby.cupId, lobby.raceIndex].join(":");
  const points = lobby.points.join(":");
  const placements = lobby.lastPlacements.map(item =>
    item === MATCH_NO_PLACEMENT ? "-" : item).join(":");
  return [Number(result.accepted), Number(result.duplicate),
    Number(result.leaderChanged), result.error, lobby.revision, lobby.matchEpoch,
    lobby.phase, lobby.leaderEndpointId, lobby.leaderGeneration,
    lobby.selectedTrack ?? "-", lobby.selectedVehicleMask, members, seats,
    session, points, placements].join(",");
}

function parityCompatibility(token: string): MatchCompatibilityV1 {
  if (token === "same") return structuredClone(compatibility);
  if (token === "blank") return blankCompatibility();
  if (token === "unsupported_rom") return {...structuredClone(compatibility), romRevision: 3};
  if (token === "mismatch") {
    const value = structuredClone(compatibility);
    value.gameplayDigest[3] = value.gameplayDigest[3]! ^ 1;
    return value;
  }
  throw new Error(`unknown parity compatibility token: ${token}`);
}

type SendCommand = (type: MatchCommandType, actor: "10" | "20", value?: number,
                    target?: string) => ReturnType<typeof dispatchMatchCommand>;

/* Leader 10 (seat 0) and guest 20 (seat 1) with complete selections;
 * mirrors build_two_seat_room in tests/test_online_lobby_core.c. */
function twoSeatRoom(): {
    lobby: NonNullable<ReturnType<typeof createMatchLobby>>; send: SendCommand} {
  const lobby = createMatchLobby("4242", "10", compatibility, 1)!;
  expect(dispatchMatchCommand(lobby,
    command("join", "20", 1, lobby.revision, 1, "0", compatibility)).accepted).toBe(true);
  let hostId = 1;
  let guestId = 2;
  const send: SendCommand = (type, actor, value = 0, target = "0") =>
    dispatchMatchCommand(lobby, command(type, actor,
      actor === "10" ? hostId++ : guestId++, lobby.revision, value, target));
  expect(send("set_character", "10", 1, "0").accepted).toBe(true);
  expect(send("set_vehicle", "10", 0, "0").accepted).toBe(true);
  expect(send("set_character", "20", 2, "1").accepted).toBe(true);
  expect(send("set_vehicle", "20", 0, "1").accepted).toBe(true);
  return {lobby, send};
}

function runRoundToRacing(send: SendCommand): void {
  expect(send("set_ready", "10", 1).accepted).toBe(true);
  expect(send("set_ready", "20", 1).accepted).toBe(true);
  expect(send("begin_loading", "10", 1).accepted).toBe(true);
  expect(send("ack_loaded", "10").accepted).toBe(true);
  expect(send("ack_loaded", "20").accepted).toBe(true);
  expect(send("begin_race", "10").accepted).toBe(true);
}

describe("MatchRoom protocol-v1 reducer", () => {
  it("bounds advertised invite lifetime by current room authority", () => {
    expect(inviteRemainingMs(1_000, 900)).toBe(100);
    expect(inviteRemainingMs(MATCH_LIMITS.inviteTtlMs + 10, 0))
      .toBe(MATCH_LIMITS.inviteTtlMs);
    expect(inviteRemainingMs(900, 900)).toBeNull();
    expect(inviteRemainingMs(899, 900)).toBeNull();
    expect(inviteRemainingMs(Number.MAX_SAFE_INTEGER + 1, 0)).toBeNull();
  });

  it("replays the shared native/service valid and invalid lifecycle trace", () => {
    const lobby = createMatchLobby("42", "100", compatibility, 1)!;
    for (const line of parityTrace.split(/\r?\n/)) {
      if (!line || line.startsWith("#")) continue;
      const fields = line.split("\t");
      expect(fields, line).toHaveLength(9);
      const [label, type, actor, id, revision, value, target, compat, expected] = fields;
      const result = dispatchMatchCommand(lobby, command(type as MatchCommandType,
        actor!, Number(id), Number(revision), Number(value), target,
        parityCompatibility(compat!)));
      const actual = canonicalParityState(lobby, result);
      expect(actual, label).toBe(expected);
    }
  });

  it("matches the native C command fingerprint vector", () => {
    const value = command("set_character", "100", 1, 1, 0, "0");
    expect(matchCommandFingerprint(value)).toBe("187cbf8c556a134d");
    /* Session-configuration commands hash their native command numbers
     * (17/18/19); a renumbering on either side breaks these vectors. */
    expect(matchCommandFingerprint(command("set_mode", "100", 1, 1, 1, "0")))
      .toBe("9955d62689bd6db3");
    expect(matchCommandFingerprint(command("set_config_track", "100", 1, 1, 1, "0")))
      .toBe("156bd60b300a2db0");
    expect(matchCommandFingerprint(command("set_cup", "100", 1, 1, 1, "0")))
      .toBe("280ce892bf4e7031");
  });

  it("rejects corrupt or noncontiguous persisted control history", () => {
    const lobby = createMatchLobby("1", "100", compatibility, 1)!;
    const accepted = dispatchMatchCommand(lobby,
      command("set_character", "100", 1, 1, 0, "0"));
    expect(validMatchControlLog([accepted], lobby.revision, lobby.matchEpoch)).toBe(true);
    expect(validMatchControlLog([{...accepted, revision: accepted.revision - 1}],
      lobby.revision, lobby.matchEpoch)).toBe(false);
    expect(validMatchControlLog([{...accepted, error: "capacity"}],
      lobby.revision, lobby.matchEpoch)).toBe(false);
  });

  it("completes two rounds while preserving deterministic room identity", () => {
    const lobby = createMatchLobby("42", "100", compatibility, 1)!;
    expect(dispatchMatchCommand(lobby,
      command("join", "200", 1, 1, 1, "0", compatibility)).accepted).toBe(true);
    let hostId = 1;
    let guestId = 2;
    const send = (type: MatchCommandType, actor: "100" | "200", value = 0,
                  target = "0") => dispatchMatchCommand(lobby,
      command(type, actor, actor === "100" ? hostId++ : guestId++,
        lobby.revision, value, target));
    expect(send("set_character", "100", 0, "0").accepted).toBe(true);
    expect(send("set_vehicle", "100", 0, "0").accepted).toBe(true);
    expect(send("set_character", "200", 1, "1").accepted).toBe(true);
    expect(send("set_vehicle", "200", 0, "1").accepted).toBe(true);
    expect(send("set_vote", "100", 5, "0").accepted).toBe(true);
    expect(send("set_vote", "200", 3, "1").accepted).toBe(true);
    expect(send("set_ready", "100", 1).accepted).toBe(true);
    expect(send("set_ready", "200", 1).accepted).toBe(true);
    expect(send("begin_loading", "100", 7).accepted).toBe(true);
    const firstTrack = lobby.selectedTrack;
    expect(lobby).toMatchObject({phase: "loading", matchEpoch: 1,
      selectedVehicleMask: 7});
    expect(send("ack_loaded", "100").accepted).toBe(true);
    expect(send("ack_loaded", "200").accepted).toBe(true);
    expect(send("begin_race", "100").accepted).toBe(true);
    expect(send("publish_results", "100", 0xffff_0100).accepted).toBe(true);
    expect(send("rematch", "100").accepted).toBe(true);
    expect(lobby).toMatchObject({phase: "lobby", matchEpoch: 1,
      selectedTrack: null, selectedVehicleMask: 0});
    expect(lobby.members.every(item => !item.ready && !item.loaded)).toBe(true);

    send("set_vote", "100", 5, "0"); send("set_vote", "200", 3, "1");
    send("set_ready", "100", 1); send("set_ready", "200", 1);
    expect(send("begin_loading", "100", 7).accepted).toBe(true);
    expect(lobby.matchEpoch).toBe(2);
    expect([3, 5]).toContain(firstTrack);
    expect([3, 5]).toContain(lobby.selectedTrack);
    expect(validMatchLobby(lobby)).toBe(true);
  });

  it("keeps session configuration leader-owned, bounded and validated", () => {
    const fresh = createMatchLobby("1", "100", compatibility, 1)!;
    expect(fresh).toMatchObject({mode: 0, configuredTrack: MATCH_NO_TRACK,
      cupId: MATCH_NO_CUP, raceIndex: 0, points: [0, 0, 0, 0],
      lastPlacements: Array(4).fill(MATCH_NO_PLACEMENT)});

    const {lobby, send} = twoSeatRoom();
    const before = JSON.stringify(lobby);
    expect(send("set_mode", "20", 1))
      .toMatchObject({accepted: false, error: "unauthorized"});
    expect(send("set_config_track", "20", 3))
      .toMatchObject({accepted: false, error: "unauthorized"});
    expect(send("set_cup", "20", 0))
      .toMatchObject({accepted: false, error: "unauthorized"});
    expect(send("set_mode", "10", 2))
      .toMatchObject({accepted: false, error: "invalid_state"});
    expect(send("set_config_track", "10", 256))
      .toMatchObject({accepted: false, error: "invalid_state"});
    expect(send("set_cup", "10", 5))
      .toMatchObject({accepted: false, error: "invalid_state"});
    expect(JSON.stringify(lobby)).toBe(before);

    expect(send("set_ready", "10", 1).accepted).toBe(true);
    expect(send("set_ready", "20", 1).accepted).toBe(true);
    expect(send("set_config_track", "10", 9).accepted).toBe(true);
    expect(lobby.configuredTrack).toBe(9);
    expect(lobby.members.every(item => !item.ready)).toBe(true);

    lobby.points[1] = 7;
    lobby.lastPlacements[0] = 1;
    lobby.raceIndex = 2;
    expect(validMatchLobby(lobby)).toBe(true);
    expect(send("set_mode", "10", 1).accepted).toBe(true);
    expect(lobby).toMatchObject({mode: 1, raceIndex: 0, points: [0, 0, 0, 0]});
    expect(lobby.lastPlacements[0]).toBe(MATCH_NO_PLACEMENT);
    expect(send("set_cup", "10", 2).accepted).toBe(true);
    expect(lobby).toMatchObject({cupId: 2, raceIndex: 0, configuredTrack: 9});

    lobby.points[0] = 37;
    expect(validMatchLobby(lobby)).toBe(false);
    lobby.points[0] = 0;
    lobby.lastPlacements[0] = 8;
    expect(validMatchLobby(lobby)).toBe(false);
    lobby.lastPlacements[0] = MATCH_NO_PLACEMENT;
    lobby.mode = 2;
    expect(validMatchLobby(lobby)).toBe(false);
    lobby.mode = 1;
    lobby.raceIndex = 4;
    expect(validMatchLobby(lobby)).toBe(false);
    lobby.raceIndex = 0;
    lobby.configuredTrack = 256;
    expect(validMatchLobby(lobby)).toBe(false);
    lobby.configuredTrack = MATCH_NO_TRACK;
    lobby.cupId = 5;
    expect(validMatchLobby(lobby)).toBe(false);
    lobby.cupId = MATCH_NO_CUP;
    expect(validMatchLobby(lobby)).toBe(true);
  });

  it("prefers the configured track over votes and gates tournaments on a cup", () => {
    const {lobby, send} = twoSeatRoom();
    expect(send("set_ready", "10", 1).accepted).toBe(true);
    expect(send("set_ready", "20", 1).accepted).toBe(true);
    expect(send("begin_loading", "10", 1))
      .toMatchObject({accepted: false, error: "not_ready"});
    expect(send("set_vote", "10", 5, "0").accepted).toBe(true);
    expect(send("set_vote", "20", 5, "1").accepted).toBe(true);
    expect(send("set_config_track", "10", 13).accepted).toBe(true);
    expect(send("set_ready", "10", 1).accepted).toBe(true);
    expect(send("set_ready", "20", 1).accepted).toBe(true);
    expect(send("begin_loading", "10", 1).accepted).toBe(true);
    expect(lobby.selectedTrack).toBe(13);

    const tournament = twoSeatRoom();
    expect(tournament.send("set_mode", "10", 1).accepted).toBe(true);
    expect(tournament.send("set_ready", "10", 1).accepted).toBe(true);
    expect(tournament.send("set_ready", "20", 1).accepted).toBe(true);
    expect(tournament.send("begin_loading", "10", 1))
      .toMatchObject({accepted: false, error: "not_ready"});
    expect(tournament.send("set_cup", "10", 0).accepted).toBe(true);
    expect(tournament.lobby.members.every(item => !item.ready)).toBe(true);
  });

  it("validates packed placements exactly and records single-race results", () => {
    const {lobby, send} = twoSeatRoom();
    expect(send("set_config_track", "10", 9).accepted).toBe(true);
    runRoundToRacing(send);
    const before = JSON.stringify(lobby);
    expect(send("publish_results", "10", 0xffff_0000), "duplicate placements")
      .toMatchObject({accepted: false, error: "invalid_state"});
    expect(send("publish_results", "10", 0xffff_ff00), "occupied no-placement")
      .toMatchObject({accepted: false, error: "invalid_state"});
    expect(send("publish_results", "10", 0xffff_0108), "placement out of range")
      .toMatchObject({accepted: false, error: "invalid_state"});
    expect(send("publish_results", "10", 0x00ff_0100), "unoccupied seat placed")
      .toMatchObject({accepted: false, error: "invalid_state"});
    expect(JSON.stringify(lobby)).toBe(before);

    expect(send("publish_results", "10", 0xffff_0100).accepted).toBe(true);
    expect(lobby.phase).toBe("results");
    expect(lobby.lastPlacements)
      .toEqual([0, 1, MATCH_NO_PLACEMENT, MATCH_NO_PLACEMENT]);
    expect(lobby.points).toEqual([0, 0, 0, 0]);
    expect(send("rematch", "10").accepted).toBe(true);
    expect(lobby.raceIndex).toBe(0);
    expect(lobby.lastPlacements).toEqual(Array(4).fill(MATCH_NO_PLACEMENT));
  });

  it("advances the cup schedule, accrues trophy points and wraps the series", () => {
    const dinoDomain = [5, 3, 29, 7];
    const {lobby, send} = twoSeatRoom();
    expect(send("set_mode", "10", 1).accepted).toBe(true);
    expect(send("set_cup", "10", 0).accepted).toBe(true);
    for (let round = 0; round < 4; round++) {
      runRoundToRacing(send);
      expect(lobby.raceIndex, `round ${round} race index`).toBe(round);
      expect(lobby.selectedTrack, `round ${round} schedule track`)
        .toBe(dinoDomain[round]);
      if (round === 0) {
        expect(send("set_mode", "10", 0), "config frozen outside the lobby phase")
          .toMatchObject({accepted: false, error: "invalid_state"});
      }
      expect(send("publish_results", "10", 0xffff_0100).accepted).toBe(true);
      expect(lobby.points[0], `round ${round} first-place points`)
        .toBe((round + 1) * 9);
      expect(lobby.points[1], `round ${round} second-place points`)
        .toBe((round + 1) * 7);
      expect(lobby.lastPlacements.slice(0, 2)).toEqual([0, 1]);
      expect(send("rematch", "10").accepted).toBe(true);
      if (round < 3) {
        expect(lobby.raceIndex).toBe(round + 1);
        expect(lobby.points[0]).toBe((round + 1) * 9);
        expect(lobby.lastPlacements[0]).toBe(0);
      }
    }
    /* The fourth publish reached the full 36/28 split; the wrapping rematch
     * then restarts the series with a fresh scoreboard. */
    expect(lobby.raceIndex).toBe(0);
    expect(lobby.points).toEqual([0, 0, 0, 0]);
    expect(lobby.lastPlacements).toEqual(Array(4).fill(MATCH_NO_PLACEMENT));
    expect(validMatchLobby(lobby)).toBe(true);
  });

  it("compacts seats and series state on leave and resets refilled seats", () => {
    const {lobby, send} = twoSeatRoom();
    expect(send("set_mode", "10", 1).accepted).toBe(true);
    expect(send("set_cup", "10", 0).accepted).toBe(true);
    runRoundToRacing(send);
    expect(send("publish_results", "10", 0xffff_0100).accepted).toBe(true);
    expect(lobby.points).toEqual([9, 7, 0, 0]);

    /* Leader 10 (seat 0) leaves during results: seat 1 shifts down together
     * with its points and last placement, mirroring remove_member in
     * platform/online/lobby_core.c. */
    expect(send("leave", "10")).toMatchObject({accepted: true,
      leaderChanged: true, leaderEndpointId: "20"});
    expect(lobby.seats).toHaveLength(1);
    expect(lobby.seats[0]!.endpointId).toBe("20");
    expect(lobby.points).toEqual([7, 0, 0, 0]);
    expect(lobby.lastPlacements).toEqual([1, MATCH_NO_PLACEMENT,
      MATCH_NO_PLACEMENT, MATCH_NO_PLACEMENT]);

    expect(send("rematch", "20").accepted).toBe(true);
    expect(lobby.raceIndex).toBe(1);
    let newcomerId = 1;
    const newcomer = (type: MatchCommandType, value = 0, target = "0",
                      compat = blankCompatibility()) =>
      dispatchMatchCommand(lobby, command(type, "30", newcomerId++,
        lobby.revision, value, target, compat));
    expect(newcomer("join", 1, "0", structuredClone(compatibility))
      .accepted).toBe(true);
    expect(lobby.seats[1]!.endpointId).toBe("30");
    expect(lobby.points[1], "no inherited trophy points").toBe(0);
    expect(lobby.lastPlacements[1], "no inherited placement")
      .toBe(MATCH_NO_PLACEMENT);
    expect(newcomer("set_character", 1, "1").accepted).toBe(true);
    expect(newcomer("set_vehicle", 0, "1").accepted).toBe(true);
    expect(send("set_ready", "20", 1).accepted).toBe(true);
    expect(newcomer("set_ready", 1).accepted).toBe(true);
    expect(send("begin_loading", "20", 1).accepted).toBe(true);
    expect(send("ack_loaded", "20").accepted).toBe(true);
    expect(newcomer("ack_loaded").accepted).toBe(true);
    expect(send("begin_race", "20").accepted).toBe(true);
    expect(send("publish_results", "20", 0xffff_01ff),
      "packed bytes shaped for the old in-place seat hole")
      .toMatchObject({accepted: false, error: "invalid_state"});
    expect(send("publish_results", "20", 0xffff_0100).accepted).toBe(true);
    expect(lobby.points, "attribution follows the compacted seats")
      .toEqual([16, 7, 0, 0]);
    expect(lobby.lastPlacements.slice(0, 2)).toEqual([0, 1]);
    expect(validMatchLobby(lobby)).toBe(true);
  });

  it("accepts only the 20 cup-schedule race tracks for set_config_track", () => {
    /* Byte-mirrored from kCupTracks in platform/online/lobby_core.c. */
    const raceTracks = [5, 3, 29, 7, 13, 6, 9, 28, 8, 4, 10, 30,
      19, 18, 20, 31, 17, 32, 33, 15];
    const {lobby, send} = twoSeatRoom();
    const before = JSON.stringify(lobby);
    for (const hostile of [0, 34, 26]) {
      expect(send("set_config_track", "10", hostile), `hostile id ${hostile}`)
        .toMatchObject({accepted: false, error: "invalid_state"});
    }
    expect(JSON.stringify(lobby), "rejections stay atomic").toBe(before);
    for (const track of raceTracks) {
      expect(send("set_config_track", "10", track).accepted,
        `race id ${track}`).toBe(true);
      expect(lobby.configuredTrack).toBe(track);
    }
  });

  it("rejects stale, conflicting, unauthorized and illegal work atomically", () => {
    const lobby = createMatchLobby("99", "100", compatibility, 1)!;
    dispatchMatchCommand(lobby, command("join", "200", 1, 1, 1, "0", compatibility));
    const before = JSON.stringify(lobby);
    expect(dispatchMatchCommand(lobby,
      command("set_character", "100", 1, lobby.revision, 0, "1")))
      .toMatchObject({accepted: false, error: "unauthorized"});
    expect(JSON.stringify(lobby)).toBe(before);

    const accepted = command("set_character", "100", 2, lobby.revision, 0, "0");
    expect(dispatchMatchCommand(lobby, accepted).accepted).toBe(true);
    const revision = lobby.revision;
    expect(dispatchMatchCommand(lobby, accepted)).toMatchObject({accepted: true,
      duplicate: true, revision});
    expect(dispatchMatchCommand(lobby, {...accepted, value: 2}))
      .toMatchObject({accepted: false, error: "command_conflict"});
    expect(lobby.revision).toBe(revision);
    expect(dispatchMatchCommand(lobby,
      command("set_vehicle", "100", 1, lobby.revision, 0, "0")))
      .toMatchObject({accepted: false, error: "stale_command"});
    expect(dispatchMatchCommand(lobby,
      command("set_vehicle", "100", 1, lobby.revision - 1, 0, "0")))
      .toMatchObject({accepted: false, error: "stale_revision"});
    expect(dispatchMatchCommand(lobby,
      command("set_vehicle", "100", 3, lobby.revision - 1, 0, "0")))
      .toMatchObject({accepted: false, error: "stale_revision"});

    dispatchMatchCommand(lobby, command("set_vehicle", "100", 3, lobby.revision, 2, "0"));
    dispatchMatchCommand(lobby, command("set_character", "200", 2, lobby.revision, 1, "1"));
    dispatchMatchCommand(lobby, command("set_vehicle", "200", 3, lobby.revision, 0, "1"));
    dispatchMatchCommand(lobby, command("set_vote", "100", 4, lobby.revision, 5, "0"));
    dispatchMatchCommand(lobby, command("set_vote", "200", 4, lobby.revision, 5, "1"));
    dispatchMatchCommand(lobby, command("set_ready", "100", 5, lobby.revision, 1));
    dispatchMatchCommand(lobby, command("set_ready", "200", 5, lobby.revision, 1));
    const ready = JSON.stringify(lobby);
    expect(dispatchMatchCommand(lobby,
      command("begin_loading", "100", 6, lobby.revision, 1)))
      .toMatchObject({accepted: false, error: "illegal_vehicle"});
    expect(JSON.stringify(lobby)).toBe(ready);
  });

  it("bounds capacity, elects deterministically, and preserves leave receipts", () => {
    const lobby = createMatchLobby("7", "400", compatibility, 1)!;
    expect(dispatchMatchCommand(lobby,
      command("join", "300", 1, lobby.revision, 1, "0", compatibility)).accepted).toBe(true);
    expect(dispatchMatchCommand(lobby,
      command("join", "200", 1, lobby.revision, 1, "0", compatibility)).accepted).toBe(true);
    expect(dispatchMatchCommand(lobby,
      command("join", "100", 1, lobby.revision, 1, "0", compatibility)).accepted).toBe(true);
    const full = JSON.stringify(lobby);
    expect(dispatchMatchCommand(lobby,
      command("join", "500", 1, lobby.revision, 1, "0", compatibility)))
      .toMatchObject({accepted: false, error: "capacity"});
    expect(JSON.stringify(lobby)).toBe(full);
    const leaving = command("leave", "400", 1, lobby.revision);
    expect(dispatchMatchCommand(lobby, leaving)).toMatchObject({accepted: true,
      leaderChanged: true, leaderEndpointId: "100"});
    const revision = lobby.revision;
    expect(dispatchMatchCommand(lobby, leaving)).toMatchObject({accepted: true,
      duplicate: true, revision});
    expect(lobby.members).toHaveLength(3);
  });

  it("remains deterministic, valid and fail-atomic under seeded command chaos", () => {
    const types: MatchCommandType[] = ["join", "leave", "disconnect", "reconnect",
      "set_ready", "set_vote", "begin_loading", "ack_loaded", "begin_race",
      "publish_results", "rematch", "transfer_leader", "close", "set_character",
      "set_vehicle", "cancel_loading", "set_mode", "set_config_track", "set_cup"];
    const errors = new Set<string>();
    let accepted = 0;
    let rejected = 0;
    let duplicateChecks = 0;

    for (let seed = 1; seed <= 48; seed++) {
      let randomState = (0x9e3779b9 ^ seed) >>> 0;
      const random = (bound: number) => {
        randomState ^= randomState << 13;
        randomState ^= randomState >>> 17;
        randomState ^= randomState << 5;
        return (randomState >>> 0) % bound;
      };
      let lobby = createMatchLobby(String(10_000 + seed), "100", compatibility, 1)!;
      let mirror = structuredClone(lobby);
      const history: {lastAccepted: MatchCommandV1 | null} = {lastAccepted: null};

      const applyBoth = (value: MatchCommandV1) => {
        const before = JSON.stringify(lobby);
        const result = dispatchMatchCommand(lobby, value);
        const mirrored = dispatchMatchCommand(mirror, structuredClone(value));
        expect(mirrored).toEqual(result);
        expect(mirror).toEqual(lobby);
        expect(validMatchLobby(lobby)).toBe(true);
        expect(lobby.members.length).toBeLessThanOrEqual(MATCH_LIMITS.maxEndpoints);
        expect(lobby.seats.length).toBeLessThanOrEqual(MATCH_LIMITS.maxSeats);
        expect(lobby.receipts.length).toBeLessThanOrEqual(MATCH_LIMITS.receiptWindow);
        if (result.accepted && !result.duplicate) {
          accepted++;
          expect(result.revision).toBe(JSON.parse(before).revision + 1);
          expect(JSON.stringify(lobby)).not.toBe(before);
          history.lastAccepted = structuredClone(value);
        } else {
          if (!result.accepted) { rejected++; errors.add(result.error); }
          expect(JSON.stringify(lobby)).toBe(before);
        }
        return result;
      };

      // Start every seed with enough independent actors for custody, selection,
      // disconnect and leader-election collisions to be reachable.
      for (const endpoint of ["200", "300"] as const) {
        const joined = command("join", endpoint, 1, lobby.revision, 1, "0",
          compatibility);
        expect(applyBoth(joined).accepted).toBe(true);
      }

      for (let stepIndex = 0; stepIndex < 512; stepIndex++) {
        if (lobby.phase === "closed") {
          lobby = createMatchLobby(String(1_000_000 + seed * 1000 + stepIndex),
            "100", compatibility, 1)!;
          mirror = structuredClone(lobby);
          history.lastAccepted = null;
        }
        const actor = String(100 + random(6) * 100);
        const revisionMode = random(5);
        const expectedRevision = revisionMode < 3 ? lobby.revision :
          revisionMode === 3 ? Math.max(1, lobby.revision - 1) :
          Math.min(0xffff_ffff, lobby.revision + 1);
        const value: MatchCommandV1 = {
          protocolVersion: random(20) === 0 ? 2 : 1,
          expectedRevision,
          commandId: String(1 + random(20_000)),
          actorEndpointId: actor,
          type: types[random(types.length)]!,
          value: random(12) === 0 ? 0xffff_ffff : random(300),
          targetEndpointId: String(random(6)),
          compatibility: random(4) === 0 ? structuredClone(compatibility) :
            blankCompatibility(),
        };
        applyBoth(value);

        // A retained accepted receipt must be exactly idempotent and a
        // fingerprint-changing replay must be an atomic conflict.
        const retained = history.lastAccepted;
        if (retained && stepIndex % 31 === 30) {
          const before = JSON.stringify(lobby);
          const replay = applyBoth(structuredClone(retained));
          expect(replay).toMatchObject({accepted: true, duplicate: true});
          expect(JSON.stringify(lobby)).toBe(before);
          const conflict = structuredClone(retained);
          conflict.value = conflict.value === 0xffff_ffff ? 0 : conflict.value + 1;
          expect(applyBoth(conflict)).toMatchObject({accepted: false,
            error: "command_conflict"});
          duplicateChecks++;
        }

        // Simulate eviction/reconstruction frequently; the next command must
        // behave byte-for-byte like the uninterrupted actor.
        if (stepIndex % 17 === 16) mirror = structuredClone(lobby);
      }
    }

    expect(accepted).toBeGreaterThan(100);
    expect(rejected).toBeGreaterThan(20_000);
    expect(duplicateChecks).toBeGreaterThan(20);
    expect(errors.size).toBeGreaterThanOrEqual(7);
  // This arm deliberately performs 48 * 512 commands against both an active
  // and reconstructed reducer, with invariant checks after every dispatch.
  // Give that fixed workload an explicit budget so background-priority local
  // validation remains reliable on an occupied workstation; the default 5 s
  // is a framework default, not a product performance threshold.
  }, 30_000);
});
