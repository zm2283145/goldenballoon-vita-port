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

Two file-authority facts are also asserted here:

* R26 new-game refusal. A party formation cannot begin a NEW campaign yet (the
  new-game shared-scene envelope is AP-11), so confirming an UN-STARTED file is
  fail-closed refused (``aparty_file_refused: reason=newgame``, cursor stays, no
  session, no campaign load) instead of silently collapsing to a 1P new game;
  the STARTED fixture file then confirms and forms the session normally.
* FIX 1 host-only copy/erase authority. ``fileselect_input_copy`` /
  ``fileselect_input_erase`` skip player-two aggregation in a party exactly as
  ``fileselect_input_root`` does. That guard is token-identical to the ROOT guard
  the ON arm already exercises behaviourally; a headless copy/erase confirm drive
  with a null-effect oracle would need gFileConfirm observability the build does
  not emit, so this is a source-level guard-presence assertion (see
  ``check_copy_erase_guard``).

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
LAYOUT_RE = re.compile(r"aparty_layout: viewports=(\d+) layout=(\d+)")
# JOINTVENTURE is magic code id 24 (see tests/check_taj_p2_adventure.py).
JOINTVENTURE_RE = re.compile(r"magic_code_submit: accepted=1 id=24")
# R26 interim refusal: confirming an UN-STARTED file in a party fails closed.
REFUSE_RE = re.compile(r"aparty_file_refused: reason=(\w+) slot=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed"
)

EEPROM_BYTES = 512
SLOT_BYTES = 40
CONFIG_OFFSET = 120
RECORD_OFFSETS = (128, 320)
RECORD_BYTES = 192

# Expected FORM roster: the exact donor character identity (voice ID) each seat
# records, derived from the fixed cursor selections the adventure_party_{N}p_admit
# scripts confirm. Seat 0 is the host (player one), who starts on Diddy (voice ID
# 9); each joined pad lands deterministically on the next grid character it is
# offered. Captured from the FORM roster trace and reproduced here so a FORM
# adapter that recorded, say, every seat as 0 would FAIL rather than pass on a
# seat-key-only check. If Character Select layout/defaults change, re-derive these
# from the aparty_roster line (n= mask= c<seat>=<voice>).
EXPECTED_CHARACTERS = {
    2: {0: 9, 1: 0},
    3: {0: 9, 1: 0, 2: 1},
    4: {0: 9, 1: 0, 2: 1, 3: 5},
}


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


def started_adventure_one_slot(taj_flags: int = 0) -> bytes:
    """A started, checksum-valid Adventure One save with no campaign progress.

    ``taj_flags`` seeds the 6-bit Taj-challenge field (structs.h:
    0x01/0x02/0x04 = car/hover/plane unlocked, 0x08/0x10/0x20 = completed). It is
    0 by default (no challenge offered), which every caller but the Taj-challenge
    gate relies on; that gate passes ``TAJ_FLAGS_CAR_CHAL_UNLOCKED`` so the hub
    Taj ROOT menu exposes the CHALLENGES row the R27 refusal guards.
    """
    bits: list[int] = []
    put_bits(bits, 16, 0)       # checksum, sealed below
    put_bits(bits, 68, 0)       # per-course flags
    put_bits(bits, 6, taj_flags & 0x3F)  # Taj flags
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


def eeprom_image(taj_flags: int = 0) -> bytes:
    out = bytearray(EEPROM_BYTES)
    out[:SLOT_BYTES] = started_adventure_one_slot(taj_flags)
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
        expected_chars = EXPECTED_CHARACTERS[players]
        if r.characters != expected_chars:
            failures.append(
                f"{label}: roster characters {r.characters}, expected "
                f"{expected_chars} (exact donor identity per seat, host seat 0 = "
                f"Diddy/9)")

    # The campaign actually loads after file entry (the session is ACTIVE_LOBBY).
    load_frames = [int(m.group(3)) for m in LEVEL_RE.finditer(output)]
    if not any(frame > file_frame for frame in load_frames):
        failures.append(
            f"{label}: no campaign level_load after FILE_SELECT; loads={load_frames}")

    # Post-AP-08 expectation (was: the hub still rendered as stock 1P): the party
    # hub now expands its roster to N split-screen viewports. This gate proves
    # admission + session; the full N-racer/HUD/binding proof is
    # check_adventure_party_hub.py.
    layouts = [(int(m.group(1)), int(m.group(2))) for m in LAYOUT_RE.finditer(output)]
    if (players, players - 1) not in layouts:
        failures.append(
            f"{label}: party hub did not expand to {players} viewports "
            f"(no aparty_layout viewports={players}); saw {layouts}")

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


def check_newgame_refuse_arm(output: str, players: int) -> list[str]:
    """R26: a party host confirming an UN-STARTED file is refused, then the
    started fixture file confirms and forms the session normally.

    The script drives the host to an EMPTY slot and confirms it (refused, no
    session, no campaign load), then back to the started slot 0 and confirms it.
    Returns failures.
    """
    failures: list[str] = []
    label = f"{players}P newgame-refuse"

    if bad := BAD_RE.search(output):
        failures.append(f"{label}: fatal marker {bad.group(0)!r}")

    # The empty-file confirm is refused, and it is OBSERVABLE.
    refusals = list(REFUSE_RE.finditer(output))
    if not refusals:
        failures.append(
            f"{label}: no aparty_file_refused diagnostic — an empty-file confirm "
            f"was not fail-closed refused")
        refuse_pos = 1 << 30
    else:
        refuse_pos = refusals[0].start()
        if refusals[0].group(1) != "newgame":
            failures.append(
                f"{label}: refusal reason was {refusals[0].group(1)!r}, "
                f"expected 'newgame'")

    # The empty-file confirm forms NO session (exactly one FORMING, from the
    # later started confirm) and starts NO campaign load before the refusal.
    sessions = session_lines(output)
    forming = [s for s in sessions if s.state == "FORMING"]
    if len(forming) != 1:
        failures.append(
            f"{label}: expected exactly ONE FORMING session (the empty confirm "
            f"forms none, the started confirm forms one), saw {len(forming)}: "
            f"{[s.state for s in sessions]}")

    # The session only forms AFTER the refusal (i.e. from the started confirm),
    # never from the refused empty confirm. (Only aparty_session lines are
    # position-compared; the many pre-menu attract/title level_loads make a raw
    # level_load position meaningless, so the campaign-load-after-file-select
    # proof is delegated to check_on_arm's frame-aware assertion below.)
    session_pos = [m.start() for m in SESSION_RE.finditer(output)]
    if session_pos and min(session_pos) < refuse_pos:
        failures.append(
            f"{label}: an aparty_session formed BEFORE the refusal — the empty "
            f"confirm was not fully fail-closed")

    # After the refusal the party proceeds EXACTLY as the ordinary ON arm: it
    # forms the session on the started fixture file and loads the campaign (the
    # campaign level_load is asserted to happen after FILE_SELECT there).
    failures.extend(check_on_arm(output, players))
    return failures


def check_copy_erase_guard() -> list[str]:
    """FIX 1 guard-presence assertion (source-level).

    Driving a headless party into the COPY/ERASE confirmation dance and then
    OBSERVING a null player-two effect would need new gFileConfirm/gFileCopy
    observability the build does not emit, which is disproportionate for a guard
    that is TOKEN-IDENTICAL to the behaviourally-proven fileselect_input_root
    guard (Task 6 Adapter 2, exercised by this gate's ON arm — the party forms
    its session through player one's file confirm with player two never
    aggregated). So instead this asserts, at the source, that
    fileselect_input_copy and fileselect_input_erase each wrap their
    ``gNumberOfActivePlayers == 2`` player-two aggregation body in the same
    ``if (!adventure_party_menu_admits())`` party guard under
    ``NATIVE_PORT && !MDKR_ADVENTURE_PARTY_OMIT``. A regression that drops either
    guard (letting a non-host drive the party's copy/erase decision) fails here.
    """
    failures: list[str] = []
    menu_c = ROOT / "game" / "src" / "menu.c"
    try:
        text = menu_c.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        return [f"copy/erase guard: cannot read {menu_c}: {exc}"]

    for func in ("fileselect_input_copy", "fileselect_input_erase"):
        # Anchor on the DEFINITION (`void <func>(`), not the earlier call site in
        # menu_file_select_loop, whose window would otherwise catch an unrelated
        # gNumberOfActivePlayers == 2 line.
        start = text.find("void " + func + "(")
        if start < 0:
            failures.append(f"copy/erase guard: {func} definition not found in menu.c")
            continue
        # Bound the search to this function body (up to the next function or EOF).
        body = text[start:start + 4000]
        two_player = body.find("gNumberOfActivePlayers == 2")
        if two_player < 0:
            failures.append(
                f"copy/erase guard: {func} lost its player-two aggregation block")
            continue
        # Wide enough to span the count line, the guard comment, and the guard
        # itself (the copy comment is long); no other count comparison lives in
        # either function within this span, so it cannot false-pass.
        window = body[two_player:two_player + 900]
        if "adventure_party_menu_admits()" not in window:
            failures.append(
                f"copy/erase guard: {func}'s player-two aggregation is NOT wrapped "
                f"in the adventure_party_menu_admits() party guard — a non-host "
                f"could drive the party's file copy/erase decision")
        elif "!adventure_party_menu_admits()" not in window:
            failures.append(
                f"copy/erase guard: {func} references the party predicate but not "
                f"as the skip guard `if (!adventure_party_menu_admits())`")
        if "MDKR_ADVENTURE_PARTY_OMIT" not in window:
            failures.append(
                f"copy/erase guard: {func}'s party guard is not behind "
                f"NATIVE_PORT && !MDKR_ADVENTURE_PARTY_OMIT (OMIT must be retail)")
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
    refuse_script = "tests/input_scripts/adventure_party_3p_newgame_refuse.txt"
    required = [binary, rom, str(ROOT / refuse_script),
                *(str(ROOT / s) for s in scripts.values())]
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

    # --- R26 new-game refusal arm: a 3P party's empty-file confirm is refused
    #     (no session, no campaign load, refusal observable), then the started
    #     fixture file confirms and proceeds normally. ---
    refuse_out = run_arm(binary, rom, refuse_script, True, ON_FRAMES, args.verbose)
    failures.extend(check_newgame_refuse_arm(refuse_out, 3))

    # --- FIX 1 copy/erase host-only file authority: source guard-presence. ---
    failures.extend(check_copy_erase_guard())

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

    # --- Positive control 3: a missing refusal diagnostic must FAIL the R26
    #     new-game refusal arm (otherwise the gate cannot tell a fail-closed
    #     refusal from a silent new-game collapse). ---
    refuse_stripped = "\n".join(
        ln for ln in refuse_out.splitlines() if "aparty_file_refused" not in ln)
    if not check_newgame_refuse_arm(refuse_stripped, 3):
        failures.append(
            "positive control (missing refusal diagnostic): the R26 refusal arm "
            "passed with every aparty_file_refused line removed — the gate does "
            "not actually require the empty-file confirm to be refused")

    if failures:
        print("check_adventure_party_admission: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("check_adventure_party_admission: PASS -- 2/3/4-player parties take the "
          "ordinary Adventure route and form a session; a party's UN-STARTED file "
          "confirm is fail-closed refused (R26) then the started file proceeds; "
          "copy/erase keep host-only file authority (FIX 1 guard present); 1P and "
          "the off arm are stock; three positive controls fired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
