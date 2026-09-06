# Public issue acceptance

This register separates implementation, verification, and release availability.
An existing feature or a passing test for one symptom is not evidence that an
entire public issue is resolved. The repository inventory on 2026-09-05 found
four open issues in `akratch/goldenballoon`, and none in `akratch/mdkr64` or
`akratch/goldenballoon-staging`.

**Release-worktree reconciliation (2026-09-06).** This update uses the local
source tree and saved validation logs; it does not refresh GitHub state or close
issues. The candidate is **not release-ready**. The custom-character KTX2/BasisU
alignment finding is fixed in the candidate with a valid-texture regression
and a clean 601-second ASan/UBSan fuzz run (4,004,077 executions). KTX2 remains
enabled; final-artifact qualification is pending. See
[the release blockers](../RELEASE_CHECKLIST.md#current-candidate-status-2026-09-06).
The passes below remain historical evidence until explicitly requalified;
local testing is authorized under the maintainer's standing policy, but new
runs currently await an execution-policy reload. The already-running old-source
diagnostic suite continues; its results do not qualify newer source changes.

Clean-source artifact checkpoint `12cab88c` additionally passes macOS bundle
and mounted-DMG LaunchServices/WebGPU qualification with exact provenance.
Its packaged launcher-skip check passes all nine arms. The clean local-only
browser payload passes rendered cloud-surface absence and save-custody checks.
These narrow artifact results neither establish Windows/NVIDIA equivalence
nor replace physical-controller acceptance or the unrestricted release suite.

The unrestricted diagnostic run also reports Modern-camera motion failures;
the candidate corrects a shoulder-census miscount, but a separate correction
re-engagement remains unresolved. Neither is issue-closure evidence. Linux
Release compilation now passes at the later `ba620a1b` checkpoint, but Linux package/runtime
qualification is incomplete. Its first complete ROM-free rerun had three online
test failures; the transport fix passes focused Linux optimized/sanitizer
checks, and the complete rerun with the adapter-harness correction passes
279/279 tests. Strict packaging at `5fc099fc` now produces both Linux artifacts;
tarball launcher checks pass and the AppImage's shared payload matches it.
A subsequent clean `72770712` qualification includes PNG-writer hardening and
passes 280/280 selected tests, built/packaged launcher checks, strict packaging,
and shared-payload parity. Native AppImage runtime/hardware acceptance and
final-source full qualification remain pending. The original native diagnostic
CTest run's stale lobby-takeover success count is also corrected; its focused
CTest and CI drift controls now pass, not the complete unrestricted suite.
Further texture-pack PNG admission hardening is compiled but awaits behavioral
validation; it is not included in the earlier qualified Linux artifacts.
Clean Debug compilation at `c522ebae` and focused ASan/UBSan-instrumented
compilation now pass; these are not behavioral acceptance results.
The subsequent complete Windows cross-build now passes after fixes for three
unit-target portability failures and a test-fixture bounds error. No Windows
executable was run, and the Windows/NVIDIA and package gaps remain open.
The web engine at `0f5222f3` additionally passes compilation and non-executing
JavaScript/WebAssembly checks, not browser acceptance. A Windows portable-mode
read-back predicate is corrected with pending CI controls; that source fix does
not establish Windows settings persistence or package acceptance.
See the release checklist for these separate
qualification gaps; neither changes the Windows/NVIDIA or controller verdicts.

## #62 — Opponent skill

[Report](https://github.com/akratch/goldenballoon/issues/62): a text field does
not explain which values are accepted.

**Fixed in the candidate, not yet released.** The shared launcher/in-game
settings row now uses the existing controller-navigable combo with Original,
Hard, and Brutal choices. Labels describe their relative speeds. The stored
values remain `authored`, `hard`, and `brutal`; the default, gameplay calculation,
restart requirement, persistence, and environment locks are unchanged.

**Mechanism and class sweep.** `drawKey()` falls back to `InputText` whenever a
string key has no `optionsFor()` entry. Opponent skill was missing that entry.
Inspection of every string schema declaration found the other closed domains
already routed to choices: simulation cadence, frame limit, smoothing, tearing,
presentation mode, shadows, camera obstruction/comfort, menu languages, window
mode, rumble profile, and controller bindings. Aspect expressions, FOV values,
texture-pack paths, and disabled-pack lists intentionally remain open domains.

**Verification (optimized native build).**

```sh
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_APP_TESTS_ALLOWED=1 MDKR_AUDIO=0 \
  ctest --test-dir build-rel --output-on-failure -j1 \
  -R '^(app_schema|app_opponent_skill_choices|app_ui_policy|video_config|video_config_runtime|enhancement_registry)$'
```

Result: `100% tests passed, 0 tests failed out of 6`. Removing only the new
option table and its routing, then rebuilding, makes
`app_opponent_skill_choices` fail with `Required regular expression not found`.
Restoring and rebuilding passes all six again. The schema dump enumerates the
actual `optionsFor()` route, including the choices and labels, rather than an
independent hard-coded claim. This proves widget selection, not a new physical
controller acceptance run.

**Remaining acceptance:** physical-controller navigation and selection on the
packaged candidate, followed by release availability.

## #60 — Skip launcher after setup

[Report](https://github.com/akratch/goldenballoon/issues/60): optionally enter
the game directly when opening the executable.

**Implemented in the candidate, not yet released.** `Launcher.SkipWhenReady`
is opt-in; Shift or both controller shoulders restore the launcher. Invalid
ROMs and failed-boot recovery must not be bypassed.

**Verification (optimized native build).** On a dedicated test desktop:

```sh
MDKR_DEDICATED_TEST_DESKTOP=1 MDKR_APP_TESTS_ALLOWED=1 MDKR_AUDIO=0 \
  python3 tests/check_launcher_skip.py --build build-rel --rom "$ROM" -v
```

Result: `PASS launcher skip: arms=9 boots=2 stays=7 controls=2`. Covers enabled,
default-off, Shift, two shoulders, one shoulder, late hold, invalid ROM,
controller teardown ordering, and failed-boot recovery. These are native
launcher decision/dispatch tests, not proof of a shipped Windows executable.

**Remaining acceptance:** Windows/package proof of direct launch, the escape
gestures, invalid-ROM refusal, and failed-boot recovery on the final artifact.

## #61 — Three separate split-screen symptoms

[Report](https://github.com/akratch/goldenballoon/issues/61): Walrus Cove's
purple viewport after the loop/tunnel, black sky edges on multiple tracks, and
lower-resolution pause UI when player two pauses.

- **Sky edges: fixed and rechecked in the candidate.** See
  [the mechanism and mutation control](renderer.md#fixed-the-split-screen-sky-quad-is-a-43-object-in-a-widescreen-frustum--issue-61).
  `check_split_screen_backdrop.py --build build-rel --rom "$ROM" -v` passes:
  4:3 derives the original `200x150` extent, 16:9 derives `267x150`; worst
  sky-band black coverage is 0.16% for P1 and 4.27% for P2, below the respective
  1.5% and 5% ceilings.
- **Purple viewport: unresolved acceptance.** Must reproduce the reported
  two-player Walrus Cove loop/tunnel route and inspect both viewports. A
  single-player cave flash or a Fossil Canyon sky-edge pass does not settle it.
  The subsequent dense investigation below isolates a blue obstruction on
  this route and qualifies a local candidate, but does not yet establish
  equivalence to the reporter's Windows/NVIDIA purple symptom.
- **P2 pause resolution: automated macOS acceptance passes; Windows remains.**
  The dedicated two-player check now compares both pause owners in Restored
  and Remastered on GL and WebGPU, including real scaled-UI and wrong-font
  controls. This is not a verdict about the reporter's Windows/NVIDIA setup.

**Focused native investigation.** An optimized US 1.1/WebGPU two-player
Walrus Cove run reached 6,200 presentation frames, with P1 on lap 2 and P2
on lap 1. The 100-frame capture sampling did not establish the reported full
purple viewport; inspection did show camera/wall clipping, which needs finer
sampling and a causal comparison, not a blanket retail-fidelity explanation.
Separate same-input two-player Ancient Lake runs paused with P1 or P2 at frame
3,200 and captured at 3,350, using Restored/WebGPU and 2x render scale. Visual
inspection showed matching pause lettering and the expected different panel
colors. This is a current macOS observation, not a Windows/GPU-matrix verdict
or a substitute for a quantitative pause-resolution regression.

**Quantitative pause acceptance (2026-09-05).** The packaged arm64 executable
from clean commit `f1fd70e4c1ff5d14df69c4aa8d8288bf6ff6b10b` passes
`check_split_screen_pause_resolution.py` on both GL and WebGPU: eight arms
per backend, with independent saves/configuration. P1's blue and P2's red
panel prove the requested owner. The common RESTART RACE glyph contours
overlap by 98.29% in Restored and 99.25% in Remastered. Native-output text
edge detail exceeds each owner's scaled-UI control by 36.4%/36.5% in
Restored and 33.3%/33.2% in Remastered. Substituting the original bitmap
font for Remastered P2 gives only 57.16% contour overlap and is rejected.
Both racers' simulation streams remain identical across all eight arms;
output-pass telemetry has no late world draws or begin failures.

The check uses a 960x540-point window (1920x1080 drawable on this Retina
host) and 2x scene rendering. At 720 points, GL's Restored bitmap glyphs
align to an integer pixel grid and the downsampled control has essentially
the same edges; the non-integer source-pixel scale is necessary to make
that control discriminate. No product renderer change or reduced threshold
was used to obtain these passes. The test is registered as serial GPU work
in the main suite. All image evidence remains local and ROM-derived.

Keep the issue open until all three are accounted for.

**Walrus Cove curtain investigation and candidate (2026-09-05).** Every-frame
sampling over frames 2700–3519, with zero dropped captures, establishes a
large flat-blue obstruction in each player's viewport after the loop. The
camera-relative void curtain was drawn after scenery with a depth-writing
opaque material, allowing its artificial 250-unit depth to hide valid bridge
geometry. Enabling Modern camera correction or removing the activation
plane's 14-unit padding does not fix this reproducer. Removing the curtain
exposes the bridge but also removes its intended coverage over holes.

The candidate draws the same curtain before scenery without depth writes in
Restored/Remastered; Pure retains the authored final pass. Geometry, activation
rules, and shadow-caster exclusion remain intact. The registered serial
`check_split_screen_void_coverage.py` passes all 20 US 1.1 arms on local
shipping-SDL2 GL/WebGPU: Restored and Remastered recover valid scenery for
both players, Pure preserves ten byte-identical authored captures per backend,
and every policy comparison preserves SIMHASH v3 and both racer streams.
At frame 3061 P1's exact flat-colour coverage falls from 84.86% to 5.67%; at
frame 3100 P2 falls from 83.03% to 4.64% in Restored. The retained pixels
disagree with the no-curtain control, proving coverage was not simply deleted.
The identical acceptance predicate rejects authored, deleted-curtain, and
early-but-depth-writing controls for each player and backend. Synthetic
controls also reject an unrelated-colour replacement for actual scenery.

**Neighboring gates completed locally.** The saved `neighbor-gates-final.log`
records PASS for `world_fx_matrix`, `render_purity`,
`camera_snapshot_coverage`, `split_screen_backdrop`,
`split_screen_pause_resolution`, and `split_screen_void_coverage`: six selected
tasks out of the then-271-task suite. This covers representative 1P/2P/4P
world-effect ownership, render/simulation separation, multiplayer and cutscene
camera snapshots, and the three focused split-screen candidates. It does not
complete the full renderer, waterfall, or release matrix.

The same log emits stale-artifact warnings, including that the native binary
predates `cmake/tests.cmake`. Preserve these passes as local candidate evidence;
they do not certify the current source tree or final package. Rebuild the final
candidate and repeat the required gates on an authorized dedicated test desktop.
European revision coverage and Windows/NVIDIA acceptance, including equivalence
to the reported purple symptom, remain pending. All captures and raw logs remain
private, ROM-derived evidence.

**Fresh candidate follow-up (2026-09-06).** The rebuilt native game passes
all 20 US void arms again. The initial European probe misses P1's obstruction
at the US witness frame and remains a failed qualification attempt. A dense
European survey fixes its witnesses at frame 2980 for P1 and 3058 for P2.
All 20 European arms then pass the same pixel thresholds and broken-render
controls, with identical authoritative streams and twelve byte-identical Pure
captures per backend. This follow-up does not establish Windows/NVIDIA
acceptance or the full European release matrix.

## #58 — Bonus portraits and NDS content

[Report and clarification](https://github.com/akratch/goldenballoon/issues/58):
Taj's results portrait should resemble a blue elephant, with portraits also for
Terry and Wizpig; separately, the reporter asks about Dixie, Tiny, and NDS tracks.

**Portrait correction implemented and verified in the candidate, not released.**
The first-party procedural Taj card now has blue elephant ears, face and trunk;
no extracted portrait is bundled. Inspection of the actual two-player Rankings
capture confirmed the changed appearance beside the unchanged Diddy card.

`check_taj_results_portrait.py` and `check_taj_hud_portrait.py` both pass on
optimized native WebGPU and OpenGL. Results coverage includes two races and a
full intervening menu/stage teardown; the Adventure HUD check preserves the
lead player. The new face assertions reject both captures from the old tan
binary. A separate pixel control erases the gold band/jewel and must fail the
gold assertion, correcting the former detector's accidental reliance on tan
face pixels. Both gates now default to WebGPU and expose `--renderer gl` for
the diagnostic comparison.

**Existing packs remain compatible.** `check_bonus_portrait_pack.py` passes
all nine native arms, including existing Taj packs on GL and WebGPU, the
current digest in the author dump, unchanged Wizpig/Terry replacements, and
byte-identical stock rendering with packs disabled. `mod_texture_store` pins
current-name precedence, toggle behavior, and isolation from unrelated digests.
Removing only the alias makes its compatibility assertion fail; restoring it
passes. That unit also passes under ASan/UBSan. A freshly rebuilt UBSan binary
passes `check_array_bounds_sweep.py --no-build`, including its eight routes,
static class sweep and boundary controls.

Character Workshop and portrait-pack support do not by themselves implement
Dixie, Tiny, or any NDS track. Record those requests separately from the portrait
fix, respect the repository's no-ROM-derived-assets boundary, and do not close
the entire issue by citing the existence of an import UI.

**Remaining scope decision:** the maintainer must explicitly decide whether
Dixie, Tiny, and NDS tracks belong in this release or remain future work. None
of those content requests is implemented by the portrait candidate.
