#include "custom_character_roster.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int roster_string_valid(const char *text, size_t capacity) {
    size_t length;
    if (text == NULL || text[0] == '\0') return 0;
    length = strnlen(text, capacity);
    return length > 0u && length < capacity;
}

static int roster_ascii_compare(const char *left, const char *right) {
    const char *left_start = left;
    const char *right_start = right;
    unsigned char a;
    unsigned char b;
    while (*left != '\0' && *right != '\0') {
        a = (unsigned char)tolower((unsigned char)*left++);
        b = (unsigned char)tolower((unsigned char)*right++);
        if (a != b) return a < b ? -1 : 1;
    }
    if (*left != *right) return *left == '\0' ? -1 : 1;
    return strcmp(left_start, right_start);
}

static int roster_item_compare(const void *left, const void *right) {
    const MdkrCustomRosterItem *a = (const MdkrCustomRosterItem *)left;
    const MdkrCustomRosterItem *b = (const MdkrCustomRosterItem *)right;
    int order = roster_ascii_compare(a->display_name, b->display_name);
    return order != 0 ? order : strcmp(a->id, b->id);
}

static void roster_cursor_remember(const MdkrCustomRoster *roster,
                                   MdkrCustomRosterCursor *cursor) {
    if (roster == NULL || cursor == NULL || cursor->item < 0 ||
        cursor->item >= roster->count) {
        if (cursor != NULL) cursor->package_id[0] = '\0';
        return;
    }
    (void)snprintf(cursor->package_id, sizeof(cursor->package_id), "%s",
                   roster->items[cursor->item].id);
}

void mdkr_custom_roster_reset(MdkrCustomRoster *roster) {
    if (roster != NULL) memset(roster, 0, sizeof(*roster));
}

int mdkr_custom_roster_add(MdkrCustomRoster *roster, int catalog_index,
                           const MdkrModernCharacterCatalogView *view) {
    MdkrCustomRosterItem *item;
    int i;
    if (roster == NULL || view == NULL || catalog_index < 0 ||
        roster->count < 0 || roster->count >= MDKR_CUSTOM_ROSTER_CAPACITY ||
        !roster_string_valid(view->id, MDKR_MODERN_CHARACTER_ID_MAX) ||
        !roster_string_valid(view->display_name,
                             MDKR_MODERN_CHARACTER_NAME_MAX) ||
        view->donor >= 10u || view->vehicle_mask == 0u ||
        (view->vehicle_mask & ~7u) != 0u) {
        if (roster != NULL) roster->rejected++;
        return 0;
    }
    for (i = 0; i < roster->count; i++) {
        if (strcmp(roster->items[i].id, view->id) == 0) {
            roster->rejected++;
            return 0;
        }
    }
    item = &roster->items[roster->count++];
    memset(item, 0, sizeof(*item));
    (void)snprintf(item->id, sizeof(item->id), "%s", view->id);
    (void)snprintf(item->display_name, sizeof(item->display_name), "%s",
                   view->display_name);
    item->catalog_index = catalog_index;
    item->donor = view->donor;
    item->vehicle_mask = view->vehicle_mask;
    item->revision = view->revision != 0u ? view->revision : 1u;
    item->availability = view->has_identity && view->portrait_rgba != NULL &&
            view->portrait_width == MDKR_MODERN_PORTRAIT_SIZE &&
            view->portrait_height == MDKR_MODERN_PORTRAIT_SIZE &&
            view->portrait_stride == MDKR_MODERN_PORTRAIT_SIZE * 4u
        ? MDKR_CUSTOM_ROSTER_AVAILABLE
        : MDKR_CUSTOM_ROSTER_IDENTITY_REQUIRED;
    return 1;
}

void mdkr_custom_roster_sort(MdkrCustomRoster *roster) {
    if (roster == NULL || roster->count <= 1) return;
    qsort(roster->items, (size_t)roster->count, sizeof(roster->items[0]),
          roster_item_compare);
}

int mdkr_custom_roster_page_count(const MdkrCustomRoster *roster) {
    if (roster == NULL || roster->count <= 0) return 0;
    return (roster->count + MDKR_CUSTOM_ROSTER_PAGE_SIZE - 1) /
           MDKR_CUSTOM_ROSTER_PAGE_SIZE;
}

int mdkr_custom_roster_page(const MdkrCustomRoster *roster,
                            const MdkrCustomRosterCursor *cursor) {
    if (roster == NULL || cursor == NULL || cursor->item < 0 ||
        cursor->item >= roster->count) return 0;
    return cursor->item / MDKR_CUSTOM_ROSTER_PAGE_SIZE;
}

int mdkr_custom_roster_slot(const MdkrCustomRoster *roster,
                            const MdkrCustomRosterCursor *cursor) {
    if (roster == NULL || cursor == NULL || cursor->item < 0 ||
        cursor->item >= roster->count) return 0;
    return cursor->item % MDKR_CUSTOM_ROSTER_PAGE_SIZE;
}

const MdkrCustomRosterItem *mdkr_custom_roster_current(
    const MdkrCustomRoster *roster, const MdkrCustomRosterCursor *cursor) {
    if (roster == NULL || cursor == NULL || cursor->item < 0 ||
        cursor->item >= roster->count) return NULL;
    return &roster->items[cursor->item];
}

void mdkr_custom_roster_cursor_sync(const MdkrCustomRoster *roster,
                                    MdkrCustomRosterCursor *cursor) {
    int i;
    if (cursor == NULL) return;
    if (roster == NULL || roster->count <= 0) {
        cursor->item = -1;
        cursor->package_id[0] = '\0';
        return;
    }
    if (cursor->package_id[0] != '\0') {
        for (i = 0; i < roster->count; i++) {
            if (strcmp(roster->items[i].id, cursor->package_id) == 0) {
                cursor->item = i;
                roster_cursor_remember(roster, cursor);
                return;
            }
        }
    }
    if (cursor->item < 0) cursor->item = 0;
    if (cursor->item >= roster->count) cursor->item = roster->count - 1;
    roster_cursor_remember(roster, cursor);
}

int mdkr_custom_roster_move(const MdkrCustomRoster *roster,
                            MdkrCustomRosterCursor *cursor, int dx, int dy) {
    int old_item;
    int page_start;
    int page_end;
    int slot;
    int row;
    int column;
    int target_row;
    int target_start;
    int target_count;
    if (roster == NULL || cursor == NULL || roster->count <= 0) return 0;
    mdkr_custom_roster_cursor_sync(roster, cursor);
    old_item = cursor->item;
    page_start = (cursor->item / MDKR_CUSTOM_ROSTER_PAGE_SIZE) *
                 MDKR_CUSTOM_ROSTER_PAGE_SIZE;
    page_end = page_start + MDKR_CUSTOM_ROSTER_PAGE_SIZE;
    if (page_end > roster->count) page_end = roster->count;
    slot = cursor->item - page_start;
    row = slot / MDKR_CUSTOM_ROSTER_PAGE_COLUMNS;
    column = slot % MDKR_CUSTOM_ROSTER_PAGE_COLUMNS;
    if (dx != 0) {
        target_start = page_start + row * MDKR_CUSTOM_ROSTER_PAGE_COLUMNS;
        target_count = page_end - target_start;
        if (target_count > MDKR_CUSTOM_ROSTER_PAGE_COLUMNS) {
            target_count = MDKR_CUSTOM_ROSTER_PAGE_COLUMNS;
        }
        if (target_count > 0) {
            column = (column + (dx > 0 ? 1 : target_count - 1)) % target_count;
            cursor->item = target_start + column;
        }
    }
    if (dy != 0) {
        target_row = row + (dy > 0 ? 1 : -1);
        if (target_row >= 0 && target_row < MDKR_CUSTOM_ROSTER_PAGE_ROWS) {
            target_start = page_start +
                           target_row * MDKR_CUSTOM_ROSTER_PAGE_COLUMNS;
            target_count = page_end - target_start;
            if (target_count > MDKR_CUSTOM_ROSTER_PAGE_COLUMNS) {
                target_count = MDKR_CUSTOM_ROSTER_PAGE_COLUMNS;
            }
            if (target_count > 0) {
                if (column >= target_count) column = target_count - 1;
                cursor->item = target_start + column;
            }
        }
    }
    roster_cursor_remember(roster, cursor);
    return cursor->item != old_item;
}

int mdkr_custom_roster_change_page(const MdkrCustomRoster *roster,
                                   MdkrCustomRosterCursor *cursor, int delta) {
    int old_item;
    int page_count;
    int page;
    int slot;
    int target;
    if (roster == NULL || cursor == NULL || roster->count <= 0 || delta == 0) {
        return 0;
    }
    mdkr_custom_roster_cursor_sync(roster, cursor);
    old_item = cursor->item;
    page_count = mdkr_custom_roster_page_count(roster);
    page = old_item / MDKR_CUSTOM_ROSTER_PAGE_SIZE;
    slot = old_item % MDKR_CUSTOM_ROSTER_PAGE_SIZE;
    page = (page + (delta > 0 ? 1 : page_count - 1)) % page_count;
    target = page * MDKR_CUSTOM_ROSTER_PAGE_SIZE + slot;
    if (target >= roster->count) target = roster->count - 1;
    cursor->item = target;
    roster_cursor_remember(roster, cursor);
    return cursor->item != old_item;
}

int mdkr_custom_roster_race_plan(
    const MdkrCustomRoster *roster, const int active[MDKR_MODERN_CHARACTER_PLAYERS],
    const int selection[MDKR_MODERN_CHARACTER_PLAYERS],
    MdkrCustomRosterRaceSelection out[MDKR_MODERN_CHARACTER_PLAYERS]) {
    int controller;
    int count = 0;
    if (roster == NULL || active == NULL || selection == NULL || out == NULL) {
        return -1;
    }
    for (controller = 0; controller < MDKR_MODERN_CHARACTER_PLAYERS;
         controller++) {
        const MdkrCustomRosterItem *item = NULL;
        if (!active[controller]) continue;
        if (selection[controller] < -1) return -1;
        if (selection[controller] >= 0) {
            if (selection[controller] >= roster->count) return -1;
            item = &roster->items[selection[controller]];
            if (item->availability != MDKR_CUSTOM_ROSTER_AVAILABLE) return -1;
        }
        out[count].controller = controller;
        out[count].roster_item = selection[controller];
        out[count].catalog_index = item != NULL ? item->catalog_index : -1;
        out[count].donor = item != NULL ? (int)item->donor : -1;
        count++;
    }
    for (controller = count; controller < MDKR_MODERN_CHARACTER_PLAYERS;
         controller++) {
        out[controller].controller = -1;
        out[controller].roster_item = -1;
        out[controller].catalog_index = -1;
        out[controller].donor = -1;
    }
    return count;
}
