#!/usr/bin/env python3
"""Prove the VISIBLE 3D engine runs a networked race driven by the LIVE transport.

This is the make-or-break gate for the VISIBLE-engine live race. Before it, the Online Room's race was
HEADLESS: the live adapter drained real WebRTC input into a headless session
bridge and never called mdkr64_engine_boot, so reaching "RACING" produced a
converging data-plane with no picture.

Here a single app process (MDKR_APP_TEST_ONLINE_LIVE=1) stands up two REAL live
adapters over the in-process loopback signal hub feeding real libdatachannel
DTLS on 127.0.0.1, drives them through create/join/preflight/loading to a ready
race transport, then boots the VISIBLE engine (mdkr64_engine_boot) on endpoint
A's live transport while endpoint B seals real input over the mesh. The engine
navigates to Ancient Lake via tests/input_scripts/race_2p_split.txt; from the
race level on, its per-tick canonical input comes from the LIVE adapter's match
transport -- the exact seam that used to be a loopback simulator.

The gate asserts the engine:
  * booted an ONLINE rollback race on the agreed track,
  * advanced authored race ticks driven by the live match-input source,
  * folded REAL peer input that crossed the mesh through the transport,
  * did NOT stall (no advance failure, clean host teardown, exit 0),
and, as convergence evidence, that the two independent endpoints agreed
byte-for-byte on the canonical input the visible engine consumed.
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
# Import the ONE golden race-hash literal rather than duplicating it: the
# direct-boot gate pins it, and this lane asserts equality with the SAME value
# so the PAL==US bit-identity is a machine gate, not an observation.
from check_online_engine_boot_direct import GOLDEN_RACE_HASH

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "tests/input_scripts/race_2p_split.txt"
# race_2p_split reaches the race level (rollback runtime active) at ~tick 2491;
# run past it so a few hundred authored race ticks flow through the live seam.
TICKS = 3000

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


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online engine boot: {message}", file=sys.stderr)
    if output:
        print(output[-16000:], file=sys.stderr)
    return 1


def golden_matches(observed: str, expected: str) -> bool:
    """The single golden race-hash comparison, factored so the lane can run a
    coded non-vacuity control against it (a deliberately wrong hash must NOT
    match, or the golden gate is vacuous)."""
    return observed == expected


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
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument(
        "--authored-hz", type=int,
        help="pin the authored online cadence the admitted race must run at "
             "(cross-region contract: every online race races at 30)",
    )
    parser.add_argument(
        "--expect-hash", default=GOLDEN_RACE_HASH,
        help="assert the converged race hash EQUALS this golden literal (default "
             f"{GOLDEN_RACE_HASH}, imported from check_online_engine_boot_direct);"
             " pass '' to skip. A forced-NTSC PAL epoch must reach the SAME "
             "canonical hash a US epoch does -- PAL==US bit-identity as a gate.",
    )
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM"), (SCRIPT, "input script")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-online-engine-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_TEST_ONLINE_LIVE="1",
            MDKR_APP_AUTOPLAY_INPUT_SCRIPT=str(SCRIPT),
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
            return fail(f"engine run timed out (a stall would look like this): "
                        f"{error}")
        output = process.stdout or ""

    for marker in ("[FATAL]", "[CRASH]", "AddressSanitizer",
                   "online race admission rejected",
                   "launcher input provider rejected",
                   "engine startup rejected before authored tick one"):
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)

    if process.returncode != 0:
        return fail(f"process exited {process.returncode}", output)

    if "[online-live] booting visible engine" not in output:
        return fail("the visible engine was never booted on the live transport",
                    output)

    online_race = ONLINE_RACE_RE.findall(output)
    if not online_race:
        return fail("the engine never entered an ONLINE rollback race", output)
    loaded_track, race_type, authored_hz = online_race[0]
    if loaded_track != "5" or race_type != "0":
        return fail(
            f"online race loaded the wrong contest track={loaded_track} "
            f"type={race_type} (expected Ancient Lake 5, standard 0)", output)
    if args.authored_hz is not None and authored_hz != str(args.authored_hz):
        return fail(
            f"online race authored at {authored_hz}Hz, expected "
            f"{args.authored_hz}Hz for this ROM region", output)

    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(f"expected one ENGINE-ONLINE-LIVE witness, got {stats!r}",
                    output)
    (result, raced, drains, advance_failed, envelopes, accepted, corrected,
     drained, fold_visible, fold_peer, hash_visible, hash_peer,
     converged) = stats[0]

    if int(result) != 0:
        return fail(f"engine boot returned {result}", output)
    if int(advance_failed) != 0:
        return fail("the live match-input source failed to advance the race "
                    "(a stall at the transport seam)", output)
    # A healthy sustained race, not a couple of ticks before an early quit (the
    # kind of regression a stray pause/quit button in the canonical input causes).
    if int(raced) < 100 or int(drains) < 100:
        return fail(f"the visible race did not sustain enough authored ticks "
                    f"through the live seam (racedTicks={raced} "
                    f"drainCalls={drains}, expected >= 100)", output)
    if int(envelopes) <= 0 and int(accepted) <= 0:
        return fail(f"no real peer input crossed the mesh into the transport "
                    f"(inputEnvelopes={envelopes} transportAccepted={accepted})",
                    output)
    if int(drained) <= 0:
        return fail(f"the transport drained no authored ticks "
                    f"(transportDrained={drained})", output)
    if "[HOST-SHUTDOWN]" not in output:
        return fail("the engine did not tear down its host cleanly", output)

    converged_ok = int(converged) == 1 and hash_visible == hash_peer and \
        int(fold_visible) >= 20
    if not converged_ok:
        return fail(
            f"the two endpoints did not converge on the canonical race input "
            f"(converged={converged} foldVisible={fold_visible} "
            f"foldPeer={fold_peer} hashVisible={hash_visible} "
            f"hashPeer={hash_peer})", output)

    # Machine-gate the headline cross-region result. Peer==peer above proves the
    # two endpoints agree with each other; this proves they agree on the SAME
    # canonical value a US epoch reaches, so a forced-NTSC PAL epoch is
    # bit-identical to a US one. The literal lives once, in
    # check_online_engine_boot_direct.GOLDEN_RACE_HASH.
    #
    # The golden pins the CANONICAL scenario ONLY: --ticks reshapes the run (and
    # thus the fold window and the converged hash), so on a non-canonical
    # scenario auto-skip the assert with a printed notice rather than firing a
    # misleading "determinism drift" -- mirroring
    # check_online_engine_boot_direct.py's --track/--mask skip.
    canonical_scenario = args.ticks == TICKS
    if args.expect_hash and canonical_scenario:
        if not golden_matches(hash_visible, args.expect_hash):
            return fail(
                f"the converged race hash {hash_visible} != the GOLDEN "
                f"{args.expect_hash} -- this online boot no longer reaches the "
                f"canonical deterministic race sim (a determinism drift, or a "
                f"legit ROM/toolchain change that needs GOLDEN_RACE_HASH "
                f"bumped)", output)
        # Coded non-vacuity control (reuses THIS run's hash; no second boot): the
        # golden comparison MUST reject a hash that differs by even one nibble. A
        # comparison that accepted anything would leave the gate above vacuous.
        wrong_hash = ("0" if hash_visible[:1] != "0" else "1") + hash_visible[1:]
        if golden_matches(hash_visible, wrong_hash):
            return fail(
                f"the golden comparison is vacuous: it accepted a deliberately "
                f"wrong expected hash {wrong_hash}", output)
    elif args.expect_hash:
        print(
            f"NOTE online engine boot: golden hash assert skipped for a "
            f"non-canonical scenario (--ticks {args.ticks} != {TICKS}); "
            f"observed hash={hash_visible}")

    print(
        "PASS online engine boot: the VISIBLE engine ran a networked race on "
        f"track {loaded_track} at {authored_hz}Hz driven by the LIVE adapter "
        f"transport -- racedTicks={raced} drainCalls={drains} "
        f"inputEnvelopes={envelopes} transportAccepted={accepted} "
        f"transportDrained={drained} corrected={corrected} "
        f"convergedTicks={fold_visible} hash={hash_visible}"
        + ("==GOLDEN" if (args.expect_hash and canonical_scenario) else "")
        + " engineExit=clean noStall=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
