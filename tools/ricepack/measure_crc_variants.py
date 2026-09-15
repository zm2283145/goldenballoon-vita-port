#!/usr/bin/env python3
"""Decide which of rice_crc.py's four candidate CRCs is the one Rice uses.

    tools/ricepack/measure_crc_variants.py \\
        --build build --rom baserom.us.v80.z64 \\
        --pack /path/to/rice-pack --out ~/dkr-crc-experiment

That is the whole experiment. It dumps a texture corpus from a real ROM,
computes all four candidate CRCs over the raw spans the engine hashed, and
reports what fraction of the pack's mappable keys each variant matched.

Why it needs a ROM, and why it cannot be a unit test
----------------------------------------------------
A Rice pack names a texture by a CRC over the raw N64 texel bytes. Those bytes
exist only inside a running game. rice_crc.py can be tested for internal
consistency without one -- and is, by tests/test_rice_crc.py -- but no ROM-free
test can show that its arithmetic is the arithmetic Rice performed. Only a
comparison against filenames a real pack carries can, and that comparison needs
the bytes on one side of it.

Nothing ROM-derived is written into the repository: `--out` must name a path
outside the working tree, which tools/mod_texture_dump.py enforces again on its
own behalf.

Reading the table
-----------------
A key counts as matched only when the candidate CRC agrees AND the fmt/siz the
dump recorded agrees with the fmt/siz in the pack filename. A wrong variant is
therefore expected to score ZERO, not "low" -- an accidental 32-bit collision
that also agrees on format is a ~2^-32 event per comparison. So:

  * one variant in the hundreds, the other three at 0
        -> that variant is the algorithm, to the strength of this sample.
  * every variant at 0
        -> read the skip counts first. A corpus that is mostly
           `crc-input-rejected` means the geometry and the span disagree, which
           is a broken INPUT, not a wrong algorithm. If the spans were accepted
           and everything still scores 0, sweep the input before the
           arithmetic: pitch = packed row bytes versus pitch =
           source_line_bytes, and height = source_size_bytes / pitch versus the
           tile's logical height.
  * two or more variants scoring highly
        -> a bug in rice_crc.py, not a discovery. The variants are supposed to
           be independent and test_rice_crc.py asserts that they are.

A PARTIAL score is the expected shape of a success. The dump only covers the
routes it was driven over and the pack only covers part of the game, so the
number to compare against is how many textures were DUMPED, not how large the
pack is. Both are printed.

Two things deflate every variant's percentage equally, and neither can change
which variant wins: a route that never bound a texture the pack replaces, and
a colour-index key. A CI texture's Rice filename carries a second, palette CRC
that this crosswalk does not compute, so such a key counts as mappable and can
never be matched. Read the winner from the table; read the percentage as a
floor.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import import_rice_pack as importer                            # noqa: E402
import rice_crc                                                # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DUMP_TOOL = ROOT / "tools" / "mod_texture_dump.py"
SCRIPT_DIR = ROOT / "tests" / "input_scripts"

# Routes driven when none are named. Chosen for texture BREADTH rather than for
# what any one of them proves: the front end, the character roster, a whole
# race, and the adventure overworld. Each is an existing fixture with a known
# frame budget, so none of these numbers is a guess about how long a route
# takes. More routes only ever help -- a texture the dump never saw is a key
# that cannot match, for every variant equally, so an under-driven corpus
# lowers every score together and cannot pick a wrong winner.
DEFAULT_ROUTES = (
    ("nav_to_track_select.txt", 1200),
    ("nav_to_character_select.txt", 1600),
    ("race_full_3lap.txt", 8000),
    ("adventure_hub_drive.txt", 12000),
)


def parse_route(value: str) -> tuple[str, int]:
    """`script.txt:frames`, or `script.txt` for the default budget."""
    name, _, frames = value.partition(":")
    if frames and not frames.isdigit():
        raise argparse.ArgumentTypeError(
            f"route '{value}': frames must be a number")
    return name, int(frames) if frames else 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default="build",
                        help="build directory or path to the mdkr64 binary")
    parser.add_argument("--rom", default="baserom.us.v80.z64")
    parser.add_argument("--pack", required=True,
                        help="the Rice pack directory to measure against")
    parser.add_argument("--rom-name",
                        help="passed through to the pack scan; refuses files "
                             "naming any other ROM")
    parser.add_argument("--out", required=True,
                        help="working directory for the corpus and the "
                             "reports. MUST be outside this repository -- it "
                             "fills with decoded ROM pixels and raw ROM texels")
    parser.add_argument("--route", type=parse_route, action="append",
                        metavar="SCRIPT[:FRAMES]",
                        help="an input script under tests/input_scripts/ to "
                             "drive, repeatable; defaults to "
                             + ", ".join(name for name, _ in DEFAULT_ROUTES))
    parser.add_argument("--frames", type=int, default=1800,
                        help="frame budget for a --route that gave none")
    parser.add_argument("--renderer", choices=("gl", "webgpu"), default="gl")
    parser.add_argument("--skip-dump", action="store_true",
                        help="reuse the corpus already in <out>/corpus instead "
                             "of driving the game again; the measurement is "
                             "seconds, the dump is not")
    parser.add_argument("-v", "--verbose", action="store_true")
    return parser.parse_args(argv)


def dump_corpus(args: argparse.Namespace, corpus: Path) -> int:
    """Drive every route into one corpus directory. Returns 1 on failure.

    Routes accumulate: each run is its own process and writes each digest it
    has not written, so a texture two routes both bind lands once with the same
    bytes either way.
    """
    routes = args.route if args.route else list(DEFAULT_ROUTES)
    for index, (name, frames) in enumerate(routes, start=1):
        script = Path(name)
        if not script.is_absolute() and not script.exists():
            script = SCRIPT_DIR / name
        if not script.is_file():
            print(f"FAIL: input script not found: {script}", file=sys.stderr)
            return 1
        budget = frames or args.frames
        print(f"[{index}/{len(routes)}] dumping {script.name} "
              f"({budget} frames)")
        command = [
            sys.executable, str(DUMP_TOOL),
            "--build", args.build,
            "--rom", args.rom,
            "--input-script", str(script),
            "--frames", str(budget),
            "--out", str(corpus),
            "--renderer", args.renderer,
        ]
        if args.verbose:
            command.append("-v")
        result = subprocess.run(command, cwd=ROOT)
        if result.returncode != 0:
            print(f"FAIL: the dump of {script.name} exited "
                  f"{result.returncode}", file=sys.stderr)
            return 1
    return 0


def skip_counts(crosswalk: dict) -> dict[str, int]:
    """Skip reasons, collapsed to their stable prefix, with counts."""
    counts: dict[str, int] = {}
    for entry in crosswalk["skipped"]:
        reason = entry["reason"].split(":")[0]
        counts[reason] = counts.get(reason, 0) + 1
    return counts


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    pack = Path(args.pack).expanduser()
    if not pack.is_dir():
        print(f"FAIL: not a directory: {pack}", file=sys.stderr)
        return 1

    out = Path(args.out).expanduser().resolve()
    # Stricter than mod_texture_dump.py, which allows the git-ignored
    # `mod-texture-dump/`, and deliberately so: this directory also collects
    # crosswalk.json and variant-hit-rate.json, which are ROM-derived (every
    # digest and CRC in them came out of a cartridge) and which no ignore rule
    # covers. Checked here rather than only there for a second reason as well:
    # --skip-dump never reaches that check at all.
    if out == ROOT or ROOT in out.parents:
        print(f"FAIL: --out {out} is inside the source tree ({ROOT}).\n"
              "      This fills with decoded ROM pixels and raw ROM texels; "
              "committing them would break the clean-room guarantee.",
              file=sys.stderr)
        return 1
    corpus = out / "corpus"
    out.mkdir(parents=True, exist_ok=True)

    if args.skip_dump:
        if not corpus.is_dir():
            print(f"FAIL: --skip-dump but there is no corpus at {corpus}",
                  file=sys.stderr)
            return 1
        print(f"reusing the corpus in {corpus}")
    else:
        corpus.mkdir(parents=True, exist_ok=True)
        if dump_corpus(args, corpus) != 0:
            return 1

    print("computing every candidate CRC over the dumped spans")
    crosswalk = importer.build_crosswalk(corpus)
    crosswalk_path = out / "crosswalk.json"
    crosswalk_path.write_text(
        json.dumps(crosswalk, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")

    print("scanning the pack")
    scan = importer.scan_pack(pack, args.rom_name)
    mappable = sum(1 for key in scan.keys
                   if key.verdict == importer.VERDICT_SELECTED)

    rows = []
    for variant in rice_crc.VARIANTS:
        table = crosswalk["variants"][variant]
        hits = importer.count_mapped(scan, table)
        rows.append({
            "variant": variant,
            "hits": hits,
            "mappable": mappable,
            "percentOfMappable": round(100.0 * hits / mappable, 4)
                                 if mappable else 0.0,
            "keyCollisions": crosswalk["keyCollisions"][variant],
        })

    skips = skip_counts(crosswalk)
    summary = {
        "crcStatus": "measured" if any(r["hits"] for r in rows) else "unvalidated",
        "corpus": str(corpus),
        "pack": str(pack),
        "texturesConsidered": crosswalk["texturesConsidered"],
        "texturesWithSpans": crosswalk["texturesMapped"],
        "dumpFormatsRead": crosswalk["dumpFormatsRead"],
        "keysDiscovered": len(scan.keys),
        "keysMappable": mappable,
        "skipReasons": skips,
        "variants": rows,
    }
    (out / "variant-hit-rate.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print()
    print(f"corpus: {crosswalk['texturesConsidered']} dumped texture(s), "
          f"{crosswalk['texturesMapped']} with a usable texel span")
    if skips:
        print("        skipped: "
              + ", ".join(f"{count} {reason}"
                          for reason, count in sorted(skips.items())))
    print(f"pack:   {len(scan.keys)} key(s) discovered, {mappable} mappable")
    print()
    print("Rice CRC variant hit-rate (a wrong variant is expected to score 0):")
    for row in rows:
        note = (f"  [{row['keyCollisions']} key collision(s) in the dump]"
                if row["keyCollisions"] else "")
        print(f"  {row['variant']:<16} {row['hits']:>6} of {mappable} "
              f"({row['percentOfMappable']:5.1f}%){note}")
    print()
    winners = [row["variant"] for row in rows if row["hits"] > 0]
    if not winners:
        print("VERDICT: none of the four matched anything. Read the skip counts "
              "above before touching the arithmetic -- see the module docstring.")
    elif len(winners) == 1:
        print(f"VERDICT: '{winners[0]}' is the only variant that matches. That "
              "is the algorithm, to the strength of this sample.")
    else:
        print("VERDICT: more than one variant matched, which the variants being "
              "independent says cannot happen. Treat this as a bug in "
              "rice_crc.py, not as a result.")
    print()
    print(f"wrote {crosswalk_path}")
    print(f"wrote {out / 'variant-hit-rate.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
