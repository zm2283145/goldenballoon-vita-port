# Custom character asset pipeline spike

Status: implemented vertical-slice spike, 2026-08-27. The source contract,
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
| External adapter handoff | `.mdkrsource`: deterministic, data-only ZIP containing strict conversion/source provenance, one self-contained `model.glb`, and an optional exact `LICENSE.txt` | Public, versioned; integrity is not adapter authentication | External DCC tools and launcher review |
| Runtime cache | `<id>.mdkc` when enabled or `<id>.mdkc.disabled` when retained outside runtime discovery: validated, GPU-oriented sections plus a source digest | Private to an engine cache version | Import compiler and renderer |

## What the spike actually implements

- deterministic `.mdkrchar` build/verify and a strict JSON manifest schema;
- an independently pinned Khronos glTF Validator boundary on every accepted
  raw, converted, source-package, portable-package and installed GLB, with
  native attested builds shipped in the macOS, Linux and Windows toolchains;
- a dependency-free, fail-closed COLLADA 1.4 subset adapter for one skinned
  triangle mesh, including centimeter/Z-up conversion and embedded PNGs;
- a launcher DAE/ZIP handoff that recursively inventories bounded archives,
  rejects unsafe or ambiguous members, converts one explicit DAE or extracts
  one character-ready GLB to an exclusively created destination, and then
  enters the ordinary resumable authoring/review path;
- format-specific, mutation-free export guidance for JSON glTF, FBX, OBJ,
  Blender, USD and common native DCC scenes. Native pickers, manual paths and
  window-wide drops all route these sources to the Workshop instead of ROM or
  package validation; the launcher never executes or loosely interprets them;
- deterministic `.mdkc` compilation with content/compiler identity, sections,
  tangents, animation tracks, semantics, sockets, materials and authored
  `MSFT_lod` levels;
- locked, transactional local install/list/enable/disable/remove/clean
operations; updates preserve enabled state, disable retains source and
provenance outside runtime discovery, and permanent removal owns only exact
content-addressed paths;
- authenticated retained-revision enumeration, optimistic-concurrency restore
  of any exact source digest, and no-overwrite source export;
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
- native browse/import/enable/disable/permanent deletion for portable packages,
  with a developer compiler fallback for source-only packages;
- private metadata-only failed-import recovery with complete bounded Khronos
  reports, missing/changed-source disclosure, exact digest-checked retry,
  no-overwrite diagnostic export and source-preserving forget;
- canonical height/ground/facing normalization, independent select/car/hover/
  plane anchor profiles, and package-specific per-context size/position/
  rotation, animation-rate, vehicle-body and LOD tuning without entering
  gameplay authority;
- an author manifest wizard plus launcher diagnostics for motionless clips,
  recommended semantic coverage, and seat/head/hand sockets;
- source-v3 identity media and names: a bounded CRC-checked portrait, authored
  minimap colour, full/short/narration/sort labels, dedicated cache sections,
  one-time pool decode, deterministic 40x40 resampling, and revisioned
  select/HUD/results/rankings/minimap resolution, roster ordering, and
  launcher Workshop/assignment narration.
- source-v4 rig metadata: an explicit authored-clips-only or humanoid mode,
  16 bounded semantic bone roles, inference provenance/confidence, author
  review state, rest rotations and bend axes, compiled cache sections, native
  joint/hierarchy validation, and bounded engine-reference fallback poses for
  missing select/race semantics plus bounded vehicle hand/foot contact solving.
  Authored clips take precedence and remain unmodified. Exact vehicle previews
  report post-warm-up solve count and mean/maximum physical contact error.
- source-v5 motion-safety metadata: optional per-role cone/twist limits and up
  to eight named, non-overlapping secondary chains (64 dynamic joints total),
  with normalized local axes and bounded spring parameters. Compiler-v9 emits
  typed MDKC-v2 sections and the native loader independently verifies role
  binding, direct parent paths, uniqueness and every numeric bound. The pose
  player clamps bind-relative swing/twist before and after vehicle contacts,
  and advances secondary deflection on a fixed 240 Hz accumulator with bounded
  hitch resets and exact held-sample resets. Rig Studio authors both contracts
  directly with named-node paths, normalized-axis controls, bounded presets,
  source-bound undo/drafts, conditional review tasks, and exact runtime clamp,
  activity, deflection, and reset diagnostics. Joint limits are admitted only
  for humanoid-reference mode; authored-clips-only revisions omit them while
  still supporting secondary follow-through for unusual skeletons.

This is deliberately a vertical slice, not a claim of production readiness.
OpenGL intentionally falls back to the retail driver, while WebGPU now has an
independent paginated custom-character select browser with package portraits,
font-width-bounded local names, and per-player identity. Compact tiles use the
authored short name while the detail surface retains the full display name;
alphabetization uses the authored sort label. The COLLADA adapter
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
    | validated registry activation and lazy renderer upload
    v
presentation-only character instance
    |
    +-- package supplies local presentation identity
    +-- authoritative donor supplies physics, vehicle and simulation state
    +-- animation adapter selects semantic clips and blend parameters
    +-- WebGPU renderer performs GPU skinning/material rendering; OpenGL keeps retail fallback
```

No source package crosses the gameplay authority boundary. A custom character
may change appearance, display name, portraits, and minimap colour, but its
physics profile remains an explicit built-in donor unless a separately reviewed
gameplay-mod system is introduced.

For authoring clarity, the launcher derives a versioned, fixed-size donor
summary from the normalized image it already owns during supported-ROM
validation. The bounded reader follows the revision's master asset LUT into
`ASSET_MISC` and its word-offset table, rejects truncated sections and
non-finite values, applies the same authored 0.45 weight coefficient as the
simulation, and copies handling plus the 14 acceleration samples consumed over
clamped speed indices 0–13. Each car, hovercraft, and plane curve is resolved
through the racer object ID, level-object translation table, bounded object
header, and its `unk5C` misc-asset reference—the same data path the simulation
uses. The ROM buffer is then released normally. Only this
immutable numeric evidence reaches the Workshop; no ROM bytes, mutable game
tables, or profile values enter a character package or configuration file.
Failure to prove the optional summary withholds the comparison without changing
the base ROM's independent boot verdict.

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

Player-owned portrait resolution is shared by the custom browser, race HUD,
rankings/results, and the giant `BHV_CHARACTER_FLAG` quads authored by Fire
Mountain and Smokey Castle. The flag seam first asks for the assigned package
card, then a project-owned bonus-racer card, and only then the bounded retail
fallback. A generated non-Diddy package is run through the real Fire Mountain
course twice with only the portrait draw suppressed in the control arm; the
package-card binding trace and isolated framebuffer difference prove that the
flag neither leaks its Bumper donor nor merely binds a texture that paints no
pixels. Ghost menus remain donor-owned, while cinematic and credits portrait
lists remain scene-authored retail cast by contract.

The Workshop reflects that boundary in a draft-aware ownership card. Race
voice and horn index `Object_Racer.characterId`, vehicle audio initializes from
that character/vehicle pair, and collision/simulation remain on the retail
object path. Ghost headers store the retail 0–9 character ID. Course-time and
fast-lap records are vehicle/course values, while adventure saves likewise do
not embed a custom package; describing those as “owned by the donor” would be
incorrect. The package owns only the local presentation fields enumerated
above, and the card says explicitly that online package negotiation is future.

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

1. **Visual character (implemented baseline).** The donor remains the sole
   authoritative gameplay identity. Data-driven launcher/character-select
   tiles resolve stable package identity plus donor, supplied or locally edited
   portraits and local display names; multiple packages may deliberately share
   one donor, and a missing/invalid assignment fails back to the retail actor.
   Donor voice/horn remains during a race and selection audio stays neutral.
   Existing saves, ghosts and online gameplay remain compatible because the
   visual choice never enters authority. Package-digest negotiation and a
   graceful peer-side missing-package presentation are still future online
   policy, not implied by the local implementation.
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

## Source package contracts (`mdkr-character-source-v2` through `v5`)

The spike implements the smallest useful envelope in
`tools/character_asset_probe.py`:

```text
manifest.json
model.glb
[portrait.png] # required by v3/v4/v5; absent from v1/v2
LICENSE.txt
[compiled.mdkc]  # optional author-prepared cache for native player import
```

Entries have a fixed order, are stored without compression, timestamped at the
ZIP epoch, and restricted to regular files. `manifest.json` records the SHA-256 of
`model.glb`; v3/v4/v5 also record the SHA-256 of `portrait.png`. This makes repeated builds byte-identical and gives the cache,
multiplayer compatibility layer, and bug reports one stable content identity.
`character_package_manager.py prepare` adds `compiled.mdkc` as the last
canonical stored member. The Python manager recompiles and byte-compares that
member when developing; the native launcher applies the same complete MDKC
validator and checks its compiler digest against every exact source member
before atomically publishing it, so packaged players need no Python and the
native launcher needs no runtime GLB compiler.
Source-only packages remain the provenance-first authoring form. Release builds
freeze the same project-owned manager and standard-library compiler into an
attested helper shipped at a deterministic path beside the game executable.
That authoring subprocess handles source-only packages and raw GLB/DAE/ZIP
without requiring a system Python installation; it never participates in a
rendered gameplay frame. Source builds retain the `.py` entry point as an
explicit developer fallback.

Compiler v5 also copies the manifest's bounded SPDX declaration, creator /
attribution text, and source URL into one optional compiled-cache provenance
record. The source digest still authenticates the complete, exact
`LICENSE.txt`; the record exists so a Python-free launcher and the runtime
inventory can present those declarations without parsing source JSON. The
launcher shows all three fields in the installed-versus-candidate review and
the installed package inspector, alongside the explicit warning that declared
metadata is not proof of rights. Compiler-v1 through compiler-v4 portable
caches remain readable and source-authenticated. Because those older caches do
not carry the record, the UI says that review metadata is unavailable and
recommends a current rebuild from retained or user-provided source instead of
inventing it or blocking local installation.

Compiler v6 fills the previously reserved short-name offset and adds one final
bounded identity-name record for narration and sort labels. Compiler-v1 through
compiler-v5 caches remain source-authenticated and use their display name for
any absent role. Candidate review protocol v3 carries all four names for both
portable and source-only packages, so a reviewed update cannot hide a label or
roster-order change.

Compiler v7 gives the existing semantic-record flags an authenticated
author-disabled meaning. Compiler-v1 through compiler-v6 caches remain valid;
only v7 source can publish disabled semantic intent. Candidate review protocol
v4 carries active and disabled semantic masks for both portable and source-only
packages, so an update cannot change runtime motion precedence behind unchanged
clip/channel/key counts.

Compiler v8 adds the explicit `$bind` fallback. When a skinned GLB has no source
animation, the compiler creates one cache-local, motionless bind channel without
changing the GLB or pretending the artist authored a clip. The package remains
unready for normal play until it gains moving semantic clips or a complete
reviewed humanoid role map; exact Workshop testing remains available.
Compiler-v1 through compiler-v7 portable packages remain accepted.

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
    "states": {},
    "disabled_states": []
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
  "minimap_rgb": [220, 72, 144],
  "short_name": "Character",
  "narration_name": "Character Name",
  "sort_label": "Name, Character"
}
```

The portrait profile is square, 16–1024 pixels, non-interlaced 8-bit RGB or
RGBA PNG, at most 8 MiB, non-animated, and fully chunk/CRC checked offline.
The three name overrides are optional and otherwise resolve to `display_name`;
when present they must be non-empty, NFC-normalized, bounded printable UTF-8
without control or bidirectional-formatting characters. The current game font
replaces each unsupported Unicode codepoint with one question-mark cell through
one bounded UTF-8 projection shared by the live custom roster and Workshop.
Authors see the exact projected display/short names, fallback counts, and the
ASCII-case-insensitive deterministic sort policy before Build; the live ROM
font still performs final pixel-width fitting. Full localized shaping remains
separate presentation work.
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

Source-v5 may add a `constraint` to a mapped role and a top-level
`secondary_motion` object:

```json
"constraint": {
  "twist_axis": [1.0, 0.0, 0.0],
  "swing_limit_degrees": 85.0,
  "twist_min_degrees": -70.0,
  "twist_max_degrees": 70.0
},
"secondary_motion": {
  "chains": [{
    "name": "hair.main",
    "root": "mixamorig:Head",
    "joints": ["hair.01", "hair.02"],
    "bend_axis": [1.0, 0.0, 0.0],
    "stiffness_hz": 6.0,
    "damping_ratio": 0.8,
    "inertia": 0.65,
    "max_angle_degrees": 35.0
  }]
}
```

Constraint axes are normalized in bind joint-local coordinates; swing is
0–180 degrees and each ordered twist endpoint is within -180–180 degrees.
There may be at most eight uniquely named chains, 16 dynamic skin joints per
chain and 64 total. Each chain is a direct root-to-child node path. Dynamic
joints cannot overlap another chain, a chain root, or a humanoid role. Spring
frequency is 0.1–30 Hz, damping ratio 0–2, inertia 0–1, and maximum deflection
0–90 degrees. The runtime applies each spring as a local additive delta over
the authored pose, clamps every integration result, and resets rather than
integrating a presentation discontinuity longer than 100 ms. Exact zero-time
held samples reset to the authored pose so comparison captures do not inherit
history from an unrelated preview.

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

High-resolution PNG and exact-renderer sources use a deterministic 40x40
conversion record: bounded square crop, crisp or premultiplied-area sampling,
edge-connected matte removal, and a project-owned background. A fixed-size
target-pixel subject mask is applied after source sampling and edge-matte
removal but before background compositing. It is non-destructive, remains
aligned to the output canvas across crop changes, and retains painted alpha
while disabled. The editor exposes both direct painting and exact numeric
coordinates; its checkerboard view deliberately suppresses the selected
background so background pixels cannot be mistaken for subject pixels. The
mask is compiled only into the resulting portrait canvas and does not add a
runtime asset format or gameplay resource.

The same revision transaction backs `revise-profile`: all ten qualified retail
donors can be selected as the built-in gameplay owner, and any non-empty subset
of car, hovercraft, and plane can be declared compatible. Enabling a previously
absent calibrated vehicle creates a neutral seat-anchored context; disabling it
retains the authored context for a later revision. Existing portrait and
license bytes remain exact. This changes which retail actor the appearance can
replace, never the donor's simulation tables or normal vehicle-selection rules.
The Workshop annotates this choice with the validated-ROM summary above, but
the annotation is read-only evidence: saving still records only the donor ID
and compatibility mask.

`revise-rig` uses that transaction for an exact bounded rig draft. It accepts
only the dedicated draft schema, rejects non-regular or oversized input,
upgrades identity-capable v3 sources to v4, recompiles the full node/joint and
ancestor contract, and preserves the model, portrait, license, profile, and
old source revision. The launcher builds this draft from its skin-joint picker;
authors do not have to edit package JSON to correct inference or approve a map.

The launcher also owns a Python-independent named-draft store. Its strict
`mdkr-character-drafts-v1` inventory is atomically replaced, caps names,
records, and opaque payloads, rejects unsafe UTF-8 controls, authenticates each
record, and never mutates the last loaded inventory after a malformed read.
The versioned binary editor snapshot captures all four identity names, the
exact portrait canvas, minimap colour, donor and vehicle choices, stable rig
node roles and solver
bases, global/context/contact tuning, assembly/test player counts, and
source/tuning-bound review state. A draft resumes only against its exact cache
source digest; restoring the retained base is required instead of guessing how
old joint node ids map onto a changed model. Snapshot v7 introduced the
portrait source's 1600-byte target-space subject mask and enabled state.
Snapshot v8 expands the exact Test camera encoding to the inclusive
-90..90-degree pitch contract while retaining the stable binary layout; v1-v7
inputs migrate under their original bounded pitch encodings, and v1-v6 mask
inputs decode with a disabled all-keep default.

`build-draft` is the corresponding single source transaction. It validates a
bounded strict build document, checks the active cache digest against the
resumed base, applies identity names, portrait, gameplay compatibility, and rig
edits to one
source snapshot, then publishes one retained revision only after package
validation and compilation succeed. Fit, contact, animation-speed, LOD, and
runtime vehicle-enable values intentionally remain a separately confirmed
launcher setting because they are not authenticated source-package fields. The
UI states that boundary and requires a second explicit apply instead of
claiming cross-file atomicity. Failed or stale builds preserve both the named
draft and playable last-known-good cache.

Later schema versions should add, without changing the principles above:

- package version and minimum/maximum engine asset API;
- optional homepage and package-description display fields beyond the
  authenticated SPDX, attribution, and source declarations already present;
- optional package-authored projected-size threshold overrides (v1 already
  reads authored `MSFT_lod` chains and package bias, then applies the engine's
  fixed projected-height thresholds and per-view hysteresis);
- per-semantic loop/once behavior, playback scale, blend duration, normalized
  parameters, and optional additive masks;
- optional richer/additive project-owned reference animation beyond the
  implemented distinct 13-state bounded library, plus richer joint/pole
  constraints over the implemented per-context target-offset contact solver;
- optional material variants and eye/mouth morph mappings;
- richer background/frame presets and icon derivatives
  beyond the implemented imported/exact-renderer
  portrait, transparent model-only render product, six-treatment comparison
  sheet, native/background/colour-vision readability proof, exact 40x40 pixel
  editor, non-destructive freeform subject mask, rectangular move/copy and
  deterministic framing, resampling, palette, dithering, outline and cleanup
  recipe;
- feature requirements such as morph targets or alpha blending.

The schema is declarative. JavaScript, Lua, native libraries, Blender Python,
shader source, network URLs used at runtime, and arbitrary filesystem paths are
all forbidden.

## Validation and security gates

Import is a hostile-input boundary even when the user trusts the artist.

The general authoring-ZIP expansion policy is explicit: each member and the
aggregate must satisfy `expanded <= compressed * 200 + 1 MiB` before any member
read, at every accepted nesting level. Stored and Deflate are the only accepted
methods. Inventory JSON exposes both byte totals and policy constants. This
guard is independent from the absolute compressed/expanded/member/depth caps.
The SPDX structural parser follows the [normative stable SPDX 3.0.1 expression
grammar](https://spdx.github.io/spdx-spec/v3.0.1/annexes/spdx-license-expressions/),
including uppercase or lowercase Boolean operators, `AdditionRef`,
custom document references, precedence, and the no-space `+` rule. It does not
freeze or imply membership in a particular revision of the evolving SPDX
License List, and it never interprets whether a declaration grants rights.

### Package gate

- Reject absolute, parent-relative, drive-qualified, duplicate, encrypted, and
  symlink ZIP members.
- Cap members, nesting, compressed bytes, expanded bytes, individual resources,
  and compression ratio before allocation.
- Require an exact schema version, model digest, non-empty license text, SPDX
  expression that passes the bounded grammar, attribution, and source URL.
- Treat license metadata as a declaration, not proof of rights. The launcher
  must say that the importer cannot verify copyright or trademark ownership.
- Never automatically upload, synchronize, or redistribute imported content.

### GLB conformance gate

- Run the pinned Khronos validator and reject all errors. Store its complete
  JSON report beside failed imports.
- The local preflight applies the [glTF 2.0 accessor and alignment
  rules](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#accessors)
  plus the narrower cache-v1 profile: it validates all buffer/view/accessor integer types,
  declared-buffer and BIN bounds/padding, component and vertex alignment,
  effective stride, finite metadata, exact POSITION binary min/max, consumed
  float finiteness, index range/restart, joint palette range, attribute counts
  and type contracts, nonzero directions, tangent handedness, usable weights,
  animation input/output shape and time order, inverse-bind layout, and embedded
  PNG structure/pixel extent before compiler byte access. Scene topology, TRS,
  shear-free matrix decomposition, material/texture/sampler references, and
  every compiler-consumed JSON scalar are checked as part of the same profile.
  The compiler independently repeats storage and hostile-type checks. Sparse
  storage and packed integer matrices are explicit cache-v1 refusals rather
  than implicit zero/default data. An extension is accepted as required only
  when cache v1 actually implements its semantics; it is never silently ignored.
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
- The WebGPU material shader decodes base color, emissive, and game-authored
  fog with the exact piecewise sRGB transfer function, performs lighting,
  shadows, occlusion, emission, and fog in linear light, then applies the exact
  inverse transfer for the existing display-space target. This matches the
  already color-aware mip builder at every level; the old `pow(2.2)` shortcut
  is forbidden by a source contract test and the generated fixture compiles
  and renders through select, all three vehicles, and four-player WebGPU.
- View-dependent response uses the exact effective eye that authored each
  gameplay view-projection, including camera shake. Matrix registration carries
  the eye and donor world together; presentation replay replaces both eye and
  view-projection from the same immutable camera pair before the HLE walk.
  The generated linked-ROM gate requires nonzero exact-eye draws and zero
  camera fallbacks in select, car, hovercraft, plane, and four-player car.
- Generate a deterministic tangent basis when a primitive lacks one. Supplied
  tangents are independently finite-checked, Gram-Schmidt orthogonalized
  against the final normal, and given a stable least-aligned-axis fallback
  when degenerate; generated bases use the same last-resort path for collapsed
  UV triangles. The compiler reports authored/generated source-part counts,
  authored repairs, degenerate UV triangles, all fallback vertices, and the
  subset touching normal-mapped materials. Candidate protocol v6 carries those
  bounded facts into mutation-free locally compiled source review, where
  normal-map fallbacks require a specific UV/tangent and lit-scene review.
  Older candidate summaries remain accepted with diagnostics explicitly
  unavailable. MDKC v2 has no free section for these authoring-only counters,
  so portable and installed-cache summaries also say unavailable rather than
  reconstructing or inventing them. The portable package still carries the
  repaired runtime tangent basis itself. No tangent path may emit a NaN.
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
from potentially sparse controller slots. Selection stages the complete
race-player plan before confirmation: every selected cache, decoded immutable
render asset, independent pose, package tuning profile, and retail-clear slot
must succeed before any live assignment changes. The runtime retains four
bounded transient pool slots in addition to the four-player active set, so even
a four-distinct-character roster can move to a disjoint four-character roster
without first sacrificing its last-known-good models. A failed stage releases
only the transient resources and preserves all prior package identities and
identity revisions. An unexpected failure at the final menu commit falls back
the complete affected presentation set to retail rather than ever pairing a
custom donor with a missing or different model.

Launcher-authored fit and motion tuning crosses the process boundary under the
stable package ID, not the player slot that happened to own it at startup. A
catalog selection therefore restores that package's height, offsets, rotations,
animation rate, LOD preference, vehicle mask, and four context corrections.
Explicit `MDKR_CUSTOM_CHARACTER_Pn_*` variables remain higher-priority
diagnostic overrides. Older per-player preferences are read only as a migration
fallback and are no longer synthesized into overrides that could contaminate a
different package selected later.

The source contract accepts embedded PNG and the exact ordinary 2D
`KHR_texture_basisu` profile. KTX2 inputs retain their authored mip chain and
are bounded and color-role checked offline, then independently authenticated by
the pinned Basis Universal runtime before WebGPU selects BC7, ASTC 4x4, ETC2,
or RGBA8. A provider that advertises but refuses compression features cannot
prevent device creation; the portable RGBA8 path remains mandatory. Likewise,
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
with ordinary playback (never parameter scrubbing). The fallback normally names
a GLB clip. `$bind` is the reserved alternative for an animationless skinned
source: it preserves bind TRS for inspection and Rig Studio, but never counts as
moving-animation or readiness evidence. An unknown fallback or a zero-duration
authored mapping rejects the package.

`animations.disabled_states` is an optional, bounded list containing only names
that remain present in `animations.states`. It records an author's reversible
decision that a mapping is unsuitable without deleting the source clip or its
semantic association. The compiler authenticates both the mapping and disabled
bit in the cache. Runtime lookup treats a disabled mapping as intentionally
missing: a reviewed humanoid receives the corresponding engine-reference pose,
while authored-clips-only content receives the package fallback. `fallback`
itself cannot be disabled. Registry diagnostics retain separate active,
disabled, and authored-moving masks so readiness and the Workshop never confuse
a preserved mapping with motion that will actually play.
Only the 13 engine select/race semantics may appear in `disabled_states`;
extension mappings remain preserved and active. Legacy rig-draft v1 revisions
do not own animation intent and therefore preserve the active source list
unchanged.

Sockets are similarly semantic. The importer resolves manifest socket names to
joint/node indices once. Gameplay references `seat`, `head`, or `hand` without
depending on an artist's bone naming convention.

### Pose is a separate calibration layer

A root transform can correct scale, floor/seat placement, and facing; it cannot
turn a T-pose into a believable driving pose. Treating those as one “rotation”
knob was the central failure exposed by the first private screenshots.

The current spike distinguishes four independent outcomes in its diagnostics:

- **normalized and anchored:** geometry is the right size, direction, and place;
- **sockets mapped:** seat/head and optional attachment sockets are named;
- **rig reviewed:** the 16 semantic skin-joint roles, rest corrections, bend
  preferences and hierarchy have explicit author approval; and
- **motion ready:** every required state has either a changing authored mapping
  or reviewed engine-reference coverage.

Source-v4 and Rig Studio implement the role map, provenance/confidence, review
lock and hierarchy validation. The runtime uses this precedence:

1. use an active authored context clip when supplied;
2. treat an author-disabled mapping exactly like an intentionally missing
   mapping, while retaining it for later restoration;
3. apply a bounded project-owned semantic reference pose through the reviewed
   rest-basis corrections when the active mapping is missing;
4. apply four-iteration bounded CCD contacts for hands and feet in vehicle
   contexts, using optional author bend preferences and persisted target
   offsets; and
5. fall back to the package fallback clip/bind pose with an explicit incomplete
   warning when reviewed reference motion is unavailable.

Base contact targets are engine-owned by vehicle context; package-local
Workshop offsets tune proportions without entering gameplay authority. Bone
mappings and optional rest/bend corrections live in the source revision.
Solver output is presentation-only, per-step angularly bounded, transition
blended, and never affects physics. Anatomical joint-limit profiles and visual
authored-pole manipulators remain follow-up work and are not claimed by the
current CCD implementation. Offset Studio does expose target manipulators and
a bounded least-squares starting point derived from exact post-solve endpoint
minus target witnesses across the current pose or complete scene battery; it
never applies the proposal without author acceptance. Models with missing limbs, non-humanoid anatomy,
mirrored bones, or unusable bind axes can choose authored-clips-only rather
than being distorted by mandatory solving.

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

1. User chooses or drops a `.mdkrchar` package.
2. The launcher validates and, for a source-only package, asks its bundled
   self-contained importer to compile a temporary candidate without publishing
   a runtime cache or retained revision.
3. A bounded candidate summary shows identity, portrait presence, donor and
   vehicle compatibility, rig/review state, total and per-LOD geometry,
   animation channels/keys, materials, textures and decoded memory beside the
   currently installed revision. Changes are stated in text, not colour alone.
4. User confirms they have the right to use the content locally. The UI states
   that included license text cannot prove copyright, trademark, attribution,
   or redistribution rights and that the package is never uploaded.
5. The install action binds the exact package SHA-256 and reviewed installed
   source digest. The commit rechecks the package bytes, then checks the current
   enabled or disabled cache under the cross-tool import lock. A changed file,
   newly appeared ID, or changed installed base fails without publishing.
6. Portable packages follow this path natively without starting the importer.
   Source-only packages emit the same strict fixed-field candidate protocol and
   use the same optimistic commit pair through the bundled author compiler.
7. A successful cache becomes selectable; failed validation or commit retains
   the last-known-good installed cache and requires a fresh visible review.
8. Named drafts autosave a bounded full editor snapshot without changing the
   playable cache. A build consumes the exact current-base snapshot and
   publishes identity/profile/rig as one optimistic source revision. Local fit
   is shown and applied separately; source-bound reviews must be repeated.
9. Disabling atomically renames `<id>.mdkc` to `<id>.mdkc.disabled`; runtime
   scans ignore it while Workshop inventory and source-backed editors retain it.
   Updates preserve this state. Player assignments and package-owned fit/review
   preferences remain, and presentation degrades to the built-in racer.
10. Permanent deletion resolves the exact cache and content-addressed source/
   provenance paths, counts named drafts in the confirmation, and clears both
   drafts and package-owned local preferences. It is locked if draft state
   cannot be read safely. Ordinary saves, records, ghosts, physics, and roster
   identity never embed the package.
11. Revision recovery authenticates the exact source and provenance pair.
   Restore snapshots those bytes, recompiles against an optimistic current-cache
   digest, and preserves enabled/disabled state; export uses exclusive creation
   and never overwrites an existing destination.
12. Portable export recompiles any selected authenticated source revision with
    the current compiler in temporary storage, then exclusively publishes the
    shareable package without touching installed state. Current-source rebuild
    uses an authenticated snapshot and optimistic cache digest; failure retains
    the last-known-good cache and enabled/disabled state.
13. Raw-source intake accepts self-contained GLB directly. DAE and ZIP inputs
    first require an explicit new GLB destination. ZIP traversal, symlinks,
    encryption, nesting, expanded bytes, member count, supported compression,
    and per-member plus aggregate expansion ratio are bounded before member
    reads; exactly one DAE or character-ready GLB must be unambiguous. Conversion happens in a
    private temporary extraction, exclusively creates the chosen output, leaves
    the download unchanged, and reports missing archive license material before
    authoring. The resulting GLB is then fingerprinted and inventoried. Up to 64
    independent source drafts live in one
    bounded, per-record authenticated, atomically replaced inventory; selection,
    switching, same-source branching, exact deletion, and legacy-singleton
    migration are durable. Open/closed editor state is independent from saved
    draft existence, so an artist can return to installed-package work and
    explicitly resume the selected raw draft without deleting it. A duplicated
    branch copies authoring decisions but
    receives a new stable draft ID and requires reinspection; the UI explicitly
    warns that its copied package ID must change to install independently. Each
    draft requires explicit identity, exact license bytes, provenance, donor,
    vehicles, forward/height calibration, fallback clip and seat/head nodes.
    Mapping names are bound to that draft's exact model SHA-256, inference is
    reviewable, duplicate names are rejected as ambiguous, and a process restart,
    source switch, or changed model requires reinspection. Switching closes only
    disposable review state. Installing or deleting removes the exact selected
    authoring record while preserving peer drafts and never mutates the external
    GLB/license. JSON glTF, FBX, OBJ, Blender, USD and native Maya/3ds Max/C4D
    scene extensions take a separate guidance-only route. It explains the
    format-specific risk and supplies a copyable GLB 2.0 checklist covering
    embedded resources, evaluated skin/animation, units/up-axis, triangulation,
    normals/tangents and later forward-axis declaration. Guidance never creates
    an output, draft, candidate, or cache and never changes the selected source.
    A successful build creates only a draft-bound disposable
    source-package candidate, which must pass steps 2–7 like any externally
    authored package.

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
- a pinned, source-digest-attested self-contained importer build plus direct
  launcher discovery and Windows/Linux/macOS release packaging, so raw authoring
  does not depend on an ambient Python installation or `PATH` lookup;
- the bounded DAE convenience path in `tools/collada_to_glb.py`;
- native `.mdkc` validation, pose sampling, GPU resource construction and
  retained character commands in `platform/modern_character_*`;
- native portable-package import/removal and source retention in
  `platform/modern_character_install.c`;
- deterministic manifest inference from clip/node names in
  `tools/character_manifest_wizard.py`;
- a bounded, source-preserving Portrait Studio style pipeline with
  strict local RGB/RGBA PNG decoding, digest-bound crop provenance,
  premultiplied-alpha reframing, edge-connected matte removal, project-owned
  background frames, deterministic median-cut palettes, ordered dithering,
  silhouette cleanup, six exact-output treatment comparisons, exact
  selection/palette operations, quality analysis, versioned draft persistence
  and source-bound history in
  `platform/app/character_portrait_import.*` and
  `platform/app/character_portrait_studio.*`;
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
pool. Quality, Balanced, Performance, and Four-player are responsive named
starting points over local-player layout and the bounded local shift of authored
LOD screen-coverage bands; both controls remain directly editable. The runtime
projects eight corners of the stable calibrated source bounds through the exact
donor-object MVP, converts the NDC span to logical viewport pixels, and applies
an 8% stateful guard band independently per player, context, and viewport. It
does not skin or scan source vertices to choose a level. Invalid or unavailable
projection evidence is recorded and falls back to the legacy distance policy.
The Workshop's projected-height scrubber mirrors the stateless thresholds,
compiled source bias, local bias, clamping, and sparse authored-level fallback.
One-LOD packages reject the false optimization affordance without being rejected
at import. Performance history owns layout and LOD preference independently from
vehicle Fit. These structural counts are deliberately advisory rather than
import ceilings; measured frame time still belongs to exact-context device
stress tests.

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
captures, four-player viewport dividers, and a stable exact backend, adapter,
driver, vendor/device ID and physical output/render size. Capture dimensions
must match the engine result even on HiDPI displays. The app lifecycle unit test covers
present, absent, repeated-write and idempotent restoration cases for the scoped
handoff. Invalid context/player values, a missing assignment, a vehicle excluded
by package tuning, incomplete view/light tuples, out-of-range or select-camera
orbits, unknown lighting, live-mode capture, noncanonical capture suffixes, and
existing destinations must all fail closed with a precise diagnostic. The gate
also holds an exact semantic phase while applying a deterministic absolute
racer-relative fitted-bounds orbit and character-only light, suppresses a
transient scripted camera bank, requires 12 consecutive fully rendered frames
after warm-up, and writes one of two explicit output-sized products. Gameplay
frame capture is the unchanged composed RGB scene. Model-only capture replays
only the validated modern-character commands from that same authored frame into
an isolated transparent WebGPU color/depth target, reads it back as straight
RGBA, and excludes the donor, vehicle, world, and HUD without changing the
visible frame. The gate decodes every PNG filter, requires the generated
character material to form a large contiguous component inside the central safe
frame, and proves the RGBA product has a nonempty bounded subject, a genuinely
transparent background, and no hidden matte color in zero-alpha pixels. It also
proves an existing capture stays byte-identical.
Every valid custom-character arm also requires result-v23's asynchronous 8 x 8 opaque-depth
witness: one exact replay records isolated occupied regions and a second replay
uses equality against the completed scene depth. Portable WebGPU occlusion
queries are treated only as booleans. Exact masks and popcounts are retained;
alpha-tested fragments use the real cutoff, blended materials are explicitly
unqualified, and a completed all-zero query is actionable “not rendered”
evidence rather than an unavailable result. The output-sized private depth
surface is released immediately after submission and the readback never blocks.
Vehicle contexts additionally replay game-tagged retained-body,
vehicle-part-sprite, and held-object batches into the private target, then test
the exact posed subject with a greater-depth diagnostic pipeline. Result v23
retains per-class presence, qualification, batch counts, and boolean overlap
masks. A named overlap means that class lies in front geometrically in this
sample, not that it is the final frontmost pixel; blended classes and an absent
held object stay explicit rather than being guessed clear.
The optional query set, resolve/readback buffers, isolated depth target, and
material-specific seed/equality pipelines are prepared as one asynchronous
validation-plus-out-of-memory error-scope transaction. No diagnostic handle is
encoded until both scopes complete. A failed transaction releases every partial
resource, leaves the ordinary character/game frame intact, and permits the
Workshop's bounded three-attempt retry policy. The linked-ROM gate injects both
a one-shot depth-allocation failure that must recover and an every-attempt
pipeline failure that must exhaust cleanly with explicit unavailable evidence;
neither may escalate to renderer-fatal state.
The linked-ROM gate includes an all-BLEND package, a two-primitive OPAQUE plus
MASK package, and the completed-zero contact fixture, so the material branches,
multi-draw replay, and failing-zero semantics execute in the real game route.
Fit approval consumes this result only after an explicit composed-scene
acknowledgement; qualified zero-fragment opaque/masked evidence blocks approval.
This qualifies the direct game route, visual inspection controls, capture seam,
stress seam, coarse opaque-depth seam, and optional exact GPU timestamp seam.
The same linked-ROM gate also captures a one-player composed retail-donor
reference with replacement pixels suppressed: the result requires zero modern
draws, nonzero fingerprint-qualified donor-character batches, nonzero
reference-only custom command preparation, the package's selected donor, a
held context midpoint, a deterministic vehicle orbit, 12 stable frames, and
automatic return. Offset Studio then queues the matching custom scene capture
as the second half of one guided action. The UI offers a pixel blend only when
the source, fit, renderer presentation, qualified scene, pose, motion source,
warmed frame/inspection timeline, target-frame fit, camera projection,
viewport/scissor, and output grid match exactly. Both products remain
comparison-only and cannot satisfy fit, motion,
portrait, or performance evidence. Embedded offscreen preview, bespoke boss,
battle, or scripted-cinematic conditions beyond the five qualified course
families, and a maintained device-profile/headroom corpus remain open.

Launcher-owned previews also arm the existing bounded presentation census. The
game discards a 120-authored-tick warm-up, resets only the observational timing
window, and freezes a structured version-21 result when the F1 overlay opens (or
at engine shutdown). The surviving launcher publishes that result back to the
same package inspector: displayed interval sample count, median/p95/p99/mean/max,
authored tick-wall sample/mean, and warmed replacement/part/donor-suppression
counts, vehicle-contact error, target-frame ground/seat anchor, calibrated
fitted volume and normalized facing from the actual replacement transform,
selected backend/adapter/driver, exact optional GPU timestamp distributions and
scope/exclusion state, and physical
output versus scene-render dimensions, held-pose/fallback ticks, camera/light
application counters, and requested/armed/written capture state, typed render
product, stable-frame count, byte count, and exact opaque-depth region masks.
Fewer than 60 intervals
and synthetic pacing are explicitly diagnostic-only. Visual-inspection results
remain session-only and cannot contaminate durable timing evidence.
Offset Studio also has a separate version-6 scene-review result containing
three complete v23 select samples or eleven complete v23 vehicle samples bound
to one of five qualified course families. Procedurally solved vehicle states
also carry a per-limb count and maximum consecutive change in endpoint-minus-
target after settling, isolating contact drift from ordinary model movement. A
single resumable launcher action
keeps select in its authored room and runs every vehicle on an open baseline,
dense scenery, an alternate climate, dark/enclosed visibility, and an
effects-heavy environment. The strict one-player exact routes
cover select idle/hover/confirm and the complete race library: both steer
extremes, reverse, boost, item, damage, spin, airborne, land, and both finishes.
Each sample requires the current held-pose generation
to settle plus sixty subsequent replacement draws before fresh camera,
visibility, and, for vehicles, surface/containment/contact evidence is accepted.
The launcher preserves each course separately, skips already-current evidence
when resuming, aggregates the most concerning state across all 55 vehicle rows,
and never stores these samples as clean performance evidence. Long clean race
detail is collapsible; any warning opens the state table by default.
The running game renders a bounded three-line status card with current course,
semantic, settling/hold/diagnostic stage, and held-draw progress. Its stop hint
opens the ordinary paused overlay with review-specific wording; stopping is a
neutral, resumable outcome and never records the incomplete course as evidence.

The launcher collects only version-21 captures armed after at least 12 eligible
frames in bounded session metadata and can export a self-contained HTML
qualification report. Publication validates the complete typed PNG and binds
its SHA-256 immediately. A model-only result also requires the renderer's exact
subject-player and authored-viewport capture witness. The backend composes the
accepted camera/object matrix with the target frame, proves that every replayed
subject primitive shares that projection and viewport, and aspect-fits the
selected split-screen viewport into the output without distortion. Engine
publication projects eight calibrated bounds corners, the fitted anchor, and a
forward endpoint into bounded integer millipixels and WebGPU depth millionths;
raw matrices remain session-only. The tray lazily decodes at most a 96-pixel bounded
thumbnail per visible digest, presents transparent products over a checkerboard,
and never retains full-resolution pixels. Report export and Portrait Studio
handoff reread the external file and refuse any later byte replacement; the
handoff carries the bound digest through its own decode so a path swap cannot
cross that boundary. Report schema v5 records `scene` or `model-alpha` and an
explicit `custom-character` or `retail-donor-reference` subject for every item,
requires the selected donor's display identity for every retail reference,
binds every item to source, fit, renderer-presentation and qualified-scene
identity, and replaces the digest record when a launcher-owned context slot is
recaptured. A failed replacement removes the stale same-path thumbnail because
its former bytes no longer exist; a full tray still admits replacement without
growing past its cap.
It displays transparency over a checkerboard and refuses RGB/RGBA
files that contradict the recorded product. It additionally records the fixed
projection witness and draws registered bounds, anchor, and forward direction
over the embedded PNG using an integer SVG coordinate space. It refuses absent
or contradictory projection data for model-only captures and refuses projection
data on composed scenes. The launcher and exporter share strict bounded PNG
validation for chunk ordering, names, CRCs, canonical IHDR dimensions, palette
requirements, nonempty image data, and terminal IEND; the exporter also checks canonical source/fit digests,
metadata bounds, capture-time identity, total byte budget and destination suffix; it escapes HTML and
embedded JSON independently, embeds image digests, omits source paths, opens the
destination exclusively, syncs it, and removes any partial write. Pure unit
tests cover integrity, injection, privacy, dimensional drift and overwrite
refusal; the rendered 200% keyboard/speech gate covers the complete tray and
report action inventory.

The launcher transactionally retains a bounded latest result and optional
qualified baseline for every select/car/hovercraft/plane by 1P-4P cell. Records
are authenticated, canonically ordered, atomically replaced, and bound to the
exact package source, per-context fit plus authored-LOD policy, result contract,
app build, presentation settings, dimensions and GPU identity. The compatibility
field remains named `fitSha256`, but its current canonical digest is the complete
test-tuning signature rather than fit alone. A fit or LOD revision can therefore
be compared only on an otherwise identical environment; stale cells remain
visible and never count as current, while an LOD-only change does not revoke the
separate vehicle-fit review. Baseline pinning, exact-cell deletion, package
deletion, restart recovery, an actual LOD-policy stale transition, and
malformed-inventory preservation are covered by a rendered ROM-free lifecycle
gate. Evidence schema v9 retains the signed target-frame bounds, ground/seat
anchor, normalized facing direction, and four exact post-solve
root/bend/target/end/error contact witnesses from the successful replacement
draw, retained-vehicle topology and containment facts, and the opaque-depth
grid plus named vehicle-body/part/held-object attribution, so Fit and
Performance retain the same renderer measurement after restart. The v23 result
contract requires either all four self-consistent
witnesses or none; a vehicle result with nonzero automatic solve count cannot
omit or partially publish them. It also carries a versioned GPU timing contract:
the initial gameplay color/depth pass is timestamped when the device supports
standard WebGPU queries, while exact ranges around accepted custom-character
draws are added only on native devices exposing in-pass timestamps. A six-slot
readback ring never waits in the frame path. Four 4,096-query character sets
cover the full accepted 512 primitives × four local packages × four viewports;
each resolve begins at a WebGPU-required 256-byte boundary. Pending, ring-full, invalid,
unsupported, device-lost, and error states remain explicit, and no wall-cadence
estimate is substituted. All optional timestamp query and readback resources are
preallocated during renderer initialization inside portable validation and
out-of-memory error scopes, so allocation failure cannot become a fatal
uncaptured device error or introduce a wait in gameplay. Authenticated
v1 through v8 inventories load older fields with explicit unavailable states
and migrate to v9 in place on the next
successful write; the established
filename remains unchanged so old evidence is never orphaned. Same-environment
baseline cards compare wall cadence, scene-pass GPU time, and—only when both
devices expose the scope—custom-character draw GPU time independently.
Weather and vehicle-condition variants beyond the qualified open, dense, and
alternate-environment course battery, plus a maintained device-profile corpus,
remain separate work.

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
forward direction. The initial adapter supplied a positive-duration but
motionless witness channel because the archive had no authored clips. Compiler
v8 removes that workaround: an animationless skinned source uses the explicit
cache-local `$bind` fallback and remains blocked from normal play until its
reference-motion rig review is complete.
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
| Textures | Embedded PNG or BasisU KTX2, max 4096 per side and 512 MiB RGBA-equivalent budget; authored KTX2 mips transcode to BC7, ASTC 4x4, ETC2, or RGBA8 | Device evidence before raising safety budgets; optional additional material profiles need their own transfer/format contracts |
| Materials | Core PBR-like factors/maps plus DKR fog/sun/ambient; OPAQUE/MASK/BLEND | IBL, calibrated tone mapping, shadow receive/cast, transparent ordering, then optional hair/clearcoat/subsurface profiles |
| Animation | TRS tracks, LINEAR/STEP/CUBICSPLINE, cross-fade, semantic clips, immutable previous/current replay interpolation | Real authored clips, local-TRS/quaternion presentation interpolation, additive masks, root-motion policy and possibly morph/facial animation |
| Morphs | Rejected | Cache v2 storage, bounded weight tracks and shader path |
| LOD | Authored `MSFT_lod`, exact calibrated projected-height thresholds per player/context/viewport, 8% runtime hysteresis, explicit distance fallback, merged-band scrubber, sparse fallback and structural transition warnings | Optional offline simplification; GPU-timed 4P targets are required by the playability gate |
| Backends | WebGPU; retail fallback on OpenGL | Implement GL parity or formally ship the modern profile as WebGPU-only |

The closest thing to a firm blocker is not polygon count. It is finishing the
resource/performance contracts around that count: compressed textures, robust
LOD/culling, shadow integration, animated normal correctness, and device-tier
qualification. The spike keeps hard caps so a user asset cannot turn those
unfinished pieces into unbounded memory or GPU work.

## Implementation plan and acceptance gates

### P0 - Freeze the source contract (baseline complete)

- Review and version the manifest schema and semantic animation/socket lists.
- The checked-in JSON Schema, duplicate-key/non-finite JSON rejection, NFC
  Unicode requirement, bounded SPDX expression parser, pre-decompression
  member/aggregate ZIP expansion gates, and complete cache-v1 GLB accessor
  preflight are implemented. Khronos glTF Validator 2.0.0-dev.3.10 at commit
  `bcd52cc4ba5f333b2999a58f67cc05ddf28b4fb1` is the independently pinned
  complete-format oracle. Its executable, distribution/source archive,
  toolchain and lockfile are hash-attested as applicable; the adapter is
  bounded for input, output, issues and runtime and never searches `PATH`.
- Validator licenses/notices, reproducible macOS arm64 source build and exact
  official Linux/Windows artifacts are part of release packaging and CI.
- Add several license-clean external fixtures: static, skinned/animated,
  multi-material, morph target, alpha mask, malformed and budget-exceeding.

Gate: identical inputs produce identical packages/reports on macOS, Linux,
Windows, and wasm-capable tooling; hostile corpus is bounded and sanitizer-clean.

### P1 - Static modern mesh vertical slice (qualified WebGPU path complete)

- Retained native scene commands, immutable resources, multiple
  primitives/materials, complete mip chains, lifecycle-safe caches and WebGPU
  device recovery are implemented.
- Race and select attach validated packages at every qualified donor seam with
  depth, fog, viewport, culling and retail fallback behavior.
- WebGPU owns the qualified modern-character path. OpenGL deliberately retains
  the built-in donor instead of attempting a partial custom draw; parity
  remains a separate renderer project if it becomes a product requirement.

Gate: ROM-free generated fixture plus one license-clean reference mesh renders
in 1P and 4P, character select, resize, device recovery, and GL fallback without
affecting authoritative hashes.

### P2 - Runtime compiler and cache (baseline complete)

- Validated GLB loading and transactional sectioned `.mdkc` cache generation
  are implemented through the bounded offline/package-manager toolchain.
- Optional deterministic meshoptimizer simplification is implemented as a
  bounded, recorded, exclusive-create offline authoring stage. Bounded
  KTX2/BasisU intake, transcoding, capability selection, reporting, and RGBA8
  fallback are complete.
- Content-addressed cache invalidation, diagnostic reports and teardown are
  implemented. The dedicated failed-import inventory retains metadata and a
  bounded validator report but never source bytes; its native Workshop surface
  supports source state, exact retry, changed-source handoff, export and forget.

Gate: cache round-trip is deterministic; corrupt/truncated/oversized sections
fail before GPU allocation; decoded cost accounting matches actual allocations.

### P3 - Skeletal animation and reviewed reference solving (baseline complete; qualification pending)

- Skeleton/clip compilation, semantic state adaptation, TRS sampling,
  cross-fades, presentation endpoint retention, WebGPU GPU skinning, reviewed
  humanoid fallback poses, and vehicle contacts are implemented. Every one of
  the 13 select/race semantics has a distinct bounded reference performance;
  evaluated-skeleton tests prove distinction and the articulated CC0 stress
  fixture exercises readable silhouettes through the linked-ROM WebGPU route.
  Package-specific artistic review remains required.
- Exact Animation Studio review holds 0/50/100% samples or alternates two
  distinct semantics through those same cross-fades. The current result-v23
  contract reports both
  motion sources, per-destination blend milliseconds, switches, blended ticks,
  completions, package-fallback ticks, and the shortest bind-relative node-local
  angular excursion for all 16 reviewed humanoid roles after contact solving.
  The bounded per-role values are session-only inspection evidence, not generic
  anatomical thresholds, automatic approval, or runtime clamps. Held reference
  samples are phase-derived and therefore independent of capture timing, while
  ordinary live reference motion continues to animate.
- Skin normals/tangents correctly and validate conservative animated bounds.
- Expand the implemented seat/head/hand/foot socket contract to named effect
  sockets only when an actual game consumer exists.

Gate: generated two-joint fixture and a license-clean production-scale rig pass
clip switching, rollback correction, uncapped interpolation, split-screen,
pause, replay, character select, and device recovery with no CPU vertex stream.

### P4 - Material and shadow profile (core inputs and world shadows complete)

- The constrained core glTF PBR inputs, correct color spaces, complete mips,
  alpha mask, sun/ambient response, fog, fitted skinned shadow casting for
  OPAQUE/MASK, and cascaded receiving for every visible material are
  implemented on the qualified WebGPU path. BLEND is receive-only by design.
- **Implemented:** presentation-camera-correct specular response. The runtime
  transforms the registered effective eye into donor-object space, interpolates
  it with the same replay camera as the VP, and records exact/fallback draw
  counts; release evidence refuses any fallback in its five standard contexts.
- **Implemented:** deterministic tangent repair/fallback plus exact authoring
  diagnostics in the compiler, candidate protocol, visual review, and spoken
  review. Retaining those counters in a future runtime-cache format is optional
  authoring provenance work, not a rendering prerequisite; current installed-
  cache summaries disclose that they are unavailable.
- Calibrate a stylized response that belongs in DKR rather than copying a
  cinematic renderer blindly.

Gate: material reference spheres and character fixtures match bounded offline
references on WebGPU native/browser and GL fallback; no missing mip, NaN,
pipeline explosion or transparent ordering regression.

### P5 - LOD and performance qualification (pipeline complete; device corpus pending)

- Compile authored LODs directly or optionally create a separate validated GLB
  with bounded, deterministic meshoptimizer levels and recorded simplification
  error. Source geometry and existing authored LODs are never overwritten.
- Retain the completed calibrated projected-height selector and its 8%
  per-viewport stateful hysteresis guard; exercise the explicit legacy-distance
  fallback in camera-seam tests.
- Bone, primitive, material, palette, geometry, texture and selected-assembly
  metrics are exposed. Exact WebGPU stress routes record post-warm-up wall
  cadence and optional timestamp distributions without fabricating unsupported
  scopes.
- Ten visible racers across four viewports and character select are executable
  stress contexts. A maintained low-end device/driver corpus remains release
  qualification rather than a claim inferred from the development machine.

Gate: no frame-budget regression outside the written target, no unbounded cache
growth, no visible LOD oscillation, and fallback engages before allocation or
GPU limits are exceeded.

### P6 - Launcher, content packs and community release (workshop baseline complete)

- Import/diagnostic/removal UI, native portable-package install, local package
  directory, P1-P4 assignment, independent in-game custom browser, vehicle
  pairing and fit/motion controls are complete.
- Validated package portraits, exact and selection-based pixel editing,
  deterministic style recipes, identity revisions, game-surface fallbacks,
  composed gameplay capture, and transparent model-only renderer capture are
  complete, including non-destructive target-space subject masking and a
  seven-view authoring readability proof; derivative background/frame presets
  remain.
- Deterministic local sort/fallback policy is implemented. Online visual-package
  digest negotiation remains a separate authority/product contract.
- Publish an SDK containing schemas, the generated animated fixture, validator,
  packer, semantic state reference, and examples that contain no Nintendo asset.
- Update modding, privacy, support, third-party, and release documentation.

Gate: a user can install, diagnose, select, disable, update and remove a
license-clean community character without a ROM beyond the game's normal base
requirement, without editing game files, and without the project redistributing
the imported source.

## Explicit non-goals for the first release

- importing `.blend` or FBX directly, or loading DAE in the game runtime (the
  launcher may convert its documented bounded DAE subset to a new GLB);
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
