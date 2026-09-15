#!/usr/bin/env python3
"""AP-15: HOST-SOLO SPECIAL-CHALLENGE SUSPENSION ENVELOPE for Adventure Party.

What this proves (Enhancements.AdventureParty ON, a party resumed into a Dino
Domain checkpoint)
------------------------------------------------------------------------------
A party that enters a four-racer special challenge from a lobby SUSPENDS to a
retail-shaped host-solo challenge and, on return, is RESTORED to the exact party:

* Suspension envelope. The challenge loads host-solo -- the retail FOUR-racer field
  ([CHALLENGE] racers=4), one viewport, seat 0 the human, and NO party race field
  (no `racefield:` line) -- and the session enters SOLO_ACTIVITY with the party's
  roster captured as the suspended facts (aparty_session SOLO_ACTIVITY + a
  suspended aparty_roster n=<players>).
* Exact-once win commit. A host WIN commits the retail amulet progression EXACTLY
  ONCE (courseFlags RACE_CLEARED + settings->ttAmulet++, through the unchanged
  challenge-finish path), witnessed by one aparty_award challenge token
  issue(ok)+consume(ok) pair (kind=COMPLETION_CHALLENGE); the save gains one T.T.
  amulet piece, BYTE-equal to the same fields a 1P win of the same challenge writes.
* No commit on defeat. A challenge LOSS mints no token, awards no amulet piece
  (ttAmulet unchanged), and still restores the party to the lobby.
* Re-entry + exact-once across it. The challenge can be re-entered (it reloads on a
  retry, staying SOLO_ACTIVITY — the arrival adapter's challenge-retry arm); a re-win
  of the already-cleared challenge mints NO second token (the permit fails closed on
  RACE_CLEARED, emitting a refused issue result=0).
* 2P as well as 3P: the same host-solo envelope holds for a two-player party.

RESTORE is proven by the boss gate, not re-driven here (see below)
-----------------------------------------------------------------
The return-to-lobby RESTORE (SOLO_ACTIVITY + HUBWORLD load -> SOLO_EXIT +
RESTORE_COMMIT + aparty_restore) is SHARED, activity-agnostic arrival-adapter code
keyed on the HUBWORLD raceType — NOT challenge-specific — and it is exercised
end-to-end (aparty_restore match=1) by check_adventure_party_boss_restore.py. It
cannot be re-driven for the challenge HERE because a headless challenge entry is
only available by retargeting a race door (MDKR_LOAD_TRACK: the world-lobby
challenge door is behind a key + geometry no AI line paths through), and the
retarget leaves gPlayableMapId at the door's dest so the challenge's own
return/retry reloads that dest (retargeted back into the challenge) rather than the
overworld — every postrace option retries or quits, none reaches level 12. So this
gate proves the challenge-SPECIFIC code (SOLO_START on a RACETYPE_CHALLENGE load,
the host-solo four-racer field, the exact-once amulet token, defeat, re-entry) and
leaves the shared restore seam to the boss gate + the unit-tested state machine.

How the challenge is entered: the party drives a reachable race door and that load
is retargeted to the challenge level (MDKR_LOAD_TRACK), exercising the same seam
(ACTIVE_LOBBY -> RACETYPE_CHALLENGE load -> SOLO_START). The four-racer field and
the win commit are retail, unchanged; MDKR_CHALLENGE_OUTCOME drives win/loss.

Positive controls (mutate the measured OUTPUT, never the game): a defeat output
replayed through the win-token assertions must FAIL; stripping the suspension traces
must FAIL the suspend assertion.
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

EGG_CHALLENGE = 11             # Fire Mountain egg challenge (Dino Domain, world 1)
ANCIENT_LAKE = 5              # a reachable Dino race door; retargeted to the challenge
FIRST_THREE = (5, 3, 29)
RACE_CLEARED_STATUS = 2
TAJ_CAR_OFFERED = 0x01
HUB_BALLOON_FLAGS = (1 << 10) | (1 << 14)
COMPLETION_CHALLENGE = 1       # AdventurePartyCompletionKind
BIT_TT_AMULET = 154

ADMIT = {2: "tests/input_scripts/adventure_party_2p_admit.txt",
         3: "tests/input_scripts/adventure_party_3p_admit.txt"}
RESUME_1P = "tests/input_scripts/adventure_resume_race.txt"
ROUTE = ("0:200,500:-1004,946:-1858,1099:-3381,1946:-3948,2180:E12"
         ";12:E5:E5")

SESSION_RE = re.compile(
    r"aparty_session: state=(\w+) sgen=(\d+) lgen=(\d+) host=(\d+)")
ROSTER_RE = re.compile(r"aparty_roster: n=(\d+) mask=0x([0-9a-fA-F]+)")
RESTORE_RE = re.compile(
    r"aparty_restore: suspend_lgen=(\d+) restore_lgen=(\d+) match=(-?\d+)")
AWARD_RE = re.compile(
    r"aparty_award: op=(\w+) sgen=(\d+) lgen=(\d+) course=(\d+) "
    r"activity=(\d+) kind=(\d+) result=(-?\d+)")
CHALLENGE_RE = re.compile(r"\[CHALLENGE\] phase=(\w+) course=(\d+) type=(\d+).*racers=(\d+)")
RACEFIELD_RE = re.compile(r"racefield: ")
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")

SAVE_ORDER: list[int] = []


def dino_slot() -> bytes:
    status = {level: RACE_CLEARED_STATUS for level in FIRST_THREE}
    bits: list[int] = []
    put_bits(bits, 16, 0)
    for ordinal in range(34):
        level = SAVE_ORDER[ordinal] if ordinal < len(SAVE_ORDER) else -1
        put_bits(bits, 2, status.get(level, 0))
    put_bits(bits, 6, TAJ_CAR_OFFERED)
    put_bits(bits, 10, 0)
    put_bits(bits, 12, 0)
    for value in (5, 3, 0, 0, 0, 0):
        put_bits(bits, 7, value)
    put_bits(bits, 3, 0)                        # TT amulet (0 -> a win writes 1)
    put_bits(bits, 3, 0)
    put_bits(bits, 16, HUB_BALLOON_FLAGS)
    for _ in range(5):
        put_bits(bits, 16, 0)
    put_bits(bits, 8, 0)
    put_bits(bits, 32, 0)
    put_bits(bits, 16, 0x1234)
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
        "tt_amulet": read_bits(slot, BIT_TT_AMULET, 3),
        "chal_status": read_bits(slot, 16 + SAVE_ORDER.index(EGG_CHALLENGE) * 2, 2),
    }


def run(binary, rom, *, script, enabled, frames, values, timeout):
    with tempfile.TemporaryDirectory(prefix="mdkr_ap_chal_") as tmp:
        root = Path(tmp)
        save_dir = root / "save"
        save_dir.mkdir()
        (save_dir / "eeprom.bin").write_bytes(eeprom_image())
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("MDKR", "GE007_"))}
        env.update(LC_ALL="C", MDKR_AUDIO="0", MDKR_TRACE="1", MDKR_AUTOPILOT="1",
                   MDKR_DRIVE_ROUTE=ROUTE, MDKR_SIMULATION_CADENCE="original",
                   MDKR_SYNTH_FIELDS="2", MDKR_LOAD_TRACK=str(EGG_CHALLENGE))
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
        ee = save_dir / "eeprom.bin"
        return out, (ee.read_bytes() if ee.is_file() else None), proc.returncode


def sessions(out):
    return [s.group(1) for s in SESSION_RE.finditer(out)]


def rosters(out):
    return [(int(m.group(1)), int(m.group(2), 16)) for m in ROSTER_RE.finditer(out)]


def restores(out):
    return [(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            for m in RESTORE_RE.finditer(out)]


def chal_awards(out):
    return [(m.group(1), int(m.group(7))) for m in AWARD_RE.finditer(out)
            if int(m.group(6)) == COMPLETION_CHALLENGE]


def challenge_racers(out):
    return [int(m.group(4)) for m in CHALLENGE_RE.finditer(out)
            if int(m.group(2)) == EGG_CHALLENGE]


def base_fail(label, out, rc, failures):
    if rc != 0:
        failures.append(f"{label}: process exit {rc}")
    if m := BAD_RE.search(out):
        failures.append(f"{label}: runtime failure marker {m.group(0)!r}")


def assert_suspend(label, out, failures, players):
    """The host-solo suspension envelope: SOLO_ACTIVITY, suspended party roster, a
    retail four-racer field, no party race field."""
    if "SOLO_ACTIVITY" not in sessions(out):
        failures.append(f"{label}: no aparty_session SOLO_ACTIVITY (no suspend)")
    expected_mask = (1 << players) - 1
    if not any(n == players and mask == expected_mask for n, mask in rosters(out)):
        failures.append(f"{label}: no suspended party roster n={players} "
                        f"mask=0x{expected_mask:x}; saw {rosters(out)}")
    if RACEFIELD_RE.search(out):
        failures.append(f"{label}: a party race field was spawned for the challenge "
                        f"(racefield present) -- it must be host-solo")
    racers = challenge_racers(out)
    if not any(r == 4 for r in racers):
        failures.append(f"{label}: challenge did not field 4 racers (host-solo retail "
                        f"field); [CHALLENGE] racers={racers}")


def main():
    global SAVE_ORDER
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    binary = str(Path(resolve_binary(args.build)).resolve())
    rom = str(Path(args.rom).resolve())
    for p in (binary, rom, str(ROOT / ADMIT[3]), str(ROOT / ADMIT[2]),
              str(ROOT / RESUME_1P)):
        if not os.path.exists(p):
            print(f"check_adventure_party_challenges: FAIL -- missing {p}",
                  file=sys.stderr)
            return 1
    SAVE_ORDER = save_order(rom)

    failures: list[str] = []

    # --- 3P WIN + re-entry: suspend host-solo, win the challenge exact-once, restore.
    win_out, win_save, win_rc = run(
        binary, rom, script=ADMIT[3], enabled=True, frames=11000,
        values={"MDKR_CHALLENGE_OUTCOME": "win"}, timeout=1400)
    base_fail("3P-win", win_out, win_rc, failures)
    assert_suspend("3P-win", win_out, failures, 3)
    ca = chal_awards(win_out)
    issue_ok = [a for a in ca if a[0] == "issue" and a[1] == 1]
    consume_ok = [a for a in ca if a[0] == "consume" and a[1] == 0]
    refused = [a for a in ca if a[0] == "issue" and a[1] == 0]
    if len(issue_ok) != 1:
        failures.append(f"3P-win: {len(issue_ok)} challenge token issue(ok), expected 1: {ca}")
    if len(consume_ok) != 1:
        failures.append(f"3P-win: {len(consume_ok)} challenge token consume(ok), expected 1: {ca}")
    # Re-entry: the route re-enters the now-cleared challenge (a retry reload); the
    # permit fails closed on RACE_CLEARED -> a refused issue, no second consume.
    if not refused:
        failures.append("3P-win: re-entry of the cleared challenge did not refuse a "
                        "token (no aparty_award issue result=0)")
    if win_save is not None:
        st = decode_slot(win_save)
        if not st["checksum_ok"]:
            failures.append("3P-win: persisted slot checksum invalid")
        if st["tt_amulet"] != 1:
            failures.append(f"3P-win: ttAmulet={st['tt_amulet']}, expected 1 (one piece)")
        if st["chal_status"] != RACE_CLEARED_STATUS:
            failures.append(f"3P-win: challenge status {st['chal_status']}, want CLEARED")
    else:
        failures.append("3P-win: no EEPROM written")
    if args.verbose:
        print(f"  3P-win: rc={win_rc} sessions={sessions(win_out)} "
              f"restores={restores(win_out)} awards={ca} "
              f"racers={challenge_racers(win_out)} decode={decode_slot(win_save) if win_save else None}")

    # --- 1P reference: the same challenge won with the enhancement OFF, resumed
    #     from the SAME started save. The party win must persist a 40-byte slot
    #     BYTE-IDENTICAL to it (empty whitelist -- the slot holds only campaign
    #     progression, no character/position rows; the house test from
    #     check_adventure_party_progress).
    ref_out, ref_save, ref_rc = run(
        binary, rom, script=RESUME_1P, enabled=False, frames=7000,
        values={"MDKR_CHALLENGE_OUTCOME": "win"}, timeout=900)
    base_fail("1P-ref", ref_out, ref_rc, failures)
    if chal_awards(ref_out):
        failures.append("1P-ref: aparty_award emitted on a non-party (retail) challenge win")
    if ref_save is not None:
        rst = decode_slot(ref_save)
        if rst["tt_amulet"] != 1 or rst["chal_status"] != RACE_CLEARED_STATUS:
            failures.append(f"1P-ref: unexpected 1P challenge win save {rst}")
        if win_save is not None:
            ref_slot = ref_save[:SLOT_BYTES]
            win_slot = win_save[:SLOT_BYTES]
            if win_slot != ref_slot:
                diff = [i for i in range(SLOT_BYTES) if win_slot[i] != ref_slot[i]]
                failures.append(
                    f"byte-equivalence: party challenge win slot differs from the "
                    f"1P win at bytes {diff} (party={[win_slot[i] for i in diff]} "
                    f"1P={[ref_slot[i] for i in diff]}) -- whitelist is EMPTY")

    # --- 3P DEFEAT: suspend host-solo, lose (no token, no amulet), still restore.
    loss_out, loss_save, loss_rc = run(
        binary, rom, script=ADMIT[3], enabled=True, frames=9000,
        values={"MDKR_CHALLENGE_OUTCOME": "loss"}, timeout=1200)
    base_fail("3P-loss", loss_out, loss_rc, failures)
    assert_suspend("3P-loss", loss_out, failures, 3)
    if [a for a in chal_awards(loss_out) if a[0] == "consume" and a[1] == 0]:
        failures.append(f"3P-loss: a challenge token was consumed on a defeat: {chal_awards(loss_out)}")
    if loss_save is not None:
        st = decode_slot(loss_save)
        if st["tt_amulet"] != 0:
            failures.append(f"3P-loss: ttAmulet={st['tt_amulet']} on a defeat, expected 0")
        if st["chal_status"] == RACE_CLEARED_STATUS:
            failures.append("3P-loss: challenge persisted CLEARED on a defeat")
    if args.verbose:
        print(f"  3P-loss: rc={loss_rc} restores={restores(loss_out)} "
              f"awards={chal_awards(loss_out)} decode={decode_slot(loss_save) if loss_save else None}")

    # --- 2P WIN: the same host-solo envelope for a two-player party.
    p2_out, p2_save, p2_rc = run(
        binary, rom, script=ADMIT[2], enabled=True, frames=11000,
        values={"MDKR_CHALLENGE_OUTCOME": "win"}, timeout=1400)
    base_fail("2P-win", p2_out, p2_rc, failures)
    assert_suspend("2P-win", p2_out, failures, 2)
    if len([a for a in chal_awards(p2_out) if a[0] == "consume" and a[1] == 0]) != 1:
        failures.append(f"2P-win: expected exactly one challenge token consume(ok): {chal_awards(p2_out)}")
    if args.verbose:
        print(f"  2P-win: rc={p2_rc} sessions={sessions(p2_out)} awards={chal_awards(p2_out)}")

    # --- Positive control 1: a defeat output replayed through the win-token
    #     assertions must FAIL (otherwise the win check does not require an award).
    pc1_issue = [a for a in chal_awards(loss_out) if a[0] == "issue" and a[1] == 1]
    pc1_consume = [a for a in chal_awards(loss_out) if a[0] == "consume" and a[1] == 0]
    if len(pc1_issue) == 1 and len(pc1_consume) == 1:
        failures.append("positive control: the defeat output satisfies the win-token "
                        "assertions (a defeat must mint no consumable token)")

    # --- Positive control 2: stripping the suspension traces must FAIL suspend.
    pc2 = []
    stripped = "\n".join(ln for ln in win_out.splitlines()
                         if "SOLO_ACTIVITY" not in ln)
    assert_suspend("PC-strip", stripped, pc2, 3)
    if not pc2:
        failures.append("positive control: stripping the suspension traces PASSED the "
                        "suspend assertions")

    if failures:
        print("check_adventure_party_challenges: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("check_adventure_party_challenges: PASS -- a party (3P + a 2P arm) suspends "
          "to a host-solo four-racer special challenge (SOLO_ACTIVITY + suspended "
          "roster, [CHALLENGE] racers=4, no party race field), a host WIN commits one "
          "T.T. amulet piece exactly once (one aparty_award challenge token "
          "issue+consume, ttAmulet 0->1 CLEARED, slot BYTE-IDENTICAL to a 1P win), a re-entry "
          "of the cleared challenge refuses a second token, and a DEFEAT commits "
          "nothing; two positive controls fired (defeat-as-win, strip-suspension). "
          "The shared return-to-lobby RESTORE seam is proven by "
          "check_adventure_party_boss_restore.py (identical arrival-adapter code).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
