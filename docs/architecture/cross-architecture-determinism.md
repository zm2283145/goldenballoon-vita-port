# Cross-architecture determinism

## The gap this closes

`platform/online/compatibility_identity.h` derives the online compatibility
identity from four things: version, source commit, source-dirty, and ROM
revision. **CPU architecture is not one of them.** Matchmaking will therefore
seat an ARM64 macOS player against an x86_64 Windows player in the same
lockstep session, and it has always been willing to.

Every determinism instrument this project owns holds the architecture fixed:

| Gate | What it varies | What it holds fixed |
| --- | --- | --- |
| `tests/check_determinism.py` | run number, renderer backend | binary, architecture, host |
| `tests/check_state_hash.py` | run number, window size, renderer, level | binary, architecture, host |
| rollback convergence gates | rollback depth, input timing | process, architecture, host |

So the project has never measured a second architecture, and a
simulation that is not bit-identical across architectures is a live desync in
shipped online play that nothing here would report.

The measurement is available on a single Apple Silicon workstation: build a
thin x86_64 executable beside the native thin arm64 one and drive both through
the same ROM and input script, comparing the authoritative `[SIMHASH]` stream
tick by tick.

## What the gate is

`tests/check_crossarch_determinism.py`. It reuses the existing instrument
rather than inventing a second one: the same `MDKR_STATE_HASH=3` field set that
`check_state_hash.py` anchors, the same `nav_to_time_trial_race.txt` route, the
same 3600-tick budget, read through `MDKR_STATE_HASH_FILE` (`platform/sim_hash.c`)
so a killed run still leaves a line-aligned prefix showing how far the two arms
agreed.

It reports the **first divergent tick**, both rows, and whether the
authoritative population (`objs=`) or only the state digest moved — those two
have disjoint suspect lists, and a boolean verdict would leave the next person
bisecting from nothing.

### Default routes

Levels 5, 37 and 11, matching `check_state_hash.py`'s process-nondeterminism
arms. Those three are where a binary was measured disagreeing with *itself*
because authoritative fields were seeded from memory the engine never wrote (an
unwritten `spawnAngle[]` slot, recycled particle-pool bytes reaching
`trans.rotation`, `localPos` and `velocity`). Changing the architecture moves
stack frames, structure sizes, allocation sizes and pool recycling order —
exactly the inputs those defects read. If a class of them survives, these
routes are where it surfaces first.

### The vacuity guards

A cross-architecture gate has an unusually easy way to pass while proving
nothing: compare two binaries of the same architecture. Each way that can
happen is a hard failure, not a skip:

1. **Both binaries are read with `lipo -archs`.** They must be thin, must
   disagree, and must cover arm64 and x86_64 between them. A *universal* binary
   is refused outright: the loader runs its native slice, so an "x86_64" arm
   would silently execute as arm64.
2. **The two `CMakeCache.txt` files must agree** on build type, compiler flags,
   IPO, and every `MDKR_*` project option. A Release arm64 against a Debug
   x86_64 measures optimisation level, not architecture.
3. **Neither binary may be older than the newest tracked source file.** A stale
   x86_64 executable is the most likely way to get a spectacular "architecture
   divergence" that is really "you edited the simulation and rebuilt one tree".
4. **Every captured stream must be complete** — non-empty, the expected row
   count, ticks contiguous from 0. Two empty streams are equal.
5. **A positive control must diverge.** One extra arm64 arm runs with
   `MDKR_RNGSEED=legacy` and must produce a different stream. If it does not,
   the capture or the comparison is broken and every "identical" verdict in the
   same run is unsupported.

`tests/test_crossarch_determinism.py` is the ROM-free unit cover for all of the
above; it runs under CTest and needs no build of either architecture.

## What the gate does NOT prove

Read this before quoting a green run at anybody.

- **It is not an OS gate.** Both binaries run on macOS, against the same kernel,
  the same dyld and the same libSystem images (the x86_64 slices of them). It
  says nothing about Windows-vs-macOS, UCRT-vs-libSystem, or the wasm lane. A
  Windows x86_64 peer can still desync against a macOS x86_64 peer for reasons
  invisible here.
- **It is not a compiler gate.** Both binaries come from the same AppleClang at
  the same version. The Windows lane is MinGW GCC (`.github/workflows/windows-validate.yml`)
  and the browser lane is emcc; a divergence in either toolchain's codegen is
  out of scope.
- **It is not a proof about untravelled code.** It covers the routes it drives.
  Cross-architecture divergence has every reason to be level-shaped, for the
  same reason the process-nondeterminism defects were.
- **It is not a claim about Rosetta 2 fidelity.** Rosetta translates x86_64 to
  ARM instructions. A divergence found here is a divergence between *this arm64
  binary* and *this x86_64 binary as Rosetta executes it*. That is what a
  Rosetta player actually runs, so a finding is real; it is not automatically
  what an Intel Mac or a Windows PC would compute. Confirm a positive finding on
  real x86_64 hardware before treating it as the whole story.
- **A green run is evidence, not a theorem.** It says these routes on these two
  builds agreed for 3600 ticks each. It does not prove the simulation contains
  no architecture-dependent expression.

## Producing the x86_64 build on an Apple Silicon host

```
macos/Scripts/build_crossarch_x86_64.sh
```

That script does the whole thing. What it is working around, in case it fails:

### Obstacle 1 — SDL2 has no x86_64 slice on this machine

`CMakeLists.txt:114-122` finds SDL2 through pkg-config only:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
```

On an Apple Silicon host, Homebrew installs an **arm64-only** dylib into
`/opt/homebrew`, and a stock machine has no x86_64 Homebrew prefix under
`/usr/local` at all. `pkg-config` resolves `sdl2` to the arm64 dylib no matter
what `CMAKE_OSX_ARCHITECTURES` says, the whole tree compiles for x86_64, and the
final link dies with:

```
building for macOS-x86_64 but attempting to link with file built for arm64
```

The fix is the script this project already has:

```
macos/Scripts/build_release_sdl2.sh --arch x86_64 \
    --deployment-target 13.0 --work-dir build-macos-deps/sdl2-x86_64
export PKG_CONFIG_PATH="$PWD/build-macos-deps/sdl2-x86_64/install/lib/pkgconfig:$PKG_CONFIG_PATH"
```

It downloads the pinned upstream SDL2 source, verifies its SHA-256, builds a
standalone x86_64 dylib, and installs an `sdl2.pc` beside it. It also refuses to
hand back an `sdl2-compat` (SDL3 shim) build, which would be a different library
wearing SDL2's name.

`build_crossarch_x86_64.sh` runs this for you and then re-reads
`pkg-config --variable=libdir sdl2` and `lipo -archs` on the resolved dylib, so a
`PKG_CONFIG_PATH` that still points at Homebrew fails immediately instead of
after two hundred compiled objects.

### Obstacle 2 — wgpu-native is fetched for the wrong architecture

`cmake/webgpu.cmake:59` selects the prebuilt wgpu-native archive with
`mdkr_select_wgpu_artifact(... "${CMAKE_SYSTEM_PROCESSOR}" ...)`.

On macOS, `CMAKE_SYSTEM_PROCESSOR` is derived from the host's `uname -m` at
configure time and is **not** re-derived from `CMAKE_OSX_ARCHITECTURES`. So a
configure that sets only `CMAKE_OSX_ARCHITECTURES=x86_64` on an arm64 host
fetches `wgpu-macos-aarch64-release.zip`, compiles everything else for x86_64,
and fails the link inside `libwgpu_native.a`. This is the same asymmetry
`macos/Scripts/build_universal.sh` warns about in its own header.

A Darwin/x86_64 wgpu-native archive **is** pinned and hash-verified upstream, so
the fix is one extra cache variable — and this project's existing scripts do not
set it:

```
-DCMAKE_SYSTEM_PROCESSOR=x86_64
```

### The full configure

```
PKG_CONFIG_PATH="$PWD/build-macos-deps/sdl2-x86_64/install/lib/pkgconfig:$PKG_CONFIG_PATH" \
cmake -S . -B build-rel-x86_64 \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel-x86_64 --target mdkr64 --parallel "$(sysctl -n hw.ncpu)"
lipo -archs build-rel-x86_64/mdkr64      # must print exactly: x86_64
```

The arm64 side is the ordinary `rel` preset (`build-rel/`), which must be
configured with the **same** `CMAKE_BUILD_TYPE` and the same `MDKR_*` options.

### Things that are NOT obstacles

- **The toolchain.** Apple's clang ships a full x86_64 backend and linker on
  Apple Silicon; no extra install is needed.
- **Rosetta 2.** Present on most machines; `softwareupdate --install-rosetta`
  otherwise. The build script checks for it before spending the build.
- **System frameworks.** OpenGL, AVFoundation, CoreFoundation and friends are
  universal.
- **Source-form dependencies.** Basis Universal, meshoptimizer, HarfBuzz,
  SheenBidi, Mbed TLS, libdatachannel and everything vendored in `lib/` are
  fetched or checked in as source and recompile for x86_64 automatically. There
  are no architecture-locked binaries checked into the repository.
- **A universal (fat) binary.** Do not try. `build_universal.sh` documents why it
  cannot work here, and the gate refuses fat binaries anyway.

### What genuinely is blocked

Full release packaging for x86_64. `macos/Scripts/build_app_bundle.sh --release`
wants a frozen character importer and a glTF validator that its own help text
labels "Frozen arm64", built only by the arm64-only `macos-release.yml` job.
Irrelevant to this gate, which needs only `build-rel-x86_64/mdkr64`.

## Running it

```
tests/check_crossarch_determinism.py \
    --build build-rel \
    --build-x86 build-rel-x86_64 \
    --rom baserom.us.v80.z64
```

Seven headless runs (three routes x two architectures, plus a two-arm positive
control). The x86_64 arms execute under Rosetta 2 and are materially slower than
the native ones; `--timeout` defaults to 1800s per arm for that reason.

The gate is **operator-owned**, not a `tools/run_checks.py` task. It is
registered in `CROSSARCH_OPERATOR_SCRIPTS` there so `--list` and
`check_ci_contract` accept the tree, but the runner never invokes it: the runner
cannot manufacture a second-architecture build tree, and a `CHECKS` entry would
turn "no x86_64 tree on this host" into a red suite on every machine. Being
operator-owned is not permission to skip it — cross-architecture sessions are
admitted in shipped builds, so a release that has not run this gate has not
measured the thing it is shipping.

### Reading a failure

```
level 5: architecture divergence at tick 2731
    arm64  [SIMHASH] tick=2731 objs=126 h=9a3f...
    x86_64 [SIMHASH] tick=2731 objs=126 h=9a3f...
    the population agrees (126 objects) and only the state digest moved: ...
    2731 ticks were identical; 869 of the 869 remaining ticks differ
    bisect with: MDKR_STATE_HASH=3 MDKR_HASH_DUMP_TICK=2730 ...
```

- **Population differs (`objs=`)** — the two architectures disagree about which
  objects exist. Look at spawn/despawn conditions and allocation order before
  arithmetic.
- **Population agrees, digest differs** — a field value differs. Float
  arithmetic, an uninitialised read, or a pointer-derived value.

Then re-run both binaries with `MDKR_HASH_DUMP_TICK` / `MDKR_HASH_DUMP_UNTIL` /
`MDKR_HASH_DUMP_IDS=1` and diff the `[HASHOBJ]` and `[HASHOBJID]` rows to name
the object and the field. The gate prints the exact command.

## Architecture-sensitivity audit

What was searched, what was found, and how confident the finding is. Read this
as the list of places to look first when the gate goes red, and as the list of
places to keep defended while it is green.

### Already defended — do not regress these

**N64 trigonometry is table-driven, not libm.** This is the single biggest
reason to expect the gate to pass. `platform/math_util_native.c` resolves
`sins_s16`, `coss_s16`, `sins_2`, `sins_f` and `coss_f` through `gSineTable`
and `gArcTanTable`, and both tables default to **baked** constants
(`kMdkrBakedSineTable`, `kMdkrBakedArcTanTable`) rather than being generated at
load time. The libm arms exist but are opt-in diagnostics:

- `s_trigLibm` (`math_util_native.c:56,421`) is set only by an `MDKR_TRIG=l...`
  environment value;
- the runtime `sin()`/`atanf()` table generation (`math_util_native.c:437,466`)
  runs only under `MDKR_DEV_RUNTIME_TRIG=1` or `MDKR_ARCTAN=trunc`.

Under either of those the tables become a function of the host libm, and
**Apple ships different libm implementations for the arm64 and x86_64 slices**,
so those arms are architecture-dependent by construction. The gate scrubs
`MDKR*` from the environment before every run precisely so a maintainer's
exported diagnostic cannot select the libm arm on one side only. Keep the baked
default; a change that makes runtime generation the default would turn every
`atan2s()` call site — 72 of them, including AI steering and the camera — into a
cross-architecture divergence.

**Float contraction is pinned.** `CMakeLists.txt:846` sets `-ffp-contract=off`,
with a comment naming the exact hazard (AppleClang fuses `a*b+c` into a
single-rounding FMA on arm64 at `-O2`; x86_64 without `-mfma` cannot). This is
the correct flag and it is the one that matters most. See the scope caveat
below.

**MIPS conversion semantics are implemented explicitly** where they were known
to matter: `mdkr_mips_trunc_w_d`, `mdkr_mips_round_w_s`, `mdkr_mips_f64_to_s16`
and `mdkr_mips_f64_to_u16` (`game/src/runtime_contracts.c:250-307`) return
MIPS' signed-word indefinite value for NaN, infinity and out-of-range instead of
inheriting the host's conversion behaviour. That is exactly the right
mitigation; it is just not applied everywhere (below).

**No per-architecture code paths.** There is no `__aarch64__`, `__x86_64__`,
`__ARM_NEON` or `__SSE2__` conditional anywhere in `game/`, `platform/` or
`include/`, and no `-march`/`-mtune`/`-mcpu` in the build. Both architectures
compile the same source at the same ISA baseline their compiler default
implies.

**The authoritative hash reads named members, not raw structs.**
`SIM_HASH_FIELD` (`platform/sim_hash.c:250`) hashes `&obj->member` for
`sizeof(member)` bytes, so structure padding is never fed to the hash. Both
macOS ABIs are LP64 with the same fundamental type sizes and alignments, so
`sizeof` is stable across the two anyway.

### Live hazards, ranked

**1. Uninitialised STACK reads.** The highest-probability failure, and the one
this gate exists to catch. `check_state_hash.py`'s process-nondeterminism arms
document three already-fixed instances of authoritative state seeded from memory
the engine never wrote (level 41's unwritten `spawnAngle[]` slot; levels 37 and
11's recycled particle-pool bytes in `trans.rotation`, `localPos`,
`velocity`, `angularVelocity`).

The important asymmetry: an uninitialised **pool/heap** read tends to be stable
within one architecture *and* across the two, because the allocation sizes and
order are identical on both LP64 ABIs — that class already fails the existing
same-host determinism arms and has been fixed. An uninitialised **stack or
register-spill** read is different: it is often perfectly stable run to run on
one architecture, because the same codegen leaves the same garbage in the same
slot every time, and therefore **passes every gate this project has**. Register
allocation and frame layout are not the same on arm64 and x86_64, so that class
diverges here and nowhere else. Search targets: any `f32`/`s32` local read
before assignment, and any struct zeroed by a loop that skips a member (the
`spawnAngle[]` shape).

**2. `-ffp-contract=off` is scoped to one target.** It is set with
`target_compile_options(${MDKR_TARGET} PRIVATE ...)`, so it reaches the sources
compiled into `mdkr64` and **not** the sources compiled into `mdkr64_app`
(`CMakeLists.txt:1792`, the ImGui launcher/overlay static library holding
`platform/app/*.cpp`). Those are built with AppleClang's default contraction:
arm64 fuses, x86_64 does not. Most of that library is UI, but it is not all UI —
`platform/app/tool_freecam.cpp:302` records that detaching the free camera
diverged the v3 stream 232 ticks later, which is proof that app-shell code can
reach authoritative state. The gate's headless runs do not drive the app shell,
so **the gate cannot see this**; the shipped game can. Either move the flag to a
shared interface target or add it to `mdkr64_app` too.

The same gap is wider than one library: `cmake/tests.cmake` contains no
occurrence of `-ffp-contract=off` at all, across ~174 `add_executable(mdkr_*)`
test targets, several of which compile production simulation math directly —
`mdkr_camera_object_bvh_test`, `mdkr_camera_dynamic_temporal_test` and
`mdkr_camera_dynamic_precedence_test` link `game/src/hasm/math_util.c`
(`vec3f_rotate_py`, `atan2s`, the matrix builders), and `mdkr_void_pairs_test`
links `game/src/tracks.c`. Those are unit binaries rather than the shipped
engine, so they are not authoritative for netplay; they matter because a
*float-comparing* unit test can pass on arm64 and fail on x86_64 for a reason
that is purely flag coverage. If a cross-architecture CTest lane is ever added,
fix the flag first or the lane will report contraction, not defects.

**3. Raw `(s32)` casts of out-of-range or non-finite floats.** On x86_64
`cvttss2si` yields `INT_MIN` for anything out of range or NaN; on arm64
`fcvtzs` **saturates** to `INT_MAX`/`INT_MIN` and yields 0 for NaN. Any
authoritative float that can leave the signed-word range or go non-finite and is
then cast produces a different integer per architecture, silently. The
`mdkr_mips_*` helpers above are the correct fix and are applied at a handful of
sites; the decomp's many plain `(f32) -> (s32)` casts in physics, positions and
timers are not covered. This is a defect class, not a single defect: audit every
cast whose input is not provably in range, starting with racer velocity,
position and pitch conversions.

**4. `platform/adventure_party/adventure_party_spawn.c`.** Lines 56-57 and
96-97 call `sinf`/`cosf` directly to build spawn headings and positions
(`px = params->setup.x + sinf(angle) * radius`). Object positions are
authoritative and hashed. This is a real libm dependency on an authoritative
path, and Apple's arm64 and x86_64 libm are not the same implementation. It is
outside the gate's default routes (Adventure party mode is not the time-trial
route), so **add an Adventure-party route before trusting a green run to cover
it** — or convert those call sites to the `sins_f`/`coss_f` table path the rest
of the simulation uses.

**5. Model/animation math in the modern-character pipeline.**
`platform/modern_character_pose.c` (`acosf`, `sinf`, `atan2f`, `fmodf` at
lines 50-53, 79-83, 165-172, 557, 792, 1084, 1297) and
`platform/modern_character_runtime.c` (699-701, 759-761, 1747) are libm-heavy.
v3 hashes model animation state, so the question is whether any of this feeds a
hashed field or is purely presentation. Classify it explicitly — the answer is
not obvious from the call sites, and the custom-character pipeline is exactly
the kind of subsystem that grows a simulation dependency by accident.

**6. `long double`.** Present at `platform/fast3d/gfx_webgpu.c:1247-1250`
(GPU timestamp conversion) and in two app-shell statistics helpers
(`platform/app/character_test_evidence_store.cpp:389`,
`platform/app/ui_settings.cpp:13738`). `long double` is 80-bit x87 on x86_64 and
128-bit quad on arm64 — a guaranteed difference in rounding. None of these three
are on an authoritative path today, so this is a boundary to keep rather than a
bug to fix, but `long double` must never appear in `game/` or in the simulation
half of `platform/`.

**7. Environment escape hatches back to host libm.** Four variables turn the
baked-table mitigation off: `MDKR_TRIG=libm`, `MDKR_DEV_RUNTIME_TRIG=1`,
`MDKR_ARCTAN=trunc` and `MDKR_ROTPY=legacy` (the last restores a transposed
pitch/yaw formula, consumed at `game/src/hasm/math_util.c:750-761`). Each is a
deliberate A/B hook and each makes the simulation a function of the host libm,
or of a different formula. The gate scrubs every `MDKR*`/`GE007_*` variable
from the environment before each run, so it cannot be fooled by an exported
one — but a maintainer reproducing a divergence **by hand** must scrub them
too, or chase an environment difference as an architecture defect.

**8. `platform/rollback/rollback_snapshot.c` hashes raw byte blobs.**
`mdkr_rollback_snapshot_capture` (lines 173-201) `memcpy`s whole registered
memory ranges — object pool, particle pools, settings, transition workspace —
and `snapshot_checksum` (149-155) FNV-hashes the entire blob **including
structure padding and any field a constructor never wrote**. That is a
different mechanism from `sim_hash.c`, which reads named members only. The
file's own comment states it is an in-process corruption oracle, not a wire
format, so it is not a cross-architecture divergence source as used today. It
becomes one the moment a rollback checksum is compared between two processes or
sent to a peer — precisely the direction lockstep netplay grows in. Do not let
it become an equality oracle across builds.

**9. `particle_allocate` does not clear a recycled slot.**
`game/src/particles.c:1984-2076` sets only `kind` and `unk_48` when reusing a
pool entry; every other field the hash reads must be set independently by each
constructor (`create_general_particle`, `create_point_particle`,
`create_line_particle`). Two historical divergences came from exactly that
contract being broken, and both were fixed field-by-field rather than by
zeroing on recycle. The residual risk is structural: any field added to the v3
hash, or any new particle-kind constructor, can reintroduce the class silently.

One mitigating fact, because it narrows the search: `dkr_arena_init`
(`platform/stubs_dkr.c:326-373`) `memset`s the whole backing arena to zero on
first allocation, so a pool slot's *first* use reads architecture-independent
zero on both hosts. Only slots recycled mid-run between different field owners
can carry leftovers — and given a bit-identical run up to that point, those
leftovers should themselves be identical on two LP64 hosts. So a divergence
here almost certainly means something upstream diverged first. A debug-mode
canary fill on particle deallocation would turn this from a silent class into a
hard failure.

**10. Vendored libraries do branch on architecture.** The simulation and
platform layers contain no `__aarch64__`/`__x86_64__`/`__ARM_NEON`/`__SSE*`
conditional at all, but four vendored libraries compiled into the binaries do:
`lib/imgui/imgui_internal.h:63,321,327` (UI math), `lib/miniz/miniz.h:174,221`
(zip decode, reached by `platform/mod_source.c`), `lib/stb/stb_image.h:692,698`
(an SSE2 PNG-decode path with no NEON equivalent) and
`lib/dr_libs/dr_wav.h:179,1412` (WAV decode). None is on an authoritative path
today. `stb_image` is the one to watch: it is the only one whose output —
decoded pixels — could in principle be read by game logic rather than only
uploaded as a texture.

### Cleared, with the reasoning recorded

- **`sqrtf`/`sqrt`** (147 and 15 call sites in `game/src`) are IEEE-754
  correctly-rounded on both architectures — `fsqrt` on arm64, `sqrtss`/`sqrtsd`
  on x86_64. Identical results. Likewise `floorf`. These are not a hazard and
  should not be rewritten.
- **`tanf`/`atanf` in `game/src/tracks.c`** (3000-3001, 5088, 5124-5128) are
  render-only by construction and the code says so: the culling code builds the
  simulation-visible planes from `sFaithfulCullPlanes` with a literal `130.0f`
  and confines the `tanf`-derived `cullSideX` to the render planes
  (`D_8011D0F8`), with a comment recording that a previous leak of the smoothing
  margin into the tick-side predicate diverged the weather identity gate at row
  968. Keep that seam.
- **`cosf`/`sinf` in `platform/gu_port.c:20`** is `guPerspective`; render matrix
  only.
- **Pointer-derived values** were searched for and none reach the simulation.
  `sort_objects_by_dist` (`game/src/objects.c:9888-9944`) keys on the float
  `distanceToCamera` with a strict `<` and no pointer tie-break;
  `roster_item_compare` (`game/src/custom_character_roster.c:40-45`) compares
  strings; every `%p` in the tree is inside a log statement. The build also
  enforces `-Werror=int-to-pointer-cast` and `-Werror=pointer-to-int-cast` on C
  sources (`CMakeLists.txt:864-865`).
- **Save-file bitfields** (`game/include/save_layout.h:16-61`) and the
  `#pragma pack(push, 2)` in `game/include/level_object_entries.h:894-903` are
  laid out identically by Clang on x86_64 SysV and AArch64 AAPCS64 — same
  LSB-first convention, same 4-byte `unsigned` on both LP64 targets. Low risk
  for this pair; it would need re-examining for a 32-bit or big-endian target.
- **Endianness** is not a variable here. Both architectures are little-endian,
  and the ROM byte-swap layer is exercised by the existing `asset_swap_invariants`
  and `endian_utils` gates. A big-endian port would need a different gate
  entirely.

### What this audit did not cover

Auto-vectorisation. Clang may vectorise float loops differently for NEON and
SSE2, and while it is not permitted to reassociate floating-point operations
without `-ffast-math` (which this project does not use), the analysis here is
"the standard forbids it", not a measurement. If the gate diverges inside a
tight numeric loop with no uninitialised read and no libm call, compare the two
disassemblies before looking anywhere else.
