# goldenballoon (mdkr64) — PS Vita port status

This branch (`vita-port`) adds an initial PS Vita target to goldenballoon's
CMake build, cross-compiled with VitaSDK against **vitaGL** (OpenGL-over-
sceGxm) and **vitashark** (runtime GLSL→GXP shader compiler), following the
same libultraship/vitaGL pattern as
[Rinnegatamante/Lighthouse](https://github.com/Rinnegatamante/Lighthouse)
(a Banjo-Kazooie Vita port used as the concrete reference for library
choices, link flags, and the VPK packaging recipe).

**Status: 0.02 — boots on real hardware and plays through races on the
default (Restored) visual preset, with audio, input, and textured
rendering all working.** This moved past "builds and links clean" through
hands-on, on-device bring-up: real crashes, pulled via a boot-time file
logger and coredumps, root-caused one at a time. **The Remastered visual
preset currently crashes on startup and must not be used — see
[Known issues](#known-issues) below.** Fixed so far, in the order they
were hit:

1. **Black screen, audio/input alive.** `platform_sdl_surface_presentable()`
   treated vitaGL's intentionally-always-NULL `s_window` as "not
   presentable," permanently eliding presentation after frame 0.
2. **Wild-jump crash once rendering started.** `dkr_resolve()`'s 32-bit
   "direct recovery" fast path trusted any pointer-shaped value with no
   bounds check — safe on 64-bit targets (where it's naturally filtered),
   a real bug on Vita's 32-bit ABI. Fixed with an explicit floor check.
3. **Hard crash inside SceGxm/vitaShaRK on the very first real shader
   compile, on every shader regardless of complexity.** Root cause: this
   file's `MGB64_PORTMASTER_GLES` code path emits ES3-style GLSL
   (`#version 320 es`, `in`/`out` qualifiers, a user `fragColor` output,
   `texture()`/`textureLod()`), but vitaGL's runtime GLSL→Cg translator
   only understands the legacy GLSL ES 1.00 dialect. Fixed with a
   Vita-only source rewrite (`dkr_vita_rewrite_glsl_to_legacy()` in
   `gfx_opengl.c`) run on the generated shader text right before it's
   compiled.
4. **Video settings failed to persist across restarts.** The Vita save
   path for video/preset settings was being written somewhere that didn't
   survive a relaunch; fixed so settings (including which visual preset is
   active) now save and load correctly.
5. **Stack-overflow crash during normal play.** A code path allocated a
   large buffer on the stack where the desktop build's much larger default
   stack absorbed it silently; fixed by moving it off the stack for the
   Vita target.
6. **Crash from World Shadows / `SHADER_OPT_SUN_SHADOW`.** This
   Remastered-only effect depends on GL features vitaGL doesn't implement
   (see the disabled-features table below) and wasn't fully excluded on
   Vita; it's now force-disabled with an explicit `__vita__` check in
   `gfx_pc_dkr.c`, confirmed inert via boot-log instrumentation.
7. **Wrong GLSL version header on every fragment shader.** The vertex
   shader's version-header selection correctly checked `__vita__` first,
   but the fragment shader's only checked `MGB64_PORTMASTER_GLES` (also
   defined for Vita), so every fragment shader got a `#version 320 es`
   header on a body that had already been rewritten to legacy GLSL ES
   1.00. Fixed by adding the same `__vita__` branch to the fragment header
   selection. Independently confirmed correct, but did **not** fix the
   Remastered-preset crash below — that turned out to be a separate,
   still-open issue.

Currently: the Restored preset is stable enough for normal play; the
Remastered preset crashes on startup every time (see
[Known issues](#known-issues)). See the git log on this branch for the
blow-by-blow of everything ruled out chasing it.

**vitaGL / vitaShaRK versions:** built from
[Rinnegatamante/vitaGL](https://github.com/Rinnegatamante/vitaGL) commit
`cd3791e` and [Rinnegatamante/vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK)
commit `df24065` (both HEAD as of 2026-08-30/2026-08-22 respectively),
built from source rather than dpm's prebuilt packages (no make on this
toolchain's host machine, so both are compiled via one-off scripts that
replicate their Makefiles). Splash screen enabled (NO_SPLASHSCREEN unset)
so the vitaGL boot logo shows on real hardware as a visual "did vitaGL
initialize" signal. Earlier in bring-up, vitaGL HEAD alone (without
updating vitaShaRK to match) failed to link — HEAD's
glSetShaderAssociationPath calls shark_set_shader_association_path,
introduced in vitaShaRK the same day but absent from the vitaShaRK version
originally paired with this toolchain — so the two must be updated
together, not independently.

## How to build

```powershell
# One-time setup (VitaSDK, cmake, ninja already installed under
# C:\vitasdk and C:\vitasdk-tools in this environment):
cmake -S . -B build-vita -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE=$env:VITASDK/share/vita.toolchain.cmake `
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita --target mdkr64
```

`CMakeLists.txt` detects `VITA` (set by `vita.toolchain.cmake`) and forces
`MDKR_WEBGPU_BACKEND`, `MDKR_APP`, and `MDKR_NATIVE_PHONE_PARTY` off before
any of their downstream `include()`s run (see the "Vita platform" block
near the top of the WebGPU option handling) — no manual flags needed beyond
the toolchain file itself.

## Packaging (VPK)

The one-time `vita-mksfoex` step (below) aside, packaging is scripted --
run `tools/package_vita.ps1` after every rebuild rather than reproducing
the individual strip/elf-create/fself/pack-vpk commands by hand. Two things
that script gets right that a naive hand-rolled version will not:

- `vita-make-fself -c -pm 0x2000000 ...` — the `-pm 0x2000000` (32MB
  physically-contiguous memory budget) flag is required. Without it,
  vitaGL's `vglInitExtended` silently fails (returns `GL_FALSE`) even
  though free-memory-pool numbers look healthy afterward — the game boots
  and audio/input/game logic all run fine, but nothing ever renders.
  Diagnosed via `mdkr_vita_boot_log` instrumentation around
  `vglInitExtended`.
- No `-a vita/livearea/...=sce_sys/...` LiveArea-asset arguments to
  `vita-pack-vpk` by default (`-IncludeIcons` opts back in for a
  deliberate one-off test). Bundling them currently breaks VitaShell's
  install on the real hardware this project is tested against — confirmed
  twice, including once after ruling out `-Wl,-q` as the cause. The
  placeholder `icon0.png`/`bg.png`/`startup.png` are individually
  valid PNGs at the expected LiveArea dimensions, so this looks like a
  `vita-pack-vpk`/VitaShell interaction rather than bad art; root cause
  not yet isolated. Until it is, ship without LiveArea assets (Vita falls
  back to a default icon/background).

`powershell
vita-mksfoex -s TITLE_ID=GBLN00001 -d ATTRIBUTE2=12 "GoldenBalloon DKR" build-vita/param.sfo   # one-time / only if param.sfo is missing
pwsh -File tools/package_vita.ps1 -BuildDir build-vita
`

`vita/livearea/*.png` are **programmatically generated placeholders**
(solid background + wordmark text), not final art, and are currently
unused in packaging for the reason above. `vita/livearea/gen_livearea_assets.py`
(not committed; available on request) regenerates them if needed once the
install-failure cause is found and fixed.

**ROM placement:** the engine looks for the ROM at a fixed path on Vita —
`ux0:data/goldenballoon/baserom.us.v80.z64` — since there is no in-app
ROM-picker UI on this platform yet (see "What's disabled" below). Copy a
legally-obtained US v1.0 (v80) Diddy Kong Racing ROM there before first
launch.

## What's disabled or stubbed on Vita, and why

| Feature | Status | Reason |
|---|---|---|
| WebGPU renderer (`MDKR_WEBGPU_BACKEND`) | Off | No WebGPU driver exists for the Vita's PowerVR SGX543MP4+; vitaGL (GL-over-sceGxm) is the only viable graphics path, same choice Lighthouse made. |
| ImGui launcher/overlay (`MDKR_APP`) | Off | Pulls in desktop file dialogs, DPI/multi-monitor queries, and other desktop-only surface. The plain CLI entry point (`platform/main_pc.c`) is used instead, with the ROM at a fixed path (see above) rather than a picker UI. |
| Phone Party (`MDKR_NATIVE_PHONE_PARTY`) | Off | Needs the datachannel/WebRTC stack, which isn't ported. |
| Online play | Off (compiles, doesn't run) | `MDKR_ENABLE_ONLINE_BETA` defaults off; a `"vita"` tag was added to `compatibility_identity.c`'s platform fence only so the file compiles — this makes Vita its own determinism domain if online is ever enabled here later, rather than silently colliding with another platform's replay data. |
| Sun-shadow mapping | Off | Needs `GL_TEXTURE_2D_ARRAY` / `glTexImage3D` / `glFramebufferTextureLayer`, none of which vitaGL implements. The whole feature (`gfx_opengl_ensure_shadow_resources` / `gfx_opengl_render_shadow_map` and friends in `gfx_opengl.c`) is compiled out; the shader-uniform receiver side already treats "shadow map not ready" as a normal, handled state, so there's no special-casing needed elsewhere. |
| MSAA (`Video.MSAA`) | Off (always 0) | vitaGL has no `GL_MAX_SAMPLES` / `glRenderbufferStorageMultisample`. `Video.RenderScale` (supersampling) remains available as the anti-aliasing option. |
| Coverage-stencil diagnostic (`GE007_DIAG_XLU_COVERAGE_STENCIL_CC`) | Degraded | Off by default anyway (opt-in env var); if ever set on Vita it now allocates a depth-only render target instead of packed depth24-stencil8 (`GL_DEPTH_STENCIL`/`GL_UNSIGNED_INT_24_8` aren't in vitaGL). |
| Framebuffer snapshot / frame-dump / `--dump-frames` diagnostics | Compiles, untested | Uses `glReadBuffer`, which vitaGL doesn't implement — the read-buffer save/select/restore calls are skipped on Vita (there's only ever one readable buffer at a time there anyway). Not expected to be used in normal play. |
| GL-fence frame-pacing backpressure | Compiled out | Relies on `GLsync`/`glFenceSync`/`glClientWaitSync`, none implemented by vitaGL. This was already dead *at runtime* even before this port (it only activates when `sdl_apply_gl_present_policy()` has run, which the Vita init path never calls — sceGxm/vitaGL does its own frame pacing) — the fix just makes the dead branch compile-clean instead of referencing missing GL symbols. |
| Mipmap level clamping (`GL_TEXTURE_BASE_LEVEL`/`MAX_LEVEL`) | Skipped | Not in vitaGL. Every uploaded mip level is simply usable without an explicit range clamp — matches how the desktop code already treats freshly-uploaded single-level textures. |
| Process self-relaunch (`mdkr_exec_replace_utf8`) | Returns `ENOSYS` | No `fork`/`exec` process model on Vita. |
| Save-file advisory locking (`flock`) | No-op | A Vita app is always the only process touching its own save data; nothing to arbitrate against. |

## What was fixed to make it compile/link (the substance of this branch)

- **CMake**: SDL2/GL `find_package` bypassed for Vita (uses the VitaSDK
  sysroot paths directly — `arm-vita-eabi-pkg-config` is a bash script,
  unusable from a plain Windows build); the desktop unit-test suite
  (`cmake/tests.cmake`) is skipped entirely for Vita; `-fno-short-enums
  -fsigned-char` added (ARM EABI's default enum/char signedness differs from
  x86/x64 GCC's, which this decomp codebase implicitly assumes — same fix
  Lighthouse's `Makefile.vita` uses); linked against `vitaGL vitashark
  SceShaccCgExt taihen_stub mathneon` plus the `Sce*_stub` import libraries
  the engine's SDL2/GL/audio/input/save paths touch, **and `stdc++`**
  (`libvitaGL.a`'s runtime GLSL preprocessor is C++ — exceptions,
  `std::string`, `operator new` — and this project links with the plain C
  driver, which doesn't pull in libstdc++ the way `g++` would).
- **`platform/main_pc.c`**: fixed ROM path, crash-handler backtrace
  (`execinfo.h` isn't available), default window size (960×544).
- **`platform/platform_sdl_min.c`**: vitaGL owns display/context creation
  directly (`vglInitExtended`/`vglSwapBuffers`), bypassing SDL's video/GL
  subsystem entirely on Vita — `s_window`/`g_sdlWindow` stay `NULL`, and
  every other use site in this ~6500-line file was already guarded for that
  case. The dead-on-Vita GL-fence backpressure path and a `SDL_SysWM`-only
  WebGPU surface-introspection function are excluded (see table above).
- **`platform/fast3d/gfx_opengl.c`**: this file is vendored/shared with a
  sibling GoldenEye 007 Vita-adjacent project ("mgb64"), which is why it
  already had an `MGB64_PORTMASTER_GLES` GLES-handheld code path — Vita
  gets its own branch (`__vita__`) instead of reusing that one, since
  vitaGL's actual capability surface doesn't match GLES3 exactly. This file
  had the bulk of the GL-capability-gap fixes (see table above).
- **`platform/fs_utf8.c`**, **`platform/stubs_dkr.c`**: `execvp`/`flock`/
  `posix_memalign` are either meaningless (no process model, single-process
  file access) or declared-but-unimplemented in VitaSDK's newlib
  (`posix_memalign` → use `memalign` instead, same arguments) — see table
  above.
- **`platform/online/compatibility_identity.c`**: added the `"vita"`
  platform tag to satisfy the file's compile-time OS-tag fence.

## Known issues

### Remastered visual preset crashes on startup (unresolved)

With the Remastered preset active (`g_pcRemasterFX=1`), the very first
shader compiled every session reliably fails `glCompileShader` with
`GL_COMPILE_STATUS=0` and an empty info log (`GL_INFO_LOG_LENGTH=0`) —
no diagnostic text at all. The failing shader is the simplest possible
combiner (a flat vertex-color pass-through, no texture/lighting/shadow).
A byte-for-byte comparison against a successful Restored-preset boot log
proved the shader source, buffer lengths, and surrounding boot-time memory
state are identical between the failing and succeeding runs — the only
difference is the raw value of `g_pcRemasterFX`. **Workaround: use the
Restored (default) preset. Do not enable Remastered.**

Ruled out so far, each with direct on-device evidence, so this isn't
re-investigated from scratch next time:

- The fragment-shader GLSL version-header bug above (real bug, fixed,
  but unrelated — the version header is correct in the failing run too).
- World Shadows / `SHADER_OPT_SUN_SHADOW` (force-disabled on Vita,
  confirmed inert via boot log before this shader is ever reached).
- RL-5 / `SHADER_OPT_DFDX_LIGHT` (the failing shader is provably the
  first one compiled all session, so nothing could have poisoned it).
- A pending/stale GL error carried into the compile call (drained and
  logged at function entry; comes back clean).
- The shader compiler not being "warmed up" yet (a 5x retry with a delay
  between attempts failed identically every time).
- Buffer/length corruption handed to `glShaderSource` (logged `strlen()`
  vs. the tracked length and the raw tail bytes; both clean and correctly
  terminated in both the failing and succeeding runs).
- Thread affinity between shader setup and the compile call (this
  codebase is single-threaded end to end on Vita — confirmed by tracing
  every thread-creation call to a no-op stub).
- Deleting and recreating the shader object on retry, in case the first
  failed compile left the object internally poisoned in vitaGL's own
  bookkeeping — this did not fix the compile failure, and on at least one
  run produced a separate, harder crash (a Data Abort deep inside SceGxm
  on a background rendering thread), so this retry-with-fresh-objects
  approach has been removed again rather than kept as a partial mitigation.

Root cause is still unknown. The next concrete step is probably to compare
what `g_pcRemasterFX` actually changes upstream of this shader (uniform
layout, a `#define` that changes generated shader text length/content in a
way not caught by the current comparison, etc.) rather than further
retry/defensive-coding attempts at the compile call itself.

### Verbose shader-compile diagnostics are off by default

The logging added while chasing the issue above is gated behind a marker
file: drop an empty file named exactly `debug` at
`ux0:data/goldenballoon/debug` before launching to re-enable the verbose
per-shader boot-log lines (checked once at first use, so it must be in
place before, not during, a session). Failure-path diagnostics (the actual
compile/link error dump right before a crash) always log regardless of
this file.

## What needs real-hardware verification

Nothing below could be checked without a device, and none of it was
guessed at without a documented reason to believe it's a reasonable
starting point — but all of it is unverified:

- **`vglInitExtended` parameters** (ring buffer / mempool sizes) — currently
  passed conservative-but-arbitrary values; may need tuning if the game
  runs out of GPU memory or the ring buffer stalls under DKR's heavier
  particle/HUD draws.
- **Input mapping** — SDL2's GameController API should map the Vita's
  physical buttons/sticks automatically via vita-sdl2's built-in mapping,
  but this has not been exercised at all; button layout (especially
  L/R vs. L1/R1/L2/R2 conventions and the D-pad) needs a hands-on pass.
- **Audio path** — untested; SDL2's audio backend on Vita goes through
  `sceAudio`, and this engine's mixer/sequence-player pipeline has not been
  run against it.
- **Performance** — DKR's HUD, minimap, and particle-heavy track sections
  on the PowerVR SGX543MP4+ are an open question; `Video.RenderScale` is
  the only scaling knob available (MSAA is off — see table above).
- **Mip-level-clamp removal** — skipping `GL_TEXTURE_BASE_LEVEL`/
  `MAX_LEVEL` entirely (rather than emulating it) could theoretically
  produce visual artifacts on textures where the frontend re-uses a texture
  ID across mip-chain-length changes; the existing `gfx_gl_set_has_mips`
  bookkeeping should prevent this in practice, but it hasn't been eyeballed
  in-game.
- **`textureSize()`/`texelFetch()` in the texture clamp/tile-mask,
  SSAO, and framebuffer-diagnostic code paths** — unlike plain
  `texture()`/`textureLod()` (fixed by renaming to `texture2D()`, see the
  git log), these ES3 functions have no equivalent at all in the legacy
  GLSL ES 1.00 dialect vitaGL's runtime translator understands, so a
  shader that reaches one of these paths on Vita will need an actual
  logic rewrite (e.g. passing texture size as a uniform), not just a
  syntax translation. Not yet hit by any shader reached so far; flagged
  here so the next occurrence is recognized immediately instead of
  requiring a fresh round of coredump archaeology.
- **Coverage-stencil / framebuffer-snapshot degradations** — both are
  opt-in diagnostics off by default; if a future contributor enables them
  on Vita, verify the degraded (depth-only / no-read-buffer-select)
  behavior actually looks acceptable rather than just "doesn't crash."

## Next steps

1. Root-cause the Remastered-preset shader-compile crash (see
   [Known issues](#known-issues)) — the biggest remaining blocker to
   calling this port stable.
2. Add a native "Controls" entry to the Options menu for on-device button
   remapping (currently DualShock-layout-only, no remapping UI).
3. Work the "needs hardware verification" list above.
4. Replace the placeholder LiveArea art in `vita/livearea/` with real art.
5. If a native ROM-picker/launcher UI is wanted eventually (rather than the
   fixed-path `--rom`/`DEFAULT_ROM` convention used for this first cut), it
   would need to be built from scratch against `vita2d`/`SceCommonDialog`
   rather than reusing `MDKR_APP`'s ImGui launcher, which is desktop-only.
