#!/usr/bin/env python3
"""Lobby-authoritative peer drop: the room's verdict, acted on in a tick.

The room (the Durable Object behind the signaling socket) knows a member left
the moment its socket closes, and broadcasts presence=false. Before N8 nothing
acted on that: the survivor waited for the transport's own ladders -- the
control-ping bound (kMdkrMatchControlPingIntervalMs +
kMdkrMatchControlPingTimeoutMs = 20 s), or ICE teardown plus the 10 s vanish
dwell -- and raced an opponent that was not there for every one of those
seconds.

This lane drives the same in-process kill signature the transport-loss lane
uses (MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK: the peer's pump freezes and the
loopback signal hub broadcasts its presence=false, byte-for-byte what the real
service's webSocketClose does) and asserts that the survivor now resolves it
from the ROOM rather than from waiting.

ARM 1 -- the drop (plumbing on, the shipped default):
  * the departed endpoint's seats are finalised at an agreed tick before
    anything is authored past it (`[MESH] room departure ... finalised at
    tick=T result=0` -- result 0 is MDKR_MATCH_TAKEOVER_ACCEPTED);
  * the loss is typed as the ROOM's verdict, not a ladder's: `[MESH] peer LOST
    reason=<PeerDeparted>`, parsed from the header rather than hard-coded;
  * the truthful attribution is unchanged -- OPPONENT_LEFT, never a demotion
    to a connection-establishment card;
  * the survivor reaches that card within CARD_TICKS authored ticks of the
    sever: the mid-race latch (`[online-live] peer lost mid-race at tick N`)
    fires within two ticks of the presence drop, after the two authored ticks
    of peer silence the D1 grace listens for;
  * the clean return still holds -- LEFT, exit 0, zero leaks, no watchdog.

ARM 2 -- the positive control (MDKR_ONLINE_LOBBY_DROP=0): the identical run
with the event plumbing disabled must NOT resolve within those four ticks. It
falls back to the transport ladder, resolving as PingTimeout somewhere inside
the named ping bound -- the pre-N8 stall, and the measurement that makes arm
1's number mean something.

ARM B -- the room-service wobble (D1). The room reports the member gone while
that member is plainly still racing: MDKR_APP_TEST_ONLINE_ROOM_DEPARTURE_AT_
TICK drops only the peer's loopback presence and leaves the peer pumping,
sealing and ponging. The survivor must NOT card. Its verdict is held --
`[MESH] room departure ... HELD` -- no seat is finalised, and the race carries
on until the peer is really severed much later, at which point the transport's
own ping ladder ends it, as it does whenever the room says nothing.

ARM C -- arm B's positive control (MDKR_ONLINE_LOBBY_DROP_GRACE=0). The
identical wobble with the grace length set to zero is the pre-D1 code: it
finalises the seat and cards within CARD_TICKS, a false "opponent left" with
the peer still sending. That is the defect D1 removes, and it is what makes
arm B's silence attributable to the grace rather than to a run that happened
not to drop.

The determinism half of the fix is adjudicated where it can be adjudicated
exactly: tests/test_match_transport.c drives a finalised transport and a
reference transport whose departed peer simply sends neutral input from the
same tick, and asserts the committed canonical frames are byte-identical. The
canonical frame stream is the simulation's only input, so identical frames
under an identical seed are an identical run; this lane covers promptness and
truthfulness, which that unit cannot see.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from harness_utils import ABORT_MARKERS, resolve_binary
from online_lane_util import (
    DIRECT_BOOT_RE, FORBIDDEN_ONLINE, ONLINE_RACE_RE, SESSION_END_RE,
    forbidden_marker, make_fail, run_engine,
)

ROOT = Path(__file__).resolve().parent.parent
TICKS = 40000
SEVER_TICK = 60           # well past the opening tick; round 1 is under way
# The ruled promptness: two authored ticks to reach the card, plus the two
# authored ticks of peer silence the D1 grace listens for first.
GRACE_TICKS = 2
CARD_TICKS = 2 + GRACE_TICKS
# Arm B: the room's verdict lands here, the peer keeps racing, and only this
# much later is it really severed so a ladder can end the run.
DEPART_TICK = 60
LATE_SEVER_TICK = 150
DETECT_SLACK_SECONDS = 8  # scheduling slack on top of the named ladder bound
AUTHORED_HZ = 30

SEVER_RE = re.compile(
    r"^\[online-live\] TEST: peer transport SEVERED at tick (\d+)",
    re.MULTILINE)
FINALISE_RE = re.compile(
    r"^\[MESH\] room departure ep=\d+ slot=(\d+) finalised at tick=(\d+) "
    r"result=(-?\d+)", re.MULTILINE)
MESH_LOST_RE = re.compile(
    r"^\[MESH\] peer LOST ep=\d+ reason=(\d+) channelsReady=\d+ offerer=\d+",
    re.MULTILINE)
MESH_LOST_FAILURE_RE = re.compile(
    r"^\[MESH\] peer LOST ep=\d+ reason=(\d+) -> failure=(\d+)", re.MULTILINE)
MIDRACE_LATCH_RE = re.compile(
    r"^\[online-live\] peer lost mid-race at tick (\d+)", re.MULTILINE)
DEPART_RE = re.compile(
    r"^\[online-live\] TEST: peer ROOM PRESENCE dropped at tick (\d+)",
    re.MULTILINE)
HELD_RE = re.compile(
    r"^\[MESH\] room departure ep=\d+ HELD: (\d+) authenticated packets",
    re.MULTILINE)
GRACEFUL_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: online peer/input lost", re.MULTILINE)
HOST_SHUTDOWN_RE = re.compile(
    r"^\[HOST-SHUTDOWN\] rom=(\d+) arena=(\d+) delayedFree=(\d+)",
    re.MULTILINE)
BEGIN_LOBBY_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\)", re.MULTILINE)
WATCHDOG_MARKERS = ("round advance TIMEOUT", "descless wait TIMEOUT")

MESH_HEADER = ROOT / "platform/online/match_peer_transport.h"
VIEW_HEADER = ROOT / "platform/online/lobby_view_model.h"

FORBIDDEN = tuple(
    m for m in FORBIDDEN_ONLINE if m != "launcher input provider rejected")

fail = make_fail("lobby drop")


def parse_constant_ms(name: str, text: str) -> int | None:
    m = re.search(rf"unsigned {re.escape(name)} = (\d+)u", text)
    return int(m.group(1)) if m else None


def parse_enum_index(text: str, enum_name: str, entry: str,
                     beta: bool) -> int | None:
    """Index of `entry` in a value-less sequential C enum, honoring the
    MDKR_ENABLE_ONLINE_BETA arm (build-beta compiles it in)."""
    m = re.search(rf"enum {enum_name} \{{(.*?)\}}", text, re.DOTALL)
    if m is None:
        m = re.search(rf"enum (?:class )?{enum_name}\s*\{{(.*?)\}};", text,
                      re.DOTALL)
    if m is None:
        return None
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.DOTALL)
    body = re.sub(r"//[^\n]*", "", body)
    if beta:
        body = body.replace("#if MDKR_ENABLE_ONLINE_BETA", "").replace(
            "#endif", "")
    else:
        body = re.sub(r"#if MDKR_ENABLE_ONLINE_BETA.*?#endif", "", body,
                      flags=re.DOTALL)
    names = []
    for token in body.split(","):
        token = token.strip()
        if not token:
            continue
        name = token.split("=")[0].strip()
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            names.append(name)
    return names.index(entry) if entry in names else None


def base_env() -> dict[str, str]:
    return {
        # The production takeover shape, identical to the transport-loss
        # lane's: descriptor-less lobby-start, single-endpoint advance,
        # tournament round 1.
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
        "MDKR_APP_TEST_ONLINE_MODE": "tournament",
        "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
        "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
        "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
        "MDKR_FORCE_LAPS": "1",
        "MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK": str(SEVER_TICK),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build-beta")
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=420)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    mesh_text = MESH_HEADER.read_text(encoding="utf-8")
    interval = parse_constant_ms("kMdkrMatchControlPingIntervalMs", mesh_text)
    stale = parse_constant_ms("kMdkrMatchControlPingTimeoutMs", mesh_text)
    if interval is None or stale is None:
        return fail("could not parse the control-ping constants from "
                    f"{MESH_HEADER}")
    ladder_bound_ticks = (((interval + stale) // 1000) +
                          DETECT_SLACK_SECONDS) * AUTHORED_HZ

    departed_reason = parse_enum_index(
        mesh_text, "MdkrMatchPeerLostReason", "PeerDeparted", beta=True)
    ping_timeout_reason = parse_enum_index(
        mesh_text, "MdkrMatchPeerLostReason", "PingTimeout", beta=True)
    view_text = VIEW_HEADER.read_text(encoding="utf-8")
    opponent_left = parse_enum_index(
        view_text, "MdkrOnlineViewFailure",
        "MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT", beta=True)
    if (departed_reason is None or ping_timeout_reason is None or
            opponent_left is None):
        return fail("could not parse the reason/failure enumerators from the "
                    "headers")

    # ===== Arm 1: the drop (shipped default) ================================
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=base_env(),
            prefix="mdkr64-online-lobby-drop-")
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a hang instead of a bounded "
                    f"return): {error}")

    marker = forbidden_marker(output, *FORBIDDEN, *ABORT_MARKERS)
    if marker:
        return fail(f"observed fatal/abort marker {marker!r}", output)
    if returncode != 0:
        return fail(f"process exited {returncode}, expected 0 (clean LEFT "
                    f"return)", output)

    # --- Non-vacuous: the descriptor-less race ran and was severed ----------
    if len(BEGIN_LOBBY_RE.findall(output)) != 1:
        return fail("the session did not begin descriptor-less (lobby-start) "
                    "exactly once", output)
    if not DIRECT_BOOT_RE.search(output) or not ONLINE_RACE_RE.search(output):
        return fail("round 1 never booted into the online rollback race",
                    output)
    sever = SEVER_RE.search(output)
    if not sever:
        return fail("the transport-sever seam never fired", output)
    sever_tick = int(sever.group(1))

    # --- The seat is finalised at an agreed tick ----------------------------
    finalise = FINALISE_RE.search(output)
    if not finalise:
        return fail("the departed endpoint's seat was never finalised (no "
                    "`[MESH] room departure ... finalised at tick=` line) -- "
                    "the room's verdict reached nothing", output)
    finalise_tick = int(finalise.group(2))
    if int(finalise.group(3)) != 0:
        return fail(f"the finalisation was refused (result="
                    f"{finalise.group(3)}, expected 0/ACCEPTED) -- the agreed "
                    f"tick was not schedulable", output)
    if finalise_tick <= sever_tick:
        return fail(f"finalisation tick {finalise_tick} is not ahead of the "
                    f"sever at {sever_tick} -- an authored tick's inputs are "
                    f"already spent", output)

    # --- The loss is the ROOM's verdict, not a ladder's ---------------------
    lost = MESH_LOST_RE.search(output)
    if not lost:
        return fail("NO `[MESH] peer LOST` after the sever", output)
    if int(lost.group(1)) != departed_reason:
        return fail(f"peer LOST reason={lost.group(1)}, expected the room's "
                    f"PeerDeparted ({departed_reason}) -- with the plumbing "
                    f"on, the room must resolve the departure before any "
                    f"transport ladder does", output)

    # --- Truthful attribution: OPPONENT_LEFT, never a demotion --------------
    mapped = MESH_LOST_FAILURE_RE.search(output)
    if not mapped:
        return fail("the adapter never mapped the loss onto the recovery card",
                    output)
    if int(mapped.group(2)) != opponent_left:
        return fail(f"loss mapped to failure={mapped.group(2)}, expected "
                    f"OPPONENT_LEFT ({opponent_left})", output)

    # --- The ruled promptness ----------------------------------------------
    latch = MIDRACE_LATCH_RE.search(output)
    if not latch:
        return fail("the room reported the departure but the mid-race latch "
                    "never ended the race", output)
    latch_tick = int(latch.group(1))
    drop_ticks = latch_tick - sever_tick
    if drop_ticks > CARD_TICKS:
        return fail(f"the survivor reached the opponent-left card "
                    f"{drop_ticks} ticks after the room reported the "
                    f"departure (sever at {sever_tick}, latch at "
                    f"{latch_tick}) -- the ruled bound is {CARD_TICKS}",
                    output)

    # --- Clean return: LEFT, exit 0, zero leaks, NO watchdog ----------------
    if not GRACEFUL_LEFT_RE.search(output):
        return fail("no clean return-to-room ([online-session] LEFT: online "
                    "peer/input lost)", output)
    end = SESSION_END_RE.findall(output)
    if not end or end[-1][0] != "LEFT" or int(end[-1][1]) != 0:
        return fail(f"session end {end[-1] if end else 'missing'}, expected "
                    f"(LEFT, 0)", output)
    for watchdog in WATCHDOG_MARKERS:
        if watchdog in output:
            return fail(f"the watchdog path fired ({watchdog!r})", output)
    shutdown = HOST_SHUTDOWN_RE.findall(output)
    if not shutdown or tuple(map(int, shutdown[-1])) != (0, 0, 0):
        return fail(f"host teardown leaked: "
                    f"{shutdown[-1] if shutdown else 'no witness'}", output)

    # ===== Arm 2: the positive control ======================================
    control_env = base_env()
    control_env["MDKR_ONLINE_LOBBY_DROP"] = "0"
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=control_env,
            prefix="mdkr64-online-lobby-drop-off-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[control] engine run timed out: {error}")
    marker = forbidden_marker(output, *FORBIDDEN, *ABORT_MARKERS)
    if marker:
        return fail(f"[control] observed fatal/abort marker {marker!r}",
                    output)
    if returncode != 0:
        return fail(f"[control] process exited {returncode}, expected 0",
                    output)
    if FINALISE_RE.search(output):
        return fail("[control] a seat was finalised with the plumbing "
                    "disabled -- the kill switch does not switch anything "
                    "off", output)
    control_sever = SEVER_RE.search(output)
    control_latch = MIDRACE_LATCH_RE.search(output)
    if not control_sever or not control_latch:
        return fail("[control] the run did not sever and end the race, so it "
                    "measures nothing", output)
    control_ticks = int(control_latch.group(1)) - int(control_sever.group(1))
    if control_ticks <= CARD_TICKS:
        return fail(f"[control] the race ended {control_ticks} ticks after "
                    f"the sever with the plumbing OFF -- arm 1's "
                    f"{drop_ticks}-tick result is not attributable to the "
                    f"room's verdict", output)
    if control_ticks > ladder_bound_ticks:
        return fail(f"[control] the fallback took {control_ticks} ticks, "
                    f"beyond the named ping bound ({ladder_bound_ticks} "
                    f"ticks) -- the ladder backstop regressed", output)
    control_lost = MESH_LOST_RE.search(output)
    if not control_lost or int(control_lost.group(1)) != ping_timeout_reason:
        return fail(f"[control] the fallback resolved as reason="
                    f"{control_lost.group(1) if control_lost else 'none'}, "
                    f"expected the ladder's PingTimeout "
                    f"({ping_timeout_reason})", output)

    # ===== Arm B: the room-service wobble ===================================
    # The room says the member left; the member is still racing. The verdict
    # must be held, and the loss must come from the ladder once the peer is
    # really severed, 90 ticks later.
    wobble_env = base_env()
    wobble_env["MDKR_APP_TEST_ONLINE_ROOM_DEPARTURE_AT_TICK"] = str(DEPART_TICK)
    wobble_env["MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK"] = str(
        LATE_SEVER_TICK)
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=wobble_env,
            prefix="mdkr64-online-lobby-drop-wobble-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[wobble] engine run timed out: {error}")
    marker = forbidden_marker(output, *FORBIDDEN, *ABORT_MARKERS)
    if marker:
        return fail(f"[wobble] observed fatal/abort marker {marker!r}", output)
    if returncode != 0:
        return fail(f"[wobble] process exited {returncode}, expected 0",
                    output)
    depart = DEPART_RE.search(output)
    if not depart:
        return fail("[wobble] the room-only departure seam never fired, so "
                    "this arm measures nothing", output)
    depart_tick = int(depart.group(1))
    if FINALISE_RE.search(output):
        return fail("[wobble] a seat was finalised while the peer was still "
                    "sending -- a false opponent-left drop", output)
    held = HELD_RE.search(output)
    if not held:
        return fail("[wobble] the room's verdict was never held -- the peer "
                    "was still sending authenticated packets and the grace "
                    "had to drop the verdict", output)
    if int(held.group(1)) == 0:
        return fail("[wobble] the verdict was held on ZERO authenticated "
                    "packets, so the hold proves nothing about the peer "
                    "still racing", output)
    wobble_latch = MIDRACE_LATCH_RE.search(output)
    if not wobble_latch:
        return fail("[wobble] the race never ended, so the ladder backstop is "
                    "unmeasured", output)
    wobble_ticks = int(wobble_latch.group(1)) - depart_tick
    if wobble_ticks <= CARD_TICKS:
        return fail(f"[wobble] the race ended {wobble_ticks} ticks after the "
                    f"room's verdict -- the held verdict still dropped the "
                    f"peer", output)
    wobble_lost = MESH_LOST_RE.search(output)
    if not wobble_lost or int(wobble_lost.group(1)) != ping_timeout_reason:
        return fail(f"[wobble] the loss resolved as reason="
                    f"{wobble_lost.group(1) if wobble_lost else 'none'}, "
                    f"expected the ladder's PingTimeout "
                    f"({ping_timeout_reason}) once the peer was really "
                    f"severed", output)

    # ===== Arm C: arm B's positive control (the grace disabled) =============
    false_env = dict(wobble_env)
    false_env["MDKR_ONLINE_LOBBY_DROP_GRACE"] = "0"
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=false_env,
            prefix="mdkr64-online-lobby-drop-nograce-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[no-grace] engine run timed out: {error}")
    marker = forbidden_marker(output, *FORBIDDEN, *ABORT_MARKERS)
    if marker:
        return fail(f"[no-grace] observed fatal/abort marker {marker!r}",
                    output)
    if returncode != 0:
        return fail(f"[no-grace] process exited {returncode}, expected 0",
                    output)
    if HELD_RE.search(output):
        return fail("[no-grace] the verdict was still held with the grace "
                    "length set to zero -- the switch does not switch "
                    "anything off", output)
    if not FINALISE_RE.search(output):
        return fail("[no-grace] the identical wobble did not finalise a seat "
                    "with the grace off, so arm B's held verdict is not "
                    "attributable to the grace", output)
    false_depart = DEPART_RE.search(output)
    false_latch = MIDRACE_LATCH_RE.search(output)
    if not false_depart or not false_latch:
        return fail("[no-grace] the run did not depart and end, so it "
                    "measures nothing", output)
    false_ticks = int(false_latch.group(1)) - int(false_depart.group(1))
    if false_ticks > CARD_TICKS:
        return fail(f"[no-grace] the false drop took {false_ticks} ticks, so "
                    f"it is not the prompt room-authoritative drop arm B "
                    f"suppresses", output)

    print(f"check_online_lobby_drop: PASS (card in {drop_ticks} ticks of the "
          f"room event; {control_ticks} ticks with the plumbing off; a live "
          f"peer's verdict held for {wobble_ticks} ticks to the ladder, "
          f"{false_ticks} with the grace off)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
