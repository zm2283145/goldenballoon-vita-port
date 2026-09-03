# check_boost_magnitude: root cause and fix

Status: **FIXED**. `tests/check_boost_magnitude.py` passes on this tree; both
arms now read peak |velocity| 22.359, the same figure the v1.6.0 binary
produces, with a cross-mode difference of 0.0004.

## Root cause

**The fault is in the FIXTURE, not in the game.** The boost itself never
changed: every boost/velocity statement in `game/src/racer.c` is untouched
decomp, and the AI behaviour that produced the high numbers is authored decomp
too (verified against the recorded upstream baseline `c6695703`).

1. `tests/check_boost_magnitude.py` drives the "human" racer with DKR's own AI
   (`MDKR_AUTOPILOT` -> `racer_AI_pathing_inputs`).
2. That AI decides, once per boost, on `roll_percent_chance(sp3A)` at
   **`game/src/racer.c:1036`**, whether to LIFT OFF the accelerator while
   boosting: it sets `racer->unk209 |= 4`, and **`game/src/racer.c:1051`** then
   clears `A_BUTTON` from `gCurrentRacerInput`.
3. With A released, the authored velocity update takes the other side of the
   `velSquare < 1.0f` split at **`game/src/racer.c:6564`**. `velSquare` is
   *negated* while driving forward, so that side is taken at **any** speed: the
   quadratic drag `v*v*traction` (line 6567) is replaced by the linear
   `v*traction*8` (line 6565). The boost's 2.0/tick thrust then runs toward
   `2.0 / (8 * 0.004) = 62.5` instead of `sqrt(2.0 / 0.004) = 22.36` — the same
   22.4 constant `mdkr_boss_cadence_clamp` (racer.c:7813) names as "the boost
   allowance". 45 ticks cannot reach 62.5, so the measured peak stops being a
   terminal speed and becomes "wherever the racing line cut the ramp off".
4. What flipped the roll between v1.6.0 and this tree is the
   **presentation-RNG split**: `cadence_compat_rand_range`
   (`platform/math_util_native.c:302`) routes render-authored rolls to the
   separate presentation stream at enhanced cadence, which is the cadence this
   check runs at. The authoritative generator cycles with period 20, so a change
   in the *number* of authoritative draws — not the seed — is what moves the
   roll. (This is the designed behaviour the split's own gates pin; it is not a
   defect.)
5. The roll's chance `sp3A` is itself interpolated from the number of CPU racers
   *ahead* of the racer (`game/src/racer.c:987`), and once the racer is on the
   unbounded ramp the peak depends on the route. **That is the entire
   "racer-count dependence"**: 50.401 (8-racer) vs 45.519 (solo) is an AI coin
   flip plus two different truncation points, not a property of the boost.

## A/B evidence

Solo Time Trial arm, `MDKR_ZIPPAD_BOOST=4000`, enhanced cadence, same ROM and
same input script, entry velocity identical to 4 decimals (12.8878 vs 12.8882):

| frame | v1.6.0 `vel` | this tree (before fix) `vel` | this tree `unk209` |
|-------|--------------|------------------------------|--------------------|
| 4000  | -12.8878     | -12.8882                     | 0                  |
| 4001  | -14.2235     | -14.4759                     | 6 (bit 4 = lift)   |
| 4005  | -18.1715     | -20.3345                     | 6                  |
| 4011  | -20.9683     | -27.8101                     | 6                  |

Fitting the per-tick deltas:

* v1.6.0: `dv = 2.0 - 0.004 * v^2` — the quadratic branch, thrust 2.0,
  `gSurfaceTractionTable[SURFACE_DEFAULT] = 0.004`. Asymptote 22.36.
* this tree, before the fix: `dv = 2.0 - 0.032 * v` — the **linear** branch,
  `traction * 8 = 0.032`. Asymptote 62.5.

Two controls confirmed the mechanism:

* Boot-seed sweep on the v1.6.0 binary (`MDKR_RNGSEED` = 6 values) and an
  arming-frame sweep (13 frames, 3800..4400) never produced the lift: peak
  22.35..22.36 every time. The generator's period-20 cycle is why the seed does
  not matter and the draw count does.
* Temporarily forcing `cadence_compat_rand_range` back to `rand_range` on this
  tree (throwaway `MDKR_EXP_NOSPLIT`, not committed) moved the solo arm from
  **45.519 back to 22.359** with nothing else changed. That is the decisive A/B.

No bisect result: `git bisect run` was attempted across the 198-revision range
and every step exited 125 — the CMake reconfigure fails on older commits with
`libdatachannel v0.24.5 resolved to <mdkr64 HEAD sha>, expected 443f6934`
(`cmake/datachannel.cmake:155`), so the build cannot be regenerated at an
arbitrary commit in a configured tree. The `MDKR_EXP_NOSPLIT` A/B answers the
same question directly and is stronger, so bisect was abandoned rather than
worked around.

## Fix

The seam already substitutes a deterministic *trigger* for a chaotic one; it now
also controls the one *input* the measurement depends on. A pad boost is authored
to be ridden with the accelerator down — that state is what the 22.36 equilibrium
is the equilibrium **of**.

* `game/src/objects.c` — `mdkr_zippad_boost_hold_throttle()`: while the seam's
  own boost is running, OR `A_BUTTON` back into `gCurrentRacerInput` for the
  armed racer. No-op unless `MDKR_ZIPPAD_BOOST` armed a boost. The AI's own
  election is never written back; `mdkr_boost_trace` reports it as a new `lift=`
  field so a contaminated arm is visible instead of silent.
* `game/src/racer.c` — one call to that hook inside the existing
  `#ifdef NATIVE_PORT` `MDKR_AUTOPILOT` block in `update_player_racer`, placed
  after the AI has written its inputs and before the velocity update reads them.
* `game/src/objects.h` — the prototype, beside the seam's siblings.
* `tests/check_boost_magnitude.py` — docstring: the throttle-hold is now part of
  the documented trigger, with the mechanism, the stale `racer.c:5727` reference
  corrected to `6542`, and the measured table / thresholds refreshed.

Retail behaviour is untouched: no boost or velocity statement in `racer.c`
changed, so the `.decomp-baseline` byte-identity claim in the check's docstring
stays true; both edits are inside `#ifdef NATIVE_PORT` and both are inert unless
`MDKR_ZIPPAD_BOOST` is set.

## Gates

| gate | result |
|------|--------|
| `check_boost_magnitude.py` | PASS (was FAIL); both controls `:120` / `:15` still fail as required |
| `check_race_drive.py` | PASS |
| `check_simulation_cadence.py` (`--roms .../dkr_roms`) | PASS |
| `check_adventure_party_boundaries.py` | PASS |
| `check_adventure_party_race_loop.py` | PASS |
| `check_adventure_party_performance.py` | FAIL — **pre-existing**, byte-identical failure on the unmodified tree (stash + rebuild + rerun): `race renderer: terminal generations retained a rising ownership suffix in counter 1`. Unrelated to this change. |

## Player impact

None. The lift-off path is in `func_80042D20`, which only ever runs for a racer
presented to the AI as `PLAYER_COMPUTER`; a human holding the accelerator through
a zip pad always takes the quadratic-drag branch and the authored 22.36 ceiling,
in every mode, on v1.6.0 and on this tree alike. CPU racers could always take the
lift-off branch — it is authored DKR behaviour reachable on the ROM — and the
presentation-RNG split changes *which* enhanced-cadence rolls land, which is its
documented intent, not a regression.
