#ifndef MDKR_ONLINE_CHARACTER_MAP_H
#define MDKR_ONLINE_CHARACTER_MAP_H

/* The ONE online-catalog-id -> engine Character-enum mapping.
 *
 * The online lobby/grid numbers racers in its OWN "online catalog" order (grid
 * cell index == the published hover_character the reducer validates == the
 * launcher's kCharacters strip):
 *   0 Diddy 1 Timber 2 Pipsy 3 Tiptup 4 Conker 5 Bumper 6 Banjo 7 Krunch
 *   8 Drumstick 9 T.T.
 * The engine Character enum (game/include/enums.h) is a DIFFERENT order:
 *   0 KRUNCH 1 BUMPER 2 TIPTUP 3 CONKER 4 TIMBER 5 BANJO 6 DRUMSTICK 7 PIPSY
 *   8 TT 9 DIDDY
 * so an online id must be TRANSLATED before it indexes any Character-enum table
 * or is assigned to a racer. Skipping the translation used the raw online id as
 * the engine character -- Tiptup(online 3) spawned CONKER(engine 3), T.T.
 * (online 9) spawned DIDDY(engine 9) -- the wrong-characters playtest defect.
 *
 * This macro is the single source of truth for that order. online_portraits.h
 * initialises its portrait/name remap from it, and the direct-boot racer spawn
 * (menu.c get_character_id_from_slot) maps through mdkr_online_character_to_engine()
 * below, so a future roster re-order can never desync the two. Entries are given
 * as Character-enum symbols, in online-catalog id order. */
#include "enums.h" /* Character enum (CHARACTER_*) */

#define MDKR_ONLINE_CHARACTER_ENGINE_ORDER                                   \
    CHARACTER_DIDDY,     /* 0 Diddy */                                        \
    CHARACTER_TIMBER,    /* 1 Timber */                                       \
    CHARACTER_PIPSY,     /* 2 Pipsy */                                        \
    CHARACTER_TIPTUP,    /* 3 Tiptup */                                       \
    CHARACTER_CONKER,    /* 4 Conker */                                       \
    CHARACTER_BUMPER,    /* 5 Bumper */                                       \
    CHARACTER_BANJO,     /* 6 Banjo */                                        \
    CHARACTER_KRUNCH,    /* 7 Krunch */                                       \
    CHARACTER_DRUMSTICK, /* 8 Drumstick */                                    \
    CHARACTER_TT         /* 9 T.T. */

/* Ten base racers == MDKR_ONLINE_CHARACTER_COUNT (lobby_core.h). */
#define MDKR_ONLINE_CHARACTER_MAP_COUNT 10

#ifdef __cplusplus
extern "C" {
#endif

/* Translate an online-catalog id to an engine Character-enum value. An id
 * outside [0, MDKR_ONLINE_CHARACTER_MAP_COUNT) is returned unchanged (defensive;
 * the reducer already bounds hover_character < 10 and the launch descriptor
 * validate rejects character_id >= MDKR_MATCH_CHARACTER_COUNT). */
int mdkr_online_character_to_engine(int online_id);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ONLINE_CHARACTER_MAP_H */
