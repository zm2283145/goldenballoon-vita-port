#!/usr/bin/env python3
"""Mid-race peer loss on the TRANSPORT itself: prompt, truthful, bounded.

The real-cloud finding this pins (a mid-race SIGKILL of one endpoint): the
killed process's signal socket dies, the service broadcasts presence=false,
and the peer goes silent with its channels still nominally up. The survivor's
mesh control-ping ladder was gated on `peer.present`, so the presence drop
FROZE the very liveness ladder that would have caught the kill -- `[MESH]
peer LOST` never fired, the mid-race peer-loss latch never engaged, and the
survivor ghost-raced a frozen opponent to the finish, then stalled to the
~120 s wall-clock watchdog with a generic ERROR and no truthful card.

This lane reproduces that exact transport signature IN-PROCESS (no cloud):
the descriptor-less lobby-start tournament session (the production takeover
shape, single-endpoint advance) races round 1, then the sever seam
(MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK) HARD-severs the in-process peer:
its pump is frozen (a corpse: no pongs, no drains -- what a SIGKILLed remote
looks like to the transport) and the loopback signal hub broadcasts its
presence=false (byte-for-byte what the real service's webSocketClose does).
NOTHING is refused: unlike the DROP_* input seams, every detection here must
come from the transport's own liveness ladders.

Post-fix assertions (the ruled behavior):
  * detection is TRANSPORT-STATE-KEYED and bounded: `[MESH] peer LOST
    reason=1` (PingTimeout -- the control-ping ladder, which now runs
    regardless of signal presence) within the named bound
    kMdkrMatchMidRaceLossPingBoundMs (= ping interval + ping timeout,
    parsed from match_peer_transport.h) of the sever, measured in authored
    ticks (30 Hz);
  * the TRUTHFUL attribution: the adapter maps the loss to OPPONENT_LEFT
    (`-> failure=<OPPONENT_LEFT>` parsed from lobby_view_model.h) -- peer
    departure is never demoted to a connection-establishment failure;
  * the EXISTING mid-race latch ends the race promptly (`[online-live] peer
    lost mid-race at tick N`, no ghost race) via the proven prepare_tick
    recoverable path -> clean return (LEFT, exit 0), zero leaks, no abort;
  * the WATCHDOG path is FORBIDDEN: no `round advance TIMEOUT`, no `descless
    wait TIMEOUT` -- the watchdog is the safety net, not the recovery.

PRE-FIX PROOF (fix reverted): the sever fires, but NO `[MESH] peer LOST`
ever appears, the latch never engages, and the survivor ghost-races round 1
to the finish then exits via `round advance TIMEOUT` + `descless wait
TIMEOUT` -> reason=ERROR -- the exact real-cloud kill-run signature.

False-positive guard: a laggy-but-alive peer never trips this detection --
pinned at the mesh layer by test_match_peer_transport.cpp's
presenceBlipWithHealthyChannelsIsNotPeerLoss (healthy channels keep ponging
through a signal-presence blip; no ladder fires).

SECOND ARM -- the NEAR-FINISH kill (each arm a full engine boot): the sever
lands the moment the race's genuine results are captured
(MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_RESULTS), so the loss is detected DURING
the post-race RESULTS/advance holds. Pre-fix those holds ground to the
frame-budget / wall-clock watchdogs' generic ERROR with no card (the lobby
wrap had cleared the race-scoped loss latches). Post-fix: once the mesh
declares the peer lost, the forward feed publishes the remote seats VACATED,
the engine's existing debounced remote-vacate ends the hold with the typed
clean LEFT, and the launcher latches the truthful opponent-left recovery
card on the session's return. reason=LEFT result=0, never ERROR.
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
SEVER_TICK = 60          # well past the opening tick; round 1 is under way
DETECT_SLACK_SECONDS = 8  # scheduling slack on top of the named bound
AUTHORED_HZ = 30

SEVER_RE = re.compile(
    r"^\[online-live\] TEST: peer transport SEVERED at tick (\d+)",
    re.MULTILINE)
# The transport's own typed loss (match_peer_transport.cpp peerLost).
MESH_LOST_RE = re.compile(
    r"^\[MESH\] peer LOST ep=\d+ reason=(\d+) channelsReady=\d+ offerer=\d+",
    re.MULTILINE)
# The adapter's truthful mapping of that loss onto the recovery card.
MESH_LOST_FAILURE_RE = re.compile(
    r"^\[MESH\] peer LOST ep=\d+ reason=(\d+) -> failure=(\d+)", re.MULTILINE)
# The EXISTING mid-race latch (liveDrainMatchInput) ending the visible race.
MIDRACE_LATCH_RE = re.compile(
    r"^\[online-live\] peer lost mid-race at tick (\d+)", re.MULTILINE)
# The proven recoverable trigger + clean return (the crash-fix path).
TRIGGER_RE = re.compile(
    r"^\[ROLLBACK\] launcher input provider rejected tick=\d+", re.MULTILINE)
GRACEFUL_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: online peer/input lost", re.MULTILINE)
HOST_SHUTDOWN_RE = re.compile(
    r"^\[HOST-SHUTDOWN\] rom=(\d+) arena=(\d+) delayedFree=(\d+)", re.MULTILINE)
BEGIN_LOBBY_RE = re.compile(
    r"^\[online-session\] begin: lobby-start \(no descriptor\)", re.MULTILINE)
# The watchdog path the fix makes unnecessary -- FORBIDDEN in the fixed build.
WATCHDOG_MARKERS = ("round advance TIMEOUT", "descless wait TIMEOUT")

# --- Near-finish arm witnesses -------------------------------------------
RESULTS_REPORTED_RE = re.compile(
    r"^\[online-resident-live\] race results reported "
    r"placements=\d+,\d+,\d+,\d+ accepted=1", re.MULTILINE)
SEVER_AT_RESULTS_RE = re.compile(
    r"^\[online-live\] TEST: peer transport SEVERED at results", re.MULTILINE)
# The forward feed presenting the transport truth to the engine.
SEATS_VACATED_RE = re.compile(
    r"^\[online-room\] mesh peer loss observed -> remote seats publish "
    r"vacated on the engine feed", re.MULTILINE)
# The engine's EXISTING debounced vacate exit -- wherever the hold was
# (RESULTS, or a re-front screen if the auto-REMATCH wrapped first).
VACATE_LEFT_RE = re.compile(
    r"^\[online-session\] LEFT: remote seat vacated at "
    r"(results|charselect|vehicleselect|trackselect|per-round re-wait)",
    re.MULTILINE)
# The truthful card, latched by the launcher on the session's return.
CARD_LATCHED_RE = re.compile(
    r"^\[online-live\] session ended after a mesh peer loss -> opponent-left "
    r"recovery card", re.MULTILINE)

# PingTimeout's enumerator index in MdkrMatchPeerLostReason (appended-only
# enum; parsed below rather than trusted).
MESH_HEADER = ROOT / "platform/online/match_peer_transport.h"
VIEW_HEADER = ROOT / "platform/online/lobby_view_model.h"

# The mid-race trigger line is the EXPECTED recoverable trigger here (same
# exemption as the peer-loss crash lane).
FORBIDDEN = tuple(
    m for m in FORBIDDEN_ONLINE if m != "launcher input provider rejected")

fail = make_fail("mid-race transport loss")


def parse_constant_ms(name: str, text: str) -> int | None:
    m = re.search(rf"unsigned {re.escape(name)} = (\d+)u", text)
    return int(m.group(1)) if m else None


def parse_enum_index(text: str, enum_name: str, entry: str,
                     beta: bool) -> int | None:
    """Index of `entry` in a value-less sequential C enum, honoring the
    MDKR_ENABLE_ONLINE_BETA arm (build-beta compiles it in)."""
    m = re.search(rf"enum {enum_name} \{{(.*?)\}}", text, re.DOTALL)
    if m is None:
        # `enum class Name {` or `typedef enum Name {`
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

    # The named bound, from the source of truth (never a magic lane number).
    mesh_text = MESH_HEADER.read_text(encoding="utf-8")
    interval = parse_constant_ms("kMdkrMatchControlPingIntervalMs", mesh_text)
    stale = parse_constant_ms("kMdkrMatchControlPingTimeoutMs", mesh_text)
    if interval is None or stale is None:
        return fail("could not parse the control-ping constants from "
                    f"{MESH_HEADER}")
    bound_ms = interval + stale  # == kMdkrMatchMidRaceLossPingBoundMs
    bound_ticks = ((bound_ms // 1000) + DETECT_SLACK_SECONDS) * AUTHORED_HZ

    ping_timeout_reason = parse_enum_index(
        mesh_text, "MdkrMatchPeerLostReason", "PingTimeout", beta=True)
    view_text = VIEW_HEADER.read_text(encoding="utf-8")
    opponent_left = parse_enum_index(
        view_text, "MdkrOnlineViewFailure",
        "MDKR_ONLINE_VIEW_FAILURE_OPPONENT_LEFT", beta=True)
    if ping_timeout_reason is None or opponent_left is None:
        return fail("could not parse the reason/failure enumerators from the "
                    "headers")

    extra_env = {
        # The production takeover shape: descriptor-less lobby-start,
        # single-endpoint advance, tournament round 1 (the real demo flow and
        # the exact shape of the cloud kill run this lane pins).
        "MDKR_APP_TEST_ONLINE_LIVE_LOBBY_START": "1",
        "MDKR_TEST_ONLINE_LOBBY_START": "1",
        "MDKR_APP_TEST_ONLINE_MODE": "tournament",
        "MDKR_TEST_ONLINE_LOBBY_TOURNAMENT": "1",
        "MDKR_TEST_ONLINE_RESULTS_HOST_PRESS": "1",
        "MDKR_APP_TEST_ONLINE_SINGLE_ENDPOINT": "1",
        "MDKR_FORCE_LAPS": "1",
        # The sever seam itself: freeze the peer's pump + drop its presence
        # at this authored tick. Refuses NOTHING.
        "MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK": str(SEVER_TICK),
    }
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=extra_env,
            prefix="mdkr64-online-midrace-sever-")
    except subprocess.TimeoutExpired as error:
        return fail(f"engine run timed out (a hang instead of a bounded "
                    f"return): {error}")

    # --- No abort / no fatal (the crash-fix promise holds throughout) -------
    marker = forbidden_marker(output, *FORBIDDEN, *ABORT_MARKERS)
    if marker:
        return fail(f"observed fatal/abort marker {marker!r}", output)
    if returncode != 0:
        return fail(f"process exited {returncode}, expected 0 (clean LEFT "
                    f"return)", output)

    # --- Non-vacuous: the descriptor-less race ran and was severed -----------
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

    # --- Detection: transport-state-keyed, typed, bounded --------------------
    lost = MESH_LOST_RE.search(output)
    if not lost:
        return fail("NO `[MESH] peer LOST` after the sever -- the transport "
                    "never detected the dead peer (the pre-fix starvation: "
                    "the survivor would ghost-race to the watchdog)", output)
    if int(lost.group(1)) != ping_timeout_reason:
        return fail(f"peer LOST reason={lost.group(1)}, expected the "
                    f"control-ping ladder's PingTimeout "
                    f"({ping_timeout_reason}) -- detection must be the "
                    f"transport's own liveness probe", output)

    # --- Truthful attribution: OPPONENT_LEFT, never a demotion ---------------
    mapped = MESH_LOST_FAILURE_RE.search(output)
    if not mapped:
        return fail("the adapter never mapped the loss onto the recovery "
                    "card (no `[MESH] peer LOST ... -> failure=` line)",
                    output)
    if int(mapped.group(2)) != opponent_left:
        return fail(f"loss mapped to failure={mapped.group(2)}, expected "
                    f"OPPONENT_LEFT ({opponent_left}) -- peer departure must "
                    f"never demote to a connection failure", output)

    # --- The EXISTING latch ends the race promptly (no ghost race) -----------
    latch = MIDRACE_LATCH_RE.search(output)
    if not latch:
        return fail("`[MESH] peer LOST` fired but the mid-race latch never "
                    "ended the race", output)
    latch_tick = int(latch.group(1))
    if latch_tick - sever_tick > bound_ticks:
        return fail(f"latch fired at tick {latch_tick}, "
                    f"{latch_tick - sever_tick} ticks after the sever at "
                    f"{sever_tick} -- beyond the named bound "
                    f"kMdkrMatchMidRaceLossPingBoundMs ({bound_ms} ms "
                    f"+ {DETECT_SLACK_SECONDS}s slack = {bound_ticks} ticks "
                    f"@ {AUTHORED_HZ} Hz)", output)
    if not TRIGGER_RE.search(output):
        return fail("the latch fired but the recoverable prepare_tick "
                    "trigger never followed", output)

    # --- Clean return: LEFT, exit 0, zero leaks, NO watchdog ------------------
    if not GRACEFUL_LEFT_RE.search(output):
        return fail("no clean return-to-room ([online-session] LEFT: online "
                    "peer/input lost)", output)
    end = SESSION_END_RE.findall(output)
    if not end or end[-1][0] != "LEFT" or int(end[-1][1]) != 0:
        return fail(f"session end {end[-1] if end else 'missing'}, expected "
                    f"(LEFT, 0)", output)
    for watchdog in WATCHDOG_MARKERS:
        if watchdog in output:
            return fail(f"the watchdog path fired ({watchdog!r}) -- recovery "
                        f"must come from the peer-loss latch, the watchdog is "
                        f"only the safety net", output)
    shutdown = HOST_SHUTDOWN_RE.findall(output)
    if not shutdown or tuple(map(int, shutdown[-1])) != (0, 0, 0):
        return fail(f"host teardown leaked: {shutdown[-1] if shutdown else 'no witness'}",
                    output)

    # ===== Arm 2: the NEAR-FINISH kill (loss lands in the post-race hold) ====
    nf_env = dict(extra_env)
    del nf_env["MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_TICK"]
    nf_env["MDKR_APP_TEST_ONLINE_SEVER_PEER_AT_RESULTS"] = "1"
    try:
        returncode, output = run_engine(
            binary, rom, ticks=args.ticks, timeout=args.timeout,
            verbose=args.verbose, extra_env=nf_env,
            prefix="mdkr64-online-nearfinish-sever-")
    except subprocess.TimeoutExpired as error:
        return fail(f"[near-finish] engine run timed out (a hang instead of "
                    f"a bounded return): {error}")
    marker = forbidden_marker(output, *FORBIDDEN, *ABORT_MARKERS)
    if marker:
        return fail(f"[near-finish] observed fatal/abort marker {marker!r}",
                    output)
    if returncode != 0:
        return fail(f"[near-finish] process exited {returncode}, expected 0 "
                    f"(clean LEFT return)", output)
    if len(BEGIN_LOBBY_RE.findall(output)) != 1:
        return fail("[near-finish] the session did not begin descriptor-less "
                    "(lobby-start) exactly once", output)
    if not DIRECT_BOOT_RE.search(output) or not ONLINE_RACE_RE.search(output):
        return fail("[near-finish] round 1 never booted into the online "
                    "rollback race", output)
    # GENUINE finish first, THEN the sever: the whole point of this arm.
    reported = RESULTS_REPORTED_RE.search(output)
    nf_sever = SEVER_AT_RESULTS_RE.search(output)
    if not reported:
        return fail("[near-finish] the race's results were never captured/"
                    "reported -- the arm must sever AFTER a genuine finish",
                    output)
    if not nf_sever:
        return fail("[near-finish] the results-hold sever seam never fired",
                    output)
    if nf_sever.start() < reported.start():
        return fail("[near-finish] the sever fired before the results were "
                    "reported -- this arm's loss must land in the post-race "
                    "window", output)
    # Typed transport detection while the session holds post-race.
    if not MESH_LOST_RE.search(output):
        return fail("[near-finish] NO `[MESH] peer LOST` after the post-race "
                    "sever -- the transport never detected the dead peer",
                    output)
    # The transport truth reaches the engine feed, the engine's EXISTING
    # debounced vacate ends the hold typed, and the launcher latches the
    # truthful card -- never the watchdogs' generic ERROR.
    if not SEATS_VACATED_RE.search(output):
        return fail("[near-finish] the forward feed never published the lost "
                    "peer's seat vacated -- the engine hold has no way to end "
                    "typed", output)
    if not VACATE_LEFT_RE.search(output):
        return fail("[near-finish] the engine hold never took the typed "
                    "remote-vacate LEFT exit", output)
    if not CARD_LATCHED_RE.search(output):
        return fail("[near-finish] the opponent-left recovery card was never "
                    "latched on the session's return (the generic-recovery "
                    "swallow)", output)
    end = SESSION_END_RE.findall(output)
    if not end or end[-1][0] != "LEFT" or int(end[-1][1]) != 0:
        return fail(f"[near-finish] session end "
                    f"{end[-1] if end else 'missing'}, expected (LEFT, 0) -- "
                    f"a watchdog ERROR is the pre-fix generic path", output)
    if "descless wait TIMEOUT" in output:
        return fail("[near-finish] the descriptor-less wall-clock watchdog "
                    "fired -- recovery must come from the typed vacate exit",
                    output)
    shutdown = HOST_SHUTDOWN_RE.findall(output)
    if not shutdown or tuple(map(int, shutdown[-1])) != (0, 0, 0):
        return fail(f"[near-finish] host teardown leaked: "
                    f"{shutdown[-1] if shutdown else 'no witness'}", output)
    nf_exit = VACATE_LEFT_RE.search(output)

    print(f"PASS online mid-race transport loss: severed at tick "
          f"{sever_tick}, PingTimeout peer LOST -> OPPONENT_LEFT "
          f"({opponent_left}) -> mid-race latch at tick {latch_tick} "
          f"(+{latch_tick - sever_tick} ticks <= {bound_ticks}; named bound "
          f"{bound_ms} ms) -> clean LEFT rc 0, zero leaks, no watchdog, "
          f"no abort; [near-finish] genuine finish, post-race sever, typed "
          f"vacate exit at {nf_exit.group(1)}, opponent-left card latched, "
          f"(LEFT, 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
