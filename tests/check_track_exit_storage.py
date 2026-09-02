#!/usr/bin/env python3
"""Issue #55: the Hot Top Volcano destination -1 exit must reload the hub.

Hot Top Volcano carries exactly one BHV_EXIT and its destinationMapId byte is
-1 (a 20-track MDKR_OBJDUMP sweep found no other race-track exit at all).  A
human can reach it only through an out-of-bounds clip -- the trigger sits ~114
units below the drivable surface -- and the community uses it as the "trophy
storage" glitch.  Once latched, the production chain is authored code all the
way down: obj_loop_exit() -> racer_enter_door() -> func_8006D968() ->
update_game() routes destination -1 to MENU_UNUSED_8, a menu id with no init
and no loop case, and retail menu_loop() returns an UNINITIALIZED value there.

Measured on retail us.v80 in the instrumented ares oracle (RDRAM poke lane,
docs/ORACLE.md): that uninitialized return is v0 = 0x8006CA60, every time.
mode_menu() consumes it through the MENU_RESULT_FLAGS_200 arm (0x8006CA60 &
0x200 != 0), so gPlayableMapId becomes 0x8006CA60 & 0x7F = 96 and
load_level_game(96, ...) runs; level_load()'s own "LOADLEVEL Error: Level out
of range" guard then substitutes ASSET_LEVEL_CENTRALAREAHUB.  Net retail
behavior: one frame in menu 8, then the central hub loads in-game and the
trophy globals survive untouched -- the glitch's whole value.  An AVOID_UB
build that returns MENU_RESULT_CONTINUE instead sits in menu 8 forever: the
1.5.1 indefinite black screen.

This check drives that path synthetically and asserts the measured retail
outcome end to end:

* the MDKR_FORCE_EXIT_LATCH seam performs obj_loop_exit()'s two writes inside
  Hot Top Volcano (non-vacuity: the latch row must name level 7, dest 255);
* menu 8 is still entered (the fix pins the dead end's RESULT, not its
  existence -- retail passes through the menu too);
* the measured consumption follows: `level_load: levelId=96` (the FLAGS_200
  arm's 0x8006CA60 & 0x7F), which the production out-of-range guard turns
  into the central hub;
* the hub is then actually simulating: the seam's post row reports level 0
  with the trophy globals it saw at the latch, unchanged.

Two arms:

* "tracks" -- the light MDKR_LOAD_TRACK=7 lane; proves the mode-independent
  routing (the hang never depended on trophy state);
* "trophy" -- the check_trophy_series fixture driven to round 4 (track 7 is
  the Dino Domain closer), latched mid-round with gTrophyRaceWorldId=1 live;
  proves the community's storage survives the reload, i.e. the glitch still
  works.

Pre-fix both arms fail the same way: `menu_init: menuId=8` and then nothing,
ever -- the dead-end stall this check exists to keep out.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ROOT = Path(__file__).resolve().parent.parent

TRACKS_SCRIPT = "tests/input_scripts/nav_to_time_trial_race.txt"  # 8-racer Tracks
TRACKS_FRAMES = 6000
TROPHY_FRAMES = 13000

# 600 hook ticks inside Hot Top Volcano: comfortably past the countdown in
# both arms, well before either race can finish.
FORCE_LATCH = "255:600"

LATCH_RE = re.compile(
    r"force_exit_latch: level=(\d+) dest=255 exit=\(([-0-9.]+), ([-0-9.]+), "
    r"([-0-9.]+)\) trophyWorld=(\d+) trophyRound=(\d+) @frame~(\d+)")
POST_RE = re.compile(
    r"force_exit_latch: post level=(\d+) trophyWorld=(\d+) trophyRound=(\d+) "
    r"@frame~(\d+)")
MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
LOAD_RE = re.compile(r"level_load: levelId=(\d+) .*@frame~(\d+)")
ROUND4_RE = re.compile(r"trophyround: world=1 round=3 track=7")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")

# The measured retail consumption: 0x8006CA60 & 0x7F.  level_load()'s
# out-of-range guard is what turns it into the central hub (level 0).
GARBAGE_LEVEL_ID = 96
HUB_LEVEL_ID = 0
HTV_LEVEL_ID = 7


def trophy_module():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "check_trophy_series.py")
    spec = importlib.util.spec_from_file_location("mdkr_check_trophy_series",
                                                  path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def clean_env() -> dict[str, str]:
    return {key: value for key, value in os.environ.items()
            if not key.startswith("MDKR_")}


def launch(binary: str, rom: str, frames: int, script: Path, run_dir: Path,
           extra_env: dict[str, str], timeout: int,
           verbose: bool) -> tuple[str, int | str]:
    env = clean_env()
    env.update(
        MDKR_AUDIO="0",
        MDKR_SIMULATION_CADENCE="enhanced",
        MDKR_SYNTH_FIELDS="1",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_FORCE_EXIT_LATCH=FORCE_LATCH,
        MDKR_SAVE_DIR=str(run_dir / "save"),
        MDKR_VIDEO_CONFIG_PATH=str(run_dir / "save" / "video.ini"),
    )
    env.update(extra_env)
    command = [binary, "--headless-frames", str(frames),
               "--input-script", str(script), "--rom", os.path.abspath(rom)]
    if verbose:
        print("$ " + " ".join(command))
    try:
        proc = subprocess.run(command, cwd=run_dir, env=env,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout)
        return proc.stdout.decode("utf-8", "replace"), proc.returncode
    except subprocess.TimeoutExpired as exc:
        return (exc.stdout or b"").decode("utf-8", "replace"), "timeout"


def run_tracks_arm(binary: str, rom: str, verbose: bool) -> tuple[str, int | str]:
    with tempfile.TemporaryDirectory(prefix="mdkr_exit55_tracks_") as temp:
        run_dir = Path(temp)
        (run_dir / "save").mkdir()
        return launch(binary, rom, TRACKS_FRAMES, ROOT / TRACKS_SCRIPT,
                      run_dir, {"MDKR_LOAD_TRACK": str(HTV_LEVEL_ID)},
                      timeout=420, verbose=verbose)


def run_trophy_arm(binary: str, rom: str, verbose: bool) -> tuple[str, int | str]:
    cts = trophy_module()
    with tempfile.TemporaryDirectory(prefix="mdkr_exit55_trophy_") as temp:
        run_dir = Path(temp)
        (run_dir / "save").mkdir()
        (run_dir / "save" / "eeprom.bin").write_bytes(cts.eeprom_image())
        script = run_dir / "trophy_input.txt"
        cts.write_input_script(script, TROPHY_FRAMES, "retry")
        return launch(binary, rom, TROPHY_FRAMES, script, run_dir,
                      {
                          "MDKR_DRIVE_ROUTE": cts.ROUTE,
                          # 1200, not the fixture's usual 600: the forced
                          # latch fires at 600 ticks into round 4, and the
                          # completion seam must never race it -- rounds 1-3
                          # simply take ~600 extra ticks each.
                          "MDKR_TROPHY_COMPLETE_AFTER": "1200",
                          "MDKR_TROPHY_COLLIDE": "1",
                      },
                      timeout=900, verbose=verbose)


def stall_diagnosis(output: str) -> str:
    """Describe the pre-fix failure shape so a RED run reads as the defect."""
    menus = list(MENU_RE.finditer(output))
    if not menus:
        return "menu 8 was never entered"
    last = menus[-1]
    if last.group(1) != "8":
        return f"last menu was id {last.group(1)}"
    tail = output[last.end():]
    if MENU_RE.search(tail) is None and LOAD_RE.search(tail) is None:
        return (f"dead-end stall: menu_init menuId=8 @frame~{last.group(2)} "
                "and no level or menu activity ever again (the 1.5.1 "
                "indefinite black screen)")
    return "menu 8 entered but later activity exists"


def validate_arm(label: str, output: str, rc: int | str,
                 expect_trophy: bool) -> list[str]:
    failures: list[str] = []
    if rc != 0:
        failures.append(f"{label}: exit={rc}")
    bad = BAD_RE.search(output)
    if bad:
        line = next((l for l in output.splitlines() if bad.group(0) in l),
                    bad.group(0))
        failures.append(f"{label}: {line.strip()}")

    if expect_trophy and ROUND4_RE.search(output) is None:
        failures.append(
            f"{label}: never reached trophy round 4 on track 7; the fixture "
            "drifted and the storage assertion below is inert")

    latch = LATCH_RE.search(output)
    if latch is None:
        failures.append(
            f"{label}: no force_exit_latch row -- the seam never latched, so "
            "nothing below was exercised")
        return failures
    if int(latch.group(1)) != HTV_LEVEL_ID:
        failures.append(
            f"{label}: latched in level {latch.group(1)}, expected "
            f"{HTV_LEVEL_ID} (Hot Top Volcano)")
    latch_world = int(latch.group(5))
    latch_round = int(latch.group(6))
    if expect_trophy and latch_world != 1:
        failures.append(
            f"{label}: latched with trophyWorld={latch_world}, expected 1 -- "
            "the latch fired outside the live trophy series")

    after_latch = output[latch.end():]

    # Retail passes THROUGH menu 8; the fix pins its result, it does not skip
    # the menu.  A run that never shows menuId=8 is not on the measured path.
    menu8 = re.search(r"menu_init: menuId=8 @frame~(\d+)", after_latch)
    if menu8 is None:
        failures.append(
            f"{label}: the destination -1 routing never entered menu 8; "
            "update_game()'s dead-end arm did not run")
        return failures

    # The measured consumption: retail menu_loop returns 0x8006CA60 for menu
    # 8, mode_menu's FLAGS_200 arm loads level 0x8006CA60 & 0x7F == 96, and
    # level_load()'s own out-of-range guard substitutes the central hub.
    after_menu8 = after_latch[menu8.end():]
    load = LOAD_RE.search(after_menu8)
    if load is None or int(load.group(1)) != GARBAGE_LEVEL_ID:
        got = f"levelId={load.group(1)}" if load else "no level_load at all"
        failures.append(
            f"{label}: after menu 8 expected the measured retail reload "
            f"(level_load levelId={GARBAGE_LEVEL_ID}), got {got} -- "
            + stall_diagnosis(output))
        return failures

    post = POST_RE.search(after_menu8)
    if post is None:
        failures.append(
            f"{label}: the hub never simulated a frame after the reload "
            "(no force_exit_latch post row) -- " + stall_diagnosis(output))
        return failures
    if int(post.group(1)) != HUB_LEVEL_ID:
        failures.append(
            f"{label}: reload landed in level {post.group(1)}, expected the "
            f"central hub ({HUB_LEVEL_ID})")
    if int(post.group(2)) != latch_world or int(post.group(3)) != latch_round:
        failures.append(
            f"{label}: trophy globals changed across the reload: "
            f"world {latch_world}->{post.group(2)}, "
            f"round {latch_round}->{post.group(3)} -- trophy storage broken")
    if not failures:
        print(f"  {label}: latch@7 trophyWorld={latch_world} -> menu 8 -> "
              f"level_load {GARBAGE_LEVEL_ID} -> hub post row "
              f"trophyWorld={post.group(2)} trophyRound={post.group(3)}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--skip-trophy", action="store_true",
                        help="run only the light tracks-mode arm")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = os.path.abspath(resolve_binary(args.build))
    for path in (binary, args.rom, str(ROOT / TRACKS_SCRIPT)):
        if not os.path.exists(path):
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    failures: list[str] = []
    output, rc = run_tracks_arm(binary, args.rom, args.verbose)
    failures.extend(validate_arm("tracks", output, rc, expect_trophy=False))

    if not args.skip_trophy:
        output, rc = run_trophy_arm(binary, args.rom, args.verbose)
        failures.extend(validate_arm("trophy", output, rc, expect_trophy=True))

    if failures:
        print(f"FAIL: track exit storage check ({len(failures)} issue(s))")
        for failure in failures:
            print("  - " + failure)
        return 1
    print("PASS: destination -1 exit reloads the central hub through the "
          "measured retail path and trophy storage survives")
    return 0


if __name__ == "__main__":
    sys.exit(main())
