#!/usr/bin/env python3
"""Keep failed optional output shaders from crashing VitaGL or retrying forever."""

from pathlib import Path
import re
import sys


SOURCE = (Path(__file__).resolve().parents[1] / "platform" / "fast3d" /
          "gfx_opengl.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)


def function_body(name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", SOURCE, re.S)
    require(match is not None, f"missing {name}")
    start = SOURCE.find("{", match.start())
    depth = 0
    for index in range(start, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:index + 1]
    raise AssertionError(f"unterminated {name}")


def main() -> int:
    compile_shader = function_body("gfx_opengl_compile_filter_shader")
    require("if (shader == 0)" in compile_shader,
            "shader allocation failure must be handled before using the handle")

    ensure = function_body("gfx_opengl_ensure_output_filter_program")
    require("if (g_output_filter_program_failed)" in ensure,
            "a permanent compile failure must not be retried every frame")
    failure = ensure[ensure.index("if (vs == 0 || fs == 0)"):]
    failure = failure[:failure.index("g_output_filter_program = glCreateProgram()")]
    require("if (vs != 0)" in failure and "if (fs != 0)" in failure,
            "failed shader cleanup must guard zero handles for VitaGL")
    require("g_output_filter_program_failed = true;" in failure,
            "compile failure must disable the optional output pass")

    print("output filter failure contract: pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
