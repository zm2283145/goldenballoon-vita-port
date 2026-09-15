#!/usr/bin/env python3
"""Presentation randomness must not advance the authoritative RNG.

The port carries two random streams. ``rand_range()`` steps
``gCurrentRNGSeed``/``gPrevRNGSeed``, the retail pair that
``platform/rollback/rollback_game_authority.c`` registers in the rollback
snapshot and that the ``[SIMHASH]`` state hash covers.
``presentation_rand_range()`` steps ``gPresentationRNGSeed``, a
platform-private word that is deliberately NOT in that registry, so nothing it
produces can steer a rollback or a peer.

The claim this gate makes is the split's whole point: presentation randomness
must not advance the authoritative pair. Menu idle at enhanced cadence isolates
that. Note what the route is NOT: at MDKR_SIMULATION_CADENCE=enhanced with
MDKR_SYNTH_FIELDS=1 the simulation ticks on EVERY presented frame, and the
[SIMHASH] hash changes on every one of them, so these are not frames that
present without advancing the simulation. They are authoritative ticks whose
settled menu-idle logic happens to draw no random numbers, while texture
animation keeps drawing from the presentation stream. That is the separation the
gate needs: two streams running side by side over the same frames, one of them
required to stand still.

Both halves are asserted, because either one alone passes for the wrong reason:
a run where nothing at all draws randomness would satisfy "the seeds did not
move", and a run that never reaches a settled frame would satisfy it too.

Arms:

* **A -- the assertion.** Over the settled window, ``gCurrentRNGSeed`` and
  ``gPrevRNGSeed`` are byte-identical on every frame -- across authoritative
  ticks, not across skipped ones -- while the presentation draw counter strictly
  increases and the presentation seed takes many distinct values. Before the
  window the authoritative seed is required to have moved, which is what proves
  the trace can observe authoritative movement at all -- without it a trace that
  printed a constant would pass.
* **B -- positive control.** ``MDKR_TEST_RENDER_IMPURITY=1`` is the existing
  render-purity seam: it performs one authoritative RNG write inside every
  non-skipped render. That is exactly the defect this gate exists to catch --
  a presentation-side caller reaching the simulation stream -- so arm A's
  pin MUST fail under it, while the presentation draw counts stay identical,
  proving the control moved only the half it claims to.
* **C -- reproducibility.** The presentation stream is seeded from a constant
  derived from the ROM seed, not from host state, so two runs of the same route
  emit byte-identical rows. Lanes that diff rendered output between two arms of
  one route depend on this; a host-seeded presentation stream would break them
  silently.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, find_fatal, resolve_binary

FRAMES = 400
# The attract screen settles well before this; everything from here to the end
# of the run is required to be a settled frame: an authoritative tick that
# draws no randomness of its own.
WINDOW_START = 150
MIN_DRAWS = 100
MIN_DISTINCT = 16

ROW_RE = re.compile(
    r"^\[RNGSPLIT\] frame=(\d+) sim=([0-9a-f]{8}) prev=([0-9a-f]{8}) "
    r"pres=([0-9a-f]{8}) draws=(\d+)$", re.MULTILINE)


class Row:
    __slots__ = ("frame", "sim", "prev", "pres", "draws")

    def __init__(self, match: re.Match[str]) -> None:
        self.frame = int(match.group(1))
        self.sim = match.group(2)
        self.prev = match.group(3)
        self.pres = match.group(4)
        self.draws = int(match.group(5))


def fail(message: str, output: str = "") -> int:
    print(f"check_presentation_rng_split: FAIL\n  - {message}")
    if output:
        print(output[-3000:])
    return 1


def run_arm(binary: Path, rom: Path, root: Path, label: str,
            extra_env: dict[str, str], timeout: int,
            verbose: bool) -> tuple[list[Row], str]:
    """Drive the idle attract route and collect its [RNGSPLIT] rows."""
    run_dir = root / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_RENDERER="gl",
        MDKR_RNG_SPLIT_TRACE="1",
        # One authored tick per presented field. The simulation still ticks on
        # every frame; what a settled attract screen gives is a tick whose own
        # logic draws no randomness. At the original two-field cadence
        # cadence_compat_rand_range() routes the HUD back onto the authoritative
        # stream for byte-exact ROM ordering and the presentation stream never
        # advances at all -- the assertion would hold vacuously.
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_SAVE_DIR=str(save_dir),
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
        MDKR64_HIDDEN="1",
    )
    env.update(extra_env)
    command = [
        str(binary), "--headless-frames", str(FRAMES),
        "--rom", str(rom), "--window-size", "320x240",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    process = subprocess.run(
        command, cwd=run_dir, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=timeout, check=False)
    output = process.stdout or ""
    if process.returncode != 0:
        raise RuntimeError(f"{label}: exit {process.returncode}\n"
                           f"{output[-3000:]}")
    fatal = find_fatal(output)
    if fatal:
        raise RuntimeError(f"{label}: emitted {fatal!r}")
    rows = [Row(m) for m in ROW_RE.finditer(output)]
    if len(rows) != FRAMES:
        raise RuntimeError(
            f"{label}: expected {FRAMES} [RNGSPLIT] rows, got {len(rows)}")
    return rows, output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom):
        if not path.is_file():
            return fail(f"missing {path}")

    with tempfile.TemporaryDirectory(prefix="mdkr-rngsplit-") as tmp:
        root = Path(tmp)
        base, _ = run_arm(binary, rom, root, "base", {},
                          args.timeout, args.verbose)
        control, _ = run_arm(binary, rom, root, "control",
                             {"MDKR_TEST_RENDER_IMPURITY": "1"},
                             args.timeout, args.verbose)
        rerun, _ = run_arm(binary, rom, root, "rerun", {},
                           args.timeout, args.verbose)

    window = [row for row in base if row.frame >= WINDOW_START]
    if len(window) < 2:
        return fail(f"window from frame {WINDOW_START} holds {len(window)} rows")

    # Arm A, half one: the authoritative pair is pinned.
    pinned_sim = window[0].sim
    pinned_prev = window[0].prev
    moved = [row.frame for row in window
             if row.sim != pinned_sim or row.prev != pinned_prev]
    if moved:
        return fail(
            f"authoritative RNG moved on {len(moved)} of {len(window)} "
            f"settled frames (first frame {moved[0]}); expected "
            f"sim={pinned_sim} prev={pinned_prev} throughout")

    # Arm A, half two: the presentation stream ran during those same frames.
    advance = window[-1].draws - window[0].draws
    if advance < MIN_DRAWS:
        return fail(
            f"presentation stream advanced {advance} draws across "
            f"{len(window)} pinned frames, expected at least {MIN_DRAWS}; "
            "the pin above is vacuous unless presentation randomness ran")
    for previous, row in zip(window, window[1:]):
        if row.draws < previous.draws:
            return fail(f"presentation draw counter fell at frame {row.frame}")
    distinct = len({row.pres for row in window})
    if distinct < MIN_DISTINCT:
        return fail(
            f"presentation seed took {distinct} distinct values over the "
            f"window, expected at least {MIN_DISTINCT}")

    # Arm A, half three: the trace can see authoritative movement at all.
    lead = [row for row in base if row.frame < WINDOW_START]
    if len({row.sim for row in lead}) < 2:
        return fail(
            "the authoritative seed never moved before the window, so the pin "
            "cannot distinguish a working split from a trace that prints a "
            "constant; the route no longer reaches a simulating frame")

    # Arm B: the positive control must break the pin, and only the pin.
    control_window = [row for row in control if row.frame >= WINDOW_START]
    if len({row.sim for row in control_window}) < 2:
        return fail(
            "MDKR_TEST_RENDER_IMPURITY=1 left the authoritative seed pinned "
            "across the window; the control cannot fail this gate, so a real "
            "presentation-to-simulation leak would pass unseen")
    control_draws = [row.draws for row in control_window]
    if control_draws != [row.draws for row in window]:
        return fail(
            "the impurity control changed the presentation draw counts; it is "
            "supposed to move only the authoritative half")

    # Arm C: the presentation stream is reproducible run to run.
    for first, second in zip(base, rerun):
        if (first.sim, first.prev, first.pres, first.draws) != \
                (second.sim, second.prev, second.pres, second.draws):
            return fail(
                f"rerun diverged at frame {first.frame}: "
                f"sim={first.sim}/{second.sim} pres={first.pres}/{second.pres}")

    print(
        "check_presentation_rng_split: PASS — authoritative sim/prev pinned at "
        f"{pinned_sim}/{pinned_prev} across {len(window)} settled "
        f"frames while the presentation stream drew {advance} times over "
        f"{distinct} distinct seeds; the render-impurity control breaks the pin "
        "without touching those draws; two runs are byte-identical")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(fail(str(error)))
