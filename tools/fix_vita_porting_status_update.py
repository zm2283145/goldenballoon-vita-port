import sys

path = sys.argv[1] + r"/PORTING_STATUS.md"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

old_status = (
    "**Status: builds and links clean (`arm-vita-eabi-gcc` → `mdkr64` ELF →\n"
    "`mdkr64.vpk`). Not yet booted on real hardware.** Everything below is\n"
    "compiler-error-driven engineering, not guesswork — but a first boot on a\n"
    "real Vita will surface a second round of issues (input mapping, audio,\n"
    "performance, GPU-side rendering correctness) that no amount of further\n"
    "desk-checking will find. See \"What needs hardware verification\" at the end.\n"
)
assert src.count(old_status) == 1, "status count=%d" % src.count(old_status)

new_status = (
    "**Status: boots on real hardware, reaches the main menu with audio and\n"
    "textured rendering, actively being debugged past there.** This moved past\n"
    "\"builds and links clean\" through hands-on, on-device bring-up: real\n"
    "crashes, pulled via a boot-time file logger and coredumps, root-caused one\n"
    "at a time. Fixed so far, in the order they were hit:\n"
    "\n"
    "1. **Black screen, audio/input alive.** `platform_sdl_surface_presentable()`\n"
    "   treated vitaGL's intentionally-always-NULL `s_window` as \"not\n"
    "   presentable,\" permanently eliding presentation after frame 0.\n"
    "2. **Wild-jump crash once rendering started.** `dkr_resolve()`'s 32-bit\n"
    "   \"direct recovery\" fast path trusted any pointer-shaped value with no\n"
    "   bounds check — safe on 64-bit targets (where it's naturally filtered),\n"
    "   a real bug on Vita's 32-bit ABI. Fixed with an explicit floor check.\n"
    "3. **Hard crash inside SceGxm/vitaShaRK on the very first real shader\n"
    "   compile, on every shader regardless of complexity.** Root cause: this\n"
    "   file's `MGB64_PORTMASTER_GLES` code path emits ES3-style GLSL\n"
    "   (`#version 320 es`, `in`/`out` qualifiers, a user `fragColor` output,\n"
    "   `texture()`/`textureLod()`), but vitaGL's runtime GLSL→Cg translator\n"
    "   only understands the legacy GLSL ES 1.00 dialect. Fixed with a\n"
    "   Vita-only source rewrite (`dkr_vita_rewrite_glsl_to_legacy()` in\n"
    "   `gfx_opengl.c`) run on the generated shader text right before it's\n"
    "   compiled.\n"
    "\n"
    "Currently being debugged past the main menu — see \"What needs\n"
    "hardware verification\" at the end for what's still open, and the git log\n"
    "on this branch for the blow-by-blow.\n"
)
src = src.replace(old_status, new_status, 1)

old_verify_bullet_anchor = (
    "- **Coverage-stencil / framebuffer-snapshot degradations** — both are\n"
)
assert src.count(old_verify_bullet_anchor) == 1, "verify anchor count=%d" % src.count(old_verify_bullet_anchor)

new_verify_bullet = (
    "- **`textureSize()`/`texelFetch()` in the texture clamp/tile-mask,\n"
    "  SSAO, and framebuffer-diagnostic code paths** — unlike plain\n"
    "  `texture()`/`textureLod()` (fixed by renaming to `texture2D()`, see the\n"
    "  git log), these ES3 functions have no equivalent at all in the legacy\n"
    "  GLSL ES 1.00 dialect vitaGL's runtime translator understands, so a\n"
    "  shader that reaches one of these paths on Vita will need an actual\n"
    "  logic rewrite (e.g. passing texture size as a uniform), not just a\n"
    "  syntax translation. Not yet hit by any shader reached so far; flagged\n"
    "  here so the next occurrence is recognized immediately instead of\n"
    "  requiring a fresh round of coredump archaeology.\n"
    "- **Coverage-stencil / framebuffer-snapshot degradations** — both are\n"
)
src = src.replace(old_verify_bullet_anchor, new_verify_bullet, 1)

with open(path, "wb") as f:
    f.write(src.encode("utf-8"))

print("patched", path)
