# Character Workshop quality audit and completion plan

Status: source-backed audit and first implementation tranche, 2026-09-06.
Not a visual-acceptance report or release approval. This audit complements
`../architecture/custom-character-workshop-ux.md`,
`../architecture/custom-character-spike-tail.md`, and section 5b of
`../RELEASE_CANDIDATE_TEST_GUIDE.md`; it does not replace their gates.

## Product outcome and ownership

A creator should understand what a supported model needs, reach the exact
in-game result without editing JSON, recover every failed operation, and know
what remains unverified. More controls alone do not establish that outcome.
No importer can guarantee that arbitrary anatomy, unsupported materials, absent
motion, or unlicensed content will become a successful character. Guarantee
clear admission/recovery, preserved source and last-known-good state, explicit
limitations, and evidence-backed readiness instead.

This parallel work owns Workshop presentation, its pure readiness model, and
the associated model/UI contracts. Decoder, PNG, source-adapter/importer,
authenticated schema, evidence approval/store, donor simulation, online,
renderer, and release artifact qualification remain coordinated integration
boundaries. No changes to those boundaries are included in this tranche.

The interface review used the
[Web Interface Guidelines](https://raw.githubusercontent.com/vercel-labs/web-interface-guidelines/main/command.md)
for visible labels, focus, recovery, readable layout, and action hierarchy.
DOM/ARIA recommendations are not fabricated for ImGui. App-spoken focus
guidance is not a native screen-reader semantic tree.

## Prioritized findings and fix requirements

Locations refer to the functions in this working revision; line numbers may
move as the parallel native-online work is integrated. **Source-confirmed**
means the code establishes the condition. **Observation required** means the
visible outcome has not been reproduced in this audit.

| ID / priority | Source and finding | Impact and required fix | Current status / proof still needed |
|---|---|---|---|
| WQ-01 / P1 | `platform/app/character_workshop_model.cpp:16`, `platform/app/ui_settings.cpp:24008`, `platform/app/ui_settings.cpp:27078`: accepted over-budget performance counted in the same aggregate as Ready, while header/library omitted the qualification. Source-confirmed. | A creator could read an exception-qualified package as fully on target. Keep eligibility unchanged but separate Ready and accepted-exception counts in overview, library, narration, and lifecycle headline. | Implemented here; regression assertions cover all-ready, incomplete, accepted, and incomplete-plus-accepted states. Compiled, not executed. Rendered/speech checks pending. |
| WQ-02 / P1 | `platform/app/ui_settings.cpp:23783` (`drawCharacterReadiness`): the old overview always used three columns, a fixed scaled state column, unwrapped action labels, and same-line destination text. Source-confirmed structure; pixel overflow not reproduced here. | Small editor areas can lose readable hierarchy. Select stacked rows from the actual content width, wrap descriptive text, bound action labels, preserve all six destinations, and keep ready/exception evidence keyboard reachable. | Implemented here: one-column rows below 680 scaled pixels, otherwise shared three-column evidence; full-width navigable actions for all rows. Wide/narrow/200% rendered containment and focus-order validation pending. |
| WQ-03 / P1 | `platform/app/character_workshop_model.cpp:534` and `platform/app/ui_settings.cpp:23904`: preview-ready was geometry availability only, although exact preview requires ROM revalidation and WebGPU. Source-confirmed. | Geometry eligibility can be mistaken for an immediately available renderer preview. Name geometry availability and exact-test prerequisites independently; do not gate ROM-free intake. | Wording corrected here. No launch or readiness predicate changed; no-ROM and non-WebGPU rendered checks pending. |
| WQ-04 / P1 | `platform/app/ui_settings.cpp:27307`: the product explicitly requires a keyboard for source paths, names, and license details. `tests/check_character_workshop_history_ui.py:115` preserves this disclosure. | The complete authoring story is not controller-only or touch-only today. Add a native/OS text-entry handoff and accessible file/provenance entry, or obtain an explicit product scope decision that distinguishes controller-play setup from authoring. Preserve paste and exact entered values. | Open. Do not remove the honest disclosure or relabel D-pad navigation as complete controller authoring. Observe real keyboard, controller, and touch text entry on every supported platform. |
| WQ-05 / P1 | `platform/app/ui_settings.cpp` (`drawCharacterPortraitStudio`, `drawCharacterDraftLifecycle`): name fields are disabled outside a named draft; previously the explanation provided no adjacent handoff to Package. | The next-action resolver could send a novice to Identity, where required setup lived elsewhere. Add an explicit non-mutating draft handoff with an intended Identity return and preserve exact-base checks. | Implemented in the second tranche: Choose a draft, package/source-bound return intent, explicit successful save/resume return, safe Back action and focused destination. Compiled, not behaviorally qualified; edge cases below remain required. |
| WQ-06 / P1 | `platform/app/ui_settings.cpp` (`drawCharacterRawIntakeEditor`) combines model inspection, draft selection, provenance, donor, vehicle, calibration, and mapping controls in one long form. Source-confirmed structure; user confusion is an observation hypothesis. | First-time authors need a visible dependency checklist, inline corrective next action, and a route back to completed steps. Retain advanced controls and resumability; do not hide missing requirements behind a generic disabled Build. | Guided-input tranche implemented: six groups list every missing form prerequisite, next/per-step jumps revisit existing controls, and Build repeats its post-edit blockers. Original admission predicate unchanged. Syntax verified; behavior, containment, novice completion and interrupted/resumed intake still require observation. |
| WQ-07 / P1 | `platform/app/ui_settings.cpp:27286` (`drawCharacterLibraryRow`), `:20427` (Identity fields), `:19134` (portrait source path), and `:26124` (SPDX error): prior rail labels and errors did not wrap; full-width controls appended lateral labels. Compact library also used authored names as ImGui IDs, colliding for equal names and interpreting literal `##`. | Separate identity from authored content, measure wrapping against actual width, and preserve full values visually and in focused guidance. Stack field labels rather than allocating field width plus an off-panel label; do not truncate stored values to fit. | Bounded containment tranche implemented: both library modes share package-ID-backed wrapped rows, the compact popup is width-bounded and echoes the full selected name, Identity/portrait/minimap labels are independent, portrait browse stacks below, and selected raw diagnostics wrap. Source contracts written; no rendered containment or duplicate-name interaction claim yet. Other forms remain unqualified. |
| WQ-08 / P1 | `platform/app/ui_settings.cpp:19133`, `platform/app/ui_settings.cpp:19807`, `platform/app/ui_settings.cpp:20211`, and `platform/app/ui_settings.cpp:20683`: provisional framing, style/canvas editing, and two immediate installed-revision routes coexist; generic Save labels previously obscured their different destinations. | Creators must distinguish provisional framing, named-draft/editor state, and the current installed revision. Disclose canvas-versus-file inputs, editor reload, separate revision recovery, and unchanged enabled state before installation. | Presentation tranche implemented: explicit destinations and install labels, collapsed compatibility PNG workflow, independent shared colour control, visible/spoken consequences, and navigation-only Package action. Source contract written; rendered, failure/recovery and novice journey qualification remain pending. No transaction or admission rules changed. |
| WQ-09 / P1 | `platform/app/ui_settings.cpp:7317`, `platform/app/ui_settings.cpp:10340`, `platform/app/ui_settings.cpp:11590`, and `platform/app/ui_settings.cpp:17758`: rig/fit/test contain exact evidence and explicit review, not an artistic-quality oracle. | Keep missing/static/reference/authored motion distinguishable; never treat pose availability, coarse depth visibility, or bounded contact sampling as universal clearance. Make the next repair and exact retest legible for unusual proportions. | Existing source implements these safeguards; complete representative-model and input observations remain required. No approval threshold weakened here. |
| WQ-10 / P0 release qualification | `docs/RELEASE_CHECKLIST.md:21` and the decoder/PNG qualification sections retain final-source validation and advisory work; `tools/check_character_release_evidence.py` owns fail-closed artifact-bound human qualification. | UI polish must not turn local decoder evidence or an earlier packaged build into security/release clearance. Complete defensive decoder/advisory disposition, latest-source behavioral suites, exact shipping artifacts, and the maintained acceptance receipt. | Open integration gate, not a newly discovered exploit. No decoder/parser/security changes or payload analysis in this audit. |
| WQ-11 / P1 | `platform/app/ui_settings.cpp:11895`, `platform/app/ui_settings.cpp:11910`, `platform/app/ui_settings.cpp:16225`, and `platform/app/ui_settings.cpp:17089`: runtime LOD choices, offline source-copy workflow, and exact device evidence are separate. | Quality presets cannot invent LODs or promise frame time. Preserve one-LOD explanation, source disambiguation, real player counts, unavailable timing, and explicit over-target exceptions. Collect low/mid/high physical-device evidence. | Structural pipeline exists; maintained representative corpus and cross-device comparison remain open. WQ-01 closes summary ambiguity only. |
| WQ-12 / P1 | `platform/app/ui_settings.cpp:23005`, `platform/app/ui_settings.cpp:23294`, `platform/app/ui_settings.cpp:24913`, `platform/app/ui_settings.cpp:26253`, `platform/app/ui_settings.cpp:3611`, and `platform/app/ui_settings.cpp:28078`: drafts/revisions/review/recovery publish through existing durable/background boundaries. | Preserve exact-base resume, last-known-good update, no-overwrite export, source-preserving removal, and launcher-global completion. Verify failure and leaving-during-work paths on packaged systems, not just individual storage units. | No new defect established here. Signed/clean-machine/upgrade/offline/crash-recovery observation and latest-candidate behavioral reruns remain open release proof. |
| WQ-13 / P2, scope-dependent | `platform/app/ui_settings.cpp:25131`, `platform/app/ui_settings.cpp:11930`, and `docs/architecture/custom-character-workshop-ux.md:846`: modern appearance is WebGPU-only; OpenGL retains donor; additional material/morph/script coverage requires explicit contracts. | Do not imply backend parity, arbitrary-model compatibility, native screen-reader compatibility, or missing glyph repertoire. Add features only with authenticated format/runtime support and observed fallbacks. | Explicit limitations, not silently implemented features. Maintainer decides additional release scope. |
| WQ-14 / P1 online integration | `platform/app/ui_settings.cpp:24076` and the draft-aware Gameplay authority card: appearance is local; donor still owns simulation/audio/save/ghost/network identity. | Do not advertise shared online custom appearance before digest negotiation, missing-package behavior, privacy, compatibility, consent, and failure handling are defined and qualified. | Parent native-online audit owns this integration. No network authority changed by Workshop polish. |

## Complete user-story and code-path inventory

These rows identify the ordinary route and its adverse paths. A function or a
test filename is implementation inventory, not proof that the current packaged
candidate passes it.

| Story | Actual UI entry / supporting seam | Success and adverse-path acceptance |
|---|---|---|
| Install a finished package without authoring knowledge | `drawCharacterImportControls`, `drawCharacterCandidateReview`, reviewed install worker, `drawCharacterAssignments` | Pick/drop once; read receiving-build compatibility, author/rights, donor and changed fields; confirm, install, enable, assign. Cancel/stale-base/failure must not activate or destroy the previous revision. |
| Start without a ROM | `resolveCharacterWorkshopPrimaryAction`, `Settings_chooseCharacterSource`, `drawCharacterRawIntakeEditor` | Import and draft do not wait for ROM validation. Distinguish optional linked-ROM tests. Choosing another source does not abandon a candidate or lose an existing draft without an explicit action. |
| Diagnose and convert a source | `characterSourceExportGuidance`, `drawCharacterImportControls`, `drawCharacterFailureRecovery` | Supported GLB, one unambiguous DAE/ZIP and data-only adapter result converge on ordinary review. DCC guidance must be useful; ambiguous/missing/changed sources get retry/export/forget recovery. Source bytes remain external and unchanged. |
| Save, branch, close and resume authoring | `drawCharacterRawIntakeEditor`, `drawCharacterDraftLifecycle`, `applyCharacterDraftSnapshot`, `autosaveActiveCharacterDraft` | Preserve named state and independent tool histories. Test restart, same-source branch, changed-base refusal, invalid inventory, full inventory, failed persistence, and close while background work settles. |
| Normalize size, front, floor and vehicle fit | `drawCharacterTuningEditor`, `drawCharacterFitOverlay`, `CharacterWorkshop_suggestFit`, `CharacterWorkshop_suggestContacts` | Numeric and direct controls agree; select/car/hovercraft/plane stay independent; suggestions are previewed and reversible; applying changes invalidates prior evidence. Inspect broad/squat/tall, non-humanoid and intentionally unusual contact cases. |
| Map rig and choose motion intentionally | `drawCharacterRigStudio`, `drawCharacterMotionAuthoringStudio`, Animation Studio inside `drawCharacterExactTests` | Review proposed names/hierarchy/confidence, correct role mappings, choose authored-only when appropriate, inspect all 13 intents and exact held/A-B samples. Static/missing/fallback motion, remap invalidation and safeguard resets stay visible. |
| Create complete identity and portraits | `drawCharacterPortraitStudio`, `drawPortraitSourceImport`, `drawPortraitStyleLab`, `drawPortraitPixelEditor`, exact name raster preview | Capture/import/draw reach identical 40x40 runtime pixels. Exercise recipe/mask/style/canvas histories, crop re-review, moved source, Unicode/bidi/fallback, minimap colour, original-size readability and every player-owned runtime surface. |
| Select fair gameplay | `drawDonorProfileGallery`, `drawDonorGameplayComparison`, package/donor ownership card | All qualified donors, correct vehicle summaries, no-ROM unavailable evidence, draft-versus-active donor. Cosmetic changes must not alter simulation, collision, audio authority, records, saves, ghosts or rollback identity. |
| Target hardware and local player counts | `drawCharacterPerformanceAssembly`, `drawCharacterTestEvidenceMatrix`, `buildCharacterDeviceProfile` | Quality/Balanced/Performance/Four-player remain editable starting points. One-LOD source handoff, exact geometry/texture budgets and source/fit/LOD/build/backend/device-bound timing all remain honest. Missing timing never turns green. |
| Inspect, review and retest | `drawCharacterExactTests`, `drawCharacterPreviewResult`, source/fit review signatures, scene-battery workflow | Exact select plus five-course vehicle batteries, all supported contexts and 1P-4P layouts. Stop/resume, rejected ROM, incomplete renderer result, changed fit/presentation, unavailable visibility, contact exception and failed capture never reuse stale approval. |
| Build, share, update and restore | `buildCharacterDraftSource`, `drawCharacterDraftTransfer`, `drawCharacterRevisionRecovery`, candidate comparison | Combined draft build versus local tuning is explicit. Export never overwrites and asks rights/privacy questions. Wrong-base imports, changed candidate, missing importer and failed rebuild preserve the last good package. Cross-device transfer requires observation. |
| Disable or permanently remove | Package tab, removal confirmation/journal seam, `drawCharacterAssignments` | Disable is reversible; delete states exact owned scope and preserves original model/license. Test exact-ID confirmation, assignments falling back, draft/evidence cleanup, cleanup failure and restart recovery. |
| Leave or quit during work | `serviceCharacterManagerWorker`, `Settings_serviceCharacterWork`, launcher ownership/shutdown integration | Navigation preserves context; Play/import cannot race a catalog transaction; Quit remains responsive with a safe keep-open choice; completion publishes on the UI thread. Test success, exception, cancellation, failed persistence and window-manager close. |

## Validation and remaining implementation sequence

First tranche changes are presentation-only:

- Separate accepted exceptions from ordinary ready counts without changing
  `readyToEnable`, `readyToPlay`, row status, source facts or approval policy.
- Share evidence between wide and stacked readiness layouts. Area names wrap;
  full-width actions retain six keyboard/spoken destinations, including already
  ready evidence. Next-step description wraps separately from its short action.
- Qualify geometry eligibility and exact-preview prerequisites explicitly.
- Keep library action/status on separate lines and size rows for three text
  lines; this is not a claim that every arbitrary name now fits.

Second tranche closes the named-draft navigation dead end (WQ-05):

- Identity's **Choose a draft** action records only session navigation intent
  and opens Package. It creates, resumes, builds, activates and assigns nothing.
- Draft setup explains the return and focuses its name field. Explicit save,
  save-as or current-base resume returns to Identity's enabled name field only
  after the existing operation succeeds and the active draft's package/source
  match. The helper confers no source-validation or persistence authority.
- **Back to Identity** remains available before the unreadable-inventory early
  return. It changes no draft, and returns focus to the corrective chooser.
  Choosing another tab or package clears stale intent; a changed source cannot
  reuse the return request. Nothing adds persistent draft schema state.
- Integration review found and corrected a deferred-tab race: ImGui's
  `SetSelected` queues the next layout rather than changing the current visible
  tab (`lib/imgui/imgui_widgets.cpp`, `TabItemEx` / `TabBarQueueFocus`). The
  Workshop previously persisted that old visible tab and thereby cancelled
  the new draft-return intent. It now tracks request versus observation in
  `CharacterWorkshop_observeTabSelection`, retains the request across old or
  absent observations, and releases it only when the target is observed.
  Subsequent manual tab selections remain authoritative. Forward/reverse,
  multiple delayed frames, no visible tab/bar, acknowledgement and subsequent
  manual navigation have regression assertions; they still need execution.
- Edge cases to execute: empty name; no saved drafts; multiple current-base
  drafts; retained-base-only drafts; full/unreadable inventory; save failure;
  snapshot-resume failure; changed package/source; explicit Back; manual tab
  change; ordinary non-handoff save/resume; keyboard/controller focus and
  narrow/200% action containment. Failed operations must stay actionable in
  Package without pretending the Identity editor is ready.

Third tranche addresses WQ-06's dependency visibility without replacing the
raw-intake form or weakening admission:

- Six source/identity/provenance/gameplay/transform/mapping groups expose all
  13 existing form predicates. **Inputs supplied** deliberately does not mean
  rights verified, exact files accepted, motion polished, or ready to play.
- A primary next-step action and each group's revisit action move only focus
  and scroll to existing sections. No jump inspects, accepts, saves, builds or
  installs. The retained jump is keyed by raw draft, model path and inspected
  fingerprint and is consumed only when the destination becomes visible.
- The top checklist uses frame-entry values; the inline Build blockers use
  post-edit values and the same final transform/SPDX facts as the unchanged
  authoritative Build predicate. All advanced controls and autosave remain.
- Added pure-model assertions for all 8,192 combinations of the 13 predicates
  and a source parity contract that requires future admission changes to
  reconcile guidance. These tests are written, not executed. Observe completed
  and incomplete step jumps, off-screen/deferred focus, source/draft switching,
  same-frame edits, no-source inspection, bind fallback, stale transform review,
  invalid SPDX, missing attribution/URL, missing mappings and all-input Build.

Mapping-findability follow-up addresses a remaining WQ-06/WQ-07 source finding:

- `platform/app/ui_settings.cpp` (`drawCharacterRawChoice`): the original raw
  motion/seat/head pickers clipped large inventories but provided no search,
  selected-item inclusion/default focus, or wrapped full-name detail. A saved
  choice outside the initial clipped range was not explicitly brought into
  view on reopening. This is a source-confirmed navigation omission, not a
  claim that a rendered failure has been observed.
- The three selectors now search source names, expose match counts and a
  clear-search recovery action, include the exact selected row in the clipper
  on opening, and request its native default focus and scroll once. Search
  never activates automatically; the unfiltered list remains navigable with
  a keyboard/controller without typing. A flattened list child preserves
  navigation between search and results while rows remain fixed-height.
- Filtering preserves source order and exact original indices, including
  duplicate names and literal `##`/`###`. ASCII comparison ignores case;
  non-ASCII bytes remain exact. Only explicit row activation changes the
  mapping, so filtering a selected row out does not clear it or consume a
  saved authoring choice. The existing autosave and Build predicates remain
  unchanged.
- Full current names wrap beneath the selector; focused/hovered names have
  a bounded, scrollable wrapped detail area and retain full spoken guidance.
  Integration source review corrected that text-only area's child flags:
  unlike the flattened result list, it exposes ImGui's native scroll-panel
  navigation target, so keyboard/controller users can enter it to read overflow.
  A flattened child containing only text has no navigable descendant and would
  otherwise omit that route (`lib/imgui/imgui.cpp`, `EndChild`).
  Keyboard focus takes precedence over incidental pointer hover. Popup width
  is constrained to the editor width. Field labels stack above full-width
  boxes instead of extending beyond them; vendored ImGui's `###label` hash
  preserves the original field ID. No stored text is shortened to fit.
- Session-only query state resets on reopening. A still-open popup closes
  without selection when its field/draft/path/fingerprint owner changes.
  Filtering runs on opening or query edits, not on every navigation frame.
  Neither the query nor its result indices are persisted to draft/schema state.
- Authored model assertions cover empty/no-match cases, negative/out-of-range
  selection, literal markers, duplicate occurrences, exact Unicode, a 2,048-byte
  name, unchanged inventory and a selection at index 8,191. Source contracts
  bind all three callers to their exact source owner, preserve explicit-only
  assignment and fixed-height row rendering, and guard opening-only focus.
  Python AST and whitespace checks passed; no test/app execution was performed
  for this follow-up. Parent compilation and behavioral evidence are separate.
  Duplicate-name assertions are defensive helper coverage only: the actual
  source inventory parser already rejects duplicate clip/node names
  (`platform/app/character_raw_intake_index.cpp:244–268`). They do not establish
  an admitted duplicate-mapping or duplicate-name resume workflow.
- Still required: observe popup opening at a deep saved selection, search by
  keyboard, unfiltered controller traversal, empty-result recovery, Escape
  without edits, exact selection/autosave/resume, source switching
  while open, long-name detail scrolling, narrow/200% containment and spoken
  focus. These changes do not complete controller-only text authoring (WQ-04)
  or qualify arbitrary models. Field-level blocker navigation is implemented
  in the subsequent follow-up below and remains behaviorally unqualified.

Reinspection-recovery follow-up closes another WQ-06 source-confirmed dead end:

- `platform/app/ui_settings.cpp` (`drawCharacterRawIntakeEditor`): initial
  inspection was available only while `!intake.inspected`, although both
  missing detailed transform evidence and an older LOD inventory explicitly
  instructed already-inspected authors to reinspect. Source inspection now
  always offers an explicit **Reinspect GLB model** for inspected sources,
  including an externally corrected file at the same path. The transform
  diagnostic also provides that action beside unavailable/invalid evidence.
- The local action delegates to the unchanged
  `queueCharacterRawGlbInspection` / `applyCharacterRawGlbInspection` pipeline.
  It checks manager, portable-install and package-inspection ownership before
  admission. Reinspection saves the current draft first, preserving current
  mapping names before the worker helper resets transient inventory/indices.
  A failed draft save starts no inspection and retains the editable state.
- After an explicit request the editor returns for that frame, including
  queue refusal. It does not run later controls/autosave against pre-reset
  transform facts or cleared mapping indices. The existing helper invalidates
  inspection before queue admission; this UI deliberately does not restore old
  evidence if admission or inspection fails. Source bytes and saved drafts
  remain available, and failure guidance names report/correction/retry actions.
- Exact-source mapping restoration, changed-fingerprint review invalidation,
  worker completion ownership and the complete Build predicate are unchanged.
  No reinspection occurs from drawing a warning, moving focus, changing a
  field or opening a checklist section. Invalid standing height still needs
  correction before the draft can be saved; reinspection cannot repair a
  draft's numeric authoring choice.
- A follow-up separates invalid/nonfinite authored height from source-bound
  failure: `CharacterWorkshop_reviewSourceTransform` rejects both, so the UI
  must not attribute every invalid result to source bounds. The diagnostic now
  names **Standing height in metres** and its unchanged finite 0.1–10 range
  before source diagnosis; the adjacent transform reinspection action appears
  only with valid height. The general Source reinspection action remains
  available. Spoken height feedback carries the same corrective explanation.
  Source contracts pin this diagnostic precedence and action guard; no input
  range, source validation, acceptance or Build predicate changed.
- Source contracts cover all three guarded action sites, save-before-queue,
  same-path dispatch, all existing busy owners, frame exit and no direct
  reset/publication bypass. Python AST and whitespace checks passed; no
  behavioral test or app was executed. Still qualify legacy bounds/LOD0,
  corrected source at the same path, unchanged-byte mapping restoration,
  changed-byte review, save/queue/importer failure, busy jobs, repeated retry,
  restart/resume and keyboard/controller/spoken navigation on packaged builds.

Field-level blocker follow-up implements the remaining WQ-06 navigation seam:

- `platform/app/character_workshop_model.cpp` (`CharacterWorkshop_rawGuide`)
  derives `nextField` in the same pass over the thirteen existing requirements.
  Invalid height precedes transform acceptance within its section. A separate
  navigation-only resolver sends unavailable transform review evidence to
  the explicit Source inspection action; it adds no admission prerequisite.
- **Go to next step** and **Review first blocker** now name and focus the
  actual ID/name/license/SPDX/attribution/URL/vehicle/height/acceptance/mapping
  control, or the Build button for a complete form. Inspection has one guide
  destination in Source inspection, not two competing same-field buttons.
  All six **Open step** actions still revisit their section headings and cancel
  any pending field request.
- `platform/app/ui_settings.cpp` (`requestCharacterRawFieldFocus`,
  `observeCharacterRawFieldFocus`, `drawCharacterRawChoice`, and the raw editor)
  issues native focus once while the target is enabled, preserves pending
  intent across deferred drawing/scroll, and acknowledges only a visible,
  focused control (or the active input forwarded by the numeric input group).
  Mapping acknowledgement occurs at its closed combo, not trailing help text.
  The nav cursor is explicitly visible; focused controls keep spoken guidance.
- Draft/path/fingerprint changes, a skipped editor frame, new pointer/wheel,
  text or keyboard/controller navigation, an import shortcut, or a popup cancel
  outstanding intent. The user can navigate elsewhere without an every-frame
  focus override. Neither cancellation nor acknowledgement mutates draft data.
- The vendored ImGui focus API enters text editing but strips activation for
  non-inputable controls during tabbing focus. Source contracts bind this
  implementation distinction: focusing a vehicle checkbox, mapping combo,
  inspection, acceptance or Build does not activate it. All operations still
  require their explicit normal confirmation; review and Build rules remain.
- Existing exhaustive assertions now check the exact first field for all
  8,192 predicate combinations. Additional assertions cover every field's
  disabled admission, one-shot issuance, wrong target, visible-but-unfocused,
  focused-but-invisible, delayed acknowledgement, cleared intent and transform
  evidence routing. Source contracts cover all actual controls, same-source
  cancellation and the distinction between combo focus and popup search.
  Python AST and whitespace checks passed; no test or app was executed for
  this tranche. Parent compilation is separate evidence, not behavioral proof.
- Required observations remain: each field and completed Build focus; top
  forward and bottom backward/off-screen routes; invalid height with missing
  evidence; busy/disabled controls; typing/paste after navigation; Escape,
  controller B and manual tab/section/import interruption; source/draft change;
  every mapping popup without automatic opening/selection; acceptance and
  Build without automatic dispatch; narrow/200% containment and spoken focus.
  Controller-only text authoring remains an open product limitation (WQ-04).

Field-focus integration review found and corrected one further WQ-06 gap:

- `platform/app/ui_settings.cpp` (`drawCharacterRawIntakeEditor`, pending
  focus cancellation) checked inserted text and navigation but omitted
  Backspace/Delete and character-free editing shortcuts. The vendored
  `lib/imgui/imgui_widgets.cpp` (`InputTextEx`, keyboard/shortcut processing)
  handles deletion, selection, clipboard and undo separately from text input.
  Thus an outstanding backward/off-screen jump could survive deliberate
  editing input. Cancellation now also observes Backspace/Delete/Insert,
  keypad Enter, and Ctrl/Cmd+A/C/V/X/Y/Z without consuming or routing those
  events. It stops our pending issuance/scroll; it does not promise to undo
  native focus that ImGui already applied at the start of a frame.
- Source contracts now bind those character-free interruption paths and
  prohibit shortcut-routing/key-ownership calls in the cancellation block.
  The reviewed source/draft/fingerprint reset, disabled-target gating,
  height-before-transform and unavailable-evidence inspection routing have
  no additional confirmed defect in this scoped review. `InputScalar` places
  its numeric entry first and `EndGroup` forwards its active ID, matching the
  request-before-control and active-group acknowledgement for height.
- Deletion/clipboard/undo while a jump is pending, delayed grouped-input
  acknowledgement, busy-target recovery, and actual keyboard/controller
  focus remain behavioral qualification scenarios; source inspection is not
  a rendered or device pass. No authoring/admission/persistence rule changed.
- This review passed Python AST parsing of the UI contract, scoped
  `git diff --check`, and strict C++17 `-Wall -Wextra -Werror -fsyntax-only`
  for the Workshop model and its existing assertion fixture. The UI
  translation unit is handed to parent compilation; no test or app was
  executed during this review.

Fourth tranche addresses WQ-08's publication hierarchy:

- Source findings: `drawPortraitSourceImport` owns a separate provisional
  framing/mask history; Send changes style source while Apply also changes the
  editable canvas. `drawPortraitStyleLab` does not apply its preview merely
  because its recipe changes. The visible copy now distinguishes these scopes.
- `tools/character_package_manager.py:1367` / `:1466` show that PNG and RGBA
  actions both compile through the installed identity-revision transaction;
  `:1174` preserves the current enabled/disabled cache destination. These are
  local current-source changes, not network publication, assignment actions,
  or mere draft saves. No manager code changed.
- `loadCharacterIdentityEdit` reloads editor state when the source digest
  changes; `beginCharacterHistory` similarly resets source-bound history.
  Consequently, neither installation is undone by Identity Undo. Both actions
  now disclose reload, excluded unpublished edits, saving a named draft first,
  last-known-good preservation on failure, and separate Package recovery.
- The compatibility PNG route remains available through a focusable, spoken
  collapsing header. Its selected file remains independent of styled/canvas
  pixels. The minimap control sits outside the collapsed section because draft
  Build and both installation routes share it. Full-width install actions use
  explicit input labels; the exact existing enablement predicates remain.
- Open Package finishes Identity history and requests its tab only. It does
  not build, install, restore, close or delete a draft. Existing deferred-tab
  handling remains authoritative; normal draft autosave is unchanged.
- `tests/check_character_portrait_studio_ui.py:243` adds source contracts for
  destination/consequence ordering, collapsed-state access, independent shared
  colour, unchanged publication inputs/gating and mutation-free navigation.
  These are written assertions, not behavioral evidence. Execute keyboard and
  spoken expansion, source/canvas divergence, open/closed draft, same-frame
  edits, invalid PNG/worker failure, successful install on enabled and disabled
  packages, retained revision recovery, and narrow/200% containment. Verify
  that unrelated editor work saved as a named draft remains recoverable.

Fifth tranche addresses WQ-07's concrete containment and identity findings:

- `drawCharacterLibraryRow` measures and paints with the same actual wrap width,
  allocates a text-derived row height, and clips painting to the row. Native
  Selectable still owns focus, hit testing and popup selection. Painting adds
  no ImGui item, so the existing complete spoken name/state attaches to the
  selection. Off-screen rows do not paint.
- `PushID(packageId)` plus a hidden constant row ID separates interaction from
  authored text. Both rail and compact popup use it. Equal display names no
  longer share name-derived IDs, and `##` in a name remains visible content.
  No source names or narration values are rewritten or truncated.
- Compact selection constrains popup width and displays the entire selected
  name in a wrapped detail line because its ordinary combo preview remains
  single-line. Identity name fields and both portrait paths use wrapped labels
  above their bounded input boxes. The minimap's existing section heading is
  its visible label; focused speech remains attached to the colour control.
- Portrait source browse no longer requires a fixed side-by-side width and
  its input speaks the complete path. Raw draft IDs, inventory/fingerprint
  summaries, invalid package IDs, SPDX errors and LOD destination corrections
  wrap without changing validation or filesystem operations.
- `tests/check_character_workshop_history_ui.py:206` pins package-owned row
  identity, equal measure/paint wrap inputs, visible-only clipping, native
  focus ownership, shared use by both library modes, bounded popup/full-name
  detail and independent field IDs. It does not prove rendered pixels.
- Pending: duplicate-name selection by mouse/keyboard/controller; literal
  `##`; long unbroken and multibyte names/paths; short/long narration; display
  names deliberately different from narration; selected/hover/focus contrast;
  popup dismissal; row scrolling; editor handoff focus after stacked labels;
  native 100%/200% and narrow layouts. Rig/fit/performance, draft/revision
  inventories, compatibility headers, source-adapter fields and every other
  unmodified long-label surface still need their own measured acceptance.

Related-pattern sweep extends WQ-07 beyond the library (sixth tranche):

- **Assignment collision, fixed in source:** `drawCharacterAssignments`
  (`ui_settings.cpp:27461`) formerly keyed custom choices by display name and
  status only. Two equally named packages could share a selectable ID despite
  different assignment targets. It now uses the existing package-ID-backed
  wrapped row. The readiness predicate, disabled controls and exact
  `AppConfig::setAndSave(key, entry->id)` operation remain unchanged.
- **Secondary node collision, fixed in source:**
  `drawCharacterMotionAuthoringStudio` (`:6822`) formerly derived stable-root
  and child selectable IDs from numeric node prefixes plus imported names.
  ImGui's `ImHashStr` (`lib/imgui/imgui.cpp:2479`) treats `###` as an ID reset;
  its presence in an authored name could defeat that prefix. Per-node scopes
  and final fixed widget suffixes now isolate these choices, with full-label
  spoken guidance. Node admission, chain limits and review invalidation remain.
- **Editable tree identity, fixed in source:** the same function's tree title
  included editable chain name and joint count, and ImGui pushes that tree ID
  around child controls. Renaming therefore changed the name input's identity
  on the next frame; changing joint count likewise replaced descendants' IDs.
  The tree now uses a constant ID under its existing chain-index scope and
  formats its title separately. No chain data or validation changed.
- **Raw mapping ambiguity, fixed in source:** `drawCharacterRawChoice`
  (`:25649`) already had per-index IDs but submitted imported names as labels;
  `##` hid distinguishing suffixes. The accepted raw index supports printable
  names and checks full-name uniqueness (`character_raw_intake_index.cpp:55`,
  `:247`), not ImGui display-name uniqueness. Rows now use a hidden constant
  ID, paint literal text, and speak the complete value. Explicit fixed text
  height and no wrapping preserve the existing list clipper; wide text is
  still clipped visually to its row rather than changing stored mappings.
- **Scoped no-collision findings:** named draft Resume/Delete buttons use
  draft-ID scopes; bundle draft previews have draft-ID scopes; main humanoid
  role choices have joint-index scopes; portrait capture actions and fixed
  style variants have index/digest scopes. Revision choices end in independent
  index IDs and display authenticated hex digests. Raw draft, failed-import,
  and performance source-draft choices append independent draft/record IDs,
  so this sweep did not establish cross-row ID collisions in those selectors.
- **Remaining at the sixth-tranche handoff:** ordinary `##` still hid part of
  authored text in several scoped selectors, including main humanoid roles,
  secondary-node visual labels, bundle previews, raw-draft and performance
  source choices, and failed-import labels. Distinct numeric/record identities
  or full focused speech reduce ambiguity in some, but do not prove visual
  acceptance. The seventh tranche below separates literal text from widget
  identity there while preserving each list's clipping/selection semantics.
  Chain deletion also needs observed focus behavior when later index-scoped
  chain controls move; no wrong-chain mutation has been reproduced here.
- Added source contracts for assignment identity and unchanged gates/target,
  per-node fixed suffixes, fixed tree identity, and literal raw-choice rendering
  with fixed-height clipping/full-value speech. Execute equal-name assignment,
  imported-name selection, continuous chain typing/rename, chain extension,
  raw mapping navigation/scrolling, and ordinary existing regression journeys
  before treating this source review as behavioral evidence.

Seventh tranche completes the bounded literal-name follow-up:

- `drawCharacterLiteralChoice` (`ui_settings.cpp:6824`) retains each caller's
  native selectable ID, height, selection state and popup behavior. It hides
  only the native parsed-label paint, restores text colour, then paints the
  separate literal value into a fixed-height row clip. It adds no focusable
  item and does not rewrite names, paths, draft/record IDs or mapping indices.
- Main humanoid roles, secondary root/child rows, bundle previews, raw-draft
  choices, exact performance-source choices and failed-import choices use
  that helper. Humanoid rows now speak the complete joint label and role
  consequence; failed-import speech no longer includes the internal widget-ID
  suffix. Existing per-node/draft scopes and appended unique IDs remain exact.
- `drawCharacterRawChoice` reuses the same fixed-height helper and keeps its
  existing index scope, selection assignment, list clipper and full-value
  speech. It does not adopt variable-height library rows under that clipper.
- Source inspection confirmed that native combo previews also pass through
  `RenderTextClipped` (`lib/imgui/imgui.cpp:3947`), which interprets `##`.
  `beginCharacterLiteralCombo` (`ui_settings.cpp:6842`) keeps the native combo
  ID, frame and arrow, supplying an empty native preview and painting the
  actual text into the captured parent draw list with arrow-safe clipping.
  Capturing parent geometry/colour before opening the popup avoids painting
  the preview into the popup. No internal ImGui APIs or extra items are used.
  The affected source/rig/recovery previews and library/assignment previews
  now use this path; current widget labels and native identities remain.
- Extended source contracts pin native label passthrough, colour restoration,
  literal text painting, fixed height/no wrapping, captured-parent preview
  drawing, separate row text, and the unchanged raw clipper. No mapping,
  admission, autosave, evidence, source validation or transaction logic changed.
- Pending rendered proof: visible `##`/`###` in rows and closed previews,
  opened/closed/off-screen/disabled combos, foreground popup layering, arrow
  containment, selected/focused/hover contrast, long UTF-8 values, clipper
  scrolling and complete keyboard/controller/spoken journeys. Long strings
  can still be width-clipped in fixed-height choices; full focused speech and
  existing selected-source detail remain important. This is not a claim that
  all Workshop visual or assistive-technology behavior has been qualified.

Executed validation for the first two tranches: `git diff --check`; strict C++17 Clang syntax checks for
the model and its test source; compilation/link of
`mdkr_character_workshop_model_test` and native `mdkr64` in `build-rel`.
The second tranche also passes Python 3.10 AST parsing for the expanded
Workshop UI source contract. Pure-model handoff tests cover default/reset,
matching identity, wrong package, wrong source and missing source. Source
contracts pin the corrective/back actions, exact active-draft checks, focus,
success handoffs and absence of mutation calls in the navigation action.
No test executable or app ran in this audit. Standing maintainer authorization
is present; the parent session reported a cached execution-tool refusal before
process creation. No alternate entry point was used to bypass that denial.

Third-tranche validation: `git diff --check`, strict C++17 Clang syntax checks
for the model and test source, and Python 3.10 AST parsing for the expanded
Workshop UI source contract passed. Integrated native compilation is handed
to the parent; the new assertions and rendered/device journeys remain
unexecuted at this handoff.

Fourth-tranche validation: `git diff --check` and Python 3.10 AST parsing for
the Portrait Studio source contract passed. No app, test, or build was run by
this tranche. The parent owns integrated native compilation; all new behavioral
observations remain pending, despite standing maintainer authorization.

Fifth-tranche validation: `git diff --check` and Python 3.10 AST parsing for
the Workshop history/UI and Portrait Studio contracts passed. No app, test
or build ran in this tranche; the parent owns integrated compilation after
the source-stable handoff. Native source contracts and behavior remain
unexecuted at this handoff.

Sixth-tranche validation: `git diff --check` and Python 3.10 AST parsing for
the extended Workshop UI contract passed. Source was handed to the parent for
integrated compilation; no build, app or behavioral test was run by this
tranche. These findings distinguish source-confirmed ID/dataflow defects from
the still-unobserved input and rendered consequences.

Seventh-tranche validation: `git diff --check` and Python 3.10 AST parsing of
the extended Workshop UI contract passed. Parent final optimized, ASan/UBSan
and Windows cross-builds compile/link the game plus `character_workshop_model`,
both prepared-dispatch fixtures and the existing RTC work fixture. All include
the settled mapping/detail focus, source-preserving reinspection and corrected
invalid-height diagnostic. Independent bounded source review found no additional
introduced issue. Windows retains its historical cloud origin, not final
partyless artifact provenance; compiler warnings remain. No new tests or apps
executed: source assertions and rendered/input observations remain unexecuted.

Latest field-navigation integration: optimized, ASan/UBSan and Windows cross-builds
compile/link the game and Workshop model with actual-field blocker focus and the
deletion/Ctrl-Cmd editing interruption correction. These builds also compile both
new actual-vendor transport fixtures and the prepared dispatcher. They are not
rendered or input acceptance; all new assertions remain unexecuted. Windows
retains its historical cloud origin rather than final partyless provenance;
existing BasisU GCC warnings remain.

Remaining behavioral verification:

1. Execute `character_workshop_model` and the Workshop history/UI contract.
   Require the new summary assertions and existing readiness destinations to
   pass. Add rendered geometry assertions for both sides of the actual-width
   breakpoint, scale 100%/200%, all six row states and long content.
2. Run raw intake, failure recovery, draft lifecycle/transfer, Portrait Studio,
   rig/fit/contact/LOD and exact renderer/identity/roster regressions against
   the same final source. Existing suites include
   `check_character_raw_intake_ui.py`,
   `check_character_workshop_history_ui.py`,
   `check_character_portrait_studio_ui.py`,
   `check_character_draft_transfer_ui.py`,
   `check_custom_character_workshop_preview.py`,
   `check_custom_character_identity_surfaces.py`, and
   `check_custom_character_roster.py`; importer/security suites remain required
   independently. Do not replace them with model-only checks.
3. Qualify WQ-05's draft handoff and WQ-06's guided intake dependency display;
   qualify WQ-08's publication hierarchy and consolidate long-label findings
   (WQ-07).
   Obtain/implement the complete-authoring input scope for WQ-04.
4. Observe novice and expert journeys on exact macOS/Windows/Linux artifacts,
   ordinary/narrow/200% layouts, mouse, keyboard, controller, available touch,
   app-spoken guidance, reduced motion and colour-vision treatments. Record
   real physical low/mid/high devices and exact provenance in the canonical
   release receipt. Fixtures do not stand in for these observations.

Parent integration of the guided-intake tranche passes optimized and ASan/UBSan
compilation/link of the native game and Workshop model-test target. This checks
the actual UI integration; it does not execute the exhaustive model assertions,
source contracts or rendered interaction journeys.

Parent integration of the fourth (portrait-publication) tranche also passes
optimized and ASan/UBSan native game and portrait-studio target compilation.
The parent added normal failure reporting for missing source-contract markers;
contract execution, visible layout/focus and actual revision/recovery outcomes
remain pending.

No competitive-superiority verdict is supported by this audit. A useful future
comparison must exercise the same user stories on identified current builds,
measure completion/recovery/readability, and distinguish MDKR's implemented
controls from observed quality and from still-planned capabilities.
