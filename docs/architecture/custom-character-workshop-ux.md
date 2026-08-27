# Custom Character Workshop product specification

Status: product architecture and phased acceptance plan, 2026-08-27.

This document defines the complete user experience around the executable
custom-character pipeline. It is intentionally broader than importing a mesh.
A finished community character needs a visual identity, a trustworthy gameplay
relationship, usable animation, vehicle fit, performance qualification,
in-game previews, provenance, and a reversible way to update or remove it.

## Product promise

A player should be able to take a rights-cleared, self-contained GLB from an
artist, understand every problem without engine vocabulary, produce a polished
local character, test every place it will appear, and install or share a
deterministic package without supplying another ROM.

The workshop must never imply that a green geometry check means the character
is complete. It reports six independent dimensions:

1. identity and portrait coverage;
2. geometry, materials, and source normalization;
3. rig mapping and authored motion;
4. ground, seat, hand, and foot attachment quality by context;
5. renderer cost and device-tier compatibility; and
6. gameplay/online authority and donor qualification.

## Industry patterns worth adopting

- Epic separates an editable MetaHuman Character from rigging and final
  assembly, including explicit real-time quality targets. The workshop should
  likewise preserve source plus a small non-destructive draft and build a
  disposable runtime cache. See [MetaHuman character states and assembly](https://dev.epicgames.com/documentation/metahuman/creating-a-character)
  and [optimized assembly quality](https://dev.epicgames.com/documentation/metahuman/metahuman-creator-python-scripting-in-unreal-engine?lang=en-US).
- Unity exposes automatic humanoid mapping for review, highlights missing or
  invalid bones, and permits manual correction rather than treating name
  inference as truth. See [Unity humanoid Avatar configuration](https://docs.unity3d.com/es/current/Manual/ConfiguringtheAvatar.html).
- Roblox opens imported characters in a preview, exposes manual front alignment
  when inference fails, and organizes animation/body/face tests before use. See
  [Roblox Avatar Setup](https://create.roblox.com/docs/avatar-setup/auto-setup).
- VRChat ranks the worst performance category, shows the underlying counts,
  and warns that the rank is guidance rather than a complete performance
  oracle. See [VRChat Avatar Performance Ranks](https://creators.vrchat.com/avatars/avatar-performance-ranking-system/).

These are interaction references, not format dependencies. MDKR remains local,
bounded, deterministic, and much smaller in scope.

## Four separate data layers

| Layer | Contains | Authority | Mutability |
|---|---|---|---|
| Source package | GLB, manifest, license, optional authored identity media | Creator-owned presentation data | Immutable after content digest |
| Workshop draft | Portrait recipe, inferred-role confirmations, context corrections, quality target, test acknowledgements | Local presentation only | Non-destructive, undoable, autosaved |
| Runtime assembly | Validated `.mdkc`, generated portrait/icon surfaces, conservative bounds and cost report | Renderer only | Disposable and reproducible |
| Gameplay profile | Built-in donor reference or separately signed/hashed custom stats profile | Simulation authority | Never implicit in a visual package |

Player assignments point to a package/draft identity. They do not duplicate the
draft four times. Updating a draft changes how that character looks for every
local player using it; assigning the character to P2 does not create another
copy of its fit settings.

## Information architecture

Custom Characters should become a dedicated **Character Workshop**, not an
ever-growing accordion inside general Settings. Settings retains only a compact
shortcut, enabled/disabled state, and per-player assignment summary.

The workshop uses a persistent library rail and one editor surface:

```text
Character library              Selected character
---------------------------    -------------------------------------------
+ Import character             Name, portrait, readiness and unsaved state
Dixie                    4/6    [Overview] [Identity] [Rig & Motion]
Tiny                     6/6    [Vehicles] [Performance] [Test] [Package]
Broken Robot           Review   Exact-context preview + contextual controls
                               Problems / suggested next action
                               Undo  Redo  Revert   Save draft   Build
```

The last selected character and tab are local UI state. A library entry remains
inspectable when it cannot be activated. Disabled actions always say why.

### First import

The import flow is a resumable sequence, not one blocking dialog:

1. **Choose source** — `.mdkrchar` preferred; GLB convenience import creates a
   draft and asks for provenance before it can build.
2. **Inventory** — show package author, declared license, source URL, digest,
   size, geometry, textures, rig, clips, and security-policy result.
3. **Normalize** — preview up/front/height/ground; require explicit front-axis
   confirmation and intended standing height.
4. **Map rig** — show proposed bones and confidence; required roles are visible
   on the model and manually replaceable.
5. **Choose identity/profile** — enter local display/narration names, create or
   import portraits, and select a qualified built-in donor profile.
6. **Fit and test** — select, car, hovercraft, and plane are separate test cards.
7. **Build** — show warnings that remain, build a deterministic assembly, and
   offer assignment only after activation gates pass.

Closing the window never loses a completed step. It leaves a clearly labelled
draft that can be deleted independently from the original imported file.

A failed validation likewise remains actionable without taking custody of the
model. The private recovery inventory stores only its absolute path, bounded
metadata, content digest when safely readable, the user-facing error and an
optional complete bounded Khronos report. It detects missing and size-changed
sources without re-reading every model during ordinary Workshop rendering;
**Retry exact source** always rehashes the full bounded file before validation.
Same-size edits therefore cannot reuse stale evidence. Authors can restore the
path as a new import, export a self-contained JSON diagnostic without model
bytes, or forget only the metadata/report after confirmation.

## Overview

Overview answers five questions without expanding advanced controls:

- What character is this and who should be credited?
- Is it safe to activate?
- What built-in gameplay profile will it use?
- Where is it incomplete?
- What is the single best next action?

Readiness is shown as named rows with **Ready**, **Review**, **Missing**, or
**Unavailable**, never color alone. “Ready to preview” and “Ready to play” are
different. A static T-pose can preview geometry but cannot receive a polished
badge. Optional shortcomings do not block local testing; hard failures do.

## Identity and Portrait Studio

Roster identity is not a donor side effect. Its versioned record should own:

- display name, short HUD name, language-neutral narration name and sort label;
- primary 40x40 HUD/results portrait;
- character-select tile/crop;
- minimap marker color and optional simple silhouette;
- results/accolade badge crop;
- local voice/horn policy: explicit bundled profile, compatible built-in
  fallback, or intentionally silent;
- creator attribution and license display.

The Workshop previews the exact retail-font projection for display and short
names through the same bounded function used by the live custom roster. Each
unsupported Unicode codepoint occupies one question-mark cell, malformed or
unterminated input fails safely, narration retains authored UTF-8, and the UI
discloses that final pixel-width fitting occurs against the loaded ROM font.
Sort labels use deterministic ASCII-case-insensitive byte order with package id
as the tie-break; authors are advised to use ASCII when locale-independent
ordering matters. This makes today's fallback honest without claiming full
localized shaping.

### Portrait inputs

The Portrait Studio offers three reversible starting points:

1. **Capture model** as a stabilized frame from the exact WebGPU character
   renderer and send it directly from the Test capture tray.
2. **Import image** from a bounded local RGB or RGBA PNG.
3. **Draw pixels** on the exact 40x40 runtime canvas.

The model capture exposes semantic pose, normalized animation time, fitted-bounds
camera yaw/pitch and character lighting. Capture runs locally. The resulting
product is chosen explicitly: either an ordinary composed RGB gameplay PNG or a
straight-RGBA model-only PNG replayed by the exact renderer without donor,
vehicle, world, or HUD. Portrait Studio supplies a square crop, optional
edge-connected matte removal for composed sources, and project-owned background
frames. A reversible 40x40 target-space subject mask can remove or restore exact
output pixels after sampling and edge-matte removal but before background
compositing. Its checkerboard editor, 1-7 pixel brush, invert/reset actions, and
numeric coordinate action provide equivalent pointer and keyboard/controller
routes. The mask stays aligned to the output canvas when the crop changes, so
the UI requires a fresh visual review or an explicit reset after reframing. The
draft stores the source kind, PNG digest, dimensions, deterministic conversion
recipe, mask, and exact framed result. The source path is only a reload
convenience, so moving the original PNG cannot invalidate already-authored
portrait work.

The pixel editor provides pencil, eraser, fill, eyedropper, lasso/move,
horizontal mirror, palette replace, undo/redo, onion comparison against the
source capture, and keyboard/controller-accessible numeric color entry. It is a
small purpose-built editor, not a general paint application.

High-resolution framing and masking are provisional until the user sends the result to the style lab
or applies it to the pixel canvas. Crop, sampling, matte, and background edits
plus freeform mask strokes have a separate bounded, gesture-coalesced undo/redo
track; loading a different
source resets only that provisional track. Reloading the same digest restores
the retained draft recipe and mask instead of silently recentring it. Disabling
the mask retains its authored pixels for later reuse; version-six and older
drafts migrate to a disabled all-keep mask.

### DKR style conversion

“DKR-style” is a deterministic filter stack with an always-visible before/after:

- crop and subject-safe area;
- optional silhouette cleanup and 1–2 pixel outline;
- fixed-size downsample using nearest, box, or area sampling;
- selectable 16/32/64-color quantization;
- optional ordered dithering with strength control;
- a responsive six-treatment comparison sheet that preserves framing and
  cleanup while previewing exact clean, classic, bold, crisp, dithered, and
  soft 40x40 results before changing the draft recipe;
- background gradient/frame presets derived from project-owned values;
- exact 40x40 author and runtime canvas.

Nothing invokes a network service or claims to create copyright-safe art. A
generated image can be edited pixel-by-pixel. The UI previews 1x native size,
4x nearest-neighbor, HUD, results, character select, light/dark backgrounds and
color-vision screening simulations. The responsive proof sheet shows the
native transparent card plus dark HUD, light results, grayscale, protanopia,
deuteranopia, and tritanopia stress views from the exact styled bytes. It labels
these as authoring aids rather than clinical simulations or scene screenshots;
exact installed HUD/results/rankings/roster/minimap/arena presentation remains
the Test gate's responsibility. It warns about transparent holes, unreadable
silhouettes, clipped hair/ears, low face occupancy, excess colors, and contrast.

One approved composition generates every required derivative; users should not
have to align five nearly identical files independently. Packages may instead
supply validated final PNGs plus attribution when an artist already created
them.

Player-owned identity resolves on the custom roster, local HUD portraits,
minimap markers, post-race cards, and trophy rankings. Scripted cinematic and
credits portraits remain scene-authored retail cast, not aliases for the local
player. Controller Pak ghosts, course records, saves, and online/rollback
identity likewise retain the donor ID by design: they cannot depend on a local
package that another machine or a later install may not have. The generated
identity-surface gate proves this separation by retaining a non-Diddy donor
while the package portrait and minimap colour reach their player-owned seams.

## Gameplay profile and kart selection

### Visual-only release

The default product is explicit: **Appearance: custom; gameplay: built-in**.
The user selects from fingerprint-qualified donor profiles, not raw character
IDs. The current Workshop reads a bounded numeric summary during the launcher's
existing validated-ROM pass; no ROM bytes, table pointers, or copied tables are
retained. Its profile comparison shows:

- the name of the built-in gameplay character;
- exact effective-weight and handling coefficients with relative roster-range
  bars sourced from the verified game tables;
- the exact 14 authored acceleration multipliers for car, hovercraft, and plane
  as consistently scaled plots, hover values, and a keyboard-readable value
  table rather than a fabricated one-number rating;
- voice/horn/effect fallbacks;
- car/hovercraft/plane qualification state;
- exact authority consequences: race voice/horn/vehicle audio use the donor;
  ghosts and network/rollback store the donor character ID; course records and
  adventure saves remain ordinary game data and never embed the package;
- a visible evidence-unavailable state when no supported ROM has been verified.

The bars describe range position, never “better” or “worse.” Top speed is not
presented as a donor scalar because the live result is contextual game logic,
not a single authoritative value in this comparison path.

The implementation exposes qualified geometry seams for every retail donor.
Profile cards can therefore present all ten choices without a hidden Diddy-only
gate; an unavailable or changed ROM schema still fails visible and retains the
built-in actor.

The current responsive ownership card puts **Custom package owns** beside
**Built-in profile remains authoritative** on wide screens and stacks the same
content on handheld/narrow layouts. It updates against the unsaved donor draft,
states that character-select audio is neutral, and identifies package
negotiation as future work rather than implying that peers already receive the
appearance.

Vehicle selection has two meanings and the UI must not conflate them:

- **Compatibility:** which car/hovercraft/plane contexts the package has been
  authored and tested for.
- **In-game choice:** the vehicle selected by the course/menu and still owned by
  normal DKR rules.

Each compatible vehicle has its own seat, hand, foot, camera-occlusion and
animation test card. A package cannot claim “all vehicles ready” merely because
three checkboxes are enabled.

### Custom performance tuning

Arbitrary acceleration, weight, handling or hitbox tuning is a separate
**Gameplay Mod Profile**, never fields added to `.mdkrchar`.

Its editor can eventually offer bounded values, built-in-character presets,
side-by-side curves, unit-labelled sliders, reset-per-field, controller test
drives and a deterministic canonical profile digest. Activating one must:

- display a persistent **Modded gameplay** badge;
- use a separate save/record namespace;
- mark or disable ordinary time-trial records and retail-compatible ghosts;
- enter simulation and rollback hashes;
- require identical profiles from every peer in an explicitly modded room;
- remain unavailable in ordinary matchmaking;
- never change because a portrait or model package was updated.

This preserves a welcoming creative workflow without letting a cosmetic import
silently change competitive authority.

## Rig and motion workspace

The implemented Rig Studio presents the human-readable role list beside each
exact skin-joint number and name. Inferred mappings show provenance and
confidence. Its actual bind-pose hierarchy has front/side projections, optional
helper joints, semantic-chain highlighting, spatial joint selection, and an
explicit assign-to-active-role action. The named controls remain the accessible
authoritative path. Vehicle target/pole overlays remain follow-up work.

Required humanoid roles for retargeting are hips, spine/chest/head, upper/lower
arm/hand and upper/lower leg/foot on each side. Seat and head remain required
presentation sockets. Non-humanoid packages can choose **Authored clips only**
and are not forced through a damaging humanoid solve.

Motion tests are organized by engine intent, not clip filenames. Every semantic
has Play, Loop/Once, Slow motion and Reset controls plus a fallback indicator.
Steering includes a -1/0/+1 scrub. The editor distinguishes no clip, static
clip, moving clip and moving-but-unmapped clip.

Reference motion uses reviewed role mappings and optional rest-axis
corrections. Hand/foot contact solving uses engine-owned vehicle targets,
package-local tuning offsets and optional bend preferences. Authored context
clips win; reviewed reference motion plus contacts covers missing semantics;
source fallback/bind pose is last and visibly incomplete. The current solver
bounds each CCD step but does not yet claim anatomical joint-limit profiles.

## Fit and vehicle workspace

Every context combines an exact renderer preview with a short purpose-built
control set:

| Context | Automatic anchor | Primary checks |
|---|---|---|
| Character select | bottom-center ground | feet on floor, faces camera, placard clear, full silhouette in frame |
| Car | pelvis/seat | faces track, body above chassis, hands near wheel, feet inside kart |
| Hovercraft | pelvis/seat | seat depth, hands/controls, skirt and wake clearance |
| Plane | pelvis/seat | cockpit depth, stick/wing clearance, rear-camera readability |

Drag manipulators and numeric fields stay synchronized. Reset affects only the
active context. Copy fit to another vehicle is explicit and undoable. A
before/after donor ghost, floor/seat axes, skeleton, bounds and anchor markers
are optional overlays. Camera orbit never changes saved character transforms.

## Performance workspace

The compiler reports vertices, triangles per LOD, skinned meshes, primitives,
materials, joints, active bone influences, texture dimensions/decoded VRAM,
animation memory, draw count and conservative skinned bounds. The UI gives each
category a tier and makes the overall tier equal the worst category. It also
states that this is a budget guide, not measured frame time.

Targets are **Quality**, **Balanced**, **Performance**, and **Four-player**.
They are assembly profiles, not a vague “detail” slider:

- inspect the exact fixed distance bands after package/local bias, clamping and
  sparse-level fallback; future schemas may author projected-size thresholds
  and hysteresis;
- choose texture transcode/maximum resolution once KTX2 is implemented;
- optionally build recorded offline simplifications;
- reject a target when no qualifying LOD exists rather than pretending the
  runtime can invent one;
- show estimated shared asset memory and per-player pose/palette memory;
- run a ten-racer/four-viewport stress scene and record actual GPU/CPU timing.

The current runtime LOD preference is useful only for packages with multiple
authored LODs. The launcher must say “one LOD; re-export or generate LODs” when
that control cannot improve performance.

The executable first assembly layer exposes the four targets as named starting
points over two honest inputs: local-player layout and a bounded shift of the
package's authored distance bands. It also keeps both inputs directly editable,
so a preset is never a ceiling. Near-view accounting uses the runtime's exact
distance thresholds, source plus local bias, clamping, and sparse-level fallback
to identify the selected authored LOD. Geometry and decoded textures remain one
shared upload while triangles, referenced vertices, draw submissions, and
current/previous pose palettes scale by the worst-visible `players × viewports`
instances. Performance history owns the layout and local LOD preference;
vehicle Fit history does not. Changing LOD policy invalidates exact timing
evidence but does not revoke an independently reviewed vehicle fit.

The interim workshop guide uses published, transparent bands while measured
device profiles are collected:

| Tier | Triangles | Vertices | Draw parts/materials | Joints | Textures |
|---|---:|---:|---:|---:|---:|
| Excellent | 15k | 20k | 4 / 4 | 64 | 8 |
| Good | 30k | 40k | 8 / 8 | 96 | 12 |
| Heavy | 60k | 70k | 12 / 12 | 128 | 16 |
| Near import limit | Above any Heavy threshold, within hard admission caps | | | | |

The overall label is the worst column. These bands are guidance and must be
replaced or split by device tier when stress-scene GPU/CPU evidence exists.

## Variants, audio, and effects

One identity may eventually own named visual variants such as alternate colors
or outfits. A variant may select package-authored material/mesh sets and its own
portrait recipe, but it cannot silently select another gameplay profile.
Variants share the base rig and semantic contract, declare their extra memory,
and are tested as distinct assemblies when geometry changes.

Voice, horn, celebration sounds and attached effects require the same explicit
licensing, decoded-cost accounting and bounded format policy as models. The
first release should offer only an engine-owned compatible donor fallback or
intentional silence; arbitrary audio and effect bundles should not be smuggled
through model extras. Future effect sockets (`head`, left/right hand, exhaust or
character-specific cosmetic points) remain presentation-only and must degrade
without affecting item or collision logic.

## Test workspace

The embedded exact-renderer preview supplies deterministic presets:

- select idle/hover/confirm;
- car/hovercraft/plane at rest, steer extrema, boost, damage, airborne/land,
  item, spin and finish;
- bright, dark, foggy and backlit lighting;
- 4:3, widescreen and split-screen safe areas;
- 1P and four-player performance scenes;
- LOD and animation slow-motion inspection.

Each preset records pass, fail, or not reviewed. **Auto checks** and **Author
review** are separate columns. A screenshot/contact sheet and machine-readable
test report can be exported without the imported model itself.

“Test in game” launches the exact selected preset and returns to the same draft.
It is not a generic boot button with undocumented menu navigation.

## Package and lifecycle

The final tab shows package contents, digest, compiler version, source/draft
changes, license and attribution before Build. It supports:

- Save Draft without making the character selectable;
- Build for This Computer;
- Export Portable Package;
- Compare Update with the installed version;
- Rebuild after engine/compiler changes;
- Reveal Source / Reveal Installed Files;
- Disable without deleting;
- Remove generated assembly while preserving source/draft;
- Remove everything with an exact, recoverability-aware confirmation.

Updates show identity, rig, gameplay relationship and cost differences. Existing
player assignments move only after the replacement builds and validates. The
last known-good assembly remains available if an update fails.

## Accessibility and input

- Complete keyboard and controller navigation, stable focus after rebuilds and
  removal, and no hover-only information.
- Screen-reader names include state and consequence: “Car fit, review required,
  opens preview.”
- Status never depends on green/red alone; icons also have text.
- UI scale to 200%, narrow layouts, localization expansion and reduced preview
  motion are acceptance requirements.
- Numeric controls permit direct entry and fine/coarse steps. Every drag action
  is undoable and resettable without affecting other contexts.
- Destructive actions state whether source, draft, assembly, assignments and
  screenshots are affected.

## Current implementation audit

| Capability | Current spike | Required product state |
|---|---|---|
| Secure GLB/package import | Mutation-free portable/source-only candidate staging, exact installed-versus-candidate identity/rig/donor/vehicle/LOD/animation/memory and authenticated SPDX/attribution/source comparison, installed provenance inventory, explicit legacy-cache state, local-rights confirmation, package/base optimistic reviewed commit, and package/GLB/DAE/ZIP/DCC native-picker plus drag/drop parity are executable. Every GLB acceptance path independently passes the exact bytes through the pinned, attested, bounded Khronos glTF Validator before the narrower MDKR policy/compiler boundary; the native validator and notices ship in macOS/Linux/Windows releases and the frozen importer. Failed intake creates a serialized private metadata-only recovery record with a bounded Khronos report, missing/changed-source state, mandatory exact-digest retry, new-import handoff, no-overwrite JSON export and source-preserving forget; merely opening the Workshop creates no quarantine directory. DAE and recursively nested ZIP intake requires an explicit new GLB destination, rejects traversal/symlinks/encryption/unsupported compression/oversize/per-member or aggregate expansion-ratio overflow/ambiguous models and every overwrite before costly member reads, converts one bounded DAE or extracts one character-ready self-contained GLB, reports absent license material, and enters the same raw workflow without changing the download. JSON glTF, FBX, OBJ, Blender, USD, Maya, 3ds Max and C4D sources route to a format-specific copyable GLB 2.0 checklist without executing, converting, drafting, publishing, or modifying them. Raw intake supports a 64-entry authenticated, atomically replaced source-draft library with durable selection and editor close/resume state, explicit switching/same-source branching/deletion, legacy singleton migration, exact per-model mapping fingerprints, reinspection after restart/switch/branch, draft-bound disposable review candidates, exact-draft cleanup after install, and external-source byte purity. Closing authoring or selecting an installed character never deletes a source draft. Each draft requires exact license bytes, a structurally parsed bounded SPDX expression, and explicit provenance/calibration/gameplay/mapping choices and emits only a deterministic source candidate for the ordinary review boundary. | Keep optional third-party adapters outside the trusted runtime boundary and add them only with explicit sandbox/signature/update policy |
| Height/front/ground/seat calibration | Executable v2 calibration plus direct per-context translation/yaw controls and exact game-renderer target-frame floor/seat/bounds/facing diagnostics retained across restart. The current editor plane and the full Test result render the authoritative calibrated volume, fitted ground/seat anchor after authored corrections, zero datum, and measured forward direction as focusable front/side/top overlays with an equivalent spoken/numeric description. Model-only captures carry an exact subject-scoped target-to-clip witness from the accepted WebGPU draw, quantized into bounded millipixel/depth records after PNG publication. The capture tray and self-contained report register the 12-edge fitted volume, anchor, and forward direction over the actual renderer pixels only when PNG digest, package source, fit revision, product, dimensions, player, viewport, and vehicle context agree. | Add representative occlusion and unusual-proportion review cases; never stretch or independently approximate a perspective capture |
| Per-context corrections | One package-keyed editor shared by every local assignment, with independent select/car/hovercraft/plane transforms; synchronized front/side/top ground-or-seat translation pads; a target-space yaw dial; numeric fields; per-plane reset/nudge; explicit undoable vehicle-fit copying; source-bound gesture undo/redo; source/fit-bound durable exact renderer measurements; a dynamic selected-plane overlay of the last matching exact measurement; and a session capture reference that prefers the newest exact front/side/top plane while clearly labelling an oblique fallback and preserving the author's model-only/composed product choice. A missing plane reference has a one-action handoff that prepares the matching Test camera and model-only capture without changing fit/package data. Top and underside are real racer-relative +/-90-degree pole cameras with a stable yaw-defined screen orientation, not relabelled oblique views. Stale fit evidence and stale capture pixels are never presented against changed controls. Exact model-capture registration uses both racer and authored-viewport ownership, so a four-player capture isolates the requested camera rather than conflating the same racer rendered through four views; the selected viewport is aspect-fitted into the capture target without distortion. | Add a donor reference silhouette under an explicit asset/occlusion contract |
| Animation semantics | Executable sampling/diagnostics plus source-v4 compiled role contract, inference provenance/confidence, transactional 16-role skin-joint editor with bind-pose hierarchy canvas, rest/bend controls and automatic review invalidation, native hierarchy validation, reviewed engine-reference fallback motion, bounded idempotent vehicle contact solving, and exact post-solve chain-root/bend/target/end witnesses | Add richer reference clips and joint-limit inspection; add an explicit pole witness only if the solver gains an authored pole rather than inventing one in UI |
| Portrait/roster identity | Source-v3 import, transactional Portrait Studio revision, exact 40x40 preview/editor with pencil/eraser/fill/eyedropper/mirror, numeric rectangular move/copy, tolerant palette replacement and source onion are executable. Portrait sources now include bounded 16-4096 px RGB/RGBA PNG decoding with strict CRC/chunk/APNG rejection, digest-bound square crop, crisp or premultiplied-area sampling, edge-connected matte removal, project-owned background frames, a non-destructive target-space subject mask with pointer and numeric-coordinate editing, and a direct handoff from either composed or transparent model-only exact-renderer Test captures. Identity undo/redo and v8 named drafts preserve source kind/digest/dimensions/recipe/mask plus the exact framed source and styled output without embedding or depending on the external PNG; old drafts migrate to an all-keep mask. The deterministic style recipe adds 16/32/64-colour median-cut palettes, ordered dithering, alpha cleanup, outlines, pinholes, textual quality checks, a responsive six-treatment exact-output comparison sheet, and a seven-view native/background/colour-vision readability proof. A shared bounded UTF-8 projection gives Workshop and live roster the same one-cell-per-unsupported-codepoint font fallback and discloses deterministic sort policy. HUD/results/rankings/minimap resolution, collection-arena racer flags, the independent 64-entry paginated select browser, all-or-nothing staged publication of sparse 1P-4P race plans (including disjoint four-asset replacement and last-stage failure rollback), explicit donor-owned ghost/save/network and scene-authored cinematic boundaries, and generated-package pixel/runtime proof across a real race, race-times page, and Fire Mountain flag are executable. | Add full localization-aware game-font shaping |
| Donor selection | All ten revision-1 donor seams are fingerprint-qualified and selectable through transactional source revisions; the engine publishes a bounded GPU-free installed-character catalog for virtual roster consumers; the launcher publishes exact bounded ROM-derived weight, handling, and vehicle-specific 14-sample acceleration evidence without retaining ROM bytes. A responsive keyboard/speech-selectable profile library now gives every donor a project-owned, colour-independent abstract badge and profile code; after ROM verification each card adds relative weight/handling bars and the selected vehicle's exact acceleration signature without using retail portrait art. A draft-aware authority card distinguishes package presentation, donor simulation/audio/ghost/network identity, ordinary save/record data, and future package negotiation. The current portrait consumers are audited: player-owned select/HUD/results/rankings/minimap/collection-flag surfaces resolve the package, while ghost menus, scripted cinematics, credits, audio and simulation intentionally retain retail/donor ownership. | Require every future character-indexed presentation surface to declare package, donor, or scene ownership |
| Vehicle support | Source-backed compatibility revision, runtime enable subset, independent transforms, persisted per-vehicle hand/foot target offsets, direct colour-keyed front/side/top contact-offset manipulators, warmed exact-context solve-count/mean/max contact-error feedback, exact target-frame seat/bounds/facing evidence, and renderer-backed root/bend/target/end/error proof for all four contacts in responsive front/side/top views. Crosses, endpoint shapes, connecting error lines, labels, keyboard focus, speech and an exact numeric table make the proof colour-independent. Missing automatic contact authority is explicit rather than fabricated. Stale-result invalidation, contextual one-click save/retest loops, source/tuning-bound per-context review, and a durable select/car/hovercraft/plane by 1P-4P exact-test matrix are executable. | Add representative course/vehicle-condition variants and human review of unusual limb proportions |
| Performance controls | Import caps; responsive Quality/Balanced/Performance/Four-player starting points; direct authored-LOD preference and 1P-4P layout controls; separate Performance history; exact runtime-equivalent source/local-bias, clamping and sparse-LOD selection; and a session-only exact distance scrubber with merged interval visualization, selected geometry counts, sparse-level disclosure, non-monotonic ordering warning and greater-than-4× transition warning. Exact selected-LOD draw/triangle/vertex/palette accounting, shared geometry/texture accounting, exact one-click WebGPU stress routes, and returned post-warm-up wall-cadence plus optional exact GPU timestamp distributions are executable. The initial gameplay pass is timed on devices with standard WebGPU timestamp queries; exact accepted character-draw ranges appear only where native in-pass timestamps exist. A six-slot nonblocking ring reports every exclusion and never fabricates unsupported scopes. One-LOD packages explicitly show one all-distance interval and refuse false optimization while retaining unrestricted import. Qualified latest results and explicitly pinned baselines survive restart with signed renderer fit, four-contact witnesses, and independently comparable wall/scene/character timing; they are bound to source, fit plus LOD policy, result contract, app build, presentation settings, physical output/render size and GPU/driver identity. Authenticated v1/v2/v3 evidence migrates to v4 without changing its established file location. The target cards, saved bias, distance inspector, and timing rows are keyboard/speech qualified at 200% scale. | Add representative scene variants, a maintained device-profile corpus, projected-size thresholds/hysteresis and optional recorded offline simplification |
| Gameplay tuning | Correctly absent from visual package | Build separate opt-in hashed gameplay-profile system |
| Preview | One-click typed requests launch select or all three vehicles through real game initialization in any 1-4P layout after ROM revalidation; requests are assignment-neutral and support all 13 select/race semantics held at an exact normalized phase. Zero yaw/pitch preserves the ordinary gameplay view; nonzero views are bounded absolute racer-relative orbits around renderer-published fitted bounds, use vehicle/split-screen-aware pull-back, suppress transient cutscene-camera selection, and retain ordinary obstruction resolution. The inclusive -90..90 pitch contract supplies exact top and underside pole views with yaw retained as the stable screen orientation. Select keeps its authored camera. Four deterministic character-only lights preserve world/simulation state. Pose inspection is counted after warm-up, usable as exact session fit proof, saved in Test history and v8 named drafts, and explicitly excluded from durable performance evidence. A one-shot exclusive PNG waits for 12 consecutive fully rendered character/pose/view/light frames after warm-up, then explicitly produces either a composed RGB gameplay frame or a straight-RGBA model-only image from an isolated WebGPU replay that excludes the donor, vehicle, world, and HUD. Model-only capture retains the accepted subject/player, authored viewport, target transform, camera MVP, normalized output viewport/scissor, and primitive count; result v13 publishes only bounded fixed-point projections for eight fit corners, the anchor, and the forward endpoint. Successful results enter a source/fit/product/digest-bound session tray with bounded inline registered overlays, can be sent directly to Portrait Studio through an exact-digest handoff, and export as a self-contained schema-v3 HTML/JSON contact sheet with no model/ROM/path or raw matrix bytes. Thumbnail decode, portrait decode, and export accept only the bound digest; an already-cached thumbnail remains the original captured image if the external path later changes. Live tests return a warmed v13 result with exact device/physical render identity, optional exact GPU scope/distribution evidence, target-frame anchor, calibrated fitted bounds, floor clearance, normalized forward evidence, exact model-capture registration, and complete post-solve contact chains from the successful replacement draw; they persist source/tuning-bound per-context author review plus the bounded 4x4 latest/baseline evidence matrix. Generated authored-only and reviewed-humanoid packages pass the complete WebGPU route/pixel/composition/transparency/stress/result/contact-witness/timestamp gate, including closed 3D top/underside composition. | Embed the renderer and add representative scene variants |
| Packaging/update/removal | Mutation-free review plus package/base-bound transactional install/update, responsive installed-versus-candidate diff, reversible runtime disable, authenticated revision restore/source export, current-compiler portable export, transactional current-source rebuild, and recoverability-aware deletion are executable. Portable publication refuses overwrite and does not mutate installed state; rebuild preserves enabled state and the last known-good cache on failure. Named full-editor snapshots are bounded, authenticated, atomically persisted, independently resumable/deletable, exact-base locked, and autosaved. A combined optimistic build publishes portrait/profile/rig as one retained revision; launcher-owned fit is separately disclosed and confirmed. A dedicated responsive launcher destination now owns a persistent library rail, seven editor tabs, named readiness rows, a deterministic best-next-action resolver, persisted package/tab selection, safe DAE/ZIP-to-GLB handoff, a multi-draft pre-package GLB authoring library, and independent bounded history for Identity, Profile, Rig, Fit/review, Performance, and Test setup; Settings retains its shortcut and assignment summary. | Add bulk draft export/import after the portable-package sharing contract is finalized |
| Accessibility | Dedicated panel/tab/library/import/assignment controls carry spoken names, textual status, keyboard/controller navigation, and a narrow single-column fallback. Spatial placement/yaw/contact canvases expose spoken values plus adjacent navigable nudge/reset and exact numeric controls, and reflow instead of forcing a wide canvas. The exact-test matrix, baseline/destructive controls, semantic pose selector, phase scrubber, camera presets/sliders, character light, exclusive PNG request, capture tray, report export/removal actions, and all four responsive inspection actions have a rendered 200% keyboard/speech lifecycle gate. Shared cards flatten keyboard navigation so nested actions remain reachable. | Qualify remaining freeform editor gestures with human controller/screen-reader review |

The secure-import baseline now preflights every GLB buffer, buffer view, and
accessor for exact bounds, alignment, stride, type, count, and consumed binary
facts before source height, anchoring, animation, skinning, or compiler byte
access. Declared POSITION bounds must equal the actual float payload, preventing
false metadata from translating an otherwise valid character through the floor;
scene cycles, unrepresentable shear, invalid material references, and corrupt or
dimension-lying embedded PNGs fail through the same actionable report. The
compiler independently repeats storage and hostile-type checks.

## Delivery sequence and gates

### W0 — Workshop shell and draft model (shell/readiness baseline complete)

The package-keyed named draft, full bounded snapshot, autosave/resume/delete,
exact-base protection, combined source build, last-selected package, and one
editor independent of P1-P4 assignments now live in a dedicated responsive
launcher destination. Wide layouts use independently scrollable library and
editor surfaces; narrow layouts preserve the same tools in one column. Seven
persisted tabs, six named readiness rows, distinct preview/play verdicts, and a
pure tested best-next-action resolver preserve the safe importer/runtime
boundary. An explicit no-overwrite DAE/ZIP-to-GLB handoff covers ordinary
download archives before raw authoring. Authenticated exact-source and
current-compiler portable export sit
beside revision restore; current-source rebuild uses the same optimistic,
last-known-good transaction as revision activation. Identity, Profile, Rig,
Fit (including exact-context review), Performance assembly, and Test setup now
have independent 32-step histories. Continuous typing, paint, color, and drag
gestures coalesce into one step; each track is capped at 512 KiB, bound to the
exact active source digest, cleared across draft/source lifecycle boundaries,
and applied without changing installed bytes. A persistence failure does not
consume the step. Complete the remaining 200%/controller qualification matrix.

Gate: import, resume, edit, assign, disable and remove are understandable at
320x568 and 200% text with keyboard/controller only; no edit is lost or applied
to a different package.

### W1 — Exact launch preview and Portrait Studio (baseline complete)

The exact-game preview request/result path, deterministic portrait import,
40x40 pixel editor, source-preserving style recipe and quality analysis,
versioned identity media, and dynamic local identity resolver across select,
HUD, results/rankings portraits and minimap are implemented. Vehicle-camera
orbit, character-only lighting, composed gameplay-frame and transparent
model-only stabilized PNG products, and self-contained schema-v3 qualification
contact sheets are implemented through the exact renderer. Test captures hand
directly to bounded Portrait Studio crop, edge matte, freeform subject mask,
background, style, six-treatment comparison, readability proof, and pixel
stages. An embedded renderer and localization-aware game-font shaping remain.
The launcher-owned Test
workspace now exposes every supported select/race semantic plus a normalized
phase scrubber. Its typed request survives ROM revalidation, is applied inside
the real animation runtime, returns post-warm-up inspected and fallback tick
counts, names an unavailable semantic/source-fallback result instead of falsely
claiming exact phase control, and is
retained by Test undo/redo plus versioned named drafts. Inspection results are
session-only by contract, never enter the performance matrix or pinned
baselines, and fail closed for unknown semantics, unsafe phases, or unpaired
environment fields. Every exact context has an
independent persisted author-review action which is unlocked only by a warmed
test that actually rendered the replacement; its canonical signature prevents
source or tuning changes from inheriting stale approval.

Gate: no donor name/portrait leaks on any audited identity surface; every output
is deterministic and readable at original 320x240 presentation.

### W2 — Rig review, reference motion and contacts (baseline complete)

The transactional role-map review UI, compiled source-v4 contract, bounded
engine-owned reference poses, runtime rest-basis corrections, authored-motion
precedence and idempotent CCD vehicle contacts are implemented. Richer
reference animation, spatial vehicle target/pole overlays, and anatomical limit
profiles remain.

Gate: humanoid fixtures of different scales/proportions pass select and all
three vehicle tests without T-pose, mirrored limbs, knee/elbow inversion or
seat drift; non-humanoid authored-only fallback remains intact.

### W3 — Performance assemblies (structural and cadence baseline complete)

Exact per-LOD structural accounting, runtime-equivalent near-view LOD selection,
source plus local bias disclosure, responsive named targets, directly editable
one-to-four-player assemblies, independent bounded Performance history, real
WebGPU stress routes, post-warm-up wall cadence, and optional exact GPU
timestamp distributions are implemented. Standard query support brackets the
initial scene pass; native in-pass support additionally brackets each accepted
custom-character draw, with explicit unavailable scope elsewhere. A six-slot
nonblocking readback ring reports pending, ring-full, and invalid exclusions. A
bounded durable 4-context by 4-layout matrix retains exact-source/fit/LOD latest
results and qualified pinned baselines. Changing only LOD policy makes existing
timing evidence stale without invalidating vehicle-fit review. It refuses
cross-build, cross-result-contract, cross-presentation, cross-resolution or
cross-device deltas and preserves malformed storage byte-for-byte.
Representative scene variants, transition visualization, a maintained device
profile corpus, projected-size LOD hysteresis and optional recorded offline
simplification remain.

Gate: the displayed counts equal GPU allocations; target builds remain inside
published device budgets and fail explicitly when they cannot.

### W4 — Qualified donor library and virtual roster (baseline complete)

All ten donor seams are fingerprint-qualified, and the independent paginated
custom-racer browser resolves package identity plus donor without extending
retail ten-wide simulation tables. Generated-package race/minimap/results and
collection-arena flag pixel proof, plus explicit donor-owned ghost/save/network
and scene-owned cinematic/credits boundaries, complete the current
identity-surface audit. Exact transparent model captures can now seed Portrait
Studio. The donor library uses project-owned abstract metric badges rather than
retail portraits and remains usable before exact ROM-derived evidence is
available. Localization-aware game-font shaping remains.

Gate: multiple packages may share a donor, four players can select distinct
virtual identities, and retail saves/ghosts/online authority remain unchanged.

### W5 — Optional gameplay mod profiles

Only after visual identities ship, implement canonical bounded physics profiles,
modded records/saves, explicit room negotiation and comparison/test-drive UX.

Gate: it is impossible for a visual package or ordinary room to activate custom
physics; every peer hashes identical canonical bytes before simulation.

## Definition of excellent

The workflow is not excellent because it has many controls. It is excellent
when a first-time user always knows what is wrong, what the next safe action is,
what will change in the game, what will remain the donor's responsibility, and
how to undo it—while an expert can inspect exact bones, matrices, budgets,
digests and test evidence without leaving the same workspace.
