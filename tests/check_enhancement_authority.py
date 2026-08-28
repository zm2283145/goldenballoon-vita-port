#!/usr/bin/env python3
"""Every enhancement's declared authority class matches its measured effect.

Why this exists
---------------
`platform/enhancement_registry.c` gives each enhancement an AUTHORITY CLASS.
`MDKR_ENH_PRESENTATION` claims the setting cannot move authoritative state;
`MDKR_ENH_GAMEPLAY` claims it does. Those are claims, and a claim nobody tests
is a comment.

This gate tests both directions, for every row, from the table the RUNNING
BINARY has:

  presentation  the `[SIMHASH]` v3 stream must be byte-identical with the
                enhancement at its default and at its probe value.
  gameplay      the stream must DIFFER.

Both directions matter. Testing only the presentation direction would let a
gameplay-changing setting be mislabelled as cosmetic and slip through; testing
only the gameplay direction would let a setting that does nothing be labelled as
if it did.

Why the table comes from the binary
-----------------------------------
The rows are parsed from `[ENHTABLE]` lines the binary emits under
`MDKR_ENH_DUMP_TABLE=1`, including the probe value each row declares for itself.
Keeping either in this file would create a second list: add an enhancement,
forget to update the test, and the gate exercises one fewer setting while still
printing PASS. The count it verified is printed for the same reason — a shrinking
number is visible.

A zero-row parse is a FAILURE, not an empty pass. That is the specific way this
gate could go vacuous, and it is checked first.

What a pass here does NOT mean
-----------------------------
For a `presentation` row, passing means "it did not move authoritative state".
It does **not** mean the enhancement works: one that does nothing at all passes
identically. Proving the effect exists belongs to a per-enhancement gate, and
EFFECT_GATES below names it per row so the gap is visible instead of assumed. A
row with no named gate prints EFFECT UNPROVEN.

That distinction was learned the hard way. EXPECTED_INERT originally listed
presentation rows as "effect not built yet" — an assertion this gate can never
falsify, because for a correct presentation row not-built and working-correctly
are the same observation. The speedometer's effect landing is what exposed it:
the entry could not have fired either way. EXPECTED_INERT is now only meaningful
for `gameplay` rows, where an unimplemented effect genuinely is distinguishable.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from harness_utils import resolve_binary, save_env
from check_adventure_party_admission import eeprom_image

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD = ROOT / "build" / "mdkr64"
SCRIPT = ROOT / "tests" / "input_scripts" / "nav_to_time_trial_race.txt"

# The adventure_party_3p proof route: three controllers join in Character Select
# and select Adventure. With the enhancement on they are admitted to the ordinary
# Adventure route and reach the central hub (levelId 0); with it off, three
# players route to Tracks — the retail JOINTVENTURE offset admits at most two.
ADMISSION_SCRIPT = ROOT / "tests" / "input_scripts" / "adventure_party_3p_admit.txt"
ADMISSION_FRAMES = 3200
HUB_LEVEL_ID = 0
# A generous floor on the identical pre-admission prefix: title, logos, and the
# whole join/confirm phase run byte-identically before the enhancement admits.
ADMISSION_PRE_MIN_TICKS = 1000

# 3500, not 900, and the reason is the same defect found twice.
#
# At 900 this route never leaves the menus. The speedometer's own gate measured
# `speedoRows=0` over that budget; the AI-difficulty work then measured 0
# `[ORACLE]` racer rows and no resolve line at all, with the first divergence
# only at tick 2858. So for two separate enhancements this gate was comparing
# two identical menu sequences and reporting a verdict about a race.
#
# A gate whose fixture cannot reach the behaviour it judges will pass whatever
# it is given. Any future change to this number must keep it past the point the
# route actually enters a race.
FRAMES = 3500
HASH_VERSION = "3"

# Proof profiles, as the registry dumps them (platform/enhancement_registry.c).
#
# The row names how it is to be proven, and this gate dispatches on that name
# rather than on a list of its own — the same anti-drift reason the probe value
# lives in the row. A row whose profile this gate does not recognise fails: a
# new proof route is a deliberate addition here, never a silent skip.
PROFILE_SOLO_RACE = "solo_race"
PROFILE_ADVENTURE_PARTY_3P = "adventure_party_3p"

# Rows whose effect is not built yet.
#
# ONLY MEANINGFUL FOR `gameplay` ROWS. This started out listing presentation
# rows too, and that was a mistake worth recording: a presentation row is
# *required* to leave the state stream identical, so "not built yet" and
# "working correctly" are the same observation to this gate. Listing one here
# asserted something unfalsifiable. The speedometer's effect landing is what
# exposed it — the entry could never have fired.
#
# A gameplay row is different: it must MOVE the stream, so an unimplemented one
# is genuinely distinguishable and worth flagging until its effect exists.
#
# The gate fails if a listed row starts moving the stream, so the note cannot
# quietly become the stale thing.
EXPECTED_INERT: dict[str, str] = {
    # Empty: every enhancement's effect is now built. A gameplay row added
    # without one belongs here, with the task that closes it.
}

# What this gate does NOT prove, and who does.
#
# A `presentation` row passing here means "it did not move authoritative
# state". It does NOT mean the enhancement works — an enhancement that does
# nothing at all passes identically. Proving the effect exists is the job of a
# per-enhancement gate, named here so the gap is visible rather than assumed.
#
# A presentation row with no named effect gate is reported as UNPROVEN. That is
# not a failure: an enhancement can legitimately land before its pixel gate
# does. It is a statement that this gate's pass covers half the claim.
EFFECT_GATES = {
    "Enhancements.Speedometer":  "check_enh_speedometer.py",
    "Enhancements.DrawDistance": "check_enh_draw_distance.py",
    "Enhancements.LodBias":      "check_enh_draw_distance.py",
}


def run(binary: Path, rom: Path, work: Path, label: str,
        overrides: list[str], verbose: bool) -> list[str]:
    """One headless race; returns its [SIMHASH] rows."""
    run_dir = work / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR_STATE_HASH=HASH_VERSION,
        MDKR_AUTOPILOT="1",
        MDKR_LOAD_TRACK="5",
        MDKR_RENDERER="gl",
        MDKR_SAVE_DIR=str(save_dir),
        # Isolate the video config with the save (see check_door_blocks.py).
        MDKR_VIDEO_CONFIG_PATH=str(save_dir / "video.ini"),
    )
    command = [
        str(binary), "--headless-frames", str(FRAMES),
        "--input-script", str(SCRIPT), "--rom", str(rom),
        "--window-size", "640x480",
    ]
    for override in overrides:
        command += ["--video-set", override]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    proc = subprocess.run(command, cwd=run_dir, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{label}: exit {proc.returncode}\n"
                           f"{(proc.stdout or '')[-3000:]}")
    rows = [ln for ln in (proc.stdout or "").splitlines()
            if ln.startswith("[SIMHASH]")]
    if not rows:
        raise RuntimeError(f"{label}: no [SIMHASH] rows; the instrument did "
                           f"not arm, so nothing below would mean anything")
    return rows


SIMHASH_RE = re.compile(r"\[SIMHASH\] tick=(\d+) objs=(\d+) h=([0-9a-f]+)")
LEVEL_RE = re.compile(r"level_load: levelId=(-?\d+) numPlayers=(-?\d+).*@frame~(\d+)")


def run_admission_route(binary: Path, rom: Path, work: Path, label: str,
                        enabled: bool, verbose: bool):
    """Drive the 3-controller Adventure admission route; return its [SIMHASH]
    hash stream and the level loads it produced. Uses the same started Adventure
    One save fixture the admission gate builds."""
    run_dir = work / label
    save_dir = run_dir / "save"
    save_dir.mkdir(parents=True)
    (save_dir / "eeprom.bin").write_bytes(eeprom_image())
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_STATE_HASH=HASH_VERSION,
               MDKR_TRACE="1", MDKR_RENDERER="gl")
    save_env(env, str(save_dir))
    command = [
        str(binary), "--headless-frames", str(ADMISSION_FRAMES),
        "--input-script", str(ADMISSION_SCRIPT), "--rom", str(rom),
        "--window-size", "640x480",
        "--video-set", f"Enhancements.AdventureParty={1 if enabled else 0}",
    ]
    if verbose:
        print(f"$ ({label}) {' '.join(command)}", flush=True)
    proc = subprocess.run(command, cwd=run_dir, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{label}: exit {proc.returncode}\n"
                           f"{(proc.stdout or '')[-3000:]}")
    out = proc.stdout or ""
    sims = [m.group(3) for m in SIMHASH_RE.finditer(out)]
    loads = [(int(m.group(1)), int(m.group(2)), int(m.group(3)))
             for m in LEVEL_RE.finditer(out)]
    if not sims:
        raise RuntimeError(f"{label}: no [SIMHASH] rows; the instrument did not arm")
    return sims, loads


def dump_table(binary: Path, rom: Path, work: Path,
               verbose: bool) -> list[dict[str, str]]:
    run_dir = work / "table"
    run_dir.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MDKR", "GE007_"))}
    env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_ENH_DUMP_TABLE="1",
               MDKR_RENDERER="gl")
    command = [str(binary), "--headless-frames", "2", "--rom", str(rom)]
    if verbose:
        print(f"$ (table) {' '.join(command)}", flush=True)
    proc = subprocess.run(command, cwd=run_dir, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300, check=False)
    rows = []
    for line in (proc.stdout or "").splitlines():
        if not line.startswith("[ENHTABLE] "):
            continue
        row = {}
        for field in line[len("[ENHTABLE] "):].split():
            if "=" in field:
                name, _, value = field.partition("=")
                row[name] = value
        rows.append(row)
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(DEFAULT_BUILD))
    ap.add_argument("--rom", default=str(ROOT / "baserom.us.v80.z64"))
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    # Resolve to absolute before anything else: every run below uses
    # cwd=run_dir, so a relative --build or --rom would be looked up under the
    # temporary directory and fail with a bare FileNotFoundError that reads
    # like a missing build rather than a path bug.
    # Do not resolve the native product independently. The shared harness
    # refuses this boundary unless the caller supplied both the app-test class
    # capability and an independent dedicated-desktop attestation. That check
    # intentionally happens before we inspect or launch an old build, because
    # the old executable itself may predate the no-focus guard.
    binary = Path(resolve_binary(args.build)).resolve()
    rom = Path(args.rom).resolve()
    if not binary.exists():
        print(f"check_enhancement_authority: FAIL — no binary at {binary}")
        return 1
    if not rom.exists():
        print(f"check_enhancement_authority: FAIL — no ROM at {rom}")
        return 1

    failures: list[str] = []
    unproven: list[str] = []
    with tempfile.TemporaryDirectory(prefix="enh_authority_") as tmp:
        work = Path(tmp)

        table = dump_table(binary, rom, work, args.verbose)
        # The vacuity guard, first: a gate that iterates an empty list and
        # prints PASS is the failure mode this whole design is arranged around.
        if not table:
            print("check_enhancement_authority: FAIL — parsed zero [ENHTABLE] "
                  "rows. Either MDKR_ENH_DUMP_TABLE stopped working or the "
                  "registry is empty; either way this gate proves nothing.")
            return 1
        print(f"  parsed {len(table)} enhancement row(s) from the binary")

        for row in table:
            key = row.get("key", "?")
            authority = row.get("authority", "?")
            probe = row.get("probe", "")
            profile = row.get("profile", "")
            if not probe:
                failures.append(f"{key}: row declares no probe value")
                continue
            if not profile:
                failures.append(
                    f"{key}: row declares no proof profile — the [ENHTABLE] "
                    f"dump must name one so this gate knows how to prove it")
                continue

            if profile == PROFILE_ADVENTURE_PARTY_3P:
                # AP-06: the 3-player ADMISSION route. Three controllers join in
                # Character Select and select Adventure; with the enhancement on
                # they are admitted to the ordinary Adventure route and reach the
                # central hub (levelId 0), and with it off three players route to
                # Tracks (the retail JOINTVENTURE offset admits at most two). The
                # gate flips the row across off and on and compares the [SIMHASH]
                # streams. (The compiled-out arm is the separate
                # MDKR_ADVENTURE_PARTY_OMIT build, verified in AP-06's off-arm
                # evidence; it behaves as the off arm here.)
                #
                # What this asserts for THIS task (AP-06):
                #   1. Nothing before admission moves: the off and on streams are
                #      byte-identical for a long common prefix (title, logos, the
                #      whole join/confirm phase).
                #   2. The measured gameplay effect is the admission itself: on
                #      reaches the Adventure hub (levelId 0) while off does not,
                #      so the streams then diverge — which is exactly the gameplay
                #      authority the row declares.
                #   3. The party hub is NOT the roster-expanded hub yet: it loads
                #      as one racer (numPlayers 0, i.e. gNumberOfActivePlayers
                #      collapsed to 1 exactly as retail), so today's on-arm hub is
                #      still 1P — the party is native-side bookkeeping only.
                #
                # NOTE (2026-08-28): the brief framed this as "on-arm hub sim
                # IDENTICAL to off-arm"; that is not assertable today because a
                # 3-controller OFF selection routes to Tracks, not the hub, and a
                # party-hub-vs-1P-baseline SIMHASH identity does not hold either
                # (the multi-pad Character Select perturbs pre-hub RNG state that
                # carries into the hub even though the hub correctly renders one
                # racer — same object count). So this asserts the honest, provable
                # facts above instead. TODO(AP-08): when roster expansion lands,
                # the party hub will load with numPlayers N-1 and gain the party's
                # racers; assertion 3 flips (expect the party count) and an in-hub
                # racer/roster assertion is added here. TODO(AP-12): extend the
                # route past the hub into a full 3P race.
                off_sim, off_loads = run_admission_route(
                    binary, rom, work, f"{key}-off", False, args.verbose)
                on_sim, on_loads = run_admission_route(
                    binary, rom, work, f"{key}-on", True, args.verbose)

                on_hub = [ld for ld in on_loads if ld[0] == HUB_LEVEL_ID]
                off_hub = [ld for ld in off_loads if ld[0] == HUB_LEVEL_ID]
                common = 0
                for a, b in zip(off_sim, on_sim):
                    if a != b:
                        break
                    common += 1

                problems: list[str] = []
                if not on_hub:
                    problems.append("the enhanced arm never reached the Adventure "
                                    f"hub (levelId {HUB_LEVEL_ID}); the party was "
                                    "not admitted")
                if off_hub:
                    problems.append("the off arm reached the Adventure hub with "
                                    "the enhancement off — three players must "
                                    "route to Tracks on the retail path")
                if on_sim == off_sim:
                    problems.append("the on and off SIMHASH streams are identical; "
                                    "a gameplay-class admission must move the "
                                    "authoritative stream")
                if common < ADMISSION_PRE_MIN_TICKS:
                    problems.append(
                        f"on/off SIMHASH diverged after only {common} ticks "
                        f"(< {ADMISSION_PRE_MIN_TICKS}); the enhancement perturbed "
                        f"the frontend before admission")
                if on_hub and on_hub[0][1] != 0:
                    problems.append(
                        f"the party hub loaded with numPlayers={on_hub[0][1]}, "
                        f"expected 0 (rendered as 1P for AP-06 — roster expansion "
                        f"is AP-08). If AP-08 has landed, flip this assertion.")

                if problems:
                    for p in problems:
                        failures.append(f"{key}: {p}")
                else:
                    print(f"  {key:32s} {authority:12s} 3P admission route: party "
                          f"reaches the Adventure hub (levelId 0, numPlayers 0) "
                          f"while off routes to Tracks; SIMHASH identical for "
                          f"{common} pre-admission ticks then diverges — gameplay "
                          f"authority confirmed via admission (in-hub roster is "
                          f"AP-08)")
                continue

            if profile != PROFILE_SOLO_RACE:
                failures.append(
                    f"{key}: unknown proof profile '{profile}' — this gate has "
                    f"no route to prove it; add its dispatch here alongside the "
                    f"profile in platform/enhancement_registry.c")
                continue

            # --- solo_race: the historical proof, unchanged. ---
            base = run(binary, rom, work, f"{key}-default", [], args.verbose)
            alt = run(binary, rom, work, f"{key}-probe",
                      [f"{key}={probe}"], args.verbose)
            identical = base == alt
            inert_note = EXPECTED_INERT.get(key)

            if inert_note is not None:
                if identical:
                    print(f"  {key:32s} {authority:12s} inert as expected "
                          f"(effect lands in {inert_note})")
                else:
                    failures.append(
                        f"{key}: listed in EXPECTED_INERT ({inert_note}) but "
                        f"its probe value moved the state stream. The effect "
                        f"landed — remove it from that list and let the real "
                        f"assertion run.")
                continue

            if authority == "presentation" and not identical:
                failures.append(
                    f"{key}: declared presentation, but setting it to "
                    f"'{probe}' changed the authoritative state stream. Either "
                    f"the effect reaches state it must not, or the row is "
                    f"mislabelled.")
            elif authority == "gameplay" and identical:
                failures.append(
                    f"{key}: declared gameplay, but setting it to '{probe}' "
                    f"left the state stream byte-identical. It does nothing.")
            elif authority == "presentation":
                gate = EFFECT_GATES.get(key)
                if gate:
                    print(f"  {key:32s} {authority:12s} ok "
                          f"(effect proven by {gate})")
                else:
                    unproven.append(key)
                    print(f"  {key:32s} {authority:12s} ok, EFFECT UNPROVEN "
                          f"— no gate shows this does anything")
            else:
                print(f"  {key:32s} {authority:12s} ok")

    if failures:
        print("check_enhancement_authority: FAIL")
        for f in failures:
            print(f"  {f}")
        return 1
    summary = (f"check_enhancement_authority: PASS — {len(table)} row(s) "
               f"verified, {len(EXPECTED_INERT)} awaiting their effect")
    if unproven:
        summary += (f"\n  NOTE: {len(unproven)} presentation row(s) have no "
                    f"gate proving the effect exists: "
                    f"{', '.join(unproven)}. This gate only shows they do not "
                    f"move authoritative state.")
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
