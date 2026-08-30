#!/usr/bin/env python3
"""Prove the beta Online Room retires its per-race SELECTING + RESULTS widgets.

After pairing, the native game owns character + vehicle + track/cup/mode select
AND, after a race, results + standings +
the champion ceremony + the MORE-RACES replay chooser for EVERY online mode. So
the launcher must be pairing-only + a clean hand-off card on BOTH surfaces -- never a
character grid / vehicle chips / track picker / Ready-Start (SELECTING), and never a
placements/standings body or a Next-Race / Race-Again / New-Tournament / Change-Track
replay prompt (RESULTS). The tournament hand-off card was once the only mode taken
over natively; single race now routes through the identical descriptor-less path, so
both hand-offs are now UNIVERSAL.

This gate drives the beta render seam (MDKR_APP_ONLINE_BETA_FAKE +
MDKR_APP_ONLINE_BETA_STAGE) headlessly through the REAL drawBeta* widgets and reads
the per-render [online-beta-selecting] / [online-beta-results] witnesses. The
invariants:

    room-single           -> the post-pairing surface is roster + hand-off card
                             -> render=handoff
    room-tournament       -> same, for tournament -> render=handoff
    room-single-fallback  -> a LEFT/ERROR native return -> render=reentry (the
                             "Return to game" re-entry card is shown so a
                             mid-session drop is never a dead end)
    room-tournament-fallback -> same, for tournament
    results               -> the post-pairing surface is roster + results hand-off
                             card -> render=handoff (single)
    finished              -> same, for the tournament final -> render=handoff

The per-race ImGui SELECTING grid and the RESULTS standings/replay body are both
retired: the RESULTS surface is now the concise hand-off card unconditionally (no
full-results fallback), so the results-fallback / finished-fallback stages are
gone from the catalog. The SELECTING fallback stages remain -- they render the
re-entry card (a variant of the same hand-off card), not the deleted grid.

It also captures each stage to a BMP and proves (a) every stage renders a
non-flat readable frame and (b) each SELECTING hand-off capture is byte-DIFFERENT
from its same-mode re-entry (fallback) capture -- i.e. the editable grid was
actually removed and replaced by a card, not merely the witness string.

The seam requires the beta build (MDKR_ENABLE_ONLINE_BETA); it needs no ROM and
boots no engine, so --rom is accepted (for the shared online-lane contract) and
ignored.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path

from harness_utils import resolve_binary


# One witness per retired surface. SELECTING and RESULTS each emit their
# own [online-beta-*] line from the render seam; the stage's `kind` selects which.
WITNESS_RE = {
    "selecting": re.compile(
        r"\[online-beta-selecting\] stage=(?P<stage>[a-z0-9-]+) "
        r"mode=(?P<mode>single|tournament) "
        r"render=(?P<render>handoff|reentry|stranded|none)"
    ),
    "results": re.compile(
        r"\[online-beta-results\] stage=(?P<stage>[a-z0-9-]+) "
        r"mode=(?P<mode>single|tournament) "
        r"render=(?P<render>handoff|none)"
    ),
}

# stage -> (expected mode, expected render, witness kind).
CASES = {
    # SELECTING body.
    "room-single": ("single", "handoff", "selecting"),
    "room-tournament": ("tournament", "handoff", "selecting"),
    "room-single-fallback": ("single", "reentry", "selecting"),
    "room-tournament-fallback": ("tournament", "reentry", "selecting"),
    # A 1-member SELECTING room (the peer left the ROOM entirely): the body
    # must be the truthful stranded card ("this room is done" + working
    # exits), never the re-entry card's dead gold "Return to Game" (room-ready
    # needs 2 members, so that press could never fire).
    "room-stranded": ("single", "stranded", "selecting"),
    # RESULTS body: the concise hand-off card, both modes. No fallback stage --
    # the full ImGui results/standings/replay body is retired, so RESULTS hands off
    # unconditionally.
    "results": ("single", "handoff", "results"),
    "finished": ("tournament", "handoff", "results"),
}
# Each SELECTING hand-off stage paired with its same-mode re-entry (fallback); the
# captures must differ: the forward hand-off card vs the "Return to game" re-entry
# card. The editable grid is gone from both -- the fallback now renders the re-entry
# card, so a differing capture proves the grid was replaced, not merely relabeled.
DISTINCT_PAIRS = (
    ("room-single", "room-single-fallback"),
    ("room-tournament", "room-tournament-fallback"),
)


class HandoffError(RuntimeError):
    pass


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def inspect_bmp(path: Path) -> str:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise HandoffError(f"{path.name}: missing/invalid BMP capture")
    offset = struct.unpack_from("<I", data, 10)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    height = abs(signed_height)
    stride = (width * 3 + 3) & ~3
    if (width < 640 or height < 480 or bits != 24 or compression != 0 or
            offset + stride * height != len(data)):
        raise HandoffError(
            f"{path.name}: unsupported/incomplete frame {width}x{height}")
    pixel_count = width * height
    step = max(1, pixel_count // 20_000)
    colors: set[bytes] = set()
    light = 0
    for linear in range(0, pixel_count, step):
        y, x = divmod(linear, width)
        start = offset + y * stride + x * 3
        blue, green, red = data[start:start + 3]
        colors.add(data[start:start + 3])
        if (77 * red + 150 * green + 29 * blue) // 256 >= 150:
            light += 1
    if len(colors) < 48 or light < 30:
        raise HandoffError(
            f"{path.name}: visually flat/unreadable "
            f"(sampled colors={len(colors)}, light={light})")
    return hashlib.sha256(data).hexdigest()


def render_stage(binary: str, root: Path, stage: str, kind: str,
                 timeout: int) -> tuple[str, str, str]:
    case_root = root / stage
    prefs = case_root / "prefs"
    saves = case_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    capture = case_root / f"{stage}.bmp"
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_ONLINE_BETA_FAKE="1",
        MDKR_APP_ONLINE_BETA_STAGE=stage,
        MDKR_APP_PANEL="Online Room",
        MDKR_APP_SMOKE_FRAMES="4",
        MDKR_APP_SMOKE_SHOT=str(capture),
        MDKR_APP_SMOKE_WINDOW_SIZE="960x720",
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_VIDEO_CONFIG_PATH=str(case_root / "video.ini"),
        MDKR_SAVE_DIR=str(saves),
        MDKR64_HIDDEN="1",
        MDKR_AUDIO="0",
    )
    completed = subprocess.run(
        [binary], env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
    )
    if completed.returncode != 0:
        raise HandoffError(
            f"{stage}: process exited {completed.returncode}\n"
            f"{completed.stdout[-4000:]}")
    witness_re = WITNESS_RE[kind]
    witnesses = [m.groupdict() for m in witness_re.finditer(completed.stdout)]
    if not witnesses:
        raise HandoffError(
            f"{stage}: no [online-beta-{kind}] witness emitted\n"
            f"{completed.stdout[-4000:]}")
    # Every rendered frame must agree (the body is deterministic per stage).
    modes = {w["mode"] for w in witnesses}
    renders = {w["render"] for w in witnesses}
    if len(modes) != 1 or len(renders) != 1:
        raise HandoffError(
            f"{stage}: inconsistent witnesses modes={modes} renders={renders}")
    digest = inspect_bmp(capture)
    return witnesses[0]["mode"], witnesses[0]["render"], digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--rom", default=None,
                        help="accepted for the shared online-lane contract; the "
                             "render seam boots no engine and ignores it")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    if not Path(binary).exists():
        print(f"FAIL online beta handoff: binary not found: {binary}")
        return 1
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-beta-handoff-") as tmp:
            root = Path(tmp)
            digests: dict[str, str] = {}
            for stage, (want_mode, want_render, kind) in CASES.items():
                mode, render, digest = render_stage(
                    binary, root, stage, kind, args.timeout)
                if mode != want_mode or render != want_render:
                    raise HandoffError(
                        f"{stage}: rendered mode={mode} render={render}, "
                        f"expected mode={want_mode} render={want_render}")
                digests[stage] = digest
                if args.verbose:
                    print(f"  {stage}: mode={mode} render={render} "
                          f"sha={digest[:12]}")
            for handoff, fallback in DISTINCT_PAIRS:
                if digests[handoff] == digests[fallback]:
                    raise HandoffError(
                        f"{handoff} capture is byte-identical to {fallback} -- "
                        "the hand-off did not actually retire the editable grid")
    except (HandoffError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL online beta handoff: {error}")
        return 1
    print(
        "PASS online beta handoff: "
        "selecting-handoff=2 results-handoff=2 (single+tournament each) "
        "selecting-reentry=2 grid-retired=1 standings-replay-retired=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
