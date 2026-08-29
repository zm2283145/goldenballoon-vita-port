#!/usr/bin/env python3
"""AP-11 focused route: the Taj transaction and the shared-scene envelope.

What this proves (Enhancements.AdventureParty ON, a party in the central hub)
---------------------------------------------------------------------------
* NON-HOST summon: seat 1 (player two) summons Taj by driving into him -- any
  occupied seat may trigger the interaction (TRIGGER_TRANSITION authority), not
  just player one. The shared dialogue latches once (ACTIVE_LOBBY ->
  SHARED_DIALOGUE).
* HOST dialogue authority: player one owns the menu regardless of who summoned
  (taj_menu_loop reads input_pressed(PLAYER_ONE)); the vehicle choice is traced
  as a host DIALOGUE_CHOICE.
* WHOLE-PARTY transform: retail transform_player_vehicle rebuilds the roster as
  EXACTLY ONE racer, which would destroy a party. The party path rebuilds ALL N
  racers atomically with the NEW shared vehicle and the SAME seat->character
  identities, and restores the split layout. The transaction's own oracle is
  `aparty_transform path=party n=N live=N` -- live is the post-rebuild gNumRacers,
  so a collapse reads `live=1` (the central failure this gate guards). Exactly
  ONE transform is emitted (the roster is rebuilt once, not repeatedly).
* R10 latch lifecycle: the DIALOGUE latch RELEASES on completion (SHARED_DIALOGUE
  -> ACTIVE_LOBBY at the SAME level generation -- the transform is within-lobby,
  never a reload), so a door works again in the SAME lobby visit. No transition
  fires between the latch and the release: retail freezes all racer input during
  the Taj scene (disable_racer_input), and the arbiter additionally rejects any
  door press while the session is not ACTIVE_LOBBY, so a door cannot bypass the
  SHARED_DIALOGUE state. Doors working FROM ACTIVE_LOBBY is proven by
  check_adventure_party_transition; this gate proves the state is restored to it.
* Identity persistence: post-transform the seat->port->racer binding holds
  (the [RACERINPUT] witness: port == player == racer) and the roster characters
  are unchanged.

Everything is read from aparty_ traces the running binary emits (AP-05 schema)
plus the aparty_transform diagnostic; the facts come from the pure reducer and
the game adapters, not from "the process did not crash".

Driving
-------
Seat 1 summons via MDKR_AP_SEAT_ROUTE="1=-197,193" (Taj's hub position -- driving
into him opens the dialogue on contact); the host navigates the menu via the
input script (taj_menu_loop reads PLAYER_ONE input). Only seat 1 drives: a second
moving racer near a lobby door raises a door textbox that blocks npc_dialogue_loop
and (an engine quirk) diverges shell-vs-headless, so the deterministic route keeps
exactly one mover -- the summoner -- and the menu is host input.

Positive controls
-----------------
* Collapse the post-transform roster to one (rewrite live=N -> live=1 in the
  aparty_transform line): the whole-party assertion must FAIL.
* Strip the SHARED_DIALOGUE latch lines: the latch assertion must FAIL.

Off arm
-------
Retail JOINTVENTURE 2P Taj is untouched: tests/check_taj_p2_adventure.py is run
and quoted here (it uses the retail lead-swap machinery a party never engages).

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

TAJ_SEAT_ROUTE = "1=-197,193"  # seat 1 drives into Taj (BHV_PARK_WARDEN) to summon
HOVERCRAFT = 1  # the vehicle the host selects (lobby default is CAR=0 -> transforms)

# aparty action / verdict enum values (adventure_party_policy.h), emitted raw.
ACTION_DIALOGUE_CHOICE = 0
ACTION_TRIGGER = 2
VERDICT_LATCHED = 0

TRANSFORM_RE = re.compile(
    r"aparty_transform: path=(\w+) vehicle=(\d+) n=(\d+) live=(\d+)")
SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
INTER_RE = re.compile(
    r"aparty_interaction: seat=(\d+) action=(-?\d+) verdict=(-?\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)((?: c\d+=\d+)*)")
LAYOUT_RE = re.compile(r"aparty_layout: viewports=(\d+) layout=(\d+)")
TRANS_RE = re.compile(r"aparty_transition: seat=(\d+) tick=(\d+)")
RACERINPUT_RE = re.compile(
    r"\[RACERINPUT\] tick=(\d+) player=(-?\d+) racer=(\d+) port=(\d+)")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")


def run_arm(binary, rom, fixture, seat_route, frames=6000, verbose=False):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_taj_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1",
                   MDKR_RACER_INPUT_TRACE="1", MDKR_AP_SEAT_ROUTE=seat_route)
        save_env(env, str(save_dir))
        env["MDKR_VIDEO_CONFIG_PATH"] = str(root / "mdkr64.ini")
        command = [
            binary, "--headless-frames", str(frames),
            "--input-script", str(ROOT / fixture), "--rom", rom,
            "--window-size", "320x240",
            "--video-set", "Enhancements.AdventureParty=1",
        ]
        if verbose:
            print(f"$ {' '.join(command)}  route={seat_route}", flush=True)
        proc = subprocess.run(command, cwd=root, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=max(240, frames // 8), check=False)
    return proc.stdout or ""


def find_line(lines, regex, start=0, pred=None):
    for i in range(start, len(lines)):
        m = regex.search(lines[i])
        if m and (pred is None or pred(m)):
            return i, m
    return -1, None


def assert_transform_scene(out, players, label):
    """Returns (failures, transform_line_index). transform_line_index is -1 when
    the scene never reached a clean single party transform."""
    f = []
    if bad := BAD_RE.search(out):
        f.append(f"{label}: fatal marker {bad.group(0)!r}")
        return f, -1
    lines = out.splitlines()

    # --- Non-host summon latches the shared dialogue --------------------------
    si, sm = find_line(lines, INTER_RE, pred=lambda m: int(m.group(2)) == ACTION_TRIGGER
                       and int(m.group(3)) == VERDICT_LATCHED)
    if si < 0:
        f.append(f"{label}: no summon interaction (action={ACTION_TRIGGER} latched)")
        return f, -1
    if int(sm.group(1)) == 0:
        f.append(f"{label}: host summoned; expected a NON-host seat (saw seat {sm.group(1)})")
    li, lm = find_line(lines, SESSION_RE, start=si,
                       pred=lambda m: m.group(1) == "SHARED_DIALOGUE")
    if li < 0:
        f.append(f"{label}: summon did not latch SHARED_DIALOGUE")
        return f, -1
    dialogue_lgen = int(lm.group(3))

    # --- Host owns the dialogue choice ---------------------------------------
    ci, cm = find_line(lines, INTER_RE, start=li,
                       pred=lambda m: int(m.group(2)) == ACTION_DIALOGUE_CHOICE
                       and int(m.group(3)) == VERDICT_LATCHED)
    if ci < 0:
        f.append(f"{label}: no host DIALOGUE_CHOICE (action={ACTION_DIALOGUE_CHOICE})")
    elif int(cm.group(1)) != 0:
        f.append(f"{label}: dialogue choice by seat {cm.group(1)}, expected host seat 0")

    # --- Whole-party transform: exactly one, live == n == N ------------------
    transforms = [(i, m) for i, m in enumerate(TRANSFORM_RE.search(l) for l in lines) if m]
    party = [(i, m) for i, m in transforms if m.group(1) == "party"]
    if len(party) != 1:
        f.append(f"{label}: expected exactly ONE party transform (roster rebuilt once), "
                 f"saw {[m.group(0) for _i, m in transforms]}")
        return f, -1
    ti, tm = party[0]
    veh, n, live = int(tm.group(2)), int(tm.group(3)), int(tm.group(4))
    if n != players or live != players:
        f.append(f"{label}: transform rebuilt live={live} of n={n}; expected the "
                 f"WHOLE party ({players}) -- a collapse reads live=1")
    if veh != HOVERCRAFT:
        f.append(f"{label}: transformed to vehicle {veh}, expected HOVERCRAFT={HOVERCRAFT}")

    # roster + layout republished by the transform (after the transform line)
    ri, rm = find_line(lines, ROSTER_RE, start=ti)
    if ri < 0 or int(rm.group(1)) != players or int(rm.group(2), 16) != (1 << players) - 1:
        f.append(f"{label}: transform did not republish the full {players}-seat roster")
    else:
        pre_ri, pre_rm = find_line(lines, ROSTER_RE)
        if pre_rm and pre_rm.group(3) != rm.group(3):
            f.append(f"{label}: characters changed across transform "
                     f"({pre_rm.group(3).strip()} -> {rm.group(3).strip()})")
    yi, ym = find_line(lines, LAYOUT_RE, start=ti,
                       pred=lambda m: int(m.group(1)) == players)
    if yi < 0:
        f.append(f"{label}: transform did not restore the {players}-viewport split layout")

    # --- R10 release: SHARED_DIALOGUE -> ACTIVE_LOBBY, same lgen, after xform --
    rel_i, rel_m = find_line(lines, SESSION_RE, start=ti,
                             pred=lambda m: m.group(1) == "ACTIVE_LOBBY"
                             and int(m.group(3)) == dialogue_lgen)
    if rel_i < 0:
        f.append(f"{label}: dialogue never released to ACTIVE_LOBBY at lgen {dialogue_lgen} "
                 f"(R10 latch release) after the transform")
    else:
        # No door transition between the latch and the release: a door cannot
        # bypass SHARED_DIALOGUE (movement is frozen and the arbiter rejects it).
        di, _ = find_line(lines, TRANS_RE, start=li)
        if 0 <= di < rel_i:
            f.append(f"{label}: a door transition fired DURING the shared dialogue "
                     f"(before the release) -- the door bypassed SHARED_DIALOGUE")

    return f, ti


def assert_identity(lines, players, transform_line_idx, label):
    """Post-transform: distinct racerIndex 0..N-1 drive, port == player == racer."""
    f = []
    seats = set()
    mismatched = 0
    for i in range(transform_line_idx, len(lines)):
        m = RACERINPUT_RE.search(lines[i])
        if not m:
            continue
        player, racer, port = int(m.group(2)), int(m.group(3)), int(m.group(4))
        if player < 0:  # a finished/door racer flips to PLAYER_COMPUTER; skip
            continue
        seats.add(racer)
        if not (player == racer == port):
            mismatched += 1
    want = set(range(players))
    if not want.issubset(seats):
        f.append(f"{label}: post-transform only racerIndex {sorted(seats)} drove; "
                 f"expected all of {sorted(want)} (a seat lost its racer)")
    if mismatched:
        f.append(f"{label}: {mismatched} post-transform [RACERINPUT] rows had "
                 f"port != player != racer (binding broke across the transform)")
    return f


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    taj3 = "tests/input_scripts/adventure_party_3p_taj.txt"
    taj2 = "tests/input_scripts/adventure_party_2p_taj.txt"
    off = ROOT / "tests" / "check_taj_p2_adventure.py"
    required = [binary, rom, *(str(ROOT / p) for p in (taj3, taj2)), str(off)]
    missing = [p for p in required if not os.path.exists(p)]
    if missing:
        for p in missing:
            print(f"check_adventure_party_taj: FAIL -- missing {p}", file=sys.stderr)
        return 1

    failures = []

    # --- 3P main scene: non-host summon + host-owned whole-party transform ----
    out3 = run_arm(binary, rom, taj3, TAJ_SEAT_ROUTE, verbose=args.verbose)
    f3, ti3 = assert_transform_scene(out3, 3, "3P transform")
    failures += f3
    if ti3 >= 0:
        failures += assert_identity(out3.splitlines(), 3, ti3, "3P identity")

    # --- 2P arm: the same transform at two seats -----------------------------
    out2 = run_arm(binary, rom, taj2, TAJ_SEAT_ROUTE, verbose=args.verbose)
    f2, _ = assert_transform_scene(out2, 2, "2P transform")
    failures += f2

    # --- Positive control 1: collapse the post-transform roster to one -------
    collapsed = re.sub(r"(aparty_transform: path=party vehicle=\d+ n=\d+ live=)\d+",
                       r"\g<1>1", out3, count=1)
    pc1, _ = assert_transform_scene(collapsed, 3, "PC-collapse")
    if not pc1:
        failures.append("positive control: a collapsed roster (live=1) PASSED the "
                        "whole-party transform assertion")

    # --- Positive control 2: strip the SHARED_DIALOGUE latch lines -----------
    stripped = "\n".join(l for l in out3.splitlines()
                         if "state=SHARED_DIALOGUE" not in l)
    pc2, _ = assert_transform_scene(stripped, 3, "PC-strip-latch")
    if not pc2:
        failures.append("positive control: output with the SHARED_DIALOGUE latch lines "
                        "stripped PASSED the latch assertion")

    # --- OFF arm: retail JOINTVENTURE 2P Taj is untouched --------------------
    off_proc = subprocess.run(
        [sys.executable, str(off), "--build", args.build, "--rom", args.rom],
        cwd=str(ROOT), text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        timeout=900, check=False)
    off_out = (off_proc.stdout or "").strip()
    off_pass = "check_taj_p2_adventure: PASS" in off_out
    if not off_pass:
        failures.append(f"OFF arm: check_taj_p2_adventure did not pass -- {off_out[-300:]}")

    if failures:
        print("check_adventure_party_taj: FAIL", file=sys.stderr)
        for x in failures:
            print(f"  - {x}", file=sys.stderr)
        return 1
    off_quote = next((l for l in off_out.splitlines()
                      if l.startswith("check_taj_p2_adventure: PASS")), "")
    print("check_adventure_party_taj: PASS -- a NON-host summons Taj (shared dialogue "
          "latched once), the host owns the vehicle choice, the WHOLE party (2P/3P) "
          "transforms transactionally to the new vehicle with the same seat->character "
          "identities and split layout (live==N, never a collapse to 1), and the "
          "dialogue releases to ACTIVE_LOBBY in the same generation (R10) with no door "
          "firing during it; both positive controls fired")
    print(f"  OFF arm: {off_quote}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
