#!/usr/bin/env python3
"""AP-19 Adventure Party resource, plateau, and lifecycle qualification.

The release arm runs one four-player process through twenty genuine world-lobby
to Ancient Lake to world-lobby cycles. Existing engine traces are treated as
accounting records, not liveness hints: each cycle must publish four cameras,
four identity controller bindings, one new race generation and one return
generation; warmed main-pool, audio, texture, shader, and pointer-registry
ownership must plateau; every display list must remain below the checked-in
four-player qualification budget and the retail heap capacity.

A ROM-free companion compiles against the production session reducer and drives
20,000 complete FORM -> activity -> QUIT -> DESTROY lifetimes, including 5,000
host-solo suspend/restores. It proves generation counters remain monotonic and
no roster, latch, token, or suspended facts survive dissolution.

``--self-test`` runs the parser/checker mutation controls only. A sub-20 cycle
``--development-cycles`` run is intentionally labelled non-qualifying and exists
solely for bounded route development; the manifest never passes it.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from check_adventure_party_admission import eeprom_image
from check_adventure_race_loop import decode_progress
from harness_utils import (DEFAULT_BUILD_DIR, SLOT_BYTES, resolve_binary,
                           save_env, seal_slot)


ROOT = Path(__file__).resolve().parent.parent
BUDGET_PATH = ROOT / "tests/data/adventure_party_performance_budgets.json"
INPUT_PATH = ROOT / "tests/input_scripts/adventure_party_4p_performance.txt"
CHURN_PATH = ROOT / "tests/data/adventure_party_performance_churn.c"
STATE_SOURCE = ROOT / "platform/adventure_party/adventure_party_state.c"

HUB_LEVEL = 12
RACE_LEVEL = 5
CENTRAL_HUB_LEVEL = 0

LEVEL_RE = re.compile(
    r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")
SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
BIND_RE = re.compile(r"aparty_binding: seat=(\d+) port=(\d+)")
LAYOUT_RE = re.compile(r"aparty_layout: viewports=(\d+) layout=(\d+)")
HUD_RE = re.compile(r"hud_init: hudPlayers=(\d+) numViewports=(\d+)")
DL_RE = re.compile(r"gfxtask: type=\d+ dl=\S+ len=(\d+)")
# The engine's own measurement of the same lists, in bytes against the row the
# display-list buffer was allocated to (game/src/rcp_dkr.c). It is emitted only
# when the high-water rises, so the last line of a run is that run's peak.
HIGH_WATER_RE = re.compile(r"dl_high_water: bytes=(\d+) limit=(\d+)")
RESOURCE_RE = re.compile(
    r"resource_state: level=(-?\d+) players=(-?\d+) cutscene=(-?\d+) "
    r"mainLive=(\d+) mainUsed=(\d+) mainFree=(\d+) mainLargest=(\d+) "
    r"audioUsed=(\d+) audioCapacity=(\d+) audioAllocs=(\d+) "
    r"voicePhys=(\d+)/(\d+)/(\d+) voiceMusic=(\d+)/(\d+) "
    r"voiceJingle=(\d+)/(\d+) sfxStates=(\d+)/(\d+) "
    r"voicePeak=(\d+)/(\d+) voiceValid=(\d+)")
RENDERER_RE = re.compile(
    r"renderer_generation: level=(-?\d+) players=(-?\d+) cutscene=(-?\d+) "
    r"texStart=(\d+) texCreated=(\d+) texDeleted=(\d+) texLive=(\d+) "
    r"texPeak=(\d+) shaderCreates=(\d+)")
REGISTRY_RE = re.compile(
    r"registry_state: level=(-?\d+) players=(-?\d+) cutscene=(-?\d+) "
    r"live=(\d+) high=(\d+) ambiguous=(\d+) fullFails=(\d+) maxProbe=(\d+)")
CHURN_RE = re.compile(
    r"aparty_perf_churn: cycles=(\d+) sgen=(\d+) lgen=(\d+) "
    r"bytes=(\d+) failures=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|\[DL\].*(?:fault|invalid|abort)|"
    r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|"
    r"Assertion failed|allocation FAILED")


@dataclass
class LevelSpan:
    level: int
    players_arg: int
    frame: int
    bindings: list[tuple[int, int]] = field(default_factory=list)
    layouts: list[tuple[int, int]] = field(default_factory=list)
    huds: list[tuple[int, int]] = field(default_factory=list)
    dl_lengths: list[int] = field(default_factory=list)


def load_budgets() -> dict[str, Any]:
    with BUDGET_PATH.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if value.get("schema_version") != 1:
        raise ValueError("unsupported Adventure Party performance budget schema")
    q = value.get("qualification", {})
    if (q.get("party_players") != 4 or q.get("race_cycles") != 20 or
            q.get("taj_transforms") != 5 or q.get("boss_suspensions") != 5):
        raise ValueError(
            "qualification must remain four-player, twenty-cycle, five-Taj, "
            "and five-boss-suspension")
    dl = value.get("display_list", {})
    if (dl.get("qualification_max_commands", 0) +
            dl.get("minimum_headroom_commands", 0) !=
            dl.get("capacity_commands", -1)):
        raise ValueError("display-list qualification budget/headroom is incoherent")
    # The engine reports bytes, the census counts commands. One Gfx is eight
    # bytes, so the byte budget is the command budget restated -- pinned here
    # rather than left as a second, driftable number.
    if dl.get("maximum_high_water_bytes") != dl["qualification_max_commands"] * 8:
        raise ValueError(
            "display-list byte budget is not the command budget in bytes")
    return value


def parse_spans(output: str) -> list[LevelSpan]:
    spans: list[LevelSpan] = []
    current: LevelSpan | None = None
    for line in output.splitlines():
        if match := LEVEL_RE.search(line):
            current = LevelSpan(*(int(value) for value in match.groups()))
            spans.append(current)
            continue
        if current is None:
            continue
        if match := BIND_RE.search(line):
            current.bindings.append((int(match.group(1)), int(match.group(2))))
        if match := LAYOUT_RE.search(line):
            current.layouts.append((int(match.group(1)), int(match.group(2))))
        if match := HUD_RE.search(line):
            current.huds.append((int(match.group(1)), int(match.group(2))))
        if match := DL_RE.search(line):
            current.dl_lengths.append(int(match.group(1)))
    return spans


def cycle_spans(spans: list[LevelSpan]) -> list[tuple[LevelSpan, LevelSpan]]:
    cycles: list[tuple[LevelSpan, LevelSpan]] = []
    for index, span in enumerate(spans):
        # A first-clear balloon/cinematic can reload the course as a one-player
        # cutscene (players_arg=-1). It is not another party race cycle.
        if span.level != RACE_LEVEL or span.players_arg != 0:
            continue
        for later in spans[index + 1:]:
            if later.level == RACE_LEVEL and later.players_arg == 0:
                break
            if later.level == HUB_LEVEL and later.players_arg == 0:
                cycles.append((span, later))
                break
    return cycles


def plateau_exact(rows: list[tuple[int, ...]], warmed: int,
                  projection, label: str) -> list[str]:
    if len(rows) < warmed:
        return [f"{label}: only {len(rows)} generations, need {warmed}"]
    tail = [projection(row) for row in rows[-warmed:]]
    if len(set(tail)) != 1:
        return [f"{label}: warmed ownership did not plateau: {tail}"]
    return []


def plateau_no_new_high(rows: list[tuple[int, ...]], warmed: int,
                        projection, label: str) -> list[str]:
    """Reject a terminal new maximum or a spike-masked rising suffix.

    Renderer/registry live counts may oscillate by a few handles because queued
    globals retire one generation later. Equality would call that ownership
    transfer a leak. Comparing only terminal maxima is also too weak: an early
    transient high could mask a later 20,21,22,23 staircase. The terminal pair
    therefore may not exceed the earlier maximum, and a non-decreasing suffix
    spanning at least three generations may not finish above where it began.
    Stable/alternating ownership and downward settling pass; sustained growth
    fails. Every count remains subject to explicit hard ceilings separately.
    """
    if len(rows) < warmed:
        return [f"{label}: only {len(rows)} generations, need {warmed}"]
    tail = [projection(row) for row in rows[-warmed:]]
    if warmed == 1:
        return []
    width = len(tail[0])
    split = max(1, warmed - 2)
    for column in range(width):
        values = [value[column] for value in tail]
        if max(values[split:]) > max(values[:split]):
            return [f"{label}: terminal generations established a new ownership "
                    f"high in counter {column}: {tail}"]
        suffix_start = len(values) - 1
        while (suffix_start > 0 and
               values[suffix_start - 1] <= values[suffix_start]):
            suffix_start -= 1
        strict_rises = sum(
            values[index] > values[index - 1]
            for index in range(suffix_start + 1, len(values)))
        if (len(values) - suffix_start >= 3 and strict_rises >= 2):
            return [f"{label}: terminal generations retained a rising ownership "
                    f"suffix in counter {column}: {tail}"]
    return []


def binding_failure(bindings: list[tuple[int, int]], players: int,
                    label: str) -> str | None:
    expected = [(seat, seat) for seat in range(players)]
    if bindings != expected:
        return (f"{label}: bindings {bindings}, expected exactly "
                f"{expected}")
    return None


def display_list_failure(high_water: int,
                         budget: dict[str, int]) -> str | None:
    if high_water > budget["qualification_max_commands"]:
        return (f"display-list high-water {high_water} exceeds qualification "
                f"budget {budget['qualification_max_commands']} (capacity "
                f"{budget['capacity_commands']})")
    return None


def high_water_failures(output: str,
                        budget: dict[str, int]) -> tuple[list[str], int]:
    """Verdict on the engine's own display-list high-water witness.

    The census above counts what the host dispatcher was handed. This reads
    what the game measured against the row it authored into, which is the
    quantity the fail-closed assertion in gfxtask_run_xbus acts on. A run that
    reports nothing fails: an assertion nobody can see fire is not evidence.
    """
    rows = [(int(bytes_), int(limit))
            for bytes_, limit in HIGH_WATER_RE.findall(output)]
    if not rows:
        return (["no [TRACE] dl_high_water witness was emitted, so the "
                 "display-list margin was never measured"], 0)
    peak = max(bytes_ for bytes_, _limit in rows)
    if peak > budget["maximum_high_water_bytes"]:
        return ([f"display-list high-water {peak} bytes exceeds budget "
                 f"{budget['maximum_high_water_bytes']} (rows carried limits "
                 f"{sorted({limit for _bytes, limit in rows})})"], peak)
    return ([], peak)


def resource_failures(output: str, budgets: dict[str, Any],
                      required_cycles: int) -> tuple[list[str], dict[str, int]]:
    failures: list[str] = []
    summary: dict[str, int] = {}
    q = budgets["qualification"]
    # A deliberately labelled development run may exercise fewer than the five
    # release generations. The manifest always supplies required_cycles=20.
    configured_warm = int(q["warmed_generations"])
    warmed = configured_warm if required_cycles >= configured_warm else 1
    players = int(q["party_players"])

    if match := BAD_RE.search(output):
        failures.append(f"runtime failure marker {match.group(0)!r}")
    if "renderer backend:" not in output:
        failures.append("no renderer backend identity was emitted")

    spans = parse_spans(output)
    cycles = cycle_spans(spans)
    summary["cycles"] = len(cycles)
    if len(cycles) != required_cycles:
        failures.append(
            f"observed {len(cycles)} complete lobby->race->lobby cycles, "
            f"need exactly {required_cycles}")
    checked_cycles = cycles[:required_cycles]
    for number, (race, hub) in enumerate(checked_cycles, 1):
        for kind, span in (("race", race), ("return lobby", hub)):
            if error := binding_failure(
                    span.bindings, players, f"cycle {number} {kind}"):
                failures.append(error)
            if (players, players - 1) not in span.layouts:
                failures.append(
                    f"cycle {number} {kind}: no four-viewport layout")
            if (players - 1, players) not in span.huds:
                failures.append(
                    f"cycle {number} {kind}: no four-viewport HUD")

    sessions = [
        (match.group(1), *(int(value) for value in match.groups()[1:]))
        for match in SESSION_RE.finditer(output)
    ]
    if any(host != 0 for _state, _sgen, _lgen, host in sessions):
        failures.append("session host changed away from seat zero")
    sgens = {sgen for _state, sgen, _lgen, _host in sessions}
    if sgens != {1}:
        failures.append(f"session generation changed during the soak: {sorted(sgens)}")
    race_sessions = [row for row in sessions if row[0] == "ACTIVE_RACE"]
    if len(race_sessions) < required_cycles:
        failures.append(
            f"only {len(race_sessions)} ACTIVE_RACE session publications, "
            f"need {required_cycles}")
    for number, race in enumerate(race_sessions[:required_cycles], 1):
        later_lobbies = [
            row for row in sessions[sessions.index(race) + 1:]
            if row[0] == "ACTIVE_LOBBY" and row[2] > race[2]
        ]
        if not later_lobbies or later_lobbies[0][2] != race[2] + 1:
            failures.append(
                f"cycle {number}: race lgen {race[2]} has no exact +1 lobby return")
    race_lgens = [row[2] for row in race_sessions[:required_cycles]]
    if any(b != a + 2 for a, b in zip(race_lgens, race_lgens[1:])):
        failures.append(
            f"race level generations did not advance exactly two per cycle: "
            f"{race_lgens}")

    dl_budget = budgets["display_list"]
    all_checked_spans = [span for pair in checked_cycles for span in pair]
    central = [span for span in spans if span.level == CENTRAL_HUB_LEVEL]
    all_checked_spans += central[-1:]
    if not all_checked_spans or any(not span.dl_lengths for span in all_checked_spans):
        failures.append("one or more checked four-camera spans emitted no display lists")
    dl_max = max(
        (max(span.dl_lengths) for span in all_checked_spans if span.dl_lengths),
        default=0)
    summary["dl_max"] = dl_max
    if error := display_list_failure(dl_max, dl_budget):
        failures.append(error)
    high_water_errors, summary["dl_high_water_bytes"] = high_water_failures(
        output, dl_budget)
    failures += high_water_errors

    resources = [tuple(map(int, match.groups()))
                 for match in RESOURCE_RE.finditer(output)]
    race_resources = [row for row in resources
                      if row[0:3] == (RACE_LEVEL, 0, 0)]
    hub_resources = [row for row in resources
                     if row[0:3] == (HUB_LEVEL, 0, 100)]
    memory_budget = budgets["memory"]
    for row in resources:
        physical_alloc, physical_free, physical_lame = row[10:13]
        music_alloc, music_free = row[13:15]
        jingle_alloc, jingle_free = row[15:17]
        sfx_alloc, sfx_free = row[17:19]
        if (physical_alloc + physical_free + physical_lame != 40 or
                music_alloc + music_free != 26 or
                jingle_alloc + jingle_free != 16 or
                sfx_alloc + sfx_free != 32 or row[21] != 1):
            failures.append(f"incoherent audio/controller ownership row: {row}")
            break
        if row[5] < memory_budget["minimum_main_free_bytes"]:
            failures.append(f"main-pool free-byte reserve below budget: {row}")
            break
        if row[6] < memory_budget["minimum_main_largest_free_bytes"]:
            failures.append(f"main-pool largest block below budget: {row}")
            break
    for label, rows in (("race resources", race_resources),
                        ("lobby resources", hub_resources)):
        failures += plateau_exact(
            rows, warmed, lambda row: row[3:7] + row[7:10], label)
    if race_resources:
        summary["main_live"] = race_resources[-1][3]
        summary["main_used"] = race_resources[-1][4]

    renderers = [tuple(map(int, match.groups()))
                 for match in RENDERER_RE.finditer(output)]
    renderer_budget = budgets["renderer"]
    for row in renderers:
        start, created, deleted, live, peak = row[3:8]
        if deleted > start + created or start + created - deleted != live:
            failures.append(f"incoherent texture accounting: {row}")
            break
        if (peak < start or peak < live or
                live > renderer_budget["maximum_texture_live"] or
                peak > renderer_budget["maximum_texture_peak"]):
            failures.append(f"texture ownership exceeded its formal budget: {row}")
            break
    for level, label in ((RACE_LEVEL, "race renderer"),
                         (HUB_LEVEL, "lobby renderer")):
        cutscene = 0 if level == RACE_LEVEL else 100
        rows = [row for row in renderers if row[0:3] == (level, 0, cutscene)]
        failures += plateau_no_new_high(
            rows, warmed, lambda row: row[6:9], label)

    registries = [tuple(map(int, match.groups()))
                  for match in REGISTRY_RE.finditer(output)]
    for row in registries:
        live, high, ambiguous, full_fails, max_probe = row[3:8]
        if (live > high or live > renderer_budget["maximum_registry_live"] or
                high > renderer_budget["maximum_registry_high"] or
                max_probe > renderer_budget["maximum_registry_probe"] or
                ambiguous != 0 or full_fails != 0):
            failures.append(f"pointer-registry ownership exceeded budget: {row}")
            break
    for level, label in ((RACE_LEVEL, "race pointer registry"),
                         (HUB_LEVEL, "lobby pointer registry")):
        cutscene = 0 if level == RACE_LEVEL else 100
        rows = [row for row in registries if row[0:3] == (level, 0, cutscene)]
        failures += plateau_no_new_high(
            rows, warmed, lambda row: row[3:5], label)
    return failures, summary


def churn_output_failure(output: str, budgets: dict[str, Any]) -> str | None:
    match = CHURN_RE.search(output)
    if not match:
        return "formation/dissolution churn emitted no terminal census"
    cycles, sgen, lgen, session_bytes, failures = map(int, match.groups())
    expected_cycles = budgets["qualification"]["churn_cycles"]
    expected_lgen = expected_cycles * 3 + ((expected_cycles + 3) // 4) * 2
    if failures != 0:
        return f"formation/dissolution churn reported {failures} failures"
    if cycles != expected_cycles or sgen != expected_cycles:
        return (f"formation/dissolution counters cycles={cycles} sgen={sgen}, "
                f"expected {expected_cycles}")
    if lgen != expected_lgen:
        return f"formation/dissolution lgen={lgen}, expected {expected_lgen}"
    if session_bytes <= 0 or session_bytes > 4096:
        return f"implausible fixed session footprint: {session_bytes} bytes"
    return None


def compiler_for(build_argument: str) -> str:
    build = Path(build_argument)
    cache = (build if build.is_dir() else build.parent) / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_C_COMPILER:FILEPATH="):
                compiler = line.split("=", 1)[1]
                if Path(compiler).is_file():
                    return compiler
    return os.environ.get("CC") or shutil.which("cc") or "cc"


def run_churn(build_argument: str, budgets: dict[str, Any], verbose: bool) -> str | None:
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_perf_churn_") as tmp:
        binary = Path(tmp) / "adventure_party_performance_churn"
        command = [
            compiler_for(build_argument), "-std=c11", "-O2",
            "-I", str(ROOT / "platform"), str(CHURN_PATH),
            str(STATE_SOURCE), "-o", str(binary),
        ]
        if verbose:
            print("$ " + " ".join(command), flush=True)
        compiled = subprocess.run(
            command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=120, check=False)
        if compiled.returncode != 0:
            return (f"formation/dissolution churn compile failed "
                    f"({compiled.returncode}):\n{compiled.stdout[-4000:]}")
        ran = subprocess.run(
            [str(binary)], cwd=ROOT, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=120, check=False)
        if ran.returncode != 0:
            return (f"formation/dissolution churn failed ({ran.returncode}):\n"
                    f"{ran.stdout[-4000:]}")
        if error := churn_output_failure(ran.stdout, budgets):
            return error + "\n" + ran.stdout[-2000:]
        if verbose:
            print("  " + ran.stdout.strip())
    return None


def drive_route(cycles: int) -> str:
    central = "0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
    lobby = "12:" + ":".join("E5" for _ in range(cycles))
    return central + ";" + lobby


def bounded_input_text(cycles: int) -> str:
    """Project the checked-in sparse advances to this run's cycle horizon.

    After the final ordered door step, another A edge at the return entrance can
    legitimately drive straight back through that door. Truncating only the
    post-admission advance edges prevents an unrequested 21st cycle while the
    process remains in the final lobby long enough to flush its counters.

    The last cycle keeps ONE advance past that horizon, and the census depends
    on it. These sparse advances are also this route's clock: every cycle's
    post-race panels are held until a player presses A (postrace_render()'s
    POSTRACE_HOLD rezeroes its own timer while gPostraceScaleMiddle is
    negative, so there is no timeout to wait out), which rounds each cycle up
    to the same multiple of the advance spacing and is what makes twenty
    generations comparable at all. Truncate at the admission itself and the
    LAST cycle alone has no advance left: its panels then hold over a resident
    race level for as long as it takes some other path to clear them —
    measured at ~800 extra frames — and the extra results-screen textures that
    window uploads raise that one generation's texPeak by 3, a terminal-only
    "new ownership high" in a census whose whole premise is equal cycles. One
    more advance costs nothing (it lands inside the terminal panels, before the
    return, so no further door is entered) and makes the last cycle the same
    shape as the nineteen before it.
    """
    horizon = 6300 + max(0, cycles - 1) * 3000
    kept: list[str] = []
    past_horizon = 0
    for line in INPUT_PATH.read_text(encoding="utf-8").splitlines():
        fields = line.strip().split()
        if fields and fields[0].isdigit():
            frame = int(fields[0])
            if frame >= 3600 and frame > horizon:
                past_horizon += 1
                if past_horizon > 1:
                    continue
        kept.append(line)
    return "\n".join(kept) + "\n"


def performance_eeprom(rom: Path) -> bytes:
    """Started A1 fixture with Ancient Lake already cleared.

    Repeated performance generations must be equivalent. Starting from an
    uncleared course lets the first natural human win mutate progression and
    schedules a later course cinematic, so the third door entry is no longer a
    race generation. AP-13 owns that behavior. AP-19 pre-clears only this one
    status field using the ROM-derived save ordinal; all runtime cycles then
    take the ordinary replay path without an award or presentation detour.
    """
    image = bytearray(eeprom_image())
    decoded = decode_progress(bytes(image), str(rom))
    ordinal = int(decoded["course_ordinal"])
    slot = bytearray(image[:SLOT_BYTES])
    offset = 16 + ordinal * 2
    value = 2  # RACE_CLEARED
    for bit_index in range(2):
        absolute = offset + bit_index
        mask = 1 << (7 - absolute % 8)
        if value & (1 << (1 - bit_index)):
            slot[absolute // 8] |= mask
        else:
            slot[absolute // 8] &= ~mask
    image[:SLOT_BYTES] = seal_slot(slot)
    return bytes(image)


def run_rom_arm(binary: Path, rom: Path, cycles: int, frames: int,
                timeout: float, artifacts_dir: Path | None,
                verbose: bool) -> tuple[str | None, str]:
    keep = artifacts_dir is not None
    if artifacts_dir is None:
        root = Path(tempfile.mkdtemp(prefix="mdkr_ap_performance_"))
    else:
        artifacts_dir.mkdir(parents=True, exist_ok=True)
        root = Path(tempfile.mkdtemp(prefix="run-", dir=artifacts_dir))
    log_path = root / "run.log"
    try:
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(performance_eeprom(rom))
        input_script = root / "adventure_party_performance.txt"
        input_script.write_text(bounded_input_text(cycles), encoding="utf-8")
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("MDKR", "GE007_"))}
        env.update({
            "LC_ALL": "C",
            "MDKR_AUDIO": "0",
            "MDKR_TRACE": "1",
            "MDKR_RESOURCE_STATS": "1",
            "MDKR_AUTOPILOT": "1",
            "MDKR_AUTOPILOT_UNSTICK": "1",
            "MDKR_DRIVE_ROUTE": drive_route(cycles),
            "MDKR_FORCE_LAPS": "1",
            "MDKR_TEST_POSTRACE_OPTION": "1",
            "MDKR_SIMULATION_CADENCE": "enhanced",
            "MDKR_SYNTH_FIELDS": "1",
        })
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "video.ini")
        command = [
            str(binary), "--headless-frames", str(frames),
            "--input-script", str(input_script), "--rom", str(rom),
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
            return f"ROM soak timed out after {timeout:.0f}s; artifacts: {root}", output
        output = log_path.read_text(encoding="utf-8", errors="replace")
        if process.returncode != 0:
            return (f"ROM soak exited {process.returncode}; artifacts: {root}",
                    output)
        if keep:
            print(f"  retained AP-19 evidence: {root}")
        return None, output
    finally:
        if not keep:
            shutil.rmtree(root, ignore_errors=True)


def self_test(budgets: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    expected = budgets["qualification"]["churn_cycles"]
    lgen = expected * 3 + ((expected + 3) // 4) * 2
    good_churn = (f"aparty_perf_churn: cycles={expected} sgen={expected} "
                  f"lgen={lgen} bytes=192 failures=0")
    if churn_output_failure(good_churn, budgets) is not None:
        failures.append("self-test: valid churn census was rejected")
    broken_churn = good_churn.replace("failures=0", "failures=1")
    if churn_output_failure(broken_churn, budgets) is None:
        failures.append("self-test: retained-session churn control passed")

    # The controls call the SAME verdict functions as the measured arm.
    dl = budgets["display_list"]
    if display_list_failure(dl["qualification_max_commands"] + 1, dl) is None:
        failures.append("self-test: over-capacity display-list control passed")

    over_budget = dl["maximum_high_water_bytes"] + 1
    witness = (f"[TRACE] dl_high_water: bytes=32 limit={over_budget}\n"
               f"[TRACE] dl_high_water: bytes={over_budget} limit={over_budget}")
    if not high_water_failures(witness, dl)[0]:
        failures.append("self-test: over-budget high-water control passed")
    if high_water_failures(witness.splitlines()[0], dl)[0]:
        failures.append("self-test: an in-budget high-water witness was rejected")
    if not high_water_failures("", dl)[0]:
        failures.append("self-test: a run with no high-water witness passed")

    synthetic = [(5, 0, 100, 20, 2000), (5, 0, 100, 20, 2000),
                 (5, 0, 100, 20, 2000), (5, 0, 100, 21, 2016),
                 (5, 0, 100, 22, 2032)]
    if not plateau_exact(synthetic, 5, lambda row: row[3:5], "control"):
        failures.append("self-test: monotonic ownership-growth control passed")
    if not plateau_no_new_high(
            synthetic, 5, lambda row: row[3:5], "control"):
        failures.append("self-test: terminal renderer-growth control passed")

    # An early high must not camouflage a later upward staircase. This is the
    # false-positive shape a terminal-max-only oracle would accept.
    masked_growth = [(5, 0, 100, 30), (5, 0, 100, 20),
                     (5, 0, 100, 21), (5, 0, 100, 22),
                     (5, 0, 100, 23)]
    if not plateau_no_new_high(
            masked_growth, 5, lambda row: row[3:4], "masked control"):
        failures.append("self-test: transient-masked ownership growth passed")

    wrong = [(0, 0), (1, 1), (2, 2), (3, 3)]
    wrong[-1] = (3, 2)
    if binding_failure(wrong, 4, "control") is None:
        failures.append("self-test: swapped-controller control passed")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--frames", type=int,
                        help="override the soak frame ceiling")
    parser.add_argument("--timeout", type=float, default=14400.0)
    parser.add_argument("--artifacts-dir", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--development-cycles", type=int,
        help="non-qualifying bounded route run (1-19 cycles)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    try:
        budgets = load_budgets()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"check_adventure_party_performance: FAIL -- budgets: {error}",
              file=sys.stderr)
        return 1
    failures = self_test(budgets)
    if args.self_test:
        if failures:
            for failure in failures:
                print("  - " + failure, file=sys.stderr)
            return 1
        print("check_adventure_party_performance: PASS -- parser, formal-budget, "
              "growth, churn, and controller-binding controls fired")
        return 0

    q = budgets["qualification"]
    cycles = int(q["race_cycles"])
    development = args.development_cycles is not None
    if development:
        if not 1 <= args.development_cycles < cycles:
            print("check_adventure_party_performance: FAIL -- "
                  "--development-cycles must be in 1..19", file=sys.stderr)
            return 1
        cycles = args.development_cycles
        print(f"NOTE: non-qualifying AP-19 development run ({cycles}/20 cycles)")
    frames = args.frames if args.frames is not None else 8000 + cycles * 3200
    minimum_frames = 7100 + max(0, cycles - 1) * 3000
    if frames < minimum_frames:
        print(f"check_adventure_party_performance: FAIL -- need at least "
              f"{minimum_frames} frames for {cycles} cycles", file=sys.stderr)
        return 1

    required = (BUDGET_PATH, INPUT_PATH, CHURN_PATH, STATE_SOURCE)
    missing = [str(path) for path in required if not path.is_file()]
    try:
        binary = Path(resolve_binary(args.build)).resolve()
    except (OSError, ValueError) as error:
        print(f"check_adventure_party_performance: FAIL -- binary: {error}",
              file=sys.stderr)
        return 1
    rom = Path(args.rom).expanduser().resolve()
    missing += [str(path) for path in (binary, rom) if not path.is_file()]
    if missing:
        print("check_adventure_party_performance: FAIL -- missing " +
              ", ".join(missing), file=sys.stderr)
        return 1

    if error := run_churn(args.build, budgets, args.verbose):
        failures.append(error)
    run_error, output = run_rom_arm(
        binary, rom, cycles, frames, args.timeout, args.artifacts_dir,
        args.verbose)
    if run_error:
        failures.append(run_error)
    measured, summary = resource_failures(output, budgets, cycles)
    failures.extend(measured)
    if failures:
        print("check_adventure_party_performance: FAIL", file=sys.stderr)
        for failure in failures:
            print("  - " + failure.replace("\n", "\n    "), file=sys.stderr)
        return 1
    qualifier = "DEVELOPMENT" if development else "QUALIFIED"
    print(
        f"check_adventure_party_performance: PASS ({qualifier}) -- "
        f"{summary.get('cycles', 0)} four-player lobby->race->lobby cycles; "
        f"DL high-water {summary.get('dl_max', 0)}/"
        f"{budgets['display_list']['qualification_max_commands']} commands "
        f"({summary.get('dl_high_water_bytes', 0)}/"
        f"{budgets['display_list']['maximum_high_water_bytes']} bytes measured "
        "in-engine); "
        f"warmed pool/audio/renderer/registry ownership plateaued; exact "
        f"four-controller/session counters held; {q['churn_cycles']} "
        "formation/dissolution lifetimes retained no state; mutation controls fired"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
