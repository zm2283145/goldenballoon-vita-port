# goldenballoon (mdkr64) — PS Vita port status

This branch (`vita-port`) adds an initial PS Vita target to goldenballoon's
CMake build, cross-compiled with VitaSDK against **vitaGL** (OpenGL-over-
sceGxm) and **vitashark** (runtime GLSL→GXP shader compiler), following the
same libultraship/vitaGL pattern as
[Rinnegatamante/Lighthouse](https://github.com/Rinnegatamante/Lighthouse)
(a Banjo-Kazooie Vita port used as the concrete reference for library
choices, link flags, and the VPK packaging recipe).

**Status: builds and links clean (`arm-vita-eabi-gcc` → `mdkr64` ELF →
`mdkr64.vpk`). Not yet booted on real hardware.** Everything below is
compiler-error-driven engineering, not guesswork — but a first boot on a
real Vita will surface a second round of issues (input mapping, audio,
performance, GPU-side rendering correctness) that no amount of further
desk-checking will find. See "What needs hardware verification" at the end.

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

```powershell
arm-vita-eabi-strip -g build-vita/mdkr64.elf
vita-elf-create build-vita/mdkr64.elf build-vita/mdkr64.velf
vita-make-fself -c -s build-vita/mdkr64.velf build-vita/eboot.bin
vita-mksfoex -s TITLE_ID=GBLN00001 -d ATTRIBUTE2=12 "GoldenBalloon DKR" build-vita/param.sfo
vita-pack-vpk -s build-vita/param.sfo -b build-vita/eboot.bin build-vita/mdkr64.vpk `
    -a vita/livearea/icon0.png=sce_sys/icon0.png `
    -a vita/livearea/bg.png=sce_sys/livearea/contents/bg.png `
    -a vita/livearea/startup.png=sce_sys/livearea/contents/startup.png `
    -a vita/livearea/template.xml=sce_sys/livearea/contents/template.xml
```

`vita/livearea/*.png` are **programmatically generated placeholders**
(solid background + wordmark text), not final art — replace them before any
public release. `vita/livearea/gen_livearea_assets.py` (not committed;
available on request) regenerates them if needed.

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
- **Coverage-stencil / framebuffer-snapshot degradations** — both are
  opt-in diagnostics off by default; if a future contributor enables them
  on Vita, verify the degraded (depth-only / no-read-buffer-select)
  behavior actually looks acceptable rather than just "doesn't crash."

## Next steps

1. Boot `mdkr64.vpk` on real hardware with a ROM at
   `ux0:data/goldenballoon/baserom.us.v80.z64` and see what happens.
2. Work the "needs hardware verification" list above in whatever order the
   first boot's actual symptoms suggest.
3. Replace the placeholder LiveArea art in `vita/livearea/` with real art.
4. If a native ROM-picker/launcher UI is wanted eventually (rather than the
   fixed-path `--rom`/`DEFAULT_ROM` convention used for this first cut), it
   would need to be built from scratch against `vita2d`/`SceCommonDialog`
   rather than reusing `MDKR_APP`'s ImGui launcher, which is desktop-only.
