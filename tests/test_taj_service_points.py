#!/usr/bin/env python3
"""Roster deferral service-point contract (source-shaped).

The D1 scene gate defers browser persistence outcomes while the racer-bindings
window is open. That is only safe because every menu-scene transition that
closes the window also services the deferral -- a property that lives in call
sites, not in taj_mod.c's unit-testable core. The defining miss: quit-to-title
reaches menu_title_screen_init() through menu_init(MENU_TITLE) and never
passes init_title_screen_variables() (logo boot only), so a missing hook there
left bindings open across the title and options screens after a quit --
deferred outcomes sat unserviced, persistence failures surfaced a scene late,
and new stores reported busy with nothing racing.

This gate extracts function bodies with a comment/string-stripping brace walk
and asserts each transition closes the window and services the deferral. A
source-shaped oracle needs its own controls: the extractor is validated
against synthetic sources where the hook is absent, lives in the wrong
function, or is only mentioned in a comment.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MENU = ROOT / "game" / "src" / "menu.c"
TAJ_MOD = ROOT / "game" / "src" / "taj_mod.c"

# Direct reset calls in menu.c must service within this many following
# statements, so a scene transition cannot reopen a gap by drifting apart.
PAIR_WINDOW_CODE_LINES = 8

failures: list[str] = []


def fail(message: str) -> None:
    failures.append(message)


def strip_comments_and_strings(source: str) -> str:
    """Return code with comments/literals blanked, geometry preserved."""
    out: list[str] = []
    i = 0
    n = len(source)
    state = "code"  # code | block | line | dquote | squote
    while i < n:
        ch = source[i]
        nxt = source[i + 1] if i + 1 < n else ""
        if state == "code":
            if ch == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if ch == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if ch == '"':
                state = "dquote"
                out.append(" ")
                i += 1
                continue
            if ch == "'":
                state = "squote"
                out.append(" ")
                i += 1
                continue
            out.append(ch)
            i += 1
            continue
        if state == "block":
            if ch == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if ch == "\n" else " ")
            i += 1
            continue
        if state == "line":
            if ch == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        # String/char literals: keep newlines, blank content, honour escapes.
        if ch == "\\" and i + 1 < n:
            out.append("  ")
            i += 2
            continue
        if (state == "dquote" and ch == '"') or (
            state == "squote" and ch == "'"
        ):
            state = "code"
            out.append(" ")
            i += 1
            continue
        out.append("\n" if ch == "\n" else " ")
        i += 1
    return "".join(out)


def function_body(code: str, name: str, where: str) -> str | None:
    """Extract the brace-matched body of `void name(void)` from stripped code."""
    marker = f"void {name}(void)"
    start = code.find(marker)
    if start < 0:
        fail(f"{where}: definition of {name} not found")
        return None
    brace = code.find("{", start)
    if brace < 0:
        fail(f"{where}: no body opens for {name}")
        return None
    depth = 0
    for i in range(brace, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[brace : i + 1]
    fail(f"{where}: unbalanced braces walking {name}")
    return None


def require_call(body: str | None, name: str, callee: str, where: str) -> None:
    if body is None:
        return
    if f"{callee}(" not in body:
        fail(f"{where}: {name} must call {callee}()")


def check_reset_service_pairing(code: str, where: str) -> None:
    """Every direct selection reset in menu.c must service the deferral."""
    lines = code.splitlines()
    sites = [
        i for i, line in enumerate(lines)
        if "taj_mod_reset_player_selections(" in line
    ]
    if not sites:
        fail(f"{where}: expected at least one direct selection-reset site")
    for site in sites:
        window = []
        taken = 0
        for line in lines[site + 1 :]:
            text = line.strip()
            if not text or text.startswith("#"):
                continue
            window.append(text)
            taken += 1
            if taken >= PAIR_WINDOW_CODE_LINES:
                break
        if not any("taj_mod_service_deferred(" in text for text in window):
            fail(
                f"{where}:{site + 1}: taj_mod_reset_player_selections() is "
                "not followed by taj_mod_service_deferred() within "
                f"{PAIR_WINDOW_CODE_LINES} statements"
            )


def self_test() -> None:
    """The extractor must reject the shapes this gate exists to catch."""
    present = strip_comments_and_strings(
        "void menu_title_screen_init(void) {\n"
        "    taj_mod_on_title_return();\n"
        "}\n"
    )
    body = function_body(present, "menu_title_screen_init", "control")
    if body is None or "taj_mod_on_title_return(" not in body:
        fail("control: extractor missed a genuinely present hook")

    comment_only = strip_comments_and_strings(
        "void menu_title_screen_init(void) {\n"
        "    /* taj_mod_on_title_return(); */\n"
        "    // taj_mod_on_title_return();\n"
        "}\n"
    )
    body = function_body(comment_only, "menu_title_screen_init", "control")
    if body is not None and "taj_mod_on_title_return(" in body:
        fail("control: a commented-out hook satisfied the extractor")

    wrong_function = strip_comments_and_strings(
        "void other(void) {\n    taj_mod_on_title_return();\n}\n"
        "void menu_title_screen_init(void) {\n    gMenuDelay = 0;\n}\n"
    )
    body = function_body(wrong_function, "menu_title_screen_init", "control")
    if body is not None and "taj_mod_on_title_return(" in body:
        fail("control: a hook in another function satisfied the extractor")


def main() -> int:
    self_test()
    menu = strip_comments_and_strings(MENU.read_text(encoding="utf-8"))
    taj = strip_comments_and_strings(TAJ_MOD.read_text(encoding="utf-8"))

    # Both title entries: logo boot AND quit-to-title (menu_init(MENU_TITLE)).
    require_call(
        function_body(menu, "init_title_screen_variables", "menu.c"),
        "init_title_screen_variables", "taj_mod_on_title_return", "menu.c")
    require_call(
        function_body(menu, "menu_title_screen_init", "menu.c"),
        "menu_title_screen_init", "taj_mod_on_title_return", "menu.c")

    # The transitions taj_mod.c owns must both close the window and service.
    for name in ("taj_mod_on_title_return", "taj_mod_on_adventure_file_deleted"):
        body = function_body(taj, name, "taj_mod.c")
        require_call(body, name, "taj_mod_reset_player_selections", "taj_mod.c")
        require_call(body, name, "taj_mod_service_deferred", "taj_mod.c")

    # Direct menu.c resets (character-select confirm today) must service too.
    check_reset_service_pairing(menu, "menu.c")

    if failures:
        print("taj service points: FAIL")
        for message in failures:
            print(f"  - {message}")
        return 1
    print("taj service points: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
