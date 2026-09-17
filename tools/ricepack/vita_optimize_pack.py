#!/usr/bin/env python3
"""Optimise a Rice/GLideN64 hi-res texture pack for PS Vita.

The runtime reads Rice packs as-is, but on Vita every replacement is PNG-decoded
on a helper thread, mip-chained, then uploaded as uncompressed RGBA8. Large and
non-power-of-two images make every step slower, and NPOT images get no mipmaps
at all on Vita (vitaGL issue #24).

This tool writes a new pack where every texture is:

  * a single `_all` RGBA image (an `_rgb` + `_a` pair is merged, so the game
    reads one file instead of two and skips the per-pixel alpha merge);
  * power-of-two on both sides (so the Vita mip path is used);
  * no larger than --max-size on either side.

Two output formats:

  png   (default) ordinary PNG, decoded on the Vita.
  vtex  raw RGBA8 plus the COMPLETE mip chain, prebuilt here with the same
        filter the engine uses (exact-area box, filtered in linear light,
        colour premultiplied by alpha). The Vita then does no PNG inflate and
        no mip construction: loading a texture is a file read. Files are much
        larger on disk (~1.33x the raw size) but far cheaper to load.

Texture coordinates are normalised against the ORIGINAL tile size by the
renderer, so resizing a replacement never changes how it maps.

Usage:
  python vita_optimize_pack.py SRC DST [--max-size 512] [--format vtex]
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import os
import re
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

RICE_RE = re.compile(
    r"^(?P<stem>.+#[0-9A-Fa-f]{8}#\d+#\d+)(?P<suffix>_all|_rgb|_a)\.png$",
    re.IGNORECASE)

VTEX_MAGIC = b"VTEX"
VTEX_VERSION = 1
VTEX_FORMAT_RGBA8 = 0
VTEX_HEADER_BYTES = 32

# sRGB <-> linear, matching platform/fast3d/gfx_mipgen.c.
_SRGB_TO_LINEAR = np.where(
    (np.arange(256) / 255.0) <= 0.04045,
    (np.arange(256) / 255.0) / 12.92,
    (((np.arange(256) / 255.0) + 0.055) / 1.055) ** 2.4,
).astype(np.float32)


def linear_to_srgb_u8(v: np.ndarray) -> np.ndarray:
    v = np.clip(v, 0.0, 1.0)
    out = np.where(v <= 0.0031308, v * 12.92, 1.055 * (v ** (1.0 / 2.4)) - 0.055)
    return np.clip(out * 255.0 + 0.5, 0, 255).astype(np.uint8)


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
    return max(min(pot_nearest(w), max_size), 1), max(min(pot_nearest(h), max_size), 1)


def resize_rgba(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    if img.size == size:
        return img
    pm = img.convert("RGBa").resize(size, Image.Resampling.LANCZOS, reducing_gap=3.0)
    return pm.convert("RGBA")


def halve(level: np.ndarray) -> np.ndarray:
    """One mip reduction: exact box in linear light, colour weighted by alpha
    (the engine's premultiplied accumulation). Each axis halves independently,
    so 1xN and odd sizes behave like the engine's floor-halving."""
    h, w, _ = level.shape
    nh, nw = max(h >> 1, 1), max(w >> 1, 1)
    fy, fx = (2 if h > 1 else 1), (2 if w > 1 else 1)
    src = level[: nh * fy, : nw * fx]
    lin = _SRGB_TO_LINEAR[src[..., :3]]
    alpha = src[..., 3].astype(np.float32) / 255.0
    blocks = lin.reshape(nh, fy, nw, fx, 3)
    ablocks = alpha.reshape(nh, fy, nw, fx)
    wsum = ablocks.sum(axis=(1, 3))
    colour = (blocks * ablocks[..., None]).sum(axis=(1, 3))
    safe = np.where(wsum > 0.0, wsum, 1.0)
    colour = colour / safe[..., None]
    plain = blocks.mean(axis=(1, 3))
    colour = np.where((wsum > 0.0)[..., None], colour, plain)
    out = np.empty((nh, nw, 4), dtype=np.uint8)
    out[..., :3] = linear_to_srgb_u8(colour)
    out[..., 3] = np.clip(ablocks.mean(axis=(1, 3)) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return out


def write_vtex(path: Path, rgba: np.ndarray) -> int:
    h, w, _ = rgba.shape
    levels = [rgba]
    lw, lh = w, h
    while lw > 1 or lh > 1:
        levels.append(halve(levels[-1]))
        lw, lh = max(lw >> 1, 1), max(lh >> 1, 1)
    payload = b"".join(l.tobytes() for l in levels)
    header = struct.pack(
        "<4sHHHHBBHII8x", VTEX_MAGIC, VTEX_VERSION, VTEX_FORMAT_RGBA8,
        w, h, len(levels), 0, 0, len(payload), 0)
    assert len(header) == VTEX_HEADER_BYTES, len(header)
    path.write_bytes(header + payload)
    return len(header) + len(payload)


def process(job: dict, max_size: int, fmt: str) -> dict:
    out = Path(job["out"])
    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        if job["all"]:
            img = Image.open(job["all"]).convert("RGBA")
        else:
            img = Image.open(job["rgb"]).convert("RGBA")
            paired = False
            if job["a"]:
                a = Image.open(job["a"]).convert("RGBA")
                if a.size == img.size:
                    img.putalpha(a.getchannel("R"))  # runtime rule: RED is alpha
                    paired = True
            if not paired:
                img.putalpha(255)  # runtime rule: unpaired _rgb is opaque
        src = img.size
        dst = target_size(src[0], src[1], max_size)
        img = resize_rgba(img, dst)
        if fmt == "vtex":
            size = write_vtex(out, np.asarray(img, dtype=np.uint8))
        else:
            img.save(out, "PNG", optimize=False, compress_level=6)
            size = out.stat().st_size
        return {"ok": True, "src": src, "dst": dst, "bytes": size}
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "error": f"{job['stem']}: {exc}"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--max-size", type=int, default=512)
    ap.add_argument("--format", choices=("png", "vtex"), default="png")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if args.max_size < 1 or args.max_size & (args.max_size - 1):
        ap.error("--max-size must be a power of two")

    src, dst = Path(args.src), Path(args.dst)
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

    ext = ".vtex" if args.format == "vtex" else ".png"
    jobs, orphan_alpha = [], []
    for g in groups.values():
        if not g["all"] and not g["rgb"]:
            orphan_alpha.append(g["a"])
            continue
        g["out"] = str(dst / g["dir"] / f"{g['stem']}_all{ext}")
        jobs.append(g)

    results = []
    with cf.ProcessPoolExecutor(max_workers=args.jobs) as ex:
        for r in ex.map(process, jobs, [args.max_size] * len(jobs),
                        [args.format] * len(jobs), chunksize=8):
            results.append(r)

    ok = [r for r in results if r["ok"]]
    bad = [r for r in results if not r["ok"]]
    src_px = sum(r["src"][0] * r["src"][1] for r in ok)
    dst_px = sum(r["dst"][0] * r["dst"][1] for r in ok)
    print(f"format           : {args.format}")
    print(f"textures written : {len(ok)}")
    print(f"resized          : {sum(1 for r in ok if r['src'] != r['dst'])}")
    print(f"decoded RGBA     : {src_px * 4 / 2**20:.1f} MiB -> {dst_px * 4 / 2**20:.1f} MiB")
    print(f"output bytes     : {sum(r['bytes'] for r in ok) / 2**20:.1f} MiB")
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
