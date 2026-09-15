# AP-04 phase 1 — Adventure object census (`OBJECT_HEADER_NO_MULTIPLATER`)

- Date: 2026-08-28
- ROM: `baserom.us.v80.z64`, md5 `b31f8cca50f31acc9b999ed5b779d6ed` (12,582,912 bytes)
- Machine-readable companion: [`object-census.json`](object-census.json)
- Regenerator: [`object-census-scan.py`](object-census-scan.py)
  (`python3 object-census-scan.py baserom.us.v80.z64 object-census.json`)
- Scope: measurement only. No game-source, test-hook, or check changes.

## What this measures and why

`objects.c:2970-2978` frees **every** object whose `ObjectHeader.flags` carries
`OBJECT_HEADER_NO_MULTIPLATER` (bit `1<<6` = `0x40`, `game/src/objects.h:191`)
the moment `numPlayers >= 2`:

```c
} else if (i & OBJECT_HEADER_NO_MULTIPLATER && numPlayers >= 2) {
    free_object(racerObj);
}
```

A party session loads two or more humans into every lobby, so this blanket
filter runs in every Adventure hub and course. If any **party-critical** object
(door, exit, Taj, golden balloon, key, teleporter, race-entry trigger, lobby
system) carried the flag, co-op would silently lose it. This census enumerates,
per level, every placed object whose header carries the flag, and classifies it.

## Method — static ROM asset parse (ground truth)

The flag lives in the shared `ObjectHeader`, not per instance, so flag presence
is the ground truth (a 1P runtime pass never exercises the `>= 2` filter). The
regenerator independently re-implements the engine's own resolution chain from
the raw ROM (it imports no game code, so agreement is cross-checking, not a
tautology):

1. Master asset LUT at ROM `0x000ED0E0`, data base `0x000ED1B0`; section `S`
   spans `[LUT[S+1], LUT[S+2])` (`game/src/asset_loading.c`,
   cross-checked against `tools/dump_misc_asset.py`).
2. `ASSET_OBJECTS` (34) via `ASSET_OBJECT_HEADERS_TABLE` (33): each
   `ObjectHeader` (`flags` @ `0x30`, `behaviorId` @ `0x54`, `internalName` @
   `0x60`). Uncompressed (`load_object_header`: `asset_load` + swap, no gzip).
   → **304 headers**, matching the source note "304 on US v80"
   (`objects.c:1509`).
3. `ASSET_LEVEL_OBJECT_TRANSLATION_TABLE` (35): a single global `s16[512]` table.
4. Per level (`ASSET_LEVEL_HEADERS` 23 via table 22, uncompressed):
   `world` @ `0x00`, `race_type` @ `0x4C`, collectables-map id @ `0x36`,
   main-map id @ `0xBA` — the two ids `game.c:838-839` hands `init_track`, which
   `tracks.c:1182-1183` spawns as map index 0 (main) and 1 (collectables).
5. Each object map (`ASSET_LEVEL_OBJECT_MAPS` 21 via table 20) is rzip/gzip
   compressed: 4-byte little-endian decompressed size + 1 byte, then raw DEFLATE
   at `+5` (`gzip.c:96,122`; `objects.c:2452-2476`). After a 16-byte header
   (`OBJ_MAP_HEADER_S32S=4`), each entry is walked exactly as `spawn_object`
   does: `objType = byte0 | ((byte1 & 0x80) << 1)`, stride `= byte1 & 0x3F`
   (`objects.c:3754,3764`).
6. `objType -> translation_table[objType] -> header index -> header`
   (`objects.c:3779-3811`); count placements whose header is flagged.

**Coverage / integrity:** 65 levels, 138 object maps, all parsed; **0**
unresolved object ids across every map; **0** maps failed to inflate. End-to-end
validation: the Central Area Hub main map resolves 8 `BHV_DOOR`, 5 `BHV_EXIT`,
2 `BHV_TAJ_TELEPOINT`, 1 `BHV_GOLDEN_BALLOON`, 6 `BHV_SETUP_POINT`,
52 `BHV_CHECKPOINT`, 5 `BHV_RANGE_TRIGGER` — every one **present and unflagged**;
its collectables map resolves the hub's 6 golden balloons, Taj
(`BHV_PARK_WARDEN`), `BHV_TELEPORT` and both rocket signposts, all unflagged.
The chain therefore both finds the party-critical objects and confirms the flag
does not touch them.

## Headline result

**Every one of the 23 flagged object headers is cosmetic scenery. No
party-critical object type carries `OBJECT_HEADER_NO_MULTIPLATER`, in any level.
Across all 65 levels / 138 maps there are 0 party-critical flagged placements.**

The 23 flagged headers (of 304), by behaviour:

| behaviour | headers | examples |
| --- | --- | --- |
| `BHV_SCENERY` | 16 | SmartieTree, BlueBerryBush, RubberSnowTree, SkinnySnowTree, XmasTree, AlpineSnowTree, RubberTree, Beachtree, PalmTreeTop, PalmPlant, PalmTreeTopChea, FirTree, SpaceTree, Lamppost, Flowers, Snowmen |
| `BHV_DINO_WHALE` | 4 | Dinosaur1, Dinosaur2, Dinosaur3, Whale (decorative background fauna) |
| `BHV_ANIMATED_OBJECT` | 2 | AnimDinosaur1, AnimDinosaur2 |
| `BHV_TORCH_MIST` | 1 | FlamingTorch |

These are exactly the objects the retail engine thins out to save budget in
split-screen. Their removal in a party session is the intended, benign behaviour
(fewer trees/torches per hub with more humans), not a co-op hazard.

## Verdict per Adventure lobby (hub)

The six Adventure lobby hubs. `flagged` = cosmetic scenery removed at
`numPlayers >= 2`; **party-critical removed = 0 for every hub → SAFE.**

| level id | hub | flagged (cosmetic) | party-critical flagged | verdict |
| --- | --- | --- | --- | --- |
| 0 | CENTRALAREAHUB (Timber's Island) | 68 | 0 | SAFE |
| 2 | DRAGONFORESTHUB | 2 | 0 | SAFE |
| 12 | DINODOMAINHUB | 0 | 0 | SAFE |
| 14 | SHERBETISLANDHUB | 4 | 0 | SAFE |
| 24 | SNOWFLAKEMOUNTAINHUB | 7 | 0 | SAFE |
| 35 | FUTUREFUNLANDHUB | 0 | 0 | SAFE |

(Nine further cutscene/anim levels also carry `race_type == HUBWORLD` —
OPENINGSEQUENCE, the amulet/rocket/party sequences, the trophy/boss anims — and
are likewise all party-critical-flagged = 0. See `object-census.json`.)

## Adventure courses

All 20 Adventure race courses plus Horseshoe Gulch (world 0-5,
`race_type` DEFAULT/HORSESHOE): **party-critical flagged = 0 for every course.**
`flagged` below is again cosmetic-only.

| level id | course | world | flagged (cosmetic) |
| --- | --- | --- | --- |
| 5 | ANCIENTLAKE | DINO_DOMAIN | 22 |
| 3 | FOSSILCANYON | DINO_DOMAIN | 25 |
| 29 | JUNGLEFALLS | DINO_DOMAIN | 14 |
| 7 | HOTTOPVOLCANO | DINO_DOMAIN | 0 |
| 6 | WALRUSCOVE | SNOWFLAKE_MOUNTAIN | 25 |
| 9 | SNOWBALLVALLEY | SNOWFLAKE_MOUNTAIN | 18 |
| 13 | EVERFROSTPEAK | SNOWFLAKE_MOUNTAIN | 16 |
| 28 | FROSTYVILLAGE | SNOWFLAKE_MOUNTAIN | 22 |
| 8 | WHALEBAY | SHERBET_ISLAND | 12 |
| 4 | PIRATELAGOON | SHERBET_ISLAND | 11 |
| 10 | CRESCENTISLAND | SHERBET_ISLAND | 21 |
| 30 | TREASURECAVES | SHERBET_ISLAND | 12 |
| 20 | WINDMILLPLAINS | DRAGON_FOREST | 25 |
| 18 | GREENWOODVILLAGE | DRAGON_FOREST | 9 |
| 19 | BOULDERCANYON | DRAGON_FOREST | 8 |
| 31 | HAUNTEDWOODS | DRAGON_FOREST | 0 |
| 17 | SPACEDUSTALLEY | FUTURE_FUN_LAND | 20 |
| 33 | STARCITY | FUTURE_FUN_LAND | 15 |
| 32 | DARKMOONCAVERNS | FUTURE_FUN_LAND | 0 |
| 15 | SPACEPORTALPHA | FUTURE_FUN_LAND | 0 |

(Boss and challenge/battle levels — Fire Mountain, Icicle Pyramid, Smokey
Castle, Darkwater Beach, the Tricky/Bluey/Bubbler/Smokey/Wizpig arenas, Trophy
Race, Star City — are enumerated in `object-census.json`; every one is
party-critical-flagged = 0 as well.)

## Scope boundary (what this does NOT assert)

This census answers exactly one question: *does the `numPlayers >= 2` object
filter remove any party-critical object?* Answer: no, anywhere. It does **not**
assert that those surviving party-critical objects behave correctly under three
or four humans — the player-one binding of Taj, hub golden balloons, doors, and
silver coins documented at `object_functions.c:2975-3031/3831-3850/4961-4984`
and `objects.c:3286-3340` is a separate policy problem owned by later AP tasks
(AP-05/AP-07/AP-10), not a filtering problem. AP-04's contribution here is that
a blanket party-count admission is **safe with respect to object removal**: no
lobby needs a measured admission exception for a filtered required object,
because none exists.

---

# Six-racer-with-four-viewports feasibility (code reading only)

Per the brief this is derived from the census + static code reading; the actual
capacity/budget under a forced six-racer/four-viewport spawn is a **phase-2
runtime probe** (it needs a dev-only forced-count hook that does not exist yet).

## Hard capacities that already permit six racers and four viewports

- **Racer arrays are sized for 10.** `gRacers`, `gRacersByPort`,
  `gRacersByPosition` are each `Object *[10]`, allocated
  `mempool_alloc_safe(sizeof(uintptr_t) * 10, ...)`
  (`objects.c:643,1462-1464`; `extern Object *(*gRacers)[10]`, `objects.c:129`).
  Six racers is well within the array bound.
- **The six-racer field already exists and ships.** `get_multiplayer_racer_count()`
  returns a fixed **6** for two-player Adventure and for the Trophy Race, and
  otherwise `(sel+1)<<1 ∈ {2,4,6}` (`menu.c:16536-16543`). `objects.c:2738`
  already assigns `gNumRacers = get_multiplayer_racer_count()` for the
  `numPlayers == 2` case. So a six-kart field is a proven, in-use configuration,
  not new engine territory.
- **Viewports are hard-capped at 4.** `cam_set_layout()` accepts only
  `VIEWPORT_LAYOUT_1_PLAYER..VIEWPORT_LAYOUT_4_PLAYERS` and clamps anything else
  to 1; `gNumCameras` is 1/2/3/4 respectively (`camera.c:1602-1627`). The
  snapshot and viewport-route arrays are fixed at 4
  (`PRESENTATION_SNAPSHOT_MAX_VIEWPORTS 4`, `presentation_snapshot.h:64`;
  `MDKR_VIEWPORT_ROUTE_MAX_VIEWPORTS 4u`, `viewport_route_cache.h:9`). Four
  human viewports is therefore the maximum, and it is already exercised by
  `tests/check_race_multiplayer.py`'s 4P arm.

**Static conclusion:** six racers (4 humans + 2 CPUs) rendered through four
viewports fits every hard capacity — racer arrays (10), viewport enum/arrays (4),
and the object pool (`OBJECT_SLOT_COUNT 512`, `objects.c:330`). Nothing in the
array sizing or the layout enum blocks it.

## The current field rule does not itself produce 6-with-4-viewports

`objects.c:2731-2748` decides `gNumRacers`:

- `raceType == RACETYPE_HUBWORLD || numPlayers >= 3` → `gNumRacers = numPlayers`
  (a 3- or 4-human race gets exactly 3 or 4 racers — **no CPU fill**);
- `numPlayers == 2` → `gNumRacers = get_multiplayer_racer_count()` (6 in 2P adv);
- `raceType & RACETYPE_CHALLENGE` → `gNumRacers = 4`;
- `raceType == RACETYPE_BOSS` → `gNumRacers = 2`, `numPlayers = 1`;
- menu/default rolling demo → `gNumRacers = 6`.

So the generic three/four-player path yields `numRacers == numPlayers`. Getting
**six** racers behind **four** human viewports requires the party spawn path to
override `gNumRacers` to 6 while `cam_set_layout(4)` gives four viewports — the
6-kart field is supported, but the current code will not emit that combination on
its own. That override is AP-07/AP-19 work.

## What only the phase-2 runtime probe can prove

The static capacities are necessary, not sufficient. Unmeasurable without a run:

- **Resource high-water** of simulating 6 racers while rendering 4 viewports in
  a lobby/standard-race context — texture live/peak and the gfx-registry
  high-water (`renderer_generation` / `registry_state`, `gfx_pc_dkr.c:2565-2608`).
  The four-viewport texture/geometry cost is the plan's "four-camera resource
  overflow" risk (`docs/architecture/adventure-party.md`), and it is what the
  baseline in [`baseline-2026-08-28.md`](baseline-2026-08-28.md) is captured to
  compare against.
- **Frame-time budget** under the combined 6-sim + 4-view load.
- The forced-count spawn itself: production party spawning does not exist yet, so
  the capacity must be measured with a dev-only forced-count spawn through the
  existing test-hook layer (explicitly a later task; not attempted here).
