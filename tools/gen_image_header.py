#!/usr/bin/env python3
"""Emit a C header holding one image file's bytes.

The repo tracks the source art (brand/*.png) but not a generated blob: this
runs at BUILD time and writes into the build tree, so a multi-megabyte byte
array never enters git. Same reasoning as the embedded fonts, which are kept as
generated base85 rather than as tracked .ttf files -- except an image is handed
to stb_image rather than to ImFontAtlas, so plain bytes are what the decoder
wants and there is nothing to compress a second time (PNG is already deflated).
"""

from __future__ import annotations

import argparse
import pathlib
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--symbol", required=True)
    args = parser.parse_args()

    payload = args.source.read_bytes()
    if not payload:
        print(f"gen_image_header: {args.source} is empty", file=sys.stderr)
        return 1
    if payload[:8] != b"\x89PNG\r\n\x1a\n":
        print(f"gen_image_header: {args.source} is not a PNG", file=sys.stderr)
        return 1

    lines = [
        f"// Generated from {args.source.name} by tools/gen_image_header.py.",
        "// Do not edit, and do not commit: this lives in the build tree.",
        f"#ifndef MDKR_GENERATED_{args.symbol.upper()}_H",
        f"#define MDKR_GENERATED_{args.symbol.upper()}_H",
        "",
        f"static const unsigned int {args.symbol}_size = {len(payload)}u;",
        f"static const unsigned char {args.symbol}_data[{len(payload)}] = {{",
    ]
    for offset in range(0, len(payload), 20):
        chunk = payload[offset:offset + 20]
        lines.append("    " + "".join(f"0x{byte:02x}," for byte in chunk))
    lines.append("};")
    lines.append("")
    lines.append(f"#endif  // MDKR_GENERATED_{args.symbol.upper()}_H")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
