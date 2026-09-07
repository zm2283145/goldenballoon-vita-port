# Public issue follow-up — 2026-09-06

Read-only GitHub API refresh of `akratch/goldenballoon` found exactly five open
issues: [#58](https://github.com/akratch/goldenballoon/issues/58),
[#60](https://github.com/akratch/goldenballoon/issues/60),
[#61](https://github.com/akratch/goldenballoon/issues/61),
[#62](https://github.com/akratch/goldenballoon/issues/62), and
[#63](https://github.com/akratch/goldenballoon/issues/63). No new issue was
present. No issue, comment, label or release was modified.

This follow-up inspected the dirty release worktree based on `c657fa627eaa`.
It supplements, rather than replaces, [the acceptance ledger](github-issues.md).
Source confirmation and compiled fixtures do not establish packaged acceptance.

## New implementable findings

### 1. Launcher escape gesture lost controller identity — #60

**Confirmed source defect; candidate corrected in this follow-up.**
`AppLaunchHold_sample()` independently accumulated left and right shoulder
states across every controller. One shoulder held on each of two different
controllers therefore satisfied the documented same-controller escape gesture.

The sampler now recognizes a shoulder pair only on the same attached device.
Keyboard Shift and the scripted raw-input test seam remain unchanged.

### 2. Launch-time controller snapshot missed late attachment — #60

**Confirmed source gap; candidate corrected in this follow-up.**
`openPads()` stopped enumerating after its first sample, although the launcher
continues sampling throughout ROM validation. A controller attached during that
window could not supply the escape gesture.

The sampler now makes one bounded enumeration per sample, retaining connected
borrows by SDL instance ID and releasing detached ones. Reordered device indices
do not reopen an existing borrow. An index whose identity changes between
enumeration and open is closed and retried on the next sample. If storage for a
new borrow throws, its newly opened handle is closed before propagating failure.
There is no retry loop, new background worker or per-frame reopening of all pads.

`tests/test_app_launch_hold.cpp` exercises the actual production sampler against
a deterministic SDL boundary, without real devices or a window: split-device
shoulders, same-device pairing, independent Shift, late attachment, stable
handles, removal/index changes, replacement, enumeration/open identity change,
idempotent release and balanced ownership. Syntax checks pass. Runtime results
must be recorded separately; no fixture was executed by this audit.

Required acceptance remains the real SDL/controller path and the final Windows
package's direct boot, escape, invalid-ROM refusal and failed-boot recovery.

### 3. Opponent-skill displayed value can disagree with its effective choice — #62

**Confirmed source-level presentation mismatch; corrected by the integrated
follow-up, behavioral qualification pending.**
`platform/video_config.c:mdkr_video_config_set()` retains raw opponent-skill
strings. `platform/enh_ai_difficulty.c:ai_difficulty_resolve()` accepts the named
arms case-insensitively and deliberately treats unknown/empty legacy values as
`authored`. That fallback is an explicit compatibility contract, not a newly
discovered gameplay validation failure.

In the previous UI, `platform/app/ui_settings.cpp:drawKey()` matched choice
values with case-sensitive `strcmp` and displayed unmatched raw text. A supported
`HARD` configuration therefore showed an unlabelled raw value with no selected
entry; a typo showed the typo instead of the effective Original choice.
`optionLabel()` had the same literal-match behavior.

Fix requirement: share a canonical/effective difficulty interpretation between
the gameplay resolver and UI choice/label presentation. Preserve restart staging,
environment locks and the intentional unknown-to-authored fallback. Do not silently
save over a user's configuration during rendering. Add mixed-case, empty,
unknown, staged and environment-locked regression cases, then check both actual
settings surfaces. Keep this separate from the already-implemented three-choice
widget and its historical six-test pass.

Integrated candidate: `enh_ai_difficulty_value.h` shares the original ASCII
case/fallback rules between gameplay's existing latch and UI snapshot formatting,
option labels and selected choices. It never reads or writes settings itself.
The C fixture enumerates all 336 case variants across the canonical names,
fallback/nonmutation and independent current/desired snapshots. Four source
bindings cover gameplay, visible/spoken selection, raw staging and source locks.
These fixtures compile/parse separately from behavioral execution; no rendered
or physical-controller pass is inferred.

## Public-state correction needed in the main ledger

The main ledger says #63 had no comments at its refresh. The current API now
shows a maintainer comment at `2026-09-06T11:47:07Z` confirming the helper defect
and promising the next release. The issue is still open. This is a public status
update, not evidence of new package validation or authority for an agent to close
the issue.

## Reviewed paths without a newly confirmed defect

- **#63:** renewed inspection of the five-field reader, guarded upgrade-only
  award, per-field best-medal merge, Tracks fifth-field read, save-domain tests,
  and trophy status counter found no additional incorrect fifth-world exclusion.
  Four-mainland unlock masks and the authored four-icon status layout remain
  intentionally distinct from five-field storage. The new exhaustive helper and
  save round-trip tests, real championships, cabinet/Tracks rendering and final
  artifacts still need their appropriate behavioral evidence.
- **#62:** the other enumerated string-choice routes were inspected alongside
  schema parsing. No additional currently missing choice table was confirmed.
  Open path/FOV/aspect fields must not be turned into closed lists by a mechanical
  sweep. The new mismatch above concerns effective-value presentation.
- **#60:** final ROM validation, one-way disarming, Workshop/quit ownership,
  failed-boot recovery visibility and controller release-before-SDL teardown
  remain present. The two newly corrected input gaps were outside the historical
  scripted-hold tests because those inject aggregate states without real devices.
- **#61:** source review is not a reproduction of the reported Windows/NVIDIA
  purple viewport. Keep its three symptoms separate; local sky/pause/void
  evidence cannot establish another platform's equivalence. Obtain that evidence
  before deciding whether further renderer changes are necessary.
- **#58:** Taj, Wizpig and Terry portrait paths exist in `game/src/menu.c`;
  the earlier Taj candidate has local evidence. The issue also requests Dixie,
  Tiny and NDS tracks, which the current candidate does not implement. Workshop
  import support is not those assets. A maintainer content/scope decision and
  compliant asset plan remain distinct from code correctness or hardware access.

## Priority and completion discipline

1. Integrate and execute the new launcher sampler fixture; qualify real input
   and final packaged #60 flows.
2. Correct and test the #62 effective-choice presentation mismatch.
3. Execute the #63 correction and negative controls, then qualify five-world
   championship persistence, visible cabinet/Tracks results and existing saves.
4. Complete #61's independent platform/backend symptom qualification and #62's
   physical-controller acceptance. Do not invent source changes in place of
   missing observations.
5. Resolve #58's explicit content scope before making an entire-issue completion
   claim. Any eventual GitHub mutation still requires maintainer approval.

This sweep introduces no paid dependency, infrastructure or operating expense.
Validation was source/syntax-only by assignment, not because the maintainer's
workstation authorization was absent; the current replacement AGENTS policy
explicitly authorizes local validation, subject to actual tool controls.
