# Inherited-red and package audit — 2026-08-31

This record separates feature regressions from repository and qualification
work that still blocks a public candidate. The feature branch base inspected
here is `774adf245b11b5d43d8a64c1c8ddca3ada7cedcc`; the available public main
reference is `395e199d5d73af943a6811461ccd3b0ee607dfd2`.

The public main reference is the `v1.5.2` source commit `fabec5e5` plus one
README-only commit. Existing `fabec5e5` behavioural logs are therefore valid
for classifying engine behaviour on the current public reference, but they do
not replace the final post-integration run.

## Current source and legal gates

| Gate | Result | Evidence |
|---|---|---|
| CI contract, feature tree | PASS | `python3 tests/check_ci_contract.py`; 20 immutable action pins |
| CI contract, public main archive | PASS | Same command against a `git archive github/main`; 17 immutable action pins |
| Clean-room history | PASS | `tools/check_clean_room.sh`; 9,132 historical blobs over 1 KiB scanned, no ROM header or blob over the 4 MiB review threshold |
| Release source policy fixtures | PASS | `check_release_ready_web_provenance.py`, `check_release_local_only_surface.py`, `check_release_party_origin.py`, and `check_player_prose.py` |
| Third-party notices and package script syntax | PASS | `python3 tools/check_third_party_notices.py` plus `bash -n` on the Windows and Linux packagers |
| Full source-readiness guard | BLOCKED | After exact fuzz-seed allowlisting, every section passes except the public-tree and reachable-history checks for `docs/HANDOFF-consolidated-branch.md` |

The tracked-binary finding was a release-harness defect. Seventeen existing
wire-protocol fuzz seeds and six new custom-character asset seeds are
intentional exact-byte test inputs. They are now individually reviewed in
`tools/public_tracked_asset_allowlist.txt`; a temporary-index replay of
`tools/ci/check_release_ready.sh` reports `OK -- no tracked binary-looking
files found`. This remains fail closed for any new path.

## Reachable-history disposition

`tools/check_public_surface.py --release-history` scans every blob version in
every commit reachable from the release revision. In this checkout no `public`
remote is configured, so the release revision is `HEAD`. A deletion or revert
cannot repair the current finding because commit `774adf24` remains reachable.
A normal merge of this history is therefore not publishable.

Two safe release constructions exist:

1. Create the final candidate as a squash commit on the then-current public
   main, with the coordination artifact excluded. This gives the smallest
   publication surface, but intentionally omits the feature branch's granular
   commit ancestry from the public graph.
2. After all feature work is complete, create a protected backup ref, rewrite
   only the unpublished feature lineage to drop `774adf24`, and replay its
   descendants. This preserves the earlier feature commits but changes every
   later commit id. It must be done on a dedicated final-integration branch,
   never while qualification is in flight.

For either construction, prove that `774adf24` is not reachable and run both
`python3 tools/check_public_surface.py --history HEAD` and
`tools/ci/check_release_ready.sh` on the exact final commit. A merge followed by
a revert is not an option. Unless preserving the granular feature ancestry is a
release requirement, the squash candidate has the lower publication risk: its
single tree and parent are straightforward to inspect, and the exact squashed
commit can be rebuilt and qualified before it is merged.

## Previously documented red gates

| Check | Current classification | Required release action |
|---|---|---|
| `check_ci_contract` | Resolved. Both trees pass. | Keep it green after manifest and release-document updates. |
| `race_2p_split_enhanced` | Reproduced on the feature tree and the `v1.5.2` baseline. P2 stalls before the required second lap while P1 finishes. This is behavioural, not a timeout. A scoped unstick experiment improved P2 from checkpoint 12/lap 0 to checkpoint 33/lap 1 but still failed the contract. | Do not waive or lower the lap contract. Stabilize the enhanced-cadence automated driving route or fix the enhanced gameplay path, then pass the existing movement, viewport, finish, and post-race assertions. |
| `taj_speed_profile` | Feature-tree combined run passed in 7m25s. The earlier main failure was three 90-second subprocess timeouts in a Debug binary, not an assertion failure. | A 180-second bounded subprocess watchdog is now used. Re-run on the exact optimized candidate. |
| `taj_visual_lifecycle` | Feature-tree combined run passed in 10m11s. Earlier main evidence was host-speed sensitive. | A 180-second bounded race-arm watchdog is now used. Re-run on the exact optimized candidate. |
| `taj_playable` | Resolved as a host-speed false red. With a 240-second bounded subprocess watchdog, the full unlock/select/carpet/restart/2P/3P/4P/vehicle/persistence/audio/Time-Trial matrix passed in about 10 minutes. The run overlapped another GPU route for part of its duration, so it is supporting evidence rather than final qualification. | Keep the 3,600-second suite budget and repeat serially on the exact candidate. |
| `taj_challenges` | One 12,000-frame arm exceeded its old 300-second watchdog after earlier arms had run for about 18 minutes. No behavioural assertion is implicated by that log. | A 600-second per-process watchdog and 7,200-second suite budget are now used. The complete 15-arm check must pass. |
| `taj_vehicle_sweep` | The old 30-minute runner budget killed a healthy, steadily advancing sweep after 20 of 47 combinations on the feature tree and after 13 combinations on the Debug main baseline. | The suite budget is now 7,200 seconds. All 47 combinations must pass; partial progress is not acceptance. |
| `taj_character_select_pal` | Resolved as qualification-input staging. With the separate qualification directory passed through `--roms`, the PAL Rev 1 image (MD5 `6b2bafe540e0af052a78e85b992be999`) completed the modeled, animated, selectable base-roster arm in 24 seconds. | Keep the owned image in the private qualification directory and repeat on the exact candidate. Never copy it into source control or an artifact. |
| `custom_character_roster` | The old assertion required a diagnostic string that production no longer emits. Resolver priority is already override, relocation, preference directory, relative. | Keep the repaired behavioural proof: install an isolated package, require its package id/catalog count/selection/draw witnesses, and keep the missing-package negative arm. |

The timeout changes only enlarge finite host-speed budgets. They do not remove,
weaken, or bypass any behavioural assertion.

## Package and release-document posture

The package manifests already name the Character Workshop importer, validator,
Basis Universal/Zstandard, meshoptimizer, HarfBuzz, and SheenBidi notices. The
CI contract pins those payloads for Windows and Linux. `NOTICE.md` and
`THIRD_PARTY.md` also describe the corresponding source and licenses.

The branch is not versioned or documented as a next release candidate:

- `CMakeLists.txt` still defaults `MDKR_VERSION` to `1.5.1`.
- `RELEASE_NOTES.md` is the shipped 1.5.1 note and explicitly says there are no
  new features or gameplay changes.
- `CHANGELOG.md` has no entry for this feature set.
- `docs/RELEASE_CHECKLIST.md` still hard-codes 1.5.1 artifact names, tags,
  commands, and provenance examples.

After integration with the future main and selection of the actual release
number, update all four in one release-only change. Release notes must remain
player-facing: describe Adventure Party, Character Workshop, compatibility,
up-front limits, and user-visible fixes without internal gate names, test logs,
or implementation notes. Re-run the CI contract and player-prose gate after
the version/docs update.

No source-tree run can supply the remaining artifact evidence. Before release,
the exact packaged candidates still need:

- complete unrestricted optimized, ASan, UBSan, GPU-labelled, and web suites;
- signed or policy-approved clean-machine package acceptance on each supported
  desktop platform;
- physical low/mid/high GPU, keyboard/controller, accessibility, and reduced-
  motion/scale/compact-layout observations for Character Workshop;
- the full Adventure Party platform, renderer, region, campaign, performance,
  and human native-feel matrices;
- per-artifact SHA-256/provenance records and the validated character acceptance
  receipt; and
- a final ROM-absence scan over every exact public archive.

An unobserved required cell remains a release blocker; `not available` is not a
passing result.

## Reproduction commands

```bash
python3 tests/check_ci_contract.py
tools/check_clean_room.sh
tools/ci/check_release_ready.sh

python3 tools/run_checks.py --build build --skip-wasm --skip-instrumented \
  --only 'race_2p_split_enhanced,taj_speed_profile,taj_visual_lifecycle,taj_playable,taj_challenges,taj_vehicle_sweep'

python3 tests/check_taj_character_select.py --build build \
  --rom baserom.us.v80.z64 --roms /path/to/private/qualification/roms \
  --layout base --require-pal
```

The final release evidence must use the unrestricted suite command in
`docs/RELEASE_CHECKLIST.md`; the focused command above is triage only.
