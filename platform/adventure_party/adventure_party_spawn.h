/* adventure_party_spawn.h — Adventure Party formation planner (AP-07, pure).
 *
 * The deterministic half of formation spawning: given player one's authored
 * setup point and heading, produce an ORDERED list of candidate positions for
 * each additional seat. This module decides WHERE the game might place a racer;
 * it never places one. It holds no Object/Settings pointer, runs no collision
 * query, projects nothing to ground, and keeps no state — value struct in,
 * value struct out, same purity contract as state.h and policy.h.
 *
 * The collision-validating GAME ADAPTER (project to ground; reject geometry,
 * water, object overlap, invalid nav) is deliberately NOT here — it lands with
 * Wave C hub integration (see docs/architecture/adventure-party.md,
 * "Transactional roster and formation spawning"). Keeping the planner pure is
 * what makes symmetry, determinism, and rotation invariance unit-testable with
 * no ROM.
 *
 * Coordinate + angle convention (stated once, relied on throughout):
 *   - Positions are game world space: x/z is the ground plane, y is up. A
 *     candidate keeps player one's y; grounding is the adapter's job.
 *   - `heading` is in RADIANS. Forward is (sin h, 0, cos h) and right is
 *     (cos h, 0, -sin h), so at heading 0 forward is +z and right is +x. The
 *     adapter converts the game's authored heading units into radians before
 *     calling; picking radians here means the only libm this module needs is
 *     sinf/cosf, whose results may vary across platforms and are the one such
 *     variance the design accepts.
 *
 * Ordering, per additional seat, is always: the side-by-side slot first, then
 * the staggered-rear slot, then bounded fallback rings — most-preferred first,
 * so the adapter takes the first candidate that survives its collision checks.
 * Player one is NOT in the output: it always uses the authored setup point.
 */
#ifndef MDKR64_ADVENTURE_PARTY_SPAWN_H
#define MDKR64_ADVENTURE_PARTY_SPAWN_H

#include <stdint.h>

#include "adventure_party/adventure_party_state.h" /* MAX_SEATS / participant bounds */

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time ceiling on candidates per seat; the array is sized to it and
 * max_candidates_per_seat must not exceed it. A defensive bound, not a target:
 * a handful of fallbacks is plenty before the activity is declared unspawnable. */
#define ADVENTURE_PARTY_MAX_CANDIDATES 8

/* Additional seats a plan can hold: everyone except player one. */
#define ADVENTURE_PARTY_MAX_EXTRA_SEATS (ADVENTURE_PARTY_MAX_SEATS - 1)

/* Which tier produced a candidate — the ordering above, made inspectable so a
 * test can assert side-by-side comes before rear comes before ring. */
typedef enum AdventurePartyFormationTier {
    ADVENTURE_PARTY_FORMATION_TIER_SIDE = 0, /* abreast of player one */
    ADVENTURE_PARTY_FORMATION_TIER_REAR = 1, /* staggered behind the side slot */
    ADVENTURE_PARTY_FORMATION_TIER_RING = 2  /* expanding fallback rings */
} AdventurePartyFormationTier;

typedef enum AdventurePartyFormationResult {
    ADVENTURE_PARTY_FORMATION_OK = 0,
    ADVENTURE_PARTY_FORMATION_ERR_ARGUMENT = -1, /* NULL params/out */
    ADVENTURE_PARTY_FORMATION_ERR_COUNT = -2,    /* participant_count outside 2..4 */
    ADVENTURE_PARTY_FORMATION_ERR_SPACING = -3,  /* a gap/stagger/step not > 0 */
    ADVENTURE_PARTY_FORMATION_ERR_CAPACITY = -4  /* max_candidates outside 1..CAP */
} AdventurePartyFormationResult;

/* A world-space point. y is carried through from the authored setup point. */
typedef struct AdventurePartySpawnPoint {
    float x;
    float y;
    float z;
} AdventurePartySpawnPoint;

/* The authored inputs, as a value. spacing parameters are distances in world
 * units; all must be strictly positive (a zero/negative gap is a degenerate
 * formation and is refused, not clamped). */
typedef struct AdventurePartyFormationParams {
    AdventurePartySpawnPoint setup;   /* player one's authored point */
    float heading;                    /* radians; see convention above */
    uint8_t participant_count;        /* 2..4 (includes player one) */
    float side_gap;                   /* lateral spacing between abreast slots */
    float rear_stagger;               /* distance a rear slot sits behind forward */
    float ring_step;                  /* radius added per fallback ring */
    uint8_t max_candidates_per_seat;  /* 1..ADVENTURE_PARTY_MAX_CANDIDATES */
} AdventurePartyFormationParams;

typedef struct AdventurePartyFormationCandidate {
    AdventurePartySpawnPoint point;
    uint8_t tier;                     /* AdventurePartyFormationTier */
} AdventurePartyFormationCandidate;

/* One additional seat's ordered candidate list. */
typedef struct AdventurePartySeatPlan {
    uint8_t seat;                     /* 1..participant_count-1 */
    uint8_t candidate_count;          /* == params.max_candidates_per_seat */
    AdventurePartyFormationCandidate
        candidates[ADVENTURE_PARTY_MAX_CANDIDATES];
} AdventurePartySeatPlan;

/* The whole plan for the additional seats (player one excluded). */
typedef struct AdventurePartyFormationPlan {
    uint8_t seat_count;               /* participant_count - 1 */
    AdventurePartySeatPlan seats[ADVENTURE_PARTY_MAX_EXTRA_SEATS];
} AdventurePartyFormationPlan;

/*
 * Builds the ordered candidate plan for every additional seat, or refuses with
 * a typed error and leaves *out untouched. Deterministic: the same params
 * produce byte-identical output every call (no statics, no RNG). Rotating
 * params.heading by A rotates every candidate rigidly by A about the setup
 * point. Every seat's list is filled to exactly max_candidates_per_seat, and
 * no two candidates within a seat coincide.
 */
AdventurePartyFormationResult adventure_party_plan_formation(
    const AdventurePartyFormationParams *params,
    AdventurePartyFormationPlan *out);

/* Diagnostic names for traces and test output; never NULL. */
const char *adventure_party_formation_result_name(
    AdventurePartyFormationResult result);
const char *adventure_party_formation_tier_name(
    AdventurePartyFormationTier tier);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ADVENTURE_PARTY_SPAWN_H */
