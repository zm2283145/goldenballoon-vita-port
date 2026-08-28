#!/usr/bin/env python3
"""AP-06 focused route: Adventure Party menu admission and session lifecycle.

What this proves
----------------
With ``Enhancements.AdventureParty`` ON and two-to-four controllers joined in
Character Select, the party takes the ordinary Adventure route the retail game
denies them:

    CHARACTER_SELECT(3) -> GAME_SELECT(19) -> Adventure -> FILE_SELECT(6)
    -> campaign load

and at file entry a session is formed (``aparty_session`` FORMING then
ACTIVE_LOBBY, with the joined seat/character roster). Retail stops a 3- or
4-player Adventure selection at TRACK_SELECT(15) — the JOINTVENTURE offset admits
at most two — so reaching GAME_SELECT with three or four seats *is* the party
path, not the retail two-player one. The retail two-player globals are never
engaged: no JOINTVENTURE magic code is submitted and the session trace is the
only thing that formed.

With the enhancement OFF, three/four players route to TRACK_SELECT exactly as
stock and no ``aparty_`` line ever appears. One player is unchanged in both arms
and never forms a party.

Positive controls (mutating the route/env, never the sources)
-------------------------------------------------------------
* Admission clamped to two: the OFF-arm 3-player run (which routes to Tracks, the
  behaviour a clamped-to-two admission would produce) must FAIL the ON-arm
  assertions — otherwise the gate could not tell admission from non-admission.
* A missing session trace: an ON-arm run with its ``aparty_session`` lines
  removed must FAIL the ON-arm assertions — otherwise the gate could not tell a
  formed session from an unformed one.

Save fixture provenance
-----------------------
This check writes its own EEPROM image: a started, checksum-valid Adventure One
save in slot 0, built with the shared ``harness_utils`` bit-stream encoders
(``save_layout.h``: 3 x SaveFile(40) | SaveConfig(8) | 2 x CourseRecords(192)).
It is the same slot shape ``tests/check_adventure_two.py`` resumes on its
Adventure One arm, minus its progress, so the host's single FILE_SELECT confirm
RESUMES an existing file rather than starting a new game — the new-game
shared-scene envelope is AP-11, deliberately out of scope here. No developer save
is read or written; every run uses a private temporary directory.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from harness_utils import (
    DEFAULT_BUILD_DIR,
    config_block,
    resolve_binary,
    save_env,
    seal_slot,
    put_bits,
)

ROOT = Path(__file__).resolve().parent.parent

# Menu IDs from game/src/menu.h (0-indexed enum MENU_ID).
MENU_CHARACTER_SELECT = 3
MENU_FILE_SELECT = 6
MENU_TRACK_SELECT = 15
MENU_GAME_SELECT = 19
MENU_CAUTION = 28

ON_FRAMES = 4200
OFF_FRAMES = 2600

MENU_RE = re.compile(r"menu_init: menuId=(\d+) @frame~(\d+)")
LEVEL_RE = re.compile(r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")
SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)"
)
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)((?: c\d+=\d+)*)")
# JOINTVENTURE is magic code id 24 (see tests/check_taj_p2_adventure.py).
JOINTVENTURE_RE = re.compile(r"magic_code_submit: accepted=1 id=24")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)

EEPROM_BYTES = 512
SLOT_BYTES = 40
CONFIG_OFFSET = 120
RECORD_OFFSETS = (128, 320)
RECORD_BYTES = 192


@dataclass(frozen=True)
class SessionLine:
    state: str
    sgen: int
    lgen: int
    host: int


@dataclass(frozen=True)
class RosterLine:
    n: int
    mask: int
    characters: dict[int, int]


def started_adventure_one_slot() -> bytes:
    """A started, checksum-valid Adventure One save with no campaign progress."""
    bits: list[int] = []
    put_bits(bits, 16, 0)       # checksum, sealed below
    put_bits(bits, 68, 0)       # per-course flags
    put_bits(bits, 6, 0)        # Taj flags
    put_bits(bits, 10, 0)       # trophies
    put_bits(bits, 12, 0)       # bosses
    for _ in range(6):
        put_bits(bits, 7, 0)    # total + five world balloon counts
    put_bits(bits, 3, 0)        # TT amulet
    put_bits(bits, 3, 0)        # Wizpig amulet
    for _ in range(6):
        put_bits(bits, 16, 0)   # world door flags
    put_bits(bits, 8, 0)        # keys
    put_bits(bits, 32, 0)       # cutscene flags (0 == Adventure One, not Two)
    put_bits(bits, 16, 0x1234)  # a named/started file
    put_bits(bits, 8, 0)        # pad
    if len(bits) != SLOT_BYTES * 8:
        raise AssertionError(f"slot builder emitted {len(bits)} bits")
    out = bytearray(
        int("".join(str(bit) for bit in bits[i:i + 8]), 2)
        for i in range(0, len(bits), 8)
    )
    return bytes(seal_slot(out))


def eeprom_image() -> bytes:
    out = bytearray(EEPROM_BYTES)
    out[:SLOT_BYTES] = started_adventure_one_slot()
    out[SLOT_BYTES:CONFIG_OFFSET] = b"\xFF" * (CONFIG_OFFSET - SLOT_BYTES)
    # Adventure Two unlocked + retail subtitle bit: the same valid config
    # check_adventure_two.py's resume arms run on. Adventure One stays option 0.
    out[CONFIG_OFFSET:CONFIG_OFFSET + 8] = config_block((1 << 25) | 1)
    for offset in RECORD_OFFSETS:
        out[offset:offset + RECORD_BYTES] = bytes(seal_slot(bytearray(RECORD_BYTES)))
    return bytes(out)


def run_arm(binary: str, rom: str, script: str, enabled: bool,
            frames: int, verbose: bool) -> str:
    """One headless run of ``script`` with the enhancement on/off; return the log."""
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_admit_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1")
        save_env(env, str(save_dir))
        command = [
            binary, "--headless-frames", str(frames),
            "--input-script", str(ROOT / script), "--rom", rom,
            "--window-size", "640x480",
            "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}",
        ]
        if verbose:
            print(f"$ ({'on' if enabled else 'off'}) {' '.join(command)}", flush=True)
        proc = subprocess.run(command, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=max(180, frames // 15), check=False)
    return proc.stdout or ""


def menu_route(output: str) -> list[tuple[int, int]]:
    return [(int(m.group(1)), int(m.group(2))) for m in MENU_RE.finditer(output)]


def session_lines(output: str) -> list[SessionLine]:
    return [
        SessionLine(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)))
        for m in SESSION_RE.finditer(output)
    ]


def roster_lines(output: str) -> list[RosterLine]:
    rosters: list[RosterLine] = []
    for m in ROSTER_RE.finditer(output):
        chars = {
            int(k[1:]): int(v)
            for k, v in (tok.split("=") for tok in m.group(3).split())
        }
        rosters.append(RosterLine(int(m.group(1)), int(m.group(2), 16), chars))
    return rosters


def check_on_arm(output: str, players: int) -> list[str]:
    """The ON-arm contract for a 2/3/4-player party. Returns failures."""
    failures: list[str] = []
    label = f"{players}P on"

    if bad := BAD_RE.search(output):
        failures.append(f"{label}: fatal marker {bad.group(0)!r}")

    route = menu_route(output)
    char_frames = [frame for menu_id, frame in route if menu_id == MENU_CHARACTER_SELECT]
    if not char_frames:
        failures.append(f"{label}: never reached CHARACTER_SELECT; route={route}")
        char_frame = -1
    else:
        char_frame = char_frames[0]

    game_frames = [
        frame for menu_id, frame in route
        if menu_id == MENU_GAME_SELECT and frame > char_frame
    ]
    if not game_frames:
        failures.append(
            f"{label}: never admitted to GAME_SELECT({MENU_GAME_SELECT}); route={route}")
    # A party must NOT be diverted to Tracks after character select.
    if any(menu_id == MENU_TRACK_SELECT and frame > char_frame
           for menu_id, frame in route):
        failures.append(
            f"{label}: routed to TRACK_SELECT({MENU_TRACK_SELECT}) instead of "
            f"admitting the party; route={route}")

    file_frames = [
        frame for menu_id, frame in route
        if menu_id == MENU_FILE_SELECT and frame > char_frame
    ]
    if not file_frames:
        failures.append(
            f"{label}: never reached FILE_SELECT({MENU_FILE_SELECT}); route={route}")
        file_frame = 1 << 30
    else:
        file_frame = file_frames[0]

    # The retail two-player JOINTVENTURE offset must not be the mechanism.
    if JOINTVENTURE_RE.search(output):
        failures.append(f"{label}: JOINTVENTURE magic code was engaged")

    # Session lifecycle at file entry: FORMING then ACTIVE_LOBBY, one session.
    sessions = session_lines(output)
    forming = [s for s in sessions if s.state == "FORMING"]
    lobby = [s for s in sessions if s.state == "ACTIVE_LOBBY"]
    if not forming:
        failures.append(f"{label}: no aparty_session FORMING; sessions={sessions}")
    else:
        s = forming[0]
        if (s.sgen, s.lgen, s.host) != (1, 0, 0):
            failures.append(
                f"{label}: FORMING session was sgen={s.sgen} lgen={s.lgen} "
                f"host={s.host}, expected 1/0/0")
    if not lobby:
        failures.append(f"{label}: no aparty_session ACTIVE_LOBBY; sessions={sessions}")
    else:
        s = lobby[0]
        if (s.sgen, s.lgen, s.host) != (1, 1, 0):
            failures.append(
                f"{label}: ACTIVE_LOBBY session was sgen={s.sgen} lgen={s.lgen} "
                f"host={s.host}, expected 1/1/0")

    rosters = roster_lines(output)
    if not rosters:
        failures.append(f"{label}: no aparty_roster line")
    else:
        r = rosters[0]
        expected_mask = (1 << players) - 1
        if r.n != players:
            failures.append(f"{label}: roster n={r.n}, expected {players}")
        if r.mask != expected_mask:
            failures.append(
                f"{label}: roster mask=0x{r.mask:x}, expected 0x{expected_mask:x}")
        if sorted(r.characters) != list(range(players)):
            failures.append(
                f"{label}: roster seats {sorted(r.characters)}, expected "
                f"{list(range(players))} (dense from host seat 0)")

    # The campaign actually loads after file entry (the session is ACTIVE_LOBBY).
    load_frames = [int(m.group(3)) for m in LEVEL_RE.finditer(output)]
    if not any(frame > file_frame for frame in load_frames):
        failures.append(
            f"{label}: no campaign level_load after FILE_SELECT; loads={load_frames}")

    return failures


def check_off_arm(output: str, players: int) -> list[str]:
    """The OFF-arm contract for a 3/4-player selection. Returns failures."""
    failures: list[str] = []
    label = f"{players}P off"
    if bad := BAD_RE.search(output):
        failures.append(f"{label}: fatal marker {bad.group(0)!r}")
    route = menu_route(output)
    ids = [menu_id for menu_id, _ in route]
    if MENU_TRACK_SELECT not in ids:
        failures.append(
            f"{label}: did not route to TRACK_SELECT({MENU_TRACK_SELECT}) as "
            f"stock; route={route}")
    if MENU_GAME_SELECT in ids:
        failures.append(
            f"{label}: reached GAME_SELECT({MENU_GAME_SELECT}) with the "
            f"enhancement off; route={route}")
    if "aparty_" in output:
        line = next(ln for ln in output.splitlines() if "aparty_" in ln)
        failures.append(f"{label}: aparty_ trace with enhancement off: {line.strip()}")
    return failures


def check_one_player(output: str) -> list[str]:
    """1P Adventure is unchanged and forms no party even with the enhancement on."""
    failures: list[str] = []
    label = "1P on"
    if bad := BAD_RE.search(output):
        failures.append(f"{label}: fatal marker {bad.group(0)!r}")
    ids = [menu_id for menu_id, _ in menu_route(output)]
    if MENU_GAME_SELECT not in ids:
        failures.append(f"{label}: 1P did not reach GAME_SELECT; menus={ids}")
    if "aparty_" in output:
        failures.append(f"{label}: a party formed for a single player")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    scripts = {
        1: "tests/input_scripts/adventure_party_1p_admit.txt",
        2: "tests/input_scripts/adventure_party_2p_admit.txt",
        3: "tests/input_scripts/adventure_party_3p_admit.txt",
        4: "tests/input_scripts/adventure_party_4p_admit.txt",
    }
    required = [binary, rom, *(str(ROOT / s) for s in scripts.values())]
    missing = [p for p in required if not os.path.exists(p)]
    if missing:
        for p in missing:
            print(f"check_adventure_party_admission: FAIL -- missing {p}",
                  file=sys.stderr)
        return 1

    failures: list[str] = []

    # --- ON arm: 2/3/4 players admitted and a session formed. ---
    on_outputs: dict[int, str] = {}
    for players in (2, 3, 4):
        out = run_arm(binary, rom, scripts[players], True, ON_FRAMES, args.verbose)
        on_outputs[players] = out
        failures.extend(check_on_arm(out, players))

    # --- OFF arm: 3/4 players route to Tracks, no party. ---
    off_outputs: dict[int, str] = {}
    for players in (3, 4):
        out = run_arm(binary, rom, scripts[players], False, OFF_FRAMES, args.verbose)
        off_outputs[players] = out
        failures.extend(check_off_arm(out, players))

    # --- 1P control (enhancement on): unchanged, no party. ---
    one = run_arm(binary, rom, scripts[1], True, ON_FRAMES, args.verbose)
    failures.extend(check_one_player(one))

    # --- Positive control 1: admission clamped to two (== the OFF 3P route)
    #     must FAIL the ON-arm assertions. ---
    clamp_failures = check_on_arm(off_outputs[3], 3)
    if not clamp_failures:
        failures.append(
            "positive control (admission clamped to 2): the ON-arm assertions "
            "passed on a 3P run that never admitted the party — the gate cannot "
            "distinguish admission from Tracks routing")

    # --- Positive control 2: a missing session trace must FAIL the ON-arm
    #     assertions. ---
    stripped = "\n".join(
        ln for ln in on_outputs[3].splitlines() if "aparty_session" not in ln)
    strip_failures = check_on_arm(stripped, 3)
    if not strip_failures:
        failures.append(
            "positive control (missing session trace): the ON-arm assertions "
            "passed with every aparty_session line removed — the gate does not "
            "actually require a formed session")

    if failures:
        print("check_adventure_party_admission: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("check_adventure_party_admission: PASS -- 2/3/4-player parties take the "
          "ordinary Adventure route and form a session; 1P and the off arm are "
          "stock; both positive controls fired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
