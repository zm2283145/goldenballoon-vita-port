/* Contract test for the launcher-side online track/cup table.
 *
 * The expected values hardcoded here are the independent decode of
 * baserom.us.v80.z64: masks/defaults from the level headers, cup round
 * order from ASSET_MISC_TRACKS_MENU_IDS (the array the retail trophy race
 * indexes at (worldId - 1) * 6 + round). Divergence between this table and
 * the engine is fail-closed at admission time (manifest.vehicle_mask must
 * equal leveltable_vehicle_usable(track)), so this test pins the values the
 * UI shows to the values the engine will accept.
 *
 * NOTE: lobby_core.h does not (yet) expose a parallel cup accessor
 * (mdkr_online_cup_track); when one lands, extend this test to assert both
 * tables agree cell-for-cell across all 20 cup slots. */
#include "platform/online/online_track_table.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* Trophy-race cup contents in DKR round order, cups in world display order
 * (ASSET_MISC_TRACKS_MENU_IDS rows Dino/Snowflake/Sherbet/Dragon/FFL,
 * columns 0-3). */
static const uint16_t EXPECTED_CUPS[5][4] = {
    {5u, 3u, 29u, 7u},    /* Dino Domain */
    {13u, 6u, 9u, 28u},   /* Snowflake Mountain */
    {8u, 4u, 10u, 30u},   /* Sherbet Island */
    {19u, 18u, 20u, 31u}, /* Dragon Forest */
    {17u, 32u, 33u, 15u}, /* Future Fun Land */
};

static void test_count_and_bounds(void) {
    expect(mdkr_online_track_count() == 20u, "track count is 20");
    expect(mdkr_online_track_count() == MDKR_ONLINE_TRACK_TABLE_COUNT,
           "count accessor agrees with the header constant");
    expect(mdkr_online_track_at(0u) != NULL, "index 0 resolves");
    expect(mdkr_online_track_at(19u) != NULL, "index 19 resolves");
    expect(mdkr_online_track_at(20u) == NULL, "index 20 is out of range");
    expect(mdkr_online_track_at(0xFFFFFFFFu) == NULL,
           "huge index is out of range");
}

static void test_ids_unique_and_roundtrip(void) {
    unsigned i, j;
    for (i = 0u; i < mdkr_online_track_count(); i++) {
        const MdkrOnlineTrackInfo *track = mdkr_online_track_at(i);
        expect(track != NULL, "every display index resolves");
        if (track == NULL) continue;
        for (j = i + 1u; j < mdkr_online_track_count(); j++) {
            const MdkrOnlineTrackInfo *other = mdkr_online_track_at(j);
            expect(other != NULL && other->id != track->id,
                   "track ids are unique");
        }
        expect(mdkr_online_track_by_id(track->id) == track,
               "by_id round-trips to the same row");
        expect(track->name != NULL && track->name[0] != '\0',
               "every track has a display name");
    }
    expect(mdkr_online_track_by_id(0u) == NULL,
           "id 0 (Central Area hub) is not a race track");
    expect(mdkr_online_track_by_id(16u) == NULL,
           "id 16 (Horseshoe Gulch) is not a standard race track");
    expect(mdkr_online_track_by_id(0xFFFFu) == NULL, "garbage id is unknown");
}

static void test_vehicle_masks(void) {
    unsigned i;
    for (i = 0u; i < mdkr_online_track_count(); i++) {
        const MdkrOnlineTrackInfo *track = mdkr_online_track_at(i);
        if (track == NULL) continue;
        expect(track->vehicle_mask != 0u, "every mask is nonzero");
        expect((track->vehicle_mask & ~MDKR_ONLINE_VEHICLE_BIT_ALL) == 0u,
               "every mask is a subset of 0x7");
        expect(track->default_vehicle <= MDKR_ONLINE_VEHICLE_PLANE,
               "default vehicle id is a real vehicle");
        expect((track->vehicle_mask & (1u << track->default_vehicle)) != 0u,
               "default vehicle bit sits inside the mask");
    }
}

static void test_picker_narrowing(void) {
    unsigned i;
    unsigned playerCount;
    for (i = 0u; i < mdkr_online_track_count(); i++) {
        const MdkrOnlineTrackInfo *track = mdkr_online_track_at(i);
        uint8_t raw;
        if (track == NULL) continue;
        raw = track->vehicle_mask;
        expect(mdkr_online_track_picker_mask(track->id, 0u) == raw,
               "player count 0 never narrows");
        expect(mdkr_online_track_picker_mask(track->id, 1u) == raw,
               "single player never narrows");
        for (playerCount = 2u; playerCount <= 4u; playerCount++) {
            uint8_t narrowed =
                mdkr_online_track_picker_mask(track->id, playerCount);
            if (track->id == 15u) {
                expect(narrowed ==
                           (raw & (uint8_t)~MDKR_ONLINE_VEHICLE_BIT_HOVERCRAFT),
                       "Spaceport Alpha drops the hovercraft at 2+ players");
            } else if (track->id == 28u) {
                expect(narrowed ==
                           (raw & (uint8_t)~MDKR_ONLINE_VEHICLE_BIT_PLANE),
                       "Frosty Village drops the plane at 2+ players");
            } else {
                expect(narrowed == raw,
                       "no other track narrows at 2+ players");
            }
            expect(narrowed != 0u, "narrowing never empties a picker mask");
        }
    }
    expect(mdkr_online_track_picker_mask(0xFFFFu, 1u) == 0u,
           "unknown id yields the empty picker mask");
    expect(mdkr_online_track_picker_mask(0u, 4u) == 0u,
           "hub id yields the empty picker mask");
}

static void test_cups(void) {
    unsigned cup, round;
    for (cup = 0u; cup < MDKR_ONLINE_CUP_COUNT; cup++) {
        expect(mdkr_online_cup_name(cup) != NULL, "every cup has a name");
        for (round = 0u; round < MDKR_ONLINE_CUP_ROUNDS; round++) {
            uint16_t id = mdkr_online_cup_track_id(cup, round);
            const MdkrOnlineTrackInfo *track = mdkr_online_track_by_id(id);
            expect(id == EXPECTED_CUPS[cup][round],
                   "cup cell matches the ROM trophy-race round order");
            expect(track != NULL, "every cup cell resolves to a known track");
            expect(track != NULL && track->world == cup,
                   "each cup's tracks share the cup's world");
        }
    }
    expect(mdkr_online_cup_track_id(5u, 0u) == 0u, "cup 5 is out of range");
    expect(mdkr_online_cup_track_id(0u, 4u) == 0u, "round 4 is out of range");
    expect(mdkr_online_cup_name(5u) == NULL, "cup 5 has no name");
}

static void test_display_order(void) {
    unsigned i;
    uint8_t seen_max = 0u;
    const MdkrOnlineTrackInfo *first = mdkr_online_track_at(0u);
    expect(first != NULL && first->world == MDKR_ONLINE_WORLD_DINO_DOMAIN,
           "display order starts with Dino Domain");
    for (i = 0u; i < mdkr_online_track_count(); i++) {
        const MdkrOnlineTrackInfo *track = mdkr_online_track_at(i);
        if (track == NULL) continue;
        expect(track->world < MDKR_ONLINE_WORLD_COUNT,
               "every world value is a real world");
        expect(track->world == seen_max || track->world == seen_max + 1u,
               "worlds are contiguous and in display order");
        if (track->world > seen_max) seen_max = track->world;
        expect(track->world == i / MDKR_ONLINE_CUP_ROUNDS,
               "exactly four tracks per world, in world display order");
    }
    expect(seen_max == MDKR_ONLINE_WORLD_FUTURE_FUN_LAND,
           "display order ends with Future Fun Land");
}

static void test_world_names(void) {
    uint8_t world;
    static const char *const expected[5] = {
        "Dino Domain", "Snowflake Mountain", "Sherbet Island",
        "Dragon Forest", "Future Fun Land"};
    for (world = 0u; world < MDKR_ONLINE_WORLD_COUNT; world++) {
        const char *name = mdkr_online_world_name(world);
        expect(name != NULL && strcmp(name, expected[world]) == 0,
               "world names follow display order");
    }
    expect(mdkr_online_world_name(MDKR_ONLINE_WORLD_COUNT) == NULL,
           "world 5 has no name");
}

int main(void) {
    test_count_and_bounds();
    test_ids_unique_and_roundtrip();
    test_vehicle_masks();
    test_picker_narrowing();
    test_cups();
    test_display_order();
    test_world_names();
    if (failures != 0) return 1;
    puts("online track table contract passed");
    return 0;
}
