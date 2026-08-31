#!/usr/bin/env python3
"""Bake gSineTable / gArcTanTable straight from the vendored hand-written .s.

platform/math_util_native.c supplies the math_util data symbols because this
build does not assemble game/src/hasm/ido/math_util.s. It historically
regenerated both trig curves through the HOST's libm at load time. That
generation is exact on every host measured so far, but "the host's libm agrees"
is a property of the machine, not of the source tree -- two builds on two
machines could in principle boot with different table bytes, and every consumer
(AI steering, camera, rotation matrices, lockstep peers) would silently follow.

This script makes the default table contents a constant of the tree: it parses
the `.half` directives out of the EXPORT(gSineTable) / EXPORT(gArcTanTable)
blocks in the vendored assembly -- the same ground truth
tests/check_math_tables.py asserts against -- and emits a committed C header of
`static const` arrays. No ROM bytes are involved: the values come from the .s
already sitting in the tree.

Usage:

    python3 tools/gen_math_tables.py            # rewrites the default output
    python3 tools/gen_math_tables.py out.h      # or an explicit path

Idempotent: running it twice produces identical bytes. The output
(platform/math_tables_baked.h) is committed; tests/check_math_tables.py asserts
entry-for-entry equality between the committed header and the .s, so a stale or
hand-edited copy fails the suite. The superseded load-time libm generation
stays reachable for A/B via MDKR_DEV_RUNTIME_TRIG=1 (see
platform/math_util_native.c).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, "game", "src", "hasm", "ido", "math_util.s")
DEFAULT_OUT = os.path.join(ROOT, "platform", "math_tables_baked.h")

# Both tables have exactly 1025 live entries: sins_s16 indexes [0,1023] and
# reads index+1; atan2_lookup's worst-case index is (s32)(1.0f*1024+0.5f)==1024.
EXPECTED_COUNT = 1025

BANNER = """\
/*
 * math_tables_baked.h -- gSineTable / gArcTanTable contents, baked from the
 * vendored hand-written assembly. GENERATED FILE, DO NOT EDIT.
 *
 * Ground truth is the .data section of game/src/hasm/ido/math_util.s (the
 * EXPORT(gSineTable) / EXPORT(gArcTanTable) .half directives), which this
 * build does not assemble. Baking the values makes the default table bytes a
 * constant of the source tree instead of a load-time computation through the
 * host's libm, so no two machines can disagree about DKR's trig tables. The
 * superseded load-time libm generation stays reachable for A/B via
 * MDKR_DEV_RUNTIME_TRIG=1 (see platform/math_util_native.c).
 *
 * Regenerate with:
 *
 *     python3 tools/gen_math_tables.py
 *
 * which re-parses the .s and rewrites this file in place.
 * tests/check_math_tables.py asserts entry-for-entry equality between these
 * arrays and the same .half directives, so a stale or hand-edited copy fails
 * the suite.
 *
 * No ROM bytes are involved or committed: the values come from the vendored
 * .s already in the tree, exactly like the checks that assert against it.
 */
#ifndef MDKR_MATH_TABLES_BAKED_H
#define MDKR_MATH_TABLES_BAKED_H

#include <stdint.h>
"""


def parse_asm_half_table(text, name):
    """Pull an EXPORT(<name>) .half table out of the hand-written .s.

    Same tolerant shape as the parser in tests/check_math_tables.py: values
    accumulate from consecutive .half lines and stop at the first line that is
    neither a .half directive, blank, nor a comment.

    The symbol is matched as an exact EXPORT(<name>) token, so a longer symbol
    (e.g. EXPORT(gSineTable2)) can never bind here; a missing symbol fails
    closed via LookupError instead of a bare .index() traceback.
    """
    m = re.search(r"EXPORT\(" + re.escape(name) + r"\)", text)
    if m is None:
        raise LookupError("EXPORT(%s) not found in %s" % (name, ASM))
    out = []
    for line in text[m.start():].split("\n")[1:]:
        s = line.strip()
        if s.startswith(".half"):
            out += [int(v.strip(), 16) for v in s[5:].split(",") if v.strip()]
        elif s == "" or s.startswith("/*") or s.startswith("*"):
            continue
        else:
            break
    return out


def emit_array(lines, c_name, macro_name, comment_lines, values):
    lines.append("")
    lines.append("/* %s" % comment_lines[0])
    for extra in comment_lines[1:]:
        lines.append(" * %s" % extra)
    lines.append(" */")
    lines.append("#define %s %d" % (macro_name, len(values)))
    lines.append("static const uint16_t %s[%s] = {" % (c_name, macro_name))
    for start in range(0, len(values), 8):
        chunk = values[start:start + 8]
        lines.append("    " + " ".join("0x%04X," % v for v in chunk))
    lines.append("};")


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT
    if len(sys.argv) > 2:
        print("usage: gen_math_tables.py [output.h]", file=sys.stderr)
        return 2

    text = open(ASM).read()
    try:
        sine = parse_asm_half_table(text, "gSineTable")
        arctan = parse_asm_half_table(text, "gArcTanTable")
    except LookupError as exc:
        print("FAIL: %s -- refusing to emit a suspect bake" % exc,
              file=sys.stderr)
        return 1

    # Fail loudly if the .s parse drifts: a wrong bake here would be committed
    # and trusted, so the shape invariants are hard errors, not warnings.
    problems = []
    if len(sine) != EXPECTED_COUNT:
        problems.append("gSineTable parsed as %d entries, want %d"
                        % (len(sine), EXPECTED_COUNT))
    if len(arctan) != EXPECTED_COUNT:
        problems.append("gArcTanTable parsed as %d entries, want %d"
                        % (len(arctan), EXPECTED_COUNT))
    if sine and (sine[0] != 0x0000 or sine[-1] != 0x8000):
        problems.append("gSineTable endpoints 0x%04X..0x%04X, want "
                        "0x0000..0x8000" % (sine[0], sine[-1]))
    if arctan and (arctan[0] != 0x0000 or arctan[-1] != 0x2000):
        problems.append("gArcTanTable endpoints 0x%04X..0x%04X, want "
                        "0x0000..0x2000 (45 deg)" % (arctan[0], arctan[-1]))
    for name, vals in (("gSineTable", sine), ("gArcTanTable", arctan)):
        if any(b < a for a, b in zip(vals, vals[1:])):
            problems.append("%s is not monotonically non-decreasing" % name)
    if problems:
        for p in problems:
            print("FAIL: %s -- refusing to emit a suspect bake" % p,
                  file=sys.stderr)
        return 1

    lines = [BANNER.rstrip("\n")]
    emit_array(lines, "kMdkrBakedSineTable", "MDKR_BAKED_SINE_TABLE_COUNT",
               ["EXPORT(gSineTable): quarter turn, peak 0x8000. Reads are lhu,",
                "so entry 1024 wrapping to -32768 as s16 is the ROM's own",
                "behaviour."], sine)
    emit_array(lines, "kMdkrBakedArcTanTable", "MDKR_BAKED_ARCTAN_TABLE_COUNT",
               ["EXPORT(gArcTanTable): atan(i/1024) scaled so 90 deg == 0x4000.",
                "This is the ROUNDED curve; the MDKR_ARCTAN=trunc A/B arm",
                "regenerates its truncated table at load time instead."],
               arctan)
    lines.append("")
    lines.append("#endif /* MDKR_MATH_TABLES_BAKED_H */")
    lines.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    print("wrote %s (gSineTable %d entries, gArcTanTable %d entries)"
          % (out_path, len(sine), len(arctan)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
