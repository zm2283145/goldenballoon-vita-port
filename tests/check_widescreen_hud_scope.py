#!/usr/bin/env python3
"""Confine the opt-in widescreen HUD offset to the HUD (issues #50 and #51).

The widescreen HUD (Video.WidescreenHUD, off by default) edge-anchors the race
counters and minimap by swapping in a left-anchored, presentation-wide ortho
(WIDE_HUD draw space) and by shifting each opted-in HUD element.  This gate
pins the source shape of that feature:

  1. the per-element anchor classification is a mode-aware TABLE
     (sHudWidescreenAnchor) that is TOTAL over enum HudTypes and matches the
     golden (element, mode) -> anchor snapshot below, so a new HUD element or
     a new per-mode reposition cannot dodge classification silently (#51),
     and the #50 groups (banana counter with its lap-counter neighbour) keep
     riding the race-mode right edge together;

  2. every DRAW-side consumer of the race-start slide (gHudOffsetX) in the
     NATIVE_PORT build goes through hud_slide_draw_offset(), the one helper
     that maps the authored 0..320 sweep onto the WIDE_HUD canvas; a raw
     `gHudOffsetX` in a draw path re-creates the "HUD parked at the right
     edge" defect (#51) and fails this census;

  3. the standard centered ortho is restored at the HUD -> dialogue-box
     boundary, gated so the widescreen-HUD-off default path is untouched, and
     that restore is confined to gGameMode == GAMEMODE_INGAME (#50).

This is a regex-over-source gate (the check_ci_contract.py / party origin-gate
house style), so it needs neither the ROM nor the GPU lane and stays green in
the unit-test tier.  The rendered-pixel witnesses for the same feature live in
check_widescreen_hud_layers.py.

A self-test replays every assertion against a synthesised pre-fix/broken
source to prove each actually fails red, matching the origin-gate discipline.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_UI = ROOT / "game" / "src" / "game_ui.c"
GAME_UI_H = ROOT / "game" / "src" / "game_ui.h"
THREAD3 = ROOT / "game" / "src" / "thread3_main.c"

MODES = ("race", "time trial", "boss", "challenge", "hub")

# Golden (element, mode) -> anchor snapshot: one string per element, one
# letter per mode column in table order (race, TT, boss, challenge, hub).
# Any table edit must consciously edit this snapshot in the same review.
GOLDEN_ANCHORS = {
    "HUD_RACE_POSITION": "LLLLL",
    "HUD_RACE_POSITION_END": "LLLLL",
    "HUD_WEAPON_DISPLAY": "LLLLL",
    # TT moves the lap counter group to the authored top-left (-58).
    "HUD_LAP_COUNT_LABEL": "RLRRR",
    "HUD_LAP_COUNT_CURRENT": "RLRRR",
    "HUD_LAP_COUNT_SEPERATOR": "RLRRR",
    "HUD_LAP_COUNT_TOTAL": "RLRRR",
    "HUD_LAP_COUNT_FLAG": "RLRRR",
    # Boss races move the banana counter group to the authored top-left
    # (-120). In the banana-collection challenge (Smokey Castle,
    # hud_main_treasure) the counter sits in a gap inside the centered
    # portrait strip and rides the CENTER anchor with it (issue #57);
    # everywhere else (race, hub) the whole group (incl. the #50 "x" glyph)
    # rides the right edge with the lap counter.
    "HUD_BANANA_COUNT_ICON_SPIN": "RRLCR",
    "HUD_BANANA_COUNT_NUMBER_1": "RRLCR",
    "HUD_BANANA_COUNT_NUMBER_2": "RRLCR",
    "HUD_BANANA_COUNT_X": "RRLCR",
    "HUD_BANANA_COUNT_ICON_STATIC": "RRLCR",
    "HUD_BANANA_COUNT_SPARKLE": "RRLCR",
    "HUD_RACE_TIME_LABEL": "RRRRR",
    "HUD_RACE_TIME_NUMBER": "RRRRR",
    "HUD_RACE_START_GO": "CCCCC",
    "HUD_RACE_START_READY": "CCCCC",
    "HUD_RACE_END_FINISH": "CCCCC",
    "HUD_MINIMAP_MARKER": "CCCCC",
    "HUD_MAGNET_RETICLE": "CCCCC",
    "HUD_BALLOON_COUNT_ICON": "LLLLL",
    "HUD_BALLOON_COUNT_X": "LLLLL",
    "HUD_BALLOON_COUNT_NUMBER_1": "LLLLL",
    "HUD_BALLOON_COUNT_NUMBER_2": "LLLLL",
    # x-origin of the TT per-lap rows (transient glyphs; right block).
    "HUD_LAP_TIME_TEXT": "CRCCC",
    "HUD_TIME_TRIAL_LAP_TEXT": "RRRRR",
    "HUD_TIME_TRIAL_LAP_NUMBER": "RRRRR",
    "HUD_STOPWATCH_HANDS": "LLLLL",
    "HUD_LAP_TEXT_FINAL": "CCCCC",
    "HUD_LAP_TEXT_LAP": "CCCCC",
    "HUD_LAP_TEXT_TWO": "CCCCC",
    # The one-player challenge arenas stride these across an authored
    # full-width, visually centered strip.
    "HUD_TREASURE_METRE": "LLLCL",
    "HUD_COURSE_ARROWS": "CCCCC",
    "HUD_STOPWATCH": "LLLLL",
    "HUD_WRONGWAY_1": "CCCCC",
    "HUD_WRONGWAY_2": "CCCCC",
    "HUD_PRO_AM_LOGO": "CCCCC",
    "HUD_SPEEDOMETRE_ARROW": "RRRRR",
    "HUD_SPEEDOMETRE_0": "RRRRR",
    "HUD_SPEEDOMETRE_30": "RRRRR",
    "HUD_SPEEDOMETRE_60": "RRRRR",
    "HUD_SPEEDOMETRE_90": "RRRRR",
    "HUD_SPEEDOMETRE_120": "RRRRR",
    "HUD_SPEEDOMETRE_150": "RRRRR",
    "HUD_SPEEDOMETRE_BG": "RRRRR",
    "HUD_SILVER_COIN_TALLY": "LLLLL",
    "HUD_CHALLENGE_FINISH_POS_1": "CCCCC",
    "HUD_CHALLENGE_FINISH_POS_2": "CCCCC",
    "HUD_WEAPON_QUANTITY": "LLLLL",
    "HUD_CHALLENGE_PORTRAIT": "LLLCL",
    "HUD_EGG_CHALLENGE_ICON": "LLLCL",
    "HUD_BATTLE_BANANA_ICON": "LLLCL",
    "HUD_BATTLE_BANANA_X": "LLLCL",
    "HUD_BATTLE_BANANA_COUNT_1": "LLLCL",
    "HUD_BATTLE_BANANA_COUNT_2": "LLLCL",
    "HUD_RACE_FINISH_POS_1": "CCCCC",
    # Authored top-right corner card, hub-only (two-player Adventure).
    "HUD_TWO_PLAYER_ADV_PORTRAIT": "CCCCR",
    "HUD_RACE_FINISH_POS_2": "CCCCC",
}

# The race banana counter and the lap counter sit stacked at the top-right in
# race mode and must ride the same right edge anchor (#50); if any counter
# glyph is left behind it separates from its group and overlaps the neighbour.
BANANA_COUNTER = (
    "HUD_BANANA_COUNT_ICON_SPIN",
    "HUD_BANANA_COUNT_X",
    "HUD_BANANA_COUNT_NUMBER_1",
    "HUD_BANANA_COUNT_NUMBER_2",
    "HUD_BANANA_COUNT_ICON_STATIC",
    "HUD_BANANA_COUNT_SPARKLE",
)
LAP_COUNTER = (
    "HUD_LAP_COUNT_LABEL",
    "HUD_LAP_COUNT_CURRENT",
    "HUD_LAP_COUNT_SEPERATOR",
    "HUD_LAP_COUNT_TOTAL",
)

ANCHOR_LETTER = {
    "MDKR_HUD_ANCHOR_LEFT": "L",
    "MDKR_HUD_ANCHOR_CENTER": "C",
    "MDKR_HUD_ANCHOR_RIGHT": "R",
}

TABLE_RE = re.compile(
    r"static\s+const\s+s8\s+sHudWidescreenAnchor\s*"
    r"\[HUD_ELEMENT_COUNT\]\s*\[HUD_WIDESCREEN_MODE_COUNT\]\s*=\s*\{"
    r"(?P<body>.*?)\n\};",
    re.DOTALL,
)
ROW_RE = re.compile(
    r"\[\s*(?P<name>HUD_[A-Z0-9_]+)\s*\]\s*=\s*"
    r"(?:HUD_ANCHOR_ALL_MODES\(\s*(?P<all>MDKR_HUD_ANCHOR_[A-Z]+)\s*\)"
    r"|HUD_ANCHOR_MODES\(\s*(?P<modes>[^)]*?)\s*\))",
    re.DOTALL,
)
ENUM_RE = re.compile(r"enum\s+HudTypes\s*\{(?P<body>.*?)HUD_ELEMENT_COUNT",
                     re.DOTALL)

# Functions that legitimately touch gHudOffsetX in the NATIVE_PORT build:
# the draw-side helper itself and the authoritative writers.
SLIDE_OWNERS = {
    "hud_slide_draw_offset",   # the one draw-side mapping
    "hud_init_element",        # authoritative park/reset writes
    "hud_player_tick",         # authoritative slide state machine
}

# The restore call must sit directly inside an in-game mode guard so it only
# fires where the expanded WIDE_HUD ortho can actually have been emitted.
INGAME_GUARD_RE = re.compile(
    r"if\s*\(\s*gGameMode\s*==\s*GAMEMODE_INGAME\s*\)\s*\{\s*"
    r"hud_widescreen_restore_screen_ortho\s*\(",
)


def enum_elements(header: str) -> list[str]:
    match = ENUM_RE.search(header)
    if not match:
        return []
    return re.findall(r"\b(HUD_[A-Z0-9_]+)\b", match.group("body"))


def parse_anchor_table(game_ui: str) -> tuple[dict[str, str], list[str]]:
    """Map each table row to its five-mode anchor-letter string."""
    problems: list[str] = []
    match = TABLE_RE.search(game_ui)
    if not match:
        return {}, ["sHudWidescreenAnchor[HUD_ELEMENT_COUNT]"
                    "[HUD_WIDESCREEN_MODE_COUNT] table not found in game_ui.c"]
    rows: dict[str, str] = {}
    for row in ROW_RE.finditer(match.group("body")):
        name = row.group("name")
        if row.group("all"):
            anchors = [row.group("all")] * len(MODES)
        else:
            anchors = [item.strip() for item in
                       row.group("modes").split(",") if item.strip()]
        if len(anchors) != len(MODES) or any(
                anchor not in ANCHOR_LETTER for anchor in anchors):
            problems.append(f"{name}: row does not name exactly "
                            f"{len(MODES)} MDKR_HUD_ANCHOR_* values")
            continue
        if name in rows:
            problems.append(f"{name}: duplicate classification row")
            continue
        rows[name] = "".join(ANCHOR_LETTER[anchor] for anchor in anchors)
    return rows, problems


def check_anchor_table(game_ui: str, header: str) -> list[str]:
    rows, problems = parse_anchor_table(game_ui)
    if not rows:
        return problems

    elements = enum_elements(header)
    if not elements:
        return problems + ["enum HudTypes not found in game_ui.h"]

    # Totality: an element added to the enum without a conscious
    # classification row is exactly how a layout dodges the anchor design.
    for name in elements:
        if name not in rows:
            problems.append(
                f"{name}: HUD element has no sHudWidescreenAnchor "
                "classification row (every element must be classified "
                "explicitly, for every mode)")
    for name in rows:
        if name not in set(elements):
            problems.append(
                f"{name}: classification row does not match any enum "
                "HudTypes element")

    # Golden snapshot: any anchor edit is a conscious diff here too.
    for name, expected in GOLDEN_ANCHORS.items():
        got = rows.get(name)
        if got is not None and got != expected:
            problems.append(
                f"{name}: anchors {got} differ from the golden snapshot "
                f"{expected} (mode order: {', '.join(MODES)})")
    for name in rows:
        if name not in GOLDEN_ANCHORS:
            problems.append(
                f"{name}: no golden snapshot entry; add one consciously")

    # The #50 contract, kept as its own named assertion: the race banana
    # counter rides the lap counter's race-mode right anchor as one group.
    lap_anchors = {rows.get(name, "?????")[0] for name in LAP_COUNTER}
    if lap_anchors != {"R"}:
        problems.append(
            f"lap counter must be a single race-mode right anchor, got "
            f"{sorted(lap_anchors)}")
    for name in BANANA_COUNTER:
        if rows.get(name, "?????")[0] != "R":
            problems.append(
                f"{name} must ride the race-mode right anchor with its "
                f"counter group, got {rows.get(name)!r}")

    # The #57 contract, its own named assertion: in the banana-collection
    # challenge (hud_main_treasure, e.g. Smokey Castle) the counter is
    # authored into a gap inside the centered portrait strip, so in challenge
    # mode every counter glyph must ride the same CENTER anchor as
    # HUD_CHALLENGE_PORTRAIT -- a RIGHT anchor there drove the counter onto
    # the portraits.
    challenge = MODES.index("challenge")
    portrait_challenge = rows.get("HUD_CHALLENGE_PORTRAIT", "?????")[challenge]
    if portrait_challenge != "C":
        problems.append(
            f"HUD_CHALLENGE_PORTRAIT must ride the challenge-mode CENTER "
            f"anchor (the centered portrait strip), got {portrait_challenge!r}")
    for name in BANANA_COUNTER:
        if rows.get(name, "?????")[challenge] != "C":
            problems.append(
                f"{name} must ride the challenge-mode CENTER anchor with the "
                f"portrait strip (issue #57), got "
                f"{rows.get(name, '?????')[challenge]!r}")
    return problems


def native_port_view(source: str) -> str:
    """The NATIVE_PORT-defined preprocessed view of a game source.

    #ifdef NATIVE_PORT keeps its body and drops its #else; #ifndef NATIVE_PORT
    does the opposite.  Every other conditional keeps BOTH branches (VERSION /
    REGION toggles do not gate the port's draw paths, and keeping both sides
    is conservative for a census).  Nesting is honoured.
    """
    kept: list[str] = []
    stack: list[tuple[bool, bool]] = []  # (keep_now, keep_after_else)
    for line in source.splitlines():
        directive = line.strip()
        if directive.startswith("#ifdef NATIVE_PORT"):
            stack.append((True, False))
            continue
        if directive.startswith("#ifndef NATIVE_PORT"):
            stack.append((False, True))
            continue
        if directive.startswith(("#if", "#ifdef", "#ifndef")):
            stack.append((True, True))
            continue
        if directive.startswith("#elif"):
            if stack:
                keep_now, keep_else = stack[-1]
                stack[-1] = (keep_else, keep_else)
            continue
        if directive.startswith("#else"):
            if stack:
                keep_now, keep_else = stack[-1]
                stack[-1] = (keep_else, keep_now)
            continue
        if directive.startswith("#endif"):
            if stack:
                stack.pop()
            continue
        if all(keep for keep, _ in stack):
            kept.append(line)
    return "\n".join(kept)


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)),
                    source, flags=re.DOTALL)
    source = re.sub(r"//[^\n]*", "", source)
    source = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', source)
    return source


def enclosing_functions(source: str) -> list[tuple[str, int, int]]:
    """(name, start, end) spans of zero-indented function definitions."""
    spans: list[tuple[str, int, int]] = []
    for match in re.finditer(
            r"^(?:static\s+)?[A-Za-z_][A-Za-z0-9_ *]*?\b"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*?\)\s*\{",
            source, re.MULTILINE | re.DOTALL):
        name = match.group(1)
        depth = 0
        index = match.end() - 1
        while index < len(source):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        spans.append((name, match.start(), index))
    return spans


def check_slide_census(game_ui: str) -> list[str]:
    """Every NATIVE_PORT draw-side gHudOffsetX consumer uses the helper."""
    problems: list[str] = []
    view = strip_comments(native_port_view(game_ui))
    if "hud_slide_draw_offset" not in view:
        return ["hud_slide_draw_offset() not defined in the NATIVE_PORT "
                "build of game_ui.c"]
    spans = enclosing_functions(view)
    for match in re.finditer(r"\bgHudOffsetX\b", view):
        owner = None
        for name, start, end in spans:
            if start <= match.start() <= end:
                owner = name
                break
        if owner is None:
            continue  # the file-scope declaration
        if owner not in SLIDE_OWNERS:
            line = view.count("\n", 0, match.start()) + 1
            problems.append(
                f"raw gHudOffsetX consumer in {owner}() (NATIVE_PORT view "
                f"line {line}): draw-side slide reads must go through "
                "hud_slide_draw_offset() so the authored 0..320 sweep maps "
                "onto the WIDE_HUD canvas")
    return problems


def check_ortho_restore(game_ui: str, thread3: str) -> list[str]:
    problems: list[str] = []

    # The restore helper must exist and must gate on the widescreen HUD being
    # active, so the default (off) path emits nothing new.
    helper = re.search(
        r"void\s+hud_widescreen_restore_screen_ortho\s*\([^)]*\)\s*\{"
        r"(?P<body>.*?)\n\}",
        game_ui, re.DOTALL)
    if not helper:
        return ["hud_widescreen_restore_screen_ortho() not defined in game_ui.c"]
    body = helper.group("body")
    if "hud_widescreen_enabled()" not in body:
        problems.append(
            "hud_widescreen_restore_screen_ortho() must gate on "
            "hud_widescreen_enabled() so the widescreen-HUD-off path is untouched")
    if "mtx_ortho(" not in body:
        problems.append(
            "hud_widescreen_restore_screen_ortho() must re-establish the "
            "standard mtx_ortho() screen space")

    # It must run at the HUD -> dialogue-box boundary, before the boxes draw.
    restore = thread3.find("hud_widescreen_restore_screen_ortho")
    boxes = thread3.find("render_dialogue_boxes")
    if restore < 0:
        problems.append(
            "thread3_main.c must call hud_widescreen_restore_screen_ortho() "
            "before render_dialogue_boxes()")
    elif boxes < 0 or restore > boxes:
        problems.append(
            "hud_widescreen_restore_screen_ortho() must be called before "
            "render_dialogue_boxes() in thread3_main.c")

    # And it must be confined to in-game: the expanded WIDE_HUD ortho is only
    # ever emitted from mode_game, so restoring it in the intro/menu/lockup
    # modes is wasted emission that also forces SAFE_2D onto the shared
    # render_dialogue_boxes path used by menu dialogue (#50 follow-up).
    if restore >= 0 and not INGAME_GUARD_RE.search(thread3):
        problems.append(
            "hud_widescreen_restore_screen_ortho() must be confined to "
            "gGameMode == GAMEMODE_INGAME, the only mode that emits the "
            "expanded WIDE_HUD ortho")
    return problems


def run(game_ui: str, header: str, thread3: str) -> list[str]:
    return (check_anchor_table(game_ui, header)
            + check_slide_census(game_ui)
            + check_ortho_restore(game_ui, thread3))


def self_test() -> int:
    """Prove every assertion rejects a synthesised pre-fix/broken source."""
    game_ui = GAME_UI.read_text()
    header = GAME_UI_H.read_text()
    thread3 = THREAD3.read_text()

    # Regression 1 (#50 shape): the banana "x" row dropped from the table --
    # both totality and the counter-group assertion must catch it.
    broken_ui = re.sub(
        r"\n\s*\[HUD_BANANA_COUNT_X\]\s*=\s*HUD_ANCHOR_MODES\([^)]*\),",
        "", game_ui, count=1)
    if broken_ui == game_ui:
        print("self-test: FAIL -- could not synthesise the banana-x regression",
              file=sys.stderr)
        return 1
    if not check_anchor_table(broken_ui, header):
        print("self-test: FAIL -- anchor table check passed a broken source",
              file=sys.stderr)
        return 1

    # Regression 2 (#51 totality): a brand-new HUD element added to the enum
    # without a classification row must fail, or a future element dodges the
    # mode-aware design silently.
    grown_header = header.replace(
        "\n    HUD_ELEMENT_COUNT",
        "\n    HUD_SELF_TEST_NEW_ELEMENT,\n    HUD_ELEMENT_COUNT")
    if grown_header == header:
        print("self-test: FAIL -- could not synthesise the new-element "
              "regression", file=sys.stderr)
        return 1
    if not any("HUD_SELF_TEST_NEW_ELEMENT" in problem
               for problem in check_anchor_table(game_ui, grown_header)):
        print("self-test: FAIL -- totality check passed an unclassified "
              "new element", file=sys.stderr)
        return 1

    # Regression 3 (#51 golden drift): silently re-anchoring one mode cell
    # (challenge portrait strip back to the pre-fix LEFT) must fail the
    # golden snapshot.
    drifted_ui = game_ui.replace(
        "    [HUD_CHALLENGE_PORTRAIT] = HUD_ANCHOR_MODES(\n"
        "        MDKR_HUD_ANCHOR_LEFT, MDKR_HUD_ANCHOR_LEFT, MDKR_HUD_ANCHOR_LEFT,\n"
        "        MDKR_HUD_ANCHOR_CENTER, MDKR_HUD_ANCHOR_LEFT),",
        "    [HUD_CHALLENGE_PORTRAIT] = "
        "HUD_ANCHOR_ALL_MODES(MDKR_HUD_ANCHOR_LEFT),")
    if drifted_ui == game_ui:
        print("self-test: FAIL -- could not synthesise the golden-drift "
              "regression", file=sys.stderr)
        return 1
    if not any("golden snapshot" in problem
               for problem in check_anchor_table(drifted_ui, header)):
        print("self-test: FAIL -- golden snapshot passed a re-anchored table",
              file=sys.stderr)
        return 1

    # Regression 7 (#57): the banana counter re-anchored to RIGHT in challenge
    # mode -- its pre-fix shape, which drove the counter onto the centered
    # portrait strip in the banana-collection challenge -- must fail the
    # challenge-mode contract.
    prefix57_ui = game_ui.replace(
        "    [HUD_BANANA_COUNT_ICON_SPIN] = HUD_ANCHOR_MODES(\n"
        "        MDKR_HUD_ANCHOR_RIGHT, MDKR_HUD_ANCHOR_RIGHT, "
        "MDKR_HUD_ANCHOR_LEFT,\n"
        "        MDKR_HUD_ANCHOR_CENTER, MDKR_HUD_ANCHOR_RIGHT),",
        "    [HUD_BANANA_COUNT_ICON_SPIN] = HUD_ANCHOR_MODES(\n"
        "        MDKR_HUD_ANCHOR_RIGHT, MDKR_HUD_ANCHOR_RIGHT, "
        "MDKR_HUD_ANCHOR_LEFT,\n"
        "        MDKR_HUD_ANCHOR_RIGHT, MDKR_HUD_ANCHOR_RIGHT),", 1)
    if prefix57_ui == game_ui:
        print("self-test: FAIL -- could not synthesise the issue-57 regression",
              file=sys.stderr)
        return 1
    if not any("issue #57" in problem
               for problem in check_anchor_table(prefix57_ui, header)):
        print("self-test: FAIL -- challenge-mode contract passed a "
              "RIGHT-anchored banana counter", file=sys.stderr)
        return 1

    # Regression 4 (#51 slide census): a draw site consuming the raw slide
    # again (the pre-fix hud_element_render shape) must fail the census.
    raw_ui = game_ui.replace(
        "        hud->pos.x += slideDrawX;",
        "        hud->pos.x += gHudOffsetX + gHudBounceX;", 1)
    if raw_ui == game_ui:
        print("self-test: FAIL -- could not synthesise the raw-slide "
              "regression", file=sys.stderr)
        return 1
    if not check_slide_census(raw_ui):
        print("self-test: FAIL -- slide census passed a raw draw-side "
              "gHudOffsetX consumer", file=sys.stderr)
        return 1

    # Regression 5 (#50): the ortho restore boundary removed.
    broken_thread3 = thread3.replace(
        "hud_widescreen_restore_screen_ortho(&gCurrDisplayList, &gGameCurrMatrix);",
        "")
    if broken_thread3 == thread3:
        print("self-test: FAIL -- could not synthesise the ortho-restore regression",
              file=sys.stderr)
        return 1
    if not check_ortho_restore(game_ui, broken_thread3):
        print("self-test: FAIL -- ortho-restore check passed a broken source",
              file=sys.stderr)
        return 1

    # Regression 6 (#50): the in-game mode guard dropped, leaving the restore
    # to fire in every mode (the pre-follow-up #50 shape).  The call still
    # runs before the dialogue boxes, so only the confinement assertion must
    # catch it.
    unguarded_thread3 = re.sub(
        r"if\s*\(\s*gGameMode\s*==\s*GAMEMODE_INGAME\s*\)\s*\{\s*"
        r"(hud_widescreen_restore_screen_ortho\s*\([^;]*;)\s*\}",
        r"\1", thread3, count=1)
    if unguarded_thread3 == thread3:
        print("self-test: FAIL -- could not synthesise the mode-guard regression",
              file=sys.stderr)
        return 1
    if INGAME_GUARD_RE.search(unguarded_thread3):
        print("self-test: FAIL -- mode-guard regression left the guard intact",
              file=sys.stderr)
        return 1
    if not check_ortho_restore(game_ui, unguarded_thread3):
        print("self-test: FAIL -- confinement check passed an unguarded source",
              file=sys.stderr)
        return 1

    print("check_widescreen_hud_scope: self-test OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                        help="only prove the assertions fail on the pre-fix source")
    args = parser.parse_args()

    # The self-test (every assertion must reject its synthesised pre-fix
    # source) runs on every invocation, so a weakened gate is a suite
    # failure -- the party origin-gate discipline.
    if self_test() != 0:
        return 1
    if args.self_test:
        return 0

    for path in (GAME_UI, GAME_UI_H, THREAD3):
        if not path.is_file():
            print(f"check_widescreen_hud_scope: FAIL -- missing {path}",
                  file=sys.stderr)
            return 1

    problems = run(GAME_UI.read_text(), GAME_UI_H.read_text(),
                   THREAD3.read_text())
    if problems:
        for problem in problems:
            print(f"check_widescreen_hud_scope: FAIL -- {problem}",
                  file=sys.stderr)
        return 1

    print("check_widescreen_hud_scope: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
