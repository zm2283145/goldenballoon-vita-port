#ifndef MDKR64_CUSTOM_CHARACTER_ROSTER_H
#define MDKR64_CUSTOM_CHARACTER_ROSTER_H

#include <stdint.h>

#include "modern_character_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_CUSTOM_ROSTER_CAPACITY MDKR_MODERN_CHARACTER_MAX
#define MDKR_CUSTOM_ROSTER_PAGE_COLUMNS 4
#define MDKR_CUSTOM_ROSTER_PAGE_ROWS 2
#define MDKR_CUSTOM_ROSTER_PAGE_SIZE \
    (MDKR_CUSTOM_ROSTER_PAGE_COLUMNS * MDKR_CUSTOM_ROSTER_PAGE_ROWS)

typedef enum MdkrCustomRosterAvailability {
    MDKR_CUSTOM_ROSTER_AVAILABLE = 0,
    MDKR_CUSTOM_ROSTER_IDENTITY_REQUIRED,
} MdkrCustomRosterAvailability;

typedef struct MdkrCustomRosterItem {
    char id[MDKR_MODERN_CHARACTER_ID_MAX];
    char display_name[MDKR_MODERN_CHARACTER_NAME_MAX];
    int catalog_index;
    uint32_t donor;
    uint32_t vehicle_mask;
    uint64_t revision;
    MdkrCustomRosterAvailability availability;
} MdkrCustomRosterItem;

typedef struct MdkrCustomRoster {
    MdkrCustomRosterItem items[MDKR_CUSTOM_ROSTER_CAPACITY];
    int count;
    int rejected;
} MdkrCustomRoster;

typedef struct MdkrCustomRosterCursor {
    int item;
    char package_id[MDKR_MODERN_CHARACTER_ID_MAX];
} MdkrCustomRosterCursor;

typedef struct MdkrCustomRosterRaceSelection {
    int controller;
    int roster_item;
    int catalog_index;
    int donor;
} MdkrCustomRosterRaceSelection;

void mdkr_custom_roster_reset(MdkrCustomRoster *roster);
/* Adds one borrowed catalog view by value. Returns zero only for malformed,
 * duplicate, or over-capacity records; legacy records remain visible but are
 * marked as requiring an identity upgrade. */
int mdkr_custom_roster_add(MdkrCustomRoster *roster, int catalog_index,
                           const MdkrModernCharacterCatalogView *view);
void mdkr_custom_roster_sort(MdkrCustomRoster *roster);
int mdkr_custom_roster_page_count(const MdkrCustomRoster *roster);
int mdkr_custom_roster_page(const MdkrCustomRoster *roster,
                            const MdkrCustomRosterCursor *cursor);
int mdkr_custom_roster_slot(const MdkrCustomRoster *roster,
                            const MdkrCustomRosterCursor *cursor);
const MdkrCustomRosterItem *mdkr_custom_roster_current(
    const MdkrCustomRoster *roster, const MdkrCustomRosterCursor *cursor);
/* Rebinds by stable package id after a rebuild, then clamps to a valid item. */
void mdkr_custom_roster_cursor_sync(const MdkrCustomRoster *roster,
                                    MdkrCustomRosterCursor *cursor);
/* Grid movement never produces an empty slot. Horizontal movement wraps
 * within the current row; vertical movement preserves the column where the
 * target row contains it and otherwise chooses that row's final item. */
int mdkr_custom_roster_move(const MdkrCustomRoster *roster,
                            MdkrCustomRosterCursor *cursor, int dx, int dy);
int mdkr_custom_roster_change_page(const MdkrCustomRoster *roster,
                                   MdkrCustomRosterCursor *cursor, int delta);
/* Produces the game-player order used by race setup from potentially sparse
 * controller slots. Retail players have roster_item/catalog_index/donor -1.
 * Returns -1 when an active controller references an invalid roster item. */
int mdkr_custom_roster_race_plan(
    const MdkrCustomRoster *roster, const int active[MDKR_MODERN_CHARACTER_PLAYERS],
    const int selection[MDKR_MODERN_CHARACTER_PLAYERS],
    MdkrCustomRosterRaceSelection out[MDKR_MODERN_CHARACTER_PLAYERS]);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_CUSTOM_CHARACTER_ROSTER_H */
