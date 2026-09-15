# Adventure Party new-game shared-scene evidence

Date: 2026-08-31

## Release contract

An admitted 2–4-player party may start an empty Adventure file. File Select
forms the exact joined roster and applies `START_NEW_GAME` before the authored
one-player cinematic. At its natural end, `SCENE_COMPLETE` advances the same
session exactly once before the first hub load. The cinematic returns the stable
party count, allowing the existing hub adapter to restore all racers, split
layout, and seat/controller bindings.

The adapter is compiled only for
`NATIVE_PORT && !MDKR_ADVENTURE_PARTY_OMIT`. A reducer refusal at formation,
scene entry, or completion tears the attempted session down and returns to title
instead of loading a one-player campaign. The reducer's existing quit matrix
proves `SHARED_SCENE -> EXITING -> OFF` remains legal.

## Evidence recorded

| Check | Result |
|---|---|
| Normal native build (`cmake --build build -- -j4`) | PASS |
| Compiled-out build (`cmake --build build-omit -- -j4`) | PASS |
| Compiled-out `menu.c.o` symbol audit | PASS; no `adventure_party_menu_*` adapter symbols |
| Adventure boundary inventory | PASS, 143 occurrences / 101 entries |
| State reducer + runtime unit suites | PASS |
| Admission checker Python compile | PASS |
| Synthetic checker valid fixture | PASS |
| Synthetic missing-scene, missing/duplicate-completion, collapsed-count, missing-layout, missing-binding, abort, and 1P-leak mutations | all rejected |
| Whitespace validation on owned files | PASS |
| ROM-backed 3P new-game + 1P stock control | PASS; focused 14,000-frame serial run |
| Embedded focused-gate mutations | PASS; missing scene, missing completion, duplicate completion, and collapsed hub were all rejected |

## Exact focused commands

```sh
cmake --build build -- -j4
cmake --build build-omit -- -j4
python3 tests/check_adventure_party_boundaries.py
build/mdkr_adventure_party_state_test
build/mdkr_adventure_party_runtime_test
python3 tests/check_adventure_party_admission.py --build build --rom baserom.us.v80.z64 --newgame-only -v
# Final integrated admission matrix:
python3 tests/check_adventure_party_admission.py --build build --rom baserom.us.v80.z64 -v
```

The final admission command includes the existing 2/3/4-player resume matrix,
3/4-player enhancement-off controls, the one-player existing-save control, the
new 3P empty-file shared-scene route, the one-player empty-file stock control,
copy/erase authority guard checks, and five mutation controls.
