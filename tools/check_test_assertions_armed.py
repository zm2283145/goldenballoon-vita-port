#!/usr/bin/env python3
"""Fail closed when a tests/ translation unit calls assert() without assertions armed.

CMakeLists.txt defaults CMAKE_BUILD_TYPE to Release when unset, and Release
adds -DNDEBUG. Under NDEBUG, <assert.h>/<cassert> turns every assert() into
`((void)0)`: the call still compiles, the check it guards silently never
runs, and the test reports PASS no matter what it was supposed to catch. 22
test files shipped exactly this way once, fixed with a per-file `#undef
NDEBUG` before the assert.h/cassert include; a sister project repeated the
mistake with an allowlist that fell out of date as files were added. This
gate has no allowlist -- it scans every tests/*.c and tests/*.cpp TU that
calls assert( and requires ONE of two provable arming mechanisms:

  TU-armed      `#undef NDEBUG` precedes the first #include this TU has of
                <assert.h>/<cassert> -- or, if it has no direct include of
                either (assert() reaching it only through a header it pulls
                in), precedes the TU's first #include of ANY kind, since
                that header could be the one that drags assert.h in.
  target-armed  EVERY CMake target this TU is compiled into (found by
                searching cmake/tests.cmake and CMakeLists.txt
                add_executable source lists -- a TU can be linked into more
                than one target) carries a `-UNDEBUG` compile option, which
                undoes -DNDEBUG on the same command line regardless of TU
                text. A TU owned by two targets where only one carries
                -UNDEBUG still ships disarmed in the other, so ALL owners
                must be armed, not just one. `NDEBUG=0` is NOT accepted:
                <assert.h> only tests whether NDEBUG is #defined, not its
                value, so `-DNDEBUG=0` still disarms assert().

Both mechanisms are exercised by name in --self-test, which every suite run
performs.
"""

from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS_DIR = ROOT / "tests"
CMAKE_FILES = (ROOT / "cmake" / "tests.cmake", ROOT / "CMakeLists.txt")

ASSERT_CALL = re.compile(r"(?<![A-Za-z0-9_])assert\s*\(")
ASSERT_INCLUDE = re.compile(r'#\s*include\s*[<"](assert\.h|cassert)[>"]')
ANY_INCLUDE = re.compile(r"#\s*include\b")
UNDEF_NDEBUG = re.compile(r"#\s*undef\s+NDEBUG\b")
UNDEF_NDEBUG_FLAG = re.compile(r"(?<![A-Za-z0-9_-])-UNDEBUG\b")

ADD_EXECUTABLE = re.compile(r"add_executable\(\s*([A-Za-z0-9_]+)\s+(.*?)\)", re.S)
TARGET_COMPILE_CALL = re.compile(
    r"target_compile_(?:options|definitions)\(\s*([A-Za-z0-9_]+)\s+(.*?)\)", re.S)


def _stripped_lines(text: str) -> list[str]:
    """``text`` with // and /* */ comments and string/char literals blanked out,
    so a mention inside prose or a printf format string cannot fool the scan."""
    lines: list[str] = []
    in_block_comment = False
    for raw in text.splitlines():
        output: list[str] = []
        index = 0
        quote = ""
        escaped = False
        while index < len(raw):
            char = raw[index]
            following = raw[index + 1] if index + 1 < len(raw) else ""
            if in_block_comment:
                if char == "*" and following == "/":
                    in_block_comment = False
                    output.extend((" ", " "))
                    index += 2
                else:
                    output.append(" ")
                    index += 1
                continue
            if quote:
                output.append(" ")
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = ""
                index += 1
                continue
            if char == "/" and following == "*":
                in_block_comment = True
                output.extend((" ", " "))
                index += 2
            elif char == "/" and following == "/":
                output.extend(" " for _ in raw[index:])
                break
            elif char in {'"', "'"}:
                quote = char
                output.append(" ")
                index += 1
            else:
                output.append(char)
                index += 1
        lines.append("".join(output))
    return lines


def _strip_cmake_comments(text: str) -> str:
    """``text`` with CMake ``#`` line comments blanked to end of line, outside
    quoted strings. Without this a ')' inside a source-list aside -- real
    examples in cmake/tests.cmake: "(and miniz under it)", "publish() asks
    the cascade planner..." -- ends the ADD_EXECUTABLE/TARGET_COMPILE_CALL
    match early and silently drops every source listed after it."""
    lines: list[str] = []
    for raw in text.splitlines():
        output: list[str] = []
        quote = ""
        escaped = False
        for char in raw:
            if quote:
                output.append(char)
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = ""
                continue
            if char == "#":
                break
            if char == '"':
                quote = char
            output.append(char)
        lines.append("".join(output))
    return "\n".join(lines)


def has_assert_call(text: str) -> bool:
    return any(ASSERT_CALL.search(line) for line in _stripped_lines(text))


def tu_status(text: str) -> str:
    """"armed" iff #undef NDEBUG precedes the boundary that decides NDEBUG for
    this TU's assert(): its own direct assert.h/cassert include when it has
    one, else its first #include of any kind (which could be the header that
    pulls assert.h in transitively)."""
    lines = _stripped_lines(text)
    undef_at = next((i for i, line in enumerate(lines) if UNDEF_NDEBUG.search(line)), None)
    if undef_at is None:
        return "unarmed"
    assert_include_at = next(
        (i for i, line in enumerate(lines) if ASSERT_INCLUDE.search(line)), None)
    boundary = assert_include_at if assert_include_at is not None else next(
        (i for i, line in enumerate(lines) if ANY_INCLUDE.search(line)), None)
    if boundary is None or undef_at < boundary:
        return "armed"
    return "unarmed"


def target_sources(cmake_text: str) -> dict[str, str]:
    """target name -> its add_executable() source-list text, as written."""
    stripped = _strip_cmake_comments(cmake_text)
    return {match.group(1): match.group(2) for match in ADD_EXECUTABLE.finditer(stripped)}


def owning_targets(basename: str, sources_by_target: dict[str, str]) -> list[str]:
    # A following identifier character would mean this matched a PREFIX of a
    # longer filename (tests/foo.c inside tests/foo.cpp), not the file itself.
    pattern = re.compile(r"(?<![A-Za-z0-9_])tests/" + re.escape(basename) + r"(?![A-Za-z0-9_])")
    return sorted(name for name, body in sources_by_target.items() if pattern.search(body))


def target_armed(name: str, cmake_text: str) -> bool:
    stripped = _strip_cmake_comments(cmake_text)
    for match in TARGET_COMPILE_CALL.finditer(stripped):
        target, body = match.group(1), match.group(2)
        if target == name and UNDEF_NDEBUG_FLAG.search(body):
            return True
    return False


def scan(tests_dir: Path, cmake_text: str) -> list[str]:
    """Names (relative to tests_dir) of every unarmed assert()-calling TU."""
    findings: list[str] = []
    sources_by_target = target_sources(cmake_text)
    paths = sorted(tests_dir.glob("*.c")) + sorted(tests_dir.glob("*.cpp"))
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="strict")
        if not has_assert_call(text):
            continue
        if tu_status(text) == "armed":
            continue
        owners = owning_targets(path.name, sources_by_target)
        # Every owning target must be armed: a TU shared by two targets is
        # disarmed in whichever one lacks -UNDEBUG, regardless of the other.
        if owners and all(target_armed(owner, cmake_text) for owner in owners):
            continue
        findings.append(path.name)
    return findings


def _scan_synthetic(files: dict[str, str], cmake_text: str) -> list[str]:
    with tempfile.TemporaryDirectory(prefix="mdkr64-assert-guard-selftest-") as tmp:
        tmp_path = Path(tmp)
        for name, content in files.items():
            (tmp_path / name).write_text(content, encoding="utf-8")
        return scan(tmp_path, cmake_text)


def self_test() -> list[str]:
    """Drive the guard's own predicates in both directions (non-vacuity proof)."""
    failures: list[str] = []

    unarmed_tu = "void f(void) { assert(1); }\n"
    if tu_status(unarmed_tu) == "armed":
        failures.append(
            "self-test: tu_status classified an undef-free TU with no "
            "assert.h include as armed -- the guard is vacuous")

    armed_tu = "#undef NDEBUG\n#include <assert.h>\nvoid f(void) { assert(1); }\n"
    if tu_status(armed_tu) != "armed":
        failures.append(
            "self-test: tu_status rejected #undef NDEBUG placed before "
            "<assert.h> -- the established per-file convention would fail "
            "its own gate")

    late_undef = "#include <assert.h>\n#undef NDEBUG\nvoid f(void) { assert(1); }\n"
    if tu_status(late_undef) == "armed":
        failures.append(
            "self-test: tu_status accepted #undef NDEBUG placed AFTER "
            "<assert.h> -- NDEBUG has already been read by then, so this "
            "ordering does not actually arm assert()")

    transitive_late = (
        '#include "other.h"\n#undef NDEBUG\nvoid f(void) { assert(1); }\n')
    if tu_status(transitive_late) == "armed":
        failures.append(
            "self-test: tu_status accepted #undef NDEBUG placed after a "
            "non-assert #include with no direct assert.h/cassert include "
            "of its own -- that #include could be the one that pulls "
            "assert.h in transitively, before NDEBUG was undefined")

    transitive_early = (
        '#undef NDEBUG\n#include "other.h"\nvoid f(void) { assert(1); }\n')
    if tu_status(transitive_early) != "armed":
        failures.append(
            "self-test: tu_status rejected #undef NDEBUG placed before "
            "every #include when the TU has no direct assert.h/cassert "
            "include of its own")

    commented = ("// assert(1) mentioned in prose, not code\n"
                 "void f(void) { /* assert(2); */ }\n")
    if has_assert_call(commented):
        failures.append(
            "self-test: has_assert_call matched assert( inside a comment "
            "-- the guard would flag prose, not code")

    synth_target = "mdkr_synth_guard_test"
    synth_source = "test_synth_guard.c"
    armed_cmake = (
        f"add_executable({synth_target}\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/{synth_source})\n"
        f"target_compile_options({synth_target} PRIVATE -UNDEBUG)\n")
    unarmed_cmake = (
        f"add_executable({synth_target}\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/{synth_source})\n")

    sources_by_target = target_sources(armed_cmake)
    owners = owning_targets(synth_source, sources_by_target)
    if owners != [synth_target]:
        failures.append(
            f"self-test: owning_targets did not resolve the synthetic "
            f"add_executable source list (got {owners})")
    elif not target_armed(owners[0], armed_cmake):
        failures.append(
            "self-test: target_armed did not recognize -UNDEBUG on the "
            "synthetic target's target_compile_options()")

    if target_armed(synth_target, unarmed_cmake):
        failures.append(
            "self-test: target_armed false-positived a target with no "
            "-UNDEBUG flag present -- the CMake-level arming path is vacuous")

    ndebug_value = (
        f"add_executable({synth_target}\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/{synth_source})\n"
        f"target_compile_definitions({synth_target} PRIVATE NDEBUG=0)\n")
    if target_armed(synth_target, ndebug_value):
        failures.append(
            "self-test: target_armed accepted NDEBUG=0 as arming -- "
            "<assert.h> only tests whether NDEBUG is #defined, so this "
            "would silently ship disarmed asserts")

    # basename anchor: tests/test_synth_guard.c must not match a source list
    # that only contains tests/test_synth_guard.cpp.
    boundary_cmake = (
        f"add_executable({synth_target}\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/{synth_source}pp)\n")
    boundary_sources = target_sources(boundary_cmake)
    boundary_owners = owning_targets(synth_source, boundary_sources)
    if boundary_owners:
        failures.append(
            f"self-test: owning_targets matched tests/{synth_source} "
            f"against a source list containing only tests/{synth_source}pp "
            f"(got {boundary_owners})")

    # CMake comment truncation: a ')' inside a source-list comment must not
    # truncate the add_executable match before a real source that follows it
    # (cmake/tests.cmake:311-323 and :703-715 both do this in the real tree).
    commented_source = "test_synth_commented.c"
    commented_cmake = (
        f"add_executable({synth_target}\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/test_synth_decoy.c\n"
        f"    # a parenthetical (like this one) must not end the match\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/{commented_source})\n"
        f"target_compile_options({synth_target} PRIVATE -UNDEBUG)\n")
    commented_sources = target_sources(commented_cmake)
    commented_owners = owning_targets(commented_source, commented_sources)
    if commented_owners != [synth_target]:
        failures.append(
            f"self-test: a ')' inside a source-list comment truncated the "
            f"add_executable match before the real source that followed it "
            f"(got owners={commented_owners})")
    elif not target_armed(commented_owners[0], commented_cmake):
        failures.append(
            "self-test: target_armed did not find target_compile_options "
            "after a preceding source-list comment containing ')'")

    commented_flag_cmake = (
        f"add_executable({synth_target}\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/{synth_source})\n"
        f"target_compile_options({synth_target} PRIVATE\n"
        f"    # a parenthetical (like this one) must not end the match\n"
        f"    -UNDEBUG)\n")
    if not target_armed(synth_target, commented_flag_cmake):
        failures.append(
            "self-test: a ')' inside a target_compile_options comment "
            "truncated the match before the real -UNDEBUG flag")

    # Negative: an unarmed TU under an unarmed target must be NAMED.
    found = _scan_synthetic({synth_source: unarmed_tu}, unarmed_cmake)
    if found != [synth_source]:
        failures.append(
            f"self-test: scan() did not name the synthetic unarmed offender "
            f"(got {found}) -- a real regression would ship silently")

    # Positive: the same TU, once its target is armed at the CMake level,
    # must scan clean.
    found = _scan_synthetic({synth_source: unarmed_tu}, armed_cmake)
    if found:
        failures.append(
            f"self-test: scan() still flagged a CMake-target-armed TU "
            f"(got {found})")

    # Positive: a TU armed on its own, registered under no CMake target at
    # all, must scan clean (covers the standalone-compiled TUs that carry
    # their own build recipe in a check_*.py, outside CMake entirely).
    found = _scan_synthetic({synth_source: armed_tu}, "")
    if found:
        failures.append(
            f"self-test: scan() flagged a TU-armed TU with no CMake target "
            f"(got {found})")

    # Negative: a TU compiled into two targets, only one of which is armed,
    # must still be NAMED -- assert() ships disarmed in the other target.
    multi_source = "test_synth_multi.c"
    multi_cmake = (
        f"add_executable(mdkr_synth_multi_armed\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/{multi_source})\n"
        f"target_compile_options(mdkr_synth_multi_armed PRIVATE -UNDEBUG)\n"
        f"add_executable(mdkr_synth_multi_unarmed\n"
        f"    ${{CMAKE_SOURCE_DIR}}/tests/{multi_source})\n")
    found = _scan_synthetic({multi_source: unarmed_tu}, multi_cmake)
    if found != [multi_source]:
        failures.append(
            f"self-test: scan() did not name a TU compiled into two "
            f"targets where only one carries -UNDEBUG (got {found}) -- the "
            f"other target ships disarmed asserts")

    # Negative control: assert-free source must never be named.
    found = _scan_synthetic({synth_source: "void f(void) { (void)0; }\n"}, "")
    if found:
        failures.append(
            f"self-test: scan() flagged a TU with no assert() call at all "
            f"(got {found})")

    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true",
                         help="also run the guard's positive/negative controls")
    args = parser.parse_args()

    failures: list[str] = []
    if args.self_test:
        failures.extend(self_test())

    cmake_parts: list[str] = []
    for path in CMAKE_FILES:
        if not path.is_file():
            failures.append(f"missing CMake file: {path.relative_to(ROOT)}")
            continue
        cmake_parts.append(path.read_text(encoding="utf-8"))
    cmake_text = "\n".join(cmake_parts)

    offenders = scan(TESTS_DIR, cmake_text)
    for name in offenders:
        failures.append(
            f"tests/{name} calls assert() but is not armed: neither does it "
            f"#undef NDEBUG before its assert.h/cassert include (or, absent "
            f"one, before its first #include of any kind), nor does every "
            f"CMake target it is compiled into carry a -UNDEBUG compile "
            f"option")

    if failures:
        print("check_test_assertions_armed: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("check_test_assertions_armed: PASS (0 unarmed test TUs)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
