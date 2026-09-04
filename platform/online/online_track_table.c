/* See online_track_table.h for provenance. Data verified 2026-08-26 against
 * baserom.us.v80.z64: masks/defaults against the level headers (asset
 * sections 22/23, LevelHeader +0x4D/+0x4E), cup round order against misc
 * sub-asset 26 (ASSET_MISC_TRACKS_MENU_IDS), whose per-world rows read
 *   Dino      05 03 1d 07 | 22 0b
 *   Sherbet   08 04 0a 1e | 22 1a
 *   Snowflake 0d 06 09 1c | 22 1b
 *   Dragon    13 12 14 1f | 22 19
 *   FFL       11 20 21 0f | 22 ff
 * (columns 4-5 are the trophy podium hub and the boss track; rounds are
 * columns 0-3). The rows land in ROM worldId order -- this table regroups
 * them into display order but keeps each world's round order untouched. */
#include "online_track_table.h"

#include <stddef.h>

/* Retail 2-player picker narrowing (menu.c menu_track_select, V79+). */
#define TRACK_SPACEPORT_ALPHA 15u
#define TRACK_FROSTY_VILLAGE 28u

/* Display order: worlds in MdkrOnlineWorld order, four tracks per world in
 * trophy-race round order. Cup c therefore spans rows [c*4, c*4+4). */
static const MdkrOnlineTrackInfo TRACKS[MDKR_ONLINE_TRACK_TABLE_COUNT] = {
    /* Dino Domain */
    {5u, "Ancient Lake", MDKR_ONLINE_WORLD_DINO_DOMAIN, 0x7u, 0u},
    {3u, "Fossil Canyon", MDKR_ONLINE_WORLD_DINO_DOMAIN, 0x7u, 0u},
    {29u, "Jungle Falls", MDKR_ONLINE_WORLD_DINO_DOMAIN, 0x7u, 0u},
    {7u, "Hot Top Volcano", MDKR_ONLINE_WORLD_DINO_DOMAIN, 0x6u, 2u},
    /* Snowflake Mountain */
    {13u, "EverFrost Peak", MDKR_ONLINE_WORLD_SNOWFLAKE_MOUNTAIN, 0x7u, 2u},
    {6u, "Walrus Cove", MDKR_ONLINE_WORLD_SNOWFLAKE_MOUNTAIN, 0x3u, 0u},
    {9u, "Snowball Valley", MDKR_ONLINE_WORLD_SNOWFLAKE_MOUNTAIN, 0x3u, 0u},
    {28u, "Frosty Village", MDKR_ONLINE_WORLD_SNOWFLAKE_MOUNTAIN, 0x7u, 0u},
    /* Sherbet Island */
    {8u, "Whale Bay", MDKR_ONLINE_WORLD_SHERBET_ISLAND, 0x2u, 1u},
    {4u, "Pirate Lagoon", MDKR_ONLINE_WORLD_SHERBET_ISLAND, 0x2u, 1u},
    {10u, "Crescent Island", MDKR_ONLINE_WORLD_SHERBET_ISLAND, 0x3u, 0u},
    {30u, "Treasure Caves", MDKR_ONLINE_WORLD_SHERBET_ISLAND, 0x7u, 0u},
    /* Dragon Forest */
    {19u, "Boulder Canyon", MDKR_ONLINE_WORLD_DRAGON_FOREST, 0x2u, 1u},
    {18u, "Greenwood Village", MDKR_ONLINE_WORLD_DRAGON_FOREST, 0x3u, 0u},
    {20u, "Windmill Plains", MDKR_ONLINE_WORLD_DRAGON_FOREST, 0x7u, 2u},
    {31u, "Haunted Woods", MDKR_ONLINE_WORLD_DRAGON_FOREST, 0x3u, 0u},
    /* Future Fun Land */
    {17u, "Spacedust Alley", MDKR_ONLINE_WORLD_FUTURE_FUN_LAND, 0x7u, 2u},
    {32u, "DarkMoon Caverns", MDKR_ONLINE_WORLD_FUTURE_FUN_LAND, 0x3u, 0u},
    {33u, "Star City", MDKR_ONLINE_WORLD_FUTURE_FUN_LAND, 0x7u, 0u},
    {15u, "Spaceport Alpha", MDKR_ONLINE_WORLD_FUTURE_FUN_LAND, 0x7u, 2u},
};

static const char *const WORLD_NAMES[MDKR_ONLINE_WORLD_COUNT] = {
    "Dino Domain",
    "Snowflake Mountain",
    "Sherbet Island",
    "Dragon Forest",
    "Future Fun Land",
};

static const char *const CUP_NAMES[MDKR_ONLINE_CUP_COUNT] = {
    "Dino Domain Cup",
    "Snowflake Mountain Cup",
    "Sherbet Island Cup",
    "Dragon Forest Cup",
    "Future Fun Land Cup",
};

unsigned mdkr_online_track_count(void) { return MDKR_ONLINE_TRACK_TABLE_COUNT; }

const MdkrOnlineTrackInfo *mdkr_online_track_at(unsigned index) {
    return index < MDKR_ONLINE_TRACK_TABLE_COUNT ? &TRACKS[index] : NULL;
}

const MdkrOnlineTrackInfo *mdkr_online_track_by_id(uint16_t id) {
    unsigned index;
    for (index = 0u; index < MDKR_ONLINE_TRACK_TABLE_COUNT; index++) {
        if (TRACKS[index].id == id) return &TRACKS[index];
    }
    return NULL;
}

const char *mdkr_online_world_name(uint8_t world) {
    return world < MDKR_ONLINE_WORLD_COUNT ? WORLD_NAMES[world] : NULL;
}

uint8_t mdkr_online_track_picker_mask(uint16_t id, unsigned playerCount) {
    const MdkrOnlineTrackInfo *track = mdkr_online_track_by_id(id);
    uint8_t mask;
    if (track == NULL) return 0u; /* nothing offerable: fail closed */
    mask = track->vehicle_mask;
    if (playerCount >= 2u) {
        if (id == TRACK_SPACEPORT_ALPHA)
            mask &= (uint8_t)~MDKR_ONLINE_VEHICLE_BIT_HOVERCRAFT;
        if (id == TRACK_FROSTY_VILLAGE)
            mask &= (uint8_t)~MDKR_ONLINE_VEHICLE_BIT_PLANE;
    }
    return mask;
}

uint16_t mdkr_online_cup_track_id(unsigned cup, unsigned round) {
    if (cup >= MDKR_ONLINE_CUP_COUNT || round >= MDKR_ONLINE_CUP_ROUNDS)
        return 0u;
    return TRACKS[cup * MDKR_ONLINE_CUP_ROUNDS + round].id;
}

const char *mdkr_online_cup_name(unsigned cup) {
    return cup < MDKR_ONLINE_CUP_COUNT ? CUP_NAMES[cup] : NULL;
}
