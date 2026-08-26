/* Launcher-side track/cup table: the single source of truth the online UI
 * and the live adapter share for the 20 standard race tracks.
 *
 * Provenance (v80 ROM, decoded independently of the engine):
 *   - id / vehicle_mask / default_vehicle mirror the ROM level headers
 *     (LevelHeader.available_vehicles / .vehicle at +0x4E / +0x4D); the
 *     engine's admission gate fail-closes any drift, because a manifest's
 *     vehicle_mask must equal leveltable_vehicle_usable(track) exactly.
 *   - Cup round order mirrors ASSET_MISC_TRACKS_MENU_IDS, the 6-per-world
 *     array DKR's trophy race reads at (worldId - 1) * 6 + round
 *     (game/src/menu.c menu_trophy_race_round_loop).
 *
 * Everything here is static const data and pure lookups: no allocation, no
 * globals, safe from any thread, usable from C and C++. */
#ifndef MDKR_ONLINE_TRACK_TABLE_H
#define MDKR_ONLINE_TRACK_TABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vehicle ids as the engine seats them (game/include/enums.h). */
#define MDKR_ONLINE_VEHICLE_CAR 0u
#define MDKR_ONLINE_VEHICLE_HOVERCRAFT 1u
#define MDKR_ONLINE_VEHICLE_PLANE 2u

/* Vehicle mask bits: bit (1 << vehicle id). */
#define MDKR_ONLINE_VEHICLE_BIT_CAR 0x1u
#define MDKR_ONLINE_VEHICLE_BIT_HOVERCRAFT 0x2u
#define MDKR_ONLINE_VEHICLE_BIT_PLANE 0x4u
#define MDKR_ONLINE_VEHICLE_BIT_ALL 0x7u

/* Worlds in the launcher's DISPLAY order. This is deliberately not the ROM
 * worldId order (Dino=1, Sherbet=2, Snowflake=3, Dragon=4, FFL=5): the UI
 * presents worlds in adventure-progression order. */
typedef enum MdkrOnlineWorld {
    MDKR_ONLINE_WORLD_DINO_DOMAIN = 0,
    MDKR_ONLINE_WORLD_SNOWFLAKE_MOUNTAIN,
    MDKR_ONLINE_WORLD_SHERBET_ISLAND,
    MDKR_ONLINE_WORLD_DRAGON_FOREST,
    MDKR_ONLINE_WORLD_FUTURE_FUN_LAND,
    MDKR_ONLINE_WORLD_COUNT
} MdkrOnlineWorld;

#define MDKR_ONLINE_TRACK_TABLE_COUNT 20u
#define MDKR_ONLINE_CUP_COUNT 5u
#define MDKR_ONLINE_CUP_ROUNDS 4u

typedef struct MdkrOnlineTrackInfo {
    /* Engine level id (AssetLevelHeadersEnum value; what the manifest and
     * the launch descriptor carry). */
    uint16_t id;
    /* Display name; static storage, never NULL. */
    const char *name;
    /* MdkrOnlineWorld value. */
    uint8_t world;
    /* Raw usable-vehicle mask from the ROM level header. This is the value
     * the manifest/admission path MUST carry unmodified; the 2-player
     * picker narrowing below never applies here. */
    uint8_t vehicle_mask;
    /* Retail default vehicle id; its bit is always inside vehicle_mask. */
    uint8_t default_vehicle;
} MdkrOnlineTrackInfo;

/* Always MDKR_ONLINE_TRACK_TABLE_COUNT; provided as a function so callers
 * can iterate without baking in the constant. */
unsigned mdkr_online_track_count(void);

/* Display-order accessor: worlds contiguous in MdkrOnlineWorld order, and
 * tracks inside each world in cup/round order. NULL when index is out of
 * range. */
const MdkrOnlineTrackInfo *mdkr_online_track_at(unsigned index);

/* Lookup by engine level id. NULL for anything that is not one of the 20
 * standard race tracks (hubs, battle maps, boss tracks, garbage). */
const MdkrOnlineTrackInfo *mdkr_online_track_by_id(uint16_t id);

/* Display name for a MdkrOnlineWorld value; NULL when out of range. */
const char *mdkr_online_world_name(uint8_t world);

/* Vehicle mask the PICKER should offer for this track at this player count.
 *
 * Retail parity (game/src/menu.c menu_track_select, VERSION >= 79): with 2+
 * active players Spaceport Alpha (15) drops the hovercraft and Frosty
 * Village (28) drops the plane. That narrowing shapes selection UI only --
 * the manifest/admission mask stays the raw table mask, exactly as the
 * engine's leveltable_vehicle_usable() gate demands.
 *
 * Returns 0 for an unknown track id (nothing offerable, fail closed). */
uint8_t mdkr_online_track_picker_mask(uint16_t id, unsigned playerCount);

/* Tournament cups, one per world, cups indexed in world display order and
 * rounds in DKR trophy-race order (ASSET_MISC_TRACKS_MENU_IDS rounds 0-3).
 * Returns the engine level id, or 0 (the Central Area hub, never a race
 * track) when cup or round is out of range. */
uint16_t mdkr_online_cup_track_id(unsigned cup, unsigned round);

/* Display name for a cup; NULL when out of range. */
const char *mdkr_online_cup_name(unsigned cup);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ONLINE_TRACK_TABLE_H */
