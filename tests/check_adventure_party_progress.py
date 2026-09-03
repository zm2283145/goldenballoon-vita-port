#!/usr/bin/env python3
"""AP-13: EXACT-ONCE CAMPAIGN PROGRESSION for Adventure Party default races.

What this proves (Enhancements.AdventureParty ON, a party resumed into the
central hub)
--------------------------------------------------------------------------
The one thing that makes a co-op win count: when ANY party human finishes a
default race first, the game writes EXACTLY ONE existing retail campaign commit
(the course's RACE_CLEARED flag + one world balloon + one central-area balloon),
through the unchanged set_course_finish_flags / race_finish_adventure path, gated
by an exact-once completion token. Losses, quits and duplicate attempts write
nothing. The persisted save is byte-identical to a 1P win of the same course.

* Byte-equivalence, per winner seat. A party win with seat 0 (host), seat 1, 2 or
  3 winning (2P/3P/4P) persists a slot BYTE-IDENTICAL to a 1P win of Ancient Lake
  driven from the same started save. The whitelist of tolerated differences is
  EMPTY (see WHITELIST below): a party win writes the same progression bytes as a
  1P win. The award is decided by the PARTY team condition + a token
  (aparty_award issue/consume traces), NOT by the retail arm.
* No mutation on a non-win. A CPU-first loss and a mid-race quit-to-lobby persist
  status VISITED only (never CLEARED), write no RACE_CLEARED, and consume no
  token.
* Exact-once. Each fresh party win emits exactly one aparty_award issue(ok) +
  consume(ok) pair and exactly one RACE_CLEARED write; a re-entry of the same
  (already-cleared) course writes nothing more and mints no token (the permit
  fails closed on the RACE_CLEARED bit).
* Round-trip. The party-progressed save loads and resumes in 1P Adventure with
  the enhancement OFF: a fresh process reads Ancient Lake back as CLEARED and
  trips no corruption/checksum gate.
* winner_seat. Each seat wins by its STABLE racerIndex (apracewinner), proving the
  team condition survives the finish/door hand-off that relabels a finished human
  PLAYER_COMPUTER (the award reads racerIndex, not playerIndex).

Positive controls (mutate the measured OUTPUT, never the game): a CPU-win output
replayed through the win assertions must FAIL; a win output with its token
consume + RACE_CLEARED write duplicated must FAIL the exactly-once assertion.

WHITELIST (fields a party win may legitimately write differently from a 1P win)
------------------------------------------------------------------------------
EMPTY. The 40-byte save slot (game/src/save_data.c func_800732E8 write order;
see tests/check_campaign_progression.py) holds only campaign progression:
checksum, per-course status, taj, trophies, bosses, balloons, amulets, world
door-flags, keys, cutscenes, filename. It contains NO character or race-position
rows -- those live in the runtime Settings.racers[] (init_racer_headers,
thread3_main.c:1782) which is never serialised -- so the retail-multiplayer
character/start-order differences the brief anticipated do not reach the save at
all. The two saves are therefore compared byte-for-byte with no field excused; if
this ever needs a non-empty whitelist that is a compatibility regression to
escalate, not to widen here.

Off arm: 1P awards are unchanged -- tests/check_adventure_race_loop.py and
tests/check_campaign_progression.py (run and quoted by tools/run_checks.py) are
the anchors. This check never touches a shared save/eeprom.bin; every run is in
its own private temp dir.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env, SLOT_BYTES
from check_adventure_party_admission import eeprom_image
from check_adventure_race_loop import decode_progress

ROOT = Path(__file__).resolve().parent.parent

RACE_LEVEL_ID = 5          # Ancient Lake, a default balloon race (Dino Domain, world 1)
LOBBY_LEVEL_ID = 12
RACE_CLEARED = 0x2         # courseFlagsPtr bit (game/src/menu.h)
STATUS_VISITED = 1
STATUS_CLEARED = 2
FIRST_RACE_LGEN = 3        # FORM(0) RESUME(1) hub->lobby(2) RACE_START(3)

# The shared hub->lobby->race route, collecting hub balloon 10 so the Dino Domain
# doors open. Identical for the party arms and the 1P reference so both open the
# same doors and collect the same balloons -- the ONLY difference between the two
# saves is the award path (party token arm vs retail arm).
DRIVE_ROUTE = ("0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
               ":-3381,1946:-2519,1516:-1858,1099;12:E5")

RESUME_1P = "tests/input_scripts/adventure_resume_race.txt"
ADMIT = {2: "tests/input_scripts/adventure_party_2p_admit.txt",
         3: "tests/input_scripts/adventure_party_3p_admit.txt",
         4: "tests/input_scripts/adventure_party_4p_admit.txt"}
QUIT3 = "tests/input_scripts/adventure_party_3p_race_quit.txt"

AWARD_RE = re.compile(
    r"aparty_award: op=(\w+) sgen=(\d+) lgen=(\d+) course=(\d+) "
    r"activity=(\d+) kind=(\d+) result=(-?\d+)")
BOSSW_RE = re.compile(
    r"\[BOSSW\] frame=(\d+) courseFlags\[(\d+)\]=0x([0-9a-f]+) \(was 0x([0-9a-f]+)\)")
WINNER_RE = re.compile(
    r"apracewinner: mode=(\d+) seat=(-?\d+) winnerPlayer=(-?\d+) "
    r"winnerRacer=(-?\d+) natural=(-?\d+)")
LEVEL_RE = re.compile(r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")


def awards(out):
    """(op, lgen, course, result) for every aparty_award line."""
    return [(m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(7)))
            for m in AWARD_RE.finditer(out)]


def consume_oks(out, course=RACE_LEVEL_ID):
    return [a for a in awards(out)
            if a[0] == "consume" and a[2] == course and a[3] == 0]


def issue_oks(out, course=RACE_LEVEL_ID):
    return [a for a in awards(out)
            if a[0] == "issue" and a[2] == course and a[3] == 1]


def cleared_writes(out, course=RACE_LEVEL_ID):
    """[BOSSW] rows that newly set RACE_CLEARED on `course` (was clear -> set)."""
    hits = []
    for m in BOSSW_RE.finditer(out):
        if int(m.group(2)) != course:
            continue
        flags, was = int(m.group(3), 16), int(m.group(4), 16)
        if (flags & RACE_CLEARED) and not (was & RACE_CLEARED):
            hits.append(int(m.group(1)))
    return hits


def run(binary, rom, *, script, enabled, frames, values, timeout):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_prog_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        save_dir_fixture = values.pop("_fixture", None)
        (save_dir / "eeprom.bin").write_bytes(save_dir_fixture or eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1", MDKR_AUTOPILOT="1",
                   MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
                   MDKR_SIMULATION_CADENCE="enhanced", MDKR_SYNTH_FIELDS="1",
                   MDKR_WATCH_COURSEFLAGS=str(RACE_LEVEL_ID))
        env.update(values)
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        cmd = [binary, "--headless-frames", str(frames),
               "--input-script", str(ROOT / script), "--rom", rom,
               "--window-size", "320x240",
               "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}"]
        proc = subprocess.run(cmd, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout, check=False)
        out = proc.stdout or ""
        save = None
        ee = save_dir / "eeprom.bin"
        if ee.is_file():
            save = ee.read_bytes()
        return out, save, proc.returncode


def win_party(binary, rom, players, seat, frames):
    """Drive a party win with `seat` forced first; return (out, save)."""
    out, save, rc = run(
        binary, rom, script=ADMIT[players], enabled=True, frames=frames,
        values={"MDKR_AP_RACE_WINNER": str(seat), "MDKR_FORCE_LAPS": "1",
                "MDKR_TEST_POSTRACE_OPTION": "1"},
        timeout=max(600, (frames // 6) * players))
    return out, save, rc


def base_fail(label, out, rc, failures):
    if rc != 0:
        failures.append(f"{label}: process exit {rc}")
    m = BAD_RE.search(out)
    if m:
        failures.append(f"{label}: runtime failure marker {m.group(0)!r}")


def assert_party_win(label, out, save, seat, players, failures):
    """A fresh party win: exactly one token issue+consume(ok), one RACE_CLEARED
    write, the winner is the forced seat by racerIndex, and the save is cleared
    with balloons (2, 1)."""
    base_fail(label, out, 0, failures)
    w = WINNER_RE.search(out)
    if not w:
        failures.append(f"{label}: no apracewinner (winner not forced)")
    elif int(w.group(4)) != seat:
        failures.append(f"{label}: winner racerIndex={w.group(4)}, expected seat {seat}")
    io = issue_oks(out)
    co = consume_oks(out)
    cw = cleared_writes(out)
    # one-to-one: exactly one issued token, one consumed token, one flag write
    if len(io) != 1:
        failures.append(f"{label}: {len(io)} token issues(ok), expected 1: {io}")
    if len(co) != 1:
        failures.append(f"{label}: {len(co)} token consumes(ok), expected 1: {co}")
    if len(cw) != 1:
        failures.append(f"{label}: {len(cw)} RACE_CLEARED writes, expected 1: {cw}")
    if io and co and io[0][1] != co[0][1]:
        failures.append(f"{label}: issue lgen {io[0][1]} != consume lgen {co[0][1]}")
    if save is None:
        failures.append(f"{label}: no EEPROM written")
        return
    st = decode_progress(save, ROOT_ROM)
    if not st["checksum_ok"]:
        failures.append(f"{label}: bad slot checksum")
    if st["course_status"] != STATUS_CLEARED:
        failures.append(f"{label}: status {st['course_status']}, expected {STATUS_CLEARED}")
    if st["balloons"][:2] != (2, 1) or any(st["balloons"][2:]):
        failures.append(f"{label}: balloons {st['balloons']}, expected (2,1,0,0,0,0)")


def assert_no_award(label, out, save, failures, *, expect_visited=True):
    """A non-win race: no token consumed(ok) in the first race, no RACE_CLEARED
    write, save not cleared."""
    base_fail(label, out, 0, failures)
    first = [a for a in consume_oks(out) if a[1] == FIRST_RACE_LGEN]
    if first:
        failures.append(f"{label}: token consumed(ok) on the first (lost) race: {first}")
    cw = cleared_writes(out)
    if cw:
        failures.append(f"{label}: {len(cw)} RACE_CLEARED write(s) on a non-win: {cw}")
    if save is not None:
        st = decode_progress(save, ROOT_ROM)
        if st["course_status"] == STATUS_CLEARED:
            failures.append(f"{label}: course persisted CLEARED on a non-win")
        if expect_visited and st["course_status"] not in (0, STATUS_VISITED):
            failures.append(f"{label}: unexpected status {st['course_status']}")


ROOT_ROM = None  # set in main


def main():
    global ROOT_ROM
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    ROOT_ROM = rom
    required = [binary, rom, *(str(ROOT / p) for p in
                              (RESUME_1P, QUIT3, *ADMIT.values()))]
    missing = [p for p in required if not os.path.exists(p)]
    if missing:
        for p in missing:
            print(f"check_adventure_party_progress: FAIL -- missing {p}", file=sys.stderr)
        return 1

    failures = []
    saves = {}
    outs = {}

    # --- Winner-seat matrix: P1(2P) P2(2P) P3(3P) P4(4P), each a byte-equivalent win.
    for label, players, seat, frames in (
            ("P1-host-2P", 2, 0, 8500),
            ("P2-nonhost-2P", 2, 1, 8500),
            ("P3-nonhost-3P", 3, 2, 8800),
            ("P4-nonhost-4P", 4, 3, 9600)):
        out, save, rc = win_party(binary, rom, players, seat, frames)
        base_fail(label, out, rc, failures)
        assert_party_win(label, out, save, seat, players, failures)
        outs[label] = out
        if save is not None:
            saves[label] = save
        if args.verbose:
            print(f"  {label}: issue={issue_oks(out)} consume={consume_oks(out)} "
                  f"cleared={cleared_writes(out)} "
                  f"decode={decode_progress(save, rom) if save else None}")

    # --- 1P reference: resume the SAME started save with the enhancement OFF and
    #     win Ancient Lake through the RETAIL arm (no aparty_award). Byte target.
    ref_out, ref_save, ref_rc = run(
        binary, rom, script=RESUME_1P, enabled=False, frames=9000,
        values={"MDKR_ADVENTURE_WIN": "1", "MDKR_FORCE_LAPS": "1",
                "MDKR_TEST_POSTRACE_OPTION": "1"},
        timeout=900)
    base_fail("1P-ref", ref_out, ref_rc, failures)
    if awards(ref_out):
        failures.append("1P-ref: aparty_award emitted on a non-party (retail) win")
    if ref_save is None:
        failures.append("1P-ref: no EEPROM written")
    else:
        st = decode_progress(ref_save, rom)
        if st["course_status"] != STATUS_CLEARED or st["balloons"][:2] != (2, 1):
            failures.append(f"1P-ref: unexpected 1P-win save {st}")

    # --- Byte-equivalence: every party-win slot == the 1P-win slot (empty whitelist).
    if ref_save is not None:
        ref_slot = ref_save[:SLOT_BYTES]
        for label, save in saves.items():
            if save[:SLOT_BYTES] != ref_slot:
                diff = [i for i in range(SLOT_BYTES)
                        if save[i] != ref_slot[i]]
                failures.append(
                    f"byte-equivalence: {label} slot differs from the 1P win at "
                    f"bytes {diff} (party={[save[i] for i in diff]} "
                    f"1P={[ref_slot[i] for i in diff]}) -- whitelist is EMPTY")

    # --- CPU-first loss: no award (one race, bounded before the route re-enters).
    cpu_out, cpu_save, cpu_rc = run(
        binary, rom, script=ADMIT[2], enabled=True, frames=7100,
        values={"MDKR_AP_RACE_WINNER": "cpu", "MDKR_FORCE_LAPS": "1",
                "MDKR_TEST_POSTRACE_OPTION": "1"},
        timeout=700)
    base_fail("CPU-loss", cpu_out, cpu_rc, failures)
    assert_no_award("CPU-loss", cpu_out, cpu_save, failures)
    cw = WINNER_RE.search(cpu_out)
    if not cw or int(cw.group(1)) != 2:
        failures.append("CPU-loss: CPU winner was not forced (apracewinner mode!=2)")

    # --- Quit-to-lobby: mid-race host pause -> RETURN TO LOBBY, no award.
    quit_out, quit_save, quit_rc = run(
        binary, rom, script=QUIT3, enabled=True, frames=5500,
        values={"MDKR_TEST_PAUSE_QUIT": "1", "MDKR_FORCE_LAPS": "1"},
        timeout=700)
    base_fail("quit", quit_out, quit_rc, failures)
    assert_no_award("quit", quit_out, quit_save, failures)
    if not re.search(r"aparty_interaction: seat=0 action=1 verdict=0", quit_out):
        failures.append("quit: host never took the pause decision")

    # --- Duplicate-commit attempt: after the win auto-returns, the shared route
    #     re-enters the SAME (now cleared) race. The permit fails closed on the
    #     RACE_CLEARED bit, so the second race mints NO token and writes NO flag;
    #     the save still shows exactly one clear + balloons (2,1).
    dup_out, dup_save, dup_rc = win_party(binary, rom, 2, 0, 14000)
    base_fail("duplicate", dup_out, dup_rc, failures)
    dup_races = [f for lv, _n, f in
                 ((int(m.group(1)), int(m.group(2)), int(m.group(3)))
                  for m in LEVEL_RE.finditer(dup_out)) if lv == RACE_LEVEL_ID]
    if len(dup_races) < 2:
        failures.append(f"duplicate: only {len(dup_races)} race load(s); the route "
                        f"did not re-enter the cleared course (raise the budget)")
    else:
        if len(cleared_writes(dup_out)) != 1:
            failures.append(f"duplicate: {len(cleared_writes(dup_out))} RACE_CLEARED "
                            f"writes across a win + re-entry, expected exactly 1")
        if len(consume_oks(dup_out)) != 1:
            failures.append(f"duplicate: {len(consume_oks(dup_out))} token "
                            f"consumes(ok), expected exactly 1 (the re-entry must "
                            f"mint none)")
        if dup_save is not None:
            st = decode_progress(dup_save, rom)
            if st["balloons"][:2] != (2, 1):
                failures.append(f"duplicate: balloons {st['balloons']} after re-entry, "
                                f"expected (2,1) -- a second award leaked")

    # --- Round-trip: the party-progressed save loads + resumes in 1P Adventure.
    if "P1-host-2P" in saves:
        rt_out, rt_save, rt_rc = run(
            binary, rom, script=RESUME_1P, enabled=False, frames=4200,
            values={"_fixture": saves["P1-host-2P"]}, timeout=600)
        base_fail("round-trip", rt_out, rt_rc, failures)
        # the reloaded courseFlags must carry RACE_CLEARED (progress visible in 1P)
        seen = [m for m in BOSSW_RE.finditer(rt_out)
                if int(m.group(2)) == RACE_LEVEL_ID
                and int(m.group(3), 16) & RACE_CLEARED]
        if not seen:
            failures.append("round-trip: the 1P resume never observed Ancient Lake "
                            "CLEARED from the party-written save")
        if rt_save is not None and not decode_progress(rt_save, rom)["checksum_ok"]:
            failures.append("round-trip: the save's checksum broke after a 1P resume")

    # --- Positive control 1: replay the CPU-loss output (with its save) through
    #     the party-win assertions. It must FAIL -- otherwise the win check does
    #     not actually require an award, and an award-on-CPU-win could slip past.
    pc1 = []
    assert_party_win("PC-cpu-as-win", cpu_out, cpu_save, 0, 2, pc1)
    if not pc1:
        failures.append("positive control: a CPU-loss output PASSED the party-win "
                        "assertions (win check does not require an award)")

    # --- Positive control 2: take a real, clean win output and DUPLICATE its token
    #     consume + RACE_CLEARED write lines (simulating a guard that let a second
    #     commit through). The exactly-once assertion must then FAIL -- proving it
    #     discriminates a duplicate. Output/replay only; the game is untouched.
    win_out = outs.get("P1-host-2P")
    if win_out is None:
        failures.append("positive control: no P1 win output to build the duplicate control")
    else:
        doubled = _double_award_lines(win_out)
        if not (len(consume_oks(doubled)) > 1 and len(cleared_writes(doubled)) > 1):
            failures.append("positive control: duplicating the award lines did not "
                            "raise the consume/flag counts -- the mutation is inert")
        pc2 = []
        assert_party_win("PC-doubled", doubled, saves.get("P1-host-2P"), 0, 2, pc2)
        if not pc2:
            failures.append("positive control: a doubled-award output PASSED the "
                            "exactly-once win assertions")

    if failures:
        print("check_adventure_party_progress: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("check_adventure_party_progress: PASS -- a party default-race win at each "
          "winner seat (P1/P2/P3/P4, by stable racerIndex) persists a slot "
          "BYTE-IDENTICAL to a 1P win of the same course (empty whitelist), gated by "
          "exactly one aparty_award issue+consume token and one RACE_CLEARED write; "
          "a CPU-first loss and a quit-to-lobby write nothing; a win + re-entry of "
          "the cleared course still writes exactly one clear; the party save "
          "round-trips into 1P Adventure; two positive controls fired (CPU-as-win, "
          "doubled-award)")
    return 0


def _double_award_lines(out):
    """Positive-control mutation: duplicate each aparty_award and RACE_CLEARED
    write line in the captured output (simulating a guard that let a second
    commit through), so the exactly-once assertions must fail on it."""
    lines = []
    for ln in out.splitlines():
        lines.append(ln)
        if "aparty_award:" in ln or ("[BOSSW]" in ln and "courseFlags[5]" in ln
                                     and "(was 0x1)" in ln):
            lines.append(ln)
    return "\n".join(lines)


if __name__ == "__main__":
    raise SystemExit(main())
