#!/usr/bin/env python3
"""Prove the modal online-lobby takeover suppresses the offline launcher shell.

The launcher is a nav-rail + top-tabs + panel-router shell whose generic Play
button launches the OFFLINE game. While a live online session is active that
button is one tab away and always visible, so the originator could launch the
offline game "over" the room and a joiner could beach-ball the process by
launching offline into a live WebRTC session. The fix is a modal takeover: once
a session progresses past the entry/chooser the launcher renders ONLY the lobby
and the nav rail, top tabs and generic Play are suppressed, making the offline
launch unreachable.

This gate drives the deterministic fake adapter (no network) into each active
view kind through the ordinary launcher smoke loop and reads the per-frame
[app-lobby-takeover] probe. The invariant it asserts:

    whenever the online session is active at frame start, the generic offline
    Play button and the nav rail are NOT drawn, and the takeover branch ran.

It FAILS RED on the pre-takeover launcher (the shell, and thus Play, is drawn
while online is active) and PASSES GREEN once the takeover is wired. The
"entry" chooser is checked in the opposite direction: the shell must remain so a
player can still choose Create/Join.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

from harness_utils import resolve_binary


PROBE_RE = re.compile(
    r"\[app-lobby-takeover\] frame=(?P<frame>\d+) "
    r"online_active=(?P<online>[01]) took_takeover=(?P<took>[01]) "
    r"play_drawn=(?P<play>[01]) nav_drawn=(?P<nav>[01]) "
    r"view_kind=(?P<kind>\d+)"
)

# Active view kinds that must engage the takeover (fake gallery slugs).
#
# The two SELECTING-kind slugs (select-character, select-start) are intentionally
# absent: this campaign retired the launcher's per-race SELECTING selection surface
# (the ImGui character/vehicle/track combos + the ImGui-initiated Start Race), so
# that legacy preview panel no longer exists to take over. Independently, the beta
# build now arms a never-silent selection-stall view-timeout on every SELECTING
# view (beta-gated, lobby_view_model.c), which makes the SELECTING-kind gallery
# specs (timeout_present=false) unbuildable in a beta build regardless of race
# admission -- so those arms could never reach an active state here anyway. The
# takeover invariant stays covered across the ROOM/PREFLIGHT/LOADING/COUNTDOWN/
# RACING/RESULTS/RECOVERY kinds below.
ACTIVE_SLUGS = (
    "room-friends",
    "preflight",
    "loading",
    "countdown",
    "racing-direct",
    "results",
    "failure-service-unavailable",
)
# The create/join chooser must KEEP the shell so a player can still pick a path.
ENTRY_SLUG = "entry"
ADMISSION_SLUGS = frozenset({"loading", "countdown", "racing-direct", "results"})


class TakeoverError(RuntimeError):
    pass


def clean_environment(**updates: str) -> dict[str, str]:
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    environment.update(updates)
    return environment


def probe(binary: str, root: Path, slug: str, timeout: int) -> list[dict]:
    case_root = root / slug
    prefs = case_root / "prefs"
    saves = case_root / "saves"
    prefs.mkdir(parents=True)
    saves.mkdir()
    environment = clean_environment(
        LC_ALL="C",
        MDKR_APP_PREFS_DIR=str(prefs),
        MDKR_VIDEO_CONFIG_PATH=str(case_root / "video.ini"),
        MDKR_SAVE_DIR=str(saves),
        MDKR_AUDIO="0",
        MDKR64_HIDDEN="1",
        MDKR_APP_PANEL="Online Room",
        MDKR_APP_ONLINE_FAKE="1",
        MDKR_APP_ONLINE_GALLERY=slug,
        MDKR_ONLINE_ROOM_PREVIEW="1",
        MDKR_APP_LOBBY_TAKEOVER_PROBE="1",
        MDKR_APP_SMOKE_FRAMES="8",
    )
    if slug in ADMISSION_SLUGS:
        environment["MDKR_APP_ONLINE_FAKE_ALLOW_START"] = "1"
    completed = subprocess.run(
        [binary], env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout,
    )
    if completed.returncode != 0:
        raise TakeoverError(
            f"{slug}: process exited {completed.returncode}\n"
            f"{completed.stdout[-4000:]}"
        )
    rows = [match.groupdict() for match in PROBE_RE.finditer(completed.stdout)]
    if not rows:
        raise TakeoverError(
            f"{slug}: no [app-lobby-takeover] probe rows were emitted\n"
            f"{completed.stdout[-4000:]}"
        )
    return rows


def check_active(binary: str, root: Path, slug: str, timeout: int) -> None:
    rows = probe(binary, root, slug, timeout)
    active_rows = [r for r in rows if r["online"] == "1"]
    if not active_rows:
        raise TakeoverError(
            f"{slug}: the fake session never reached an active state "
            "(online_active stayed 0)"
        )
    for row in active_rows:
        if row["play"] == "1":
            raise TakeoverError(
                f"{slug}: the generic offline Play button was drawn while the "
                f"online session was active (frame {row['frame']}) -- the "
                "offline launch is reachable during a live session"
            )
        if row["nav"] == "1":
            raise TakeoverError(
                f"{slug}: the launcher nav rail/top tabs were drawn while the "
                f"online session was active (frame {row['frame']})"
            )
        if row["took"] != "1":
            raise TakeoverError(
                f"{slug}: online was active but the modal lobby takeover did "
                f"not engage (frame {row['frame']})"
            )


def check_entry(binary: str, root: Path, slug: str, timeout: int) -> None:
    rows = probe(binary, root, slug, timeout)
    for row in rows:
        if row["online"] == "1" or row["took"] == "1":
            raise TakeoverError(
                f"{slug}: the entry/chooser must NOT take over the launcher "
                f"(frame {row['frame']})"
            )
    if not any(row["play"] == "1" for row in rows):
        raise TakeoverError(
            f"{slug}: the launcher shell (generic Play) was never drawn at the "
            "chooser -- the player has no way to reach it"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    binary = resolve_binary(args.build)
    if not Path(binary).exists():
        print(f"FAIL online lobby takeover: binary not found: {binary}")
        return 1
    try:
        with tempfile.TemporaryDirectory(prefix="mdkr-lobby-takeover-") as tmp:
            root = Path(tmp)
            for slug in ACTIVE_SLUGS:
                check_active(binary, root, slug, args.timeout)
            check_entry(binary, root, ENTRY_SLUG, args.timeout)
    except (TakeoverError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL online lobby takeover: {error}")
        return 1
    print(
        "PASS online lobby takeover: "
        f"active-cases={len(ACTIVE_SLUGS)} entry-shell=1 "
        "offline-play-suppressed=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
