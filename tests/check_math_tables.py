#!/usr/bin/env python3
"""The math_util data tables this port GENERATES must match the ones it replaces.

What this covers
----------------
`game/src/hasm/ido/math_util.s` is hand-written assembly, and its `.data` section
holds `gArcTanTable`, `gSineTable`, `gCurrentRNGSeed`, `gPrevRNGSeed` and
`gIntDisFlag`.  This build does not assemble that file, so
`platform/math_util_native.c` supplies those symbols instead -- the tables by
generating the curves at load time, the seeds as C initialisers.  Nothing had ever
checked the substitutes against the originals, and the originals are sitting in
the tree.

Three divergences were found that way.  ALL THREE ARE NOW FIXED AND DEFAULT ON
("closedloop" wave); this check asserts the ROM-faithful default is *exactly*
right and that the superseded behaviour is still reachable for A/B, because each
one shifts every AI racing line and the next person to chase a route change needs
to be able to bisect it.  See docs/OPEN_ITEMS.md.

1. **gArcTanTable was truncated where the ROM rounds.**  491 of the 1025 live
   entries were one unit low.  Rounding reproduces the ROM's table *exactly* -- 0
   of 1025 entries differ -- asserted here by FNV-1a over the table the binary
   actually built versus the same hash computed from the `.half` directives in the
   `.s`.  `MDKR_ARCTAN=trunc` restores the truncation.

2. **gCurrentRNGSeed / gPrevRNGSeed were 0x00051234 / 0, where the ROM boots
   0x5141564D / 0x5141564D** ('QAVM').  Those are the live starting seeds:
   `set_rng_seed()` has exactly one caller in the whole game
   (`game/src/waves.c:364`, bracketed by save/load), so nothing re-seeds at boot
   and all 98 `rand_range()` call sites descend from them.  The port was on a
   different random sequence from frame 0 for its whole history -- silently,
   because the run is still perfectly deterministic, just deterministically wrong,
   so `check_determinism.py` could not see it.  `MDKR_RNGSEED=legacy` restores the
   invented seeds.

3. **sins_s16 / coss_s16 / sins_2 / sins_f / coss_f called libm** where the ROM
   walks `gSineTable` and lerps between two entries.  The port agreed with the ROM
   on only 21368 of the 65536 angles (32.6 %).  The table is reproduced with no ROM
   data at all -- `round(sin(i*pi/2/1024) * 0x8000)` matches all 1025 entries -- and
   the walk is transcribed from `XLEAF(sins_s16)` (math_util.s:2432).  This check
   re-implements that walk in Python straight from the assembly and requires the
   binary's `sinFnv` (FNV-1a over all 65536 `sins_s16` results) to match it bit for
   bit, so the table alone is not what is being trusted.  `MDKR_TRIG=libm`
   restores the approximation.

The baked default, and the equality gate over it
------------------------------------------------
The DEFAULT build no longer generates either table through the host's libm at
load time: it copies the BAKED arrays in `platform/math_tables_baked.h`
(generated from the same `.s` by `tools/gen_math_tables.py` and committed), so
the table bytes are a constant of the source tree rather than a property of
whichever libm the host ships.  This check is the equality gate over that
committed header: every entry of both baked arrays must EQUAL the
corresponding `.half` directive in the `.s`, entry for entry, in addition to
the binary-side FNV assertions below.  `MDKR_DEV_RUNTIME_TRIG=1` restores the
superseded load-time libm generation (a third A/B arm here, and a live-online
refusal seam like the other three) -- on this host it must still reproduce the
`.s` exactly, which is precisely the premise the bake stops relying on.

Why the FIXTURES had to change first, and why that matters here
--------------------------------------------------------------
All three are reached and material -- measured 80 of 359 [PACE] rows changed for
the seed (from row 279) and 110 of 359 for the arctan table (from row 233) -- so
flipping them shifts every AI racing line.  Two fixtures were calibrated against
one particular line and had to become closed-loop first: `check_race_drive` now
drives with `MDKR_AUTOPILOT`, and `check_collision_gridmask` states its positive
control against the fixed arm's own ceiling and reaches the boss win by writing
`finishPosition` rather than by crippling the boss's velocity.  Flipping the
defaults without that would have traded a silent fidelity bug for a blind suite.

No ROM bytes are involved or committed: the ground truth is the vendored `.s`,
which this check parses at run time.

    MDKR_AUDIO=0 python3 tests/check_math_tables.py -v       # ~10 s

Always muted + headless, per tests/README.md.  Exit 0 = pass.
"""
import argparse
import math
import os
import re
import subprocess
import sys

from harness_utils import DEFAULT_BUILD_DIR, resolve_binary

ASM = os.path.join("game", "src", "hasm", "ido", "math_util.s")
BAKED = os.path.join("platform", "math_tables_baked.h")
ARCTAN_LIVE = 1025          # atan2_lookup's worst-case index is 1024
SINE_PEAK = 0x8000          # gSineTable peaks at 0x8000; sins_s16 doubles it

MATH_RE = re.compile(r"\[MATH\] rngSeed=0x([0-9a-f]+) prevSeed=0x([0-9a-f]+) "
                     r"arctan=(\w+) arctanN=(\d+) arctanFnv=0x([0-9a-f]+) "
                     r"trig=(\w+) sineN=(\d+) sineFnv=0x([0-9a-f]+) "
                     r"sinFnv=0x([0-9a-f]+) sineSrc=(\w+) arctanSrc=(\w+)")
BAD_RE = re.compile(r"\[FATAL\]|\[CRASH\]|AddressSanitizer|Assertion")
LEGACY_SEED = 0x00051234    # the invented boot seed, kept reachable for A/B
REQUIRED_STRONG = {
    "obj_shade_fast",
    "obj_animate",
    "calc_dynamic_lighting_for_object_2",
    "gzip_inflate_block",
    "func_80049794",
    "sins_s16",
    "coss_s16",
    "sins_2",
    "sins_f",
    "coss_f",
    "mtxf_transform_dir",
    "gSineTable",
    "gArcTanTable",
    "gCurrentRNGSeed",
    "gPrevRNGSeed",
    "gIntDisFlag",
}


def check_required_strong_symbols(binary, failures):
    """Required native providers must not resolve weakly or have weak source fallbacks."""
    try:
        proc = subprocess.run(
            ["nm", "-g", binary], text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False
        )
    except OSError as exc:
        failures.append("could not execute nm for the required-provider audit: %s" % exc)
        return
    if proc.returncode != 0:
        failures.append("nm failed during the required-provider audit: %s" % proc.stdout[-500:])
        return

    symbols = {}
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        code, raw_name = parts[-2], parts[-1]
        name = raw_name[1:] if raw_name.startswith("_") else raw_name
        if name in REQUIRED_STRONG:
            symbols[name] = code
    for name in sorted(REQUIRED_STRONG):
        code = symbols.get(name)
        if code is None:
            failures.append("required native provider %s is absent from the linked binary" % name)
        elif code in "UuWwVv":
            failures.append("required native provider %s resolved with non-strong nm type %s"
                            % (name, code))

    weak_definition = re.compile(
        r"(?:\bWEAK\b|__attribute__\s*\(\([^)]*\bweak\b[^)]*\)\)).*\b("
        + "|".join(re.escape(name) for name in sorted(REQUIRED_STRONG))
        + r")\b"
    )
    for root in ("game", "platform"):
        for dirpath, _dirnames, filenames in os.walk(root):
            for filename in filenames:
                if not filename.endswith((".c", ".h", ".cc", ".cpp", ".m", ".mm")):
                    continue
                path = os.path.join(dirpath, filename)
                with open(path, errors="replace") as source:
                    for line_number, line in enumerate(source, 1):
                        match = weak_definition.search(line)
                        if match:
                            failures.append(
                                "required provider %s still has a weak fallback at %s:%d"
                                % (match.group(1), path, line_number)
                            )


def parse_asm_half_table(text, name):
    """Pull an EXPORT(<name>) .half table out of the hand-written .s.

    The symbol is matched as an exact EXPORT(<name>) token, so a longer symbol
    (e.g. EXPORT(gSineTable2)) can never bind here; a missing symbol raises
    LookupError (caught in main) instead of a bare .index() traceback.
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


def parse_asm_word(text, name):
    i = text.index("EXPORT(%s)" % name)
    m = re.search(r"\.word\s+(0x[0-9A-Fa-f]+)", text[i:i + 400])
    return int(m.group(1), 16)


def parse_baked_array(text, name):
    """Pull a `static const uint16_t <name>[...] = { ... };` array out of the
    committed baked-table header, or None if the array is not there."""
    m = re.search(re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", text)
    if m is None:
        return None
    return [int(v.strip(), 16) for v in m.group(1).split(",") if v.strip()]


def baked_equality_failures(baked, rom, header_name, asm_name):
    """The committed baked array must EQUAL the .s table entry for entry."""
    out = []
    if baked is None:
        out.append("%s has no parseable %s array -- the baked header the "
                   "default build copies is gone or malformed"
                   % (BAKED, header_name))
        return out
    if len(baked) != len(rom):
        out.append("%s's %s has %d entries, EXPORT(%s) in %s has %d -- "
                   "regenerate with tools/gen_math_tables.py"
                   % (BAKED, header_name, len(baked), asm_name, ASM, len(rom)))
        return out
    bad = [i for i in range(len(rom)) if baked[i] != rom[i]]
    if bad:
        i = bad[0]
        out.append("%s's %s differs from EXPORT(%s) in %s on %d of %d entries "
                   "(first at [%d]: 0x%04X != 0x%04X) -- the committed bake no "
                   "longer equals the assembly it was baked from; regenerate "
                   "with tools/gen_math_tables.py"
                   % (BAKED, header_name, asm_name, ASM, len(bad), len(rom),
                      i, baked[i], rom[i]))
    return out


def fnv1a32_u16(vals):
    """Must match the loop in platform/math_util_native.c exactly."""
    h = 2166136261
    for v in vals:
        v &= 0xFFFF
        h = ((h ^ (v & 0xFF)) * 16777619) & 0xFFFFFFFF
        h = ((h ^ ((v >> 8) & 0xFF)) * 16777619) & 0xFFFFFFFF
    return h


def fnv1a32_u32(vals):
    """The 4-byte-per-value variant the binary uses for sinFnv."""
    h = 2166136261
    for v in vals:
        v &= 0xFFFFFFFF
        for k in range(4):
            h = ((h ^ ((v >> (k * 8)) & 0xFF)) * 16777619) & 0xFFFFFFFF
    return h


def sins_s16_from_asm(table, angle):
    """`XLEAF(sins_s16)`, math_util.s:2432, transcribed independently of the C.

    This is the point of the sinFnv assertion: the Python here and the C in
    platform/math_util_native.c are two separate readings of the same assembly, so
    agreeing over all 65536 angles is real cross-checking and not a tautology.

        sll v0, a0, 17 ; bgez -> xori a0, 0x7FFF     mirror inside the half turn
        srl t2, a0, 3  ; andi t2, 0x7FE              entry (a0 >> 4) & 0x3FF
        lhu hi, 2(p)   ; lhu lo, 0(p)                unsigned table reads
        andi t1, a0, 0xF                             frac
        mul (hi - lo), frac ; srl 3                  the lerp, /16 then x2
        sll lo, 1 ; addu                             amplitude 0x10000
        sll a0, 16 ; bgez -> negu v0                 sign from bit 15
    """
    a = angle & 0xFFFFFFFF
    if (a >> 14) & 1:
        a ^= 0x7FFF
    idx = (a >> 4) & 0x3FF
    frac = a & 0xF
    lo = table[idx] & 0xFFFF
    hi = table[idx + 1] & 0xFFFF
    v = (lo << 1) + ((((hi - lo) * frac) & 0xFFFFFFFF) >> 3)
    if (a >> 15) & 1:
        v = (-v) & 0xFFFFFFFF
    return v & 0xFFFFFFFF


def run(binary, rom, env_extra, verbose):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"      # belt-and-braces; --headless-frames is the guarantee
    env["MDKR_TRACE"] = "1"      # arms the [MATH] probe
    for k in ("MDKR_ARCTAN", "MDKR_RNGSEED", "MDKR_TRIG", "MDKR_DEV_RUNTIME_TRIG"):
        env.pop(k, None)
    env.update(env_extra)
    cmd = [binary, "--headless-frames", "3", "--rom", rom]
    if verbose:
        print("  $ %s %s" % (" ".join("%s=%s" % kv for kv in sorted(env_extra.items()))
                             or "(no toggles)", " ".join(cmd)))
    p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=300)
    out = p.stdout.decode("utf-8", "replace")
    m = MATH_RE.search(out)
    return {
        "rc": p.returncode,
        "seed": int(m.group(1), 16) if m else None,
        "prev": int(m.group(2), 16) if m else None,
        "mode": m.group(3) if m else None,
        "n": int(m.group(4)) if m else None,
        "fnv": int(m.group(5), 16) if m else None,
        "trig": m.group(6) if m else None,
        "sineN": int(m.group(7)) if m else None,
        "sineFnv": int(m.group(8), 16) if m else None,
        "sinFnv": int(m.group(9), 16) if m else None,
        "sineSrc": m.group(10) if m else None,
        "arctanSrc": m.group(11) if m else None,
        "bad": BAD_RE.findall(out),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--rom", default="baserom.us.v80.z64")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    binary = resolve_binary(args.build)
    for path in (binary, args.rom, ASM, BAKED):
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1

    failures: list[str] = []
    check_required_strong_symbols(binary, failures)

    # ---- ground truth, straight out of the vendored assembly ---------------
    text = open(ASM).read()
    try:
        rom_arctan = parse_asm_half_table(text, "gArcTanTable")
        rom_sine = parse_asm_half_table(text, "gSineTable")
    except LookupError as exc:
        print("FAIL: %s" % exc, file=sys.stderr)
        return 1
    rom_seed = parse_asm_word(text, "gCurrentRNGSeed")
    rom_prev = parse_asm_word(text, "gPrevRNGSeed")

    if len(rom_arctan) < ARCTAN_LIVE:
        print("FAIL: %s's gArcTanTable parsed as %d entries, need >= %d -- the "
              "parser has drifted from the file" % (ASM, len(rom_arctan), ARCTAN_LIVE),
              file=sys.stderr)
        return 1
    rom_fnv = fnv1a32_u16(rom_arctan[:ARCTAN_LIVE])

    # ---- the EQUALITY gate over the committed baked arrays ------------------
    # The default build copies platform/math_tables_baked.h instead of
    # generating through libm, so the committed header IS the default table
    # bytes. Entry-for-entry equality against the .s (not just a hash) keeps a
    # perturbed or stale bake diagnosable down to the index.
    baked_text = open(BAKED).read()
    baked_sine = parse_baked_array(baked_text, "kMdkrBakedSineTable")
    baked_arctan = parse_baked_array(baked_text, "kMdkrBakedArcTanTable")
    failures += baked_equality_failures(baked_sine, rom_sine,
                                        "kMdkrBakedSineTable", "gSineTable")
    failures += baked_equality_failures(baked_arctan,
                                        rom_arctan[:ARCTAN_LIVE],
                                        "kMdkrBakedArcTanTable", "gArcTanTable")

    # The generators, so the divergence is stated in entry counts and not only as
    # a hash. Rounding must reproduce the ROM table exactly; truncation must not.
    K = 32768.0 / math.pi
    gen_round = [int(math.atan(i / 1024.0) * K + 0.5) for i in range(ARCTAN_LIVE)]
    gen_trunc = [int(math.atan(i / 1024.0) * K) for i in range(ARCTAN_LIVE)]
    n_round_bad = sum(1 for i in range(ARCTAN_LIVE) if gen_round[i] != rom_arctan[i])
    n_trunc_bad = sum(1 for i in range(ARCTAN_LIVE) if gen_trunc[i] != rom_arctan[i])
    if n_round_bad != 0:
        failures.append("rounding the arctan curve no longer reproduces the ROM's "
                        "gArcTanTable (%d of %d entries differ) -- the premise of "
                        "the deferred fix is gone" % (n_round_bad, ARCTAN_LIVE))
    if n_trunc_bad == 0:
        failures.append("truncating the arctan curve now reproduces the ROM's table "
                        "exactly, so there is no divergence left to defer and this "
                        "check is describing a bug that no longer exists")

    # gSineTable must be reproducible with no ROM data at all.
    sine_round = [int(round(math.sin(i * math.pi / 2 / 1024) * SINE_PEAK))
                  for i in range(len(rom_sine))]
    n_sine_bad = sum(1 for i in range(len(rom_sine)) if sine_round[i] != rom_sine[i])
    if n_sine_bad != 0:
        failures.append("the ROM's gSineTable is no longer reproduced by "
                        "round(sin(i*pi/2/1024)*0x8000) (%d of %d differ) -- the "
                        "route to an exact sins_s16 without ROM data is gone"
                        % (n_sine_bad, len(rom_sine)))
    rom_sine_fnv = fnv1a32_u16(rom_sine)
    # The whole-domain reference: the assembly's walk over the ROM's own table.
    rom_sin_fnv = fnv1a32_u32(sins_s16_from_asm(rom_sine, i) for i in range(65536))
    # ...and what libm would have produced, so the divergence is quantified rather
    # than merely asserted away. C truncates toward zero, hence int().
    n_libm_bad = 0
    for i in range(65536):
        angle = i - 65536 if i >= 32768 else i
        libm = int(math.sin(angle * math.pi / 32768.0) * 65536.0)
        exact = sins_s16_from_asm(rom_sine, i)
        if exact >= 0x80000000:
            exact -= 0x100000000
        if libm != exact:
            n_libm_bad += 1
    if n_libm_bad == 0:
        failures.append("libm now reproduces the ROM's table+lerp on all 65536 "
                        "angles, so MDKR_TRIG=libm is no longer a divergence and "
                        "this check is describing a bug that does not exist")

    # ---- the three arms ----------------------------------------------------
    romarm = run(binary, args.rom, {}, args.verbose)
    legacy = run(binary, args.rom, {"MDKR_ARCTAN": "trunc", "MDKR_RNGSEED": "legacy",
                                    "MDKR_TRIG": "libm"}, args.verbose)
    runtime = run(binary, args.rom, {"MDKR_DEV_RUNTIME_TRIG": "1"}, args.verbose)

    for name, r in (("default/rom", romarm), ("legacy", legacy),
                    ("dev-runtime", runtime)):
        if r["rc"] != 0:
            failures.append("%s arm: exit code %d" % (name, r["rc"]))
        if r["bad"]:
            failures.append("%s arm: %s in output" % (name, r["bad"][0]))
        # Fail closed: without the probe nothing below means anything.
        if r["fnv"] is None:
            failures.append("%s arm: no '[MATH] rngSeed=... arctanFnv=...' line -- the "
                            "probe is gone, so the generated tables are unchecked"
                            % name)
    if failures:
        for f in failures:
            print("  - %s" % f, file=sys.stderr)
        print("check_math_tables: FAIL")
        return 1

    if romarm["n"] != ARCTAN_LIVE:
        failures.append("the binary hashed %d arctan entries, this check hashes %d -- "
                        "they must cover the same range" % (romarm["n"], ARCTAN_LIVE))
    if romarm["sineN"] != len(rom_sine):
        failures.append("the binary hashed %d gSineTable entries, %s has %d -- they "
                        "must cover the same range"
                        % (romarm["sineN"], ASM, len(rom_sine)))

    # ---- the DEFAULT build must be EXACTLY right ---------------------------
    if romarm["sineSrc"] != "baked" or romarm["arctanSrc"] != "baked":
        failures.append("the default build reports sineSrc=%s arctanSrc=%s, want "
                        "baked/baked -- the default tables are supposed to be the "
                        "committed %s copy of the .s, not a load-time generation"
                        % (romarm["sineSrc"], romarm["arctanSrc"], BAKED))
    if romarm["mode"] != "round":
        failures.append("the default build reports arctan=%s, want round -- the "
                        "ROM-faithful arctan table is no longer the default"
                        % romarm["mode"])
    if romarm["fnv"] != rom_fnv:
        failures.append("the default build's gArcTanTable hashes to FNV 0x%08x, but "
                        "the ROM's table in %s hashes to 0x%08x -- it no longer "
                        "reproduces the assembly it replaces"
                        % (romarm["fnv"], ASM, rom_fnv))
    if romarm["seed"] != rom_seed or romarm["prev"] != rom_prev:
        failures.append("the default build boots seed=0x%08x prev=0x%08x, but %s has "
                        "0x%08x / 0x%08x" % (romarm["seed"], romarm["prev"], ASM,
                                             rom_seed, rom_prev))
    if romarm["trig"] != "table":
        failures.append("the default build reports trig=%s, want table -- sins_s16 is "
                        "back on libm instead of the ROM's gSineTable walk"
                        % romarm["trig"])
    if romarm["sineFnv"] != rom_sine_fnv:
        failures.append("the default build's gSineTable hashes to FNV 0x%08x, but the "
                        "ROM's table in %s hashes to 0x%08x"
                        % (romarm["sineFnv"], ASM, rom_sine_fnv))
    # The strongest of the lot: the whole 65536-angle domain, C against a Python
    # transcription of the same assembly over the ROM's own table entries.
    if romarm["sinFnv"] != rom_sin_fnv:
        failures.append("the default build's sins_s16 hashes to 0x%08x over all 65536 "
                        "angles, but XLEAF(sins_s16) applied to %s's gSineTable hashes "
                        "to 0x%08x -- the table walk (mirroring, lerp or sign) does not "
                        "match the assembly"
                        % (romarm["sinFnv"], ASM, rom_sin_fnv))

    # ---- the legacy arm must still be reachable, and still be the divergence -
    # Not an endorsement -- it is what lets the next person bisect a route change
    # against the three behaviours that moved every AI racing line.
    if legacy["mode"] != "trunc":
        failures.append("MDKR_ARCTAN=trunc reported arctan=%s -- the A/B toggle is not "
                        "taking effect, so the superseded behaviour is unreachable"
                        % legacy["mode"])
    if legacy["fnv"] == rom_fnv:
        failures.append("MDKR_ARCTAN=trunc built the ROM's table anyway -- truncation "
                        "and rounding now agree, so there is no divergence left to "
                        "A/B against")
    if legacy["seed"] != LEGACY_SEED or legacy["prev"] != 0:
        failures.append("MDKR_RNGSEED=legacy booted seed=0x%08x prev=0x%08x, want "
                        "0x%08x / 0x00000000 -- the historic seeds are what a bisect "
                        "needs" % (legacy["seed"], legacy["prev"], LEGACY_SEED))
    if legacy["trig"] != "libm":
        failures.append("MDKR_TRIG=libm reported trig=%s -- the A/B toggle is not "
                        "taking effect" % legacy["trig"])
    if legacy["sinFnv"] == rom_sin_fnv:
        failures.append("MDKR_TRIG=libm produced the same 65536-angle hash as the "
                        "table walk, so the libm approximation is no longer a "
                        "divergence and this check is describing a bug that does not "
                        "exist")
    # gSineTable itself is filled identically in both arms -- only its use
    # changes -- so a mismatch here means the fill, not the toggle.
    if legacy["sineFnv"] != rom_sine_fnv:
        failures.append("MDKR_TRIG=libm changed gSineTable's contents (0x%08x vs "
                        "0x%08x) -- the toggle must change how the table is USED, not "
                        "what is in it" % (legacy["sineFnv"], rom_sine_fnv))
    # The trunc curve is a deliberate divergence from the .s, so it has no baked
    # source: the trunc arm MUST report a load-time arctan (and an untouched
    # baked sine). arctanSrc=baked here would mean the toggle silently stopped
    # producing the superseded table at all.
    if legacy["arctanSrc"] != "runtime" or legacy["sineSrc"] != "baked":
        failures.append("the legacy arm reports sineSrc=%s arctanSrc=%s, want "
                        "baked/runtime -- MDKR_ARCTAN=trunc must regenerate the "
                        "truncated arctan curve at load time and leave the sine "
                        "table on the baked copy"
                        % (legacy["sineSrc"], legacy["arctanSrc"]))

    # ---- the dev-runtime arm: the superseded libm generation, still exact ----
    # MDKR_DEV_RUNTIME_TRIG=1 regenerates BOTH tables through the host's libm
    # (the pre-bake behaviour). On this host that generation must still equal
    # the .s bit for bit -- if this arm ever fails while the default stays
    # green, that IS the cross-host libm divergence the baked default exists to
    # make irrelevant, caught by the A/B toggle doing its job.
    if runtime["sineSrc"] != "runtime" or runtime["arctanSrc"] != "runtime":
        failures.append("MDKR_DEV_RUNTIME_TRIG=1 reports sineSrc=%s arctanSrc=%s, "
                        "want runtime/runtime -- the A/B toggle is not taking "
                        "effect, so the superseded libm generation is unreachable"
                        % (runtime["sineSrc"], runtime["arctanSrc"]))
    if runtime["mode"] != "round" or runtime["trig"] != "table":
        failures.append("the dev-runtime arm reports arctan=%s trig=%s, want "
                        "round/table -- MDKR_DEV_RUNTIME_TRIG must only move the "
                        "table SOURCE, not the curve rounding or the sine walk"
                        % (runtime["mode"], runtime["trig"]))
    if runtime["fnv"] != rom_fnv or runtime["sineFnv"] != rom_sine_fnv \
            or runtime["sinFnv"] != rom_sin_fnv:
        failures.append("the dev-runtime arm's tables no longer reproduce the .s "
                        "(arctanFnv 0x%08x want 0x%08x, sineFnv 0x%08x want "
                        "0x%08x, sinFnv 0x%08x want 0x%08x) -- this host's libm "
                        "has diverged from the vendored assembly, exactly the "
                        "hazard the baked default removes from shipping builds"
                        % (runtime["fnv"], rom_fnv, runtime["sineFnv"],
                           rom_sine_fnv, runtime["sinFnv"], rom_sin_fnv))

    if args.verbose:
        print("  ground truth %s: gArcTanTable %d entries fnv=0x%08x, "
              "gSineTable %d entries peak=0x%X fnv=0x%08x, seeds 0x%08x/0x%08x"
              % (ASM, len(rom_arctan), rom_fnv, len(rom_sine), max(rom_sine),
                 rom_sine_fnv, rom_seed, rom_prev))
        print("  arctan generators vs ROM: rounded %d/%d differ, truncated %d/%d differ"
              % (n_round_bad, ARCTAN_LIVE, n_trunc_bad, ARCTAN_LIVE))
        print("  sins_s16 over 65536 angles: asm-from-.s fnv=0x%08x; libm differs on "
              "%d angles (%.1f%%)"
              % (rom_sin_fnv, n_libm_bad, 100.0 * n_libm_bad / 65536.0))
        print("  baked header %s: sine %d entries, arctan %d entries, both equal "
              "the .s entry for entry"
              % (BAKED, len(baked_sine or []), len(baked_arctan or [])))
        print("  default arm: seed=0x%08x arctan=%s fnv=0x%08x trig=%s "
              "sinFnv=0x%08x srcs=%s/%s"
              % (romarm["seed"], romarm["mode"], romarm["fnv"], romarm["trig"],
                 romarm["sinFnv"], romarm["sineSrc"], romarm["arctanSrc"]))
        print("  legacy  arm: seed=0x%08x arctan=%s fnv=0x%08x trig=%s "
              "sinFnv=0x%08x srcs=%s/%s"
              % (legacy["seed"], legacy["mode"], legacy["fnv"], legacy["trig"],
                 legacy["sinFnv"], legacy["sineSrc"], legacy["arctanSrc"]))
        print("  runtime arm: seed=0x%08x arctan=%s fnv=0x%08x trig=%s "
              "sinFnv=0x%08x srcs=%s/%s"
              % (runtime["seed"], runtime["mode"], runtime["fnv"],
                 runtime["trig"], runtime["sinFnv"], runtime["sineSrc"],
                 runtime["arctanSrc"]))

    if failures:
        for msg in failures:
            print("  - %s" % msg, file=sys.stderr)
        print("check_math_tables: FAIL")
        return 1
    print("check_math_tables: PASS  (baked header equals the .s entry for entry; "
          "default arm is exact and baked: gArcTanTable 0/%d differ (fnv 0x%08x), "
          "gSineTable 0/%d differ, sins_s16 matches XLEAF(sins_s16) on all 65536 "
          "angles, seed 0x%08x. Dev-runtime arm still reproduces the .s through "
          "this host's libm. Legacy arm still reachable and still the divergence: "
          "%d/%d arctan entries low, libm wrong on %d/65536 angles, seed 0x%08x)"
          % (ARCTAN_LIVE, rom_fnv, len(rom_sine), rom_seed,
             n_trunc_bad, ARCTAN_LIVE, n_libm_bad, LEGACY_SEED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
