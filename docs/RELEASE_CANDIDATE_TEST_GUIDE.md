# Golden Balloon 1.7.0 acceptance guide

Use this guide only with artifacts built from the same clean candidate commit.
Do not publish, retag, or substitute a rebuilt file after testing begins.

This is the complete player-facing walkthrough for changes since 1.6.0:
Adventure Party, the Character Workshop, Skip the launcher, Content Pack bonus
portraits, and the native online beta's route measurement, loss repair and
departure handling. Native release artifacts must expose Online Room. The
published browser remains local-only: it must ship no Online Room, cloud Phone
Party, controller, or room route and must attempt no service API. Local LAN
phone controllers may remain available in desktop packages without a cloud
origin. Cloud Phone Party is accepted only when the candidate carries the
deployed origin; a deliberately partyless release must say so in its provenance
and show no cloud Phone Party surface.

Use a legally owned US 1.1 or European 1.1 ROM. Share text logs and hashes in a
bug report, not ROMs or ROM-derived captures.

## 1. Record the candidate

For each desktop artifact, record its filename and SHA-256. Verify the adjacent
`.sha256` file where supplied and inspect `.provenance.json`: version must be
`1.7.0`, `commit` must match the candidate commit, and its recorded hash
must match the artifact.

For the browser build, open `build-info.json` and confirm version `1.7.0`, the
same source commit, and `source_dirty: false`. Hard-refresh before testing.

Stop if any identity differs. Do not test an archive in place: extract it to a
writable folder first.

## 2. Launcher and settings

Run these checks on macOS, Windows, and Linux; repeat the browser-relevant steps
on the web build.

1. Start without a saved ROM. The launcher must remain responsive while a ROM
   is checked, and an invalid file must not replace the last valid selection.
2. Select both supported revisions if available. European 1.1 must offer
   English, German, and French; US 1.1 must offer English and French.
3. Resize down to 640×480 and test UI scaling. The layout must remain usable,
   and dragging the scale slider must not flash or flicker. With no remembered
   ROM, the path field and full **Use This Path** button must stack inside the
   window rather than clip at the right edge.
4. On a Windows touchscreen or handheld such as ROG Ally, use 1.25× scale or
   larger. Tap every navigation mode, a combo choice, a checkbox, and the
   primary action. Swipe Settings from non-interactive text and confirm it
   follows the finger; a drag beginning on a slider or scrollbar must operate
   that control instead of moving the page. Open the F1 overlay and confirm its
   guidance changes to touch instructions.
5. Change master, music, and effects levels. Previews must be audible and free
   of obvious clicks; active loops must react immediately.
6. On Windows, toggle borderless fullscreen from Settings, **F11**, and
   **Alt+Enter**. Restart and confirm the saved choice returns.
7. Remap a controller button, disable rumble, then test Light, Balanced, and
   Strong. Restore defaults and confirm no stale binding remains.
8. Change a restart-scoped video setting during play and choose **Restart &
   Apply**. The same ROM must reopen. A forced startup failure must return to a
   usable launcher with diagnostics rather than exit.
9. Use a physical controller to change **Opponent skill** through **Original**,
   **Hard**, and **Brutal** in both launcher Settings and the F1 overlay. Check
   focus, selection, confirmation, and backing out without changing another
   setting. Restart when requested, then relaunch and verify the saved choice.
   Restore defaults and confirm **Original** returns. Record the controller
   model and candidate hash; keyboard or synthetic-input evidence alone does
   not close issue #62's physical-controller acceptance.

## 3. Core gameplay and presentation

Use WebGPU with Restored presentation unless a step says otherwise.

1. Watch the opening logos and title sequence at 16:9 and ultrawide sizes. The
   logo animation must fill the horizontal presentation without stretching;
   its authored top and bottom bands and centered copyright text must remain.
2. Open track select and move through its track and vehicle transitions. Live
   previews must remain inside the wooden frame with no side bleed. The
   decorative menu background must reach both display edges without black
   gutters, stretching, or a visible tile seam. Neighboring carousel labels,
   question marks, and navigation arrows must not appear in the widescreen
   side areas before their card enters the authored canvas. Once the expanding frame is
   gone, the vehicle/time-trial setup backdrop must fill the screen in Hor+.
   Back out once: the backdrop must return inside the wooden frame on the first
   frame of the reverse transition, with no flash or one-frame bleed.
3. Finish a single-player race. Check every post-race page, including Race
   Order, times, records, and options. Initial unframed footage must use the
   widescreen presentation; from the first wooden-frame transition onward,
   footage must remain inside its aperture with no one-frame bleed or FOV pop.
4. View animated credits. The moving background must fill the horizontal
   presentation while text remains in the safe reading area. Ordinary races
   must still fill the selected widescreen aspect.
5. Open the F1 overlay during a timed race. The kart, race clock, and world must
   freeze. Engine and ambient effects must fall away while quieter music keeps
   playing; resume must restore the mix cleanly. Escape must back out or request
   quit, never close the desktop app immediately.
6. Complete a three-lap race with music enabled. The background music must
   audibly accelerate when the final lap begins and remain stable at the faster
   tempo through the finish; it must not restart, stutter, or change pitch.
7. Drive a car, hovercraft, and plane. Engine sound must track throttle and
   speed, with no growing delay, breakup, or stuck loop.
8. Test Original cadence first. Then use Match Display or a numeric frame limit
   with Motion smoothing set to Interpolated. Gameplay speed must not change.
   Look closely at kart shadows, wheels, particles, fades, split-screen seams,
   and camera cuts for flicker, doubling, or intermediate-frame artifacts.
   On water and lava tracks, pan quickly across moving surfaces and watch the
   wave shape, texture scroll, sky, and projected shadows. They must move as
   one picture without shimmer, shearing, twisted shadows, or a one-tick sky
   lag. Repeat on a variable-refresh display if one is available.
9. Return Motion smoothing to Off and Frame Limit to Original. Confirm the
   authored presentation remains stable.
10. Confirm **Keep the camera out of walls** remains opt-in and **Authored** is
    the default. If reviewing the optional mode, record every rapid
    blocked-to-clear-to-blocked correction and every side switch. Compare the
    exact 1.6.0 artifact to classify regressions, not to waive a failed gate.
    A historical baseline failure does not waive a current release gate.
    The unresolved rapid-reengagement motion-quality failure remains a release
    blocker even if it is unchanged from 1.6.0. Any penetration, invalid/degraded
    pose, default-camera change, or worsening also blocks the candidate.

## 3b. Separate split-screen acceptance for issue #61

Record three separate symptom verdicts; a pass for one does not close the others.
Use two physical controllers and record the candidate hash, OS, GPU/driver,
renderer, ROM revision, aspect ratio, and presentation mode for each observation.

1. **Sky edges:** play two-player races on several tracks at 4:3 and widescreen,
   inspecting both viewports for black sky edges or uncovered background. Repeat
   with WebGPU and diagnostic OpenGL in Restored and Remastered presentation.
2. **Pause detail:** pause from player 1, resume, then pause from player 2 at the
   same window size and UI scale. Text and menu detail must be equivalent, with
   correct player ownership, in Restored and Remastered. Authored differences
   in panel color alone are not a resolution failure.
3. **Walrus Cove purple/blue:** reproduce the reported two-player route through
   the loop/tunnel on Windows/NVIDIA hardware and inspect both views. Compare
   Restored and Remastered against **Original** (`pure` in configuration) as
   the authored reference. The void curtain must still cover holes without
   obscuring track geometry or scenery; Original intentionally retains the
   authored presentation. Local macOS blue-area
   evidence does not establish that the Windows/NVIDIA purple report is fixed.

Keep unavailable hardware or an unobserved reported symptom marked pending,
not passed. Reconcile each result with `docs/open-items/github-issues.md`;
issue closure still requires explicit maintainer approval.

## 4. Wizpig, Terry, and persistent Magic Codes

Use a disposable save for code tests, then repeat the earned-unlock paths on a
normal save where practical.

1. Beat Wizpig for the second time. Wizpig must join the character picker. On a
   separate disposable save, enter `WIZPIGPOWER` and confirm the same unlock.
2. Beat the Dino Domain rematch. Terry must join the picker. On a separate
   disposable save, enter `TERRYFLY` and confirm the same unlock.
3. Select Wizpig on a car, hovercraft, and plane track. He must have his own
   portrait, name, placard, voice/identity, and character-select entrance; on a
   plane track he must ride his rocket. Attacks, items, collisions, boosts,
   finish order, and results must behave normally.
4. Repeat with Terry. His full flying pose and wings must render correctly with
   no missing model, collapsed animation, wrong portrait, or oversized shadow.
5. Test both racers in two-player local character selection and gameplay. Each
   controller must retain its own slot, vehicle, input, HUD identity, and result.
6. Finish a Time Trial with each bonus racer. Neither run may overwrite a retail
   character's canonical record or ghost.
7. In Magic Codes, toggle `CONTROL WIZPIG` and `CONTROL TERRY` after unlocking
   them. Restart and confirm the unlocks persist and each active toggle returns.
8. Enable ordinary reversible codes, restart, and confirm they return. Verify
   progression-changing, one-shot reward, credits, and deliberate lockout codes
   do not automatically reactivate.
9. Import, export, and erase saves. The shown unlocks and active reversible
   codes must match the selected save operation after every restart.

## 5. Taj regression pass

Test once on a fresh save and once on an existing save.

1. Enter `ABRACADABRA`. “Taj has joined the race” must be followed by a visible
   Taj slot in the actual character picker, regardless of which retail
   characters are unlocked.
2. Select Taj with keyboard and controller. His portrait, name, voice, horn,
   HUD, pause screen, and results identity must be his own.
3. Race with car, hovercraft, and plane selections. Taj must use the scaled,
   animated magic carpet; it must not appear as a flat oversized sheet or cast
   a large character-picker shadow.
4. Test two-player selection with Taj in either port. Player ownership and
   results order must remain correct.
5. Complete a Taj Time Trial and confirm it does not replace an original
   character's canonical record or ghost.
6. Relaunch and confirm the unlock persists. Exercise save import and erase;
   the unlock state shown by the UI must match the documented operation.

## 5b. Custom Character Workshop acceptance

Run this section on macOS, Windows, and Linux packaged candidates whenever the
Workshop is part of the release. Use a license-clean test model you are allowed
to modify and a disposable data directory. Do not publish the character source,
ROM, captures, or generated reports merely because they were used for
acceptance.

Before the first observation, create the canonical receipt without overwriting
an existing record:

```bash
python3 tools/check_character_release_evidence.py \
  --write-template character-acceptance.json
```

Replace its placeholders as the exact macOS, Windows, and Linux packaged
candidates are exercised. The record deliberately has no operator identity,
paths, ROM data, model bytes, screenshots, or device-profile contents; keep
those private and bind only their reviewed digests. The template starts with
failing statuses so an untouched or partial record can never look approved.
During data entry, `--structure-only` provides an explicitly non-approving
preflight. A release approval always requires `--artifact-dir`; the tool refuses
to print an approving verdict without rehashing the artifact and provenance
bytes itself. Name the normalized physical device in every passing modality
note (for example, the controller or touch-display model), without recording an
operator identity.

1. Start with no ROM selected. Open **Character Workshop**, import a GLB, review
   its validator report and rights, choose a donor profile, create a named
   draft, close the app, and resume it. Repeat intake with one unambiguous
   DAE/ZIP or canonical `.mdkrsource` result. Confirm unsupported DCC formats
   receive copyable conversion guidance and execute no adapter code.
2. Try an invalid GLB, ambiguous ZIP, traversal entry, invalid SPDX expression,
   missing importer, changed source, and an existing export destination. Each
   failure must preserve the source and playable last-known-good package, name
   the corrective action, and never overwrite a file.
3. Link the normal base-game ROM. In Offset Studio inspect character select,
   car, hovercraft, and plane from front, side, top, and underside. Correct
   scale, facing, floor/seat height, XYZ placement, yaw, and hand/foot targets
   with pointer controls and exact numeric fields. Confirm undo/redo, per-context
   isolation, copy-fit confirmation, restart persistence, stale-evidence
   invalidation, and reset-to-package-anchor wording.
4. In Animation Studio inspect held 0/50/100% phases and at least one A/B
   transition. Review source/reference/fallback motion, contact residuals,
   limits, and secondary motion. An awkward or clipping pose must stay visibly
   unapproved; the UI must not convert it into a green result automatically.
5. Capture a transparent model-only still and send it directly to Portrait
   Studio. Exercise crop/matte/mask, a style preset, pixel edit, undo/redo, and
   all seven readability views. Verify the exact 40x40 portrait in character
   select, HUD, results/rankings, minimap/collection flag, and the independent
   custom roster.
6. Set a mixed Latin/Arabic or Latin/Hebrew display and short name. Confirm the
   exact native shaped preview, direction announcement, live roster pixels, and
   cluster-safe compact fit. Then add an uncovered glyph and confirm the UI
   names the retail fallback and shows its exact projected text rather than a
   misleading partial native rendering.
7. Run the complete select plus five-course race review and the 1P-through-4P
   matrix. Inspect bounds, floor/seat/facing, camera/anatomy, four contacts,
   retained-vehicle surface and opaque-depth witnesses, LOD intervals, wall
   cadence, and optional GPU timestamps. Export a device profile only after its
   GPU/driver privacy disclosure. Keep over-target rows red or explicitly
   excepted against the exact device/workload; never relabel them as passing.
8. Build/install, assign to multiple local players, disable, rebuild/update,
   restore a prior revision, export and mutation-free review a portable package,
   re-enable, and permanently remove it. Disconnect networking before the
   offline relaunch. Verify assignments fall back safely, the external source
   and license remain byte-identical, and removal cleans only package-owned
   drafts/evidence.
9. Confirm donor simulation, collision, audio, save/ghost identity, and network
   authority remain explicit. OpenGL must retain the donor rather than show a
   partial custom model; online peers must not be told that visual-package
   negotiation or transfer exists.

Observe the complete flow at both ordinary and 200% UI scale, including a
narrow 640x480 layout. Cover mouse, keyboard, controller, and touch; enable and
verify the app's own spoken focus guidance, enable reduced motion, and inspect
the colour-vision views. Spoken guidance is deliberately not recorded as a
screen-reader pass: the ImGui shell has no native assistive-technology semantic
tree and the product does not claim one. Record each cell as `pass`, `fail`, or
`not available` with platform, OS, GPU/driver, display, controller/touch device,
package/source digest, candidate artifact SHA-256, and evidence-report SHA-256.
A failed or unobserved required cell blocks release; `not available` is
acceptable only for an input modality the tested platform genuinely cannot
provide and must be covered on another supported test system.

After all observations, place the four candidate artifacts and their provenance
sidecars in one directory and run:

```bash
python3 tools/check_character_release_evidence.py \
  character-acceptance.json --artifact-dir /path/to/candidate-artifacts
```

The verifier requires both Linux formats, the macOS DMG, Windows ZIP, all three
platform runs, every 1P-4P row and front/side/top/underside context, every
identity surface, at least one real pass for each input modality, and one
privacy-bounded low/mid/high physical-device profile. It rejects failed required
cells, silent `not_available` values, substituted bytes, placeholder hashes,
private machine paths, and unexplained performance exceptions. Record the
printed receipt SHA-256 in the release decision; a source-tree test log is not a
substitute.

## 5c. Adventure Party acceptance

Use disposable copies of a fresh save and a progressed Adventure Two save. The
setting is off by default; first confirm an unchanged one-player Adventure with
it off, then enable **Adventure Party** and restart when asked.

1. Admit two, three, and four local controllers at character select. Give every
   player a different racer and verify that controller, viewport, HUD, racer,
   pause and results identities stay aligned through a hub-to-race-to-hub cycle.
   Reorder physical controllers before a separate run; seats must follow the
   explicit assignments rather than discovery order.
2. In at least two different hubs, have non-host players collect a balloon and
   a hidden key and enter a door while another player reaches a competing exit.
   The party must receive each award once and take one whole-party transition;
   no player may be stranded in the old level or receive duplicate progress.
3. Finish ordinary races with 2P, 3P, and 4P parties. The field must contain six
   racers in each case, every human must control the selected racer, and the
   party must return to the same hub positions without losing its roster.
4. Finish a silver-coin race after splitting the eight coins across multiple
   humans. The shared count must advance once per coin on every viewport and
   award the result only when the party has all eight and a human wins.
5. Run a complete trophy series. Confirm the ordinary eight-racer field,
   per-race standings, and the championship result based on player one's rank.
6. Ask Taj for each vehicle transform. Every party member must change together
   without a seat, identity, camera, or input swap. Repeat one transform after a
   race and one in Adventure Two.
7. Enter one boss and one four-racer challenge. Only player one participates;
   the other players wait, and the original party must return afterwards with
   its roster, positions and shared progress intact.
8. Pause from a non-host controller, disconnect each occupied controller in
   turn, reconnect it, then resume. Simulation must remain stopped during the
   interruption, stale input must be neutral, and no other player may inherit
   the missing controller.
9. Attempt a native save-state capture while the party is live. It must be
   visibly refused without changing the campaign save. Save through the game's
   ordinary path, relaunch, and verify the shared progress on player one's file.

Any roster/identity swap, duplicated or lost progression, split transition,
stale disconnected input, display-list fault, or failure to restore after a
host-solo activity blocks the release.

## 5d. Native online beta acceptance

Use two separately installed 1.7.0 candidates on the same supported platform,
with clean data directories and legally owned supported ROMs. Repeat the core
route on macOS, Windows, and Linux before claiming those platforms; do not infer
cross-platform support, relay support, or more than two racers from a same-LAN
test.

1. Create a private room, join by the six-digit code, and compare the displayed
   verification words before accepting them. Deliberately reject one mismatched
   phrase and verify both clients return to a safe retry state without starting
   a race or leaking the room capability.
2. Select racers whose online-catalog and engine IDs differ (for example Diddy
   and Pipsy), then run one car, hovercraft, and plane race. Each player must
   spawn as the racer and vehicle they selected, with correct HUD/results
   identity and byte-identical race outcome on both endpoints.
3. Wait for the connection chip to settle, then start another race before it
   settles. Start must never wait for measurement; the chip must report a real
   route result when available and a slow route must widen only the local input
   lead, never refuse an otherwise compatible match.
4. Under controlled packet loss, drop a contiguous run longer than the input
   bundle's redundancy. The authority channel must repair the gap and both
   endpoints must finish converged rather than ending the race.
5. Introduce a short signaling/service interruption while the direct peer link
   remains healthy. The race must continue and must not claim the opponent left.
   Then close one endpoint: the survivor must reach the typed opponent-left
   result after the brief authored-tick grace, without waiting for the old
   20–30 second transport timeout.
6. Exercise a single-race rematch and every round of one tournament. Check
   character/vehicle/track ownership, results choice, re-keying, route
   remeasurement, and clean return to the room across consecutive races.
7. Join once with a different version and once with an unsupported ROM revision.
   Both must fail before racing with truthful compatibility copy. A custom
   character must remain local presentation only; the peer sees its built-in
   donor and neither side claims package transfer.

Record both endpoint logs and redact room credentials. Any divergent state,
wrong racer, unrepaired input gap, false departure, stale room authority,
credential disclosure, or unbounded wait blocks the release.

## 5e. Skip-launcher and bonus-portrait acceptance

1. Confirm **Skip the launcher** is off by default. Enable it, close normally,
   and verify the same validated ROM starts on the next launch without a visible
   launcher flash.
2. Hold Shift throughout one launch and both controller shoulders throughout a
   second. Each hold must keep the launcher open. Tap only after dispatch on a
   control run; it must not retroactively cancel a launch already committed.
3. Move or alter the remembered ROM before relaunch. The app must return to a
   usable launcher with the validation reason, not start the changed file or
   exit. Turn the setting off from the in-game Advanced panel and verify the
   launcher returns on the next ordinary start.
4. Quit from the hold-open launcher and from the game, with a controller
   attached. Both exits must be clean; no pad may remain open past SDL teardown.
5. Install a Content Pack that replaces Taj's, Wizpig's, and Terry's portrait
   keys with three visibly different, license-clean images and non-default
   dimensions. Verify each portrait in character select and relevant HUD/result
   surfaces, dump the replacements, and confirm every dumped PNG keeps the
   replacement's own dimensions. Disable the pack and verify the generated
   defaults return.

Dixie, Tiny, and NDS tracks requested in issue #58 are not implemented by the
bonus-portrait changes. Record the maintainer's release-scope decision in the
issue ledger before claiming complete 1.7.0 acceptance; this walkthrough does
not silently defer that content or count a portrait pass as its completion.

## 5f. Phone Party acceptance when the cloud surface ships

If artifact provenance declares a partyless release, confirm the cloud Phone
Party card is absent and skip the cloud-only steps below; local LAN phone
controllers still require their own two-phone route. Otherwise the compiled
origin must be the deployed HTTPS service used for this acceptance.

1. Pair one iOS and one Android phone by QR and by the fallback code. Compare
   the verification phrase, approve distinct seats, and finish a local
   split-screen race using both phones at once.
2. Add two more phones and verify four independent seats, mixed physical-pad
   plus phone input, rotation, browser chrome changes, touch chords, optional
   haptics, and explicit per-seat removal without disturbing another phone.
3. Background and restore each phone, lock/unlock once, change networks once,
   rotate the invite, and close/reopen the launcher around an engine loan. Input
   must fail neutral while absent, a stale capability or generation must not
   regain a seat, and an approved current lease must recover without reassignment.
4. Repeat the essential route at 200% UI scale with reduced motion and the app's
   spoken focus guidance. Record iOS/Android versions, device models, network
   topology, artifact hash and any unavailable modality without claiming native
   screen-reader semantics.

## 6. Browser custody and the local-only boundary

Before importing a ROM, confirm there is no **Add phone controllers**, Phone
Party, or Online Room control. The corresponding browser launcher APIs must be
absent. Old `/room/` and `/controller/` links must return the site's ordinary
not-found response, without redeeming or retaining an invitation.

1. Import a ROM, reload, and confirm the browser restores it locally. In
   developer tools, no request may contain a ROM filename or bytes.
2. Export a save, erase stored progress, import the backup, reload, and verify
   the preview and progress.
3. Repeat save management with WebGPU unavailable. Play may be blocked, but
   backup, restore, and erase controls must remain usable.
4. On a touch device, steer while holding Go plus Drift or Item. Rotate the
   device and collapse/expand browser chrome; controls must stay reachable and
   no input may remain stuck.
5. Do not rely on a browser-specific site-setting toggle. Serve the candidate
   with a `Permissions-Policy: fullscreen=()` response, then press the
   fullscreen button. Play must continue in the window and a visible
   explanation must appear. Remove the policy, open the same candidate directly
   in a top-level tab, and confirm the next attempt succeeds. The automated
   browser gate also injects a rejected exit request; its message must say that
   fullscreen could not be exited and offer Escape/browser controls.
6. Force browser storage unavailable, import a valid ROM, and start it. The ROM
   must remain usable for the session, the warning must leave save management
   unobscured, and **Retry browser storage** must persist the retained ROM after
   storage is restored without asking for the file again.
7. With keyboard focus on the game canvas, force a WebGPU startup failure. The
   launcher must return, announce a usable error, and move focus to that visible
   recovery message rather than leaving it on the hidden canvas.

## 7. Platform packaging

Know what the hosted workflows already proved for each artifact, so this pass
covers the rest rather than repeating it. `macos-release.yml` runs the packaged
app through LaunchServices on a `macos-14` Apple silicon runner and requires
WebGPU-default startup and successful surface presents there; that smoke runs
with `MDKR_AUDIO=0` and touches no audio device, controller, or hotplug. The
Linux AppImage and tarball are
uploaded only after `release.yml` renders and content-checks them under Xvfb
with Mesa's lavapipe/llvmpipe software stack, both as built and as extracted
through `AppRun` — no hosted Linux job has ever touched a physical GPU. The
Windows zip is built, import-checked, extracted, and launched from an unrelated
directory. `release.yml` preserves those exact bytes as a workflow artifact but
never attaches them to the public release automatically; they may be attached
only after this pass succeeds on Windows hardware.

### macOS Apple silicon

Verify the DMG sidecars, mount it read-only, copy `Golden Balloon.app` to a fresh folder,
and launch from Finder without renderer environment variables. An
unidentified-developer prompt is expected for the unsigned candidate; a
“damaged” warning is a failure. Diagnostics must report WebGPU. Test audio from
physical speakers or headphones, device hotplug, pause/resume, and clean quit.

### Windows 10/11 x64

Extract the whole `GoldenBalloon` folder and launch `GoldenBalloon.exe` from an
unrelated working directory. Test a ROM and save path containing non-ASCII
characters. For the long-path arm, create nested named folders until the full
ROM path is longer than 260 characters, copy the ROM there, and select it
through the native picker. Create a separate >260-character data root. In
Command Prompt, set `MDKR_SAVE_DIR` to that exact full path before running
`GoldenBalloon.exe`; record both full path lengths. Launch a race, save, return
to the launcher, and use Restart & Apply.
Confirm diagnostics, the long save root, and the long ROM remain usable after
the restart. Check WebGPU, fullscreen, controller input/remapping, rumble,
physical audio, saves, diagnostics, and Restart & Apply. Start the extracted
app cold at least five times and watch the full logo/title/character-select
sequence; audio must not begin behind video and then catch up while waiting at
character select. The package must not require a bundled DLL.

Repeat settings persistence with a Windows account whose user-folder path has
non-ASCII characters. Then create an empty `portable.txt` beside
`GoldenBalloon.exe`, change the ROM path and at least three settings, restart,
and confirm the data stays beside the game. Finally make the normal user data
folder unwritable without `portable.txt`; the launcher must report its fallback,
save beside the game, and restore those settings on the next launch.

### Linux x86-64

Test both the AppImage and extracted tarball on a physical GPU. Check WebGPU and
diagnostic OpenGL under the available X11/Wayland session. ROM selection is by
drag/drop or an absolute typed path. Record the distribution, display server,
GPU/driver, controller, and audio device; Linux remains best effort until this
physical breadth exists.

Launch the outer AppImage file itself, not only its extracted `AppRun` payload.
Record that runtime result separately from extracted-payload and tarball checks;
matching payload bytes or software-GPU passes do not prove AppImage startup on
the target desktop.

## 8. Campaign breadth and report

On at least one desktop candidate, play a clean save from start through credits,
including silver-coin races, later boss rematches, both Wizpig races, trophy
series, and save/reload boundaries. This manual route closes the campaign area
that automation deliberately does not claim.

Report:

- PASS or FAIL;
- candidate commit and artifact SHA-256;
- platform, OS, GPU/driver, display, audio device, and controllers;
- ROM revision and renderer;
- which sections above were run; and
- the first failed step with a redacted text log.

Any crash, damaged-app warning, renderer corruption, gameplay-speed change,
save loss, silent relaunch failure, input starvation, sustained audio defect, or
live image escaping a fixed frame is a release blocker.
