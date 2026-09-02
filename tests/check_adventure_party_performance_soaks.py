#!/usr/bin/env python3
"""AP-19 game-side Taj and host-solo ownership soaks.

The companion AP-19 transition gate owns long hub/race generations. This check
owns the two other literal resource criteria from the architecture:

* a four-player party performs five production Taj racer rebuilds in one shared
  scene. A one-rebuild control has the same timing and final vehicle; their
  normalized post-E12/E0 hub memory, renderer-live, and registry-live ownership
  must be identical, while every rebuild republishes exactly four racers,
  bindings, and viewports and remains below the formal display-list budget;
* a pre-cleared boss checkpoint performs five real party -> host-solo boss ->
  exact-party restore lifetimes in one process. Equivalent boss and return-lobby
  generations must plateau across main/audio, renderer, and registry ownership.

All runs use existing production routes and diagnostics. No allocation or
gameplay seam exists only for this check. ``--self-test`` is ROM/GPU-free.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

import check_adventure_party_boss_restore as boss
from check_adventure_party_admission import eeprom_image
from check_adventure_party_performance import (
    BAD_RE, BIND_RE, DL_RE, LAYOUT_RE, REGISTRY_RE, RENDERER_RE, RESOURCE_RE,
    binding_failure, display_list_failure, high_water_failures, load_budgets,
    parse_spans, plateau_exact, plateau_no_new_high,
)
from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env


ROOT = Path(__file__).resolve().parent.parent
TAJ_ONE = ROOT / "tests/input_scripts/adventure_party_4p_taj_performance_1x.txt"
TAJ_FIVE = ROOT / "tests/input_scripts/adventure_party_4p_taj_performance_5x.txt"
ADMIT_FOUR = ROOT / "tests/input_scripts/adventure_party_4p_admit.txt"

PLAYERS = 4
HUB = 0
LOBBY = 12
TRICKY = 38
HUB_ROUTE = "0:200,500:-1004,946:-1858,1099:-3381,1946:-3948,2180:E12"
TAJ_EXIT_ROUTE = (
    "1=-197,193;"
    "0=200,500|-1004,946|-1858,1099|-3381,1946|-3948,2180|E12;"
    "2=E0")

TRANSFORM_RE = re.compile(
    r"aparty_transform: path=(\w+) vehicle=(\d+) n=(\d+) live=(\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)")
SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
RESTORE_RE = re.compile(
    r"aparty_restore: suspend_lgen=(\d+) restore_lgen=(\d+) match=(-?\d+)")
BOSS_FINISH_RE = re.compile(
    r"bossfinish: finishPos=(-?\d+) playerIndex=(-?\d+) courseId=(\d+)")
BOSS_LEVEL_RE = re.compile(
    r"level_load: levelId=(-?\d+) numPlayers=(-?\d+) entrance=(-?\d+) "
    r"vehicle=(-?\d+) cutscene=(-?\d+) @frame~(\d+)")
RACEFIELD_RE = re.compile(r"racefield:")


def boss_loss_checkpoint_eeprom(rom: Path) -> bytes:
    """AP-17's legal unbeaten checkpoint for repeated equivalent defeats.

    A pre-cleared boss uses retail's rematch self-reload and never crosses the
    party restore adapter. Keeping Tricky unbeaten and forcing no verdict makes
    every natural second-place finish return to the lobby without a save write;
    the next ordered door therefore starts another equivalent suspension.
    """
    boss.SAVE_ORDER = boss.save_order(str(rom))
    return boss.eeprom_image()


def clean_environment() -> dict[str, str]:
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update({
        "LC_ALL": "C",
        "MDKR_AUDIO": "0",
        "MDKR_TRACE": "1",
        "MDKR_RESOURCE_STATS": "1",
    })
    return env


def run_arm(binary: Path, rom: Path, fixture: Path, frames: int,
            timeout: float, values: dict[str, str], save: bytes,
            artifacts_dir: Path | None, label: str,
            verbose: bool) -> tuple[str | None, str]:
    keep = artifacts_dir is not None
    if keep:
        artifacts_dir.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix=f"{label}-", dir=artifacts_dir))
    else:
        root = Path(tempfile.mkdtemp(prefix=f"mdkr_ap19_{label}_"))
    log_path = root / "run.log"
    try:
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(save)
        env = clean_environment()
        env.update(values)
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "video.ini")
        command = [
            str(binary), "--headless-frames", str(frames),
            "--input-script", str(fixture), "--rom", str(rom),
            "--window-size", "640x480",
            "--video-set", "Enhancements.AdventureParty=1",
        ]
        if verbose:
            print("$ " + " ".join(command), flush=True)
        try:
            with log_path.open("w", encoding="utf-8") as log:
                process = subprocess.run(
                    command, cwd=root, env=env, text=True, stdout=log,
                    stderr=subprocess.STDOUT, timeout=timeout, check=False)
        except subprocess.TimeoutExpired:
            output = log_path.read_text(encoding="utf-8", errors="replace")
            return f"{label}: timed out after {timeout:.0f}s; artifacts: {root}", output
        output = log_path.read_text(encoding="utf-8", errors="replace")
        if process.returncode != 0:
            return f"{label}: exited {process.returncode}; artifacts: {root}", output
        if keep:
            print(f"  retained {label} evidence: {root}")
        return None, output
    finally:
        if not keep:
            shutil.rmtree(root, ignore_errors=True)


def records(output: str, pattern: re.Pattern[str]) -> list[tuple[int, ...]]:
    return [tuple(map(int, match.groups())) for match in pattern.finditer(output)]


def memory_budget_failures(rows: list[tuple[int, ...]], budgets: dict[str, Any],
                           label: str) -> list[str]:
    failures: list[str] = []
    if not rows:
        return [f"{label}: no resource ownership rows"]
    memory = budgets["memory"]
    for row in rows:
        physical_alloc, physical_free, physical_lame = row[10:13]
        music_alloc, music_free = row[13:15]
        jingle_alloc, jingle_free = row[15:17]
        sfx_alloc, sfx_free = row[17:19]
        if (physical_alloc + physical_free + physical_lame != 40 or
                music_alloc + music_free != 26 or
                jingle_alloc + jingle_free != 16 or
                sfx_alloc + sfx_free != 32 or row[21] != 1):
            failures.append(f"{label}: incoherent audio ownership {row}")
            break
        if (row[5] < memory["minimum_main_free_bytes"] or
                row[6] < memory["minimum_main_largest_free_bytes"]):
            failures.append(f"{label}: main-pool reserve below budget {row}")
            break
    return failures


def renderer_budget_failures(rows: list[tuple[int, ...]], budgets: dict[str, Any],
                             label: str) -> list[str]:
    ceiling = budgets["renderer"]
    if not rows:
        return [f"{label}: no renderer ownership rows"]
    for row in rows:
        start, created, deleted, live, peak = row[3:8]
        if (deleted > start + created or start + created - deleted != live or
                peak < start or peak < live or
                live > ceiling["maximum_texture_live"] or
                peak > ceiling["maximum_texture_peak"]):
            return [f"{label}: incoherent or over-budget renderer ownership {row}"]
    return []


def registry_budget_failures(rows: list[tuple[int, ...]], budgets: dict[str, Any],
                             label: str) -> list[str]:
    ceiling = budgets["renderer"]
    if not rows:
        return [f"{label}: no pointer-registry ownership rows"]
    for row in rows:
        live, high, ambiguous, full_fails, probe = row[3:8]
        if (live > high or live > ceiling["maximum_registry_live"] or
                high > ceiling["maximum_registry_high"] or
                probe > ceiling["maximum_registry_probe"] or ambiguous or full_fails):
            return [f"{label}: incoherent or over-budget registry ownership {row}"]
    return []


def checked_dl_failure(output: str, budgets: dict[str, Any],
                       levels: set[int],
                       label: str) -> tuple[list[str], int, int]:
    """Both display-list verdicts: the host census, and what the game measured.

    The census counts the commands the dispatcher was handed on the checked
    spans. The witness is the arm's whole-process high-water in bytes against
    the row the buffer was allocated to -- the same quantity the fail-closed
    assertion in gfxtask_run_xbus acts on, so a soak that never emitted it
    proves nothing about the margin.
    """
    witness_failures, high_bytes = high_water_failures(
        output, budgets["display_list"])
    failures = [f"{label}: {error}" for error in witness_failures]
    spans = [span for span in parse_spans(output) if span.level in levels]
    if not spans or any(not span.dl_lengths for span in spans):
        failures.append(f"{label}: a checked span emitted no display lists")
        return failures, 0, high_bytes
    high = max(max(span.dl_lengths) for span in spans)
    if error := display_list_failure(high, budgets["display_list"]):
        failures.append(f"{label}: {error}")
    return failures, high, high_bytes


def taj_failures(output: str, expected: int, budgets: dict[str, Any],
                 label: str) -> tuple[list[str], dict[str, tuple[int, ...] | int]]:
    failures: list[str] = []
    summary: dict[str, tuple[int, ...] | int] = {}
    if match := BAD_RE.search(output):
        failures.append(f"{label}: runtime failure marker {match.group(0)!r}")
    lines = output.splitlines()
    transforms = [(index, match) for index, line in enumerate(lines)
                  if (match := TRANSFORM_RE.search(line))]
    if len(transforms) != expected:
        failures.append(f"{label}: {len(transforms)} transforms, expected {expected}")
    expected_vehicles = [1 if index % 2 == 0 else 0 for index in range(expected)]
    for number, (line_index, transform) in enumerate(transforms[:expected], 1):
        path, vehicle, count, live = transform.groups()
        if (path, int(vehicle), int(count), int(live)) != (
                "party", expected_vehicles[number - 1], PLAYERS, PLAYERS):
            failures.append(f"{label}: transform {number} invalid: {transform.group(0)}")
        stop = transforms[number][0] if number < len(transforms) else len(lines)
        next_level = next(
            (index for index in range(line_index + 1, stop)
             if "level_load:" in lines[index]), stop)
        stop = min(stop, next_level)
        segment = "\n".join(lines[line_index:stop])
        roster = ROSTER_RE.search(segment)
        if not roster or (int(roster.group(1)), int(roster.group(2), 16)) != (4, 0xF):
            failures.append(f"{label}: transform {number} did not republish roster 4/0xf")
        layouts = [(int(match.group(1)), int(match.group(2)))
                   for match in LAYOUT_RE.finditer(segment)]
        if (4, 3) not in layouts:
            failures.append(f"{label}: transform {number} did not republish 4-view layout")
        bindings = [(int(match.group(1)), int(match.group(2)))
                    for match in BIND_RE.finditer(segment)]
        if error := binding_failure(bindings, PLAYERS, f"{label} transform {number}"):
            failures.append(error)

    sessions = [(match.group(1), *(int(value) for value in match.groups()[1:]))
                for match in SESSION_RE.finditer(output)]
    dialogue_index = next((index for index, row in enumerate(sessions)
                           if row[0] == "SHARED_DIALOGUE"), -1)
    if dialogue_index < 0:
        failures.append(f"{label}: no SHARED_DIALOGUE lifecycle")
    elif not any(row[0] == "ACTIVE_LOBBY"
                 for row in sessions[dialogue_index + 1:]):
        failures.append(f"{label}: shared dialogue never released")
    if {row[1] for row in sessions} != {1} or any(row[3] != 0 for row in sessions):
        failures.append(f"{label}: session generation/host changed")

    resources = records(output, RESOURCE_RE)
    renderers = records(output, RENDERER_RE)
    registries = records(output, REGISTRY_RE)
    failures += memory_budget_failures(resources, budgets, label)
    failures += renderer_budget_failures(renderers, budgets, label)
    failures += registry_budget_failures(registries, budgets, label)
    # E12 closes the transformed hub, then a real E0 return gives queued
    # renderer/registry retirement one additional generation to settle. The
    # exact one-vs-five comparison is made only at that normalized central-hub
    # endpoint, never at the known one-generation transfer boundary.
    post_resources = [row for row in resources if row[0:3] == (HUB, 0, 0)][1:]
    closed_hub = [row for row in renderers if row[0:3] == (LOBBY, 0, 0)]
    post_registry = [row for row in registries if row[0:3] == (HUB, 0, 0)][1:]
    if not post_resources or not closed_hub or not post_registry:
        failures.append(f"{label}: did not close Taj through Dino and return central")
    else:
        summary["resource"] = post_resources[-1][3:10]
        summary["renderer_live"] = closed_hub[-1][6]
        summary["registry_live"] = post_registry[-1][3]
    dl_failures, dl_max, dl_bytes = checked_dl_failure(
        output, budgets, {HUB, LOBBY}, label)
    failures += dl_failures
    summary["dl_max"] = dl_max
    summary["dl_high_water_bytes"] = dl_bytes
    return failures, summary


def compare_ownership(one: dict[str, tuple[int, ...] | int],
                      five: dict[str, tuple[int, ...] | int], label: str,
                      allow_renderer_settling: bool = False) -> list[str]:
    failures: list[str] = []
    for key in ("resource", "renderer_live", "registry_live"):
        if key not in one or key not in five:
            continue
        if (allow_renderer_settling and key in {"renderer_live", "registry_live"}
                and isinstance(one[key], int) and isinstance(five[key], int)
                and five[key] <= one[key]):
            continue
        if one[key] != five[key]:
            failures.append(f"{label}: repeated final {key}={five[key]}, "
                            f"one-lifetime baseline={one[key]}")
    return failures


def restore_sequence_failures(restores: list[tuple[int, int, int]],
                              expected: int, label: str) -> list[str]:
    failures: list[str] = []
    if len(restores) != expected:
        failures.append(f"{label}: {len(restores)} restores, expected {expected}")
    for number, (suspend, restore, match) in enumerate(restores, 1):
        if match != 1 or restore != suspend + 1:
            failures.append(f"{label}: restore {number} was not exact +1/match=1: "
                            f"{(suspend, restore, match)}")
    return failures


def boss_failures(output: str, expected: int, budgets: dict[str, Any],
                  label: str) -> tuple[list[str], dict[str, tuple[int, ...] | int]]:
    failures: list[str] = []
    summary: dict[str, tuple[int, ...] | int] = {}
    if match := BAD_RE.search(output):
        failures.append(f"{label}: runtime failure marker {match.group(0)!r}")
    sessions = [(match.group(1), *(int(value) for value in match.groups()[1:]))
                for match in SESSION_RE.finditer(output)]
    solo = [row for row in sessions if row[0] == "SOLO_ACTIVITY"]
    # The terminal extra suspension is deliberate: beginning generation N+1
    # closes and publishes renderer/registry ownership for completed restore N.
    if len(solo) != expected + 1:
        failures.append(f"{label}: {len(solo)} SOLO_ACTIVITY entries, expected "
                        f"{expected + 1} including the terminal flush generation")
    if {row[1] for row in sessions} != {1} or any(row[3] != 0 for row in sessions):
        failures.append(f"{label}: session generation/host changed")
    restores = [tuple(map(int, match.groups())) for match in RESTORE_RE.finditer(output)]
    failures += restore_sequence_failures(restores, expected, label)
    finishes = [tuple(map(int, match.groups()))
                for match in BOSS_FINISH_RE.finditer(output)]
    if len(finishes) != expected or any(row[1:] != (0, TRICKY) for row in finishes):
        failures.append(f"{label}: host-solo boss finishes invalid: {finishes}")
    if RACEFIELD_RE.search(output):
        failures.append(f"{label}: party race field spawned inside boss envelope")

    spans = parse_spans(output)
    boss_spans = [span for span in spans if span.level == TRICKY]
    # A boss defeat returns through the retail level-57 cutscene/arrival seam;
    # the adapter restores the party there before the next retargeted entry.
    restore_loads = [tuple(map(int, match.groups()))
                     for match in BOSS_LEVEL_RE.finditer(output)
                     if (int(match.group(1)), int(match.group(2)),
                         int(match.group(5))) == (boss.TRICKY_CUTSCENE, 0, 5)]
    return_spans = [span for span in spans
                    if span.level == boss.TRICKY_CUTSCENE and span.players_arg == 0]
    return_spans = return_spans[-expected:]
    if (len(boss_spans) < expected or len(restore_loads) != expected or
            len(return_spans) != expected):
        failures.append(f"{label}: level spans boss={len(boss_spans)} "
                        f"restore-scenes={len(restore_loads)}, expected at "
                        f"least/exactly {expected}")
    for number, span in enumerate(return_spans[:expected], 1):
        if error := binding_failure(span.bindings, PLAYERS,
                                    f"{label} return {number}"):
            failures.append(error)
        if (PLAYERS, PLAYERS - 1) not in span.layouts:
            failures.append(f"{label}: return {number} did not restore four views")

    resources = records(output, RESOURCE_RE)
    renderers = records(output, RENDERER_RE)
    registries = records(output, REGISTRY_RE)
    failures += memory_budget_failures(resources, budgets, label)
    failures += renderer_budget_failures(renderers, budgets, label)
    failures += registry_budget_failures(registries, budgets, label)
    boss_resources = [row for row in resources if row[0] == TRICKY]
    lobby_resources = [row for row in resources
                       if row[0:3] == (boss.TRICKY_CUTSCENE, 0, 5)]
    warm = expected
    failures += plateau_exact(
        boss_resources, warm, lambda row: row[3:10], f"{label} boss resources")
    failures += plateau_exact(
        lobby_resources, warm, lambda row: row[3:10], f"{label} restored resources")
    for level, cutscene, kind in (
            (TRICKY, 0, "boss"),
            (boss.TRICKY_CUTSCENE, 5, "restored")):
        renderer_rows = [row for row in renderers if row[0] == level and row[2] == cutscene]
        registry_rows = [row for row in registries if row[0] == level and row[2] == cutscene]
        failures += plateau_no_new_high(
            renderer_rows, warm, lambda row: row[6:9],
            f"{label} {kind} renderer")
        failures += plateau_no_new_high(
            registry_rows, warm, lambda row: row[3:5],
            f"{label} {kind} registry")
    if lobby_resources:
        summary["resource"] = lobby_resources[-1][3:10]
    lobby_renderers = [row for row in renderers
                       if row[0:3] == (boss.TRICKY_CUTSCENE, 0, 5)]
    lobby_registries = [row for row in registries
                        if row[0:3] == (boss.TRICKY_CUTSCENE, 0, 5)]
    if lobby_renderers:
        summary["renderer_live"] = lobby_renderers[-1][6]
    if lobby_registries:
        summary["registry_live"] = lobby_registries[-1][3]
    dl_failures, dl_max, dl_bytes = checked_dl_failure(
        output, budgets, {TRICKY, boss.TRICKY_CUTSCENE}, label)
    failures += dl_failures
    summary["dl_max"] = dl_max
    summary["dl_high_water_bytes"] = dl_bytes
    return failures, summary


def self_test(budgets: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    taj_transforms = int(budgets["qualification"]["taj_transforms"])
    transform_lines = []
    for vehicle in (1, 0, 1, 0, 1):
        transform_lines += [
            f"aparty_transform: path=party vehicle={vehicle} n=4 live=4",
            "aparty_roster: n=4 mask=0xf c0=1 c1=2 c2=3 c3=4",
            "aparty_layout: viewports=4 layout=3",
            "aparty_binding: seat=0 port=0",
            "aparty_binding: seat=1 port=1",
            "aparty_binding: seat=2 port=2",
            "aparty_binding: seat=3 port=3",
        ]
    mutated = "\n".join(transform_lines).replace("live=4", "live=5", 1)
    taj_errors, _ = taj_failures(mutated, taj_transforms, budgets, "control")
    if not any("transform 1 invalid" in error for error in taj_errors):
        failures.append("self-test: retained fifth racer passed Taj ownership oracle")

    exact = {"resource": (600, 1000), "renderer_live": 6, "registry_live": 45}
    grown = dict(exact)
    grown["resource"] = (601, 1032)
    if not compare_ownership(exact, grown, "control"):
        failures.append("self-test: five-transform retained-memory control passed")

    synthetic = [(TRICKY, -1, 0, 10, 100), (TRICKY, -1, 0, 10, 100),
                 (TRICKY, -1, 0, 10, 100), (TRICKY, -1, 0, 11, 101),
                 (TRICKY, -1, 0, 12, 102)]
    if not plateau_exact(synthetic, 5, lambda row: row[3:5], "control boss"):
        failures.append("self-test: boss ownership-growth control passed")
    if binding_failure([(0, 0), (1, 1), (2, 2), (3, 2)], 4,
                       "control restore") is None:
        failures.append("self-test: swapped restored controller passed")
    if not restore_sequence_failures([(2, 4, 1)], 1, "control restore"):
        failures.append("self-test: skipped restore generation passed")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=float, default=7200.0)
    parser.add_argument("--artifacts-dir", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    try:
        budgets = load_budgets()
    except (OSError, ValueError) as error:
        print(f"check_adventure_party_performance_soaks: FAIL -- budgets: {error}",
              file=sys.stderr)
        return 1
    failures = self_test(budgets)
    if args.self_test:
        if failures:
            for failure in failures:
                print("  - " + failure, file=sys.stderr)
            return 1
        print("check_adventure_party_performance_soaks: PASS -- Taj live-racer, "
              "retained-memory, boss-growth, and restored-controller controls fired")
        return 0

    required = (TAJ_ONE, TAJ_FIVE, ADMIT_FOUR)
    missing = [str(path) for path in required if not path.is_file()]
    try:
        binary = Path(resolve_binary(args.build)).resolve()
    except (OSError, ValueError) as error:
        print(f"check_adventure_party_performance_soaks: FAIL -- binary: {error}",
              file=sys.stderr)
        return 1
    rom = Path(args.rom).expanduser().resolve()
    missing += [str(path) for path in (binary, rom) if not path.is_file()]
    if missing:
        print("check_adventure_party_performance_soaks: FAIL -- missing " +
              ", ".join(missing), file=sys.stderr)
        return 1

    taj_transforms = int(budgets["qualification"]["taj_transforms"])
    boss_suspensions = int(budgets["qualification"]["boss_suspensions"])
    taj_values = {"MDKR_AP_SEAT_ROUTE": TAJ_EXIT_ROUTE}
    error, taj_one = run_arm(
        binary, rom, TAJ_ONE, 13000, args.timeout, taj_values, eeprom_image(),
        args.artifacts_dir, "taj-one", args.verbose)
    if error:
        failures.append(error)
    error, taj_five = run_arm(
        binary, rom, TAJ_FIVE, 13000, args.timeout, taj_values, eeprom_image(),
        args.artifacts_dir, "taj-five", args.verbose)
    if error:
        failures.append(error)
    one_failures, one_summary = taj_failures(taj_one, 1, budgets, "Taj baseline")
    five_failures, five_summary = taj_failures(
        taj_five, taj_transforms, budgets, "Taj soak")
    failures += one_failures + five_failures
    failures += compare_ownership(one_summary, five_summary, "Taj soak")

    save = boss_loss_checkpoint_eeprom(rom)
    common_boss = {
        "MDKR_AUTOPILOT": "1",
        "MDKR_AUTOPILOT_UNSTICK": "1",
        "MDKR_SIMULATION_CADENCE": "original",
        "MDKR_SYNTH_FIELDS": "2",
        "MDKR_WATCH_COURSEFLAGS": str(TRICKY),
        "MDKR_LOAD_TRACK": str(TRICKY),
        "MDKR_BOSS_ROUTE": "1",
    }
    baseline_values = dict(common_boss)
    baseline_values["MDKR_DRIVE_ROUTE"] = HUB_ROUTE
    soak_values = dict(common_boss)
    soak_values["MDKR_DRIVE_ROUTE"] = HUB_ROUTE
    error, boss_one = run_arm(
        binary, rom, ADMIT_FOUR, 6200, args.timeout, baseline_values, save,
        args.artifacts_dir, "boss-one", args.verbose)
    if error:
        failures.append(error)
    error, boss_five = run_arm(
        binary, rom, ADMIT_FOUR, 19500, args.timeout, soak_values, save,
        args.artifacts_dir, "boss-five", args.verbose)
    if error:
        failures.append(error)
    boss_one_failures, boss_one_summary = boss_failures(
        boss_one, 1, budgets, "boss baseline")
    boss_five_failures, boss_five_summary = boss_failures(
        boss_five, boss_suspensions, budgets, "boss soak")
    failures += boss_one_failures + boss_five_failures
    failures += compare_ownership(
        boss_one_summary, boss_five_summary, "boss soak",
        allow_renderer_settling=True)

    if failures:
        print("check_adventure_party_performance_soaks: FAIL", file=sys.stderr)
        for failure in failures:
            print("  - " + failure.replace("\n", "\n    "), file=sys.stderr)
        return 1
    print(
        "check_adventure_party_performance_soaks: PASS -- five in-scene 4P Taj "
        f"rebuilds retained baseline ownership (DL {five_summary.get('dl_max', 0)}/"
        f"{budgets['display_list']['qualification_max_commands']} commands, "
        f"{five_summary.get('dl_high_water_bytes', 0)}/"
        f"{budgets['display_list']['maximum_high_water_bytes']} bytes measured "
        "in-engine); five real "
        "host-solo boss suspensions/restores plateaued main/audio/renderer/registry "
        f"ownership (DL {boss_five_summary.get('dl_max', 0)}/"
        f"{budgets['display_list']['qualification_max_commands']}); exact four-seat "
        "bindings/session generations held; mutation controls fired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
