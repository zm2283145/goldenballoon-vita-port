#!/usr/bin/env python3
"""Compare two per-tick state-hash artifacts and report the first divergence.

The engine can mirror its per-tick authoritative-state hash (`[SIMHASH]` rows,
platform/sim_hash.c) to a file via `MDKR_STATE_HASH_FILE=<path>`. Certifying
that two builds simulate identically -- across OS (macOS vs Windows) or ROM
region (US vs EU) -- reduces to: run each, then compare the two artifacts.

This tool is that comparison. It exits 0 iff BOTH files are non-empty, the same
length, and byte-equal modulo line endings on every tick line (the artifact is
read line-oriented, so a macOS LF and a Windows CRLF carrying the same tick are
NOT a divergence). Otherwise it exits non-zero and names the first divergent
tick (its number and both hash values) or the truncation point.

It FAILS CLOSED. A comparison that cannot be trusted is a failure, never a
pass, so each of these is a distinct non-zero exit with its own message:

    exit 1  wrong usage (bad command-line arguments)
    exit 2  a file is missing / unreadable
    exit 3  a file is empty (no tick lines)
    exit 4  a file has a non-ASCII byte, or a line that is not a well-formed
            `[SIMHASH]` row
    exit 5  the files have different lengths (one truncates the other)
    exit 6  the files diverge on a tick line

Each artifact line is the exact stdout form:

    [SIMHASH] tick=<n> objs=<n> h=<16 hex digits>

Usage:
    compare_sim_hash_artifacts.py <artifact_a> <artifact_b>
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import NoReturn

# Distinct exit codes so a caller (or a test) can tell the failure modes apart
# without scraping the message text.
EXIT_OK = 0
EXIT_USAGE = 1
EXIT_MISSING = 2
EXIT_EMPTY = 3
EXIT_UNPARSEABLE = 4
EXIT_LENGTH = 5
EXIT_DIVERGENCE = 6

# The one line format both sinks emit. Anchored so a partially written or
# corrupted row cannot slip through as if it parsed.
LINE_RE = re.compile(r"^\[SIMHASH\] tick=(\d+) objs=(-?\d+) h=([0-9a-f]{16})$")


class ArtifactError(Exception):
    """A file could not be read as a trustworthy artifact."""

    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code


def load(path_text: str) -> list[str]:
    """Read one artifact into its list of tick lines, or fail closed.

    Lines keep their exact text so the top-level comparison is byte-for-byte;
    parsing is only used to validate each row and to report a divergence's tick.
    A trailing newline on the final row is not a divergence, so line endings are
    stripped uniformly here.
    """
    path = Path(path_text)
    try:
        # The artifact format is pure ASCII (see LINE_RE). Decode strictly so an
        # undecodable byte is a fail-closed parse error rather than a raw
        # traceback -- and, critically, so the verdict is NOT locale-dependent:
        # a default decode would silently accept cp1252 bytes on Windows while
        # tracebacking on UTF-8 macOS, so two hosts could disagree.
        raw = path.read_text(encoding="ascii")
    except FileNotFoundError as error:
        raise ArtifactError(EXIT_MISSING, f"missing file: {path}") from error
    except (UnicodeDecodeError, ValueError) as error:
        raise ArtifactError(
            EXIT_UNPARSEABLE,
            f"unparseable file {path}: non-ASCII/undecodable byte: "
            f"{error}") from error
    except OSError as error:
        raise ArtifactError(
            EXIT_MISSING, f"unreadable file: {path}: {error}") from error

    lines = raw.splitlines()
    if not lines:
        raise ArtifactError(EXIT_EMPTY, f"empty file: {path}")
    for number, line in enumerate(lines):
        if not LINE_RE.match(line):
            raise ArtifactError(
                EXIT_UNPARSEABLE,
                f"unparseable line {number} in {path}: {line!r}")
    return lines


def tick_of(line: str) -> str:
    """The tick number a validated line carries, for reporting."""
    match = LINE_RE.match(line)
    return match.group(1) if match else "?"


def compare(a_path: str, b_path: str) -> tuple[int, str]:
    """Return an (exit_code, message) for comparing the two artifacts."""
    try:
        a_lines = load(a_path)
        b_lines = load(b_path)
    except ArtifactError as error:
        return error.code, str(error)

    if len(a_lines) != len(b_lines):
        shorter, longer = (
            (a_path, b_path) if len(a_lines) < len(b_lines)
            else (b_path, a_path))
        cut = min(len(a_lines), len(b_lines))
        continues = (a_lines if len(a_lines) > len(b_lines) else b_lines)[cut]
        return EXIT_LENGTH, (
            f"length mismatch: {a_path} has {len(a_lines)} ticks, {b_path} "
            f"has {len(b_lines)} ticks; {shorter} truncates at line {cut} "
            f"where {longer} continues with tick={tick_of(continues)}")

    for index, (a_line, b_line) in enumerate(zip(a_lines, b_lines)):
        if a_line != b_line:
            return EXIT_DIVERGENCE, (
                f"divergence at line {index} (tick={tick_of(a_line)}):\n"
                f"    {a_path}: {a_line}\n"
                f"    {b_path}: {b_line}")

    return EXIT_OK, (
        f"OK: {len(a_lines)} ticks identical and byte-equal between "
        f"{a_path} and {b_path}")


class _UsageParser(argparse.ArgumentParser):
    """ArgumentParser that exits usage errors with EXIT_USAGE.

    argparse's default is exit status 2, which would collide with EXIT_MISSING
    and make the code ambiguous. Routing usage errors to EXIT_USAGE keeps every
    non-zero code mapped to exactly one failure mode, as the docstring table
    promises.
    """

    def error(self, message: str) -> NoReturn:
        self.print_usage(sys.stderr)
        self.exit(EXIT_USAGE, f"{self.prog}: error: {message}\n")


def main(argv: list[str] | None = None) -> int:
    parser = _UsageParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("artifact_a", help="first per-tick hash artifact")
    parser.add_argument("artifact_b", help="second per-tick hash artifact")
    args = parser.parse_args(argv)

    code, message = compare(args.artifact_a, args.artifact_b)
    stream = sys.stdout if code == EXIT_OK else sys.stderr
    print(message, file=stream)
    return code


if __name__ == "__main__":
    sys.exit(main())
