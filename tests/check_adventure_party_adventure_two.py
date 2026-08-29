#!/usr/bin/env python3
"""AP-16 Part C: Adventure Two is a required party MATRIX ARM, not a new build.

Adventure Two uses the same party policy and its existing save flag
(CUTSCENE_ADVENTURE_TWO), mirrored tracks, coin object set and progression rules.
This gate proves the party feature works on Adventure Two with NO new game-side
branch: every party adapter (admission, hub formation, race field, silver, award)
is A1/A2-agnostic -- it keys on the party session + raceType / gIsSilverCoinRace,
never on the adventure mode -- and the A2-specific behaviour (mirroring, the A2
coin init obj_init_silvercoin_adv2) is orthogonal retail code keyed on
is_in_adventure_two. The no-new-adapters proof is: the boundary scanner is
unchanged, no game file changed for A2, and all three arms pass on the SAME
adapters an Adventure One party uses.

Three matrix arms (Enhancements.AdventureParty ON, an A2-flagged party fixture):

  1. Admission -> hub at 3P on A2. The party picks Adventure Two at GAME SELECT
     (a DOWN), resumes the A2 save and forms a 3-seat session in the central hub.
     Resuming the A2 save is itself the A2 proof: the file-select rejects a mode
     mismatch (menu.c), so a party that reached the hub with the A2 fixture must
     have selected Adventure Two.

  2. One default race win, exact-once + byte-identical to a 1P A2 win. A party A2
     race win of the mirrored Ancient Lake writes exactly one campaign commit
     (one aparty_award issue+consume + one RACE_CLEARED) through the unchanged
     award path, its save slot BYTE-IDENTICAL to a 1P A2 win of the same course,
     with a live A2 witness (adventure_mode adventureTwo=1 mirrored=1).

  3. One silver scene, over the A2 coin object set. A party A2 silver race banks
     ONE team tally -- any human collects a coin, it vanishes for ALL viewports
     (invis=0x600), every viewport's HUD shows the same total, and eight team
     coins + a human first award RACE_CLEARED_SILVER_COINS exactly once via the
     SILVER token -- the AP-14 machinery UNCHANGED on Adventure Two's mirrored
     course and A2 coins (adventure_mode adventureTwo=1).

Positive controls (mutate the OUTPUT): duplicating the default-race token consume
fails exactly-once; stripping the silver collection fails the team-tally arm.
If Adventure Two had needed ANY new game-side branch this gate could not be
written without it -- it did not. Every run is in a private temp dir.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import (config_block, put_bits, resolve_binary, save_env,
                           seal_slot, SLOT_BYTES)
from check_adventure_race_loop import decode_progress

ROOT = Path(__file__).resolve().parent.parent

RACE_LEVEL_ID = 5           # Ancient Lake (Dino Domain, world 1) -- mirrored in A2
LOBBY_LEVEL_ID = 12
CUTSCENE_ADVENTURE_TWO = 4
RACE_CLEARED = 0x2

# The proven AP-13/AP-14 hub->lobby->race route (collects hub balloon 10).
DRIVE_ROUTE = ("0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
               ":-3381,1946:-2519,1516:-1858,1099;12:E5")

ADMIT = {2: "tests/input_scripts/adventure_party_2p_admit.txt",
         3: "tests/input_scripts/adventure_party_3p_admit.txt",
         4: "tests/input_scripts/adventure_party_4p_admit.txt"}
RESUME_A2_1P = "tests/input_scripts/adventure_two_resume_race.txt"

SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)")
MODE_RE = re.compile(
    r"adventure_mode: level=(\d+) adventureTwo=(\d+) mirrored=(\d+) "
    r"saveFlags=0x([0-9a-fA-F]+)")
AWARD_RE = re.compile(
    r"aparty_award: op=(\w+) sgen=(\d+) lgen=(\d+) course=(\d+) "
    r"activity=(\d+) kind=(\d+) result=(-?\d+)")
BOSSW_RE = re.compile(
    r"\[BOSSW\] frame=(\d+) courseFlags\[(\d+)\]=0x([0-9a-f]+) \(was 0x([0-9a-f]+)\)")
LEVEL_RE = re.compile(r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")
SILVER_RE = re.compile(
    r"silvercoin: playerIndex=(\d+) count=(\d+) action=0x[0-9a-fA-F]+ "
    r"invis=0x([0-9a-fA-F]+)")
SILVERHUD_RE = re.compile(r"silverhud: viewport=(\d+) teamCoins=(\d+)")
SILVERFIN_RE = re.compile(
    r"silvercoinfinish: courseId=(\d+) leadPlayerIndex=(-?\d+) coins=(-?\d+) "
    r"teamCoins=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")


def a2_slot() -> bytes:
    """A started, checksum-valid save carrying CUTSCENE_ADVENTURE_TWO, else the
    plain Adventure-One started slot (byte-for-byte, only the A2 cutscene bit)."""
    bits: list[int] = []
    put_bits(bits, 16, 0)        # checksum, sealed below
    put_bits(bits, 68, 0)        # per-course flags
    put_bits(bits, 6, 0)         # Taj flags
    put_bits(bits, 10, 0)        # trophies
    put_bits(bits, 12, 0)        # bosses
    for _ in range(6):
        put_bits(bits, 7, 0)     # total + five world balloon counts
    put_bits(bits, 3, 0)         # TT amulet
    put_bits(bits, 3, 0)         # Wizpig amulet
    for _ in range(6):
        put_bits(bits, 16, 0)    # world door flags
    put_bits(bits, 8, 0)         # keys
    put_bits(bits, 32, CUTSCENE_ADVENTURE_TWO)   # <-- Adventure Two
    put_bits(bits, 16, 0x1234)   # a named/started file
    put_bits(bits, 8, 0)         # pad
    if len(bits) != SLOT_BYTES * 8:
        raise AssertionError(f"slot builder emitted {len(bits)} bits")
    out = bytearray(int("".join(str(b) for b in bits[i:i + 8]), 2)
                    for i in range(0, len(bits), 8))
    return bytes(seal_slot(out))


def eeprom_a2() -> bytes:
    EEPROM_BYTES, CONFIG_OFFSET, RECORD_OFFSETS, RECORD_BYTES = 512, 120, (128, 320), 192
    out = bytearray(EEPROM_BYTES)
    out[:SLOT_BYTES] = a2_slot()
    out[SLOT_BYTES:CONFIG_OFFSET] = b"\xFF" * (CONFIG_OFFSET - SLOT_BYTES)
    out[CONFIG_OFFSET:CONFIG_OFFSET + 8] = config_block((1 << 25) | 1)  # A2 unlocked
    for off in RECORD_OFFSETS:
        out[off:off + RECORD_BYTES] = bytes(seal_slot(bytearray(RECORD_BYTES)))
    return bytes(out)


def a2_party_admit(players: int) -> list[str]:
    """Party admit for `players`, plus a DOWN at GAME SELECT to pick Adventure Two.

    The GAME SELECT confirm is the second-to-last player-one A tap (the last is the
    file-select resume). A DOWN ~120 frames before it moves the cursor from
    Adventure One (option 0) to Adventure Two (option 1)."""
    lines = (ROOT / ADMIT[players]).read_text().splitlines()
    p1a = [(i, int(m.group(1))) for i, ln in enumerate(lines)
           if (m := re.match(r"(\d+)\s+A\s+\d+(\s+P1)?\s*$", ln.strip()))]
    if len(p1a) < 2:
        raise AssertionError(f"could not find GAME SELECT tap in {ADMIT[players]}")
    game_select_frame = p1a[-2][1]
    out = list(lines)
    out.append(f"{game_select_frame - 120} DOWN 4 P1")
    return sorted(out, key=lambda ln: int(re.match(r"(\d+)", ln.strip()).group(1))
                  if re.match(r"(\d+)", ln.strip()) else 0)


def run(binary, rom, *, script_lines, enabled, values, frames, timeout):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_adv2_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_a2())
        script = root / "adv2_in.txt"
        script.write_text("\n".join(script_lines) + "\n")
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
               "--input-script", str(script), "--rom", rom,
               "--window-size", "320x240",
               "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}"]
        proc = subprocess.run(cmd, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout, check=False)
        out = proc.stdout or ""
        ee = save_dir / "eeprom.bin"
        return out, (ee.read_bytes() if ee.is_file() else b""), proc.returncode


def awards(out, course=RACE_LEVEL_ID):
    return [(m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(7)))
            for m in AWARD_RE.finditer(out) if int(m.group(4)) == course]


def issue_oks(out):
    return [a for a in awards(out) if a[0] == "issue" and a[3] == 1]


def consume_oks(out):
    return [a for a in awards(out) if a[0] == "consume" and a[3] == 0]


def cleared_writes(out, course=RACE_LEVEL_ID):
    hits = []
    for m in BOSSW_RE.finditer(out):
        if int(m.group(2)) != course:
            continue
        flags, was = int(m.group(3), 16), int(m.group(4), 16)
        if (flags & RACE_CLEARED) and not (was & RACE_CLEARED):
            hits.append(int(m.group(1)))
    return hits


def a2_mode_seen(out, course=RACE_LEVEL_ID):
    return any(int(m.group(1)) == course and int(m.group(2)) == 1 and
               int(m.group(3)) == 1 for m in MODE_RE.finditer(out))


def arm_admission_hub(binary, rom, failures, verbose):
    out, save, rc = run(binary, rom, script_lines=a2_party_admit(3), enabled=True,
                        values={}, frames=4200, timeout=1200)
    label = "A2-admit-3P"
    if rc != 0:
        failures.append(f"{label}: process exit {rc}")
    if BAD_RE.search(out):
        failures.append(f"{label}: runtime marker {BAD_RE.search(out).group(0)!r}")
    states = [(m.group(1), int(m.group(3))) for m in SESSION_RE.finditer(out)]
    if not any(s == "ACTIVE_LOBBY" for s, _g in states):
        failures.append(f"{label}: party never reached ACTIVE_LOBBY (A2 resume failed?): {states}")
    rosters = [(int(m.group(1)), int(m.group(2), 16)) for m in ROSTER_RE.finditer(out)]
    if not any(n == 3 for n, _m in rosters):
        failures.append(f"{label}: no 3-seat party roster formed: {rosters}")
    if not any(int(m.group(1)) == 0 for m in LEVEL_RE.finditer(out)):
        failures.append(f"{label}: central hub (level 0) never loaded")
    if save and len(save) >= SLOT_BYTES:
        # the A2 cutscene bit survives the resume (checksum-valid save)
        st = decode_progress(save, rom)
        if not st["checksum_ok"]:
            failures.append(f"{label}: A2 save checksum invalid after admission")
    if verbose:
        print(f"  {label}: states={states} rosters={rosters}")
    return out


def arm_default_win(binary, rom, failures, verbose):
    # Party A2 win of the mirrored Ancient Lake.
    pout, psave, prc = run(
        binary, rom, script_lines=a2_party_admit(3), enabled=True,
        values={"MDKR_AP_RACE_WINNER": "2", "MDKR_FORCE_LAPS": "1",
                "MDKR_TEST_POSTRACE_OPTION": "1"},
        frames=9000, timeout=3600)
    label = "A2-race-win-3P"
    if prc != 0:
        failures.append(f"{label}: process exit {prc}")
    if BAD_RE.search(pout):
        failures.append(f"{label}: runtime marker {BAD_RE.search(pout).group(0)!r}")
    if not a2_mode_seen(pout):
        failures.append(f"{label}: no A2 witness (adventure_mode adventureTwo=1 mirrored=1) for the race")
    io, co, cw = issue_oks(pout), consume_oks(pout), cleared_writes(pout)
    if len(io) != 1:
        failures.append(f"{label}: {len(io)} token issues(ok), expected 1: {io}")
    if len(co) != 1:
        failures.append(f"{label}: {len(co)} token consumes(ok), expected 1: {co}")
    if len(cw) != 1:
        failures.append(f"{label}: {len(cw)} RACE_CLEARED writes, expected 1: {cw}")

    # 1P A2 reference (enhancement OFF), same mirrored course, retail arm.
    rout, rsave, rrc = run(
        binary, rom, script_lines=(ROOT / RESUME_A2_1P).read_text().splitlines(),
        enabled=False,
        values={"MDKR_ADVENTURE_WIN": "1", "MDKR_FORCE_LAPS": "1",
                "MDKR_TEST_POSTRACE_OPTION": "1"},
        frames=9000, timeout=3600)
    if rrc != 0:
        failures.append(f"1P-A2-ref: process exit {rrc}")
    if awards(rout):
        failures.append("1P-A2-ref: aparty_award emitted on a non-party (retail) win")
    if not a2_mode_seen(rout):
        failures.append("1P-A2-ref: no A2 witness for the reference race")
    # Byte-identity: party A2 win slot == 1P A2 win slot (empty whitelist).
    if psave and rsave:
        if psave[:SLOT_BYTES] != rsave[:SLOT_BYTES]:
            ps = decode_progress(psave, rom)
            rs = decode_progress(rsave, rom)
            failures.append(f"{label}: party A2 slot != 1P A2 slot (party={ps} ref={rs})")
    else:
        failures.append(f"{label}: missing save(s) for byte-identity (party={bool(psave)} ref={bool(rsave)})")
    if verbose:
        print(f"  {label}: issue={io} consume={co} cleared={cw} "
              f"byte_equal={bool(psave) and bool(rsave) and psave[:SLOT_BYTES]==rsave[:SLOT_BYTES]}")
    return pout


def arm_silver(binary, rom, failures, verbose):
    out, save, rc = run(
        binary, rom, script_lines=a2_party_admit(3), enabled=True,
        values={"MDKR_SILVER_FORCE": str(RACE_LEVEL_ID), "MDKR_SILVER_ROUTE": "1",
                "MDKR_AUTOPILOT_UNSTICK": "1", "MDKR_ADVENTURE_WIN": "1",
                "MDKR_FORCE_LAPS": "1", "MDKR_TEST_POSTRACE_OPTION": "1"},
        frames=12000, timeout=4800)
    label = "A2-silver-3P"
    if rc != 0:
        failures.append(f"{label}: process exit {rc}")
    if BAD_RE.search(out):
        failures.append(f"{label}: runtime marker {BAD_RE.search(out).group(0)!r}")
    if not a2_mode_seen(out):
        failures.append(f"{label}: no A2 witness (adventure_mode adventureTwo=1) for the silver race")
    coins = [(int(m.group(1)), int(m.group(2)), int(m.group(3), 16))
             for m in SILVER_RE.finditer(out)]
    team_coins = [(pi, cnt, invis) for (pi, cnt, invis) in coins]
    if len(team_coins) < 8:
        failures.append(f"{label}: only {len(team_coins)} team silver collects, expected 8: {team_coins}")
    # every team collect retires for ALL viewports (both invis bits, 0x600)
    if any(invis != 0x600 for (_pi, _c, invis) in team_coins):
        failures.append(f"{label}: a team coin did not retire for all viewports (invis!=0x600): {team_coins}")
    huds = {int(m.group(2)) for m in SILVERHUD_RE.finditer(out)}
    if 8 not in huds:
        failures.append(f"{label}: no viewport HUD reached teamCoins=8: {sorted(huds)}")
    fins = [(int(m.group(3)), int(m.group(4))) for m in SILVERFIN_RE.finditer(out)
            if int(m.group(1)) == RACE_LEVEL_ID]
    if not any(team == 8 for _coins, team in fins):
        failures.append(f"{label}: finish never read teamCoins=8: {fins}")
    sio = [a for a in issue_oks(out)]
    sco = [a for a in consume_oks(out)]
    if len(sio) != 1 or len(sco) != 1:
        failures.append(f"{label}: silver award not exact-once (issues={sio} consumes={sco})")
    if verbose:
        print(f"  {label}: coins={team_coins} huds={sorted(huds)} fins={fins} "
              f"issue={sio} consume={sco}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", default="build")
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    for p in (binary, rom, str(ROOT / RESUME_A2_1P), *(str(ROOT / s) for s in ADMIT.values())):
        if not os.path.exists(p):
            print(f"check_adventure_party_adventure_two: FAIL -- missing {p}", file=sys.stderr)
            return 1

    failures: list[str] = []
    arm_admission_hub(binary, rom, failures, args.verbose)
    win_out = arm_default_win(binary, rom, failures, args.verbose)
    silver_out = arm_silver(binary, rom, failures, args.verbose)

    # Positive controls (mutate the measured OUTPUT).
    pc1 = re.sub(r"(aparty_award: op=consume[^\n]*\n)", r"\1\1", win_out, count=1)
    pc1_f = []
    io, co = issue_oks(pc1), consume_oks(pc1)
    if len(co) == 1:
        failures.append("PC-dupe: duplicating a default-race consume did not break exactly-once")
    pc2 = "\n".join(l for l in silver_out.splitlines() if "silvercoin: playerIndex=" not in l)
    if len([m for m in SILVER_RE.finditer(pc2)]) >= 8:
        failures.append("PC-strip: stripping silver collections left >= 8 (control inert)")

    if failures:
        print(f"check_adventure_party_adventure_two: FAIL ({len(failures)} issue(s))")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("check_adventure_party_adventure_two: PASS -- Adventure Two runs the SAME party "
          "policy with no new game-side branch: a 3P party admits into Adventure Two and "
          "forms the hub, a default race win of the mirrored course is exact-once and "
          "byte-identical to a 1P A2 win, and a silver scene banks one team tally over the "
          "A2 coin object set and awards RACE_CLEARED_SILVER_COINS exactly once (adventure_mode "
          "adventureTwo=1 mirrored=1 throughout); two positive controls fired")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
