#!/usr/bin/env python3
"""Drive a FULL 4-race Dino Domain cup through the in-process loopback room.

The sibling check_online_engine_boot_direct.py proves ONE online race reaches
the VISIBLE engine straight from the manifest. This gate proves the
TOURNAMENT lifecycle around it: mode=tournament + cup=0 (Dino Domain: tracks
5 Ancient Lake, 3 Fossil Canyon, 29 Jungle Falls, 7 Hot Top Volcano) runs all
four races through ONE live room, with authentic trophy-point accrual
(9/7/5/3/1, gTrophyRacePointsArray) and a REMATCH-advanced race_index after
every RESULTS phase. Round 4's Hot Top Volcano is deliberately a NARROW-mask
round (0x6, no Car): the cup itself exercises the non-0x07 admission equality
and the non-Car vehicle path inside the frozen manifest.

WHAT IS ENGINE-PROVEN VS TRANSPORT-PROVEN -- read this before trusting a
green run. The MDKR_APP_TEST_ONLINE_LIVE loopback branch (main_app.cpp) boots
the visible engine EXACTLY once per process and then tears the pair down, so
the four races split:

  Race 1 (ENGINE-proven): the visible engine boots track 5 off the live
    transport with no menu-nav script, races to the FINISH LINE, converges
    byte-for-byte with the peer ([ENGINE-ONLINE-LIVE] converged=1), ends its
    session ~2.5 s post-finish ([online-postrace]), records real placements
    ([online-results]) and the launcher reports them to the room
    ([online-live] race results reported ... accepted=1).

  Races 2-4 (TRANSPORT-proven): after the leader's REMATCH advances
    race_index, each round is driven through the SAME live adapters at the
    transport level (the tests/test_online_live_adapter.cpp lifecycle rig's
    approach, in platform/app/online_live_wiring.cpp's tournament
    continuation): both endpoints re-vote + re-Ready, the leader starts the
    round with the round track's raw ROM vehicle mask, both race transports
    reach READY on a fresh epoch with byte-identical frozen descriptors
    carrying the cup schedule's track + mask, >= 30 authored ticks are
    sealed/drained and FNV-hash-compared across both endpoints, and the
    leader publishes fixed placements (slot 0 first, slot 1 second) through
    the exact report seam the launcher uses. The engine does NOT boot again
    for rounds 2-4; their manifests are asserted, not raced.

The final standings must equal the authentic accrual: whatever race 1's real
engine placements scored, plus 9/7 per transport round (constant 0,1
placements) -- e.g. 36/28 when the visible endpoint also won race 1.

Expected wall time: the engine race runs near real time to the finish plus a
2.5 s postrace grace (~2-4 minutes with boot/teardown); the transport rounds
add a few seconds each. The default --ticks 9000 is a stall bound only -- the
engine ends its own session at the postrace witness, so a finished race never
consumes the full budget. Budget --timeout accordingly (default 900 s).

Runtime discipline: timing-sensitive engine test -- run it alone, never
concurrently with builds or other engine gates.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent
TICKS = 9000  # stall bound; the engine exits itself ~2.5 s after the finish

# Dino Domain (cup 0) in DKR trophy-race round order, with each track's raw
# ROM vehicle mask (platform/online/online_track_table.c; the engine admission
# fail-closes any drift from leveltable_vehicle_usable()).
CUP = 0
CUP_TRACKS = (5, 3, 29, 7)
CUP_MASKS = (0x7, 0x7, 0x7, 0x6)
TROPHY_POINTS = (9, 7, 5, 3, 1, 0, 0, 0)  # gTrophyRacePointsArray

ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) racedTicks=(\d+) drainCalls=(\d+) "
    r"advanceFailed=(\d+) inputEnvelopes=(\d+) transportAccepted=(\d+) "
    r"transportCorrected=(\d+) transportDrained=(\d+) foldVisible=(\d+) "
    r"foldPeer=(\d+) hashVisible=([0-9a-f]{16}) hashPeer=([0-9a-f]{16}) "
    r"converged=(\d+)$",
    re.MULTILINE,
)
ONLINE_RACE_RE = re.compile(
    r"^\[ROLLBACK\] online race: loadedTrack=(\d+) raceType=(\d+) "
    r"authoredHz=(\d+)$",
    re.MULTILINE,
)
CONFIG_RE = re.compile(
    r"^\[online-live\] loopback config mode=tournament cup=(\d+) "
    r"round1Track=(\d+) startMask=0x([0-9a-f]{2}) vehicle=(\d+)$",
    re.MULTILINE,
)
REPORTED_RE = re.compile(
    r"^\[online-live\] race results reported placements=(\d+),(\d+),(\d+),"
    r"(\d+) accepted=1$",
    re.MULTILINE,
)
RESULTS_RE = re.compile(
    r"^\[online-tournament\] results race=(\d+) race_index=(\d+) track=(\d+) "
    r"points=(\d+),(\d+),(\d+),(\d+) last_placements=(\d+),(\d+),(\d+),(\d+)$",
    re.MULTILINE,
)
REMATCH_RE = re.compile(
    r"^\[online-tournament\] rematch race_index=(\d+) "
    r"points=(\d+),(\d+),(\d+),(\d+)$",
    re.MULTILINE,
)
RACE_READY_RE = re.compile(
    r"^\[online-tournament\] race-ready round=(\d+) track=(\d+) "
    r"mask=0x([0-9a-f]{2}) epoch=(\d+) descriptorsIdentical=(\d+) "
    r"convergedTicks=(\d+) hashEqual=(\d+)$",
    re.MULTILINE,
)
TRANSPORT_RESULTS_RE = re.compile(
    r"^\[online-tournament\] transport results reported round=(\d+) "
    r"placements=0,1,255,255 accepted=1$",
    re.MULTILINE,
)
FINAL_RE = re.compile(
    r"^\[online-tournament\] final cup=(\d+) race_index=(\d+) "
    r"points=(\d+),(\d+),(\d+),(\d+) result=ok$",
    re.MULTILINE,
)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online tournament: {message}", file=sys.stderr)
    if output:
        print(output[-16000:], file=sys.stderr)
    return 1


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, default="baserom.us.v80.z64")
    parser.add_argument("--ticks", type=int, default=TICKS)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-online-tournament-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        # The direct-boot environment (no menu-nav script; the only inputs are
        # the live transport and MDKR_AUTOPILOT) plus the tournament seams.
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_TEST_ONLINE_LIVE="1",
            MDKR_APP_TEST_ONLINE_MODE="tournament",
            MDKR_APP_TEST_ONLINE_CUP=str(CUP),
            MDKR_APP_AUTOPLAY_TICKS=str(args.ticks),
            MDKR_APP_PREFS_DIR=str(run_dir / "preferences"),
            MDKR_AUDIO="0",
            MDKR_AUTOPILOT="1",
            MDKR_NO_CRASH_HANDLER="1",
            MDKR_PRESENT_RATE="original",
            MDKR_RENDERER="gl",
            MDKR_ROM=str(rom),
            MDKR_SAVE_DIR=str(run_dir / "saves"),
            MDKR_STATE_HASH="3",
            MDKR_TEST_SCRIPT_ONLY_INPUT="1",
            MDKR_VIDEO_CONFIG_PATH=str(run_dir / "video.ini"),
            MDKR64_HIDDEN="1",
        )
        if args.verbose:
            print(f"$ {binary}", flush=True)
        try:
            process = subprocess.run(
                [str(binary)], cwd=run_dir, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired as error:
            return fail(f"run timed out (a stall would look like this): "
                        f"{error}")
        output = process.stdout or ""

    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer",
                   "online race admission rejected",
                   "launcher input provider rejected",
                   "engine startup rejected before authored tick one",
                   "[online-tournament] result=error"):
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)
    if process.returncode != 0:
        return fail(f"process exited {process.returncode}", output)
    if "input-script" in output or "race_2p_split" in output:
        return fail("a menu-nav input script was loaded", output)

    # -- The wiring froze the tournament configuration. ---------------------
    config = CONFIG_RE.findall(output)
    if len(config) != 1:
        return fail("the tournament loopback config witness never fired",
                    output)
    cfg_cup, cfg_track, cfg_mask, _cfg_vehicle = config[0]
    if int(cfg_cup) != CUP or int(cfg_track) != CUP_TRACKS[0] or \
            int(cfg_mask, 16) != CUP_MASKS[0]:
        return fail(f"wrong tournament config cup={cfg_cup} "
                    f"round1Track={cfg_track} mask=0x{cfg_mask}", output)

    # -- Race 1: ENGINE-proven. ---------------------------------------------
    online_race = ONLINE_RACE_RE.findall(output)
    if len(online_race) != 1:
        return fail(f"expected exactly ONE engine race boot (the loopback "
                    f"branch boots the visible engine once per process), got "
                    f"{online_race!r}", output)
    loaded_track, race_type, authored_hz = online_race[0]
    if int(loaded_track) != CUP_TRACKS[0] or race_type != "0":
        return fail(f"engine race loaded track={loaded_track} "
                    f"type={race_type}, expected cup round 1 "
                    f"(track {CUP_TRACKS[0]}, standard)", output)

    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(f"expected one ENGINE-ONLINE-LIVE witness, got {stats!r}",
                    output)
    (result, raced, drains, advance_failed, envelopes, accepted, _corrected,
     drained, fold_visible, _fold_peer, hash_visible, hash_peer,
     converged) = stats[0]
    if int(result) != 0 or int(advance_failed) != 0:
        return fail(f"engine race failed result={result} "
                    f"advanceFailed={advance_failed}", output)
    if int(raced) < 100 or int(drains) < 100 or int(drained) <= 0 or \
            (int(envelopes) <= 0 and int(accepted) <= 0):
        return fail(f"engine race did not sustain the live seam "
                    f"racedTicks={raced} drainCalls={drains} "
                    f"inputEnvelopes={envelopes} drained={drained}", output)
    if int(converged) != 1 or hash_visible != hash_peer or \
            int(fold_visible) < 20:
        return fail(f"race 1 endpoints did not converge converged={converged} "
                    f"foldVisible={fold_visible}", output)

    # Race 1 must FINISH: postrace session end + recorded + reported results.
    if "[online-postrace] session end requested" not in output:
        return fail("race 1 never finished (no [online-postrace] session-end "
                    "witness); raise --ticks if the budget cut the race",
                    output)
    if not re.search(r"^\[online-results\] placements=", output, re.MULTILINE):
        return fail("the engine recorded no race-1 placements", output)
    reported = REPORTED_RE.findall(output)
    if len(reported) != 1:
        return fail(f"expected one accepted race-1 results report, got "
                    f"{reported!r}", output)
    race1 = tuple(int(v) for v in reported[0])
    if sorted(race1[:2]) != [0, 1] or race1[2] != 255 or race1[3] != 255:
        return fail(f"race-1 placements are not a 2-endpoint contest: "
                    f"{race1}", output)

    # -- Rounds 2-4: TRANSPORT-proven through the same room. -----------------
    rematches = [tuple(int(v) for v in row) for row in
                 REMATCH_RE.findall(output)]
    if [row[0] for row in rematches] != [1, 2, 3]:
        return fail(f"REMATCH did not advance race_index 1,2,3: {rematches!r}",
                    output)

    ready = [row for row in RACE_READY_RE.findall(output)]
    if len(ready) != 3:
        return fail(f"expected 3 transport race-ready witnesses, got "
                    f"{ready!r}", output)
    for index, row in enumerate(ready):
        rnd, track, mask, epoch, identical, conv_ticks, hash_equal = row
        round_number = index + 2
        if (int(rnd) != round_number or
                int(track) != CUP_TRACKS[round_number - 1] or
                int(mask, 16) != CUP_MASKS[round_number - 1] or
                int(epoch) != round_number or int(identical) != 1 or
                int(conv_ticks) < 30 or int(hash_equal) != 1):
            return fail(
                f"round {round_number} transport proof wrong: track={track} "
                f"(want {CUP_TRACKS[round_number - 1]}) mask=0x{mask} "
                f"(want {CUP_MASKS[round_number - 1]:#04x}) epoch={epoch} "
                f"descriptorsIdentical={identical} "
                f"convergedTicks={conv_ticks} hashEqual={hash_equal}", output)

    if len(TRANSPORT_RESULTS_RE.findall(output)) != 3:
        return fail("expected 3 accepted transport results reports", output)

    # -- Authentic trophy-point accrual across all four RESULTS phases. ------
    results = [tuple(int(v) for v in row) for row in
               RESULTS_RE.findall(output)]
    if [row[0] for row in results] != [1, 2, 3, 4]:
        return fail(f"expected RESULTS witnesses for races 1..4, got "
                    f"{results!r}", output)
    expected = [TROPHY_POINTS[race1[0]], TROPHY_POINTS[race1[1]], 0, 0]
    for race_no, race_index, track, p0, p1, p2, p3, l0, l1, l2, l3 in results:
        if race_index != race_no - 1 or track != CUP_TRACKS[race_no - 1]:
            return fail(f"race {race_no} RESULTS carried race_index="
                        f"{race_index} track={track}, expected "
                        f"{race_no - 1}/{CUP_TRACKS[race_no - 1]}", output)
        want_last = race1 if race_no == 1 else (0, 1, 255, 255)
        if (l0, l1, l2, l3) != want_last:
            return fail(f"race {race_no} last_placements=({l0},{l1},{l2},{l3})"
                        f", expected {want_last}", output)
        if race_no > 1:
            expected[0] += TROPHY_POINTS[0]
            expected[1] += TROPHY_POINTS[1]
        if [p0, p1, p2, p3] != expected:
            return fail(f"race {race_no} points=({p0},{p1},{p2},{p3}), "
                        f"authentic accrual expects {tuple(expected)}", output)

    final = FINAL_RE.findall(output)
    if len(final) != 1:
        return fail("the tournament never reached its final standings "
                    "witness", output)
    f_cup, f_index, f0, f1, f2, f3 = (int(v) for v in final[0])
    if f_cup != CUP or f_index != 3 or [f0, f1, f2, f3] != expected:
        return fail(f"final standings wrong: cup={f_cup} race_index={f_index} "
                    f"points=({f0},{f1},{f2},{f3}), expected cup {CUP} "
                    f"race_index 3 points {tuple(expected)}", output)

    if "[HOST-SHUTDOWN]" not in output:
        return fail("the engine did not tear down its host cleanly", output)

    print(
        "PASS online tournament: Dino Domain cup ran races 1-4 through ONE "
        "live loopback room -- race 1 ENGINE-proven (visible engine booted "
        f"track {loaded_track} at {authored_hz}Hz off the LIVE transport with "
        f"no menu-nav script, converged=1 hash={hash_visible}, finished, real "
        f"placements={race1[0]},{race1[1]} reported accepted=1), races 2-4 "
        "TRANSPORT-proven (REMATCH advanced race_index 1->3; frozen manifests "
        "carried tracks 3/29/7 masks 0x07/0x07/0x06 epochs 2/3/4 with "
        "byte-identical descriptors, >=30 hash-equal authored ticks and "
        "accepted results per round; round 4 is the narrow-mask no-Car "
        "round) -- final points="
        f"{expected[0]},{expected[1]} (authentic 9/7 trophy accrual) "
        "engineBoots=1 transportRounds=3"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
