#!/usr/bin/env python3
"""AP-18 release qualification: exhaustive campaign inventory + real doors.

The checked-in manifest is an oracle, not a hand-waved checklist.  This gate
independently decodes the supported ROM's level headers, save-order table and
vehicle masks, then requires every playable Adventure lobby and every
save-eligible course to appear exactly once with its party policy, entry class,
world, retail name, default vehicle and available vehicle set.

The behavioural arm replaces the four AP v1 retarget precedents wherever the
headless driver can physically traverse retail geometry:

* silver: hub -> Dino lobby -> the real Ancient Lake silver door -> silver race;
* boss: hub -> lobby -> real Hot Top door -> lobby -> the real Tricky door,
  host-solo suspension, exact roster restore, and a vehicle-stack witness;
* breadth: the boss arm is a multi-hop 0->12->7->12->38->12 route;
* conflict: two party racers target distinct real Dino doors (E5 and E3); the
  combined route must latch one transition, reject the other, and load exactly
  one destination.

Challenge-key and trophy-cabinet hook removal is not falsely claimed.  Their
real ROM objects and measured headless collision limits are explicit manifest
facts.  The key-door driver reaches 708.3 units but cannot cross the authored
wall (open radius 70); two legitimate trophy approaches either wedge at the
interior wall or let another live party racer hit a real exit first.  The
dedicated AP-15/AP-16 policy gates remain the behavioural coverage for those
two envelopes until a navigation-capable headless driver exists.

No MDKR_LOAD_TRACK, MDKR_SILVER_FORCE, MDKR_CHALLENGE_FORCE or
MDKR_TROPHY_FORCE is used here.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env
from check_campaign_progression import Slot, derive_seam_a, level_worlds, world_topology
from check_first_boss_progression import checkpoint_image, save_order
from check_vehicle_sweep import rom_vehicle_matrix

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tests/data/adventure_party_campaign_manifest.json"
SCRIPT = ROOT / "tests/input_scripts/adventure_party_campaign_3p.txt"
ENUMS = ROOT / "game/include/asset_enums.h"

HUB = 0
DINO = 12
ANCIENT_LAKE = 5
FOSSIL_CANYON = 3
HOT_TOP = 7
TRICKY = 38

HUB_ROUTE = "0:200,500:-1004,946:-1858,1099:-3381,1946:-3948,2180:E12"
SILVER_ROUTE = HUB_ROUTE + ";12:-300,700:E5"
BOSS_ROUTE = HUB_ROUTE + ";12:E7:E38"
CONFLICT_ROUTES = "0=E5;2=E3"

ACTION_TRIGGER = 2
VERDICT_REJECTED_LATCHED = -1

BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")
LEVEL_RE = re.compile(
    r"level_load: levelId=(-?\d+) numPlayers=(-?\d+) entrance=(-?\d+) "
    r"vehicle=(-?\d+) cutscene=(-?\d+) @frame~(\d+)")
TRANS_RE = re.compile(
    r"aparty_transition: seat=(\d+) tick=(\d+) trigger=(\d+) dest=(\d+) lgen=(\d+)")
INTER_RE = re.compile(r"aparty_interaction: seat=(\d+) action=(-?\d+) verdict=(-?\d+)")
SESSION_RE = re.compile(r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
RESTORE_RE = re.compile(r"aparty_restore: suspend_lgen=(\d+) restore_lgen=(\d+) match=(-?\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)")
SILVER_RACE_RE = re.compile(
    r"silvercoinrace: courseId=(\d+) worldId=(-?\d+) bosses=0x([0-9a-f]+) "
    r"courseFlags=0x([0-9a-f]+).*verdict=(-?\d+)")
SILVER_FINISH_RE = re.compile(
    r"silvercoinfinish: courseId=(\d+) leadPlayerIndex=(-?\d+) coins=(-?\d+) "
    r"teamCoins=(-?\d+) silverRace=(-?\d+)")
BOSS_FINISH_RE = re.compile(
    r"bossfinish: finishPos=(-?\d+) playerIndex=(-?\d+) courseId=(\d+) ")
AWARD_RE = re.compile(
    r"aparty_award: op=(\w+) sgen=\d+ lgen=\d+ course=(\d+) "
    r"activity=(\d+) kind=(\d+) result=(-?\d+)")

FORBIDDEN_RETARGETS = {
    "MDKR_LOAD_TRACK", "MDKR_SILVER_FORCE", "MDKR_CHALLENGE_FORCE",
    "MDKR_TROPHY_FORCE",
}


def level_names() -> dict[int, str]:
    text = ENUMS.read_text(encoding="utf-8")
    block = text.split("typedef enum AssetLevelHeadersEnum", 1)[1]
    block = block.split("ASSET_LEVEL_HEADERS_COUNT", 1)[0]
    names = re.findall(r"ASSET_LEVEL_([A-Z0-9]+)\s*,", block)
    return dict(enumerate(names))


def expected_course_class(level: int, race_type: int) -> str:
    challenge = {11: "challenge_eggs", 26: "challenge_battle",
                 27: "challenge_battle", 25: "challenge_bananas"}
    boss = {
        38: "boss_first", 46: "boss_rematch", 40: "boss_first",
        53: "boss_rematch", 1: "boss_first", 52: "boss_rematch",
        41: "boss_first", 54: "boss_rematch", 37: "boss_final_one",
        55: "boss_final_two",
    }
    if race_type == 0:
        return "default"
    if level in challenge:
        return challenge[level]
    if level in boss:
        return boss[level]
    return f"unclassified_racetype_{race_type}"


def validate_manifest(data: dict, rom: str) -> list[str]:
    failures: list[str] = []
    if data.get("schema") != 1:
        failures.append(f"manifest schema={data.get('schema')!r}, expected 1")

    info = level_worlds(rom)
    eligible = save_order(rom)
    vehicles = rom_vehicle_matrix(rom)
    names = level_names()

    lobbies = data.get("lobbies", [])
    lobby_ids = [row.get("id") for row in lobbies]
    expected_lobbies = {i for i, (world, race_type) in info.items()
                        if world >= 0 and race_type == 5}
    if len(lobby_ids) != len(set(lobby_ids)):
        failures.append(f"manifest lobbies contain duplicate ids: {lobby_ids}")
    if set(lobby_ids) != expected_lobbies:
        failures.append(f"manifest lobby ids={sorted(lobby_ids)}, ROM={sorted(expected_lobbies)}")
    for row in lobbies:
        level = row.get("id")
        if level not in info:
            continue
        if row.get("name") != names.get(level):
            failures.append(f"lobby {level}: name={row.get('name')!r}, enum={names.get(level)!r}")
        if row.get("world") != info[level][0]:
            failures.append(f"lobby {level}: world={row.get('world')}, ROM={info[level][0]}")
        if row.get("policy") != "split_party" or not row.get("route"):
            failures.append(f"lobby {level}: missing split-party policy/routed witness")

    courses = data.get("courses", [])
    course_ids = [row.get("id") for row in courses]
    if len(course_ids) != len(set(course_ids)):
        failures.append(f"manifest courses contain duplicate ids: {course_ids}")
    if set(course_ids) != set(eligible):
        missing = sorted(set(eligible) - set(course_ids))
        extra = sorted(set(course_ids) - set(eligible))
        failures.append(f"manifest save-course inventory differs: missing={missing} extra={extra}")
    for row in courses:
        level = row.get("id")
        if level not in info:
            continue
        world, race_type = info[level]
        if row.get("name") != names.get(level):
            failures.append(f"course {level}: name={row.get('name')!r}, enum={names.get(level)!r}")
        if row.get("world") != world:
            failures.append(f"course {level}: world={row.get('world')}, ROM={world}")
        expected_class = expected_course_class(level, race_type)
        if row.get("class") != expected_class:
            failures.append(f"course {level}: class={row.get('class')!r}, expected={expected_class!r}")
        expected_policy = "split_party_six_racers" if race_type == 0 else "host_solo_restore"
        if row.get("policy") != expected_policy:
            failures.append(f"course {level}: policy={row.get('policy')!r}, expected={expected_policy!r}")
        if race_type == 0:
            default, mask = vehicles[level]
            available = [v for v in range(3) if mask & (1 << v)]
            if row.get("default_vehicle") != default or row.get("vehicles") != available:
                failures.append(
                    f"course {level}: vehicles default/list="
                    f"{row.get('default_vehicle')}/{row.get('vehicles')}, "
                    f"ROM={default}/{available}")
            # VERSION >= 79's multiplayer picker narrows exactly two masks.
            # Adventure Party is always 2+ players, so classify both the raw
            # header capability and the actual party-facing choice set.
            party_available = [v for v in available
                               if not (level == 15 and v == 1)
                               and not (level == 28 and v == 2)]
            if row.get("party_vehicles") != party_available:
                failures.append(
                    f"course {level}: party vehicles={row.get('party_vehicles')}, "
                    f"expected multiplayer set={party_available}")
        if not row.get("door"):
            failures.append(f"course {level}: no entry-door classification")

    expected_pairs = {
        (0, 12), (0, 24), (0, 14), (0, 2), (0, 35),
        (12, 0), (24, 0), (14, 0), (2, 0), (35, 0),
    }
    rows = data.get("world_transitions", [])
    pairs = [(r.get("from"), r.get("to")) for r in rows]
    if len(pairs) != len(set(pairs)) or set(pairs) != expected_pairs:
        failures.append(f"world-transition inventory differs: rows={pairs}")
    for row in rows:
        if not all(row.get(k) for k in ("class", "requirement", "route")):
            failures.append(f"world transition {row.get('from')}->{row.get('to')} is not classified/routed")

    required_doors = {
        "world_lobby", "world_return", "default", "silver", "world_key",
        "boss_first", "boss_rematch", "trophy_cabinet", "future_fun_land",
    }
    doors = data.get("door_classes", {})
    if set(doors) != required_doors:
        failures.append(f"door-class inventory differs: {sorted(doors)}")
    for key, row in doors.items():
        if not row.get("policy") or not row.get("dynamic_witness"):
            failures.append(f"door class {key}: missing policy/witness")

    branches = data.get("campaign_branches", {})
    required_branches = {
        "adventure_one", "adventure_two", "default_race", "silver_race",
        "challenge", "boss", "trophy", "taj_transform", "new_game",
        "in_hub_taj_challenge",
    }
    if set(branches) != required_branches or not all(branches.values()):
        failures.append(f"campaign-branch inventory differs: {sorted(branches)}")

    limits = data.get("headless_route_limits", {})
    key = limits.get("world_key", {})
    if (key.get("level"), key.get("destination"), key.get("fixture_key_bit"),
            key.get("open_radius")) != (12, 11, 2, 70):
        failures.append("world-key headless limit lost its exact ROM route facts")
    if not isinstance(key.get("closest_approach"), (int, float)) or key.get("closest_approach", 0) <= 70:
        failures.append("world-key headless limit no longer proves the driver remained outside open radius")
    trophy = limits.get("trophy_cabinet", {})
    if (trophy.get("level"), trophy.get("npc_behavior"), trophy.get("npc_position")) != (12, 74, [933, -6, -2172]):
        failures.append("trophy-cabinet headless limit lost its exact ROM object facts")
    return failures


def run_arm(binary: str, rom: str, fixture: bytes, *, frames: int,
            values: dict[str, str], label: str, verbose: bool) -> tuple[str, int | str]:
    illegal = FORBIDDEN_RETARGETS.intersection(values)
    if illegal:
        raise AssertionError(f"{label}: forbidden retarget hook(s): {sorted(illegal)}")
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_campaign_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(fixture)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                   MDKR_SIMULATION_CADENCE="original", MDKR_SYNTH_FIELDS="2")
        env.update(values)
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        command = [
            binary, "--headless-frames", str(frames),
            "--input-script", str(SCRIPT), "--rom", rom,
            "--window-size", "320x240",
            "--video-set", "Enhancements.AdventureParty=1",
        ]
        if verbose:
            print(f"$ {' '.join(command)}  # {label}", flush=True)
        try:
            proc = subprocess.run(
                command, cwd=root, env=env, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=max(300, frames // 6), check=False)
            out, rc = proc.stdout or "", proc.returncode
        except subprocess.TimeoutExpired as exc:
            out = (exc.stdout or "")
            if isinstance(out, bytes):
                out = out.decode("utf-8", "replace")
            rc = "timeout"
    if verbose:
        for line in out.splitlines():
            if any(token in line for token in (
                    "level_load:", "aparty_transition:", "aparty_session:",
                    "aparty_restore:", "silvercoinrace:", "silvercoinfinish:",
                    "bossfinish:")):
                print(line)
    return out, rc


def base_fail(label: str, out: str, rc: int | str) -> list[str]:
    failures = []
    if rc != 0:
        failures.append(f"{label}: process exit {rc}")
    if bad := BAD_RE.search(out):
        failures.append(f"{label}: runtime failure marker {bad.group(0)!r}")
    return failures


def transitions(out: str) -> list[tuple[int, int, int, int]]:
    return [(int(m[1]), int(m[2]), int(m[4]), int(m[5]))
            for m in TRANS_RE.finditer(out)]  # seat, tick, dest, generation


def levels(out: str) -> list[tuple[int, int, int, int]]:
    return [(int(m[1]), int(m[3]), int(m[4]), int(m[6]))
            for m in LEVEL_RE.finditer(out)]  # level, entrance, vehicle, frame


def assert_silver(out: str, label: str) -> list[str]:
    failures: list[str] = []
    tr = transitions(out)
    dests = [row[2] for row in tr]
    if dests[:2] != [DINO, ANCIENT_LAKE]:
        failures.append(f"{label}: real-door prefix was {dests[:2]}, expected [12, 5]")
    if f"drive: level={DINO} step 1: exit to {ANCIENT_LAKE} TAKEN" not in out:
        failures.append(f"{label}: no physical E5 door-taken witness")
    races = [(int(m[1]), int(m[5])) for m in SILVER_RACE_RE.finditer(out)]
    if (ANCIENT_LAKE, 1) not in races:
        failures.append(f"{label}: Ancient Lake was not a retail-qualified silver race: {races}")
    finishes = [(int(m[1]), int(m[4]), int(m[5])) for m in SILVER_FINISH_RE.finditer(out)]
    if not any(course == ANCIENT_LAKE and team == 8 and silver == 1
               for course, team, silver in finishes):
        failures.append(f"{label}: no silver finish at teamCoins=8: {finishes}")
    lobby_loads = [m for m in LEVEL_RE.finditer(out) if int(m[1]) == DINO]
    if len(lobby_loads) < 2:
        failures.append(f"{label}: silver race did not return through the Dino lobby")
    else:
        returned = out[lobby_loads[1].start():]
        if not any(int(n) == 3 and int(mask, 16) == 0x7
                   for n, mask in ROSTER_RE.findall(returned)):
            failures.append(f"{label}: returned lobby did not republish roster n=3 mask=0x7")
    return failures


def assert_boss(out: str, label: str) -> list[str]:
    failures: list[str] = []
    tr = transitions(out)
    dests = [row[2] for row in tr]
    wanted = [DINO, HOT_TOP, TRICKY]
    cursor = 0
    for dest in dests:
        if cursor < len(wanted) and dest == wanted[cursor]:
            cursor += 1
    if cursor != len(wanted):
        failures.append(f"{label}: missing real multi-hop 0->12->7->12->38 route; transitions={tr}")
    if not any(state == "SOLO_ACTIVITY" for state, *_ in SESSION_RE.findall(out)):
        failures.append(f"{label}: real boss door did not suspend to SOLO_ACTIVITY")
    if not any(int(match) == 1 for _a, _b, match in RESTORE_RE.findall(out)):
        failures.append(f"{label}: party restore did not report match=1")
    boss_finishes = [(int(pos), int(pidx), int(course))
                     for pos, pidx, course in BOSS_FINISH_RE.findall(out)]
    if not any(pidx == 0 and course == TRICKY for _pos, pidx, course in boss_finishes):
        failures.append(f"{label}: no host-solo Tricky finish: {boss_finishes}")

    lev = levels(out)
    boss_frame = next((frame for level, _ent, _veh, frame in lev if level == TRICKY), None)
    if boss_frame is None:
        failures.append(f"{label}: Tricky never loaded")
    else:
        before = [(ent, veh, frame) for level, ent, veh, frame in lev
                  if level == DINO and frame < boss_frame]
        after = [(ent, veh, frame) for level, ent, veh, frame in lev
                 if level == DINO and frame > boss_frame]
        if not before or not after:
            failures.append(f"{label}: missing before/after lobby loads for vehicle restore witness")
        else:
            pre, post = before[-1], after[0]
            if pre[1] != post[1]:
                failures.append(
                    f"{label}: suspended lobby vehicle {pre[1]} restored as {post[1]} "
                    f"(pre frame {pre[2]}, post frame {post[2]})")
            if post[0] != 5:
                failures.append(f"{label}: boss return entrance={post[0]}, expected retail entrance 5")
    consumes = [(int(course), int(kind), int(result))
                for op, course, _activity, kind, result in AWARD_RE.findall(out)
                if op == "consume"]
    if sum(course == TRICKY and kind == 2 and result == 0
           for course, kind, result in consumes) != 1:
        failures.append(f"{label}: first boss did not consume exactly one boss token: {consumes}")
    return failures


def assert_conflict(out: str, label: str) -> list[str]:
    failures: list[str] = []
    tr = [row for row in transitions(out) if row[2] in (ANCIENT_LAKE, FOSSIL_CANYON)]
    if len(tr) != 1:
        failures.append(f"{label}: two distinct real doors latched {len(tr)} transitions: {tr}")
    # Scope rejection evidence to the destination lobby. Shared driving can put
    # multiple racers through the *hub* exit too; that rejection proves E12's
    # latch, not the distinct E5/E3 conflict under test.
    lobby_pos = out.find(f"level_load: levelId={DINO}")
    tail = out[lobby_pos:] if lobby_pos >= 0 else ""
    rejects = [(int(seat), int(action), int(verdict))
               for seat, action, verdict in INTER_RE.findall(tail)
               if int(action) == ACTION_TRIGGER and int(verdict) == VERDICT_REJECTED_LATCHED]
    if not rejects:
        failures.append(f"{label}: losing distinct door was not rejected")
    elif tr and all(seat == tr[0][0] for seat, _action, _verdict in rejects):
        failures.append(f"{label}: only the winning seat appears in rejections: {rejects}")
    loaded = [level for level, _ent, _veh, _frame in levels(out)
              if level in (ANCIENT_LAKE, FOSSIL_CANYON)]
    if len(loaded) != 1:
        failures.append(f"{label}: conflicting doors loaded {loaded}, expected exactly one destination")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--self-test", action="store_true",
                        help="validate manifest and mutation controls; skip game runs")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    rom = str(Path(args.rom).resolve())
    required = [MANIFEST, SCRIPT, ENUMS, Path(rom)]
    if not args.self_test:
        binary = str(Path(resolve_binary(args.build)).resolve())
        required.append(Path(binary))
    missing = [str(p) for p in required if not Path(p).is_file()]
    if missing:
        for path in missing:
            print(f"check_adventure_party_campaign: FAIL -- missing {path}", file=sys.stderr)
        return 1

    try:
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"check_adventure_party_campaign: FAIL -- manifest: {exc}", file=sys.stderr)
        return 1

    failures = validate_manifest(manifest, rom)

    # Positive control: deletion of one classified course must be detected.
    mutated = copy.deepcopy(manifest)
    mutated["courses"] = mutated["courses"][1:]
    if not validate_manifest(mutated, rom):
        failures.append("positive control: missing-course manifest mutation passed")

    if not args.self_test:
        eligible = save_order(rom)
        topo = world_topology(rom, eligible)[1]

        silver_slot = derive_seam_a(topo)
        # A fully progressed legitimate player has completed every Taj vehicle
        # offer.  Without these retail flags, a lobby arrival starts Taj's
        # automatic first-offer dialogue before a door can be driven.
        silver_slot.taj = 0x3F
        silver, rc = run_arm(
            binary, rom, silver_slot.image(eligible), frames=9000,
            label="real silver door", verbose=args.verbose,
            values={
                "MDKR_AUTOPILOT": "1", "MDKR_DRIVE_ROUTE": SILVER_ROUTE,
                "MDKR_SILVER_ROUTE": "1", "MDKR_FORCE_LAPS": "1",
                "MDKR_AP_RACE_WINNER": "0", "MDKR_WATCH_COURSEFLAGS": "5",
            })
        failures += base_fail("real silver door", silver, rc)
        failures += assert_silver(silver, "real silver door")
        # Positive control: deleting the measured real-door transition must fail.
        stripped = TRANS_RE.sub("", silver, count=1)
        if not assert_silver(stripped, "silver stripped-transition control"):
            failures.append("positive control: stripped silver real-door transition passed")

        boss, rc = run_arm(
            binary, rom, checkpoint_image(eligible), frames=15000,
            label="real boss multi-hop", verbose=args.verbose,
            values={
                "MDKR_AUTOPILOT": "1", "MDKR_AUTOPILOT_UNSTICK": "L7",
                "MDKR_DRIVE_ROUTE": BOSS_ROUTE, "MDKR_BOSS_ROUTE": "1",
                "MDKR_BOSS_WIN": "1", "MDKR_FORCE_LAPS": "1",
                "MDKR_AP_RACE_WINNER": "0", "MDKR_WATCH_COURSEFLAGS": "38",
            })
        failures += base_fail("real boss multi-hop", boss, rc)
        failures += assert_boss(boss, "real boss multi-hop")
        # Positive control: corrupt only the post-restore vehicle witness.
        boss_levels = list(LEVEL_RE.finditer(boss))
        trick = next((m for m in boss_levels if int(m[1]) == TRICKY), None)
        post = next((m for m in boss_levels
                     if trick and int(m[1]) == DINO and int(m[6]) > int(trick[6])), None)
        if post:
            line = post.group(0)
            changed = re.sub(r"vehicle=-?\d+", "vehicle=1", line)
            corrupted = boss[:post.start()] + changed + boss[post.end():]
            if not assert_boss(corrupted, "boss vehicle mutation control"):
                failures.append("positive control: mutated restore vehicle passed")
        else:
            failures.append("positive control setup: no boss restore load to mutate")

        conflict_fixture = Slot(taj=0x3F, balloons=[3, 3, 0, 0, 0, 0]).image(eligible)
        common = {"MDKR_AUTOPILOT": "1", "MDKR_DRIVE_ROUTE": HUB_ROUTE}
        # The input contract itself is part of the oracle: these must remain two
        # distinct ROM destination ids, rather than regressing to AP-10's same-E12
        # collision. The run below supplies this exact string to the seat driver.
        if set(CONFLICT_ROUTES.split(";")) != {"0=E5", "2=E3"}:
            failures.append(f"conflict route is not the exact distinct E5/E3 pair: {CONFLICT_ROUTES}")
        conflict, rc = run_arm(
            binary, rom, conflict_fixture, frames=5000,
            label="two distinct conflicting doors", verbose=args.verbose,
            values=common | {"MDKR_AP_SEAT_ROUTE": CONFLICT_ROUTES})
        failures += base_fail("two distinct conflicting doors", conflict, rc)
        failures += assert_conflict(conflict, "two distinct conflicting doors")
        stripped = re.sub(
            r"(?m)^.*aparty_interaction: seat=\d+ action=2 verdict=-1.*\n?",
            "", conflict)
        if not assert_conflict(stripped, "stripped-rejection conflict control"):
            failures.append("positive control: stripped distinct-door rejection passed")

    if failures:
        print("check_adventure_party_campaign: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    mode = "manifest/self-test" if args.self_test else "manifest + real-door campaign routes"
    print(f"check_adventure_party_campaign: PASS -- AP-18 {mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
