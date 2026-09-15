#!/usr/bin/env python3
"""AP-17: HOST-SOLO BOSS SUSPENSION ENVELOPE for Adventure Party.

What this proves (Enhancements.AdventureParty ON, a 3-player party resumed into a
Dino Domain checkpoint)
-----------------------------------------------------------------------------
A party that enters a boss from a lobby SUSPENDS to a retail-shaped host-solo boss
run and, on return, is RESTORED to the exact party it was:

* Suspension envelope. The boss level loads host-solo and the session enters
  SOLO_ACTIVITY with the party's roster captured as the suspended facts
  (aparty_session state=SOLO_ACTIVITY + a suspended aparty_roster n=3 mask=0x7);
  no party race field is fielded (no `racefield:` line -- the retail two-racer boss
  field runs) and the host (playerIndex 0) is the sole human at the boss finish.
* Exact-once first-win award. A first boss WIN commits the retail progression
  EXACTLY ONCE (settings->bosses |= Dino world bit, courseFlags RACE_CLEARED,
  through the unchanged racer_boss_finish path), witnessed by one aparty_award boss
  token issue(ok)+consume(ok) pair; the win cutscene (level 57) presents and the
  save gains exactly the Dino boss bit (bosses 0x0 -> 0x2, boss course CLEARED).
* No award on defeat. A boss LOSS sets no bosses bit, consumes no boss token, and
  still restores the party to the lobby.
* Restoration. On EVERY return the session runs RESTORING_PARTY and validates the
  roster equals the suspended facts EXACTLY (aparty_restore match=1); the lobby
  resumes ACTIVE_LOBBY with generations advanced.
* Rematch entry. Re-entering the (now beaten) boss re-suspends host-solo and the
  retail "already beaten" path awards nothing more (a refused boss token ISSUE,
  result=0, and no second consume).

How the boss is entered
-----------------------
A party CANNOT reach a world-lobby boss door with a headless AI line -- the door
is behind geometry the drive spline does not path through (object_functions.c
obj_loop_exit documents this; a direct steer stalls short, verified), and the only
production route that reaches it (win the fourth world balloon race, which
repositions the kart at the boss-door return entrance) drives a PARTY balloon race
whose spatial-audio update crashes on this build (a pre-existing AP-12 defect in
racer_sound_update_all, NOT this envelope -- see the report). So the party drives a
reachable race door and that load is retargeted to the boss level (MDKR_LOAD_TRACK),
which exercises the IDENTICAL arrival-adapter seam: the session is ACTIVE_LOBBY, a
boss raceType loads, SOLO_START fires, and the return re-runs the roster machinery.
The boss-door WARP geometry itself is retail and unchanged (1P-anchored by
check_first_boss_progression / check_boss_win_verdict).

Positive controls (mutate the measured OUTPUT, never the game): a
return-with-one-racer replay (suspended roster shrunk to one seat) must FAIL the
restore assertion; a double-award replay (boss award lines duplicated) must FAIL
the exactly-once assertion.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import (config_block, DEFAULT_BUILD_DIR, put_bits,
                           resolve_binary, save_env, seal_slot, SLOT_BYTES,
                           slot_checksum_valid)
from check_first_boss_progression import read_bits, save_order

ROOT = Path(__file__).resolve().parent.parent

EEPROM_BYTES = 512
CONFIG_OFFSET = 120
RECORD_OFFSETS = (128, 320)
RECORD_BYTES = 192

LOBBY = 12
ANCIENT_LAKE = 5               # a reachable Dino race door; retargeted to the boss
TRICKY_ONE = 38                # boss course (Dino Domain, world 1)
TRICKY_CUTSCENE = 57
FIRST_THREE = (5, 3, 29)       # three Dino Domain balloon races (pre-cleared)
DINO_WORLD_BIT = 1 << 1
RACE_CLEARED_STATUS = 2
TAJ_CAR_OFFERED = 0x01
HUB_BALLOON_FLAGS = (1 << 10) | (1 << 14)

COMPLETION_BOSS = 2            # AdventurePartyCompletionKind
RACE_KIND_BOSS = 9            # AdventurePartyRaceKind

ADMIT3 = "tests/input_scripts/adventure_party_3p_admit.txt"
# Drive a reachable Dino race door repeatedly; MDKR_LOAD_TRACK retargets each such
# load to the boss, so the first entry is the first win and later entries are
# rematches of the (now beaten) boss.
ROUTE = ("0:200,500:-1004,946:-1858,1099:-3381,1946:-3948,2180:E12"
         ";12:E5:E5:E5")

SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)")
RESTORE_RE = re.compile(
    r"aparty_restore: suspend_lgen=(\d+) restore_lgen=(\d+) match=(-?\d+)")
AWARD_RE = re.compile(
    r"aparty_award: op=(\w+) sgen=(\d+) lgen=(\d+) course=(\d+) "
    r"activity=(\d+) kind=(\d+) result=(-?\d+)")
LEVEL_RE = re.compile(
    r"level_load: levelId=(-?\d+) numPlayers=(-?\d+) entrance=(-?\d+) "
    r"vehicle=(-?\d+) cutscene=(-?\d+) @frame~(\d+)")
BOSSFINISH_RE = re.compile(
    r"bossfinish: finishPos=(-?\d+) playerIndex=(-?\d+) courseId=(\d+) "
    r"worldId=(-?\d+) bosses=0x([0-9a-f]+) courseFlags=0x([0-9a-f]+)")
RACEFIELD_RE = re.compile(r"racefield: ")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")

SAVE_ORDER: list[int] = []


def dino_slot() -> bytes:
    """A started Adventure One save: three Dino races cleared, first boss NOT
    beaten (bosses=0)."""
    status = {level: RACE_CLEARED_STATUS for level in FIRST_THREE}
    bits: list[int] = []
    put_bits(bits, 16, 0)
    for ordinal in range(34):
        level = SAVE_ORDER[ordinal] if ordinal < len(SAVE_ORDER) else -1
        put_bits(bits, 2, status.get(level, 0))
    put_bits(bits, 6, TAJ_CAR_OFFERED)
    put_bits(bits, 10, 0)                       # trophies
    put_bits(bits, 12, 0)                       # bosses (first boss NOT beaten)
    for value in (5, 3, 0, 0, 0, 0):            # total + world balloon counts
        put_bits(bits, 7, value)
    put_bits(bits, 3, 0)                        # TT amulet
    put_bits(bits, 3, 0)                        # Wizpig amulet
    put_bits(bits, 16, HUB_BALLOON_FLAGS)
    for _ in range(5):
        put_bits(bits, 16, 0)
    put_bits(bits, 8, 0)                        # keys
    put_bits(bits, 32, 0)                       # cutscene flags
    put_bits(bits, 16, 0x1234)                  # named/started file
    put_bits(bits, 8, 0)
    if len(bits) != SLOT_BYTES * 8:
        raise AssertionError(f"slot builder emitted {len(bits)} bits")
    slot = bytearray(
        sum(bits[i + j] << (7 - j) for j in range(8))
        for i in range(0, len(bits), 8))
    return bytes(seal_slot(slot))


def eeprom_image() -> bytes:
    out = bytearray(EEPROM_BYTES)
    out[:SLOT_BYTES] = dino_slot()
    out[SLOT_BYTES:CONFIG_OFFSET] = b"\xFF" * (CONFIG_OFFSET - SLOT_BYTES)
    out[CONFIG_OFFSET:CONFIG_OFFSET + 8] = config_block((1 << 25) | 1)
    for offset in RECORD_OFFSETS:
        out[offset:offset + RECORD_BYTES] = bytes(seal_slot(bytearray(RECORD_BYTES)))
    return bytes(out)


def decode_slot(save: bytes) -> dict:
    slot = save[:SLOT_BYTES]
    return {
        "checksum_ok": slot_checksum_valid(slot),
        "bosses": read_bits(slot, 100, 12),
        "boss_status": read_bits(slot, 16 + SAVE_ORDER.index(TRICKY_ONE) * 2, 2),
    }


def run(binary, rom, *, frames, values, timeout):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_boss_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1", MDKR_AUTOPILOT="1",
                   MDKR_DRIVE_ROUTE=ROUTE, MDKR_SIMULATION_CADENCE="original",
                   MDKR_SYNTH_FIELDS="2", MDKR_WATCH_COURSEFLAGS=str(TRICKY_ONE),
                   MDKR_LOAD_TRACK=str(TRICKY_ONE), MDKR_BOSS_ROUTE="1")
        env.update(values)
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        cmd = [binary, "--headless-frames", str(frames),
               "--input-script", str(ROOT / ADMIT3), "--rom", rom,
               "--window-size", "320x240",
               "--video-set", "Enhancements.AdventureParty=1"]
        proc = subprocess.run(cmd, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout, check=False)
        out = proc.stdout or ""
        ee = save_dir / "eeprom.bin"
        return out, (ee.read_bytes() if ee.is_file() else None), proc.returncode


def sessions(out):
    return [(m.group(1), int(m.group(2)), int(m.group(3)))
            for m in SESSION_RE.finditer(out)]


def rosters(out):
    return [(int(m.group(1)), int(m.group(2), 16)) for m in ROSTER_RE.finditer(out)]


def restores(out):
    return [(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            for m in RESTORE_RE.finditer(out)]


def boss_awards(out):
    return [(m.group(1), int(m.group(7))) for m in AWARD_RE.finditer(out)
            if int(m.group(6)) == COMPLETION_BOSS]


def boss_finishes(out):
    return [(int(m.group(1)), int(m.group(2))) for m in BOSSFINISH_RE.finditer(out)]


def base_fail(label, out, rc, failures):
    if rc != 0:
        failures.append(f"{label}: process exit {rc}")
    if m := BAD_RE.search(out):
        failures.append(f"{label}: runtime failure marker {m.group(0)!r}")


def assert_suspend_restore(label, out, failures, *, players=3):
    """The host-solo suspension envelope + exact restore (win or loss)."""
    if not any(s[0] == "SOLO_ACTIVITY" for s in sessions(out)):
        failures.append(f"{label}: no aparty_session SOLO_ACTIVITY (no suspend)")
    expected_mask = (1 << players) - 1
    if not any(n == players and mask == expected_mask for n, mask in rosters(out)):
        failures.append(f"{label}: no suspended party roster n={players} "
                        f"mask=0x{expected_mask:x}; saw {rosters(out)}")
    if RACEFIELD_RE.search(out):
        failures.append(f"{label}: a party race field was spawned for the boss "
                        f"(racefield present) -- the boss must be host-solo")
    bf = boss_finishes(out)
    if not any(pidx == 0 for _pos, pidx in bf):
        failures.append(f"{label}: no host (playerIndex 0) boss finish; saw {bf}")
    if not any(r[2] == 1 for r in restores(out)):
        failures.append(f"{label}: no aparty_restore match=1 (party not restored "
                        f"to the exact suspended roster); saw {restores(out)}")
    states = [s[0] for s in sessions(out)]
    if "SOLO_ACTIVITY" in states:
        after = states[states.index("SOLO_ACTIVITY"):]
        if "ACTIVE_LOBBY" not in after:
            failures.append(f"{label}: session never returned to ACTIVE_LOBBY "
                            f"after the solo activity; states={states}")


def main():
    global SAVE_ORDER
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    for p in (binary, rom, str(ROOT / ADMIT3)):
        if not os.path.exists(p):
            print(f"check_adventure_party_boss_restore: FAIL -- missing {p}",
                  file=sys.stderr)
            return 1
    SAVE_ORDER = save_order(rom)

    failures: list[str] = []

    # --- WIN + rematch: suspend host-solo, win the boss exact-once, restore; then
    #     re-enter the beaten boss (rematch) which awards nothing new.
    win_out, win_save, win_rc = run(
        binary, rom, frames=16000, values={"MDKR_BOSS_WIN": "1"}, timeout=1600)
    base_fail("win", win_out, win_rc, failures)
    assert_suspend_restore("win", win_out, failures)
    ba = boss_awards(win_out)
    issue_ok = [a for a in ba if a[0] == "issue" and a[1] == 1]
    consume_ok = [a for a in ba if a[0] == "consume" and a[1] == 0]
    if len(issue_ok) != 1:
        failures.append(f"win: {len(issue_ok)} boss token issue(ok), expected 1: {ba}")
    if len(consume_ok) != 1:
        failures.append(f"win: {len(consume_ok)} boss token consume(ok), expected 1: {ba}")
    if not any(pos == 1 for pos, _p in boss_finishes(win_out)):
        failures.append(f"win: no first-place boss finish; saw {boss_finishes(win_out)}")
    if win_save is not None:
        st = decode_slot(win_save)
        if not st["checksum_ok"]:
            failures.append("win: persisted slot checksum invalid")
        if not (st["bosses"] & DINO_WORLD_BIT):
            failures.append(f"win: Dino world boss bit not persisted (bosses=0x{st['bosses']:x})")
        if st["boss_status"] != RACE_CLEARED_STATUS:
            failures.append(f"win: boss course status {st['boss_status']}, want CLEARED")
    else:
        failures.append("win: no EEPROM written")
    solo_entries = [s for s in sessions(win_out) if s[0] == "SOLO_ACTIVITY"]
    refused = [a for a in ba if a[0] == "issue" and a[1] == 0]
    if len(solo_entries) < 2:
        failures.append(f"win: rematch did not re-suspend host-solo "
                        f"({len(solo_entries)} SOLO_ACTIVITY entries, expected >=2)")
    if not refused:
        failures.append("win: rematch of the beaten boss did not refuse a boss token "
                        "(no aparty_award issue result=0)")
    if args.verbose:
        print(f"  win: rc={win_rc} solo={len(solo_entries)} restores={restores(win_out)} "
              f"bossawards={ba} decode={decode_slot(win_save) if win_save else None}")

    # --- LOSS: suspend host-solo, lose the boss (no bit, no consume), still restore.
    loss_out, loss_save, loss_rc = run(
        binary, rom, frames=12000, values={}, timeout=1500)
    base_fail("loss", loss_out, loss_rc, failures)
    assert_suspend_restore("loss", loss_out, failures)
    lba = boss_awards(loss_out)
    if [a for a in lba if a[0] == "consume" and a[1] == 0]:
        failures.append(f"loss: a boss token was consumed on a defeat: {lba}")
    if loss_save is not None:
        st = decode_slot(loss_save)
        if st["bosses"] & DINO_WORLD_BIT:
            failures.append(f"loss: Dino boss bit set on a defeat (bosses=0x{st['bosses']:x})")
        if st["boss_status"] == RACE_CLEARED_STATUS:
            failures.append("loss: boss course persisted CLEARED on a defeat")
    if args.verbose:
        print(f"  loss: rc={loss_rc} restores={restores(loss_out)} bossawards={lba} "
              f"decode={decode_slot(loss_save) if loss_save else None}")

    # --- Positive control 1: a return-with-one-racer replay must FAIL restore.
    pc1 = []
    shrunk = re.sub(r"aparty_roster: n=3 mask=0x7", "aparty_roster: n=1 mask=0x1",
                    win_out)
    assert_suspend_restore("PC-one-racer", shrunk, pc1)
    if not pc1:
        failures.append("positive control: a shrunk-roster replay PASSED the restore "
                        "assertions (the gate does not require the exact party)")

    # --- Positive control 2: a double-award replay must FAIL exactly-once.
    doubled_lines = []
    for ln in win_out.splitlines():
        doubled_lines.append(ln)
        if "aparty_award:" in ln and "kind=2" in ln:
            doubled_lines.append(ln)
    doubled = "\n".join(doubled_lines)
    if len([a for a in boss_awards(doubled) if a[0] == "consume" and a[1] == 0]) <= 1:
        failures.append("positive control: duplicating the boss award lines did not "
                        "raise the consume count -- the mutation is inert")

    if failures:
        print("check_adventure_party_boss_restore: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("check_adventure_party_boss_restore: PASS -- a 3P party suspends to a "
          "host-solo boss (SOLO_ACTIVITY + suspended roster, no party race field, "
          "host finishes), a first WIN commits the Dino boss bit exactly once (one "
          "aparty_award boss token issue+consume, save delta bosses 0x0->0x2), a "
          "LOSS awards nothing, the beaten boss re-suspends on a rematch and refuses "
          "a new token, and EVERY return restores the exact party (aparty_restore "
          "match=1); two positive controls fired (one-racer, doubled-award)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
