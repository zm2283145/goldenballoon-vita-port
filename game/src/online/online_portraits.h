#ifndef MDKR_ONLINE_PORTRAITS_H
#define MDKR_ONLINE_PORTRAITS_H

/* SEPARATED-BOOT-PATH (Strategy D2) shared portrait / name / asset-id tables.
 *
 * The THREE byte-identical tables the native online CHARSELECT, RESULTS and
 * CEREMONY screens each need to blit the ten racer faces + their short names:
 *   - sOnlineToPortrait[]  online char id -> gRacerPortraits[] slot remap
 *   - sOnlineNames[]       online char id -> canonical short display name
 *   - sPortraitAssetIds[]  the portrait-ONLY texture group (KRUNCH..TIMBER, -1)
 *
 * Keeping them here (rather than a per-screen copy) is a drift guard: a future
 * character re-order or portrait-asset change applied to only some of the screens
 * would silently draw the WRONG FACE on one, with no compile-time or headless
 * catch (each screen's witness only asserts its own copy). One source guarantees
 * the three screens can NEVER disagree about which face/name maps to a given
 * online id -- the obvious sibling of the online_standings.h lift.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and it is only ever included by the beta-gated online TUs
 * (game/src/online/ is NOT auto-globbed). The tables are file-scope statics (each
 * TU gets its own copy -- no linkage change, no new object, no ODR risk), the same
 * discipline online_standings.h models. sPortraitAssetIds is intentionally NON-
 * const: menu_assetgroup_load/free take s16* and each TU owns its own array.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "types.h"
#include "menu.h" /* gRacerPortraits, TEXTURE_ICON_PORTRAIT_* */

/* Ten racers (== MDKR_ONLINE_CHARACTER_COUNT). Each screen keeps its own
 * *_CHAR_COUNT bound for its own indexing; this is the table dimension. */
#define MDKR_ONLINE_PORTRAIT_COUNT 10u

/* Online character id -> gRacerPortraits[] index. The screen is laid out in
 * ONLINE id order (grid cell index == online char id == the published
 * hover_character the reducer validates), so this maps that id to the portrait
 * slot to blit. */
static const u8 sOnlineToPortrait[MDKR_ONLINE_PORTRAIT_COUNT] = {
    1u, /* 0 Diddy     -> gRacerPortraits[1] */
    9u, /* 1 Timber    -> [9] */
    8u, /* 2 Pipsy     -> [8] */
    6u, /* 3 Tiptup    -> [6] */
    5u, /* 4 Conker    -> [5] */
    3u, /* 5 Bumper    -> [3] */
    4u, /* 6 Banjo     -> [4] */
    0u, /* 7 Krunch    -> [0] */
    2u, /* 8 Drumstick -> [2] */
    7u, /* 9 T.T.      -> [7] */
};

/* Short display names, in online id order (matches the launcher's kCharacters). */
static const char *const sOnlineNames[MDKR_ONLINE_PORTRAIT_COUNT] = {
    "DIDDY", "TIMBER", "PIPSY", "TIPTUP", "CONKER",
    "BUMPER", "BANJO", "KRUNCH", "DRUMSTICK", "T.T.",
};

/* The exact portrait texture ids (TEXTURE_ICON_PORTRAIT_KRUNCH .. _TIMBER, all
 * ASSET_MASK_TEXTURE assets) plus the -1 terminator menu_assetgroup_load stops
 * on. A portrait-only list: menu_asset_load routes every entry to load_texture,
 * so this loads the ten faces and spawns NO menu objects; menu_racer_portraits()
 * then binds them. */
static s16 sPortraitAssetIds[] = {
    TEXTURE_ICON_PORTRAIT_KRUNCH, TEXTURE_ICON_PORTRAIT_DIDDY,
    TEXTURE_ICON_PORTRAIT_DRUMSTICK, TEXTURE_ICON_PORTRAIT_BUMPER,
    TEXTURE_ICON_PORTRAIT_BANJO, TEXTURE_ICON_PORTRAIT_CONKER,
    TEXTURE_ICON_PORTRAIT_TIPTUP, TEXTURE_ICON_PORTRAIT_TT,
    TEXTURE_ICON_PORTRAIT_PIPSY, TEXTURE_ICON_PORTRAIT_TIMBER,
    -1,
};

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_PORTRAITS_H */
