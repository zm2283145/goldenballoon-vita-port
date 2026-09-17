#!/usr/bin/env python3
"""Optimise a Rice/GLideN64 hi-res texture pack for PS Vita.

The runtime reads Rice packs as-is, but on Vita every replacement is PNG-decoded
on the render thread, then CPU-mipmapped, then uploaded as uncompressed RGBA8.
Large and non-power-of-two images make every one of those steps slower, and
NPOT images get no mipmaps at all on Vita (vitaGL issue #24).

This tool writes a new pack where every texture is:

  * a single `_all` RGBA image (an `_rgb` + `_a` pair is merged, so the game
    decodes one PNG instead of two and skips the per-pixel alpha merge);
  * power-of-two on both sides (so the Vita mip path is used);
  * no larger than --max-size on either side (default 512).

Texture coordinates are normalised against the ORIGINAL tile size by the
renderer, so resizing a replacement never changes how it maps.

File names (the Rice identity) are preserved; directory layout is preserved.
Non-PNG files and files that are not Rice names are skipped and reported.

Usage:
  python vita_optimize_pack.py SRC_DIR DST_DIR [--max-size 512] [--jobs N]
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import re
import sys
from pathlib import Path

from PIL import Image

RICE_RE = re.compile(r"^(?P<stem>.+#[0-9A-Fa-f]{8}#\d+#\d+)(?P<suffix>_all|_rgb|_a)\.png$",
                     re.IGNORECASE)


def pot_floor(n: int) -> int:
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def pot_nearest(n: int) -> int:
    lo = pot_floor(n)
    hi = lo * 2
    return lo if (n - lo) <= (hi - n) else hi


def target_size(w: int, h: int, max_size: int) -> tuple[int, int]:
    tw = min(pot_nearest(w), max_size)
    th = min(pot_nearest(h), max_size)
    return max(tw, 1), max(th, 1)


def resize_rgba(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    if img.size == size:
        return img
    # Premultiplied resample so transparent texels do not bleed dark fringes.
    pm = img.convert("RGBa")
    pm = pm.resize(size, Image.Resampling.LANCZOS, reducing_gap=3.0)
    return pm.convert("RGBA")


def process(job: dict, max_size: int) -> dict:
    out = Path(job["out"])
    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        if job["all"]:
            img = Image.open(job["all"]).convert("RGBA")
        else:
            img = Image.open(job["rgb"]).convert("RGBA")
            alpha_ok = False
            if job["a"]:
                a = Image.open(job["a"]).convert("RGBA")
                if a.size == img.size:
                    img.putalpha(a.getchannel("R"))  # runtime rule: RED is alpha
                    alpha_ok = True
            if not alpha_ok:
                img.putalpha(255)  # runtime rule: unpaired/mismatched _rgb is opaque
        src = img.size
        dst = target_size(src[0], src[1], max_size)
        img = resize_rgba(img, dst)
        img.save(out, "PNG", optimize=False, compress_level=6)
        return {"ok": True, "src": src, "dst": dst, "out": str(out),
                "bytes": out.stat().st_size}
    except Exception as exc:  # noqa: BLE001 - report and continue
        return {"ok": False, "error": f"{job['stem']}: {exc}"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--max-size", type=int, default=512)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if args.max_size < 1 or args.max_size & (args.max_size - 1):
        ap.error("--max-size must be a power of two")

    src = Path(args.src)
    dst = Path(args.dst)
    groups: dict[tuple[str, str], dict] = {}
    skipped: list[str] = []
    for path in sorted(src.rglob("*")):
        if not path.is_file():
            continue
        m = RICE_RE.match(path.name)
        if not m:
            skipped.append(str(path.relative_to(src)))
            continue
        rel_dir = str(path.parent.relative_to(src))
        key = (rel_dir, m.group("stem").lower())
        g = groups.setdefault(key, {"stem": m.group("stem"), "dir": rel_dir,
                                    "all": None, "rgb": None, "a": None})
        g[m.group("suffix")[1:].lower()] = str(path)

    jobs, orphan_alpha = [], []
    for g in groups.values():
        if not g["all"] and not g["rgb"]:
            orphan_alpha.append(g["a"])
            continue
        g["out"] = str(dst / g["dir"] / f"{g['stem']}_all.png")
        jobs.append(g)

    results = []
    with cf.ProcessPoolExecutor(max_workers=args.jobs) as ex:
        for r in ex.map(process, jobs, [args.max_size] * len(jobs), chunksize=8):
            results.append(r)

    ok = [r for r in results if r["ok"]]
    bad = [r for r in results if not r["ok"]]
    src_px = sum(r["src"][0] * r["src"][1] for r in ok)
    dst_px = sum(r["dst"][0] * r["dst"][1] for r in ok)
    resized = sum(1 for r in ok if r["src"] != r["dst"])
    print(f"textures written : {len(ok)}")
    print(f"resized          : {resized}")
    print(f"decoded RGBA     : {src_px * 4 / 2**20:.1f} MiB -> {dst_px * 4 / 2**20:.1f} MiB")
    print(f"output PNG bytes : {sum(r['bytes'] for r in ok) / 2**20:.1f} MiB")
    print(f"skipped files    : {len(skipped)}")
    for s in skipped[:20]:
        print(f"   skip {s}")
    print(f"orphan _a halves : {len(orphan_alpha)}")
    print(f"errors           : {len(bad)}")
    for r in bad[:20]:
        print(f"   {r['error']}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
