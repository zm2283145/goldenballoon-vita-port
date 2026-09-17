#!/usr/bin/env python3
"""Guard the Vita renderer's measured steady-state CPU fast paths."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "platform" / "fast3d" / "gfx_pc_dkr.c").read_text(
    encoding="utf-8"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)


def body(name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", SOURCE, re.S)
    require(match is not None, f"missing {name}")
    start = SOURCE.find("{", match.start())
    depth = 0
    state = "code"
    index = start
    while index < len(SOURCE):
        char = SOURCE[index]
        pair = SOURCE[index:index + 2]
        if state == "code":
            if pair == "/*":
                state = "block"
                index += 2
                continue
            if pair == "//":
                state = "line"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return SOURCE[start:index + 1]
        elif state == "block" and pair == "*/":
            state = "code"
            index += 2
            continue
        elif state == "line" and char == "\n":
            state = "code"
        elif state in ("string", "character"):
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    raise AssertionError(f"unterminated {name}")


def main() -> int:
    texcoord = body("dkr_vbo_texcoord")
    require("cur.tex_u_scale[ti]" in texcoord and
            "cur.tex_v_scale[ti]" in texcoord,
            "per-vertex UV packing must use per-draw coefficients")
    texcoord_code = re.sub(r"/\*.*?\*/|//[^\n]*", "", texcoord, flags=re.S)
    require("/" not in texcoord_code,
            "per-vertex UV packing must not restore runtime divisions")

    setup = body("dkr_setup_draw_state")
    require(setup.index("cur.tex_w[i] = w; cur.tex_h[i] = h;") <
            setup.index("dkr_update_uv_coefficients(i, td);"),
            "UV coefficients must use the resolved texture dimensions")
    require("cur.feat = comb->feat;" in setup and
            "gfx_cc_get_features" not in setup,
            "combiner features must be extracted once at creation")

    lookup = body("dkr_lookup_or_create_combiner")
    require("cc_bucket[bucket]" in lookup and "cc_chain_next[slot]" in lookup,
            "combiner lookup must use its hash index")
    require(not re.search(
        r"for\s*\(int\s+i\s*=\s*0;\s*i\s*<\s*cc_pool_size", lookup),
        "combiner lookup must not regress to a full pool scan")

    bind = body("dkr_bind_tile")
    require(re.search(
        r"font_entry\s*=\s*font_text_draw\s*\?\s*"
        r"gfx_font_registry_find", bind, re.S) is not None,
        "ordinary geometry must skip the font registry scan")

    polygon = body("dkr_sp_polygon")
    require("dkr_homogeneous_winding(&a, &b, &c)" in polygon,
            "ordinary triangle culling must avoid perspective divisions")
    require("area += dkr_ndc_cross" in polygon,
            "clipped polygon fans must retain normalized area accumulation")

    reset = body("gfx_reset_renderer_caches")
    require("tex_cache_index_initialized = false;" in reset and
            "cc_index_initialized = false;" in reset,
            "renderer rebind must retire both hash indexes")
    print("renderer hot-path contract: pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
