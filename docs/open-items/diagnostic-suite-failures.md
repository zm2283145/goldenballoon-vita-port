# Completed diagnostic suite: failure disposition

Status: **not release qualification**. The clean-source `12cab88c` diagnostic
attempt finished all 271 tasks in 626m50s: 235 passed, 36 failed, process exit 1.
This inventory accounts for every failed task, not every individual assertion.
The nested native CTest task ran 321 tests and failed one of them.

Evidence is the privately retained `full-suite-12cab88c.log`; the task numbers
and line references below identify its observations without copying private
ROM-derived traces, captures, save contents, paths, or artifacts into this file.
Current-source inspection was performed on the newer dirty release worktree on
2026-09-06. A current source correction does not retroactively pass the diagnostic
attempt. No tests, applications, builds, or network services were run for this
triage. Later evidence must identify the exact source and artifact revision.

## Confirmed native artifact-routing failures (15 tasks)

The recorded commands passed `--rom` but omitted `--build`; the runner had already
selected an external native executable. These checks therefore searched an absent
default build and did not reach their behavioral assertions. The current manifest
classifies all 15 as `native`, and `command_for()` passes the selected executable.
The broader prior correction involved 16 engine-using tasks: `rom_checker_page`
was not one of the 36 failures and is deliberately absent from this table.

| Task | Failure evidence | Cause confidence | Current fix location | Required proof |
| --- | --- | --- | --- | --- |
| 12 `enhancement_authority` | L15–17: default executable missing | Confirmed routing omission | `tools/run_checks.py`: native role and `command_for()` | Rerun authority gate against the selected candidate binary. |
| 13 `subentry_bounds` | L18–20: default build missing | Confirmed routing omission | `tools/run_checks.py`: native role | Prior focused rerun is recorded in the release checklist; include again in complete candidate qualification. |
| 14 `dev_tools_purity` | L21–23: default build missing | Confirmed routing omission | `tools/run_checks.py`: native role | Run actual purity assertions with the selected binary. |
| 15 `a11y_race` | L24–27: argument admission rejects missing executable | Confirmed routing omission | `tools/run_checks.py`: native role | Run race accessibility gate on the selected artifact. |
| 16 `input_hotplug` | L28–30: default build missing | Confirmed routing omission | `tools/run_checks.py`: native role | Prior focused rerun is recorded; include in complete qualification and retain separate physical-device acceptance. |
| 18 `rom_text_indices` | L32–34: no default build | Confirmed routing omission | `tools/run_checks.py`: native role | Complete text-index behavioral checks with the candidate. |
| 19 `a11y_shell` | L35–39: argument admission rejects missing executable | Confirmed routing omission | `tools/run_checks.py`: native role | Prior focused rerun is recorded; complete qualification still required. |
| 20 `future_fun_land` | L40–42: default build missing | Confirmed routing omission | `tools/run_checks.py`: native role | Run the gate, plus the separate issue #63 trophy award/save/reload acceptance. |
| 21 `mod_music_override` | L43–45: build executable not found | Confirmed routing omission | `tools/run_checks.py`: native role | Run actual override behavior and restoration checks. |
| 22 `enh_ai_difficulty` | L46–48: default build missing | Confirmed routing omission | `tools/run_checks.py`: native role | Run gameplay difficulty checks; this does not replace issue #62 controller acceptance. |
| 23 `crash_screen` | L49–51: default build missing | Confirmed routing omission | `tools/run_checks.py`: native role | Complete normal diagnostic-screen gate with the candidate. |
| 139 `enh_speedometer` | L911–914: no default executable | Confirmed routing omission | `tools/run_checks.py`: native role | Run the enhancement gate with the selected binary. |
| 140 `enh_draw_distance` | L916–919: no default executable | Confirmed routing omission | `tools/run_checks.py`: native role | Run actual draw-distance assertions. |
| 141 `mod_texture_override` | L921–924: build executable not found | Confirmed routing omission | `tools/run_checks.py`: native role | Run texture override acceptance with current decoder hardening. |
| 142 `tool_freecam` | L926–929: default build missing | Confirmed routing omission | `tools/run_checks.py`: native role | Run the free-camera gate and complete candidate suite. |

## Native assertions, camera, pacing, and shadow (4 tasks)

These failures cannot all be attributed to missing artifacts. In particular,
camera re-engagement and pacing re-anchors are measured hard failures, and a
timed-out shadow route does not establish either simulation parity or divergence.

| Task | Failure evidence | Cause confidence | Current fix or investigation location | Required proof |
| --- | --- | --- | --- | --- |
| 39 `camera_motion_quality` | L125–127: one Ancient Lake re-engagement inside the hard window; one 3P+T.T. continuous-surface shoulder-flip report | Mixed: shoulder measurement correction is source-backed and its later route evidence is recorded in the checklist; the remaining re-engagement root cause is unresolved | `game/src/camera_obstruction_runtime.c`: `mdkr_camera_shoulder_sample()` separates basis crossings; new level-1 event evidence does not alter the resolver. `tests/check_camera_motion_quality.py` preserves per-arm events | Fix and rerun the real re-engagement with hard thresholds unchanged; requalify all three routes and high-rate arm. The separately documented proportional recovery cap/calibration gap remains open. |
| 135 `rom_free_units` | L829–832, L873–884: 320/321 nested tests passed; `app_online_lobby_takeover` prints seven active cases while CMake requires nine | Confirmed stale CTest success count, not a native unit assertion crash | `CMakeLists.txt`: current seven-case success predicate; `tests/check_online_lobby_takeover.py`: seven active states; `tests/check_ci_contract.py`: inventory/count binding | The checklist records a subsequent focused lobby/CI-contract pass. Rerun the current full nested inventory, including new lifecycle and ownership fixtures. |
| 178 `pacing_quality` | L1309–1313: final non-strict companion retry reports 43 re-anchors against a limit of three | Hard measurement confirmed; underlying pacing cause unknown. Printed strict-arm distributions did not describe the failing companion | `tests/check_pacing_quality.py`: per-attempt evidence and mandatory realtime integrity reporting; `tests/test_pacing_quality_reporting.py`. No product pacing fix or threshold waiver established | Capture the failing companion's own evidence, identify and fix the cause, then pass realtime/companion integrity and pacing limits. A missing-baseline exemption cannot excuse this failure. |
| 243 `widescreen_shadow_asan` | L2048–2064: all six arms exit 124, final shadow reports absent; partial stream lengths differ | Timeouts and incomplete routes confirmed; reason for exceeding the budget unknown. Unequal truncated streams are not proof of simulation divergence | `tests/check_widescreen_shadow.py`: explicit completion and complete-trace predicates, private per-arm evidence, comparison unavailable for incomplete runs; `tests/test_widescreen_shadow_reporting.py`. No renderer fix established by that reporting change | Finish all six original routes under ASan, retain complete diagnostics, check real full-stream parity and the depth/overflow controls, and investigate timeout cause without waiving correctness assertions. |

## Browser/native artifact routing (4 tasks)

The three driver routes now pass the selected build directory. Gallery inventory
instead consumes the selected game executable itself; it must not be treated as
a sibling driver. Current mock regressions cover these distinct routes and early
missing-artifact admission, including exact executable names and Windows suffixes.

| Task | Failure evidence | Cause confidence | Current fix location | Required proof |
| --- | --- | --- | --- | --- |
| 257 `browser_online_room_gallery` | L3267–3270: no `--build`; launch fails with missing default `build` | Confirmed runner omission, independently reproduced by source tracing | `tools/run_checks.py`: separate `NATIVE_BROWSER_BINARIES` route/preflight; `tests/test_run_checks_artifact_routing.py`: gallery executable/shell/admission cases | Execute the routing fixture, then the 43-case native/browser gallery, keyboard, touch, accessibility and zoom checks on matching artifacts. |
| 264 `party_native_e2e` | L3302–3305: no selected build argument; default sibling driver missing | Confirmed routing omission; selected build's driver availability was not established by this failure | `tools/run_checks.py`: `NATIVE_BROWSER_DRIVERS`, command routing and early admission; standalone script honors executable suffix | Build/admit the actual native driver and run the full local Worker/browser/native path with matching staged shell. |
| 265 `online_live_transport_e2e` | L3307–3310: no selected build argument; default online driver missing | Confirmed routing omission; no live match behavior exercised | Same runner driver route; `tests/check_online_live_transport_e2e.py` suffix handling | Run actual two-process local multiplayer and shutdown acceptance with the current transport/owner fixes. |
| 266 `party_lan_e2e` | L3312–3315: no selected build argument; default party driver missing | Confirmed routing omission; LAN behavior not exercised | Same runner driver route; `tests/check_party_lan_e2e.py` suffix handling | Run native embedded-LAN/browser acceptance and close/reconnect behavior with no cloud dependency. |

## Local Worker startup and dependency admission (5 tasks)

Three tasks explicitly identified a missing lockfile-pinned Wrangler. Two only
reported an early Worker exit. They share `check_party_capacity.start_worker()`
and the same launcher dependency, so a common cause is plausible, but the combined
log does not contain their Worker stderr. Do not promote that inference to a
confirmed cause or claim any service assertion passed.

The current shared helper now checks the pinned entrypoint before command
construction or process creation, and retains ownership until readiness succeeds.
If startup fails, it attempts cleanup before reporting the failure and explicitly
reports an unsuccessful cleanup. The firewall helper uses the same ownership path
with its existing cleanup callback. Admission, service arguments, and time budgets
are not relaxed. The new `party_worker_startup_reporting` CTest uses mocked
process/network boundaries; it is registered but its assertions remain unexecuted.

Startup reporting scans at most 16 KiB from the current attempt and emits only
fixed category labels and bounded metadata into a private diagnostic file; raw
Worker output, commands, environment values, and exception chains are not copied
into that report. The shared implementation is now in
`tests/party_worker_reporting.py`. Two-person and chaos aggregate failures also
use fixed categories and owned source locations instead of raw exception strings
or log tails. Full-log error detection still scans the original log; bounded
reporting does not weaken that verdict. The corresponding `party_worker_reporting`
CTest is registered but unexecuted. Unknown categories remain unclassified, not
presumed successes.
These diagnostic/ownership corrections do not retrospectively identify either
historical early-exit cause. The runner now admits the pinned Wrangler entrypoint
before later staged-artifact or subprocess checks for its exact seven Worker-backed
checks, explicitly excluding the cloud-free LAN route. Mocked admission coverage
is implemented but remains unexecuted, as do the shared startup regressions.

| Task | Failure evidence | Cause confidence | Current fix or investigation location | Required proof |
| --- | --- | --- | --- | --- |
| 254 `party_experience_canary_smoke` | L3252–3255: Worker exits 1 before readiness | Startup failure confirmed; specific cause unknown | `tests/check_party_capacity.py`: shared `require_worker_dependency()`, `start_worker_command()` ownership/cleanup, and `startup_diagnostic()` fixed-category evidence, inherited by the canary caller | Validate mocked admission/ownership/reporting first; provision pinned dependencies and establish actual startup behavior before the local canary smoke. Historical cause stays unknown without retained evidence. No operated/public service or paid endpoint is authorized by this local task. |
| 259 `browser_online_two_person` | L3277–3280: pinned Wrangler script missing | Confirmed missing service dependency | `tests/check_browser_online_two_person.py`: artifact admission; `services/party/package-lock.json`: dependency identity; `tools/run_checks.py`: `PARTY_WORKER_CHECKS` and `PARTY_WRANGLER` early admission candidate | Install from the lockfile in the qualification checkout, verify early runner admission, then prove two real local profiles' complete flow. |
| 260 `party_capacity` | L3282–3285: missing lockfile-pinned Wrangler | Confirmed missing service dependency | `tests/check_party_capacity.py`: dependency admission now enforced by both shared startup entrypoints; `tests/test_party_worker_startup.py`: registered mocked regressions | Execute the new regressions, admit pinned local tools before the suite, and complete existing defensive capacity/restart checks without changing budgets. |
| 261 `party_service_chaos` | L3287–3290: Worker exits 1 before readiness | Startup failure confirmed; exact cause unknown | `tests/check_party_service_chaos.py` inherits shared startup admission, ownership cleanup and privacy-safe diagnostics from `check_party_capacity.py`; no service-code fix established by this log | Validate helper behavior, retain safe startup categories and investigate unresolved failures privately, then complete existing local service resilience checks. Do not infer the historical cause solely from neighboring failures. |
| 262 `party_firewall_negative` | L3292–3295: missing lockfile-pinned Wrangler | Confirmed missing service dependency | `tests/check_party_firewall_negative.py`: shared admission and `start_worker_command(..., stop=stop_worker)` preserve firewall-specific command/cleanup behavior; mocked delegation coverage added | Execute delegation/cleanup regressions, provision pinned local tools, then validate the existing bounded offline recovery flow. |

## Browser engine progress and startup (8 tasks)

Several engine-bearing browser gates reached `main-started`, presented one frame,
then made no observed progress before timeout. Engine-free browser/controller and
shared room-module neighbors did pass. That distinguishes the affected surface;
it does **not** isolate a browser scheduler, GPU, wasm, or application root cause.
Likewise, magic-code acceptance timeouts do not independently prove broken unlock
or persistence behavior when the engine has not advanced to those assertions.

| Task | Failure evidence | Cause confidence | Current fix or investigation location | Required proof |
| --- | --- | --- | --- | --- |
| 249 `browser_resource_plateau` | L3227–3230: four-generation cycle timeout; one frame and no completed engine runs | Engine progress failure confirmed; underlying cause unknown | `tests/check_browser_resource_plateau.py`; investigate browser frame/engine scheduling and current renderer diagnostics. No confirmed fix from this attempt | Restore sustained progress, then complete all four generations and verify actual resource plateau and cleanup. |
| 250 `persistent_browser_session` | L3232–3235: first race never returns to live launcher; one frame | Early engine progress failure confirmed; persistent-session cause not established | `tests/check_persistent_browser_session.py`; engine/browser handoff path | Complete both engine epochs and launcher returns in one session, including persistence and teardown. |
| 251 `touch_controls` | L3237–3240: no matching pad observation before timeout; trace ends after initial presentation | Observation timeout confirmed; gesture/input defect not isolated from stalled engine | `tests/check_touch_controls.py`; browser engine progress and input observation path | First show advancing engine observations, then pass the actual multi-touch, steering and neutral-release assertions. |
| 267 `browser_presentation_rates` | L3317–3321: first finite `web-original` run times out at `main-started` | Engine startup/progress timeout confirmed; policy correctness unmeasured | `tests/check_browser_presentation_rates.py`; browser scheduler and renderer path | Complete original and subsequent policy arms; verify rate/authority invariants using actual finished runs. |
| 268 `browser_taj_character_select` | L3325–3366: unlock acceptance timeout; tail ends after initial presentation | Engine progress symptom confirmed; unlock/selection root cause unknown | `tests/check_browser_taj_character_select.py`; browser engine startup path | Complete unlock, actual picker actor/identity/animation and clean teardown acceptance after progress is restored. |
| 269 `browser_taj_persistence` | L3370–3411: unlock acceptance never observed; same early trace pattern | Timeout confirmed; no persistence-loss finding established | `tests/check_browser_taj_persistence.py`; engine startup before persistence path | Prove unlock, durable write, fresh-session restoration and clean exit. |
| 270 `browser_magic_codes_persistence` | L3415–3456: expected code acceptance never observed; early engine trace | Timeout confirmed; no saved-code defect isolated | `tests/check_browser_magic_codes_persistence.py`; engine startup before code/persistence assertions | Complete code acceptance and intended durable/reload checks with an advancing engine. |
| 271 `browser_runtime` | L3458–3461: frame-400 wait times out at one frame; injected overlay failure is logged as handled | Engine progress failure confirmed; relationship to handled overlay fault unknown | `tests/check_browser_runtime.py`; current WebGPU/browser diagnostic work. A handled fault message is not proof of recovery to later frames | Reach the full frame budget, complete the existing fault-recovery/save/audio/cleanup assertions, then rerun affected neighbors. |

## Remaining cross-cutting work

- Execute corrected routing/admission regressions before another long attempt;
  the gallery fix and three native-driver fixes are source candidates until tested.
- Validate shared Worker admission, startup ownership cleanup, privacy-safe
  diagnostics and runner-wide early toolchain admission. These source candidates
  do not establish a successful local Worker run or excuse service gates.
- Resolve Ancient Lake motion and pacing hard failures using per-arm evidence;
  reporting improvements and the shoulder measurement correction do not clear them.
- Complete shadow routes and diagnose the browser engine stall before claiming
  renderer, persistence, character selection, or input acceptance from those gates.
- Run the complete current candidate suite and final package/platform acceptance.
  This 36-row inventory neither replaces the issue ledger nor clears independent
  KTX2 containment, hardware, packaging, content-scope, or release approval gates.

Related release status and later evidence: [release checklist](../RELEASE_CHECKLIST.md).
