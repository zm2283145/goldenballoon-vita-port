#!/usr/bin/env python3
"""AP-05 Adventure Party trace schema — cross-language contract (Python half).

Stdlib-only. Parses the SAME golden file the C unit test compares against
(tests/data/adventure_party_trace_golden.txt), turning each line into a dict,
checking the schema, and asserting a byte-exact field round-trip. Because both
languages read one authored golden file, a drift in the C formatter OR in this
parser fails one of the two tests -- which is the whole cross-language guard.

Run::

    python3 tests/adventure_party_trace.py                 # parse the golden
    python3 tests/adventure_party_trace.py --self-test     # + parser controls

Exit 0 on pass, nonzero with a precise message on failure.

The schema authority is the header comment in
platform/adventure_party/adventure_party_trace.h; the constants below mirror it
and the check keeps them honest.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests" / "data" / "adventure_party_trace_golden.txt"

TAG_PREFIX = "aparty_"
SCHEMA_VERSION = "1"

# Every fact class that must be present, and the exact ordered key list each
# line carries. Mirrors the header's schema authority; a mismatch here or in
# the golden file fails the round-trip or the coverage check.
EXPECTED_FIELDS = {
    "aparty_schema": ["v"],
    "aparty_session": ["state", "sgen", "lgen", "host"],
    "aparty_roster": ["n", "mask"],  # plus a variable run of c<seat> keys
    "aparty_binding": ["seat", "port"],
    "aparty_transition": ["seat", "tick", "trigger", "dest", "lgen"],
    "aparty_interaction": ["seat", "action", "verdict"],
    "aparty_award": ["op", "sgen", "lgen", "course", "activity", "kind",
                     "result"],
    "aparty_layout": ["viewports", "layout"],
    "aparty_restore": ["suspend_lgen", "restore_lgen", "match"],
}


class ParseError(Exception):
    pass


def parse_line(line: str) -> tuple[str, list[tuple[str, str]]]:
    """`tag: k=v k=v` -> (tag, ordered [(k, v)]). Raises ParseError on any
    malformed shape so the self-test controls can assert rejection."""
    if ": " not in line:
        raise ParseError(f"missing 'tag: ' separator: {line!r}")
    tag, _, rest = line.partition(": ")
    if not tag.startswith(TAG_PREFIX):
        raise ParseError(f"tag {tag!r} lacks {TAG_PREFIX!r} prefix")
    fields: list[tuple[str, str]] = []
    for token in rest.split(" "):
        if token == "" or "=" not in token:
            raise ParseError(f"malformed field token {token!r} in {line!r}")
        key, _, value = token.partition("=")
        if key == "" or value == "":
            raise ParseError(f"empty key or value in token {token!r}")
        fields.append((key, value))
    if not fields:
        raise ParseError(f"line has no fields: {line!r}")
    return tag, fields


def render(tag: str, fields: list[tuple[str, str]]) -> str:
    """Inverse of parse_line: rebuild the canonical line text."""
    return tag + ": " + " ".join(f"{k}={v}" for k, v in fields)


def check_golden(path: Path) -> list[str]:
    problems: list[str] = []
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        return [f"cannot read golden file {path}: {exc}"]

    seen_tags: list[str] = []
    schema_ok = False
    for lineno, raw in enumerate(text.splitlines(), start=1):
        if raw.strip() == "":
            continue
        # Round-trip: parse then re-render must reproduce the exact bytes, which
        # catches reordering, stray whitespace, or a dropped field.
        try:
            tag, fields = parse_line(raw)
        except ParseError as exc:
            problems.append(f"line {lineno}: {exc}")
            continue
        if render(tag, fields) != raw:
            problems.append(
                f"line {lineno}: round-trip mismatch\n"
                f"  original: {raw!r}\n"
                f"  rendered: {render(tag, fields)!r}")
        seen_tags.append(tag)

        keys = [k for k, _ in fields]
        if tag == "aparty_schema":
            schema_ok = True
            version = dict(fields).get("v")
            if version != SCHEMA_VERSION:
                problems.append(
                    f"line {lineno}: schema version {version!r} != expected "
                    f"{SCHEMA_VERSION!r} (bump both parsers + golden together)")
        expected = EXPECTED_FIELDS.get(tag)
        if expected is None:
            problems.append(f"line {lineno}: unknown fact class {tag!r}")
        elif tag == "aparty_roster":
            # n, mask, then a run of c<seat>=<char> keys.
            if keys[:2] != ["n", "mask"]:
                problems.append(
                    f"line {lineno}: roster must lead with n, mask; got {keys}")
            for k in keys[2:]:
                if not (k.startswith("c") and k[1:].isdigit()):
                    problems.append(
                        f"line {lineno}: unexpected roster key {k!r}")
        elif keys != expected:
            problems.append(
                f"line {lineno}: {tag} keys {keys} != expected {expected}")

    if not schema_ok:
        problems.append("no aparty_schema line found")
    missing = sorted(set(EXPECTED_FIELDS) - set(seen_tags))
    if missing:
        problems.append(f"golden is missing fact classes: {missing}")
    return problems


def run_self_tests() -> list[str]:
    """Each control asserts the parser accepts a valid shape and rejects an
    invalid one, so the golden check above cannot pass vacuously."""
    problems: list[str] = []

    def rejects(name: str, line: str) -> None:
        try:
            parse_line(line)
        except ParseError:
            return
        problems.append(f"self-test[{name}]: bad line was NOT rejected: {line!r}")

    def accepts(name: str, line: str) -> None:
        try:
            tag, fields = parse_line(line)
        except ParseError as exc:
            problems.append(f"self-test[{name}]: good line rejected: {exc}")
            return
        if render(tag, fields) != line:
            problems.append(f"self-test[{name}]: round-trip changed {line!r}")

    accepts("good-schema", "aparty_schema: v=1")
    accepts("good-roster", "aparty_roster: n=2 mask=0x3 c0=10 c1=11")
    rejects("no-prefix", "session: state=OFF sgen=0")
    rejects("no-separator", "aparty_schema v=1")
    rejects("empty-value", "aparty_layout: viewports= layout=1")
    rejects("bare-token", "aparty_layout: viewports 1")

    # A wrong schema version in an otherwise valid line must be caught by the
    # golden checker, not silently accepted.
    tmp = ROOT / "tests" / "data" / "__aparty_trace_selftest__.txt"
    try:
        tmp.write_text("aparty_schema: v=2\n", encoding="utf-8")
        if not check_golden(tmp):
            problems.append(
                "self-test[bad-version]: check_golden accepted schema v=2")
    finally:
        tmp.unlink(missing_ok=True)

    return problems


def main(argv: list[str]) -> int:
    self_test = "--self-test" in argv[1:]
    failures = check_golden(GOLDEN)
    if self_test:
        failures += [f"[self-test] {p}" for p in run_self_tests()]

    if failures:
        print("FAIL: adventure_party_trace", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1

    print(
        "adventure_party_trace: PASS — "
        f"{len(EXPECTED_FIELDS)} fact classes parsed and round-tripped from "
        f"{GOLDEN.name}, schema v{SCHEMA_VERSION}"
        + (" (self-test controls all fired)" if self_test else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
