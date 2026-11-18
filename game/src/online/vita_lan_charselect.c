#include "online/vita_lan_charselect.h"

#if defined(__vita__) && defined(MDKR_ENABLE_VITA_LAN)

#include "PR/os_cont.h"
#include "asset_enums.h"
#include "audio.h"
#include "font.h"
#include "joypad.h"
#include "menu.h"
#include "rcp_dkr.h"
#include "sound_ids.h"
#include "vita_lan.h"

#include <string.h>

extern Gfx *sMenuCurrDisplayList;
extern DrawTexture *gRacerPortraits[10];

enum {
    CHARSELECT_COLS = 5,
    CHARSELECT_COUNT = 10,
    CHARSELECT_CELL_W = 60,
    CHARSELECT_CELL_H = 67,
    CHARSELECT_GRID_X = 22,
    CHARSELECT_GRID_Y = 48
};

static const u8 sGridCharacters[CHARSELECT_COUNT] = {
    0u, 9u, 6u, 1u, 5u,
    3u, 2u, 8u, 7u, 4u
};

static const char *const sCharacterNames[CHARSELECT_COUNT] = {
    "KRUNCH", "BUMPER", "TIPTUP", "CONKER", "TIMBER",
    "BANJO", "DRUMSTICK", "PIPSY", "T.T.", "DIDDY"
};

static s16 sPortraitAssets[] = {
    TEXTURE_ICON_PORTRAIT_KRUNCH, TEXTURE_ICON_PORTRAIT_DIDDY,
    TEXTURE_ICON_PORTRAIT_DRUMSTICK, TEXTURE_ICON_PORTRAIT_BUMPER,
    TEXTURE_ICON_PORTRAIT_BANJO, TEXTURE_ICON_PORTRAIT_CONKER,
    TEXTURE_ICON_PORTRAIT_TIPTUP, TEXTURE_ICON_PORTRAIT_TT,
    TEXTURE_ICON_PORTRAIT_PIPSY, TEXTURE_ICON_PORTRAIT_TIMBER,
    -1
};

static struct {
    u8 active;
    u8 cursorCell;
    u8 confirmed;
    s8 stickX;
    s8 stickY;
    u16 blink;
} sCharselect;

static unsigned int cell_for_character(unsigned int character) {
    unsigned int cell;
    for (cell = 0; cell < CHARSELECT_COUNT; cell++) {
        if (sGridCharacters[cell] == character) {
            return cell;
        }
    }
    return 1u;
}

static void charselect_enter(unsigned int localSlot,
                             const unsigned char characters[2],
                             const unsigned char confirmed[2]) {
    memset(&sCharselect, 0, sizeof(sCharselect));
    sCharselect.active = 1;
    sCharselect.cursorCell =
        (u8)cell_for_character(characters[localSlot]);
    sCharselect.confirmed = confirmed[localSlot];
    menu_assetgroup_load(sPortraitAssets);
    menu_racer_portraits();
}

void mdkr_vita_lan_charselect_close(void) {
    if (!sCharselect.active) {
        return;
    }
    gfxtask_wait();
    menu_assetgroup_free(sPortraitAssets);
    memset(&sCharselect, 0, sizeof(sCharselect));
}

static void draw_text_shadowed(s32 x, s32 y, char *text,
                               AlignmentFlags alignment,
                               s32 red, s32 green, s32 blue) {
    set_text_font(ASSET_FONTS_FUNFONT);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(0, 0, 0, 255, 180);
    draw_text(&sMenuCurrDisplayList, x + 1, y + 2, text, alignment);
    set_text_colour(red, green, blue, 0, 255);
    draw_text(&sMenuCurrDisplayList, x, y, text, alignment);
}

static void draw_marker(unsigned int character, unsigned int number,
                        int local) {
    unsigned int cell = cell_for_character(character);
    s32 x = CHARSELECT_GRID_X +
        (s32)(cell % CHARSELECT_COLS) * CHARSELECT_CELL_W + 22;
    s32 y = CHARSELECT_GRID_Y +
        (s32)(cell / CHARSELECT_COLS) * CHARSELECT_CELL_H - 8;
    char label[3] = {'P', (char)('1' + number), '\0'};
    draw_text_shadowed(x, y, label, ALIGN_MIDDLE_CENTER,
                       local ? 255 : 96, local ? 220 : 180,
                       local ? 64 : 255);
}

static void charselect_draw(unsigned int localSlot,
                            const unsigned char characters[2],
                            const unsigned char confirmed[2]) {
    unsigned int cell;
    unsigned int remoteSlot = 1u - localSlot;
    s32 pulse = (sCharselect.blink & 0x1f) << 4;
    char footer[64];
    if (pulse > 255) {
        pulse = 511 - pulse;
    }
    draw_text_shadowed(160, 22, "PLAYER SELECT", ALIGN_MIDDLE_CENTER,
                       255, 220, 80);
    for (cell = 0; cell < CHARSELECT_COUNT; cell++) {
        unsigned int character = sGridCharacters[cell];
        s32 x = CHARSELECT_GRID_X +
            (s32)(cell % CHARSELECT_COLS) * CHARSELECT_CELL_W;
        s32 y = CHARSELECT_GRID_Y +
            (s32)(cell / CHARSELECT_COLS) * CHARSELECT_CELL_H;
        u8 red = 210u;
        u8 green = 210u;
        u8 blue = 210u;
        if (confirmed[remoteSlot] &&
            characters[remoteSlot] == character) {
            red = green = blue = 72u;
        }
        if (cell == sCharselect.cursorCell) {
            red = sCharselect.confirmed ? 96u : 255u;
            green = sCharselect.confirmed ? 255u : (u8)(160 + pulse / 3);
            blue = sCharselect.confirmed ? 96u : 64u;
        }
        if (gRacerPortraits[character] != NULL) {
            texrect_draw(&sMenuCurrDisplayList, gRacerPortraits[character],
                         x, y, red, green, blue, 255);
        }
        draw_text_shadowed(x + 22, y + 47,
                           (char *)sCharacterNames[character],
                           ALIGN_MIDDLE_CENTER, 255, 255, 255);
    }
    draw_marker(characters[localSlot], localSlot, 1);
    draw_marker(characters[remoteSlot], remoteSlot, 0);
    if (confirmed[0] && confirmed[1]) {
        strcpy(footer, "READY - STARTING RACE");
    } else if (sCharselect.confirmed) {
        strcpy(footer, "SELECTED   B: CHANGE");
    } else {
        strcpy(footer, "A: SELECT   CHOOSE A DIFFERENT RACER");
    }
    draw_text_shadowed(160, 208, footer, ALIGN_MIDDLE_CENTER,
                       255, 255, 255);
}

s32 mdkr_vita_lan_charselect_frame(s32 updateRate) {
    unsigned int localSlot;
    unsigned int remoteSlot;
    unsigned char characters[2];
    unsigned char confirmed[2];
    unsigned int currentCharacter;
    u32 pressed;
    s32 sx;
    s32 sy;
    s8 wantX = 0;
    s8 wantY = 0;

    if (!mdkr_vita_lan_character_selection(
            &localSlot, characters, confirmed)) {
        mdkr_vita_lan_charselect_close();
        return FALSE;
    }
    if (!sCharselect.active) {
        charselect_enter(localSlot, characters, confirmed);
    }
    remoteSlot = 1u - localSlot;
    sCharselect.confirmed = confirmed[localSlot];
    sCharselect.cursorCell =
        (u8)cell_for_character(characters[localSlot]);
    sCharselect.blink =
        (u16)((sCharselect.blink + (updateRate > 0 ? updateRate : 1)) &
              0x3f);

    pressed = input_pressed(0);
    sx = input_clamp_stick_x(0);
    sy = input_clamp_stick_y(0);
    if (pressed & R_JPAD) wantX = 1;
    if (pressed & L_JPAD) wantX = -1;
    if (pressed & D_JPAD) wantY = 1;
    if (pressed & U_JPAD) wantY = -1;
    if (wantX == 0) {
        if (sx > 40 && sCharselect.stickX == 0) wantX = 1;
        if (sx < -40 && sCharselect.stickX == 0) wantX = -1;
    }
    if (wantY == 0) {
        if (sy < -40 && sCharselect.stickY == 0) wantY = 1;
        if (sy > 40 && sCharselect.stickY == 0) wantY = -1;
    }
    sCharselect.stickX = sx > 40 ? 1 : (sx < -40 ? -1 : 0);
    sCharselect.stickY = sy > 40 ? 1 : (sy < -40 ? -1 : 0);

    if (!sCharselect.confirmed && (wantX != 0 || wantY != 0)) {
        s32 column = sCharselect.cursorCell % CHARSELECT_COLS;
        s32 row = sCharselect.cursorCell / CHARSELECT_COLS;
        column = (column + wantX + CHARSELECT_COLS) % CHARSELECT_COLS;
        row = (row + wantY + 2) % 2;
        sCharselect.cursorCell = (u8)(row * CHARSELECT_COLS + column);
        currentCharacter = sGridCharacters[sCharselect.cursorCell];
        (void)mdkr_vita_lan_set_character_selection(
            currentCharacter, 0);
        sound_play(SOUND_MENU_PICK3, NULL);
    }

    currentCharacter = sGridCharacters[sCharselect.cursorCell];
    if ((pressed & A_BUTTON) && !sCharselect.confirmed) {
        if (!confirmed[remoteSlot] ||
            characters[remoteSlot] != currentCharacter) {
            if (mdkr_vita_lan_set_character_selection(
                    currentCharacter, 1)) {
                sCharselect.confirmed = 1;
                sound_play(SOUND_SELECT2, NULL);
            }
        } else {
            sound_play(SOUND_HORN_DRUMSTICK, NULL);
        }
    } else if ((pressed & B_BUTTON) && sCharselect.confirmed) {
        (void)mdkr_vita_lan_set_character_selection(
            currentCharacter, 0);
        sCharselect.confirmed = 0;
        sound_play(SOUND_MENU_BACK3, NULL);
    }

    if (!mdkr_vita_lan_character_selection(
            &localSlot, characters, confirmed)) {
        mdkr_vita_lan_charselect_close();
        return FALSE;
    }
    charselect_draw(localSlot, characters, confirmed);
    return TRUE;
}

#endif
