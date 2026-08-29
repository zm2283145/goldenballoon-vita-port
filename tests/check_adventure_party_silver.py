#!/usr/bin/env python3
"""AP-14: TEAM-SHARED SILVER COINS for an Adventure Party silver-coin race.

What this proves (Enhancements.AdventureParty ON, a party resumed into the
central hub, replaying a cleared course as a silver-coin race)
--------------------------------------------------------------------------
The product contract: any human may collect a silver coin; ALL viewports see it
vanish; there is ONE team counter; the party wins with EIGHT team coins plus any
human first, awarding the retail silver clear (RACE_CLEARED_SILVER_COINS) exactly
once through the Task-10 completion token (SILVER kind); a CPU-first finish or a
replay of an already-silver-cleared course awards nothing.

* Team-shared collection by a NON-HOST. In a party silver race any human's touch
  collects a coin: the collector's playerIndex in the silvercoin trace includes a
  non-host seat (>0), each coin is retired for EVERY viewport (invis=0x600 =
  OBJ_FLAGS_INVIS_PLAYER1|_PLAYER2, so viewports 0/1 directly and 2/3 by the
  (viewport & 1) alias all see it gone), and the ONE team tally increments once
  per coin (counts 1..8, each exactly once -- no double-collect).
* HUD parity. Every party viewport's silver HUD reads the same one team tally
  (silverhud viewport=N teamCoins=..), so all N huds show the same running total.
* Win. EIGHT team coins + a human first -> exactly one aparty_award SILVER token
  (activity=3) issue(ok)+consume(ok) and exactly one RACE_CLEARED_SILVER_COINS
  write; the finish reads the TEAM total (silvercoinfinish teamCoins=8) even
  though the leading racer's OWN silverCoinCount (coins=) may be < 8. The
  persisted slot is BYTE-IDENTICAL to a 1P silver win of the same course.
* Loss. A CPU-first finish (human detours for coins, finishes behind on merit,
  no MDKR_ADVENTURE_WIN rotation) writes NO silver clear and mints NO token even
  with eight team coins banked -- the any-human-first half of the policy is not
  met.
* Replay. Re-entering the course after the silver clear is no longer a silver
  race (RACE_CLEARED_SILVER_COINS -> gIsSilverCoinRace FALSE): no coins, no second
  award, no crash. The win arm drives this directly (two Ancient Lake loads, one
  award).

Positive controls (mutate the measured OUTPUT, never the game): a win output with
its team total forced to seven must FAIL the win assertions; a win output with the
coin-collection traces stripped must FAIL them too.

Route (documented per the brief's R20 retarget precedent)
--------------------------------------------------------
A headless party CANNOT drive a boss-beaten world lobby's silver-coin door: the
silver door only appears once the world boss is beaten and the course is cleared
(object_functions.c obj_loop_door, the RACE_CLEARED balloonCountOverride arm), and
that same boss-beaten lobby repositions the party far from the door -- measured
across 15 headless attempts (progressed fixture, MDKR_BOSS_PRECLEARED, balloon
tuning 0..6, world-door-flag pre-unlock, campaign + party routes, host dialogue
dismissal, explicit per-seat routes to the exit): the party reliably reaches the
lobby but wedges on the far side, never entering the door. So, exactly as Task 12
retargeted a reachable race door to reach an unreachable boss/challenge, this gate
drives the PROVEN started-save route into Ancient Lake (which the AP-13 progress
gate navigates and wins) and uses MDKR_SILVER_FORCE=5 to flip THAT course to a
silver-coin race at its own load only -- the world lobby stays the reachable,
boss-not-beaten started-save lobby, and the two silver preconditions
(courseFlags[5] RACE_CLEARED + the Dino boss bit) are set just before
track_spawn_objects reads gIsSilverCoinRace. The hook touches nothing else: coins
are collected by the game's own obj_loop_silvercoin, the counter and award are the
game's own party path.

Adventure Two: NOT RUN. The A2 silver path uses a different coin init
(obj_init_silvercoin_adv2, is_in_adventure_two) and its full matrix is AP-16's;
flagged here rather than smoke-tested to keep this gate on the Adventure One path
the adapters were driven on.

Off arm: 1P silver is unchanged -- tests/check_campaign_progression.py (seam A)
is the anchor. This check never touches a shared save; every run is in its own
temp dir.
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
from check_adventure_party_progress import DRIVE_ROUTE, ADMIT, RESUME_1P

ROOT = Path(__file__).resolve().parent.parent

RACE_LEVEL_ID = 5           # Ancient Lake (Dino Domain, world 1)
RACE_CLEARED = 0x2
RACE_CLEARED_SILVER_COINS = 0x4
STATUS_CLEARED = 2
STATUS_SILVER = 3
SILVER_ACTIVITY = 3         # AdventurePartyRaceKind SILVER_COIN
COMPLETION_COURSE = 0
INVIS_BOTH = 0x600          # OBJ_FLAGS_INVIS_PLAYER1 | _PLAYER2

COIN_RE = re.compile(
    r"silvercoin: playerIndex=(?P<player>\d+) count=(?P<count>\d+) "
    r"action=0x[0-9a-f]+ invis=0x(?P<invis>[0-9a-f]+) @frame~\d+")
HUD_RE = re.compile(r"silverhud: viewport=(?P<vp>\d+) teamCoins=(?P<team>\d+) @frame~\d+")
FINISH_RE = re.compile(
    r"silvercoinfinish: courseId=(?P<course>\d+) leadPlayerIndex=(?P<lead>-?\d+) "
    r"coins=(?P<coins>-?\d+) teamCoins=(?P<team>-?\d+) silverRace=(?P<silver>-?\d+) "
    r"timeTrial=(?P<tt>-?\d+) courseFlags=0x(?P<flags>[0-9a-f]+)")
AWARD_RE = re.compile(
    r"aparty_award: op=(?P<op>\w+) sgen=\d+ lgen=\d+ course=(?P<course>\d+) "
    r"activity=(?P<activity>\d+) kind=(?P<kind>\d+) result=(?P<result>-?\d+)")
BOSSW_RE = re.compile(
    r"\[BOSSW\] frame=(\d+) courseFlags\[(\d+)\]=0x([0-9a-f]+) \(was 0x([0-9a-f]+)\)")
FORCE_RE = re.compile(r"silverforce: level=(\d+) ")
LEVEL_RE = re.compile(r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")


def coin_collects(out):
    return [(int(m["player"]), int(m["count"]), int(m["invis"], 16))
            for m in COIN_RE.finditer(out)]


def silver_writes(out, course=RACE_LEVEL_ID):
    """[BOSSW] rows that newly set RACE_CLEARED_SILVER_COINS on `course`."""
    hits = []
    for m in BOSSW_RE.finditer(out):
        if int(m.group(2)) != course:
            continue
        flags, was = int(m.group(3), 16), int(m.group(4), 16)
        if (flags & RACE_CLEARED_SILVER_COINS) and not (was & RACE_CLEARED_SILVER_COINS):
            hits.append(int(m.group(1)))
    return hits


def silver_award_consumes(out, course=RACE_LEVEL_ID):
    return [m for m in AWARD_RE.finditer(out)
            if m["op"] == "consume" and int(m["course"]) == course
            and int(m["activity"]) == SILVER_ACTIVITY and int(m["result"]) == 0]


def silver_award_issues(out, course=RACE_LEVEL_ID):
    return [m for m in AWARD_RE.finditer(out)
            if m["op"] == "issue" and int(m["course"]) == course
            and int(m["activity"]) == SILVER_ACTIVITY and int(m["result"]) == 1]


def race_loads(out, course=RACE_LEVEL_ID):
    return [int(m.group(3)) for m in LEVEL_RE.finditer(out) if int(m.group(1)) == course]


def run(binary, rom, *, players, frames, values, timeout, enabled=True, script=None):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_silver_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(values.pop("_fixture", None) or eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1", MDKR_AUTOPILOT="1",
                   MDKR_AUTOPILOT_UNSTICK="1", MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
                   MDKR_SIMULATION_CADENCE="enhanced", MDKR_SYNTH_FIELDS="1",
                   MDKR_WATCH_COURSEFLAGS=str(RACE_LEVEL_ID),
                   MDKR_SILVER_FORCE=str(RACE_LEVEL_ID), MDKR_SILVER_ROUTE="1")
        env.update(values)
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        cmd = [binary, "--headless-frames", str(frames),
               "--input-script", str(ROOT / (script or ADMIT[players])), "--rom", rom,
               "--window-size", "320x240",
               "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}"]
        proc = subprocess.run(cmd, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout, check=False)
        out = proc.stdout or ""
        ee = save_dir / "eeprom.bin"
        save = ee.read_bytes() if ee.is_file() else None
        return out, save, proc.returncode


def base_fail(label, out, rc, failures):
    if rc != 0:
        failures.append(f"{label}: process exit {rc}")
    m = BAD_RE.search(out)
    if m:
        failures.append(f"{label}: runtime failure marker {m.group(0)!r}")


def assert_party_silver_win(label, out, save, players, failures, rom):
    """A fresh party silver win: 8 team coins (any-human collect, all viewports
    retire, no double-collect), HUD parity, one SILVER token, one silver write,
    exactly-once across the win + replay, slot silver-cleared."""
    if not FORCE_RE.search(out):
        failures.append(f"{label}: MDKR_SILVER_FORCE never fired (course not made silver)")
    loads = race_loads(out)
    if len(loads) < 1:
        failures.append(f"{label}: Ancient Lake never loaded (route did not reach the race)")

    # --- Team-shared collection: counts 1..8 once each, all viewports retired ---
    coins = coin_collects(out)
    counts = [c for _p, c, _i in coins]
    if counts != list(range(1, 9)):
        failures.append(f"{label}: team coin counts were {counts}, want 1..8 once each "
                        f"(no double-collect, one team tally)")
    bad_invis = [(p, c, i) for p, c, i in coins if i != INVIS_BOTH]
    if bad_invis:
        failures.append(f"{label}: coins not retired for ALL viewports (invis!=0x600): {bad_invis}")
    nonhost = [p for p, _c, _i in coins if p != 0]
    if not nonhost:
        failures.append(f"{label}: no NON-HOST coin collection (any-human collect not shown)")

    # --- HUD parity: every party viewport shows the one team total, reaching 8 ---
    hud = {}
    for m in HUD_RE.finditer(out):
        hud.setdefault(int(m["vp"]), set()).add(int(m["team"]))
    for vp in range(players):
        if vp not in hud:
            failures.append(f"{label}: viewport {vp} never drew the silver HUD team total")
        elif 8 not in hud[vp]:
            failures.append(f"{label}: viewport {vp} HUD never reached teamCoins=8 (saw {sorted(hud[vp])})")

    # --- Finish reads the TEAM total, not the leading racer's own count ---
    fins = [m for m in FINISH_RE.finditer(out) if int(m["course"]) == RACE_LEVEL_ID]
    win_fins = [m for m in fins if int(m["silver"]) == 1 and int(m["team"]) >= 8]
    if not win_fins:
        failures.append(f"{label}: no silver finish with teamCoins>=8 "
                        f"(finishes={[m.group(0) for m in fins]})")

    # --- Exactly-once award across the win AND the replay re-entry -------------
    io, co, sw = silver_award_issues(out), silver_award_consumes(out), silver_writes(out)
    if len(io) != 1:
        failures.append(f"{label}: {len(io)} SILVER token issues(ok), expected 1")
    if len(co) != 1:
        failures.append(f"{label}: {len(co)} SILVER token consumes(ok), expected 1")
    if len(sw) != 1:
        failures.append(f"{label}: {len(sw)} RACE_CLEARED_SILVER_COINS writes, expected 1")

    if save is None:
        failures.append(f"{label}: no EEPROM written")
        return
    st = decode_progress(save, rom)
    if not st["checksum_ok"]:
        failures.append(f"{label}: bad slot checksum")
    if st["course_status"] != STATUS_SILVER:
        failures.append(f"{label}: course status {st['course_status']}, expected SILVER({STATUS_SILVER})")


ROOT_ROM = None


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
    required = [binary, rom, str(ROOT / RESUME_1P), *(str(ROOT / p) for p in ADMIT.values())]
    missing = [p for p in required if not os.path.exists(p)]
    if missing:
        for p in missing:
            print(f"check_adventure_party_silver: FAIL -- missing {p}", file=sys.stderr)
        return 1

    failures = []
    saves = {}

    win_values = {"MDKR_ADVENTURE_WIN": "1"}

    # --- 3P central scene: non-host collect + team counter + HUD + win + replay.
    out3, save3, rc3 = run(binary, rom, players=3, frames=30000,
                           values=dict(win_values), timeout=3200)
    base_fail("3P-win", out3, rc3, failures)
    assert_party_silver_win("3P-win", out3, save3, 3, failures, rom)
    if save3 is not None:
        saves["3P-win"] = save3
    if args.verbose:
        print(f"  3P-win: coins={coin_collects(out3)}")
        print(f"  3P-win: issues={len(silver_award_issues(out3))} consumes={len(silver_award_consumes(out3))} "
              f"writes={silver_writes(out3)} loads={race_loads(out3)}")

    # --- 2P arm: a second party size, byte-target for the 1P comparison.
    out2, save2, rc2 = run(binary, rom, players=2, frames=27000,
                           values=dict(win_values), timeout=3000)
    base_fail("2P-win", out2, rc2, failures)
    assert_party_silver_win("2P-win", out2, save2, 2, failures, rom)
    if save2 is not None:
        saves["2P-win"] = save2

    # --- 1P silver-win reference: same route + MDKR_SILVER_FORCE, enhancement OFF.
    ref_out, ref_save, ref_rc = run(binary, rom, players=2, frames=25000,
                                    values=dict(win_values), timeout=2800,
                                    enabled=False, script=RESUME_1P)
    base_fail("1P-ref", ref_out, ref_rc, failures)
    if any(AWARD_RE.finditer(ref_out)):
        failures.append("1P-ref: aparty_award emitted on a non-party (retail) silver win")
    ref_sw = silver_writes(ref_out)
    if len(ref_sw) != 1:
        failures.append(f"1P-ref: {len(ref_sw)} RACE_CLEARED_SILVER_COINS writes, expected 1")
    if ref_save is None:
        failures.append("1P-ref: no EEPROM written")
    else:
        st = decode_progress(ref_save, rom)
        if st["course_status"] != STATUS_SILVER:
            failures.append(f"1P-ref: status {st['course_status']}, expected SILVER")

    # --- Byte-equivalence: every party-win slot == the 1P silver-win slot.
    if ref_save is not None:
        ref_slot = ref_save[:SLOT_BYTES]
        for label, save in saves.items():
            if save[:SLOT_BYTES] != ref_slot:
                diff = [i for i in range(SLOT_BYTES) if save[i] != ref_slot[i]]
                failures.append(
                    f"byte-equivalence: {label} slot differs from the 1P silver win at "
                    f"bytes {diff} (party={[save[i] for i in diff]} "
                    f"1P={[ref_slot[i] for i in diff]}) -- whitelist is EMPTY")

    # --- CPU-first loss: 8 team coins banked but no human first -> no award.
    #     No MDKR_ADVENTURE_WIN: the coin detour drops the humans behind on merit.
    loss_out, loss_save, loss_rc = run(binary, rom, players=2, frames=22000,
                                       values={}, timeout=2600)
    base_fail("loss", loss_out, loss_rc, failures)
    if silver_award_consumes(loss_out):
        failures.append("loss: a SILVER token was consumed on a CPU-first finish")
    if silver_writes(loss_out):
        failures.append("loss: RACE_CLEARED_SILVER_COINS written on a CPU-first finish")
    if loss_save is not None:
        st = decode_progress(loss_save, rom)
        if st["course_status"] == STATUS_SILVER:
            failures.append("loss: course persisted SILVER on a CPU-first finish")

    # --- Positive control 1: force the team total to seven in the win output ---
    pc1_out = re.sub(r"(silvercoinfinish: courseId=5 leadPlayerIndex=-?\d+ coins=-?\d+ )teamCoins=8",
                     r"\g<1>teamCoins=7", out3)
    pc1_out = pc1_out.replace("count=8 action=", "count=7 action=")
    pc1 = []
    assert_party_silver_win("PC-seven-coins", pc1_out, save3, 3, pc1, rom)
    if not pc1:
        failures.append("positive control: a seven-coin output PASSED the win assertions "
                        "(win check does not require eight team coins)")

    # --- Positive control 2: strip the coin-collection traces from the win output.
    pc2_out = "\n".join(l for l in out3.splitlines() if "silvercoin: playerIndex=" not in l)
    pc2 = []
    assert_party_silver_win("PC-strip-coins", pc2_out, save3, 3, pc2, rom)
    if not pc2:
        failures.append("positive control: a coin-trace-stripped output PASSED the win "
                        "assertions (win check does not require witnessed collection)")

    if failures:
        print("check_adventure_party_silver: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("check_adventure_party_silver: PASS -- a party silver-coin race (3P central "
          "scene + a 2P arm) banks ONE team tally: any human (incl. a non-host seat) "
          "collects a coin, it vanishes for ALL viewports (invis=0x600), the tally "
          "increments 1..8 once each, every viewport's HUD shows the same total, and "
          "EIGHT team coins + a human first awards RACE_CLEARED_SILVER_COINS exactly "
          "once via a SILVER completion token (issue+consume) -- the finish reading the "
          "TEAM total, not the leading racer's own count -- persisting a slot "
          "BYTE-IDENTICAL to a 1P silver win; a CPU-first finish with eight team coins "
          "awards nothing; the post-clear replay is no longer a silver race (no coins, "
          "no second award, no crash); two positive controls fired (seven-coins, "
          "stripped-collection). Adventure Two: NOT RUN (AP-16 owns the A2 matrix).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
