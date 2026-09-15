#!/usr/bin/env python3
"""Cross-feature authority and presentation proof for parties using customs.

This gate owns the first release-critical Adventure Party/custom-character
composites:

* D1: a real three-player admission publishes retail donor ids in the party
  roster while all three package identities remain live presentation state;
* D2: Taj's whole-party vehicle rebuild retains every package assignment;
* D3: vehicle-incompatible presentation falls back to its built-in donor;
* D4/D5: two players can select the same package through the real roster and
  reach a real split-screen race;
* D6: a host-solo boss detour restores the party and its custom identities;
* D7: a custom-equipped party win writes byte-identical save data to the same
  party win without custom presentation;
* D8 is the reducer-level quit/re-form staleness case;
* D9: a party-origin host-solo challenge binds all four flag identities; and
* D10: a playable Taj and a custom appearance coexist in one party roster.

Package ids never enter the Adventure Party reducer or EEPROM.  The running
binary joins those domains only in ``aparty_custom_identity`` diagnostics at
live racer publication seams; the gate also requires real replacement draws.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, SLOT_BYTES, resolve_binary, save_env


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import character_asset_probe as probe  # noqa: E402
import character_manifest_wizard as wizard  # noqa: E402
import character_package_manager as manager  # noqa: E402
import check_adventure_party_boss_restore as boss  # noqa: E402
import check_adventure_party_challenges as challenge  # noqa: E402
import check_adventure_party_progress as progress  # noqa: E402
import check_adventure_party_taj as taj  # noqa: E402
from character_validation_fixture import accepted_character_validation  # noqa: E402
from check_adventure_party_admission import eeprom_image  # noqa: E402
from check_custom_character_identity_surfaces import make_identity_portrait  # noqa: E402
from test_character_asset_probe import make_animated_glb  # noqa: E402


DONOR_NAMES = {
    0: "krunch",
    1: "bumper",
    5: "banjo",
    9: "diddy",
}
PACKAGE_IDS = {
    donor: f"org.mdkr.party-proof-{name}"
    for donor, name in DONOR_NAMES.items()
}
CAR_ONLY_PACKAGE_ID = "org.mdkr.party-proof-car-only-banjo"
DISPLAY_NAMES = {
    donor: f"Party Proof {name.title()}"
    for donor, name in DONOR_NAMES.items()
}
MINIMAP_COLOURS = {
    0: [238, 72, 91],
    1: [52, 219, 117],
    5: [232, 188, 46],
    9: [62, 142, 241],
}
EXPECTED_CHARACTERS = {
    2: {0: 9, 1: 0},
    3: {0: 9, 1: 0, 2: 1},
    4: {0: 9, 1: 0, 2: 1, 3: 5},
}
CUSTOM_RE = re.compile(
    r"aparty_custom_identity: phase=([a-z-]+) seat=(\d+) "
    r"package=([^ ]+) donor=(-?\d+) racer=(-?\d+) vehicle=(-?\d+) "
    r"matches=(\d+)"
)
ROSTER_RE = re.compile(
    r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)"
    r"((?: c\d+=\d+)*)"
)
REPLACEMENT_RE = re.compile(
    r"\[MODERN-CHARACTER\] replacements=(\d+) primitives=(\d+) "
    r"hiddenDonorBatches=(\d+)"
)
RESTORE_RE = re.compile(
    r"aparty_restore: suspend_lgen=(\d+) restore_lgen=(\d+) match=(-?\d+)"
)
FLAG_RE = re.compile(
    r"charflag_bound: playerID=(\d+) characterID=(\d+) texture=(\w+) "
    r"identity=(-?\d+) source=(\S+) package=(\S+)"
)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)


class CrossFeatureError(RuntimeError):
    pass


def install_fixtures(root: Path) -> Path:
    source = root / "source"
    characters = root / "characters"
    source.mkdir()
    model = source / "model.glb"
    license_path = source / "LICENSE.txt"
    model.write_bytes(make_animated_glb())
    license_path.write_text("CC0 1.0 Universal\n", encoding="utf-8")
    with accepted_character_validation(manager):
        for donor, donor_name in DONOR_NAMES.items():
            portrait = source / f"portrait-{donor_name}.png"
            manifest_path = source / f"manifest-{donor_name}.json"
            package = source / f"party-proof-{donor_name}.mdkrchar"
            portrait.write_bytes(make_identity_portrait())
            manifest, _ = wizard.build_manifest(
                model,
                PACKAGE_IDS[donor],
                DISPLAY_NAMES[donor],
                "CC0-1.0",
                "Generated Adventure Party integration fixture",
                f"https://example.invalid/{donor_name}",
                donor_name,
                ["car", "hovercraft", "plane"],
                portrait=portrait,
                minimap_rgb=MINIMAP_COLOURS[donor],
            )
            manifest["identity"].update({
                "short_name": f"AP {donor_name.title()}",
                "narration_name": f"Adventure Party proof {donor_name}",
                "sort_label": f"Party Proof, {donor_name.title()}",
            })
            manifest_path.write_text(
                json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
            )
            probe.build_package(
                model,
                manifest_path,
                license_path,
                package,
                portrait_path=portrait,
            )
            manager.install(package, characters)
        car_only_portrait = source / "portrait-car-only-banjo.png"
        car_only_manifest = source / "manifest-car-only-banjo.json"
        car_only_package = source / "party-proof-car-only-banjo.mdkrchar"
        car_only_portrait.write_bytes(make_identity_portrait())
        manifest, _ = wizard.build_manifest(
            model,
            CAR_ONLY_PACKAGE_ID,
            "Party Proof Car-Only Banjo",
            "CC0-1.0",
            "Generated Adventure Party vehicle fallback fixture",
            "https://example.invalid/car-only-banjo",
            "banjo",
            ["car"],
            portrait=car_only_portrait,
            minimap_rgb=[244, 132, 42],
        )
        car_only_manifest.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        probe.build_package(
            model,
            car_only_manifest,
            license_path,
            car_only_package,
            portrait_path=car_only_portrait,
        )
        manager.install(car_only_package, characters)
    return characters


def custom_environment(characters: Path, players: int) -> dict[str, str]:
    values = {
        "MDKR_RENDERER": "webgpu",
        "MDKR_RENDER_SCALE": "1",
        "MDKR64_HIDDEN": "1",
        "MDKR_CUSTOM_CHARACTER_DIRECTORY": str(characters),
    }
    for seat, donor in EXPECTED_CHARACTERS[players].items():
        values[f"MDKR_CUSTOM_CHARACTER_P{seat + 1}"] = PACKAGE_IDS[donor]
    return values


def custom_rows(output: str, phase: str) -> dict[int, tuple[str, int, int, int, int]]:
    rows: dict[int, tuple[str, int, int, int, int]] = {}
    for match in CUSTOM_RE.finditer(output):
        if match.group(1) != phase:
            continue
        rows[int(match.group(2))] = (
            match.group(3),
            int(match.group(4)),
            int(match.group(5)),
            int(match.group(6)),
            int(match.group(7)),
        )
    return rows


def require_custom_rows(output: str, phase: str, players: int) -> None:
    rows = custom_rows(output, phase)
    if set(rows) != set(range(players)):
        raise CrossFeatureError(
            f"{phase}: custom identity seats {sorted(rows)} != "
            f"{list(range(players))}"
        )
    for seat, donor in EXPECTED_CHARACTERS[players].items():
        package, reported_donor, racer, _vehicle, matches = rows[seat]
        if (
            package != PACKAGE_IDS[donor]
            or reported_donor != donor
            or racer != donor
            or matches != 1
        ):
            raise CrossFeatureError(
                f"{phase}: seat {seat} identity mismatch: {rows[seat]}"
            )


def require_clean_runtime(output: str, label: str) -> None:
    if match := BAD_RE.search(output):
        raise CrossFeatureError(f"{label}: fatal marker {match.group(0)!r}")
    if "[modern-character] P" in output and "fallback:" in output:
        fallbacks = [line for line in output.splitlines() if "fallback:" in line]
        raise CrossFeatureError(f"{label}: unexpected custom fallback {fallbacks}")
    replacements = REPLACEMENT_RE.findall(output)
    if not any(int(row[0]) > 0 for row in replacements):
        raise CrossFeatureError(f"{label}: no real custom replacement draws")


def require_roster(output: str, players: int) -> None:
    expected = EXPECTED_CHARACTERS[players]
    for match in ROSTER_RE.finditer(output):
        characters = {
            int(key[1:]): int(value)
            for key, value in (
                token.split("=") for token in match.group(3).split()
            )
        }
        if (
            int(match.group(1)) == players
            and int(match.group(2), 16) == (1 << players) - 1
            and characters == expected
        ):
            return
    raise CrossFeatureError(
        f"no exact {players}P donor roster {expected} reached the runtime"
    )


def run_hub(binary: Path, rom: Path, characters: Path, evidence: Path) -> str:
    arm = evidence / "d1-party-hub"
    save_dir = arm / "save"
    save_dir.mkdir(parents=True)
    (save_dir / "eeprom.bin").write_bytes(eeprom_image())
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_RACER_INPUT_TRACE="1",
    )
    environment.update(custom_environment(characters, 3))
    save_env(environment, str(save_dir))
    environment["MDKR_VIDEO_CONFIG_PATH"] = str(arm / "mdkr64.ini")
    process = subprocess.run(
        [
            str(binary),
            "--headless-frames",
            "4000",
            "--input-script",
            str(ROOT / "tests/input_scripts/adventure_party_3p_hub.txt"),
            "--rom",
            str(rom),
            "--window-size",
            "640x480",
            "--video-set",
            "Enhancements.AdventureParty=1",
        ],
        cwd=arm,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=600,
        check=False,
    )
    output = process.stdout or ""
    (arm / "run.log").write_text(output, encoding="utf-8")
    if process.returncode != 0:
        raise CrossFeatureError(f"D1 hub exited {process.returncode}")
    require_roster(output, 3)
    require_custom_rows(output, "hub", 3)
    for seat, donor in EXPECTED_CHARACTERS[3].items():
        marker = re.compile(
            rf"custom_character_minimap: player={seat} "
            rf"name={re.escape(DISPLAY_NAMES[donor])} revision=[1-9][0-9]* "
            rf"rgb={','.join(str(value) for value in MINIMAP_COLOURS[donor])}"
        )
        if marker.search(output) is None:
            raise CrossFeatureError(
                f"D1 hub missing minimap identity for seat {seat}"
            )
    require_clean_runtime(output, "D1 hub")
    return output


def run_taj(binary: Path, rom: Path, characters: Path, evidence: Path) -> str:
    output = taj.run_arm(
        str(binary),
        str(rom),
        "tests/input_scripts/adventure_party_3p_taj.txt",
        taj.TAJ_SEAT_ROUTE,
        frames=6000,
        extra_env=custom_environment(characters, 3),
    )
    (evidence / "d2-taj-transform.log").write_text(output, encoding="utf-8")
    failures, _ = taj.assert_transform_scene(output, 3, "D2-custom-taj")
    if failures:
        raise CrossFeatureError("; ".join(failures))
    require_custom_rows(output, "taj-transform", 3)
    require_clean_runtime(output, "D2 Taj transform")
    return output


def run_vehicle_fallback(
    binary: Path, rom: Path, characters: Path, evidence: Path
) -> str:
    values = custom_environment(characters, 3)
    values["MDKR_CUSTOM_CHARACTER_P1"] = CAR_ONLY_PACKAGE_ID
    output = taj.run_arm(
        str(binary),
        str(rom),
        "tests/input_scripts/adventure_party_3p_taj.txt",
        taj.TAJ_SEAT_ROUTE,
        frames=6000,
        extra_env=values,
    )
    (evidence / "d3-vehicle-fallback.log").write_text(
        output, encoding="utf-8"
    )
    failures, _ = taj.assert_transform_scene(output, 3, "D3-car-only-taj")
    if failures:
        raise CrossFeatureError("; ".join(failures))
    hub = custom_rows(output, "hub")
    transformed = custom_rows(output, "taj-transform")
    if hub.get(0) != (CAR_ONLY_PACKAGE_ID, 5, 5, 0, 1):
        raise CrossFeatureError(
            f"D3 car-only package was not active in the car hub: {hub.get(0)}"
        )
    if transformed.get(0) != (CAR_ONLY_PACKAGE_ID, 5, 5, 1, 0):
        raise CrossFeatureError(
            "D3 unsupported hovercraft did not fail closed to the donor: "
            f"{transformed.get(0)}"
        )
    # The product intentionally has no in-game warning here (R29): the roster
    # discloses vehicle coverage before selection.  `matches=0` is the direct
    # runtime predicate consumed by the draw path, so it is the deterministic
    # proof that the donor fallback is selected without mistaking the absence
    # of a presentation warning for the behavior itself.
    if match := BAD_RE.search(output):
        raise CrossFeatureError(f"D3 fatal marker {match.group(0)!r}")
    if not any(int(row[0]) > 0 for row in REPLACEMENT_RE.findall(output)):
        raise CrossFeatureError("D3 never rendered the compatible car appearance")
    return output


def run_real_select_duplicate_race(
    binary: Path, rom: Path, characters: Path, evidence: Path
) -> str:
    arm = evidence / "d4-d5-real-select-duplicate"
    save_dir = arm / "save"
    save_dir.mkdir(parents=True)
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_AUTOPILOT="1",
        MDKR_RENDERER="webgpu",
        MDKR_RENDER_SCALE="1",
        MDKR64_HIDDEN="1",
        MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
    )
    save_env(environment, str(save_dir))
    environment["MDKR_VIDEO_CONFIG_PATH"] = str(arm / "mdkr64.ini")
    process = subprocess.run(
        [
            str(binary),
            "--headless-frames",
            "4200",
            "--input-script",
            str(ROOT / "tests/input_scripts/custom_character_2p_duplicate_race.txt"),
            "--rom",
            str(rom),
            "--window-size",
            "640x480",
        ],
        cwd=arm,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=600,
        check=False,
    )
    output = process.stdout or ""
    (arm / "run.log").write_text(output, encoding="utf-8")
    if process.returncode != 0:
        raise CrossFeatureError(f"D4/D5 race exited {process.returncode}")
    selected = re.findall(
        r"custom_roster_select: controller=(\d+) race_player=(\d+) "
        r"package=([^ ]+) donor=(\d+)",
        output,
    )
    expected = {
        ("0", "0", CAR_ONLY_PACKAGE_ID, "5"),
        ("1", "1", CAR_ONLY_PACKAGE_ID, "5"),
    }
    if not expected.issubset(set(selected)):
        raise CrossFeatureError(
            f"D4/D5 real roster did not select one package twice: {selected}"
        )
    for player in range(2):
        marker = (
            f"custom_roster_commit: player={player} controller={player} "
            f"package={CAR_ONLY_PACKAGE_ID} donor=5"
        )
        if marker not in output:
            raise CrossFeatureError(f"D4/D5 missing commit for player {player}")
        assignment = (
            f"[modern-character] P{player + 1}={CAR_ONLY_PACKAGE_ID} donor=5"
        )
        if assignment not in output:
            raise CrossFeatureError(
                f"D4/D5 duplicate pool did not publish P{player + 1}"
            )
    if "level_load: levelId=5 numPlayers=1" not in output:
        raise CrossFeatureError("D4/D5 never reached a real two-player race")
    require_clean_runtime(output, "D4/D5 duplicate-donor race")
    return output


def run_party_challenge_flags(
    binary: Path, rom: Path, characters: Path, evidence: Path
) -> str:
    challenge.SAVE_ORDER = challenge.save_order(str(rom))
    output, _save, returncode = challenge.run(
        str(binary),
        str(rom),
        script=challenge.ADMIT[3],
        enabled=True,
        frames=7000,
        values={
            **custom_environment(characters, 3),
            "MDKR_CHALLENGE_OUTCOME": "loss",
        },
        timeout=1000,
    )
    (evidence / "d9-party-challenge-flags.log").write_text(
        output, encoding="utf-8"
    )
    if returncode != 0:
        raise CrossFeatureError(f"D9 challenge exited {returncode}")
    rows = {
        int(player): (int(character), texture, source, package)
        for player, character, texture, _identity, source, package
        in FLAG_RE.findall(output)
    }
    if set(rows) != {0, 1, 2, 3}:
        raise CrossFeatureError(
            f"D9 did not bind all four collection-arena flag quads: {rows}"
        )
    if rows[0] != (9, "ok", "package-card", PACKAGE_IDS[9]):
        raise CrossFeatureError(f"D9 host flag lost its package portrait: {rows[0]}")
    if any(rows[player][1] != "ok" for player in (1, 2, 3)):
        raise CrossFeatureError(f"D9 an AI flag texture was missing: {rows}")
    failures: list[str] = []
    challenge.base_fail("D9", output, returncode, failures)
    challenge.assert_suspend("D9", output, failures, 3)
    if failures:
        raise CrossFeatureError("; ".join(failures))
    require_clean_runtime(output, "D9 party-origin challenge flags")
    return output


def run_party_taj_custom_mix(
    binary: Path, rom: Path, characters: Path, evidence: Path
) -> str:
    arm = evidence / "d10-party-taj-custom"
    save_dir = arm / "save"
    save_dir.mkdir(parents=True)
    (save_dir / "eeprom.bin").write_bytes(eeprom_image())
    (save_dir / "taj_mod_state.ini").write_text(
        "mod_roster_version=3\n"
        "taj_unlocked=1\n"
        "taj_migration_complete=1\n"
        "wizpig_unlocked=0\n"
        "wizpig_migration_complete=0\n"
        "terry_unlocked=0\n"
        "terry_migration_complete=0",
        encoding="ascii",
    )
    environment = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_TRACE="1",
        MDKR_RENDERER="webgpu",
        MDKR_RENDER_SCALE="1",
        MDKR64_HIDDEN="1",
        MDKR_CUSTOM_CHARACTER_DIRECTORY=str(characters),
        MDKR_CUSTOM_CHARACTER_P1=PACKAGE_IDS[5],
    )
    save_env(environment, str(save_dir))
    environment["MDKR_VIDEO_CONFIG_PATH"] = str(arm / "mdkr64.ini")
    process = subprocess.run(
        [
            str(binary),
            "--headless-frames",
            "4300",
            "--input-script",
            str(ROOT / "tests/input_scripts/adventure_party_2p_taj_custom.txt"),
            "--rom",
            str(rom),
            "--window-size",
            "640x480",
            "--video-set",
            "Enhancements.AdventureParty=1",
        ],
        cwd=arm,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=700,
        check=False,
    )
    output = process.stdout or ""
    (arm / "run.log").write_text(output, encoding="utf-8")
    if process.returncode != 0:
        raise CrossFeatureError(f"D10 party/Taj/custom exited {process.returncode}")
    if (
        "mod_racer_select: player=0 controller=0 identity=0 donor=5"
        not in output
        or "mod_racer_select: player=1 controller=1 identity=1 donor=9"
        not in output
    ):
        raise CrossFeatureError("D10 did not commit the custom + playable-Taj mix")
    expected_roster = {0: 5, 1: 9}
    if not any(
        int(match.group(1)) == 2
        and {
            int(token.split("=")[0][1:]): int(token.split("=")[1])
            for token in match.group(3).split()
        }
        == expected_roster
        for match in ROSTER_RE.finditer(output)
    ):
        raise CrossFeatureError("D10 party donor roster did not preserve Taj identity")
    row = custom_rows(output, "hub").get(0)
    if row is None or row[:3] != (PACKAGE_IDS[5], 5, 5) or row[4] != 1:
        raise CrossFeatureError(f"D10 custom seat was not live beside Taj: {row}")
    if "taj_minimap: identity=taj rgb=255,0,255 player=1" not in output:
        raise CrossFeatureError(
            "D10 playable Taj identity did not reach the live party HUD"
        )
    require_clean_runtime(output, "D10 party + Taj + custom")
    return output


def run_boss_restore(
    binary: Path, rom: Path, characters: Path, evidence: Path
) -> str:
    boss.SAVE_ORDER = boss.save_order(str(rom))
    values = custom_environment(characters, 3)
    values["MDKR_BOSS_WIN"] = "1"
    output, _save, returncode = boss.run(
        str(binary), str(rom), frames=12000, values=values, timeout=1600
    )
    (evidence / "d6-boss-restore.log").write_text(output, encoding="utf-8")
    if returncode != 0:
        raise CrossFeatureError(f"D6 boss restore exited {returncode}")
    failures: list[str] = []
    boss.assert_suspend_restore("D6-custom-boss", output, failures)
    if failures:
        raise CrossFeatureError("; ".join(failures))
    restore = next((match for match in RESTORE_RE.finditer(output)
                    if int(match.group(3)) == 1), None)
    if restore is None:
        raise CrossFeatureError("D6 boss emitted no successful party restore")
    tail = output[restore.end():]
    require_custom_rows(tail, "hub", 3)
    require_clean_runtime(output, "D6 boss restore")
    return output


def run_save_equivalence(
    binary: Path, rom: Path, characters: Path, evidence: Path
) -> str:
    progress.ROOT_ROM = str(rom)
    common = {
        "MDKR_AP_RACE_WINNER": "0",
        "MDKR_FORCE_LAPS": "1",
        "MDKR_TEST_POSTRACE_OPTION": "1",
    }
    custom_values = dict(common)
    custom_values.update(custom_environment(characters, 2))
    custom_out, custom_save, custom_rc = progress.run(
        str(binary),
        str(rom),
        script=progress.ADMIT[2],
        enabled=True,
        frames=8500,
        values=custom_values,
        timeout=900,
    )
    plain_out, plain_save, plain_rc = progress.run(
        str(binary),
        str(rom),
        script=progress.ADMIT[2],
        enabled=True,
        frames=8500,
        values=dict(common),
        timeout=900,
    )
    (evidence / "d7-custom-win.log").write_text(custom_out, encoding="utf-8")
    (evidence / "d7-plain-win.log").write_text(plain_out, encoding="utf-8")
    failures: list[str] = []
    progress.base_fail("D7-custom", custom_out, custom_rc, failures)
    progress.base_fail("D7-plain", plain_out, plain_rc, failures)
    progress.assert_party_win("D7-custom", custom_out, custom_save, 0, 2,
                              failures)
    progress.assert_party_win("D7-plain", plain_out, plain_save, 0, 2,
                              failures)
    if failures:
        raise CrossFeatureError("; ".join(failures))
    if custom_save is None or plain_save is None:
        raise CrossFeatureError("D7 produced no comparable EEPROM images")
    if custom_save[:SLOT_BYTES] != plain_save[:SLOT_BYTES]:
        differences = [
            index for index in range(SLOT_BYTES)
            if custom_save[index] != plain_save[index]
        ]
        raise CrossFeatureError(
            f"D7 custom presentation changed save bytes {differences}"
        )
    require_custom_rows(custom_out, "race", 2)
    require_clean_runtime(custom_out, "D7 custom party win")
    return custom_out


def positive_controls(hub_output: str, taj_output: str) -> None:
    broken_hub = re.sub(r"matches=1", "matches=0", hub_output, count=1)
    try:
        require_custom_rows(broken_hub, "hub", 3)
    except CrossFeatureError:
        pass
    else:
        raise CrossFeatureError(
            "positive control: donor/package mismatch passed D1 assertions"
        )
    broken_taj = "\n".join(
        line for line in taj_output.splitlines()
        if "phase=taj-transform seat=1" not in line
    )
    try:
        require_custom_rows(broken_taj, "taj-transform", 3)
    except CrossFeatureError:
        pass
    else:
        raise CrossFeatureError(
            "positive control: missing transformed seat passed D2 assertions"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    binary = Path(resolve_binary(args.build)).resolve()
    rom = args.rom.resolve()
    if not binary.is_file() or not rom.is_file():
        print(
            "check_adventure_party_custom_characters: FAIL -- missing binary or ROM",
            file=sys.stderr,
        )
        return 2
    temporary: tempfile.TemporaryDirectory[str] | None = None
    if args.evidence_dir is None:
        temporary = tempfile.TemporaryDirectory(prefix="mdkr-ap-custom-")
        evidence = Path(temporary.name)
    else:
        evidence = args.evidence_dir.resolve()
        evidence.mkdir(parents=True, exist_ok=True)
    try:
        characters = install_fixtures(evidence)
        hub_output = run_hub(binary, rom, characters, evidence)
        taj_output = run_taj(binary, rom, characters, evidence)
        run_vehicle_fallback(binary, rom, characters, evidence)
        run_real_select_duplicate_race(binary, rom, characters, evidence)
        run_boss_restore(binary, rom, characters, evidence)
        run_save_equivalence(binary, rom, characters, evidence)
        run_party_challenge_flags(binary, rom, characters, evidence)
        run_party_taj_custom_mix(binary, rom, characters, evidence)
        positive_controls(hub_output, taj_output)
    except (
        OSError,
        subprocess.SubprocessError,
        CrossFeatureError,
        probe.ProbeError,
        manager.ManagerError,
    ) as error:
        print(
            f"check_adventure_party_custom_characters: FAIL -- {error}",
            file=sys.stderr,
        )
        print(f"evidence: {evidence}", file=sys.stderr)
        return 1
    print(
        "check_adventure_party_custom_characters: PASS -- D1 party admission "
        "kept donor-only authority with three live package identities; D2 Taj "
        "rebuild and D6 host-solo boss restoration retained every package; D7 "
        "custom presentation produced byte-identical party progress; D3-D5 "
        "vehicle fallback, duplicate donors, and a real 2P custom race held; "
        "D9 all four challenge flag quads and D10 a custom/Taj party held; "
        "two positive controls fired"
    )
    if args.evidence_dir is not None:
        print(f"evidence: {evidence}")
    if temporary is not None:
        temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
