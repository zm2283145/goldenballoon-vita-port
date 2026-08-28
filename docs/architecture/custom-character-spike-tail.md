# Custom character spike: remaining execution tail

Status: evidence-backed implementation scope after the first external-character
proof. This plan complements `custom-character-pipeline.md` and
`custom-character-workshop-ux.md`; it does not weaken their security or durable
publication contracts.

## Definition of done

The spike is complete when a person can start with a legally usable ordinary
artist deliverable, reach a polished local character without editing JSON, and
understand every decision that still needs human judgment. The same package
must remain correct in character select, car, hovercraft, plane, one-to-four
player layouts, portraits, HUD/results/minimap surfaces, saves, ghosts, and
local/online simulation authority.

“Correct” means all of the following:

- the source is self-contained, validated, attributed, and never bundled by the
  project without a verified redistribution license;
- forward/up/units, standing height, ground contact, seat placement, and each
  vehicle correction are visibly reviewed against exact renderer output;
- select and all race semantics use an intentional authored clip or a reviewed
  engine-reference pose—never an accidental bind pose;
- the head and torso sit in the ordinary camera envelope, intentional vehicle
  occlusion is distinguishable from floor/kart intersection, and contact error
  is quantified per limb;
- portrait generation begins with a tightly framed isolated subject and ends
  with a readable exact 40x40 result;
- a failed build, update, test, export, or deletion leaves the previous playable
  character and recoverable author work intact;
- unrestricted input remains possible within documented safety ceilings, while
  device-tier warnings and measured evidence make expensive content an informed
  choice rather than an arbitrary rejection.

## Example-asset evidence

The user-provided `dixie-kong.zip` was processed locally and is not committed or
bundled. The archive itself contains no license file and its nested source name
references Mario Kart Tour, so the proof uses
`LicenseRef-UserProvided-ProofOnly`; redistribution remains blocked pending
independent rights verification.

The real pipeline successfully:

1. recursively inspected the ZIP and selected its one convertible COLLADA model;
2. converted Z-up COLLADA with a declared `0.01` meter unit into a self-contained
   GLB;
3. authenticated the GLB with the pinned Khronos validator;
4. compiled and transactionally installed source revisions;
5. corrected explicit forward from `+z` to `-z` using renderer evidence;
6. added a local portrait/minimap identity revision;
7. validated and installed a reviewed 16-role humanoid map; and
8. rendered character select, car, hovercraft, plane, composed inspection, and
   isolated model products through the real WebGPU/game paths.

Measured input/runtime facts:

| Fact | Result |
|---|---:|
| Vertices / triangles | 2,517 / 3,489 |
| Skin joints | 35 |
| Materials / embedded textures | 2 / 4 |
| Authored animation | one generated static `idle` |
| Scene-world height | 0.0144802 m |
| Requested standing height | 1.25 m |
| Runtime source schema after review | v4, 16/16 roles reviewed |
| Contact maximum, car / hovercraft / plane | 176 / 180 / 146 mm |
| Pre-fix isolated-capture subject height | 266/1,920 px (13.85%) |

The proof exposed six release-blocking authoring defects:

1. `+z` initially showed the character from behind. Geometry alone could not
   safely infer the semantic front.
2. The initial proof's explicit `select.idle` mapping points to a generated
   static bind pose. Because authored mappings correctly outranked fallback
   motion, select remained in a T pose even after the rig was reviewed. The
   pipeline now preserves a reversible author decision to disable that mapping
   and use reviewed reference motion; refreshed private-model visual evidence
   remains part of this milestone's acceptance pass.
3. Name-only rig inference selected `Hip`, but the actual skeleton has `Hip`
   and `Spine1` as siblings under `Skl_Root`. The native hierarchy validator
   refused it; `Skl_Root` is the structurally valid common hips role.
4. Identity rest rotations and bend axes have not been calibrated. The reference
   solver removes the T pose, but hands/feet and the plane arm remain visibly
   imperfect.
5. One zero-offset seat transform does not fit all three vehicles. Race views
   show mostly the head, and exact limb-target residuals reach 146–180 mm.
6. Isolated/model and composed inspection framing is too conservative for fast
   diagnosis or portrait handoff on this unusual source transform.

These are pipeline/UX findings, not Blender-skill findings.

The 2026-08-27 private refresh rebuilt the same GLB as compiler-v7 with the
static `select.idle` mapping preserved and disabled. The report records one
static animation, active semantic mask `fallback`, disabled semantic mask
`select.idle`, and a reviewed 16-role humanoid map. The exact linked-ROM select
route rendered 200 held `select.idle` inspection ticks with zero source-fallback
ticks: Dixie is grounded, front-facing, and no longer in the authored T pose.
The exact car route likewise rendered 200 held steering ticks with zero source
fallback; it still reports the known 176 mm maximum contact residual, correctly
leaving vehicle fit/rest-basis work incomplete. Local, unbundled review images
are `13-select-disabled-reference.png`, `14-car-disabled-reference.png`, and
`15-animation-intent-studio.png` under the ignored Dixie evidence directory.

## Ordered execution plan

### M0 — Make the example reproducible without shipping it (small)

- **Implemented:** add a checked-in, license-clean generated CC0 humanoid that exercises
  sibling pelvis/spine roots, non-meter units, hair chains, multiple materials,
  an animationless skin using compiler-v8's explicit bind fallback, and unusual
  proportions. The user-provided model remains a
  private manual arm.
- **Implemented:** add a one-command local evidence runner accepting `--source`, `--license`,
  `--rom`, and `--evidence-dir`. It must run conversion, intake, reviewed local
  installation, all context tests, and a contact/performance summary without
  copying the source into the repository.
- **Implemented:** emit a redacted evidence manifest containing digests, tool versions, decisions,
  metrics, and screenshot names but no model, ROM, absolute source path, or
  claimed license not present in the input.

Acceptance: a clean checkout can run the licensed fixture automatically; a
private source can run manually; neither path modifies the normal user library.

The deterministic fixture proof now covers select, car, hovercraft, plane, and
four-player car through the exact linked-ROM WebGPU routes. It has 504 vertices,
252 triangles, 20 joints, three materials, two embedded textures, a reviewed
16-role map, and one deliberately static clip whose `select.idle` mapping is
preserved but disabled. The fixture's explicit `+z` front is visible in select
and its hair/back is visible from the race chase camera. All five contexts have
zero package-fallback pose ticks and meet the initial frame-time target on the
development host under a warmed, real-time enhanced-cadence window. Screenshot
publication runs separately so filesystem work cannot contaminate p95/p99. The
intentionally uncalibrated contact residuals remain roughly 277–298 mm; the
evidence runner records those as M4 advisories instead
of misreporting a successful package as an invalid import.

The runner creates conversion, package, install, and disposable character
catalog state under the operating-system temporary directory. Its caller-chosen
evidence directory receives only redacted logs, PNG captures, and
`evidence.json`; it refuses to overwrite a nonempty directory. A private source
requires the pinned native validator and a reviewed
`mdkr-character-spike-decisions-v1` file. The synthetic validator seam is
explicitly restricted to the generated fixture and exists only for CI.

### M1 — Guided orientation, scale, and anchor calibration (medium)

- **Implemented:** present aggregate mesh-local bounds, scene-world bounds, normalization
  multiplier, and suspicious-transform warnings together. A 1.45 cm scene-world
  height beside roughly human mesh-local proportions must read as “review
  source transform,” not merely a generic small-height warning.
- **Implemented:** bind an explicit scale/facing acceptance to the exact model
  fingerprint, chosen forward axis, and target height. Any one of those changing
  clears acceptance; the source GLB remains byte-exact and the source-package
  build stays unavailable until the revised proposal is accepted.
- **Implemented:** expose all four front candidates (`+z`, `-z`, `+x`, `-x`) as
  equal, keyboard/speech-accessible choices rather than hiding them in a
  dropdown. The UI labels them honestly as coordinate-axis choices pending
  exact visual confirmation.
- **Implemented:** the post-compile Facing Studio prepares four exact,
  model-only `+z`, `-z`, `+x`, and `-x` side captures and presents only
  source/fit-matching results as equal thumbnails (reflowing to one column at
  compact width). Choosing the recognizable face applies a reversible overall
  yaw correction and invalidates every old fit/capture; geometry is never used
  to infer front and the GLB remains byte-exact.
- **Implemented:** propose a source/fit-bound vertical ground or conservative
  seat translation plus measured facing correction from exact renderer evidence.
  Applying is an explicit, reversible Fit-history edit and always invalidates
  the evidence until the author reruns that context.
- **Implemented:** open the exact select/car/hovercraft/plane scene as a live,
  responsive Offset Studio. Overall and per-context corrections publish through
  the validated runtime tuning seam on the next frame while the scene continues
  to simulate and editor input cannot reach gameplay. Save failure restores the
  prior runtime and staged configuration; return waits for a post-edit warmed
  diagnostic frame and preserves editor runs as fit evidence rather than clean
  performance approval.
- **Implemented:** fit each context independently. The guided sequence never
  treats a car/hovercraft/plane correction or review as evidence for another;
  an explicit vehicle-only copy action remains available for intentional reuse.
- **Implemented for the complete scene-review contract:** visible advisory bands classify floor/seat
  datum error, facing, and unusual volume proportions as Ready, Review, or
  Action needed. Result v22 also retains the last complete accepted scene MVP,
  viewport, and scissor and projects the calibrated volume plus exact
  current-pose hips/chest/head node origins into bounded fixed-point camera
  evidence. Offset Studio reports camera occupancy, clipping, head placement,
  torso placement, and seat-to-hips displacement across restart. For every
  vehicle, one resumable action now runs all eleven race samples on five
  qualified courses: open baseline, dense scenery, alternate climate,
  dark/enclosed visibility, and effects-heavy presentation. Each held
  generation snaps to its target sample, publishes
  only after the runtime reports it settled plus sixty replacement draws (a
  deliberate human-visible inspection dwell), and
  returns automatically with aggregated framing, retained-body surface,
  topology-qualified containment, opaque-depth visibility, and contact evidence.
  A game-native status card keeps the active course, semantic, stage, and
  held-draw progress visible throughout; Escape/F1 or gamepad Back opens a
  review-specific stop action that discards only the incomplete course and
  preserves every completed course for resume.
  Approval requires all current source/fit/LOD/presentation-bound course
  batteries and reopens every legacy approval once. Select approval covers
  idle/hover/confirm; every vehicle approval covers 55 samples in total.
  Stable game-owned scopes now distinguish retained body, vehicle-part sprites,
  and held objects and carry their presence, replay qualification, and in-front
  overlap into the per-state table and explicit warning acknowledgement. This
  intentionally does not identify an individual wheel/prop sprite or claim
  scene-owned boss, battle, or cinematic presentation as package-owned.

Acceptance: the example faces the camera in select, its feet are floor-aligned,
and its head/torso land inside each ordinary vehicle camera without manual JSON.

### M2 — Rig inference by structure and rest basis (large)

- **Implemented:** score role candidates using bounded skin membership and
  normalized names, then validate the complete canonical ancestor graph.
  A bounded structural fallback can resolve a clean unnamed humanoid only when
  it has unique bilateral hand/foot leaves, a unique central head leaf,
  mirrored height/span agreement, and distinct LCA/path chains for every role.
  Close candidates, hair/finger/toe continuations, cycles, asymmetry, or
  duplicate names without decisive structure remain unresolved instead of
  being selected by list order.
- **Implemented:** detect the sibling-pelvis pattern and propose the lowest
  common skin-joint ancestor of the torso and both legs as hips. The manifest
  decision report retains per-role provenance; native Rig Studio presents the
  same source-bound proposal, confidence, and rationale before a reversible
  apply action. Applying always clears review.
- **Implemented:** derive every role's canonical-to-joint-local rest correction
  from the full normalized bind-orientation chain and the exact reviewed
  `source_forward` convention. Rig Studio discloses whether each basis was
  derived, applies it reversibly, exposes its exact quaternion, and offers a
  per-role restore action after manual edits. A one-click role proposal is
  unavailable unless all 16 rest bases are valid.
- **Implemented:** derive joint-local bend preferences only from stable bind-pose
  limb planes. Near-straight or degenerate chains remain visibly automatic;
  the runtime now converts an authored joint-local fallback into parent space,
  matching the documented contract instead of interpreting it in the wrong
  frame. Exact bind-relative per-role travel visualization is now implemented;
  optional authored anatomical constraints must build on that evidence rather
  than inventing a generic limit.
- **Implemented:** provide a source-bound five-region anatomy checklist that
  clears with mapping/basis edits, participates in undo/redo, and survives
  named-draft resume (v10 with reviewed-draft migration). Provisional solver
  approval is unavailable until all five regions are checked. Five one-click
  motion-battery presets hand off to exact Test without claiming a pass; the UI
  explicitly asks authors to exercise select and every supported vehicle after
  saving. “16 roles present” no longer implies “pose quality approved.”

Acceptance: inferred mappings compile on the first structurally valid proposal,
all corrections remain reviewable, and the complete pose battery has no limb
flip, collapse, or mirror inversion.

### M3 — Intentional animation-semantic precedence (medium)

- **Implemented:** let authors disable or unmap a bad source clip without deleting it. Make the
  choice among authored clip, reviewed reference motion, and static fallback
  explicit for every one of the 13 semantics.
- **Implemented:** detect clips with no meaningful joint motion. A generated static `idle` mapped
  to `select.idle` must be labelled “bind/static-looking” and must not silently
  win over a reviewed reference select pose.
- **Implemented:** Animation Studio exposes exact 0/50/100% held samples and a
  reversible A/B semantic review. A/B alternates through the ordinary runtime
  pose player, measures each destination's authored blend duration, and reports
  authored clip, reviewed reference, or package fallback independently for both
  ends. Moving reviews cannot create a misleading nondeterministic screenshot;
  held samples feed the existing digest-bound side-by-side capture tray.
- **Implemented:** expand the bounded reference library so select idle,
  hover/confirm, steering, reverse, boost, damage, item, spin, airborne/land,
  and win/lose finish states each evaluate to a distinct skeleton pose. A
  generated articulated CC0 humanoid exercises readable select and race
  silhouettes through the linked-ROM WebGPU capture route. This qualifies the
  pipeline, not universal artistic quality for every imported rig.
- **Implemented as non-prescriptive inspection:** every exact held sample and
  complete semantic battery publishes the shortest bind-relative node-local
  angular excursion for all 16 reviewed humanoid roles after reference motion
  and contact solving. Animation Studio shows the peak role per state and a
  focusable, spoken per-role matrix. Partial, non-finite, and out-of-range
  publications fail closed. No generic anatomy threshold is presented as a
  pass, limit, or runtime clamp.
- **Runtime contract implemented:** source-v5 strictly validates optional
  cone/twist profiles and bounded hair/tail chains; compiler-v9 emits typed
  MDKC-v2 records, native admission rechecks their graph and numeric bounds,
  unchanged rig edits preserve limits, remaps invalidate them, and package
  review discloses all three counts. Bind-relative constraints run before and
  after vehicle contacts; secondary motion uses a bounded fixed-step spring,
  explicit hitch resets, and exact held-sample resets without weakening
  authored-clip precedence. The matching direct-manipulation Studio remains.

Acceptance: the example no longer T-poses in select, every missing authored state
uses an intentional reviewed reference, and the source/fallback decision appears
in the package review and durable evidence.

### M4 — Per-vehicle seating and contact-assist workflow (large)

- **Implemented:** add “Fit all enabled contexts” as a guided sequence, not one
  opaque automation. It shows independent current-review state for Select, Car,
  Hovercraft, and Plane, then routes to the next incomplete context. The exact
  preview/camera-layout step now reflows at compact width instead of disappearing
  for handheld or large-text users.
- **Implemented conservatively:** the per-context sequence proposes only the
  exact-evidence vertical and facing corrections that can be justified from the
  fitted volume, previews exact gameplay, and then offers all four measured
  contact corrections per vehicle. X/Z and scale remain direct author controls
  instead of being guessed from insufficient evidence.
- **Implemented for the current pose:** the fit review card shows exact
  calibrated camera occupancy, head/torso points, seat-to-hips displacement,
  floor/seat datum, hand/foot reach, retained-body surface intersections,
  topology-qualified bounded containment, and an isolated-versus-final-depth
  visibility grid. It does not mislabel 2D overlap as penetration or claim one
  pose covers motion.
- **Implemented:** the current exact result or complete 55-state battery reduces
  root/bend/target/end witnesses to the constant least-squares endpoint-minus-
  target correction for each limb. Offset Studio shows millimetre deltas and
  sample counts, refuses the one-click proposal outside the +/-1 m safety
  envelope, changes nothing before acceptance, records one reversible Fit edit,
  and requires exact retesting afterward.
- **Implemented for the qualified settled windows:** hand residual <=25 mm and foot residual <=40 mm
  are visible exact-evidence guides. An over-limit review now requires an
  explicit source-and-fit-bound exception, participates in Fit undo/redo,
  survives named-draft resume, and rolls back if persistence fails. Every
  procedurally solved state also records at least eight consecutive changes in
  endpoint-minus-target after settling, reports the maximum residual drift per
  limb, and uses a disclosed 2 mm starting guide. This deliberately excludes
  ordinary whole-pose or vehicle motion. Older approvals reopen once under the
  stronger witness, while unusual anatomy retains the same explicit exception
  path.
  Named retained-body, vehicle-part, and held-object attribution is now an exact
  sampled-pose renderer witness.
- **Implemented:** test car, hovercraft, and plane across all eleven race
  samples on five qualified courses rather than one favourable parked frame.
  The UI
  names only an actual framing/body problem, truthfully distinguishes an
  unqualified volume or blended-material visual check, and reports the minimum
  qualified visibility and maximum contact residual across all 55 rows.

Acceptance: each example context is visually seated, ordinary cameras show the
intended body region, and residuals/approved exceptions are source- and
fit-bound.

### M5 — Inspection and portrait framing (medium)

- **Implemented:** after the existing held-pose stabilization gate, isolated
  capture mirrors the exact current CPU skin/model/MVP transform for every
  selected primitive, unions positive-W posed vertices, and applies a bounded
  padded subject fit to capture-only UBO slots. The one-shot stabilized product
  does not need temporal camera hysteresis and never changes gameplay framing.
- **Implemented:** target 60–85% subject-height occupancy for portrait-ready
  model-only captures and fail closed on degenerate/behind-camera subjects. The
  volumetric ROM-backed regression fixture now occupies exactly 72% of the
  1,920-pixel output height and is centered to within one pixel in front, top,
  underside, one-player, and four-player capture arms.
- **Implemented:** separate “gameplay camera proof” from “author inspection
  framing.” Composed captures retain the actual camera; isolated captures use
  an independent replay slot and publish the correspondingly framed exact
  target-to-clip projection witness.
- **Implemented:** add a one-action default model portrait plus front,
  left-three-quarter, and right-three-quarter alternatives. The default chooses
  the first supported vehicle, configures the exact
  one-player model-only renderer, held select-idle midpoint, bright light,
  repeatable front vehicle-orbit camera, and 60–85% subject-fit contract. It
  writes only to a bounded launcher-owned cache, returns automatically after a
  stabilized capture, validates the PNG and source digest, and opens the
  reversible crop/matte/palette/outline/readability workflow without asking for
  a filename. A result with inconsistent pose, phase, context, player, camera,
  lighting, or render-product metadata is removed from the private cache and
  cannot silently fall back into the ordinary report tray. The alternative
  angles prepare the same exact Test controls for
  explicit report captures. This is the first portrait suggestion immediately
  after safe package intake; exact 40x40 editing and explicit Apply/Build remain
  authoritative.

Acceptance: the example produces a centered isolated capture, a recognizable
40x40 portrait, and matching select/HUD/results/minimap identity without an
external image editor.

### M6 — High-fidelity renderer tail

1. **Custom shadow integration (implemented):** accepted OPAQUE and MASK modern
   primitives replay into the world shadow maps from an immutable two-bank
   model/bone/material snapshot; MASK casters perform the same base-alpha
   discard as the scene. All visible modern materials receive the per-view
   cascades through the exact donor-object world binding. BLEND deliberately
   remains receive-only rather than casting an opaque silhouette. Calibrated
   eight-corner bounds enter the shared planner without CPU skinning or mesh
   duplication; missing resources, stale assets, invalid bounds, and command
   overflow fail to the existing world/decal path without partial modern
   caster publication. Normal frames replay the previous authored bank while
   presentation interpolation reuses the current frozen bank.
2. **Transparent ordering (primitive assembly implemented):** OPAQUE/MASK
   primitives remain authored-first and BLEND primitives stably sort per view
   from live posed-centroid depth. Missing camera evidence retains the complete
   authored BLEND subset. Intersecting/self-overlapping triangles inside one
   primitive and independent vehicle/world transparency queues remain visual
   qualification work; MASK remains the preferred hair/fur path.
3. **Texture compression:** add bounded KTX2/BasisU validation/transcoding,
   authenticated compiler records, format-capability selection, mip accounting,
   and PNG fallback. Preserve the current 4096-side/512 MiB decoded safety
   profile until device evidence supports a change.
4. **Projected LOD (implemented):** calibrated bounds use exact object MVP and
   logical viewport height with thresholds, 8% hysteresis, per-view state,
   sparse authored-level fallback, and deterministic split-screen behavior;
   invalid projection evidence retains an explicit distance fallback.
5. **Offline simplification:** integrate a deterministic meshoptimizer stage as
   an optional recorded source revision. Preserve seams, skin weights, material
   boundaries, sockets, and author-provided LODs; show comparison/error evidence
   before replacement.
6. **Material expansion:** add IBL/calibrated tone mapping, then bounded optional
   hair/clearcoat/subsurface profiles. Every feature needs an explicit fallback
   and device cost. Add morph targets and secondary skin influences only through
   new authenticated format capabilities, never by ambiguously reinterpreting v1.

Current ceilings—1,000,000 vertices, 2,000,000 triangles, 256 joints, 512
primitives, four authored LODs, 4096 texture sides, and 512 MiB decoded texture
bytes—are safety boundaries, not recommended budgets. Quality presets should
warn from measured device profiles rather than impose a low-poly aesthetic.
The current complete exact matrix now follows that rule: an over-target result
is never relabelled as a pass, but a deliberate local exception can be recorded
against the exact source/fit/LOD/build/presentation/resolution/device/driver/
timing evidence instead of making an expensive-but-safe character impossible
to enable.

Acceptance: representative high-, mid-, and low-tier devices pass one-to-four
player evidence with shadows, transparent hair, compressed textures, and stable
LOD transitions; unsupported optional features degrade visibly and safely.

### M7 — Distribution, compatibility, and ecosystem (large)

- Ship the frozen importer and pinned validator in every supported native app;
  qualify clean install, upgrade, missing-tool repair, and offline behavior on
  macOS, Windows, and Linux.
- Maintain portable-package rights/provenance review, recipient diff, receiving-
  build compatibility, and non-overwrite export. Maintain the separate bounded
  named-draft transfer contract: one exact source revision, privacy-normalized
  snapshots, exclusive export, mutation-free review, and additive import.
- Add a documented extension point for third-party source adapters that must
  output canonical self-contained GLB plus provenance. Do not load arbitrary
  importer code in the game process.
- Define online package negotiation, missing-package fallback, digest agreement,
  and privacy boundaries before custom visuals are advertised as shared online.
  Donor simulation/network identity remains authoritative.
- Maintain schema migrations and last-known-good rollback fixtures for every
  new renderer capability.

Acceptance: a normal player can install a reviewed portable package without a
development environment; an author can use external adapters without weakening
the native trust boundary; multiplayer never disagrees about simulation.

The release workflows now execute a complete network-independent lifecycle
through both the freshly frozen and packaged importer on macOS, Windows, and
Linux: raw inventory, source candidate build, independent review, optimistic
clean install, inventory, disable, deterministic disabled rebuild, no-overwrite
portable export plus independent review, re-enable, scoped removal, and exact
external-source preservation. Common network routes are deliberately made
unusable during the gate. The rendered launcher suite also covers a missing or
untrusted importer with actionable repair guidance and a preserved resumable
draft. Signed/notarized clean-machine launch, in-place application upgrade, and
physical offline/device observation remain release qualification rather than
unimplemented pipeline behavior.

Recipient review now leads with a bounded at-a-glance card after the receiving
build has either authenticated the embedded portable cache or compiled the
source itself. It separates package compatibility from post-install play
readiness, discloses new/update/same-source relationship, donor authority,
WebGPU/OpenGL behavior, rig-review status, LOD count, and the explicitly
unmeasured import estimate before the exhaustive installed-versus-candidate
diff. The same decision evidence is spoken from the focusable rights control
before consent; no package bytes install during review.

Named-draft transfer is separately executable and deliberately carries no
installation authority. A `.mdkrdrafts` file contains only all current-base
editor snapshots for one package, with exact portrait pixels and saved
timestamps disclosed before export; it strips the external portrait path and
omits model, package, license, ROM/save, and performance-evidence bytes. The
complete bounded inventory is integrity-checked and schema-validated. Recipient
review shows package/source compatibility, additions, semantic duplicates, and
collision renames before a local-use confirmation. Import re-reads the exact
reviewed digest and atomically adds only new snapshots; it never overwrites,
rebases, resumes, builds, activates, or assigns a character. The real rendered
two-catalog lifecycle covers exclusive export, 200% keyboard/speech review,
confirmed durable import, duplicate idempotence, and wrong-source refusal.

The third-party source-adapter extension is now executable without broadening
the trusted runtime. Any external Blender/FBX/USD/DCC tool can emit the public,
deterministic `.mdkrsource` v1 data contract: one self-contained GLB bound to
the exact artifact, original source digest, adapter name/version/homepage
claim, and conversion profile/settings digest, plus optional exact rights
bytes. The launcher performs a mutation-free bounded review, explicitly says
that integrity is not a signature, collects separate acceptance and a new
destination, then revalidates the reviewed artifact and GLB before fail-atomic
exclusive extraction. It opens the result through the ordinary resumable raw
draft and never loads or executes adapter code. The public reference packer,
hostile parser/extraction suite, real rendered lifecycle, and 200% keyboard/
speech review freeze the extension contract. A future signed executable
adapter marketplace remains a separate optional product with its own sandbox,
permission, signing, update, and revocation policy.

### M8 — Human and device qualification (ongoing release gate)

- Run observed first-use sessions with a finished package, a raw GLB, a nested
  DAE ZIP, a bad rig, a missing license, an oversized character, and a failed
  update. Measure time-to-first-preview, wrong turns, recovery, and completion.
- Qualify mouse, keyboard, controller, touch, screen reader, 200% scale, narrow
  layout, reduced motion, and color-vision treatments. Freeform spatial gestures
  must always have equivalent numeric/nudge controls.
- Maintain GPU/driver/device evidence for quality presets, scene variants,
  one-to-four players, capture, shadows, and LOD transitions.
- Add screenshot/perceptual fixtures for floor, facing, camera occupancy,
  vehicle intersection, pose collapse, portrait legibility, and model-capture
  occupancy. Numeric witnesses remain the authoritative supplement, not a
  substitute for human visual review.

Acceptance: every end-to-end user story passes with no source loss, no hidden
blocking work, no mouse-only step, and no success claim unsupported by durable
or renderer evidence.

## Recommended implementation order

Execute M0 first so every later milestone has reproducible evidence. M1, M2,
and M3 form the critical authoring path and should land in that order. M4 and M5
then close the visible example defects. Renderer work M6 can proceed in parallel
after M1 stabilizes the transform contract. M7 follows the format decisions made
by M6. M8 begins with M0 and remains a release gate throughout.

The forward-axis thumbnail studio, structural/symmetry rig proposal, derived
rest/bend bases, 72%-occupancy isolated capture, portrait handoff, exact
retained-body surface intersection, topology-qualified bounded containment,
final opaque-depth visibility witness, one-session three-state select and
five-course eleven-sample vehicle scene batteries, a renderer-registered
held-pose retail-donor/custom comparison studio, and fresh
five-context/performance fixture qualification are complete.
Fit approval now requires a current warmed exact contract, current aggregated
vehicle motion-and-scene evidence, and explicit composed-scene inspection; a completed
zero-fragment opaque/masked replay blocks approval, while unusual anatomy,
intentional occlusion, transparent materials, and contact exceptions retain an
explicit author-acknowledgement path. Detailed battery rows are session-only;
the durable approval remains source/fit/LOD/presentation and contract bound.
The immediate remaining order is therefore:

1. add bespoke boss, battle, and scripted-cinematic conditions only if those
   scene-owned surfaces become part of package presentation authority;
2. add optional author-defined joint constraint profiles and deterministic
   secondary hair/tail motion; exact bind-relative per-role travel inspection,
   the linked-ROM articulated unusual-proportion fixture, and the distinct
   13-state bounded reference library are complete, and the comparison overlay
   already requires exact source, fit, presentation, scene, held motion source,
   camera, viewport/scissor, and output-grid registration rather than inferring
   alignment;
3. execute the remaining optional M6 renderer tail (within-primitive/global
   transparency qualification, compressed textures, recorded simplification,
   and expanded material profiles); custom world-shadow casting/receiving,
   per-view primitive ordering, and projected LOD are complete;
4. complete signed clean-machine/upgrade observation plus online contracts;
   the data-only adapter contract, recipient compatibility/diff review,
   exact-base draft transfer, and the frozen and packaged importer lifecycle are automated
   on all three release operating systems; and
5. run the observed human controller/screen-reader, perceptual, and maintained
   multi-device qualification matrix in M8; automated 200% keyboard held-mode
   and virtual-controller transition-mode traversal of Animation Studio is
   complete, but it is not a substitute for observed assistive-technology use.
