# Custom character asset pipeline spike

Status: implemented vertical-slice spike, 2026-08-26. The source contract,
compiler/cache, native transactional portable-package install, launcher
workshop and per-player fit controls, retained
WebGPU GPU-skinned renderer, animation sampler, PBR-like materials, authored
LODs, and all ten fingerprint-qualified retail donor seams are executable. This
document does not approve or propose bundling any imported character asset.

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
| Portable source package | `.mdkrchar`: deterministic ZIP containing `manifest.json`, `model.glb`, `LICENSE.txt`, and optionally a verified `compiled.mdkc` for Python-free player import | Public, versioned | Community tools and launcher |
| Runtime cache | `.mdkc`: validated, GPU-oriented sections plus a source digest | Private to an engine cache version | Import compiler and renderer |

## What the spike actually implements

- deterministic `.mdkrchar` build/verify and a strict JSON manifest schema;
- a dependency-free, fail-closed COLLADA 1.4 subset adapter for one skinned
  triangle mesh, including centimeter/Z-up conversion and embedded PNGs;
- deterministic `.mdkc` compilation with content/compiler identity, sections,
  tangents, animation tracks, semantics, sockets, materials and authored
  `MSFT_lod` levels;
- locked, transactional local install/list/remove/clean operations that retain
  source and provenance but publish only a final validated cache filename;
- bounded native cache loading, shared immutable render assets and per-player
  pose instances;
- WebGPU GPU skinning for up to 256 joints, four weights, multiple primitives,
  directional/ambient/fog response, core metallic-roughness inputs, role-aware
  mip generation, alpha masks, and explicit resource release/recreation;
- a copied retained draw command and seat-socket attachment at the retail racer
  and character-select seams, including exact-alpha previous/current pose
  replay;
- a presentation-only semantic adapter for phase-driven steering, reverse,
  boost, item, airborne/landing edges, spin, damage, win/lose finish, and
  character-select idle/hover/confirm, with package fallback for clips an
  author does not provide;
- exact US/PAL car/hover/plane LOD fingerprints and 64-bit driver-batch masks
  for all ten retail donors, so replacement is atomic and vehicle/effect
  geometry remains authored even for models with more than 32 batches;
- launcher discovery, drag-and-drop/import diagnostics and P1-P4 selection of
  installed caches;
- native browse/import/removal for portable packages, with a developer compiler
  fallback for source-only packages;
- canonical height/ground/facing normalization, independent select/car/hover/
  plane anchor profiles, and package-specific per-context size/position/
  rotation, animation-rate, vehicle-body and LOD tuning without entering
  gameplay authority;
- an author manifest wizard plus launcher diagnostics for motionless clips,
  recommended semantic coverage, and seat/head/hand sockets;
- source-v3 identity media: a bounded CRC-checked portrait, authored minimap
  colour, dedicated cache sections, one-time pool decode, deterministic 40x40
  resampling, and revisioned HUD/results/rankings/minimap resolution.
- source-v4 rig metadata: an explicit authored-clips-only or humanoid mode,
  16 bounded semantic bone roles, inference provenance/confidence, author
  review state, rest rotations and bend axes, compiled cache sections, native
  joint/hierarchy validation, and bounded engine-reference fallback poses for
  missing select/race semantics plus bounded vehicle hand/foot contact solving.
  Authored clips take precedence and remain unmodified. Exact vehicle previews
  report post-warm-up solve count and mean/maximum physical contact error.

This is deliberately a vertical slice, not a claim of production readiness.
OpenGL intentionally falls back to the retail driver, while WebGPU now has an
independent paginated custom-character select browser with package portraits,
font-width-bounded local names, and per-player identity. The COLLADA adapter
still synthesizes a motionless one-second witness clip when the source has no
animation.

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
    | author compiler embeds portable cache, or developer import compiles source
    | launcher validates and transactionally installs
    v
local versioned .mdkc cache
    |
    | async resource load
    v
presentation-only character instance
    |
    +-- package supplies local presentation identity
    +-- authoritative donor supplies physics, vehicle and simulation state
    +-- animation adapter selects semantic clips and blend parameters
    +-- WebGPU/GL renderer performs GPU skinning and material rendering
```

No source package crosses the gameplay authority boundary. A custom character
may change appearance, display name, portraits, and voice presentation, but its
physics profile remains an explicit built-in donor unless a separately reviewed
gameplay-mod system is introduced.

## Presentation identity versus a new gameplay profile

The implemented spike is a **virtual presentation identity**, not an eleventh
retail `Character` enum value. The native character-select menu now exposes
validated packages through an independent, alphabetized custom-racer browser:
eight portrait/name tiles per page, all 64 bounded catalog entries, stable
package-ID cursor rebinding, per-controller selection markers, explicit legacy
identity-upgrade states, and exact package assignment in sparse-controller
race-player order. Multiple local players may select the same package or
different packages. The package's declared donor becomes the authoritative
retail `characterId` only when race setup commits, while package identity owns
the visible name and portrait and donor voice/music are kept neutral during
character selection.

The selected package still replaces its fingerprint-qualified donor actor in
the authored 3D select scene after the browser closes; a locked donor uses
Diddy only as a neutral scene anchor while retaining the package's exact donor
for gameplay. This is enough to ship a visually distinct local character with
an explicit familiar stats profile without expanding fixed ROM tables or
pretending a donor tile is the custom identity.

This separation avoids corrupting assumptions that are genuinely fixed at ten:

- `Character`, `NUM_CHARACTERS`, the 10-by-3 `gRacerObjectTable`, and several
  ROM misc tables for weight, handling, steering response, scale and effects;
- acceleration curves reached through each donor vehicle `ObjectHeader`;
- HUD portrait/minimap colours, character voices and character-select graph;
- ghost validation, whose stored character field currently permits 0..9;
- online lobby and match-launch descriptors whose character count is frozen at
  ten; and
- rollback/simulation hashes, which include the retail `characterId`.

There are two sensible product tiers:

1. **Visual character (recommended first release).** Keep the donor as the sole
   authoritative identity. Add data-driven launcher/character-select tiles that
   resolve to `(package digest, donor)`, generated or supplied portraits, local
   display name/voice fallbacks, duplicate-donor policy, and graceful fallback
   for peers that lack the package. Existing saves, ghosts and online gameplay
   remain compatible because the visual choice never enters authority.
2. **Custom gameplay profile (separate mod category).** Define a bounded,
   versioned profile for weight, handling, steering response, acceleration
   curve, hitbox/effect choices and vehicle availability. Compile it to a
   canonical byte representation, include its digest in match preflight and
   replay/save metadata, require every peer to possess identical bytes, feed
   every physics lookup through one profile resolver, and mark records/ghosts
   modded. This is not a harmless extension of the visual manifest and should
   not be enabled by default matchmaking.

Adding two hard-coded enum rows would be quicker for one fork but is the wrong
community pipeline: it multiplies ROM-table bounds, UI layouts and protocol
versions for every character. A virtual identity registry over donor assets is
the scalable design; a canonical gameplay-profile registry can be added later
without pretending custom stats are cosmetic.

## Source package contracts (`mdkr-character-source-v2`, `v3`, and `v4`)

The spike implements the smallest useful envelope in
`tools/character_asset_probe.py`:

```text
manifest.json
model.glb
[portrait.png] # required by v3/v4; absent from v1/v2
LICENSE.txt
[compiled.mdkc]  # optional author-prepared cache for native player import
```

Entries have a fixed order, are stored without compression, timestamped at the
ZIP epoch, and restricted to regular files. `manifest.json` records the SHA-256 of
`model.glb`; v3/v4 also record the SHA-256 of `portrait.png`. This makes repeated builds byte-identical and gives the cache,
multiplayer compatibility layer, and bug reports one stable content identity.
`character_package_manager.py prepare` adds `compiled.mdkc` as the last
canonical stored member. The Python manager recompiles and byte-compares that
member when developing; the native launcher applies the same complete MDKC
validator and checks its compiler digest against every exact source member
before atomically publishing it, so packaged players need no Python and the
native launcher needs no runtime GLB compiler.
Source-only packages remain the provenance-first authoring form.

A minimal manifest is:

```json
{
  "schema": "mdkr-character-source-v2",
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
    "states": {}
  },
  "gameplay": {
    "donor": "diddy",
    "vehicles": ["car", "hovercraft", "plane"]
  },
  "presentation": {
    "source_forward": "+z",
    "target_height_m": 1.25,
    "contexts": {
      "select": {
        "anchor": "ground",
        "translation_m": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
        "scale": 1.0
      },
      "car": {
        "anchor": "seat",
        "translation_m": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
        "scale": 1.0
      },
      "hovercraft": {
        "anchor": "seat",
        "translation_m": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
        "scale": 1.0
      },
      "plane": {
        "anchor": "seat",
        "translation_m": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
        "scale": 1.0
      }
    },
    "lod_bias": 0.0
  },
  "sockets": {
    "seat": "Root",
    "head": "Head"
  }
}
```

For authored local identity, v3 adds `portrait.png` and this manifest member:

```json
"identity": {
  "portrait_file": "portrait.png",
  "portrait_sha256": "<64 lowercase hex characters>",
  "minimap_rgb": [220, 72, 144]
}
```

The portrait profile is square, 16–1024 pixels, non-interlaced 8-bit RGB or
RGBA PNG, at most 8 MiB, non-animated, and fully chunk/CRC checked offline.
The runtime independently bounds and decodes it, then uses integer
premultiplied-alpha bilinear filtering to produce the game-owned 40x40 card.
Legacy v1/v2 packages retain donor portrait and minimap fallbacks.

For reviewed skeleton semantics, v4 extends v3 with a `rig` member. It does not
infer anatomy at runtime:

```json
"rig": {
  "mode": "humanoid-retarget-v1",
  "reviewed": false,
  "roles": {
    "hips": {
      "node": "mixamorig:Hips",
      "inferred": true,
      "confidence": 0.9,
      "rest_rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
      "bend_axis": [0.0, 0.0, 1.0]
    }
  }
}
```

Humanoid mode requires distinct skin-joint mappings for hips, spine, chest,
head, both upper/lower arms and hands, and both upper/lower legs and feet. The
compiler permits intervening shoulder, neck, twist, and helper joints but
requires each semantic chain to have the correct ancestor relationship.
Inferred mappings retain `inferred: true` after review; `reviewed` records the
separate human decision. Until it becomes true, the reference solver remains
locked. `authored-clips-only` permits an empty or partial role map and is the
supported final choice for non-humanoids.

`rest_rotation_xyzw` is the normalized correction that maps the engine's
canonical reference axes into that joint's local rest basis. `bend_axis` is a
zero or normalized joint-local preference used only when a contact target is
exactly opposite the current limb direction; zero requests a stable automatic
axis. Rig Studio exposes both under each role, normalizes edited values, clears
review after any change, and shows exact compiled node indices alongside
bounded UTF-8-safe display names.

`source_forward` is deliberately explicit because arbitrary geometry does not
contain a reliable semantic front. The wizard accepts `+z`, `-z`, `+x`, or
`-x`, records that decision in its review report, and the launcher offers a
visible 180-degree correction if an author chose incorrectly. The compiler
measures the transformed scene bounds, makes height canonical, records the
bottom-center ground point, and stores the intended standing height. It does
not use a guessed centimeter-to-meter repair.

Select and vehicle placement are different contracts. Select aligns the
synthetic ground anchor to the measured donor floor. Car, hovercraft, and plane
align the named `seat` node's animated translation to independently qualified
engine seat frames. The source seat's orientation is intentionally not
inverted: source axis normalization is applied exactly once, and authored or
user context rotations remain visible instead of being canceled by a bone
basis. At runtime the root transform is composed in this order:

```text
donor object frame
  * qualified live donor target frame
  * user correction for this context
  * package correction for this context
  * canonical source height/facing transform
  * inverse source ground-or-seat translation
```

Each qualified donor profile derives local units from the live hidden driver
batches. It maps their measured height to the 1.25 m presentation reference, places
select on the measured body ground, and preserves each vehicle model's
qualified origin as its seat frame. The pure fit function is native-tested;
unknown donors, changed fingerprints, missing anchors, non-finite bounds, or
invalid heights fail visibly and retain the retail character.

The older `mdkr-character-source-v1` presentation transform remains accepted
for installed packages. It is marked legacy/uncalibrated in diagnostics and
receives safe default context anchors. New authoring emits v2 without identity
media, v3 when the wizard is given `--portrait` and `--minimap-rgb`, and v4
when an explicit rig mode is also requested.

The launcher's Portrait Studio can add or replace identity media on an
installed package without asking the artist to rebuild it. `revise-identity`
resolves the content-addressed source that exactly matches the live cache,
validates the PNG, emits a deterministic identity revision, compiles it, and
activates it with a compare-and-swap check. The prior source and provenance
remain retained; invalid input, stale provenance, or a concurrent edit cannot
replace the live cache. Existing v4 rig metadata is preserved rather than
downgraded. V2 upgrades directly. A v1 uniform transform is moved
losslessly into calibrated context transforms; non-uniform or out-of-range v1
transforms are refused because v3 cannot represent them without a visible
change.

The same revision transaction backs `revise-profile`: all ten qualified retail
donors can be selected as the built-in gameplay owner, and any non-empty subset
of car, hovercraft, and plane can be declared compatible. Enabling a previously
absent calibrated vehicle creates a neutral seat-anchored context; disabling it
retains the authored context for a later revision. Existing portrait and
license bytes remain exact. This changes which retail actor the appearance can
replace, never the donor's simulation tables or normal vehicle-selection rules.

`revise-rig` uses that transaction for an exact bounded rig draft. It accepts
only the dedicated draft schema, rejects non-regular or oversized input,
upgrades identity-capable v3 sources to v4, recompiles the full node/joint and
ancestor contract, and preserves the model, portrait, license, profile, and
old source revision. The launcher builds this draft from its skin-joint picker;
authors do not have to edit package JSON to correct inference or approve a map.

Later schema versions should add, without changing the principles above:

- package version and minimum/maximum engine asset API;
- optional creator, homepage, description, and attribution display fields;
- projected-size LOD thresholds and hysteresis (v1 already reads authored
  `MSFT_lod` chains and a package LOD bias);
- per-semantic loop/once behavior, playback scale, blend duration, normalized
  parameters, and optional additive masks;
- expanded project-owned reference clips, per-context author target offsets,
  and richer joint/pole constraints over the bounded contact solver;
- optional material variants and eye/mouth morph mappings;
- generated portrait captures, advanced selection/style tools, and icon
  derivatives beyond the implemented imported and exact 40x40 pixel-edited
  primary portrait;
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
8. texture table: v1 PNG payloads with generated complete mip chains,
   dimensions, role/color-space metadata and decoded-size accounting;
9. provenance/diagnostic summary for the launcher, not the render loop.

Cache files are disposable. A compiler/schema/backend capability change
invalidates and rebuilds them from the source package. Failed compilation is
transactional: write a temporary file, fully validate it, then rename it into
the cache. The last known-good cache remains available until replacement.

Menu and Workshop code enumerate validated caches through a lightweight
runtime catalog view containing stable package/display identity, decoded 40x40
portrait, minimap colour, donor, vehicle mask, and source revision. Catalog
inspection retains no GPU mesh or texture ownership; selecting an entry by
index resolves back through the registry and then uses the ordinary validated
player-assignment path. The select browser copies only bounded identity tokens,
sorts deterministically by display name and ID, never enters an empty grid
cell, preserves columns across partial pages, and derives a tested race plan
from potentially sparse controller slots. Selection loads the real package
before confirmation; a load failure restores the previous selection, while an
unexpected commit-time failure falls back all affected presentation identities
to retail rather than pairing a custom donor with a missing model.

Launcher-authored fit and motion tuning crosses the process boundary under the
stable package ID, not the player slot that happened to own it at startup. A
catalog selection therefore restores that package's height, offsets, rotations,
animation rate, LOD preference, vehicle mask, and four context corrections.
Explicit `MDKR_CUSTOM_CHARACTER_Pn_*` variables remain higher-priority
diagnostic overrides. Older per-player preferences are read only as a migration
fallback and are no longer synthesized into overrides that could contaminate a
different package selected later.

V1 deliberately accepts embedded PNG only. KTX2/BasisU is the preferred future
portable texture payload because it can carry mip levels and transcode to a
GPU-supported block format, but accepting it before a bounded transcoder exists
would create a package that compiles and then fails at runtime. Likewise,
`EXT_meshopt_compression` is a later source option only after the decoder and
decompression-size gates exist; the private cache may eventually use
meshoptimizer without requiring a glTF extension at runtime.

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
  time, producing previous/current poses.
- The implemented v1 retains previous/current mesh and bone-palette endpoints
  in each immutable native draw command, then resolves them with the renderer's
  exact rational replay alpha before GPU skinning. This avoids interpolating an
  already-skinned CPU vertex stream. A later animation profile can retain local
  TRS and rebuild palettes after quaternion interpolation where rigs exhibit
  large per-tick joint rotations.
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
maps semantic names to clips. `race.steer` is sampled continuously: phase 0 is
full left, 0.5 is neutral, and 1 is full right. `race.damage`, `race.land`, and
`select.confirm` clamp as one-shots; persistent states loop. A landing edge owns
a bounded 0.2-second reaction window. Missing optional mappings use `fallback`
with ordinary playback (never parameter scrubbing); missing fallback or a
zero-duration mapped clip rejects the package.

Sockets are similarly semantic. The importer resolves manifest socket names to
joint/node indices once. Gameplay references `seat`, `head`, or `hand` without
depending on an artist's bone naming convention.

### Pose is a separate calibration layer

A root transform can correct scale, floor/seat placement, and facing; it cannot
turn a T-pose into a believable driving pose. Treating those as one “rotation”
knob was the central failure exposed by the first private screenshots.

The current spike therefore distinguishes three outcomes in its diagnostics:

- **normalized and anchored:** geometry is the right size, direction, and place;
- **rig mapped:** seat/head plus independent left/right hands and feet are named;
- **motion ready:** mapped semantic clips contain changing animation keys.

The supplied static fixture reaches the first two and deliberately reports the
third as incomplete. A production general-purpose pose stage should add a
versioned humanoid role map (`hips`, spine/chest/head, upper/lower arm/hand and
upper/lower leg/foot per side), validate hierarchy and limb lengths, then use
this precedence:

1. use an authored context clip when supplied;
2. retarget a project-owned reference clip through the role map;
3. apply bounded two-bone IK for hands to wheel/grip targets and feet to pedal/
   footrest targets, with author-declared bend planes;
4. fall back to the source clip/bind pose with an explicit incomplete warning.

IK targets belong to the qualified donor vehicle profile, not to a community
package. Bone mappings and optional twist/rest-axis corrections belong to the
package. Solver output is presentation-only, clamped to joint limits, blended
at semantic transitions, and must never affect physics. Automatic bone-name
matching can propose a map, as the wizard already does for six sockets, but the
author must be able to review every inferred role. Models with missing limbs,
non-humanoid anatomy, mirrored bones, or unusable bind axes must be allowed to
choose authored animation only rather than being distorted by mandatory IK.

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

The supplied archive was used only from the user's Downloads directory; no
extracted or normalized asset is tracked by this worktree.

Archive inventory:

- outer ZIP: 5 files, 919,224 expanded bytes;
- nested archive named `Mobile - Mario Kart Tour - Drivers - Dixie Kong.zip`;
- no license, copyright, attribution, or source record in either archive;
- DAE and binary FBX 7.4 sources;
- 3,489 triangles, two materials, a skin, and body/eye textures;
- DAE metadata says OpenCOLLADA for 3ds Max, centimeters, Z-up, author `Ziella`;
- the DAE has no animation channels.

The bounded DAE adapter preserved 3,489 triangles as 2,517 unified vertices,
35 joints, two primitives/materials, two base-color maps and two normal maps.
It embedded every resource and put the declared centimeter scale and Z-up to
meter/Y-up conversion on one shared root. The policy probe consequently
reported the true scene-world height of only 0.01448 m, rather than silently
discarding the exporter metadata. The first v1 proof used an explicit 100x
presentation scale and exposed why that is insufficient: inverting the full
pelvis transform pushed the legs through the select floor, one root transform
could not represent both ground and vehicle seats, and the source faced the
wrong way in the kart. The v2 proof instead declared `source_forward: -z` and a
1.25 m target height, compiled the 0.01448 m source to canonical unit height,
used a bottom-center ground anchor for select, and used translation-only pelvis
anchors for the three vehicle contexts. Because the DAE contains no animation
channels, the
adapter generated only a named one-second motionless witness clip. Authored
COLLADA animation fails closed and must be exported as GLB from a DCC tool.

This is why “the converter succeeded” is not an acceptance criterion. The two
source representations disagree about scale, animation, and material content.
The archive is useful for exercising intake and diagnostics, but it is not a
license-complete or animation-complete character package. A download-page claim
of Creative Commons licensing would not, by itself, establish that the embedded
Mario Kart Tour-labelled files are redistributable.

## Proof delivered by this spike

The executable proof provides:

- bounded, recursive ZIP inventory without extraction;
- traversal, symlink, encrypted-member, nesting, member-count, and expanded-size
  rejection;
- COLLADA metadata/geometry/skin/animation inspection;
- GLB header/chunk parsing and an MDKR v1 geometry, skin, animation, extension,
  self-containment, and budget policy check;
- deterministic `.mdkrchar` packaging with GLB digest and required provenance;
- package verification;
- deterministic compilation by `tools/character_asset_compiler.py`;
- transactional local management by `tools/character_package_manager.py`;
- the bounded DAE convenience path in `tools/collada_to_glb.py`;
- native `.mdkc` validation, pose sampling, GPU resource construction and
  retained character commands in `platform/modern_character_*`;
- native portable-package import/removal and source retention in
  `platform/modern_character_install.c`;
- deterministic manifest inference from clip/node names in
  `tools/character_manifest_wizard.py`;
- actual WebGPU upload/draw and fingerprint-qualified replacement for every
  retail donor family.

`tests/test_character_asset_probe.py` generates a tiny license-clean GLB in
memory with indexed geometry, a two-joint skin, PBR factors, and a one-second
animation. It proves successful character admission, deterministic package
bytes, round-trip verification, rejection of external GLB resources, rejection
of missing archive provenance, and ZIP traversal protection. The generated GLB
also passes the pinned Khronos validator with zero errors and zero warnings.

`tests/test_collada_to_glb.py` independently generates a license-clean DAE and
proves unit/up-axis conversion, GLB policy acceptance, cache compilation, and
fail-closed authored-animation/polylist handling. The native loader test then
consumes the exact compiler result and covers corrupt-cache rejection, pose,
sockets, render ownership and retained-command lifetime.

`tests/test_high_fidelity_character.py` generates and compiles license-clean
25,600-vertex/50,562-triangle and 129,600-vertex/257,762-triangle skinned GLBs.
These are intentionally much larger than any retail driver mesh and prove that
the compiler/cache path does not fall back through N64 vertices or display
lists. Security ceilings are now 1,000,000 vertices, 2,000,000 triangles, 256
materials and 256 joints per skin; they are memory/work bounds, not quality
targets. The Workshop still reports much lower measured performance tiers and
expects complete LOD assemblies for ordinary play.

Workshop performance accounting follows the runtime LOD contract: it records
vertices, triangles, draw parts, and skin palette matrices per LOD instead of
misclassifying the sum of every authored LOD as one frame's cost. Its 1P-4P
assembly view uses the exact worst-visible `players × viewports` instance count,
including current/previous bone palettes, while reporting immutable geometry
and decoded texture uploads once because a repeated package shares its runtime
pool. These structural counts are deliberately advisory rather than import
ceilings; measured frame time still belongs to exact-context device stress
tests.

The launcher now has a typed, one-shot exact-context test request for character
select and car, hovercraft, or plane races in every one- through four-player
layout. It enters the ordinary select or Ancient Lake race initialization
directly—there is no menu-navigation input script—and temporarily publishes the
selected package to each requested local player. A scoped environment
transaction preserves the exact existence/value of all preview and P1-P4
variables and restores them after the blocking engine session, so a test cannot
replace saved assignments or leak into the next launch. The select actor is
anchored to the package's real donor row immediately rather than showing Diddy
until the custom browser opens.

`tests/check_custom_character_workshop_preview.py` builds and installs a
license-clean generated package with a non-Diddy donor into an isolated catalog,
then exercises select plus every vehicle, including three- and four-player
split screen, through the real WebGPU path. It requires one shared asset upload,
nonzero modern draws/triangles, zero refused draws, the exact donor, visible
captures and four-player viewport dividers. The app lifecycle unit test covers
present, absent, repeated-write and idempotent restoration cases for the scoped
handoff. Invalid context/player values, a missing assignment, and a vehicle
excluded by package tuning must all fail closed with a precise diagnostic. This
qualifies the direct game route and stress seam; embedded offscreen preview,
GPU timestamps/headroom isolation, semantic-pose controls and exported review
reports remain open.

Launcher-owned previews also arm the existing bounded presentation census. The
game discards a 120-authored-tick warm-up, resets only the observational timing
window, and freezes a structured version-1 result when the F1 overlay opens (or
at engine shutdown). The surviving launcher publishes that result back to the
same package inspector: displayed interval sample count, median/p95/p99/mean/max,
authored tick-wall sample/mean, and warmed replacement/part/donor-suppression
counts. Fewer than 60 intervals and synthetic pacing are explicitly
diagnostic-only. The report deliberately does not invent a “GPU time” from CPU
wall cadence; timestamp queries and controlled same-device comparisons remain
separate work.

The private Dixie fixture completed the same chain without contributing any
tracked bytes: DAE -> self-contained GLB -> source and portable `.mdkrchar` ->
`.mdkc` -> live character select and race. The source/portable packages,
compiled cache, logs, and screenshots remain private temporary artifacts.
The resulting cache contained 2,517 vertices, 3,489 triangles, 35 joints, two
materials, four PNG textures and 1,922,384 decoded RGBA+mip bytes. In the
corrected scripted Ancient Lake WebGPU run, diagnostics recorded one asset
upload, 1,194 complete model draws (2,082,933 triangles), 13,061 suppressed
qualified donor batches, and zero refused modern draws. The measured car-driver
body was 101 local units high; the calibrated character's pelvis landed on the
independent car seat frame and its declared -Z front was converted to the engine
forward direction. The T-pose is expected: the adapter supplied one
positive-duration but motionless witness channel because the archive has no
authored clips. The compiler and launcher now report that distinction instead
of mistaking “one clip exists” for real motion.
The corrected character-select run independently measured the donor body from
(-50, 1, -116) to (50, 179, 140), derived a 178-unit target scale and ground
point (0, 1, 12), recorded 304 primitive draws (530,328 triangles), 153 complete
replacements and zero refused draws while retaining Diddy's numbered placard.
The resulting private frame has the imported feet on the roster floor at donor
height and the face toward the select camera. This proves normalization and the
select lifecycle/semantic seam, not a polished pose for that static fixture.

### Higher-fidelity answer and firm v1 limits

There is no N64 display-list or vertex-format ceiling on this path: modern
vertices, 32-bit indices, bone palettes and textures bypass `ObjectModel` and
go directly to WebGPU. A substantially higher-fidelity character is therefore
possible. The limits below are deliberate admission/performance policy, not
legacy-engine representation blockers:

| Capability | Spike v1 | What must change for a cinematic/AAA profile |
|---|---|---|
| Geometry | 1,000,000 vertices and 2,000,000 triangles per source | Profile/device-tier budgets, measured LODs, culling and GPU timing; importing a multi-million-poly sculpt directly remains inappropriate |
| Skin | 256 joints, four linear influences, GPU skinned; non-uniform joint bind scale and joint scale tracks rejected | Normal palettes and joint-scale animation in a later profile; dual-quaternion skinning only if art requires it |
| Textures | Embedded PNG, max 4096 per side, 512 MiB decoded with full generated mips | Bounded KTX2/BasisU transcode and GPU block compression before allowing larger sets |
| Materials | Core PBR-like factors/maps plus DKR fog/sun/ambient; OPAQUE/MASK/BLEND | IBL, calibrated tone mapping, shadow receive/cast, transparent ordering, then optional hair/clearcoat/subsurface profiles |
| Animation | TRS tracks, LINEAR/STEP/CUBICSPLINE, cross-fade, semantic clips, immutable previous/current replay interpolation | Real authored clips, local-TRS/quaternion presentation interpolation, additive masks, root-motion policy and possibly morph/facial animation |
| Morphs | Rejected | Cache v2 storage, bounded weight tracks and shader path |
| LOD | Authored `MSFT_lod`, per-viewport distance bands | Projected-size thresholds, hysteresis, optional offline simplification and measured 4P targets |
| Backends | WebGPU; retail fallback on OpenGL | Implement GL parity or formally ship the modern profile as WebGPU-only |

The closest thing to a firm blocker is not polygon count. It is finishing the
resource/performance contracts around that count: compressed textures, robust
LOD/culling, shadow integration, animated normal correctness, and device-tier
qualification. The spike keeps hard caps so a user asset cannot turn those
unfinished pieces into unbounded memory or GPU work.

## Implementation plan and acceptance gates

### P0 - Freeze the source contract (partly complete)

- Review and version the manifest schema and semantic animation/socket lists.
- The checked-in JSON Schema, duplicate-key/non-finite JSON rejection and NFC
  Unicode requirement are complete. Add SPDX expression parsing,
  compression-ratio gates for general source archives, and complete GLB
  accessor checks.
- Pin Khronos Validator and adapter versions with hashes and notices.
- Add several license-clean external fixtures: static, skinned/animated,
  multi-material, morph target, alpha mask, malformed and budget-exceeding.

Gate: identical inputs produce identical packages/reports on macOS, Linux,
Windows, and wasm-capable tooling; hostile corpus is bounded and sanitizer-clean.

### P1 - Static modern mesh vertical slice (race/select seams complete for Diddy)

- Add a retained native scene command and immutable resource handle.
- Generalize `GfxModernMesh` to multiple primitives/materials, mip chains,
  lifecycle-safe caches, and WebGPU device recovery.
- Attach one static GLB-derived mesh to a donor in race and character select.
- Implement correct depth, fog, viewport, culling, and fallback behavior.
- Add OpenGL parity or formally qualify WebGPU-only fallback behavior.

Gate: ROM-free generated fixture plus one license-clean reference mesh renders
in 1P and 4P, character select, resize, device recovery, and GL fallback without
affecting authoritative hashes.

### P2 - Runtime compiler and cache (baseline complete; optimization pending)

- Implement `cgltf`-based validated loading or another small pinned glTF parser.
- Generate the sectioned `.mdkc` cache transactionally.
- Integrate meshoptimizer and KTX2/BasisU behind bounded compilation stages.
- Add content-addressed cache invalidation, quarantine reports, and teardown.

Gate: cache round-trip is deterministic; corrupt/truncated/oversized sections
fail before GPU allocation; decoded cost accounting matches actual allocations.

### P3 - Skeletal animation (baseline complete; qualification pending)

- Add skeleton/clip compilation, semantic state adapter, TRS sampling,
  cross-fades, presentation endpoint retention, and WebGPU GPU skinning.
- Skin normals/tangents correctly and validate conservative animated bounds.
- Add sockets for the vehicle seat, head, hands and effects.

Gate: generated two-joint fixture and a license-clean production-scale rig pass
clip switching, rollback correction, uncapped interpolation, split-screen,
pause, replay, character select, and device recovery with no CPU vertex stream.

### P4 - Material and shadow profile (core inputs complete; shadows pending)

- Implement the constrained core glTF PBR inputs, correct color spaces, complete
  mips, alpha mask, sun/ambient response, fog, shadow receive and shadow cast.
- Add tangent repair diagnostics and material fallbacks.
- Calibrate a stylized response that belongs in DKR rather than copying a
  cinematic renderer blindly.

Gate: material reference spheres and character fixtures match bounded offline
references on WebGPU native/browser and GL fallback; no missing mip, NaN,
pipeline explosion or transparent ordering regression.

### P5 - LOD and performance qualification (authored LODs only)

- Compile authored LODs first; optionally generate lower LODs with recorded
  simplification error.
- Choose LOD by projected size per viewport with hysteresis.
- Add bone/primitive/material batching metrics and GPU timing.
- Exercise ten visible racers across four viewports and character select at the
  low-end WebGPU device tier.

Gate: no frame-budget regression outside the written target, no unbounded cache
growth, no visible LOD oscillation, and fallback engages before allocation or
GPU limits are exceeded.

### P6 - Launcher, content packs and community release (workshop baseline complete)

- Import/diagnostic/removal UI, native portable-package install, local package
  directory, P1-P4 assignment, independent in-game custom browser, vehicle
  pairing and fit/motion controls are complete.
- Validated package portraits, exact pixel editing, identity revisions and
  game-surface fallbacks are complete; renderer capture and advanced style
  generation remain.
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
The complete user journey, including the Portrait Studio, package-keyed drafts,
donor/gameplay profile selection, exact-context previews, performance assembly
targets and accessibility gates, is specified in
[`custom-character-workshop-ux.md`](custom-character-workshop-ux.md).
