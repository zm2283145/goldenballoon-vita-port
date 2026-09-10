// Native Vita trophy bridge. Other targets deliberately receive a no-op.
#ifndef MDKR64_VITA_TROPHY_H
#define MDKR64_VITA_TROPHY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Settings;

/* DKR's original EEPROM has no persisted completion state for Horseshoe
 * Gulch, and consequently cannot faithfully record a four-arena set.  The
 * native port owns these high, otherwise-unused cutscene bits.  They are
 * per-save-slot, survive a reload, and are deliberately ignored by original
 * cutscene/gameplay logic. */
#define MDKR_VITA_ARENA_HORSESHOE_COMPLETE UINT32_C(0x10000000)
#define MDKR_VITA_ARENA_DARKWATER_COMPLETE UINT32_C(0x20000000)
#define MDKR_VITA_ARENA_ICICLE_COMPLETE    UINT32_C(0x40000000)
#define MDKR_VITA_ARENA_SMOKEY_COMPLETE    UINT32_C(0x80000000)
#define MDKR_VITA_ARENA_COMPLETE_MASK      \
    (MDKR_VITA_ARENA_HORSESHOE_COMPLETE |  \
     MDKR_VITA_ARENA_DARKWATER_COMPLETE |  \
     MDKR_VITA_ARENA_ICICLE_COMPLETE |     \
     MDKR_VITA_ARENA_SMOKEY_COMPLETE)


// Reconciles the currently loaded save with the installed Vita trophy pack.
// It is safe to call once per game tick.
void mdkr_vita_trophy_pump(const struct Settings *settings);

// Starts the Vita trophy setup/registration flow once graphics are live. This
// is independent of save data and is intended for the title screen's Start.
void mdkr_vita_trophy_register(void);

// Exact gameplay seams for the optional trophy groups. These receive a level
// ID from the game, rather than an index from a menu, so every unlock remains
// tied to a completed race result.
void mdkr_vita_trophy_silver_coin_race(int levelId);
void mdkr_vita_trophy_tt_ghost_beaten(int levelId);
void mdkr_vita_trophy_developer_time(int levelId, int courseTime);
void mdkr_vita_trophy_banana_collected(int bananaCount);
void mdkr_vita_trophy_max_powerup(int balloonType, int balloonLevel);
void mdkr_vita_trophy_golden_balloon_collected(int characterId, int playerIndex);
void mdkr_vita_trophy_set_adventure_active(int active);

/* Save-editor support: these change and evaluate the same five-balloon
 * condition used by gameplay; they never target a trophy ID from the UI. */
int mdkr_vita_trophy_is_unlocked(unsigned trophy_id);
int mdkr_vita_trophy_character_balloon_progress(unsigned character_index);
void mdkr_vita_trophy_set_character_balloon_progress(unsigned character_index,
                                                      unsigned balloon_count);

/* The Time Trial editor stores genuine course records. These helpers expose
 * the exact developer-time threshold used by the trophy condition without
 * duplicating that conversion in the menu. */
int mdkr_vita_trophy_developer_time_target(unsigned track_index);
int mdkr_vita_trophy_developer_time_beaten(unsigned track_index,
                                           int course_time);
/* Maps a game level ID to the canonical RetroAchievements/credits time-trial
 * order. The save editor presents tracks in world order, which differs for
 * Sherbet and Snowflake worlds, so it must not use its menu index here. */
int mdkr_vita_trophy_track_index(int level_id);

#ifdef __cplusplus
}
#endif

#endif
