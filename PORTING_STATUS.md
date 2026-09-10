# goldenballoon (mdkr64) — PS Vita port status

This branch (`vita-port`) adds an initial PS Vita target to goldenballoon's
CMake build, cross-compiled with VitaSDK against **vitaGL** (OpenGL-over-
sceGxm) and **vitashark** (runtime GLSL→GXP shader compiler), following the
same libultraship/vitaGL pattern as
[Rinnegatamante/Lighthouse](https://github.com/Rinnegatamante/Lighthouse)
(a Banjo-Kazooie Vita port used as the concrete reference for library
choices, link flags, and the VPK packaging recipe).

**Status: 1.6.9 — complete Adventure playthrough confirmed on real hardware.** It
boots, loads a ROM, saves progress, and plays through the full game on the default
(Restored) visual preset, with audio, input, textured rendering, correctly
rendered 3D race/menu scenes, a 98-trophy pack, and a magic-code-gated Save
Editor plus persistent Vita control remapping. This moved past "builds
and links clean" through hands-on, on-device bring-up: real crashes and
rendering bugs, pulled via a boot-time file logger, coredumps, and targeted
diagnostic logging, root-caused one at a time. **The Remastered visual preset
currently crashes on startup and must not be used — see
[Known issues](#known-issues) below.** Fixed so far, in the order they were
hit:

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
4. **3D scene never rendering past the main menu, plus corrupted intro
   logo copyright text.** Root cause: `dkr_k0_to_physical()` (what
   `OS_K0_TO_PHYSICAL()` calls on native ports) registers a converted
   pointer into the renderer's lookup table so `dkr_resolve()` can find it
   again later — but only on 64-bit (LP64) targets; the 32-bit (ILP32)
   branch was a no-op. That only mattered for call sites that convert
   their pointer *before* handing it to the display-list macros (chiefly
   `gSPViewport(pkt, OS_K0_TO_PHYSICAL(&gViewportStack[...]))` in
   `camera.c`); by the time anything downstream could register it, the
   real pointer was already gone. Unregistered, the viewport address fell
   back to the segment-table heuristic, where it collided by coincidence
   with an unrelated small transient pool (Vita processes load around
   `0x81000000+`, and the flipped/converted address happened to decode
   with a top nibble of 1 — "segment 1"). That handed back a
   "successfully resolved" pointer into unowned, zeroed memory: an
   all-zero `Vp_t` (viewport scale/translate both zero), which collapses
   all 3D geometry, and the same misresolution corrupted the logo text.
   Fixed in `platform/stubs_dkr.c` by registering the original pointer on
   ILP32 too, gated to the real Vita host-pointer range so it can't be
   confused with the already-truncated 32-bit tokens (e.g.
   `TextureHeader.cmd`) that also pass through the same conversion
   function. **Note:** an earlier attempt at this exact fix
   (`4c0fee6`, "gViewportStack pointers colliding with the segment-token
   window") took a different approach — loosening the segment-token
   exclusion check that a *different*, previously-fixed bug relies on —
   and that broke something else, causing an unrooted crash a few frames
   in; it was reverted (`f423e4f`) before this fix replaced it. If a
   similar-looking bug resurfaces, that revert's commit message and the
   diagnostic-logging commits around it (`4614e570`, `9df8fb6c`) are the
   place to start, not another loosen-the-exclusion attempt.

Confirmed on real hardware: the boot log shows zero viewport-resolution
failures, shaders compile and textures upload normally, and a complete
Adventure playthrough reaches the ending with working audio, controls, saves,
trophies, menus, bosses, and every world on the default Restored preset. See
the git log on this branch for the bring-up history.

5. **Video settings failed to persist across restarts.** The Vita save
   path for video/preset settings was being written somewhere that didn't
   survive a relaunch; fixed so settings (including which visual preset is
   active) now save and load correctly.
6. **Stack-overflow crash during normal play.** A code path allocated a
   large buffer on the stack where the desktop build's much larger default
   stack absorbed it silently; fixed by moving it off the stack for the
   Vita target.
7. **Crash from World Shadows / `SHADER_OPT_SUN_SHADOW`.** This
   Remastered-only effect depends on GL features vitaGL doesn't implement
   (see the disabled-features table below) and wasn't fully excluded on
   Vita; it's now force-disabled with an explicit `__vita__` check in
   `gfx_pc_dkr.c`, confirmed inert via boot-log instrumentation.
8. **Wrong GLSL version header on every fragment shader.** The vertex
   shader's version-header selection correctly checked `__vita__` first,
   but the fragment shader's only checked `MGB64_PORTMASTER_GLES` (also
   defined for Vita), so every fragment shader got a `#version 320 es`
   header on a body that had already been rewritten to legacy GLSL ES
   1.00. Fixed by adding the same `__vita__` branch to the fragment header
   selection. Independently confirmed correct, but did **not** fix the
   Remastered-preset crash below — that turned out to be a separate,
   still-open issue.
9. **Bundling LiveArea assets (icon0/bg/startup/`template.xml`) broke a
   fresh VitaShell install every time**, with a generic `0x80104004` error
   — confirmed via a hardware binary search to have nothing to do with the
   image files themselves (every combination of icon0/bg/startup, with
   plain-RGB and palette-indexed PNGs, was tested and ruled out) and
   nothing to do with `template.xml`'s `content-id` value or its XML
   content in general — even a maximally trivial `template.xml` still
   broke the install. Root cause: `template.xml` used the newer
   "frame-based" LiveArea schema (`<frame>`/`<pos>`/`<bg><img>`, with a
   `content-id` attribute), which this project's target hardware/VitaShell
   apparently can't register during a fresh install, even though it's
   valid enough to not be rejected outright (the app still installs, just
   without the background). Found by comparing against a sibling project's
   (Render96Ex-Vita) working `template.xml`, which uses the older, simpler
   schema instead (`<livearea-background><image>...</image></livearea-background>`,
   `<gate><startup-image>...</startup-image></gate>`, `format-ver`/
   `content-rev` attributes instead of `content-id`). Switching to that
   schema fixed it — confirmed on real hardware to install cleanly from
   fresh and display both the background and startup/gate art correctly.
   LiveArea assets are bundled by default now (see Packaging below).

Currently: the Restored preset is stable for normal play; the
Remastered preset crashes on startup every time (see
[Known issues](#known-issues)). See the git log on this branch for the
blow-by-blow of everything ruled out chasing it.

**vitaGL / vitaShaRK versions:** built from
[Rinnegatamante/vitaGL](https://github.com/Rinnegatamante/vitaGL) commit
`cd3791e` and [Rinnegatamante/vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK)
commit `df24065` (both HEAD as of 2026-08-30/2026-08-22 respectively),
built from source rather than dpm's prebuilt packages (no make on this
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

Needs VitaSDK with these packages installed via `vdpm` first: `SDL2`,
`vitaGL`, `vitashark`, `libmathneon`, `taihen` (this project's own
environment builds vitaGL/vitaShaRK from source instead — see the versions
note above — but `vdpm`'s prebuilt packages work too if source builds
aren't needed).

```powershell
# One-time setup (VitaSDK, cmake, ninja already installed under
# C:\vitasdk and C:\vitasdk-tools in this environment):
cmake -S . -B build-vita -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE=$env:VITASDK/share/vita.toolchain.cmake `
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita --target mdkr64
```

Rebuilding an existing `build-vita` directory after pulling changes is just
`ninja -C build-vita` (or `cmake --build build-vita --target mdkr64` again)
— re-running the `cmake -S ...` configure step is only needed after a
`CMakeLists.txt` change or a fresh checkout. Either way, **packaging is a
separate step** (see below) — neither of these produces `mdkr64.vpk`.

`CMakeLists.txt` detects `VITA` (set by `vita.toolchain.cmake`) and forces
`MDKR_WEBGPU_BACKEND`, `MDKR_APP`, and `MDKR_NATIVE_PHONE_PARTY` off before
any of their downstream `include()`s run (see the "Vita platform" block
near the top of the WebGPU option handling) — no manual flags needed beyond
the toolchain file itself.

## Packaging (VPK)

Packaging is scripted -- run `tools/package_vita.ps1` after every rebuild
rather than reproducing
the individual strip/elf-create/fself/pack-vpk commands by hand. Two things
that script gets right that a naive hand-rolled version will not:

- `vita-make-fself -c -pm 0x2000000 ...` — the `-pm 0x2000000` (32MB
  physically-contiguous memory budget) flag is required. Without it,
  vitaGL's `vglInitExtended` silently fails (returns `GL_FALSE`) even
  though free-memory-pool numbers look healthy afterward — the game boots
  and audio/input/game logic all run fine, but nothing ever renders.
  Diagnosed via `mdkr_vita_boot_log` instrumentation around
  `vglInitExtended`.
- LiveArea assets (`icon0.png`/`bg.png`/`startup.png`/`template.xml`) are
  bundled into the VPK by default (pass `-NoIcons` to opt out and ship a
  bare eboot + param.sfo VPK instead). This used to break a fresh
  VitaShell install every time with a generic `0x80104004` error —
  root-caused (see "Fixed so far" item 9 above) to `template.xml`'s XML
  schema, not the image assets, and fixed by switching to the
  `<livearea-background>`/`<gate><startup-image>` schema. Confirmed on
  real hardware: fresh installs now succeed and both the background and
  gate art display correctly.

```powershell
# Regenerates param.sfo from CMakeLists.txt's MDKR_VERSION, then packages.
pwsh -File tools/package_vita.ps1 -BuildDir build-vita   # bundles LiveArea assets by default; add -NoIcons to omit them
```

`vita/livearea/*.png` are original "Golden Balloon" themed art (a hot-air
balloon, checkered-finish-line motif, and an original character/logo
treatment — no Nintendo/Rare IP), committed alongside `template.xml`.

**ROM placement:** the engine looks for the ROM at a fixed path on Vita —
`ux0:data/goldenballoon/baserom.us.v80.z64` — since there is no in-app
ROM-picker UI on this platform yet (see "What's disabled" below). Copy a
legally-obtained US v1.0 (v80) Diddy Kong Racing ROM there before first
launch.

## Adding trophies to a PS Vita project

This port is a working reference for unsigned homebrew trophies on a Vita.
The console's trophy service expects a title-specific archive and normally
verifies Sony's signature; homebrew cannot produce that signature. Install
and enable [NoTrpDrm](https://github.com/Rinnegatamante/NoTrpDrm) beneath
`*main` in the active taiHEN `config.txt` on the target Vita to permit the
archive. Copy `NoTrpDrm.suprx` to the matching `ur0:tai/` or `ux0:tai/`
directory, add that exact path below `*main`, and reboot. Treat the plugin as
an optional runtime dependency: initialize trophies defensively and keep the
game fully playable if the module, plugin, archive, or service is absent.

1. **Choose a stable communication ID and title ID.** This project uses Vita
   title ID `GBLN00001` and trophy communication ID `GBLN00001_00`. The
   title ID must be used in `param.sfo` and the VPK path
   `sce_sys/trophy/GBLN00001_00/TROPHY.TRP`; the communication ID goes in
   the trophy XML. Do not change either after release, or the Vita sees a
   different trophy set.
2. **Define the trophy data and groups.**
   [`tools/build_vita_trophy_pack.py`](tools/build_vita_trophy_pack.py)
   is the source of truth here. `MAIN_TROPHIES` contains the base set and
   platinum; its entries use group `0`. `ADVENTURE_TWO` uses `gid="001"` and
   `TIME_TRIALS` uses `gid="002"`. The groups are declared in both the
   compact `TROPCONF.SFM` manifest and the localized `TROP.SFM` metadata.
   Give optional/DLC-style challenges a non-zero group ID and set their
   parent to `-1`; only trophies in group 0 should be parents/children of
   the platinum. Supply a title image, one 320×176 image per group, and a
   240×240 image per trophy.
3. **Build a valid TRP.** Vita reads `TROPCONF.SFM` before the localized
   `TROP.SFM`; omitting the configuration manifest can cause `NP-6182-7`.
   The archive also needs the expected NoTrpDrm development signature
   placeholder and conventional file ordering. Reuse or adapt the packer
   rather than zipping files by hand.
4. **Bundle the archive in the VPK.** The packaging script creates
   `build-vita/TROPHY.TRP` and adds it at
   `sce_sys/trophy/GBLN00001_00/TROPHY.TRP`. Keep `param.sfo` current on
   every build. This script derives Vita's `APP_VER` from `MDKR_VERSION` in
   `CMakeLists.txt` (semantic `1.6.9` becomes Vita `01.69`), keeping the
   compiled version, VPK metadata, and release version aligned.
5. **Register at a safe early point.** Load `SCE_SYSMODULE_NP_TROPHY`, call
   `sceNpTrophyInit`, create the context, run the setup dialog to completion,
   then create a handle. This port does it from `platform/vita_trophy.c`
   during startup after vitaGL is available, before gameplay, and swaps
   frames while the dialog runs. The first successful setup makes the pack
   appear in the Trophy app; a trophy does not need to unlock first.
6. **Wire triggers to persistent game state.** Call a small trophy bridge
   whenever a reliable event happens (for example a balloon collection),
   and poll saved progress after loading or on a regular safe update path
   for achievements that may already be satisfied. Always let
   `sceNpTrophyUnlockTrophy` remain the persistence authority; keep only an
   in-session retry guard to avoid submitting the same ID every frame.
7. **Ship changes safely.** Once a set has been installed, a new group,
   trophy, title, or image requires raising `<trophyset-version>` (this port
   currently uses `01.04`) so the Vita imports the update. Test from a clean
   install or remove the title's local trophy entry between compatibility
tests. Never renumber shipped trophies: add new IDs instead.

The included 98-trophy pack uses distinct unlocked Achievement artwork from
[RetroAchievements](https://retroachievements.org/) by permission, plus
custom Golden Balloon artwork for the platinum and the port-exclusive Taj,
Wizpig, and Terry challenges. Use only artwork you are licensed or otherwise
authorized to distribute; trophy art is packaged inside the TRP and must be
included when testing an updated trophy set.

## Save Editor (Vita)

`GOLDENEDIT`, entered through the Magic Codes screen, persistently unlocks
the in-game Save Editor. It deliberately works through real save structures
and trophy conditions instead of calling the trophy API directly: changing a
condition makes the normal runtime trophy check see the condition. Its pages
cover track and world state, bosses and advancement, key arenas, Main /
Adventure 2 / Time Trial / Character / Power-Up trophy conditions, Taj races,
hub balloons, time-trial records, unlocks, and save-slot creation, rename,
and erase. Progression edits normalize prerequisite flags and balloon totals;
the four key-arena completion flags are persisted per slot using unused
cutscene-flag bits so completion survives a reload.

For troubleshooting, create `ux0:data/goldenballoon/debug` before launch;
the existing boot log then records trophy-module, context, setup, handle,
and unlock results in `ux0:data/goldenballoon/mdkr_boot.log`.

## Controls (Vita)

**Options → Controls** opens a VitaGL ImGui mapping screen with vector icons
for the Vita face buttons, shoulders, Start, D-pad, and both stick directions.
Every digital game action can store up to two inputs. Selecting an action opens
a five-second capture window; the final two distinct inputs are saved when it
expires, while receiving no input cancels the change. Either binding can be
cleared separately and the complete shipped mapping can be restored in one
step. Bindings persist in the normal settings file. D-pad navigation is
supported, and input consumed by this overlay is blocked from simultaneously
operating the original menu behind it.

## What's disabled or stubbed on Vita, and why

| Feature | Status | Reason |
|---|---|---|
| WebGPU renderer (`MDKR_WEBGPU_BACKEND`) | Off | No WebGPU driver exists for the Vita's PowerVR SGX543MP4+; vitaGL (GL-over-sceGxm) is the only viable graphics path, same choice Lighthouse made. |
| Desktop ImGui launcher (`MDKR_APP`) | Off | Pulls in desktop file dialogs, DPI/multi-monitor queries, and other desktop-only surface. The plain CLI entry point (`platform/main_pc.c`) is used instead, with the ROM at a fixed path (see above). The Vita-native Save Editor and Controls overlays use a small, independently integrated ImGui renderer and remain available. |
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

## Remaining work

A complete Adventure playthrough on real Vita hardware has now validated
installation and boot, ROM loading, menus and races, all five worlds and their
bosses, controls, audio, save persistence, trophy registration and unlocking,
the Save Editor, and the Controls overlay. The former bring-up verification
checklist was removed because it no longer described the state of the port.

1. Root-cause the optional Remastered-preset shader-compile crash (see
   [Known issues](#known-issues)). The supported Restored preset is stable.
2. Additional multiplayer, third-party controller, and long-session reports
   remain useful for broad hardware coverage, but are not release blockers.
3. If a native ROM-picker/launcher UI is wanted eventually (rather than the
   fixed-path `--rom`/`DEFAULT_ROM` convention used for this first cut), it
   would need to be built from scratch against `vita2d`/`SceCommonDialog`
   rather than reusing `MDKR_APP`'s ImGui launcher, which is desktop-only.
