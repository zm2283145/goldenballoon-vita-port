# Presentation RNG census

Status: **census complete; no caller is redirectable at this commit.** The
split mechanism already exists and is already applied to everything it can
reach. This document records what every remaining `rand_range()` caller does,
why it stays on the authoritative stream, and the two measurements that decide
it.

## The two streams

`rand_range()` (`game/src/hasm/math_util.c:246`) steps `gCurrentRNGSeed`. That
word and `gPrevRNGSeed` are registered in the rollback snapshot as
`TAG_RNG_CURRENT` / `TAG_RNG_PREVIOUS`
(`platform/rollback/rollback_game_authority.c:121-122,491-492`) and the
`[SIMHASH]` v3 state hash covers the seed directly (`platform/sim_hash.c:26`).
Everything that draws from it is authoritative by construction.

`presentation_rand_range()` (`platform/math_util_native.c:272`) steps
`gPresentationRNGSeed`, a file-static word in the platform translation unit. It
is deliberately outside the snapshot registry, so nothing it produces can steer
a rollback or a peer. It uses the ROM generator's exact step and inclusive-range
semantics and is seeded from a constant derived from the ROM seed — **not** from
host state, because lanes that compare pixels between two arms of one route
require the presentation stream to be reproducible run to run.

`cadence_compat_rand_range()` (`platform/math_util_native.c:298`) is the bridge:
at the shipping two-field cadence it routes back to `rand_range()` for
byte-exact ROM ordering, and only at the opt-in enhanced cadence does it use the
presentation stream.

## The rule the census applies

A caller is redirectable only if **both** hold:

1. Its *output* never reaches authoritative state, and
2. Its *draw* is not a shared-sequence event that an ordering-sensitive gate
   pins.

Condition 2 is the one that decides this census, and it is easy to miss. The
two streams are linear sequences. Removing any draw from the authoritative
stream shifts every subsequent value for every downstream consumer, so a
redirect changes the race even when the redirected value itself is only ever
turned into a pitch or a palette index. Verdicts below therefore separate "what
the value does" from "what the draw does".

**This argument applies only to an UNCONDITIONAL redirect** — one that moves a
caller onto the presentation stream at every cadence. A cadence-conditional
redirect, the pattern the tree already uses for the 24 sites below, does not
shift the stream any oracle records; what it spends is something else. See
"What would change the answer".

## Measurements

Two callers were redirected, measured, and reverted. Both are cases where
condition 1 holds and condition 2 does not.

**Engine audio jitter** — `game/src/audio_vehicle.c:517-518`. `engineJitter` is
read only at `audio_vehicle.c:525-526`, where it becomes a pitch and volume
delta; the value never leaves audio. A probe build counted **989 draws** on the
3600-frame determinism route with `MDKR_AUDIO=0`, so the caller runs whether or
not anything is listening. Redirected to the presentation stream,
`python3 tests/check_state_hash.py --build build` fails:

```
check_state_hash: FAIL
  - v3 particle field-set control changed ticks NONE — expected exactly [2710].
```

Retail ownership of this draw is independently pinned: the header of
`tests/check_authored_rng_compat.py` records that the ROM's `racer_sound_car`
consumes the shared RNG stream, proved against hardware with the ares
PC/return-address witness.

**Menu image fields** — `game/src/menu.c:16916-16918`. `unk1A`/`unk1B`/`unk1C`
on `gMenuImages` are written here and read nowhere in the tree. A probe counted
**10 draws** on the same route, all of them before the race starts. Redirected,
`check_state_hash.py` still passes. That is not a hole in it: its arms compare
the binary against itself, which is the invariance it exists to state, and a
uniform stream shift is not that question. RNG-stream shifts are adjudicated by
`check_authored_rng_compat.py` at original cadence, and by nothing at enhanced
cadence. Here the oracle fails:

```
$ python3 tests/check_authored_rng_compat.py --build build
FAIL: raw stream SHA-256 bb1e7c49b94d1a067ab9922520205d432bcb5e7daae3baef2b64f7cde2072d98,
      expected 191bee35a973b2bde6133cc6ae2c2c41961a97ec72a9b034a08574d53aacba5b
```

Ten draws of a value nothing reads, taken before the green light, move the whole
recorded race. That is condition 2 stated as a number.

## Census

113 logical call sites across `game/src`. `game/src/game_ui.c` reaches the
generator through the `hud_rand_range` macro (`game_ui.c:43/47`) and
`game/src/textures_sprites.c:2010` through an explicit branch; both are counted
once per source line.

| Verdict | Sites |
|---|---|
| Simulation | 80 |
| Presentation output, stream-owning draw | 8 |
| Already split (cadence-conditional) | 24 |
| Not compiled | 1 |
| **Redirectable at this commit** | **0** |

"Redirectable" here means an *unconditional* redirect, on every cadence.
The 8 latent callers could be moved cadence-conditionally without failing
any gate in the tree; what that spends, and why it was not done, is under
"What would change the answer".

### Simulation — 80 sites

| Caller | File:line | Path | Evidence |
|---|---|---|---|
| `setup_particle_velocity`, `setup_particle_position`, `create_general_particle` | `particles.c:1200-1940` (25) | particle spawn | Particle pools and counts are snapshot-registered (`TAG_ALLOC_PARTICLE_*`, `gParticleCount`); `check_state_hash.py` carries a v3 particle field-family control. |
| `weather_reset`, `rain_render_splashes`, `rain_splash_tick`, `rain_lightning` | `weather.c:452-1538` (14) | weather integrator | `weather.c:1455` documents these rolls as authoritative and deliberately unbracketed: racer AI consumes the seed they leave (`racer.c:338/4335/4539/5219/5223/5415/5872/9019`). |
| `obj_loop_scenery`, `obj_init_fish`, `obj_loop_fish`, `obj_loop_lavaspurt`, `obj_init_bombexplosion`, `obj_loop_parkwarden`, `obj_loop_butterfly`, `obj_loop_bubbler`, `obj_loop_frog` | `object_functions.c:196-7067` (17) | object behaviour | Writes object state (`modelIndex`, timers, velocities) that the object list carries and the state hash covers. |
| `func_80042D20`, `roll_percent_chance`, `racer_ai_challenge`, `update_player_racer`, `func_8004F7F4`, `func_80050A28`, `handle_racer_head_turning`, `play_random_character_voice`, `update_AI_racer` | `racer.c:750-9969` (15) | racer AI and physics | AI skill, steering jitter (`RACER_STEER_JITTER_OFFSET` feeds `gCurrentStickX`), head angle and voice selection all sit inside the authoritative racer update. |
| `racerfx_alloc` | `objects.c:748-765` (4) | racer FX allocation | Seeds shield/boost object fields at allocation time, inside the object list. |
| `charselect_assign_ai` | `menu.c:9335` | AI roster | Chooses which characters the AI races; a race input, not a drawing. |
| `func_80092188` | `menu.c:12125` | menu behaviour | Unidentified menu branch. Classified simulation on doubt, per the conservative rule. |
| `waves_init` | `waves.c:750-751` (2) | wave field | `gWaveHeightIndices` is snapshot-registered (`TAG_ALLOC_WAVE_HEIGHT_INDICES`). |
| `spawn_boss_hazard` | `vehicle_smokey.c:323` | object spawn | Sets `animFrame` on a spawned object, a hashed object field. |

### Presentation output, stream-owning draw — 8 sites

Condition 1 holds for all eight: the value never reaches authoritative state.
Condition 2 fails: the draw is pinned by an ordering-sensitive gate. None is
redirectable.

| Caller | File:line | Path | Evidence |
|---|---|---|---|
| `racer_sound_car` | `audio_vehicle.c:517-518` (2) | engine audio | Measured above: 989 draws, `check_state_hash.py` fails on redirect; ares witness pins retail ownership. |
| `racer_boss_sound_spatial`, `play_random_boss_sound` | `vehicle_tricky.c:249,261` (2) | boss sound choice | Value selects a sound offset only, but the draw sits in the authoritative boss update alongside `check_fadeout_transition` consumers. |
| `menu_image_load` | `menu.c:16916-16918` (3) | menu image fields | Measured above: written, never read, 10 pre-race draws, `check_authored_rng_compat.py` fails on redirect. |
| `menu_credits_init` | `menu.c:15981` | credits cheat pick | Chooses which cheat the credits display. Not reached on the recorded route, but the credits screen precedes a return to racing, so the draw still shifts a later race. |

### Already split — 24 sites

On the presentation stream at enhanced cadence, back on the authoritative
stream at the shipping cadence, via `cadence_compat_rand_range()`. This is the
existing redirect and the reason the census finds nothing new to move: the only
callers whose draws can leave the authoritative sequence are ones already
carrying a compatibility switch to put them back.

| Caller | File:line | Path |
|---|---|---|
| `hud_init_element`, `hud_time_trial_authored_rng_tick`, `hud_wrong_way_authoritative_tick`, `hud_race_start_authoritative_tick`, `hud_main_time_trial`, `hud_race_start`, `hud_wrong_way` | `game_ui.c:1002-3936` (23) | HUD voices and nag timers |
| `tex_animate_texture_impl` | `textures_sprites.c:2010` | animated palette pick |

### Not compiled — 1 site

| Caller | File:line | Evidence |
|---|---|---|
| `__amHandleFrameMsg` | `audiomgr.c:479` | Inside `#ifdef ANTI_TAMPER`, which this build never defines. The anti-piracy output-frequency dither is not in the binary. |

## The gate

`tests/check_presentation_rng_split.py` pins the split that exists.
`MDKR_RNG_SPLIT_TRACE=1` emits one `[RNGSPLIT]` row per **presented** frame.
Be precise about the route: at enhanced cadence with `MDKR_SYNTH_FIELDS=1` the
simulation ticks on every presented frame and the `[SIMHASH]` hash changes on
every one of them, so these are not frames that present without advancing the
simulation. They are authoritative ticks whose settled menu-idle logic draws no
random numbers, while texture animation keeps drawing from the presentation
stream — two streams running side by side over the same frames, one of them
required to stand still.

Both halves are asserted, because either alone passes for the wrong reason — a
run drawing no randomness at all would satisfy "the seeds did not move". Over
251 settled frames the lane requires `gCurrentRNGSeed`/`gPrevRNGSeed`
byte-identical while the presentation counter advances 235 draws across 30
distinct seeds, requires the authoritative seed to have moved *before* the
window (so a trace printing a constant cannot pass), and requires two runs to
be byte-identical. `MDKR_TEST_RENDER_IMPURITY=1` is the positive control: the
existing render-purity seam performs one authoritative RNG write inside every
render, which must break the pin while leaving the presentation draw counts
untouched.

At the shipping two-field cadence the presentation stream never advances on
this route — every HUD roll routes back through
`cadence_compat_rand_range()` — so the lane pins the enhanced cadence, where the
split is live and the assertion has something to see.

## What would change the answer

A caller becomes redirectable when its draw stops being shared, or when the
redirect is arranged so that no recorded stream ever sees it. Three routes, none
taken here:

- **Cadence-conditional redirect — the cheapest route, and the one this census
  deliberately did not take.** Route the 8 latent callers through
  `cadence_compat_rand_range()` (`platform/math_util_native.c:300-305`) exactly
  as the existing 24 are: authoritative stream at the shipping cadence,
  presentation stream only at the opt-in enhanced cadence. No oracle rebaseline
  is needed and every existing gate stays green, because every gate that records
  an RNG stream records the original arm — `check_authored_rng_compat.py:189`
  runs `MDKR_SIMULATION_CADENCE="original"` with `MDKR_SYNTH_FIELDS="2"`,
  `check_state_hash.py` sets no cadence and takes that same default, and
  `check_weather_rng_order.py:69` pins `EXPECTED_ORIGINAL_SHA256`, the original
  arm alone.

  What it spends is the second compatibility target named in the comment at
  `platform/math_util_native.c:296-299`: "the pre-FPS native gameplay stream at
  opt-in enhanced cadence". No gate holds that target today, so a redirect would
  move the enhanced-cadence authoritative stream silently and nothing in the
  tree would report it. Spending an ungated compatibility target is an owner
  decision, not a test change — which is why the redirect stops here and the
  eight callers are listed instead of moved.

- Give a subsystem its own authoritative sub-stream seeded from the match seed,
  so removing its draws cannot shift anyone else's. That is a wire-format and
  snapshot change, and rebaselines `check_authored_rng_compat.py`.

- Accept a rebaseline of the authored oracle for an unconditional redirect of a
  caller proved presentation-only. That trades away the ROM-ordering
  compatibility the oracle exists to hold, and is an owner decision, not a test
  change.
