# Custom Character Workshop product specification

Status: product architecture and phased acceptance plan, 2026-08-26.

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

### Portrait inputs

The Portrait Studio offers three reversible starting points:

1. **Capture model** using the exact WebGPU character renderer.
2. **Import image** from a local PNG with alpha.
3. **Draw pixels** on a 32x32 working canvas.

The model capture exposes semantic pose, normalized animation time, camera yaw,
pitch, projection, framing, lighting preset, background and expression when the
rig supports one. A head socket supplies the initial camera target; the user can
always reframe it. Capture runs locally and stores a deterministic recipe plus
the generated result.

The pixel editor provides pencil, eraser, fill, eyedropper, lasso/move,
horizontal mirror, palette replace, undo/redo, onion comparison against the
source capture, and keyboard/controller-accessible numeric color entry. It is a
small purpose-built editor, not a general paint application.

### DKR style conversion

“DKR-style” is a deterministic filter stack with an always-visible before/after:

- crop and subject-safe area;
- optional silhouette cleanup and 1–2 pixel outline;
- fixed-size downsample using nearest, box, or area sampling;
- selectable 16/32/64-color quantization;
- optional ordered dithering with strength control;
- background gradient/frame presets derived from project-owned values;
- final 32x32 author canvas and fixed-point 40x40 game expansion.

Nothing invokes a network service or claims to create copyright-safe art. A
generated image can be edited pixel-by-pixel. The UI previews 1x native size,
4x nearest-neighbor, HUD, results, character select, light/dark backgrounds and
color-vision simulations. It warns about transparent holes, unreadable
silhouettes, clipped hair/ears, low face occupancy, excess colors, and contrast.

One approved composition generates every required derivative; users should not
have to align five nearly identical files independently. Packages may instead
supply validated final PNGs plus attribution when an artist already created
them.

## Gameplay profile and kart selection

### Visual-only release

The default product is explicit: **Appearance: custom; gameplay: built-in**.
The user selects from fingerprint-qualified donor profiles, not raw character
IDs. A profile card shows:

- portrait/name of the built-in gameplay character;
- weight, handling, acceleration and top-speed comparison bars sourced from the
  game tables;
- voice/horn/effect fallbacks;
- car/hovercraft/plane qualification state;
- records, ghosts, saves and online status;
- why a profile is unavailable.

The implementation exposes qualified geometry seams for every retail donor.
Profile cards can therefore present all ten choices without a hidden Diddy-only
gate; an unavailable or changed ROM schema still fails visible and retains the
built-in actor.

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
confidence. A spatial skeleton view, selection highlighting, and target/pole
overlays remain explicit follow-up work; the current editor does not pretend a
combo list is a visual rig debugger.

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

- choose authored LOD thresholds and preview each transition;
- choose texture transcode/maximum resolution once KTX2 is implemented;
- optionally build recorded offline simplifications;
- reject a target when no qualifying LOD exists rather than pretending the
  runtime can invent one;
- show estimated shared asset memory and per-player pose/palette memory;
- run a ten-racer/four-viewport stress scene and record actual GPU/CPU timing.

The current runtime LOD preference is useful only for packages with multiple
authored LODs. The launcher must say “one LOD; re-export or generate LODs” when
that control cannot improve performance.

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
| Secure GLB/package import | Executable | Preserve, add resumable draft and inventory screen |
| Height/front/ground/seat calibration | Executable v2 | Add exact embedded preview and manipulators |
| Per-context corrections | Executable | Move from four repeated player panels to one package editor |
| Animation semantics | Executable sampling/diagnostics plus source-v4 compiled role contract, inference provenance/confidence, transactional 16-role skin-joint editor with rest/bend controls and automatic review invalidation, native hierarchy validation, reviewed engine-reference fallback motion, and bounded idempotent vehicle contact solving | Add richer reference clips, skeleton visualization, target overlays and joint-limit inspection |
| Portrait/roster identity | Source-v3 import, transactional Portrait Studio revision, exact 40x40 preview/editor with pencil/eraser/fill/eyedropper/mirror/undo, HUD/results/rankings/minimap resolver, and independent 64-entry paginated select browser with per-player portrait/name identity executable | Add model capture, advanced selection/style tools, localization-aware game-font shaping, and remaining identity-surface audit |
| Donor selection | All ten revision-1 donor seams are fingerprint-qualified and selectable through transactional source revisions; the engine publishes a bounded GPU-free installed-character catalog for virtual roster consumers | Add game-table comparison bars and exact-context review status |
| Vehicle support | Source-backed compatibility revision, runtime enable subset, independent transforms, persisted per-vehicle hand/foot target offsets, warmed exact-context solve-count/mean/max contact-error feedback, stale-result invalidation, and contextual one-click save/retest loops | Add visual target manipulators and complete vehicle test matrix |
| Performance controls | Import caps, authored LOD bias, exact per-LOD draw accounting, 1P-4P worst-visible assembly counts, exact one-click WebGPU stress routes, and a returned post-warm-up wall-cadence percentile/result card with synthetic/short-sample refusal | Add GPU timestamps, repeatable comparison baselines, representative scene matrix and device profiles |
| Gameplay tuning | Correctly absent from visual package | Build separate opt-in hashed gameplay-profile system |
| Preview | One-click typed requests launch select or all three vehicles through real game initialization in any 1-4P layout after ROM revalidation; requests are assignment-neutral, return a warmed structured result, and a generated non-Diddy package passes the complete WebGPU route/pixel/stress/result gate | Embed the renderer, add semantic pose/camera/lighting controls, screenshots/contact sheets and persist per-context author review |
| Packaging/update/removal | Executable | Add draft lifecycle, update diff, disable and source-preserving cleanup |
| Accessibility | Inherits launcher fundamentals | Qualify every editor tool at keyboard/controller/200%/screen reader |

## Delivery sequence and gates

### W0 — Workshop shell and draft model

Create the dedicated library/editor layout, package-keyed draft with undo/redo,
one editor independent of P1-P4 assignments, readiness model and next-action
resolver. Keep the existing safe importer and runtime unchanged.

Gate: import, resume, edit, assign, disable and remove are understandable at
320x568 and 200% text with keyboard/controller only; no edit is lost or applied
to a different package.

### W1 — Exact launch preview and Portrait Studio (baseline complete)

The exact-game preview request/result path, deterministic portrait import and
40x40 pixel editor, versioned identity media, and dynamic local identity
resolver across select, HUD, results/rankings portraits and minimap are
implemented. An embedded renderer, model capture, style conversion, camera and
lighting controls, and contact overlays remain.

Gate: no donor name/portrait leaks on any audited identity surface; every output
is deterministic and readable at original 320x240 presentation.

### W2 — Rig review, reference motion and contacts (baseline complete)

The transactional role-map review UI, compiled source-v4 contract, bounded
engine-owned reference poses, runtime rest-basis corrections, authored-motion
precedence and idempotent CCD vehicle contacts are implemented. Richer
reference animation, a spatial skeleton/target view, and anatomical limit
profiles remain.

Gate: humanoid fixtures of different scales/proportions pass select and all
three vehicle tests without T-pose, mirrored limbs, knee/elbow inversion or
seat drift; non-humanoid authored-only fallback remains intact.

### W3 — Performance assemblies (structural and cadence baseline complete)

Exact per-LOD structural accounting, authored LOD control, one-to-four-player
worst-visible assemblies, real WebGPU stress routes and post-warm-up wall
cadence results are implemented. GPU timestamps, representative scene/device
baselines, projected-size LOD hysteresis and optional offline simplification
remain.

Gate: the displayed counts equal GPU allocations; target builds remain inside
published device budgets and fail explicitly when they cannot.

### W4 — Qualified donor library and virtual roster (baseline complete)

All ten donor seams are fingerprint-qualified, and the independent paginated
custom-racer browser resolves package identity plus donor without extending
retail ten-wide simulation tables. Richer profile comparison cards and a
complete remaining identity-surface audit remain.

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
