#!/usr/bin/env python3
"""AP-01 static source-boundary gate for the Adventure Party feature.

Adventure Party is a separate native session policy (see
``docs/architecture/adventure-party.md``). Its central compatibility invariant
is that a party session never sets the retail two-player-adventure globals, so
every retail exact-two / lead-swap branch stays mechanically inert without an
edit. This gate keeps that boundary honest at the source level. It is ROM-free,
needs no build, and runs standalone::

    python3 tests/check_adventure_party_boundaries.py

Exit 0 on pass, nonzero with precise messages on failure. ``--self-test`` also
feeds every rule a synthetic in-memory violation (never touching the repo on
disk) and asserts the rule fires, and that a matching clean control does not.

Rules (each with a positive control in ``run_self_tests``):

  1. Inventory drift (``game/src``). Every use of
     ``is_in_two_player_adventure``, ``race_is_adventure_2P``,
     ``swap_lead_player``, ``input_swap_id``, ``gIsInTwoPlayerAdventure``,
     ``gTwoPlayerAdvRace``, or an exact player-count comparison of
     ``gNumberOfActivePlayers`` / ``gNumActivePlayers`` against an integer
     literal must be declared in ``tests/adventure_party_boundary_inventory.json``
     with a classification and a treatment. A NEW undeclared branch fails closed;
     a stale declaration a later ticket removed also fails, so the inventory
     stays complete. Entries are keyed by file + the comment-stripped,
     whitespace-normalized source line (counted), so they survive line-number
     drift.
  2. No party-path write to the retail 2P globals. A write to
     ``gIsInTwoPlayerAdventure`` / ``gTwoPlayerAdvRace`` or a set of
     ``CHEAT_TWO_PLAYER_ADVENTURE`` from adventure-party code — any file under
     ``platform/adventure_party/`` or inside a ``#ifdef NATIVE_PORT`` adapter
     region that names an ``adventure_party`` identifier nearby — fails.
  3. No online-authority leak. ``mdkr_authoritative_player_count`` must not be
     referenced from ``platform/adventure_party/**``.
  4. No ambiguous "party player count" global. A new global of the shape
     ``g...Party...Count`` in ``game/src`` or ``platform/`` fails; the design
     uses named count queries, never a new ambiguous global.
  5. No vocabulary leak. ``adventure_party`` identifiers must not appear inside
     the test-hook file ``platform/mdkr_adventure.c``/``.h`` or Phone Party
     infrastructure ``platform/party/**``.

``platform/adventure_party/`` does not exist yet (it lands in a parallel
workstream); a missing directory simply contributes no violations.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INVENTORY_PATH = ROOT / "tests" / "adventure_party_boundary_inventory.json"

CODE_EXTS = {".c", ".h", ".cpp", ".cc", ".mm", ".hpp", ".hh"}
SCAN_ROOTS = ("game/src", "platform")
ADVENTURE_PARTY_PREFIX = "platform/adventure_party/"
MDKR_ADVENTURE_FILES = {"platform/mdkr_adventure.c", "platform/mdkr_adventure.h"}
PARTY_INFRA_PREFIX = "platform/party/"

VALID_TREATMENTS = {"bypass", "adapter-seam", "inert-by-globals", "unrelated"}
PROXIMITY_LINES = 40

# --- Rule 1 detection: named symbols + exact player-count comparisons. ---
NAMED_SYMBOL_RE = re.compile(
    r"\b(is_in_two_player_adventure|race_is_adventure_2P|swap_lead_player|"
    r"input_swap_id|gIsInTwoPlayerAdventure|gTwoPlayerAdvRace)\b"
)
_COUNT_GLOBAL = r"(?:gNumberOfActivePlayers|gNumActivePlayers)"
COUNT_CMP_LEFT_RE = re.compile(
    r"\b" + _COUNT_GLOBAL + r"\b\s*(?:==|!=|>=|<=|>|<)\s*\d"
)
COUNT_CMP_RIGHT_RE = re.compile(
    r"\d\s*(?:==|!=|>=|<=|>|<)\s*\b" + _COUNT_GLOBAL + r"\b"
)

# --- Rule 2 detection: writes to the retail 2P boundary flags/cheat. ---
BOUNDARY_WRITE_RE = re.compile(
    r"\b(?:gIsInTwoPlayerAdventure|gTwoPlayerAdvRace)\b\s*(?:[-+*/%|&^]?)=(?!=)"
)
SET_CHEAT_RE = re.compile(
    r"(?<![=!<>])=(?!=)\s*(?:[^;]*\|\s*)?CHEAT_TWO_PLAYER_ADVENTURE\b"
)

# --- Rules 3-5. ---
AUTH_COUNT_RE = re.compile(r"\bmdkr_authoritative_player_count\b")
AMBIGUOUS_GLOBAL_RE = re.compile(r"\bg[A-Za-z0-9_]*Party[A-Za-z0-9_]*Count\b")
PARTY_ID_RE = re.compile(r"\badventure_party")

DIRECTIVE_RE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")


def strip_comments(text: str) -> str:
    """Replace C/C++ comment spans with spaces, preserving line structure."""
    out: list[str] = []
    i, n = 0, len(text)
    in_block = in_line = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_block:
            if c == "*" and nxt == "/":
                in_block = False
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        if in_line:
            if c == "\n":
                in_line = False
                out.append("\n")
                i += 1
                continue
            out.append(" ")
            i += 1
            continue
        if c == "/" and nxt == "*":
            in_block = True
            out.append("  ")
            i += 2
            continue
        if c == "/" and nxt == "/":
            in_line = True
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def norm(line: str) -> str:
    return re.sub(r"\s+", " ", line).strip()


def line_kinds(line: str) -> list[str]:
    """Tracked boundary pattern kinds present on one comment-stripped line."""
    kinds: list[str] = []
    for match in NAMED_SYMBOL_RE.finditer(line):
        symbol = match.group(1)
        if symbol not in kinds:
            kinds.append(symbol)
    if COUNT_CMP_LEFT_RE.search(line) or COUNT_CMP_RIGHT_RE.search(line):
        kinds.append("player-count-comparison")
    return kinds


# --------------------------------------------------------------------------- #
# Source loading
# --------------------------------------------------------------------------- #
def load_sources() -> dict[str, str]:
    sources: dict[str, str] = {}
    for base in SCAN_ROOTS:
        root = ROOT / base
        if not root.exists():
            continue
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix in CODE_EXTS:
                rel = path.relative_to(ROOT).as_posix()
                sources[rel] = path.read_text(encoding="utf-8", errors="replace")
    return sources


# --------------------------------------------------------------------------- #
# Rule 1 — inventory drift
# --------------------------------------------------------------------------- #
def occurrences_from_sources(sources: dict[str, str]) -> dict[tuple[str, str], int]:
    current: dict[tuple[str, str], int] = {}
    for path, text in sources.items():
        if not path.startswith("game/src/"):
            continue
        if not (path.endswith(".c") or path.endswith(".h")):
            continue
        for raw in strip_comments(text).splitlines():
            if not line_kinds(raw):
                continue
            key = (path, norm(raw))
            current[key] = current.get(key, 0) + 1
    return current


def load_inventory() -> tuple[dict[tuple[str, str], int], list[str]]:
    data = json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))
    expected: dict[tuple[str, str], int] = {}
    problems: list[str] = []
    for entry in data.get("entries", []):
        key = (entry["file"], entry["line"])
        expected[key] = expected.get(key, 0) + int(entry["count"])
        if entry.get("treatment") not in VALID_TREATMENTS:
            problems.append(
                f"inventory entry {entry['file']}: '{entry['line']}' has invalid "
                f"treatment {entry.get('treatment')!r} "
                f"(allowed: {sorted(VALID_TREATMENTS)})"
            )
        if not entry.get("classification"):
            problems.append(
                f"inventory entry {entry['file']}: '{entry['line']}' is missing a "
                "classification"
            )
    return expected, problems


def rule1_inventory_drift(
    current: dict[tuple[str, str], int],
    expected: dict[tuple[str, str], int],
) -> list[str]:
    failures: list[str] = []
    for key in sorted(set(current) | set(expected)):
        found = current.get(key, 0)
        declared = expected.get(key, 0)
        path, line = key
        if found > declared:
            failures.append(
                f"undeclared boundary occurrence: {path}: '{line}' appears "
                f"{found}x, inventory declares {declared}x — classify this new "
                f"Adventure count/lead branch in {INVENTORY_PATH.name}"
            )
        elif found < declared:
            failures.append(
                f"stale inventory entry: {path}: '{line}' declared {declared}x, "
                f"source has {found}x — reclassify or remove it in "
                f"{INVENTORY_PATH.name}"
            )
    return failures


# --------------------------------------------------------------------------- #
# Rule 2 — party-path writes to the retail 2P globals
# --------------------------------------------------------------------------- #
def native_guard_flags(cleaned: str) -> list[bool]:
    """Per-line: is the line inside a #ifdef NATIVE_PORT-true arm?"""
    flags: list[bool] = []
    stack: list[dict[str, object]] = []
    for raw in cleaned.splitlines():
        directive = DIRECTIVE_RE.match(raw)
        if directive:
            kind, expression = directive.group(1), directive.group(2).strip()
            expression = re.sub(r"\s+", "", expression)
            if kind in {"ifdef", "ifndef", "if"}:
                value: object = None
                if kind == "ifdef" and expression == "NATIVE_PORT":
                    value = True
                elif kind == "ifndef" and expression == "NATIVE_PORT":
                    value = False
                elif kind == "if":
                    if expression in {"defined(NATIVE_PORT)", "definedNATIVE_PORT",
                                      "NATIVE_PORT"}:
                        value = True
                    elif expression in {"!defined(NATIVE_PORT)", "!definedNATIVE_PORT",
                                        "!NATIVE_PORT"}:
                        value = False
                stack.append({"val": value})
            elif kind == "elif" and stack:
                stack[-1]["val"] = None
            elif kind == "else" and stack:
                current = stack[-1]["val"]
                stack[-1]["val"] = (not current) if isinstance(current, bool) else None
            elif kind == "endif" and stack:
                stack.pop()
            flags.append(False)
            continue
        flags.append(any(frame["val"] is True for frame in stack))
    return flags


def rule2_party_writes(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for path, text in sorted(sources.items()):
        if not (path.startswith("game/src/") or path.startswith("platform/")):
            continue
        cleaned = strip_comments(text)
        lines = cleaned.splitlines()
        flags = native_guard_flags(cleaned)
        party_lines = [i for i, line in enumerate(lines) if PARTY_ID_RE.search(line)]
        in_party_dir = path.startswith(ADVENTURE_PARTY_PREFIX)
        for i, line in enumerate(lines):
            if not (BOUNDARY_WRITE_RE.search(line) or SET_CHEAT_RE.search(line)):
                continue
            if in_party_dir:
                failures.append(
                    f"{path}:{i + 1}: adventure-party code sets a retail 2P "
                    f"boundary flag/cheat: {line.strip()}"
                )
                continue
            near_party = any(abs(i - j) <= PROXIMITY_LINES for j in party_lines)
            if flags[i] and near_party:
                failures.append(
                    f"{path}:{i + 1}: NATIVE_PORT adventure-party adapter sets a "
                    f"retail 2P boundary flag/cheat: {line.strip()}"
                )
    return failures


# --------------------------------------------------------------------------- #
# Rule 3 — online-authority boundary
# --------------------------------------------------------------------------- #
def rule3_authoritative_count(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for path, text in sorted(sources.items()):
        if not path.startswith(ADVENTURE_PARTY_PREFIX):
            continue
        for i, line in enumerate(strip_comments(text).splitlines()):
            if AUTH_COUNT_RE.search(line):
                failures.append(
                    f"{path}:{i + 1}: platform/adventure_party must not reference "
                    f"mdkr_authoritative_player_count (online-authority boundary): "
                    f"{line.strip()}"
                )
    return failures


# --------------------------------------------------------------------------- #
# Rule 4 — ambiguous party-count global
# --------------------------------------------------------------------------- #
def rule4_ambiguous_global(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for path, text in sorted(sources.items()):
        if not (path.startswith("game/src/") or path.startswith("platform/")):
            continue
        for i, line in enumerate(strip_comments(text).splitlines()):
            for match in AMBIGUOUS_GLOBAL_RE.finditer(line):
                failures.append(
                    f"{path}:{i + 1}: ambiguous party player-count global "
                    f"'{match.group(0)}' — use a named count query, never a new "
                    f"ambiguous global: {line.strip()}"
                )
    return failures


# --------------------------------------------------------------------------- #
# Rule 5 — vocabulary leak into test hooks / Phone Party
# --------------------------------------------------------------------------- #
def rule5_vocab_leak(sources: dict[str, str]) -> list[str]:
    failures: list[str] = []
    for path, text in sorted(sources.items()):
        if not (path in MDKR_ADVENTURE_FILES or path.startswith(PARTY_INFRA_PREFIX)):
            continue
        for i, line in enumerate(strip_comments(text).splitlines()):
            if PARTY_ID_RE.search(line):
                failures.append(
                    f"{path}:{i + 1}: adventure_party identifier inside "
                    f"test-hook/Phone-Party infrastructure: {line.strip()}"
                )
    return failures


# --------------------------------------------------------------------------- #
# Positive controls
# --------------------------------------------------------------------------- #
def run_self_tests(
    current: dict[tuple[str, str], int],
    expected: dict[tuple[str, str], int],
) -> list[str]:
    problems: list[str] = []

    def fires(name: str, result: list[str]) -> None:
        if not result:
            problems.append(f"self-test[{name}]: positive control did not fire")

    def clean(name: str, result: list[str]) -> None:
        if result:
            problems.append(
                f"self-test[{name}]: clean control fired unexpectedly: {result}"
            )

    # Rule 1 — a new undeclared branch, a stale entry, and the clean tree.
    new_branch = dict(current)
    new_branch[("game/src/__selftest__.c", "if (gNumberOfActivePlayers == 5) {")] = 1
    fires("rule1-new-branch", rule1_inventory_drift(new_branch, expected))
    if expected:
        dropped = next(iter(expected))
        stale = {k: v for k, v in current.items() if k != dropped}
        fires("rule1-stale-entry", rule1_inventory_drift(stale, expected))
    clean("rule1-clean", rule1_inventory_drift(current, expected))

    # Rule 2 — party dir, NATIVE_PORT adapter, cheat set; retail + bare-native clean.
    fires("rule2-party-dir", rule2_party_writes(
        {"platform/adventure_party/adventure_party_state.c":
         "void f(void) { gIsInTwoPlayerAdventure = 1; }\n"}))
    fires("rule2-native-adapter", rule2_party_writes(
        {"game/src/menu.c":
         "#ifdef NATIVE_PORT\n"
         "    if (adventure_party_is_active()) {\n"
         "        gTwoPlayerAdvRace = TRUE;\n"
         "    }\n"
         "#endif\n"}))
    fires("rule2-cheat-set", rule2_party_writes(
        {"platform/adventure_party/adventure_party_policy.c":
         "void f(void) { gActiveMagicCodes |= CHEAT_TWO_PLAYER_ADVENTURE; }\n"}))
    clean("rule2-retail-clean", rule2_party_writes(
        {"game/src/game.c": "void f(void) { gTwoPlayerAdvRace = TRUE; }\n"}))
    clean("rule2-native-noparty-clean", rule2_party_writes(
        {"game/src/game.c":
         "#ifdef NATIVE_PORT\n    gTwoPlayerAdvRace = TRUE;\n#endif\n"}))

    # Rule 3 — party reference vs the legitimate game/src reference.
    fires("rule3-party-ref", rule3_authoritative_count(
        {"platform/adventure_party/adventure_party_policy.c":
         "int f(void) { return mdkr_authoritative_player_count(2); }\n"}))
    clean("rule3-game-ref-clean", rule3_authoritative_count(
        {"game/src/object_functions.c":
         "int f(void) { return mdkr_authoritative_player_count(2); }\n"}))

    # Rule 4 — the named shape and the general shape; a plain count global clean.
    fires("rule4-named-shape", rule4_ambiguous_global(
        {"platform/adventure_party/x.c": "s32 gPartyPlayerCount = 0;\n"}))
    fires("rule4-general-shape", rule4_ambiguous_global(
        {"game/src/menu.c": "s32 gAdventurePartySeatCount = 0;\n"}))
    clean("rule4-clean", rule4_ambiguous_global(
        {"game/src/menu.c": "s32 gNumberOfActivePlayers = 1;\n"}))

    # Rule 5 — leak into the hook file and Phone Party; the real module is clean.
    fires("rule5-mdkr-hook", rule5_vocab_leak(
        {"platform/mdkr_adventure.c": "void f(void) { adventure_party_reset(); }\n"}))
    fires("rule5-party-infra", rule5_vocab_leak(
        {"platform/party/lan_party_room.cpp":
         "void f(void) { adventure_party_init(); }\n"}))
    clean("rule5-module-clean", rule5_vocab_leak(
        {"platform/adventure_party/x.c": "void f(void) { adventure_party_init(); }\n"}))

    return problems


# --------------------------------------------------------------------------- #
# Entry point
# --------------------------------------------------------------------------- #
def main(argv: list[str]) -> int:
    self_test = "--self-test" in argv[1:]
    sources = load_sources()
    expected, inventory_problems = load_inventory()
    current = occurrences_from_sources(sources)

    failures: list[str] = list(inventory_problems)
    failures += [f"[inventory] {x}" for x in rule1_inventory_drift(current, expected)]
    failures += [f"[party-write] {x}" for x in rule2_party_writes(sources)]
    failures += [f"[authority] {x}" for x in rule3_authoritative_count(sources)]
    failures += [f"[ambiguous-global] {x}" for x in rule4_ambiguous_global(sources)]
    failures += [f"[vocab-leak] {x}" for x in rule5_vocab_leak(sources)]
    if self_test:
        failures += [f"[self-test] {x}" for x in run_self_tests(current, expected)]

    if failures:
        print("FAIL: check_adventure_party_boundaries", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    total = sum(current.values())
    print(
        "check_adventure_party_boundaries: PASS — "
        f"{total} Adventure count/lead occurrences across {len(current)} "
        f"declared inventory entries; no party-path 2P writes, online-authority "
        f"leaks, ambiguous party-count globals, or test-hook/Phone-Party "
        f"vocabulary leaks"
        + (" (self-test controls all fired)" if self_test else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
