#!/usr/bin/env python3
"""Issue #60: "Skip the launcher" boots the game, and the hold gets it back.

The setting (`Launcher.SkipWhenReady`, off by default) trades the launcher for
a faster start. That trade is only safe if the way back is real, so this gate
spends most of its arms on the way back rather than on the feature.

What the product actually does
------------------------------
`main()` takes ONE decision before the first launcher frame -- the setting, and
what was being held when the app opened -- and logs the whole thing:

    [app] skip-launcher setting=<0|1> shift=<0|1> shoulderL=<0|1>
          shoulderR=<0|1> holdOpensLauncher=<0|1> armed=<0|1>

When it arms, `Launcher::draw()` waits for the remembered ROM to settle and
then calls `RomPanel_requestPlayValidation()` -- the SAME call the Play button
makes. The mandatory final ROM check still runs, and only its verdict
publishes a boot. Nothing is skipped except the waiting, and the trace says so:

    [app-ui] skip-launcher direct boot requested rom=<path> finalCheck=<0|1>

`MDKR_APP_SMOKE_SKIP_LAUNCHER=1` adds no behaviour. It only keeps the headless
launcher smoke rendering real frames until a Play action arrives or a deadline
passes -- hashing a 32 MiB image takes longer than the four frames a smoke
draws -- and then reports what happened:

    [app] smoke: skip-launcher armed=<0|1> dispatched=<0|1> serviceFrames=<n>
          actions=<n> actionRom=<path> romPath=<path> romValid=<0|1>
          settled=<0|1>

The arms
--------
1. **boots**       setting on, nothing held -> armed, dispatched, and exactly
                   one Play action carrying the remembered ROM.
2. **shift**       setting on, Shift held -> not armed, no Play action. The
                   documented way back works.
3. **shoulders**   setting on, both shoulders held -> same. The pad way back
                   works.
4. **default**     setting absent, nothing held -> not armed, no Play action.
                   The POSITIVE CONTROL for arm 1: it proves the boot came from
                   the setting and not from merely running the smoke.
5. **one shoulder**setting on, L alone held -> STILL boots. The POSITIVE
                   CONTROL for arms 2-3: it proves they were stopped by the
                   hold policy rather than by the mere presence of the test
                   variable, and pins the "a pad resting in a bag is not a
                   request" rule.
6. **late hold**   setting on, the hold appears only from launcher frame 3
                   -> the launch ARMS (main()'s pre-frame sample saw nothing)
                   and then disarms without ever dispatching. This is the arm
                   a one-shot sample fails: SDL folds keyboard state from
                   events, so a Shift already down before the window existed is
                   invisible to a single sample taken at window creation -- on
                   macOS the first thing SDL learns about that key is its
                   RELEASE. `armed=1 dispatched=0` is a signature only
                   per-frame sampling can produce; the one-shot sampler gives
                   `armed=0` for arm 2 and `armed=1 dispatched=1` here.
7. **bad ROM**     setting on, remembered file is not a ROM -> armed, but
                   never dispatched and no Play action. The direct boot cannot
                   route around the ROM check.
8. **recovery**    setting on, a valid ROM, and a failed previous boot's
                   message inherited -> armed, but never dispatched. This is
                   the one that matters most and is the least obvious: a boot
                   that fails relaunches the app with MDKR_APP_BOOT_RECOVERY
                   set, so an arm that booted past that card would be an
                   infinite relaunch loop with the message flashing past every
                   time.

`MDKR_APP_TEST_LAUNCH_HOLD` injects the RAW hold (`shift`, `shoulders`,
`left-shoulder`, `right-shoulder`, any of them suffixed `@<sample>` to make it
appear only from that sample onward), not the decision, so
`AppUi_launcherHoldOpensLauncher()` still runs for real in every arm above --
arm 5 in particular is a case no automated run could produce with a real hand.
The pure policy itself, including every readiness field, is unit-tested in
`tests/test_app_ui_policy.cpp`; this gate is about the wiring.

Every run is muted (`MDKR_AUDIO=0`), offscreen (`MDKR64_HIDDEN=1`), and pinned
to a private prefs/config/save root, per tests/README.md. Exit 0 = pass.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

FRAMES = 4
# Which launcher frame the late hold appears on. main()'s pre-frame sample is 0
# and the launcher's frames continue from 1, so this is comfortably after the
# decision has armed and far before the ROM hash settles and the boot could
# dispatch (a real run reaches the dispatch around sample 45).
LATE_HOLD_SAMPLE = 3

DECISION_RE = re.compile(
    r"^\[app\] skip-launcher setting=(\d) shift=(\d) shoulderL=(\d) "
    r"shoulderR=(\d) holdOpensLauncher=(\d) armed=(\d)$", re.MULTILINE)
DISPATCH_RE = re.compile(
    r"^\[app-ui\] skip-launcher direct boot requested rom=(.*) "
    r"finalCheck=(\d)$", re.MULTILINE)
DISARM_RE = re.compile(
    r"^\[app-ui\] skip-launcher disarmed by hold sample=(\d+) shift=(\d) "
    r"shoulderL=(\d) shoulderR=(\d)$", re.MULTILINE)
REPORT_RE = re.compile(
    r"^\[app\] smoke: skip-launcher armed=(\d) dispatched=(\d) "
    r"serviceFrames=(\d+) actions=(\d+) actionRom=(.*) romPath=(.*) "
    r"romValid=(\d) settled=(\d)$", re.MULTILINE)
BAD_RE = re.compile(
    r"\[CRASH\]|\[FATAL\]|AddressSanitizer|UndefinedBehaviorSanitizer|"
    r"runtime error:|Assertion failed")

# Right size band for a file somebody might point at, wrong magic in every
# byte order dkr_rom_normalize_byte_order() checks.
GARBAGE_BYTES = (b"not-an-n64-rom-skip-launcher-fixture-" * 4)[:128]


class GateFailure(RuntimeError):
    """A failure whose message is the finding, not a stack trace."""


class Arm:
    """One launch, and everything the app said about its own decision."""

    def __init__(self, name: str, returncode: int, output: str):
        self.name = name
        self.returncode = returncode
        self.output = output
        decision = DECISION_RE.search(output)
        if decision is None:
            raise GateFailure(
                f"{name}: the app never logged its skip-launcher decision. "
                "Without that line a player who turned the setting on has no "
                "way to find out why it did not boot.\n" + output[-2000:])
        (setting, shift, shoulder_l, shoulder_r, hold, armed) = decision.groups()
        self.setting = int(setting)
        self.shift = int(shift)
        self.shoulder_l = int(shoulder_l)
        self.shoulder_r = int(shoulder_r)
        self.hold_opens_launcher = int(hold)
        self.armed = int(armed)

        report = REPORT_RE.search(output)
        if report is None:
            raise GateFailure(
                f"{name}: the launcher smoke printed no skip-launcher "
                "report\n" + output[-2000:])
        (r_armed, dispatched, service_frames, actions, action_rom, rom_path,
         rom_valid, settled) = report.groups()
        self.report_armed = int(r_armed)
        self.dispatched = int(dispatched)
        self.service_frames = int(service_frames)
        self.actions = int(actions)
        self.action_rom = action_rom
        self.rom_path = rom_path
        self.rom_valid = int(rom_valid)
        self.settled = int(settled)

        dispatch = DISPATCH_RE.search(output)
        self.dispatch_rom = dispatch.group(1) if dispatch else None
        self.final_check = int(dispatch.group(2)) if dispatch else None

        disarm = DISARM_RE.search(output)
        self.disarm_sample = int(disarm.group(1)) if disarm else None


def run_arm(binary: Path, root: Path, name: str, rom: Path,
            settings_on: bool, hold: str | None, recovery: str | None,
            timeout: int, verbose: bool) -> Arm:
    home = root / name
    prefs = home / "prefs"
    saves = home / "saves"
    prefs.mkdir(parents=True, exist_ok=True)
    saves.mkdir(parents=True, exist_ok=True)
    # The remembered ROM, written exactly as AppConfig::save() writes it.
    (prefs / "mdkr64_app.ini").write_text(f"rom_path={rom}\n", encoding="utf-8")
    config = home / "video.ini"
    # Through the settings FILE, not the environment: an environment override
    # pins a key above runtime precedence and draws it disabled, which would
    # take the very control this gate is about out of a player's reach.
    config.write_text(
        "[Launcher]\nSkipWhenReady=1\n" if settings_on else "", encoding="utf-8")

    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MDKR", "GE007_"))}
    env.update(
        LC_ALL="C",
        MDKR_AUDIO="0",
        MDKR64_HIDDEN="1",
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_VIDEO_CONFIG_PATH=str(config),
        # The scrub above also drops the suite's per-task MDKR_SAVE_DIR. An
        # unpinned save resolves to the SHARED per-user directory (issue #54),
        # so every arm gets its own.
        MDKR_SAVE_DIR=str(saves),
        MDKR_APP_SMOKE_FRAMES=str(FRAMES),
        MDKR_APP_SMOKE_SKIP_LAUNCHER="1",
    )
    if hold is not None:
        env["MDKR_APP_TEST_LAUNCH_HOLD"] = hold
    if recovery is not None:
        env["MDKR_APP_BOOT_RECOVERY"] = recovery
    if verbose:
        print(f"$ [{name}] hold={hold} setting={int(settings_on)} {binary}",
              flush=True)
    completed = subprocess.run(
        [str(binary)], env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout, check=False)
    output = completed.stdout or ""
    if completed.returncode != 0:
        raise GateFailure(
            f"{name}: the app exited {completed.returncode}\n{output[-3000:]}")
    marker = BAD_RE.search(output)
    if marker is not None:
        raise GateFailure(f"{name}: {marker.group(0)} in the run\n{output[-3000:]}")
    return Arm(name, completed.returncode, output)


def check_boots(arm: Arm, rom: Path) -> list[str]:
    """The feature: it starts the game the player last played."""
    problems: list[str] = []
    if arm.armed != 1 or arm.report_armed != 1:
        problems.append(
            f"{arm.name}: the setting is on and nothing was held, but the "
            f"launch did not arm (setting={arm.setting} "
            f"hold={arm.hold_opens_launcher} armed={arm.armed})")
    if arm.dispatched != 1:
        problems.append(
            f"{arm.name}: armed, the remembered ROM settled valid "
            f"(settled={arm.settled} romValid={arm.rom_valid}), and the "
            "direct boot never asked to play")
    if arm.dispatch_rom != str(rom):
        problems.append(
            f"{arm.name}: the direct boot asked to play {arm.dispatch_rom!r}, "
            f"not the remembered {str(rom)!r}")
    if arm.final_check != 1:
        problems.append(
            f"{arm.name}: the direct boot published without starting the "
            "mandatory final ROM check (finalCheck="
            f"{arm.final_check}). It must press the same Play a player would.")
    if arm.actions != 1:
        problems.append(
            f"{arm.name}: expected exactly one Play action, got {arm.actions}")
    if arm.action_rom != str(rom):
        problems.append(
            f"{arm.name}: the Play action carried {arm.action_rom!r}, not the "
            f"remembered {str(rom)!r}")
    return problems


def check_stays(arm: Arm, expect_armed: int, expect_dispatched: int,
                reason: str, expect_still_armed: int | None = None) -> list[str]:
    """A launch that must leave the player in their launcher.

    `expect_armed` is the LAUNCH DECISION main() logged; `expect_still_armed`
    is whether the launcher was still armed when it finished. They differ for
    exactly one arm -- a hold that appears after the window did, which arms and
    then disarms -- and keeping them separate is what makes that arm say
    something a one-shot sampler could not also say.
    """
    if expect_still_armed is None:
        expect_still_armed = expect_armed
    problems: list[str] = []
    if arm.armed != expect_armed:
        problems.append(
            f"{arm.name}: expected the launch decision armed={expect_armed} "
            f"({reason}), got {arm.armed} (setting={arm.setting} "
            f"shift={arm.shift} L={arm.shoulder_l} R={arm.shoulder_r} "
            f"holdOpensLauncher={arm.hold_opens_launcher})")
    if arm.report_armed != expect_still_armed:
        problems.append(
            f"{arm.name}: expected the launcher to end armed="
            f"{expect_still_armed} ({reason}), got {arm.report_armed}")
    if arm.dispatched != expect_dispatched:
        problems.append(
            f"{arm.name}: expected dispatched={expect_dispatched} ({reason}), "
            f"got {arm.dispatched}")
    if arm.actions != 0:
        problems.append(
            f"{arm.name}: {reason}, yet the launcher published "
            f"{arm.actions} Play action(s) carrying {arm.action_rom!r}. The "
            "player was taken into the game they were trying to stay out of.")
    if arm.dispatch_rom is not None:
        problems.append(
            f"{arm.name}: {reason}, yet the direct boot asked to play "
            f"{arm.dispatch_rom!r}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    binary = Path(os.path.abspath(resolve_binary(args.build)))
    rom = Path(os.path.abspath(args.rom))
    for path in (binary, rom):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    problems: list[str] = []
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-launcher-skip-") as tmp:
            root = Path(tmp)
            garbage = root / "not_a_rom.z64"
            garbage.write_bytes(GARBAGE_BYTES)

            boots = run_arm(binary, root, "boots", rom, True, None, None,
                            args.timeout, args.verbose)
            shift = run_arm(binary, root, "shift", rom, True, "shift", None,
                            args.timeout, args.verbose)
            shoulders = run_arm(binary, root, "shoulders", rom, True,
                                "shoulders", None, args.timeout, args.verbose)
            default = run_arm(binary, root, "default", rom, False, None, None,
                              args.timeout, args.verbose)
            one_shoulder = run_arm(binary, root, "one-shoulder", rom, True,
                                   "left-shoulder", None, args.timeout,
                                   args.verbose)
            late_hold = run_arm(binary, root, "late-hold", rom, True,
                                f"shift@{LATE_HOLD_SAMPLE}", None,
                                args.timeout, args.verbose)
            bad_rom = run_arm(binary, root, "bad-rom", garbage, True, None,
                              None, args.timeout, args.verbose)
            recovery = run_arm(
                binary, root, "recovery", rom, True, None,
                "The game did not start. Your ROM and settings were kept.",
                args.timeout, args.verbose)

        problems += check_boots(boots, rom)
        problems += check_stays(
            shift, 0, 0, "Shift was held while the app opened")
        problems += check_stays(
            shoulders, 0, 0, "both shoulders were held while the app opened")
        problems += check_stays(
            default, 0, 0, "the setting is off, which is the shipped default")
        # The positive control for the two hold arms above. Same variable, same
        # code path, one shoulder instead of two -- and it must boot, or those
        # arms proved only that setting a test variable stops the feature.
        problems += check_boots(one_shoulder, rom)
        if one_shoulder.hold_opens_launcher != 0:
            problems.append(
                "one-shoulder: L on its own was read as a request for the "
                "launcher. A pad resting in a bag holds one shoulder down for "
                "hours; the setting would be off for everyone who owns one.")
        # The arm a one-shot sample fails. It must ARM -- main()'s pre-frame
        # sample legitimately saw nothing -- and then disarm from a launcher
        # frame, without ever dispatching. Both halves are asserted, because
        # `armed=0` here would mean the seam leaked into the pre-frame sample
        # and the arm was proving arm 2 over again.
        problems += check_stays(
            late_hold, 1, 0,
            "the hold appeared while the launcher was on screen",
            expect_still_armed=0)
        if late_hold.disarm_sample is None:
            problems.append(
                "late-hold: the launcher never re-sampled the hold, so it "
                "never saw one that appeared after the window did. SDL folds "
                "keyboard state from events: a Shift already down before the "
                "window existed is invisible to a single sample taken at "
                "window creation, which is every real macOS hold.")
        elif late_hold.disarm_sample < LATE_HOLD_SAMPLE:
            problems.append(
                f"late-hold: disarmed at sample {late_hold.disarm_sample}, "
                f"before the hold was scripted to appear ({LATE_HOLD_SAMPLE}). "
                "The seam is not injecting what this arm thinks it is.")
        problems += check_stays(
            bad_rom, 1, 0, "the remembered file is not a ROM")
        if bad_rom.rom_valid != 0:
            problems.append(
                "bad-rom: the fixture was accepted as a ROM, so this arm "
                "proved nothing about the direct boot's ROM check")
        problems += check_stays(
            recovery, 1, 0,
            "a failed previous boot left a message for the player to read")

        # Naming the loop the recovery arm exists to prevent, so a future edit
        # that deletes the bootErrorVisible guard fails with the reason.
        if recovery.dispatched != 0:
            problems.append(
                "recovery: the direct boot ran past an inherited boot-failure "
                "message. A failed boot relaunches the app with that message, "
                "so this is an infinite relaunch loop, not one bad start.")
    except GateFailure as failure:
        print(f"FAIL launcher skip: {failure}", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired as expired:
        print(f"FAIL launcher skip: {expired}", file=sys.stderr)
        return 1

    if problems:
        for problem in problems:
            print(f"FAIL {problem}", file=sys.stderr)
        return 1
    print("PASS launcher skip: arms=8 boots=2 stays=6 controls=2")
    return 0


if __name__ == "__main__":
    sys.exit(main())
