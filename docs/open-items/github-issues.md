# Public issue acceptance

This register separates implementation, verification, and release availability.
An existing feature or a passing test for one symptom is not evidence that an
entire public issue is resolved. The repository inventory on 2026-09-05 found
four open issues in `akratch/goldenballoon`, and none in `akratch/mdkr64` or
`akratch/goldenballoon-staging`.

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
- **P2 pause resolution: unresolved acceptance.** Must compare pause initiated
  by P1 and P2 under the same presentation/backend settings. The existing
  single-player HUD-resolution check does not exercise that distinction.

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

Keep the issue open until all three are accounted for.

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
