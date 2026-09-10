// Native Vita trophy bridge. Other targets deliberately receive a no-op.
#ifndef MDKR64_VITA_TROPHY_H
#define MDKR64_VITA_TROPHY_H

struct Settings;

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
void mdkr_vita_trophy_golden_balloon_collected(int characterId);
void mdkr_vita_trophy_set_adventure_active(int active);

#endif
