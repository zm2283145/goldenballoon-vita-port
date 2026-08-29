#!/usr/bin/env python3
"""AP-12 / R16 focused route: the DEFAULT PARTY RACE LOOP.

What this proves (Enhancements.AdventureParty ON, a party resumed into the
central hub)
--------------------------------------------------------------------------
* R16 two-hop travel: the whole party crosses central hub -> world lobby (a
  lobby->lobby door, the ADVENTURE_PARTY_EVENT_LOBBY_TRANSITION generation bump)
  and then lobby -> race, each as ONE arbitrated whole-party transition.
* Six-racer field: entering a default balloon-race door runs the race with ALL N
  party humans plus CPUs filling to a SIX-racer total (2P=4 CPUs, 3P=3, 4P=2),
  through the existing race machinery -- N race viewports, per-seat binding held.
  Read from the running binary's `racefield:` adapter diagnostic and the AP-05
  aparty_ roster/layout/binding facts.
* Winner independence: a host win, a non-host-human win, and a CPU win each
  returns the SAME party to the SAME lobby -- roster, layout and per-seat binding
  identical to the race entry, session generation (sgen) unchanged, level
  generation (lgen) advanced (RACE_START then RACE_RESULT_COMMITTED). No lead
  swap, no roster swap. Winners are forced with MDKR_AP_RACE_WINNER (a CPU field
  never naturally lets a chosen human win); method documented in the report.
* Retry and quit-to-lobby: TRY AGAIN reloads the same race (party field intact,
  session stays ACTIVE_RACE -- no RACE_RESULT_COMMITTED between the two loads);
  a mid-race host pause -> RETURN TO LOBBY (PAUSE_QUIT_LOBBY) returns the party
  to the lobby. Neither double-loads.

Driving
-------
The shared MDKR_DRIVE_ROUTE steers every human at the hub/lobby doors (the party
travels as one; the arbiter latches a single whole-party transition); in the
race, unrouted, MDKR_AUTOPILOT drives all humans while the CPUs fill the field.
MDKR_FORCE_LAPS=1 keeps each arm short. MDKR_AP_RACE_WINNER forces the winner,
MDKR_TEST_POSTRACE_OPTION drives the loss/CPU postrace menu, MDKR_TEST_PAUSE_QUIT
drives the mid-race pause menu to RETURN TO LOBBY.

Positive controls
-----------------
* Strip aparty_roster from a return output: the party-returned-intact assertion
  must FAIL (otherwise it cannot tell a re-formed party from nothing).
* Replay a 2P output through the 4P field assertions: must FAIL (a 2P field is
  humans=2/cpus=4, not humans=4/cpus=2).

Off arm
-------
The 1P adventure race loop is unchanged with the enhancement off -- covered by
tests/check_adventure_race_loop.py, run and quoted by tools/run_checks.py.

Save fixture: the started Adventure One slot-0 save from
check_adventure_party_admission (the host's FILE_SELECT confirm resumes it).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary, save_env
from check_adventure_party_admission import eeprom_image

ROOT = Path(__file__).resolve().parent.parent

HUB_LEVEL_ID = 0
LOBBY_LEVEL_ID = 12   # Dino Domain world hub
RACE_LEVEL_ID = 5     # Ancient Lake (a default balloon race)
RACE_FIELD_TOTAL = 6  # the capability row's six-racer field

# hub(0): swing wide of Taj, collect balloon 10, take the door to lobby 12;
# lobby(12): take the door to Ancient Lake (race 5). Unnamed levels (the race)
# are left to MDKR_AUTOPILOT. Sampled from check_adventure_race_loop's proven
# waypoints.
DRIVE_ROUTE = ("0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
               ":-3381,1946:-2519,1516:-1858,1099;12:E5")

SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
TRANS_RE = re.compile(
    r"aparty_transition: seat=(\d+) tick=(\d+) trigger=(\d+) dest=(\d+) lgen=(\d+)")
RACEFIELD_RE = re.compile(
    r"racefield: level=(\d+) humans=(\d+) cpus=(\d+) total=(\d+) viewports=(\d+)")
WINNER_RE = re.compile(
    r"apracewinner: mode=(\d+) seat=(-?\d+) winnerPlayer=(-?\d+) winnerRacer=(-?\d+) natural=(-?\d+)")
LEVEL_RE = re.compile(r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)")
LAYOUT_RE = re.compile(r"aparty_layout: viewports=(\d+) layout=(\d+)")
BINDING_RE = re.compile(r"aparty_binding: seat=(\d+) port=(\d+)")
# racer.c's own per-frame input dispatch (MDKR_RACER_INPUT_TRACE) -- an
# INDEPENDENT seam from the AP trace adapter: for every non-CPU racer it reports
# the racerIndex it carries and the controller port that racer actually reads.
RACERINPUT_RE = re.compile(
    r"\[RACERINPUT\] tick=(\d+) player=(-?\d+) racer=(-?\d+) port=(-?\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")


def sessions(out):
    return [(m.group(1), int(m.group(2)), int(m.group(3)))
            for m in SESSION_RE.finditer(out)]


def transitions(out):
    return [(int(m.group(1)), int(m.group(4)), int(m.group(5)))
            for m in TRANS_RE.finditer(out)]


def racefields(out):
    return [tuple(int(m.group(i)) for i in range(1, 6))
            for m in RACEFIELD_RE.finditer(out)]


def level_seq(out):
    """(levelId, frame) in order."""
    return [(int(m.group(1)), int(m.group(3))) for m in LEVEL_RE.finditer(out)]


def party_facts(segment, players):
    """(has_full_roster, has_layout, bindings) within a text segment."""
    full = any(int(m.group(1)) == players and
               int(m.group(2), 16) == (1 << players) - 1
               for m in ROSTER_RE.finditer(segment))
    layout = any(int(m.group(1)) == players and int(m.group(2)) == players - 1
                 for m in LAYOUT_RE.finditer(segment))
    bindings = sorted({(int(m.group(1)), int(m.group(2)))
                       for m in BINDING_RE.finditer(segment)})
    return full, layout, bindings


def racerinputs(out):
    """(tick, player, racer, port) for every [RACERINPUT] row (humans only --
    a finished/CPU racer is not on this input path)."""
    return [(int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)))
            for m in RACERINPUT_RE.finditer(out)]


def race_entry_tick(out):
    """The simulation tick the lobby->race door latched on (the dest=5
    aparty_transition), in the same tick clock [RACERINPUT] uses."""
    ticks = [int(m.group(2)) for m in TRANS_RE.finditer(out)
             if int(m.group(4)) == RACE_LEVEL_ID]
    return ticks[0] if ticks else None


def assert_race_binding(out, players, label):
    """Real per-seat binding witness for the race, from the INDEPENDENT racer.c
    input-dispatch seam (not the AP adapter that emits seat=i port=i by
    construction). Windowed to mid-race by tick so it is the RACE, not the hub /
    return lobby: every human racer reads its OWN controller port (player==port
    -- would break only if a party wrongly engaged the retail 2P input swap) and
    carries its own seat identity (player==racer), and exactly the N seats
    0..N-1 are actively driven as humans (so seats 1..N-1 are real human racers,
    not CPUs). Returns a list of failures."""
    f = []
    entry = race_entry_tick(out)
    if entry is None:
        return [f"{label}: no lobby->race transition tick (cannot window the race)"]
    rows = racerinputs(out)
    if not rows:
        return [f"{label}: no [RACERINPUT] rows (MDKR_RACER_INPUT_TRACE off?)"]
    maxtick = max(t for t, *_ in rows)
    lo, hi = entry + 200, min(entry + 1800, maxtick - 50)
    win = [(t, pl, rc, pt) for (t, pl, rc, pt) in rows if lo <= t <= hi]
    if len(win) < players * 20:
        f.append(f"{label}: too few mid-race [RACERINPUT] rows in tick window "
                 f"[{lo},{hi}] (saw {len(win)}) -- cannot witness the binding")
        return f
    bad = [(t, pl, rc, pt) for (t, pl, rc, pt) in win if pl != pt or pl != rc]
    if bad:
        f.append(f"{label}: race binding broken -- {len(bad)} row(s) where "
                 f"player!=port or player!=racer, e.g. {bad[0]} "
                 f"(seat did not read its own port / carried a wrong identity)")
    seats = sorted({rc for (_t, _pl, rc, _pt) in win})
    if seats != list(range(players)):
        f.append(f"{label}: race drove human racerIndices {seats}, expected "
                 f"{list(range(players))} (a seat is missing or a CPU is human)")
    return f


def swap_racerinput_port(out, seat, players):
    """Positive-control mutation: rewrite the target seat's [RACERINPUT] rows so
    it reads a DIFFERENT controller port (a swapped binding). assert_race_binding
    must then fail -- otherwise the check cannot discriminate a real swap."""
    lines = []
    for line in out.splitlines():
        m = RACERINPUT_RE.search(line)
        if m and int(m.group(3)) == seat:
            wrong = (int(m.group(4)) + 1) % players
            line = line[:m.start(4)] + str(wrong) + line[m.end(4):]
        lines.append(line)
    return "\n".join(lines)


def run_arm(binary, rom, fixture, players, winner=None, postrace=None,
            pause_quit=False, enabled=True, frames=8500, verbose=False,
            racer_trace=False):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_race_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                   MDKR_AUTOPILOT="1", MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
                   MDKR_FORCE_LAPS="1", MDKR_SIMULATION_CADENCE="enhanced",
                   MDKR_SYNTH_FIELDS="1")
        if winner is not None:
            env["MDKR_AP_RACE_WINNER"] = str(winner)
        if postrace is not None:
            env["MDKR_TEST_POSTRACE_OPTION"] = str(postrace)
        if pause_quit:
            env["MDKR_TEST_PAUSE_QUIT"] = "1"
        if racer_trace:
            env["MDKR_RACER_INPUT_TRACE"] = "1"
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        command = [
            binary, "--headless-frames", str(frames),
            "--input-script", str(ROOT / fixture), "--rom", rom,
            "--window-size", "320x240",
            "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}",
        ]
        if verbose:
            print(f"$ {' '.join(command)}", flush=True)
        # Generous per-arm ceiling, scaled by viewport count (N viewports render
        # ~N x slower headless) with wide margin for a loaded host -- a slow box
        # must not turn into a spurious timeout.
        proc = subprocess.run(command, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=max(600, (frames // 6) * players), check=False)
    return proc.stdout or ""


def first_race_cycle(out):
    """(race_entry_frame, return_frame, race_loads_between) for the first
    lobby -> race -> lobby cycle, or (None, None, n)."""
    seq = level_seq(out)
    race_entry = None
    for lv, fr in seq:
        if lv == RACE_LEVEL_ID:
            race_entry = fr
            break
    if race_entry is None:
        return None, None, 0
    ret = None
    races_between = 0
    for lv, fr in seq:
        if fr <= race_entry:
            continue
        if lv == RACE_LEVEL_ID:
            races_between += 1
        elif lv == LOBBY_LEVEL_ID and ret is None:
            ret = fr
            break
    return race_entry, ret, races_between


def assert_race_entry(out, players, label):
    """The whole party entered the race: two-hop transitions, six-racer field,
    N viewports, per-seat binding. Returns (failures, race_entry_frame)."""
    f = []
    if bad := BAD_RE.search(out):
        f.append(f"{label}: fatal marker {bad.group(0)!r}")
    tr = transitions(out)
    if not any(dest == LOBBY_LEVEL_ID for _s, dest, _g in tr):
        f.append(f"{label}: no hub->lobby transition (R16 two-hop), saw {tr}")
    if not any(dest == RACE_LEVEL_ID for _s, dest, _g in tr):
        f.append(f"{label}: no lobby->race transition, saw {tr}")
    rf = [r for r in racefields(out) if r[0] == RACE_LEVEL_ID]
    if not rf:
        f.append(f"{label}: no party race field was published (racefield:)")
    else:
        humans, cpus, total, viewports = rf[0][1], rf[0][2], rf[0][3], rf[0][4]
        if humans != players:
            f.append(f"{label}: race humans={humans}, expected {players}")
        if total != RACE_FIELD_TOTAL:
            f.append(f"{label}: race total={total}, expected {RACE_FIELD_TOTAL}")
        if cpus != RACE_FIELD_TOTAL - players:
            f.append(f"{label}: race CPUs={cpus}, expected "
                     f"{RACE_FIELD_TOTAL - players} (field rule 6-N)")
        if viewports != players:
            f.append(f"{label}: race viewports={viewports}, expected {players}")
    race_entry, _ret, _rb = first_race_cycle(out)
    if race_entry is None:
        f.append(f"{label}: the race never loaded")
        return f, None
    # The race-entry party facts: roster/layout/binding published at the race
    # load (between the race load and the next level load).
    seq = level_seq(out)
    nxt = next((fr for lv, fr in seq if fr > race_entry), None)
    entry_seg = _segment(out, race_entry, nxt)
    full, layout, bindings = party_facts(entry_seg, players)
    if not full:
        f.append(f"{label}: race entry missing full {players}-seat roster")
    if not layout:
        f.append(f"{label}: race entry missing {players}-viewport layout")
    if bindings != [(i, i) for i in range(players)]:
        f.append(f"{label}: race entry per-seat binding {bindings} != "
                 f"{[(i, i) for i in range(players)]}")
    return f, race_entry


def _frame_of(line):
    m = re.search(r"@frame~(\d+)", line)
    return int(m.group(1)) if m else None


def _segment(out, lo, hi):
    """Text from the level_load at frame `lo` up to (not including) frame `hi`."""
    lines = out.splitlines()
    start = 0
    for i, ln in enumerate(lines):
        fr = _frame_of(ln) if "level_load" in ln else None
        if fr == lo:
            start = i
            break
    end = len(lines)
    if hi is not None:
        for i in range(start + 1, len(lines)):
            fr = _frame_of(lines[i]) if "level_load" in lines[i] else None
            if fr == hi:
                end = i + 40  # include the spawn traces just after the load
                break
    return "\n".join(lines[start:end])


def assert_return_intact(out, players, label, expect_race_result=True):
    """After the race, the SAME party returned to the lobby: identical roster /
    layout / binding, sgen unchanged, lgen advanced past the race."""
    f = []
    race_entry, ret, races_between = first_race_cycle(out)
    if ret is None:
        f.append(f"{label}: never returned to the lobby after the race")
        return f
    if races_between != 0:
        f.append(f"{label}: {races_between} extra race load(s) before the return "
                 f"(a double-load; want exactly one race then the return)")
    # session arc: an ACTIVE_RACE then a later ACTIVE_LOBBY, sgen stable, lgen up.
    ss = sessions(out)
    race_s = next((s for s in ss if s[0] == "ACTIVE_RACE"), None)
    ret_s = None
    seen_race = False
    for st, sgen, lgen in ss:
        if st == "ACTIVE_RACE":
            seen_race = True
        elif st == "ACTIVE_LOBBY" and seen_race:
            ret_s = (st, sgen, lgen)
            break
    if race_s is None:
        f.append(f"{label}: session never entered ACTIVE_RACE")
    if expect_race_result:
        if ret_s is None:
            f.append(f"{label}: session never returned to ACTIVE_LOBBY (no "
                     f"RACE_RESULT_COMMITTED)")
        elif race_s is not None:
            if ret_s[1] != race_s[1]:
                f.append(f"{label}: sgen changed on return "
                         f"{race_s[1]}->{ret_s[1]} (party re-formed?)")
            if ret_s[2] <= race_s[2]:
                f.append(f"{label}: lgen did not advance on return "
                         f"{race_s[2]}->{ret_s[2]}")
    # the returned lobby's party facts must match the race entry's
    seq = level_seq(out)
    nxt = next((fr for lv, fr in seq if fr > ret), None)
    ret_seg = _segment(out, ret, nxt)
    full, layout, bindings = party_facts(ret_seg, players)
    if not full:
        f.append(f"{label}: returned lobby missing full {players}-seat roster")
    if not layout:
        f.append(f"{label}: returned lobby missing {players}-viewport layout")
    if bindings != [(i, i) for i in range(players)]:
        f.append(f"{label}: returned lobby per-seat binding {bindings} != "
                 f"{[(i, i) for i in range(players)]}")
    return f


def assert_winner(out, mode, seat, label):
    w = [m for m in WINNER_RE.finditer(out)]
    if not w:
        return [f"{label}: no apracewinner trace (winner was not forced)"]
    m = w[0]
    if int(m.group(1)) != mode:
        return [f"{label}: winner mode={m.group(1)}, expected {mode}"]
    if mode == 1 and int(m.group(4)) != seat:
        return [f"{label}: winner racerIndex={m.group(4)}, expected seat {seat}"]
    if mode == 2 and int(m.group(4)) < 0:
        return [f"{label}: CPU-win winner racerIndex invalid {m.group(4)}"]
    return []


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    admit3 = "tests/input_scripts/adventure_party_3p_admit.txt"
    admit2 = "tests/input_scripts/adventure_party_2p_admit.txt"
    admit4 = "tests/input_scripts/adventure_party_4p_admit.txt"
    quit3 = "tests/input_scripts/adventure_party_3p_race_quit.txt"
    required = [binary, rom, *(str(ROOT / p)
                               for p in (admit3, admit2, admit4, quit3))]
    missing = [p for p in required if not os.path.exists(p)]
    if missing:
        for p in missing:
            print(f"check_adventure_party_race_loop: FAIL -- missing {p}",
                  file=sys.stderr)
        return 1

    failures = []

    # --- 3P host win: full door->race + return assertions (with the real
    #     input-path binding witness from the independent racer.c seam) ---
    host3 = run_arm(binary, rom, admit3, 3, winner=0, postrace=1,
                    frames=8500, verbose=args.verbose, racer_trace=True)
    fe, _ = assert_race_entry(host3, 3, "3P host-win entry")
    failures += fe
    failures += assert_race_binding(host3, 3, "3P host-win binding")
    failures += assert_winner(host3, 1, 0, "3P host-win")
    failures += assert_return_intact(host3, 3, "3P host-win return")

    # --- 3P non-host human win ---
    non3 = run_arm(binary, rom, admit3, 3, winner=1, postrace=1,
                   frames=8500, verbose=args.verbose)
    failures += assert_winner(non3, 1, 1, "3P non-host-win")
    failures += assert_return_intact(non3, 3, "3P non-host-win return")

    # --- 3P CPU win (postrace menu return path) ---
    cpu3 = run_arm(binary, rom, admit3, 3, winner="cpu", postrace=1,
                   frames=9500, verbose=args.verbose)
    failures += assert_winner(cpu3, 2, -1, "3P cpu-win")
    failures += assert_return_intact(cpu3, 3, "3P cpu-win return")

    # --- 3P retry: TRY AGAIN reloads the SAME race, no RACE_RESULT_COMMITTED
    #     between the two race loads, party field intact both times ---
    retry3 = run_arm(binary, rom, admit3, 3, winner="cpu", postrace=0,
                     frames=9000, verbose=args.verbose)
    rseq = [lv for lv, _fr in level_seq(retry3)]
    # find two race loads with no lobby load between them
    consecutive = False
    last_race = None
    for lv in rseq:
        if lv == RACE_LEVEL_ID:
            if last_race is not None:
                consecutive = True
                break
            last_race = True
        elif lv == LOBBY_LEVEL_ID:
            last_race = None
    if not consecutive:
        failures.append(f"3P retry: TRY AGAIN did not reload the race without a "
                        f"lobby in between; level seq {rseq}")
    rf_retry = [r for r in racefields(retry3) if r[0] == RACE_LEVEL_ID]
    if len(rf_retry) < 2 or any(r[1] != 3 or r[3] != RACE_FIELD_TOTAL
                                for r in rf_retry[:2]):
        failures.append(f"3P retry: the reloaded race did not re-field the party "
                        f"six-racer field, racefields {rf_retry}")
    # the session must not have committed a race result between the two races
    retry_states = [s[0] for s in sessions(retry3)]
    # ACTIVE_RACE appears once; a second ACTIVE_RACE would mean a re-entry
    # through the lobby (RACE_START), i.e. not a retry.
    if retry_states.count("ACTIVE_RACE") != 1:
        failures.append(f"3P retry: session re-entered the race through the "
                        f"lobby instead of retrying; states {retry_states}")

    # --- 3P quit-to-lobby: mid-race host pause -> RETURN TO LOBBY ---
    quit3out = run_arm(binary, rom, quit3, 3, pause_quit=True,
                       frames=5500, verbose=args.verbose)
    failures += assert_return_intact(quit3out, 3, "3P quit-to-lobby return")
    if not any(re.search(r"aparty_interaction: seat=0 action=1 verdict=0", l)
               for l in quit3out.splitlines()):
        failures.append("3P quit-to-lobby: host never took the pause decision")

    # --- 2P field rule + return (+ binding witness) ---
    p2 = run_arm(binary, rom, admit2, 2, winner=0, postrace=1,
                 frames=8500, verbose=args.verbose, racer_trace=True)
    fe2, _ = assert_race_entry(p2, 2, "2P entry")
    failures += fe2
    failures += assert_race_binding(p2, 2, "2P binding")
    failures += assert_return_intact(p2, 2, "2P return")

    # --- 4P field rule + binding (race entry only: 4 viewports render ~4x
    #     slower, and the brief scopes 4P to the field rule; the full winner/
    #     return matrix is the 3P arms above). Stop just past the race load. ---
    p4 = run_arm(binary, rom, admit4, 4, winner=0, postrace=1,
                 frames=4600, verbose=args.verbose, racer_trace=True)
    fe4, _ = assert_race_entry(p4, 4, "4P entry")
    failures += fe4
    failures += assert_race_binding(p4, 4, "4P binding")

    # --- Positive control 1: strip aparty_roster -> return-intact must FAIL ---
    stripped = "\n".join(l for l in host3.splitlines()
                         if "aparty_roster" not in l)
    if not assert_return_intact(stripped, 3, "PC-strip", expect_race_result=True):
        failures.append("positive control: return-intact PASSED with all "
                        "aparty_roster lines stripped")

    # --- Positive control 2: replay 2P output through 4P field assertions ---
    pc2, _ = assert_race_entry(p2, 4, "PC-2P-as-4P")
    if not pc2:
        failures.append("positive control: a 2P output PASSED the 4P field "
                        "assertions (field rule does not discriminate N)")

    # --- Positive control 3: swap seat 1's read-port in the 3P host output and
    #     require the binding witness to FAIL (otherwise it cannot discriminate a
    #     real seat->port swap -- the whole point of the finding). ---
    swapped = swap_racerinput_port(host3, seat=1, players=3)
    if not assert_race_binding(swapped, 3, "PC-swapbind"):
        failures.append("positive control: race binding witness PASSED with seat "
                        "1's controller port swapped (it does not discriminate a "
                        "swapped binding)")

    if failures:
        print("check_adventure_party_race_loop: FAIL", file=sys.stderr)
        for x in failures:
            print(f"  - {x}", file=sys.stderr)
        return 1
    print("check_adventure_party_race_loop: PASS -- party crosses hub->lobby->race "
          "(R16 two-hop), a default race fields six racers (N humans + 6-N CPUs, N "
          "viewports, per-seat binding proven from racer.c's own input dispatch) at "
          "2P/3P/4P, and a host win, a non-host win, a CPU win, a retry and a "
          "mid-race quit-to-lobby each return the same party to the lobby (sgen "
          "stable, lgen advanced); three positive controls fired (roster-strip, "
          "2P-as-4P field, swapped-binding)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
