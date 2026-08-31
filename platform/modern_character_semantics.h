/* One authoritative catalog for exact Workshop semantic pose inspection.
 * Consumers supply X(enum_suffix, runtime_string, player_label); keeping the
 * enum, UI, launcher handoff, game parser, and runtime validator generated from
 * this list makes semantic drift a compile-time problem. Order is part of the
 * versioned draft/result contract: append new entries, never reorder them. */
#ifndef MDKR64_MODERN_CHARACTER_SEMANTICS_H
#define MDKR64_MODERN_CHARACTER_SEMANTICS_H

#define MDKR_MODERN_CHARACTER_INSPECTION_SEMANTICS(X)                    \
    X(SELECT_IDLE, "select.idle", "Select idle")                        \
    X(SELECT_HOVER, "select.hover", "Select hover")                     \
    X(SELECT_CONFIRM, "select.confirm", "Select confirm")              \
    X(RACE_STEER, "race.steer", "Race steer")                          \
    X(RACE_REVERSE, "race.reverse", "Race reverse")                    \
    X(RACE_BOOST, "race.boost", "Race boost")                          \
    X(RACE_DAMAGE, "race.damage", "Race damage")                       \
    X(RACE_ITEM, "race.item", "Race item")                             \
    X(RACE_SPIN, "race.spin", "Race spin")                             \
    X(RACE_AIRBORNE, "race.airborne", "Race airborne")                \
    X(RACE_LAND, "race.land", "Race land")                            \
    X(RACE_FINISH_WIN, "race.finish_win", "Race finish win")          \
    X(RACE_FINISH_LOSE, "race.finish_lose", "Race finish lose")

#define MDKR_MODERN_CHARACTER_INSPECTION_SEMANTIC_COUNT 13u
#define MDKR_MODERN_CHARACTER_INSPECTION_TRANSITION_DWELL_MILLI 1000u
/* Enum value after LIVE; race.steer's midpoint is the most useful neutral
 * vehicle-fit default while remaining valid in character select. */
#define MDKR_MODERN_CHARACTER_INSPECTION_DEFAULT_POSE 4u

#endif /* MDKR64_MODERN_CHARACTER_SEMANTICS_H */
