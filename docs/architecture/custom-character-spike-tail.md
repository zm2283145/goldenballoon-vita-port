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
| Isolated-capture subject height | 266/1,920 px (13.85%) |

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
  a bad static idle, and unusual proportions. The user-provided model remains a
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
development host. The intentionally uncalibrated contact residuals remain
roughly 319–336 mm; the evidence runner records those as M4 advisories instead
of misreporting a successful package as an invalid import.

The runner creates conversion, package, install, and disposable character
catalog state under the operating-system temporary directory. Its caller-chosen
evidence directory receives only redacted logs, PNG captures, and
`evidence.json`; it refuses to overwrite a nonempty directory. A private source
requires the pinned native validator and a reviewed
`mdkr-character-spike-decisions-v1` file. The synthetic validator seam is
explicitly restricted to the generated fixture and exists only for CI.

### M1 — Guided orientation, scale, and anchor calibration (medium)

- Present declared units, mesh-local bounds, scene-world bounds, normalization
  multiplier, and suspicious-transform warnings together. A 1.45 cm scene-world
  height beside roughly human mesh-local proportions must read as “review
  source transform,” not merely a generic small-height warning.
- Render four front candidates (`+z`, `-z`, `+x`, `-x`) as equally framed
  thumbnails. Let the author choose the face they recognize; never silently
  infer front from geometry.
- Propose ground and seat translations from compiled sockets and measured bounds,
  but keep them preview-only until accepted. Show before/after and offer one-step
  undo.
- Fit each context independently. Never seed car/hovercraft/plane from one shared
  accepted offset without immediately testing all three.
- Add visible severity bands for floor clearance, camera-envelope occupancy, and
  seat-relative bounds. Bands guide review and do not become arbitrary import
  blockers.

Acceptance: the example faces the camera in select, its feet are floor-aligned,
and its head/torso land inside each ordinary vehicle camera without manual JSON.

### M2 — Rig inference by structure and rest basis (large)

- Score role candidates using skin membership, ancestor chains, branching,
  symmetry, bind positions, and names. Names remain evidence, not authority.
- Detect the sibling-pelvis pattern and propose the lowest common ancestor as
  hips while explaining the tradeoff. Show why rejected candidates violate the
  native hierarchy contract.
- Estimate each role’s rest-basis correction from bind matrices and child
  directions. Display source/canonical axes, mirrored-limb consistency, and the
  exact quaternion; editing any value clears review.
- Generate stable bend-axis suggestions and an explicit “automatic” option.
  Add joint-limit visualization before introducing optional anatomical limits.
- Provide a task checklist: torso, left/right arms, left/right legs, head, then
  a motion battery. “16 roles present” must not imply “pose quality approved.”

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
- Add side-by-side held-phase thumbnails and transition playback for every
  semantic, including blend duration and fallback reason.
- Expand the reference library for select hover/confirm, steering, reverse,
  boost, damage, item, spin, airborne/land, and finish states. Add optional
  hair/tail secondary motion only after deterministic base-pose behavior is
  locked.

Acceptance: the example no longer T-poses in select, every missing authored state
uses an intentional reviewed reference, and the source/fallback decision appears
in the package review and durable evidence.

### M4 — Per-vehicle seating and contact-assist workflow (large)

- Add “Fit all vehicles” as a guided sequence, not one opaque automation:
  propose root translation/yaw/scale, preview exact gameplay, then propose four
  contact offsets per vehicle.
- Show head/torso camera occupancy, seat-to-hips displacement, kart/body
  intersection, floor clearance, and hand/foot reach in one review card.
- Use the existing root/bend/target/end witnesses to offer bounded least-squares
  offset suggestions. Never change package/tuning state until the author accepts
  each context.
- Define quality targets, initially advisory: hand residual <=25 mm, foot
  residual <=40 mm, no unexplained body/vehicle penetration, and no meaningful
  frame-to-frame contact oscillation. Explicitly record approved exceptions for
  unusual anatomy.
- Test car, hovercraft, and plane across representative start, steer, airborne,
  and finish states rather than one parked frame.

Acceptance: each example context is visually seated, ordinary cameras show the
intended body region, and residuals/approved exceptions are source- and
fit-bound.

### M5 — Inspection and portrait framing (medium)

- Make isolated capture solve a subject-fit camera from projected animated
  bounds, with padding and hysteresis, after the held pose stabilizes. Do not
  reuse a conservative gameplay pull-back for portrait products.
- Target 60–85% subject-height occupancy for portrait-ready model-only captures;
  disclose clipping and wide-proportion exceptions. The example’s current
  13.85% is a failing regression fixture.
- Separate “gameplay camera proof” from “author inspection framing.” Composed
  captures may retain the actual camera; isolated captures should optimize the
  subject while preserving the exact transform/projection witness.
- Add one-click front/three-quarter portrait arms, subject centering, and direct
  handoff into the existing crop/matte/palette/outline/readability workflow.
- Generate a first portrait suggestion during intake, while keeping exact 40x40
  pixel editing and explicit author approval.

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

The immediate next slice is therefore:

1. add four-way forward thumbnails plus transform severity/proposal review;
2. implement structural hips/root suggestions and rest-basis diagnostics;
3. add side-by-side held-phase and transition review for the now-reversible
   animation decisions;
4. fix isolated-capture framing to the 60–85% occupancy contract; and
5. re-run this example privately to verify select, car, hovercraft, plane,
   portrait, contacts, and four-player performance.
