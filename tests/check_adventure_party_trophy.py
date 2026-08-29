#!/usr/bin/env python3
"""AP-16: the ADVENTURE TROPHY SERIES for an Adventure Party (Part A = SPLIT).

Part A decision (measured, see task-14-report.md)
-------------------------------------------------
The retail Adventure trophy race fields EIGHT racers (track_setup_racers defaults
gNumRacers to 8 and a 1P trophy keeps it; the rankings width is 8). AP-16 Part A
measured that this eight-racer field renders four viewports within the per-frame
DL/matrix/vertex budgets -- the 4P DL high-water is 6354 Gfx, far under the 11000
4P budget (and under the always-present 4-viewport hub's 9670). So the trophy row
is a SPLIT: the party's N humans plus (8-N) CPUs to the retail EIGHT-racer total,
one-for-one, four viewports -- never a shrunken or improvised field.

What this proves (Enhancements.AdventureParty ON, a party resumed to the hub)
-----------------------------------------------------------------------------
* Entry + field. The party enters the real trophy series (trophyseries ENTER) and
  every round is the retail 8-racer split: racefield total=8, humans=N, cpus=8-N,
  viewports=N (the Part A decision, exercised on the live trophy path).
* Series progression + standings. All four production rounds run (trophyround
  rounds 0..3, the Dino tracks 5,3,29,7), the between-round rankings screens are
  produced (trophyrankings x4), and the leading racer's series points accumulate
  across rounds (points0 strictly increases) -- one continuous series through the
  EXISTING trophy state, no new session state.
* Exact-once series award. A gold championship writes the trophy exactly once
  (trophyaward old->new, new != old) AND mints+consumes exactly ONE COMPLETION_TROPHY
  token (one aparty_award issue result=1 + one consume result=0, activity=8 TROPHY,
  kind=3 TROPHY) -- the party's ONE shared series result, keyed to the host (racer 0)
  the ceremony reads (a party collapses to gNumberOfActivePlayers==1, so the trophy
  machinery runs exactly like 1P). The persisted trophy is Dino gold (0x3), the same
  value a 1P gold writes (check_trophy_series is the retail anchor).
* Same-party return. The whole series is ONE ACTIVE_RACE span; on the series exit
  the session returns to ACTIVE_LOBBY with the same party (a later hub load re-forms
  N viewports).

Negative arm: 1P-ref (enhancement OFF) drives the same forced trophy series and
awards the SAME gold trophy through the retail ceremony with NO aparty_award -- the
token arm is party-only, and the party's persisted trophy byte-matches the 1P one.

Positive controls (mutate the measured OUTPUT, never the game): duplicating the
token consume must FAIL the exactly-once assertion; stripping the racefield line
must FAIL the eight-racer field assertion.

Route reachability (R20/R24 retarget precedent, documented)
-----------------------------------------------------------
A headless party cannot drive the world-lobby trophy cabinet's collision/dialogue:
with the cabinet's real precondition set (world boss beaten) the party wedges on a
central-hub Taj-summon SHARED_DIALOGUE before the lobby (the AP-14 finding), and the
boss-beaten lobby repositions the party away from the cabinet. So the gate keeps the
REACHABLE boss-not-beaten world lobby (the AP-13 progress route drives the party into
the Dino lobby) and MDKR_TROPHY_FORCE_ENTER forces the cabinet's OWN
begin_trophy_race_teleport() there -- the least-fake entry: the real trophy-series
machinery runs unchanged; only the precondition-gated collision+dialogue is bypassed
(that gate is covered 1P by check_trophy_series). Every run is in a private temp dir.
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

ROOT = Path(__file__).resolve().parent.parent

DINO_WORLD = 1
LOBBY_LEVEL_ID = 12
EXPECTED_TRACKS = (5, 3, 29, 7)     # Dino Domain trophy rounds
TROPHY_BIT_OFFSET = 16 + 68 + 6     # after checksum + per-course + Taj
DINO_GOLD = 0x3                     # both Dino trophy bits set = gold

# The proven AP-13 route to the Dino lobby (collects hub balloon 10 so the Dino
# door opens), then `;12:T` steers the host toward the cabinet in the lobby. The
# host never has to reach the cabinet -- MDKR_TROPHY_FORCE_ENTER fires there.
DRIVE_ROUTE = ("0:200,500:-1004,946:-1858,1099:B10:-3381,1946:-3948,2180:E12"
               ":-3381,1946:-2519,1516:-1858,1099;12:T")

ADMIT = {2: "tests/input_scripts/adventure_party_2p_admit.txt",
         3: "tests/input_scripts/adventure_party_3p_admit.txt",
         4: "tests/input_scripts/adventure_party_4p_admit.txt"}
RESUME_1P = "tests/input_scripts/adventure_resume_race.txt"

# Racer 0 (the host) first in every round -> 9+9+9+9 = 36 points, championship
# rank 0 = the gold trophy (both Dino bits). This is check_trophy_series's GOLD_ORDER.
GOLD_ORDER = "/".join(("0,1,2,3,4,5,6,7",) * 4)
# Three CPUs ahead of every party human in every round -> the host places 4th
# (rank 3), no top-3 finish, no trophy, no token.
LOSE_ORDER = "/".join(("4,5,6,0,1,2,3,7",) * 4)

SERIES_RE = re.compile(r"trophyseries: ENTER world=(\d+)")
FIELD_RE = re.compile(
    r"racefield: level=(\d+) humans=(\d+) cpus=(\d+) total=(\d+) viewports=(\d+)")
ROUND_RE = re.compile(
    r"trophyround: world=(\d+) round=(\d+) track=(\d+) points0=(\d+)")
RANK_RE = re.compile(r"trophyrankings: world=(\d+) completedRound=(\d+)")
TROPHYAWARD_RE = re.compile(
    r"trophyaward: world=(\d+) rank=(\d+) points=(\d+) old=0x([0-9a-f]+) "
    r"new=0x([0-9a-f]+) cinematic=(\d+)")
AWARD_RE = re.compile(
    r"aparty_award: op=(\w+) sgen=(\d+) lgen=(\d+) course=(\d+) "
    r"activity=(\d+) kind=(\d+) result=(-?\d+)")
SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")

# aparty_award activity/kind for the trophy token (policy enums).
ACTIVITY_TROPHY = 8      # ADVENTURE_PARTY_RACE_KIND_TROPHY
KIND_TROPHY = 3          # ADVENTURE_PARTY_COMPLETION_TROPHY


def read_bits(data: bytes, offset: int, width: int) -> int:
    value = 0
    for bit in range(offset, offset + width):
        value = (value << 1) | ((data[bit // 8] >> (7 - bit % 8)) & 1)
    return value


def dino_trophy(save: bytes) -> int:
    if not save or len(save) < SLOT_BYTES:
        return -1
    return read_bits(save[:SLOT_BYTES], TROPHY_BIT_OFFSET, 10) & 0x3


def build_script(players: int | None) -> list[str]:
    """Party admit (or 1P resume) + trophy rankings A-taps.

    The A-taps advance the round intros and the between-round rankings menus. They
    start at 3400 -- AFTER the party has reached the Dino lobby and the force hook
    has entered the series -- so they never open a central-hub NPC SHARED_DIALOGUE
    that would wedge the party during the drive.
    """
    if players is None:
        base = (ROOT / RESUME_1P).read_text().splitlines()
    else:
        base = (ROOT / ADMIT[players]).read_text().splitlines()
    taps = [f"{f} A 4" for f in range(3400, 13000, 200)]
    return base + taps


def run(binary, rom, *, players, enabled, order, frames):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_trophy_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_image())
        script = root / "trophy_in.txt"
        script.write_text("\n".join(build_script(players)) + "\n")
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1", MDKR_AUTOPILOT="1",
                   MDKR_DRIVE_ROUTE=DRIVE_ROUTE,
                   MDKR_SIMULATION_CADENCE="enhanced", MDKR_SYNTH_FIELDS="1",
                   MDKR_TROPHY_FORCE_ENTER="1", MDKR_TROPHY_COMPLETE_AFTER="500",
                   MDKR_TROPHY_ORDER=order)
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        cmd = [binary, "--headless-frames", str(frames),
               "--input-script", str(script), "--rom", rom,
               "--window-size", "320x240",
               "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}"]
        proc = subprocess.run(cmd, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=max(900, (frames // 5) * max(players or 1, 1)),
                              check=False)
        out = proc.stdout or ""
        ee = save_dir / "eeprom.bin"
        return out, (ee.read_bytes() if ee.is_file() else b""), proc.returncode


def token_pairs(out):
    """(op, lgen, course, activity, kind, result) for every aparty_award line."""
    return [(m.group(1), int(m.group(3)), int(m.group(4)), int(m.group(5)),
             int(m.group(6)), int(m.group(7))) for m in AWARD_RE.finditer(out)]


def trophy_issues(out):
    return [t for t in token_pairs(out) if t[0] == "issue" and t[5] == 1
            and t[3] == ACTIVITY_TROPHY and t[4] == KIND_TROPHY]


def trophy_consumes(out):
    return [t for t in token_pairs(out) if t[0] == "consume" and t[5] == 0
            and t[3] == ACTIVITY_TROPHY and t[4] == KIND_TROPHY]


def assert_party_gold(label, out, save, players, failures):
    if BAD_RE.search(out):
        failures.append(f"{label}: runtime marker {BAD_RE.search(out).group(0)!r}")
    # Entry
    if not SERIES_RE.search(out):
        failures.append(f"{label}: no trophyseries ENTER (party never entered the series)")
    # Field: the retail 8-racer split, at least two rounds (brief: >= 2 of 4).
    fields = [tuple(map(int, m.groups())) for m in FIELD_RE.finditer(out)]
    trophy_fields = [f for f in fields if f[0] in EXPECTED_TRACKS]
    if len(trophy_fields) < 2:
        failures.append(f"{label}: only {len(trophy_fields)} trophy-round race fields, "
                        f"expected >= 2: {trophy_fields}")
    for lvl, humans, cpus, total, vp in trophy_fields:
        if total != 8 or humans != players or cpus != 8 - players or vp != players:
            failures.append(
                f"{label}: round track {lvl} field humans={humans} cpus={cpus} "
                f"total={total} viewports={vp}, expected 8-racer split "
                f"({players} humans + {8 - players} CPUs, {players} viewports)")
    # Progression: all four rounds, the Dino tracks, points accumulating.
    rounds = [(int(m.group(2)), int(m.group(3)), int(m.group(4)))
              for m in ROUND_RE.finditer(out) if int(m.group(1)) == DINO_WORLD]
    got_rounds = [(r, t) for (r, t, _p) in rounds[:4]]
    if got_rounds != list(enumerate(EXPECTED_TRACKS)):
        failures.append(f"{label}: round/track sequence {got_rounds}, "
                        f"expected {list(enumerate(EXPECTED_TRACKS))}")
    points = [p for (_r, _t, p) in rounds[:4]]
    if points != sorted(points) or len(set(points)) < 2:
        failures.append(f"{label}: series points0 did not accumulate across rounds: {points}")
    ranks = [m for m in RANK_RE.finditer(out) if int(m.group(1)) == DINO_WORLD]
    if len(ranks) < 2:
        failures.append(f"{label}: only {len(ranks)} rankings screens, expected 4")
    # Exact-once trophy award: one upgrade + one token issue + one consume.
    aw = TROPHYAWARD_RE.search(out)
    if not aw:
        failures.append(f"{label}: no trophyaward (championship never resolved)")
    else:
        old, new, rank = int(aw.group(4), 16), int(aw.group(5), 16), int(aw.group(2))
        if new == old:
            failures.append(f"{label}: trophyaward wrote no upgrade (old={old:#x} new={new:#x})")
        if rank != 0 or (new & DINO_GOLD) != DINO_GOLD:
            failures.append(f"{label}: expected a gold (rank 0, new & 0x3 == 0x3); "
                            f"got rank={rank} new={new:#x}")
    io, co = trophy_issues(out), trophy_consumes(out)
    if len(io) != 1:
        failures.append(f"{label}: {len(io)} trophy token issues(ok), expected 1: {io}")
    if len(co) != 1:
        failures.append(f"{label}: {len(co)} trophy token consumes(ok), expected 1: {co}")
    if io and co and io[0][1] != co[0][1]:
        failures.append(f"{label}: token issue lgen {io[0][1]} != consume lgen {co[0][1]}")
    # Same-party return: an ACTIVE_RACE span then a later ACTIVE_LOBBY.
    states = [(m.group(1), int(m.group(3))) for m in SESSION_RE.finditer(out)]
    race_i = next((i for i, s in enumerate(states) if s[0] == "ACTIVE_RACE"), None)
    if race_i is None or not any(s[0] == "ACTIVE_LOBBY" for s in states[race_i + 1:]):
        failures.append(f"{label}: no same-party ACTIVE_RACE->ACTIVE_LOBBY return: {states}")
    # Persisted gold trophy.
    if dino_trophy(save) != DINO_GOLD:
        failures.append(f"{label}: persisted Dino trophy {dino_trophy(save):#x}, expected gold {DINO_GOLD:#x}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("--players", type=int, default=4, choices=(2, 3, 4))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    for p in (binary, rom):
        if not os.path.exists(p):
            print(f"check_adventure_party_trophy: FAIL -- missing {p}", file=sys.stderr)
            return 1

    failures: list[str] = []

    # --- Arm A: a party trophy series to gold (the Part A SPLIT, full deliverable).
    out, save, rc = run(binary, rom, players=args.players, enabled=True,
                        order=GOLD_ORDER, frames=13000)
    if rc != 0:
        failures.append(f"party-gold: process exit {rc}")
    assert_party_gold("party-gold", out, save, args.players, failures)
    if args.verbose:
        print("  party-gold: issues=", trophy_issues(out), "consumes=", trophy_consumes(out),
              "trophy=", TROPHYAWARD_RE.search(out).group(0) if TROPHYAWARD_RE.search(out) else None,
              "persisted=", hex(dino_trophy(save)))

    # --- Arm B: 1P-ref (enhancement OFF) drives the same forced series to the SAME
    #     gold through the retail ceremony, with NO party token. Byte target for the
    #     persisted trophy and the party-only proof of the token arm.
    ref_out, ref_save, ref_rc = run(binary, rom, players=None, enabled=False,
                                    order=GOLD_ORDER, frames=12000)
    if ref_rc != 0:
        failures.append(f"1P-ref: process exit {ref_rc}")
    if BAD_RE.search(ref_out):
        failures.append(f"1P-ref: runtime marker {BAD_RE.search(ref_out).group(0)!r}")
    if trophy_issues(ref_out) or trophy_consumes(ref_out) or AWARD_RE.search(ref_out):
        failures.append("1P-ref: aparty_award emitted on a non-party (retail) trophy award")
    if not TROPHYAWARD_RE.search(ref_out):
        failures.append("1P-ref: retail trophy award did not resolve")
    if dino_trophy(ref_save) != DINO_GOLD:
        failures.append(f"1P-ref: retail trophy {dino_trophy(ref_save):#x}, expected gold {DINO_GOLD:#x}")
    if save and ref_save and dino_trophy(save) != dino_trophy(ref_save):
        failures.append("byte target: party trophy != 1P trophy")

    # --- Positive controls (mutate Arm A's OUTPUT; the assertions must catch it).
    pc_dupe = re.sub(r"(aparty_award: op=consume[^\n]*\n)", r"\1\1", out, count=1)
    pc_f, pc_save = [], save
    assert_party_gold("PC-dupe", pc_dupe, pc_save, args.players, pc_f)
    if not any("consumes(ok), expected 1" in m for m in pc_f):
        failures.append("PC-dupe: duplicating a token consume did not fail exactly-once")

    pc_strip = "\n".join(l for l in out.splitlines() if "racefield:" not in l)
    pc_f2 = []
    assert_party_gold("PC-strip", pc_strip, pc_save, args.players, pc_f2)
    if not any("trophy-round race fields" in m for m in pc_f2):
        failures.append("PC-strip: stripping the racefield lines did not fail the field assertion")

    if failures:
        print(f"check_adventure_party_trophy: FAIL ({len(failures)} issue(s))")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"check_adventure_party_trophy: PASS -- a {args.players}P party enters the real "
          "Adventure trophy series (forced past the headless-unreachable cabinet), every "
          "round fields the retail EIGHT-racer split (total=8, N humans + 8-N CPUs, N "
          "viewports -- Part A decision), all four rounds run with accumulating standings, "
          "and a gold championship writes the Dino trophy exactly once via one "
          "COMPLETION_TROPHY token (issue+consume), persisting gold (0x3) BYTE-EQUAL to a 1P "
          "gold; the 1P-ref awards the same gold with NO party token; the party returns to "
          "the same lobby. Two positive controls fired (duplicate-consume, stripped-field)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
