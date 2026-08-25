# Custom character asset pipeline spike

Status: pipeline contract and ROM-free tooling proof, 2026-08-25. This document
does not approve a bundled character asset and does not claim that the current
renderer can display the proposed package.

## Decision

Use a self-contained **glTF 2.0 binary (`.glb`)** as the only normative model
input. Put the GLB, a data-only character manifest, and explicit license text in
a deterministic `.mdkrchar` source package. Import that package into a
versioned, local compiled cache before the game can select it.

FBX, COLLADA, OBJ, and native DCC files are not runtime formats. They may be
handled by optional offline adapters, but every adapter must emit GLB and pass
the same validator and policy gates. An adapter result is never trusted merely
because conversion returned success.

This gives three deliberately separate formats:

| Layer | Format | Stability promise | Owner |
|---|---|---|---|
| Authoring | Blender/Maya/etc.; optional FBX or DAE handoff | None | Creator and their DCC tools |
| Portable source package | `.mdkrchar`: deterministic ZIP containing `manifest.json`, `model.glb`, and `LICENSE.txt` | Public, versioned | Community tools and launcher |
| Runtime cache | Proposed `.mdkc`: validated, GPU-oriented sections plus a source digest | Private to an engine cache version | Import compiler and renderer |

The game must not parse FBX or DAE, execute scripts from a character package,
or put imported mesh/pose buffers in authoritative or rollback state.

## Why GLB is the boundary

glTF is explicitly an API-neutral runtime asset delivery format. Its core 2.0
format already defines indexed meshes, node transforms, linear-blend skins,
joint hierarchies, morph targets, named keyframe animations, and a
metallic-roughness PBR material model. GLB makes the JSON description, binary
accessors, and images self-contained.

The contract inherits glTF's fixed conventions instead of inventing new ones:

- right-handed coordinates;
- +Y up, +Z forward, and -X right;
- meters for linear distance and radians for angles;
- XYZW quaternions;
- counter-clockwise front faces under a positive-determinant global transform;
- linear-blend skinning through `skins`, `JOINTS_0`, `WEIGHTS_0`, and inverse
  bind matrices;
- animation time in seconds;
- core metallic-roughness materials, with base color, metallic/roughness,
  normal, occlusion, and emissive texture roles.

Normative references:

- [Khronos glTF 2.0.1 registry and specification](https://registry.khronos.org/glTF/)
- [Khronos glTF Validator](https://github.com/KhronosGroup/glTF-Validator)
- [KHR_texture_basisu](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_basisu)
- [Assimp format support](https://github.com/assimp/assimp/blob/master/doc/Fileformats.md)
- [meshoptimizer/gltfpack](https://github.com/zeux/meshoptimizer/tree/master/gltf)

FBX is a particularly poor public contract: it is a large, evolving DCC
handoff format with exporter-specific transforms and materials. COLLADA is open
and inspectable, but real exporters encode axis/unit conversion, controllers,
and effects inconsistently. The supplied fixture demonstrates both problems.

## End-to-end flow

```text
creator-owned source
    |
    | optional, untrusted offline adapter
    v
self-contained GLB 2.0
    |
    | Khronos validation + MDKR policy validation
    v
deterministic .mdkrchar source package
    |
    | launcher import, provenance confirmation, compile and optimize
    v
local versioned .mdkc cache
    |
    | async resource load
    v
presentation-only character instance
    |
    +-- authoritative donor supplies identity, physics, vehicle and state
    +-- animation adapter selects semantic clips and blend parameters
    +-- WebGPU/GL renderer performs GPU skinning and material rendering
```

No source package crosses the gameplay authority boundary. A custom character
may change appearance, display name, portraits, and voice presentation, but its
physics profile remains an explicit built-in donor unless a separately reviewed
gameplay-mod system is introduced.

## Source package contract (`mdkr-character-source-v1`)

The spike implements the smallest useful envelope in
`tools/character_asset_probe.py`:

```text
manifest.json
model.glb
LICENSE.txt
```

Entries have a fixed order, are stored without compression, timestamped at the
ZIP epoch, and restricted to regular files. `manifest.json` records the SHA-256 of
`model.glb`. This makes repeated builds byte-identical and gives the cache,
multiplayer compatibility layer, and bug reports one stable content identity.

A minimal manifest is:

```json
{
  "schema": "mdkr-character-source-v1",
  "id": "org.example.character-name",
  "display_name": "Character Name",
  "renderer_profile": "modern-skeletal-v1",
  "license": {
    "spdx": "CC-BY-4.0",
    "attribution": "Creator name",
    "source_url": "https://example.invalid/source"
  },
  "animations": {
    "fallback": "idle",
    "states": {
      "race.steer": "steer",
      "race.boost": "boost",
      "race.damage": "damage",
      "select.idle": "select_idle",
      "select.confirm": "select_confirm"
    }
  }
}
```

The production schema should add, without changing the principles above:

- package version and minimum/maximum engine asset API;
- optional creator, homepage, description, and attribution display fields;
- built-in donor profile and supported vehicle set;
- LOD mesh/node names and projected-size thresholds;
- named sockets such as `seat`, `head`, `left_hand`, `right_hand`, and
  `portrait_camera`;
- semantic clip mapping, loop/once behavior, playback scale, blend duration,
  and optional additive masks;
- root placement transform and a declared standing/seat reference height;
- optional material variants and eye/mouth morph mappings;
- portrait/icon references, with generated fallback renders;
- feature requirements such as morph targets or alpha blending.

The schema is declarative. JavaScript, Lua, native libraries, Blender Python,
shader source, network URLs used at runtime, and arbitrary filesystem paths are
all forbidden.

## Validation and security gates

Import is a hostile-input boundary even when the user trusts the artist.

### Package gate

- Reject absolute, parent-relative, drive-qualified, duplicate, encrypted, and
  symlink ZIP members.
- Cap members, nesting, compressed bytes, expanded bytes, individual resources,
  and compression ratio before allocation.
- Require an exact schema version, model digest, non-empty license text, SPDX
  declaration, attribution, and source URL.
- Treat license metadata as a declaration, not proof of rights. The launcher
  must say that the importer cannot verify copyright or trademark ownership.
- Never automatically upload, synchronize, or redistribute imported content.

### GLB conformance gate

- Run the pinned Khronos validator and reject all errors. Store its complete
  JSON report beside failed imports.
- Require GLB 2.0 with no external buffers, images, data fetched from URLs, or
  required extensions outside an explicit allowlist.
- Permit triangle lists only in v1; require indices, positions, normals, and UV0.
- Require `JOINTS_0` and `WEIGHTS_0` together and reject a second influence set
  initially, giving a deterministic four-weight contract.
- Validate finite transforms, normalized quaternions/normals/tangents, accessor
  bounds, increasing animation time, nonzero clip duration, weight sums, joint
  indices, inverse-bind counts, node cycles, and skin roots.
- Verify texture roles and color spaces: base color/emissive are sRGB;
  normal/occlusion/metallic-roughness are linear.
- Generate MikkTSpace tangents in the compiler when a normal-mapped primitive
  lacks them. Invalid supplied tangents are removed before generation. A
  degenerate UV island receives a counted diagnostic and material fallback,
  never a NaN.
- Compute world-space bounds after node transforms. Compare height, origin,
  orientation, and seat socket against policy; never infer a silent 100x scale
  correction.

### Initial policy budgets

These are safe starting hypotheses, not measured shipping limits:

| Resource | Hard import cap | Recommended LOD0 target |
|---|---:|---:|
| Triangles | 100,000 | 15,000-30,000 |
| Unique vertices | 100,000 | 30,000 or fewer |
| Joints | 128 | 64 or fewer |
| Influences per vertex | 4 | 4 |
| Materials/primitives | 16 | 4 or fewer |
| Texture dimension | 4096 | 2048 |
| Morph targets | 8 | 4 or fewer |
| Named clips | 64 | 16 or fewer |
| Total source package | 512 MiB | under 64 MiB |

The compiler must also calculate decoded texture VRAM, vertex/index bytes,
animation bytes, per-instance bone-palette bytes, and worst-case draw count.
Admission uses those calculated costs rather than compressed package size.

## Runtime compiled cache (`.mdkc`)

Do not make the renderer a general glTF implementation. The launcher/import
worker should compile validated GLB into a private, versioned cache with:

1. fixed header: magic, cache schema, source package digest, compiler version,
   section table, total size and checksum;
2. mesh table: LODs, primitives, material index, bounds and vertex/index ranges;
3. interleaved GPU vertices: position, normal, tangent, UV, joint indices and
   normalized weights;
4. 16- or 32-bit index streams selected per primitive;
5. skeleton: parent indices, inverse bind matrices, bind-pose TRS and socket map;
6. animation table: semantic/name hash, duration, resampled/compressed TRS tracks
   and optional morph weights;
7. material table: factors, alpha mode, double-sided flag and texture indices;
8. texture table: KTX2/BasisU payloads with complete mip chains, dimensions,
   role/color-space metadata and decoded-size accounting;
9. provenance/diagnostic summary for the launcher, not the render loop.

Cache files are disposable. A compiler/schema/backend capability change
invalidates and rebuilds them from the source package. Failed compilation is
transactional: write a temporary file, fully validate it, then rename it into
the cache. The last known-good cache remains available until replacement.

`KHR_texture_basisu` is the preferred portable texture payload because KTX2 can
carry mip levels and transcode to a GPU-supported block format. PNG remains a
source fallback. `EXT_meshopt_compression` is a later source option only after
the decoder and decompression-size gates exist; the private cache may use
meshoptimizer directly without requiring a glTF extension at runtime.

## Renderer scope

The existing `GfxModernMesh` path proves that WebGPU can draw float vertices and
32-bit indexed geometry into the live DKR color/depth target. It is not yet a
character renderer. Production work should replace or generalize it rather than
route modern characters through `ObjectModel`/F3DDKR.

### Geometry and resource layer

- Immutable vertex/index buffers per mesh LOD and shared material textures per
  imported character asset.
- Per-instance root transform, tint/opacity, pose palette, visibility and LOD.
- Resource handles with generation IDs; no raw mutable pointers in retained
  display lists.
- Async decode/upload and a placeholder/fallback until resources are ready.
- Bounded caches with explicit release on package removal, level teardown,
  renderer shutdown, and WebGPU device recovery.
- Equivalent OpenGL implementation or an explicit fallback to the low-poly
  `ObjectModel` when the diagnostic GL backend is selected.

### Skinning and animation

- Evaluate the skeleton once per authored tick from semantic state and clip
  time, producing previous/current local poses.
- Interpolate TRS at presentation time, then build a GPU bone palette. Do not
  interpolate already-skinned vertices.
- Skin position, normal, and tangent in the vertex shader. Normalize weights at
  import and the transformed normal/tangent in the shader.
- Support linear and step tracks first. Either implement cubic spline exactly
  or resample it during compilation; never silently treat it as linear.
- Start with cross-fade between two clips. Add additive upper-body/head/hair
  layers only after the base state matrix is proven.
- Compute conservative per-clip/per-LOD skinned bounds offline for culling.

### Material and lighting profile

`modern-skeletal-v1` should implement a constrained glTF material subset:

- base color factor/texture;
- metallic/roughness factor and packed texture;
- tangent-space normal map;
- occlusion and emissive;
- OPAQUE and MASK initially; BLEND after ordering is designed;
- double-sided flag;
- existing DKR fog and scene depth;
- directional sun, ambient/environment term, and the existing remastered shadow
  system as receiver and caster.

This is a deliberately bounded PBR-like profile, not arbitrary glTF shader
extensions. Skin subsurface scattering, anisotropic hair, clearcoat,
transmission, and custom shaders are later renderer profiles with separate
compatibility and performance gates.

### Scene insertion

Modern draws must occur at the correct point in each viewport's retained scene,
not as a post-scene overlay. A native-only retained command should reference an
immutable asset handle plus a copied presentation instance record. Replay then
resolves interpolated root and bone endpoints without observing mutable game
memory.

The object/donor render seam should:

1. identify a virtual character assigned to a built-in donor;
2. suppress only the donor's character geometry, not its vehicle/effects;
3. emit the modern character command using the donor/vehicle attachment
   transform;
4. fall back atomically to the authored low-poly visual if the asset, renderer,
   pose, or material is unavailable;
5. repeat independently for every viewport and character-select camera.

## Game and animation integration

The game currently expresses useful animation intent through numeric animation
IDs, frame counters, steering, attack/spin/boost state, race finish state, and
separate character-select logic. The modern layer needs a typed adapter instead
of teaching packages those internal numbers.

Initial semantic states should include:

- `race.steer` with a continuous normalized steering parameter;
- `race.reverse`, `race.boost`, `race.damage`, `race.item`, and `race.spin`;
- `race.airborne`, `race.land`, `race.finish_win`, and `race.finish_lose` where
  the vehicle/state supplies them;
- `select.idle`, `select.hover`, and `select.confirm`;
- `fallback`, used whenever a mapped clip is absent.

Each semantic row defines priority, interruptibility, loop behavior, normalized
phase source, playback rate, and blend duration in engine code. A package only
maps semantic names to clips. Missing optional mappings use `fallback`; missing
fallback or a zero-duration mapped clip rejects the package.

Sockets are similarly semantic. The importer resolves manifest socket names to
joint/node indices once. Gameplay references `seat`, `head`, or `hand` without
depending on an artist's bone naming convention.

## Rollback and multiplayer

- The authoritative racer remains a built-in identity/profile. Imported IDs,
  paths, GPU handles, animation floats, and material state never enter rollback
  snapshots or gameplay hashes.
- Presentation pose is re-derived after correction from donor state and clip
  phase. Retained rendering owns immutable previous/current pose endpoints.
- A session manifest may carry an optional visual package digest for diagnostics
  only. A peer missing that digest renders its local fallback.
- Peers never transfer character packages to one another. A server never hosts
  them as part of matchmaking.
- Voice, name, icon, and accessibility narration need explicit local fallbacks;
  none may alter collision, stats, item logic, or roster indexing.

## Launcher and community workflow

1. User chooses a `.mdkrchar` or a raw `.glb` plus manifest/license files.
2. Launcher inventories the package without extracting it and shows provenance.
3. User confirms they have the right to use the content locally.
4. Import worker runs Khronos validation, MDKR policy validation, normalization,
   compilation, and a headless render smoke.
5. Launcher shows triangle/joint/material/texture/VRAM/clip statistics and every
   repair or fallback. Repairs are never silent.
6. A successful cache becomes selectable; a failed import is quarantined with a
   machine-readable report.
7. Removing a package removes its source and cache only after resolving their
   exact content-addressed paths. Saves retain only a stable local package ID
   and degrade to fallback if it disappears.

Raw GLB convenience import can have the launcher generate a manifest template,
but it still requires explicit license/provenance fields before activation.

## Supplied Dixie archive: objective result

The archive was used only from `/Users/adamkratch/Downloads`; no extracted or
normalized asset is tracked by this worktree.

Archive inventory:

- outer ZIP: 5 files, 919,224 expanded bytes;
- nested archive named `Mobile - Mario Kart Tour - Drivers - Dixie Kong.zip`;
- no license, copyright, attribution, or source record in either archive;
- DAE and binary FBX 7.4 sources;
- 3,489 triangles, two materials, a skin, and body/eye textures;
- DAE metadata says OpenCOLLADA for 3ds Max, centimeters, Z-up, author `Ziella`;
- the DAE has no animation channels.

The legacy adapter experiment used locally built Assimp 6.0.5, glTF-Transform
4.4.2, gltfpack 1.0.1, and Khronos Validator 2.0.0-dev.3.10:

| Route | Preserved | Lost or invalid | Result |
|---|---|---|---|
| DAE -> Assimp GLB | 3,489 triangles, skin, four images including two normal maps | No clips; approximately 100x scale discrepancy; invalid zero tangents; external images until repacked | Can be made GLB-valid by embedding resources and removing invalid tangents, but not character-ready |
| FBX -> Assimp GLB | 3,489 triangles, plausible meter scale, 34-joint normalized skin, two base-color images | Normal maps are lost; Windows path separators need normalization; `Take 001` has zero duration | Khronos-valid after repacking, but rejected as a character because it has no positive-duration clip |

The final FBX-derived normalized GLB measured 2,444 vertices, 3,489 triangles,
two draw primitives, two materials, two textures, one skin with 34 joints, and a
single nine-channel zero-duration animation. The Khronos validator reported no
errors and one skin-root warning. The MDKR policy validator correctly rejected
it for having no animation with positive duration.

This is why “the converter succeeded” is not an acceptance criterion. The two
source representations disagree about scale, animation, and material content.
The archive is useful for exercising intake and diagnostics, but it is not a
license-complete or animation-complete character package. A download-page claim
of Creative Commons licensing would not, by itself, establish that the embedded
Mario Kart Tour-labelled files are redistributable.

## Proof delivered by this spike

`tools/character_asset_probe.py` provides:

- bounded, recursive ZIP inventory without extraction;
- traversal, symlink, encrypted-member, nesting, member-count, and expanded-size
  rejection;
- COLLADA metadata/geometry/skin/animation inspection;
- GLB header/chunk parsing and an MDKR v1 geometry, skin, animation, extension,
  self-containment, and budget policy check;
- deterministic `.mdkrchar` packaging with GLB digest and required provenance;
- package verification.

`tests/test_character_asset_probe.py` generates a tiny license-clean GLB in
memory with indexed geometry, a two-joint skin, PBR factors, and a one-second
animation. It proves successful character admission, deterministic package
bytes, round-trip verification, rejection of external GLB resources, rejection
of missing archive provenance, and ZIP traversal protection. The generated GLB
also passes the pinned Khronos validator with zero errors and zero warnings.

This proves the portable source/package boundary. It intentionally does not
pretend that a Python policy probe is the runtime compiler or that a triangle
fixture proves the renderer.

## Implementation plan and acceptance gates

### P0 - Freeze the source contract

- Review and version the manifest schema and semantic animation/socket lists.
- Add JSON Schema, duplicate-key detection, SPDX expression parsing, Unicode
  normalization, compression-ratio gates, and complete GLB accessor checks.
- Pin Khronos Validator and adapter versions with hashes and notices.
- Add several license-clean external fixtures: static, skinned/animated,
  multi-material, morph target, alpha mask, malformed and budget-exceeding.

Gate: identical inputs produce identical packages/reports on macOS, Linux,
Windows, and wasm-capable tooling; hostile corpus is bounded and sanitizer-clean.

### P1 - Static modern mesh vertical slice

- Add a retained native scene command and immutable resource handle.
- Generalize `GfxModernMesh` to multiple primitives/materials, mip chains,
  lifecycle-safe caches, and WebGPU device recovery.
- Attach one static GLB-derived mesh to a donor in race and character select.
- Implement correct depth, fog, viewport, culling, and fallback behavior.
- Add OpenGL parity or formally qualify WebGPU-only fallback behavior.

Gate: ROM-free generated fixture plus one license-clean reference mesh renders
in 1P and 4P, character select, resize, device recovery, and GL fallback without
affecting authoritative hashes.

### P2 - Runtime compiler and cache

- Implement `cgltf`-based validated loading or another small pinned glTF parser.
- Generate the sectioned `.mdkc` cache transactionally.
- Integrate meshoptimizer and KTX2/BasisU behind bounded compilation stages.
- Add content-addressed cache invalidation, quarantine reports, and teardown.

Gate: cache round-trip is deterministic; corrupt/truncated/oversized sections
fail before GPU allocation; decoded cost accounting matches actual allocations.

### P3 - Skeletal animation

- Add skeleton/clip compilation, semantic state adapter, TRS sampling,
  cross-fades, presentation endpoint retention, and WebGPU GPU skinning.
- Skin normals/tangents correctly and validate conservative animated bounds.
- Add sockets for the vehicle seat, head, hands and effects.

Gate: generated two-joint fixture and a license-clean production-scale rig pass
clip switching, rollback correction, uncapped interpolation, split-screen,
pause, replay, character select, and device recovery with no CPU vertex stream.

### P4 - Material and shadow profile

- Implement the constrained core glTF PBR inputs, correct color spaces, complete
  mips, alpha mask, sun/ambient response, fog, shadow receive and shadow cast.
- Add tangent repair diagnostics and material fallbacks.
- Calibrate a stylized response that belongs in DKR rather than copying a
  cinematic renderer blindly.

Gate: material reference spheres and character fixtures match bounded offline
references on WebGPU native/browser and GL fallback; no missing mip, NaN,
pipeline explosion or transparent ordering regression.

### P5 - LOD and performance qualification

- Compile authored LODs first; optionally generate lower LODs with recorded
  simplification error.
- Choose LOD by projected size per viewport with hysteresis.
- Add bone/primitive/material batching metrics and GPU timing.
- Exercise ten visible racers across four viewports and character select at the
  low-end WebGPU device tier.

Gate: no frame-budget regression outside the written target, no unbounded cache
growth, no visible LOD oscillation, and fallback engages before allocation or
GPU limits are exceeded.

### P6 - Launcher, content packs and community release

- Build import/diagnostic/removal UI and a local package directory.
- Generate portraits or accept validated package portraits with fallbacks.
- Add local enable/order policy and online digest/fallback diagnostics.
- Publish an SDK containing schemas, the generated animated fixture, validator,
  packer, semantic state reference, and examples that contain no Nintendo asset.
- Update modding, privacy, support, third-party, and release documentation.

Gate: a user can install, diagnose, select, disable, update and remove a
license-clean community character without a ROM beyond the game's normal base
requirement, without editing game files, and without the project redistributing
the imported source.

## Explicit non-goals for the first release

- importing `.blend`, FBX, or DAE inside the game;
- arbitrary character scripts or native plugins;
- custom gameplay physics in a visual package;
- peer-to-peer asset transfer;
- arbitrary glTF extensions or custom shaders;
- cinematic facial rigs, strand hair, subsurface scattering, or unbounded 4K/8K
  texture sets;
- claiming that a syntactically present license makes copyrighted source safe
  to distribute.

The first useful release is a secure, deterministic, modern-stylized character
pipeline with reliable fallbacks. “AAA” features can then become additional
renderer profiles rather than exceptions punched through the import contract.
