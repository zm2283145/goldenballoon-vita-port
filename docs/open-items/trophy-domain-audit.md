# Trophy and world-domain audit — 2026-09-06

Triggered by [issue #63](https://github.com/akratch/goldenballoon/issues/63).
This is a source-backed class sweep, not a claim of exhaustive game correctness
or an executed runtime reproduction. Candidate changes are not yet released.

## Findings and candidate corrections

| Site | Finding | Correction / coverage |
|---|---|---|
| `game/src/runtime_contracts.c: mdkr_trophy_state` | Accepted only worlds 1–4, excluding world 5 from award, cabinet spawn and model selection | Bound by `WORLD_DINO_DOMAIN` through `WORLD_FUTURE_FUN_LAND`; all 1,024 ten-bit values across five worlds, invalid worlds and null outputs |
| `platform/mdkr_adventure.c: mdkr_trophy_control_world` | Test selector also excluded world 5, leaving the missing championship outside the series matrix | Same five-world enum bounds; added Future Funland's production schedule and gold/persisted-checksum assertions |
| `game/src/menu.c: trackmenu_set_records` | Bitwise OR treats encoded medal ranks as independent flags: bronze plus silver becomes gold | Per-world maximum through `mdkr_trophy_records_merge`; all rank pairs in all five positions, neighbor preservation, identity and gold dominance |
| `game/src/menu.c: dialogue_tt_gamestatus` | Signed trophy mask shifted across 16 fields though only five are stored | Read the five saved fields through the checked helper; retain authored four-icon layout |
| `tests/test_runtime_contracts.c` | Unit explicitly required world 5 rejection | Replace wrong oracle with five-world boundary and full persisted-domain coverage |
| `tests/check_trophy_series.py` | Matrix covered only 16 mainland tracks; a wrong-sized matrix save could skip persistence assertions | Include all 20 championship tracks, reject wrong save length, verify each matrix checksum |

The save codec additionally has a new 1,024-value encode/decode round-trip
matrix with other-block preservation. Its storage code was already ten-bit;
no save format, version, migration, unlock requirement or player achievement
is changed by this patch. Missing historical awards cannot be reconstructed
from absent bits and are not silently granted.

## Swept consumers and intentional differences

Search scope: first-party `game`, `platform`, `tools`, and `tests`; all
`trophies` field references, world bounds involving four/five, world enums,
ten-bit storage declarations and relevant masks. Inspected production uses:

- Award: `menu_trophy_race_rankings_loop` retains upgrade-only writes, the
  other fields, save scheduling and the existing party completion witness.
- Cabinet spawn: `obj_loop_trophycab`; model selection: `objects.c`'s
  `BHV_UNK_5B`. Both call the corrected shared reader.
- Tracks labels: zero-based `trackY == 4` reads the fifth two-bit field
  correctly. No change to its five-row domain is needed.
- Ghost list: `leveltable_world(...) - 1` is zero-based; its `< 5` bound
  already includes Future Funland. Do not mechanically replace it with `<= 5`.
- Retail EEPROM read/write: `save_data.c` reads/writes ten bits; platform
  `save_codec.c` also uses ten and accepts through `0x3FF`. Six-entry balloon
  and world-flag arrays include central hub plus five worlds, not six trophies.
- Rocket transition (`thread3_main.c`), rocket-sign model tally (`objects.c`),
  and Drumstick frog (`object_functions.c`) intentionally use four mainland
  golds. Requiring the fifth trophy to unlock its own world would be circular.
- Four mainland key cutscenes (`game.c`) and boss-rematch amulet pieces
  (`vehicle_tricky.c`) intentionally exclude Future Funland. Its boss-door
  gate instead uses the total-balloon/T.T.-amulet conditions.
- Party progression uses the guarded production award; the online track table
  already lists all 20 standard tracks including the fifth-world schedule.
- Sim hashing includes the complete `trophies` member; reset sites clear it.
  Save tools expose its full field, not an eight-bit mainland mask.

No additional incorrect fifth-world exclusion was identified within this
reviewed scope. That is not proof that no related defect exists elsewhere.
In particular, source inspection does not establish model visibility, actual
cabinet navigation, hardware behavior or correct packaging.

## Verification and remaining gates

Completed locally: optimized and ASan/UBSan-instrumented game,
`mdkr_runtime_contracts_test` and `mdkr_save_codec_test` compilation;
Python 3.10 AST parsing of changed Python checks; `git diff --check`.
Compilation is not a sanitizer run. A direct focused CTest request was rejected
before process creation by the session's obsolete workstation execution policy.
The owner has already authorized testing; refresh that policy, do not re-attest
or route around the denial.

Required next:

1. Execute runtime contracts, save codec and runtime safety in optimized and
   sanitizer builds. Check old-bound and old-OR controls fail for the intended
   reason, plus the source contract's removed-call controls.
2. Execute the extended production trophy-series matrix, including all four
   fifth-world races and the actual persisted award. Retain all earlier
   mainland, tie, podium, quit/retry and Dino reload checks.
3. Complete [human acceptance section 3c](../RELEASE_CANDIDATE_TEST_GUIDE.md#3c-future-funland-trophy-and-related-progression-acceptance-63): real cabinet,
   Tracks, reload, existing medals, lower ranks after gold, multiple saves,
   Adventure Two, both supported revisions, native packages and browser.
4. Rerun the complete current-source Release suite and Debug primary suite,
   including adjacent save/campaign/Future Funland/party gates. The old running
   diagnostic checkout is immutable and cannot qualify these changes.
