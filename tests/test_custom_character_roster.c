#include "custom_character_roster.h"
#include "modern_character_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t s_portrait[MDKR_MODERN_PORTRAIT_BYTES];

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test_custom_character_roster: %s\n", message);
        exit(1);
    }
}

static MdkrModernCharacterCatalogView view(const char *id, const char *name,
                                           uint32_t identity) {
    MdkrModernCharacterCatalogView result;
    memset(&result, 0, sizeof(result));
    result.id = id;
    result.display_name = name;
    result.portrait_rgba = identity ? s_portrait : NULL;
    result.portrait_width = identity ? MDKR_MODERN_PORTRAIT_SIZE : 0u;
    result.portrait_height = identity ? MDKR_MODERN_PORTRAIT_SIZE : 0u;
    result.portrait_stride = identity ? MDKR_MODERN_PORTRAIT_SIZE * 4u : 0u;
    result.donor = 9u;
    result.vehicle_mask = 7u;
    result.has_identity = identity;
    result.revision = 42u;
    return result;
}

int main(void) {
    MdkrCustomRoster roster;
    MdkrCustomRosterCursor cursor;
    MdkrModernCharacterCatalogView catalog;
    MdkrCustomRosterRaceSelection plan[MDKR_MODERN_CHARACTER_PLAYERS];
    int active[MDKR_MODERN_CHARACTER_PLAYERS] = {0, 1, 0, 1};
    int selection[MDKR_MODERN_CHARACTER_PLAYERS] = {-1, 8, -1, 9};
    char ids[MDKR_CUSTOM_ROSTER_CAPACITY][16];
    char names[MDKR_CUSTOM_ROSTER_CAPACITY][24];
    int i;
    char projected[32];
    MdkrModernCharacterTextProjection text_projection;

    require(mdkr_modern_character_text_project(
                "Dixie Kong", 11u, projected, sizeof(projected),
                &text_projection) &&
                strcmp(projected, "Dixie Kong") == 0 &&
                text_projection.valid_utf8 &&
                text_projection.input_codepoints == 10u &&
                text_projection.unsupported_codepoints == 0u &&
                !text_projection.output_truncated,
            "printable ASCII game-font projection changed supported text");
    require(mdkr_modern_character_text_project(
                "Dixie \xC3\x89 \xF0\x9F\x8F\x81", 14u,
                projected, sizeof(projected), &text_projection) &&
                strcmp(projected, "Dixie ? ?") == 0 &&
                text_projection.valid_utf8 &&
                text_projection.input_codepoints == 9u &&
                text_projection.unsupported_codepoints == 2u,
            "UTF-8 codepoints did not project one-for-one into fallback cells");
    require(mdkr_modern_character_text_project(
                "A\tB", 4u, projected, sizeof(projected),
                &text_projection) &&
                strcmp(projected, "A B") == 0 &&
                text_projection.replaced_controls == 1u,
            "control bytes were not neutralized in the game-font projection");
    {
        static const char malformed[] = {
            (char)0xF0, '(', (char)0x8C, '(', '\0'
        };
        require(mdkr_modern_character_text_project(
                    malformed, sizeof(malformed), projected,
                    sizeof(projected), &text_projection) &&
                    strcmp(projected, "?(?(") == 0 &&
                    !text_projection.valid_utf8 &&
                    text_projection.unsupported_codepoints == 2u,
                "malformed UTF-8 projection was not bounded and explicit");
    }
    require(mdkr_modern_character_text_project(
                "Dixie", 6u, projected, 4u, &text_projection) &&
                strcmp(projected, "Dix") == 0 &&
                text_projection.output_truncated,
            "bounded projection did not report output truncation");
    {
        static const char unterminated[] = {'A', 'B'};
        require(!mdkr_modern_character_text_project(
                    unterminated, sizeof(unterminated), projected,
                    sizeof(projected), &text_projection) &&
                    strcmp(projected, "AB") == 0,
                "unterminated source was accepted by the bounded projection");
    }

    mdkr_custom_roster_reset(&roster);
    memset(&cursor, 0, sizeof(cursor));
    cursor.item = -1;
    mdkr_custom_roster_cursor_sync(&roster, &cursor);
    require(cursor.item == -1 && mdkr_custom_roster_page_count(&roster) == 0,
            "empty roster must have no cursor or pages");

    catalog = view("zeta", "Zeta", 1u);
    catalog.short_name = "Z";
    catalog.narration_name = "Zeta custom character";
    catalog.sort_label = "00 Zeta";
    require(mdkr_custom_roster_add(&roster, 17, &catalog),
            "valid identity record rejected");
    require(strcmp(roster.items[0].short_name, "Z") == 0 &&
                strcmp(roster.items[0].narration_name,
                       "Zeta custom character") == 0 &&
                strcmp(roster.items[0].sort_label, "00 Zeta") == 0,
            "authored identity names were not copied into the virtual roster");
    catalog = view("legacy", "Legacy", 0u);
    require(mdkr_custom_roster_add(&roster, 9, &catalog),
            "legacy record should remain visible");
    require(roster.items[1].availability == MDKR_CUSTOM_ROSTER_IDENTITY_REQUIRED,
            "legacy record was not marked unavailable");
    require(!mdkr_custom_roster_add(&roster, 10, &catalog) && roster.rejected == 1,
            "duplicate package id was not rejected");
    catalog = view("alpha", "alpha", 1u);
    require(mdkr_custom_roster_add(&roster, 3, &catalog), "alpha rejected");
    mdkr_custom_roster_sort(&roster);
    require(strcmp(roster.items[0].id, "zeta") == 0 &&
                strcmp(roster.items[1].id, "alpha") == 0 &&
                strcmp(roster.items[2].id, "legacy") == 0,
            "case-insensitive authored sort label is not deterministic");

    cursor.item = 2;
    snprintf(cursor.package_id, sizeof(cursor.package_id), "%s", "legacy");
    mdkr_custom_roster_cursor_sync(&roster, &cursor);
    require(cursor.item == 2, "cursor did not rebind by stable package id");

    mdkr_custom_roster_reset(&roster);
    memset(&cursor, 0, sizeof(cursor));
    cursor.item = -1;
    for (i = 0; i < MDKR_CUSTOM_ROSTER_CAPACITY; i++) {
        snprintf(ids[i], sizeof(ids[i]), "character-%02d", i);
        snprintf(names[i], sizeof(names[i]), "Character %02d", i);
        catalog = view(ids[i], names[i], 1u);
        catalog.donor = (uint32_t)(i % 10);
        require(mdkr_custom_roster_add(&roster, 100 + i, &catalog),
                "capacity record rejected");
    }
    catalog = view("overflow", "Overflow", 1u);
    require(!mdkr_custom_roster_add(&roster, 999, &catalog),
            "over-capacity record accepted");
    require(roster.count == MDKR_CUSTOM_ROSTER_CAPACITY &&
                mdkr_custom_roster_page_count(&roster) == 8,
            "64-entry pagination is incorrect");

    mdkr_custom_roster_cursor_sync(&roster, &cursor);
    require(cursor.item == 0 && mdkr_custom_roster_page(&roster, &cursor) == 0,
            "cursor did not initialize to first item");
    require(mdkr_custom_roster_move(&roster, &cursor, -1, 0) &&
                cursor.item == 3,
            "left movement did not wrap within row");
    require(mdkr_custom_roster_move(&roster, &cursor, 0, 1) &&
                cursor.item == 7,
            "vertical movement did not preserve column");
    require(mdkr_custom_roster_change_page(&roster, &cursor, 1) &&
                cursor.item == 15 && mdkr_custom_roster_page(&roster, &cursor) == 1,
            "next page did not preserve slot");
    require(mdkr_custom_roster_change_page(&roster, &cursor, -1) &&
                cursor.item == 7,
            "previous page did not preserve slot");
    cursor.item = 63;
    cursor.package_id[0] = '\0';
    mdkr_custom_roster_cursor_sync(&roster, &cursor);
    require(mdkr_custom_roster_change_page(&roster, &cursor, 1) &&
                cursor.item == 7,
            "last-to-first page wrap is incorrect");
    require(mdkr_custom_roster_current(&roster, &cursor)->catalog_index == 107,
            "selection lost its original catalog index");

    mdkr_custom_roster_reset(&roster);
    for (i = 0; i < 10; i++) {
        snprintf(ids[i], sizeof(ids[i]), "short-%02d", i);
        snprintf(names[i], sizeof(names[i]), "Short %02d", i);
        catalog = view(ids[i], names[i], 1u);
        require(mdkr_custom_roster_add(&roster, i, &catalog),
                "short-page record rejected");
    }
    cursor.item = 7;
    cursor.package_id[0] = '\0';
    require(mdkr_custom_roster_change_page(&roster, &cursor, 1) &&
                cursor.item == 9,
            "partial target page did not clamp to final item");
    require(mdkr_custom_roster_move(&roster, &cursor, -1, 0) &&
                cursor.item == 8,
            "partial row horizontal navigation is incorrect");
    require(!mdkr_custom_roster_move(&roster, &cursor, 0, 1),
            "movement entered a nonexistent row");
    require(mdkr_custom_roster_race_plan(&roster, active, selection, plan) == 2,
            "sparse controller race plan has the wrong player count");
    require(plan[0].controller == 1 && plan[0].catalog_index == 8 &&
                plan[0].donor == 9 && plan[1].controller == 3 &&
                plan[1].catalog_index == 9 && plan[2].controller == -1,
            "sparse controller race plan lost ordering or exact donor data");
    selection[3] = 10;
    require(mdkr_custom_roster_race_plan(&roster, active, selection, plan) == -1,
            "race plan accepted an out-of-range selection");
    selection[3] = -2;
    require(mdkr_custom_roster_race_plan(&roster, active, selection, plan) == -1,
            "race plan accepted an invalid negative selection");
    selection[1] = -1;
    selection[3] = -1;
    require(mdkr_custom_roster_race_plan(&roster, active, selection, plan) == 2 &&
                plan[0].catalog_index == -1 && plan[0].donor == -1,
            "race plan did not preserve retail players explicitly");

    puts("test_custom_character_roster: PASS");
    return 0;
}
