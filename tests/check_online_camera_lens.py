#!/usr/bin/env python3
"""A joiner endpoint keeps its full-screen output lens compatible across a race.

Regression gate for the online camera-lens defect: on a 2-endpoint online race
the NON-host endpoint renders canonical slot 1 full-screen (local viewport_count
1), while the host renders slot 0. The endpoint output-lens compatibility guard
(game/src/camera.c) recomputes the local presentation lens and compares it to the
canonical camera the fixed-tick resolver latched. That guard used the LOCAL
output viewport index (0) to pick the world region (safe-aperture vs
presentation), but the resolver latched the canonical camera using the CANONICAL
viewport index (1). The host never noticed (its local and canonical indices are
both 0); the joiner did, the moment a framed screen -- the post-race results
aperture sets the local index 0 to safe -- put viewport 0 into the 4:3 safe
region while its canonical slot 1 stayed full-screen 16:9. The guard then failed
closed and spammed a non-aborting [FATAL] every frame, and viewport_main returned
before drawing the joiner's world -- a camera glitch on the non-host endpoint.

This drives the in-process two-adapter live loopback with the JOINER made the
visible endpoint (MDKR_APP_TEST_ONLINE_LIVE_JOINER) and forces a window of
prediction/correction rollbacks (MDKR_APP_TEST_ONLINE_LIVE_PREDICT) so the run
also proves the lens survives rollback resims. It asserts the joiner mapping is
witnessed (output=0 canonical=1), the output lens is reported compatible, real
rollbacks were reconciled, NO [FATAL] appears, the two endpoints still converge
byte-for-byte, and the process exits cleanly.

Before the fix this run prints hundreds of
    [FATAL] endpoint output lens is incompatible ... output=0 canonical=1 layout=0
lines; after it, zero.
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
TICKS = 3000
# Force a window of predicted (then corrected) active-race ticks so the joiner
# actually rolls back; large enough to span race entry and produce many
# corrections without exhausting the retained rollback window.
PREDICT_WINDOW = 120

VIEW_RE = re.compile(r"^\[NET-VIEW\] output=(\d+) canonical=(\d+) layout=(\d+)$",
                     re.MULTILINE)
LENS_RE = re.compile(
    r"^\[NET-LENS\] output=(\d+) canonical=(\d+) layout=(\d+) "
    r"aspect=([0-9.]+) vfov=([0-9.]+) compatible=1$", re.MULTILINE)
RECONCILE_RE = re.compile(
    r"^\[ROLLBACK\] online correction reconciled ticks=\d+\.\.\d+ depth=\d+$",
    re.MULTILINE)
ENGINE_LIVE_RE = re.compile(
    r"^\[ENGINE-ONLINE-LIVE\] result=(-?\d+) racedTicks=(\d+) drainCalls=(\d+) "
    r"advanceFailed=(\d+) inputEnvelopes=(\d+) transportAccepted=(\d+) "
    r"transportCorrected=(\d+) transportDrained=(\d+) foldVisible=(\d+) "
    r"foldPeer=(\d+) hashVisible=([0-9a-f]{16}) hashPeer=([0-9a-f]{16}) "
    r"converged=(\d+)$", re.MULTILINE)


def fail(message: str, output: str = "") -> int:
    print(f"FAIL online camera lens: {message}", file=sys.stderr)
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
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()

    binary = Path(resolve_binary(args.build)).expanduser().resolve()
    rom = args.rom.expanduser().resolve()
    for path, label in ((binary, "binary"), (rom, "ROM")):
        if not path.is_file():
            parser.error(f"missing {label}: {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr64-camera-lens-") as temp:
        run_dir = Path(temp)
        (run_dir / "saves").mkdir()
        (run_dir / "preferences").mkdir()
        environment = clean_environment(
            LC_ALL="C",
            MDKR_APP_AUTOPLAY="1",
            MDKR_APP_TEST_ONLINE_LIVE="1",
            MDKR_APP_TEST_ONLINE_LIVE_JOINER="1",
            MDKR_APP_TEST_ONLINE_LIVE_PREDICT=str(PREDICT_WINDOW),
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
        try:
            process = subprocess.run(
                [str(binary)], cwd=run_dir, env=environment, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=args.timeout, check=False,
            )
        except subprocess.TimeoutExpired as error:
            return fail(f"run timed out (a stall would look like this): {error}")
        output = process.stdout or ""

    if "[FATAL]" in output:
        count = output.count("[FATAL]")
        return fail(
            f"observed {count} [FATAL] line(s) -- the endpoint output lens went "
            "incompatible (the defect this gate guards)", output)
    for marker in ("[CRASH]", "AddressSanitizer"):
        if marker in output:
            return fail(f"observed forbidden marker {marker!r}", output)
    if process.returncode != 0:
        return fail(f"process exited {process.returncode}", output)

    views = VIEW_RE.findall(output)
    if ("0", "1", "0") not in views:
        return fail(
            f"the joiner mapping (output=0 canonical=1 layout=0) was not "
            f"witnessed -- got {views!r}", output)

    lenses = LENS_RE.findall(output)
    if not any(lens[0] == "0" and lens[1] == "1" for lens in lenses):
        return fail(
            f"no [NET-LENS] compatible=1 witness for the joiner output=0 "
            f"canonical=1 -- got {lenses!r}", output)

    reconciles = RECONCILE_RE.findall(output)
    if len(reconciles) < 5:
        return fail(
            f"the run did not exercise enough rollback corrections "
            f"(reconciled={len(reconciles)}, expected >= 5); the lens claim "
            "would be vacuous without resims", output)

    stats = ENGINE_LIVE_RE.findall(output)
    if len(stats) != 1:
        return fail(f"expected one ENGINE-ONLINE-LIVE witness, got {stats!r}",
                    output)
    (result, raced, drains, advance_failed, _envelopes, _accepted, corrected,
     _drained, fold_visible, _fold_peer, hash_visible, hash_peer,
     converged) = stats[0]
    if int(result) != 0 or int(advance_failed) != 0:
        return fail(f"engine did not run clean (result={result} "
                    f"advanceFailed={advance_failed})", output)
    if int(corrected) < 5:
        return fail(f"transport reported too few corrections "
                    f"(transportCorrected={corrected})", output)
    if int(converged) != 1 or hash_visible != hash_peer or \
            int(fold_visible) < 20:
        return fail(
            f"endpoints did not converge byte-for-byte (converged={converged} "
            f"foldVisible={fold_visible} hashVisible={hash_visible} "
            f"hashPeer={hash_peer})", output)

    print(
        "PASS online camera lens: joiner endpoint (output=0 canonical=1 "
        f"layout=0) kept a compatible full-screen output lens across "
        f"{len(reconciles)} rollback corrections and the post-race framed view "
        f"-- zero [FATAL], racedTicks={raced} drainCalls={drains} "
        f"transportCorrected={corrected} convergedTicks={fold_visible} "
        f"hash={hash_visible}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
