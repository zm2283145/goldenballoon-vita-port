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
  Action needed. Result v20 also retains the last complete accepted scene MVP,
  viewport, and scissor and projects the calibrated volume plus exact
  current-pose hips/chest/head node origins into bounded fixed-point camera
  evidence. Offset Studio reports camera occupancy, clipping, head placement,
  torso placement, and seat-to-hips displacement across restart. For every
  vehicle, one resumable action now runs all eleven race samples on three
  qualified courses: an open baseline, dense scenery, and an alternate
  environment. Each held generation snaps to its target sample, publishes
  only after the runtime reports it settled plus sixty replacement draws (a
  deliberate human-visible inspection dwell), and
  returns automatically with aggregated framing, retained-body surface,
  topology-qualified containment, opaque-depth visibility, and contact evidence.
  Approval requires all current source/fit/LOD/presentation-bound course
  batteries and reopens every legacy approval once. Select approval covers
  idle/hover/confirm; every vehicle approval covers 33 samples in total. This
  still does not identify a specific occluding attachment or prove weather and
  vehicle conditions outside the qualified course set.

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
  frame. Add joint-limit visualization before introducing optional anatomical
  limits.
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
- Expand the reference library for select hover/confirm, steering, reverse,
  boost, damage, item, spin, airborne/land, and finish states. Add optional
  hair/tail secondary motion only after deterministic base-pose behavior is
  locked.

Acceptance: the example no longer T-poses in select, every missing authored state
uses an intentional reviewed reference, and the source/fallback decision appears
in the package review and durable evidence.

### M4 — Per-vehicle seating and contact-assist workflow (large)

- **Implemented:** add “Fit all enabled contexts” as a guided sequence, not one
  opaque automation. It shows independent current-review state for Select, Car,
  Hovercraft, and Plane, then routes to the next incomplete context. The exact
  preview/camera-layout step now reflows at compact width instead of disappearing
  for handheld or large-text users.
- Continue the existing per-context sequence to
  propose root translation/yaw/scale, preview exact gameplay, then propose four
  contact offsets per vehicle.
- **Implemented for the current pose:** the fit review card shows exact
  calibrated camera occupancy, head/torso points, seat-to-hips displacement,
  floor/seat datum, hand/foot reach, retained-body surface intersections,
  topology-qualified bounded containment, and an isolated-versus-final-depth
  visibility grid. It does not mislabel 2D overlap as penetration or claim one
  pose covers motion.
- Use the existing root/bend/target/end witnesses to offer bounded least-squares
  offset suggestions. Never change package/tuning state until the author accepts
  each context.
- **Partially implemented:** hand residual <=25 mm and foot residual <=40 mm
  are visible exact-evidence guides. An over-limit review now requires an
  explicit source-and-fit-bound exception, participates in Fit undo/redo,
  survives named-draft v13 resume, and rolls back if persistence fails.
  Named attached-part attribution and frame-to-frame oscillation still require
  renderer-derived witnesses before they can be qualified honestly.
- **Implemented:** test car, hovercraft, and plane across all eleven race
  samples on three qualified courses rather than one favourable parked frame.
  The UI
  names only an actual framing/body problem, truthfully distinguishes an
  unqualified volume or blended-material visual check, and reports the minimum
  qualified visibility and maximum contact residual across all 33 rows.

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

### M6 — High-fidelity renderer tail (extra large, parallelizable after M1)

1. **Custom shadow integration:** replay accepted modern primitives into the
   world shadow maps and receive cascaded shadows under the same material/skin
   transforms. Add missing-resource failover and one-to-four-player cost gates.
2. **Transparent ordering:** sort BLEND primitives per view using stable depth
   keys, retain MASK as the preferred hair/fur path, and qualify intersecting
   hair/face layers. Do not reorder opaque or alpha-mask batches.
3. **Texture compression:** add bounded KTX2/BasisU validation/transcoding,
   authenticated compiler records, format-capability selection, mip accounting,
   and PNG fallback. Preserve the current 4096-side/512 MiB decoded safety
   profile until device evidence supports a change.
4. **Projected LOD:** replace distance-only selection with projected-size
   thresholds, hysteresis, per-view state, sparse authored-level fallback, and
   deterministic split-screen behavior.
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
- Finalize portable-package rights/provenance review, recipient diff, compiler
  compatibility, non-overwrite export, and bulk named-draft import/export.
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
three-course eleven-sample vehicle scene batteries, a renderer-registered
held-pose retail-donor/custom comparison studio, and fresh
five-context/performance fixture qualification are complete.
Fit approval now requires a current warmed exact contract, current aggregated
vehicle motion-and-scene evidence, and explicit composed-scene inspection; a completed
zero-fragment opaque/masked replay blocks approval, while unusual anatomy,
intentional occlusion, transparent materials, and contact exceptions retain an
explicit author-acknowledgement path. Detailed battery rows are session-only;
the durable approval remains source/fit/LOD/presentation and contract bound.
The immediate remaining order is therefore:

1. attribute separately drawn donor attachments where stable identities exist,
   and add weather/vehicle-condition variants beyond the three qualified
   Workshop course families;
2. add a real linked-ROM unusual-proportion fixture, then qualify richer
   reference clips, joint limits, and optional deterministic secondary
   hair/tail motion; the comparison overlay already requires exact source,
   fit, presentation, scene, held motion source, camera, viewport/scissor, and
   output-grid registration rather than inferring alignment;
3. execute the M6 renderer tail (shadows, transparent ordering, compressed
   textures, projected LOD, optional simplification, and material expansion);
4. complete packaged cross-platform/offline/adapter/online contracts; and
5. run the observed human, controller/screen-reader, perceptual, and maintained
   multi-device qualification matrix in M8.
