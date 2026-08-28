/* adventure_party_spawn.c — see adventure_party_spawn.h.
 *
 * Implementation notes, in the order a reader will wonder about them:
 *
 *   - Validate everything before writing *out. A refusal returns a typed error
 *     and never touches the caller's plan, exactly like the state reducer.
 *   - Additional seats fill outward in mirror pairs: seat 1 to the right, seat
 *     2 the same distance left, seat 3 one rank further right. `rank` and
 *     `sign` encode that, so seats 1 and 2 (and any future pair) are exact
 *     reflections across the heading axis — the symmetry the plan asserts.
 *   - Per seat the order is fixed: [0] side-by-side, [1] staggered rear, then
 *     fallback rings. Every candidate sits at a STRICTLY larger distance from
 *     the setup point than the one before it (side < rear < ring1 < ring2 ...),
 *     so "no two candidates coincide" is structural, not a coincidence of the
 *     chosen spacing.
 *   - The only libm used is sinf/cosf; their per-platform variance is the one
 *     the header says the design accepts. No statics, no RNG: same params in,
 *     byte-identical plan out.
 */
#include "adventure_party/adventure_party_spawn.h"

#include <math.h>
#include <string.h>

#ifndef ADVENTURE_PARTY_PI
#define ADVENTURE_PARTY_PI 3.14159265358979323846f
#endif

/* Angular fan applied per fallback ring so successive rings splay to the
 * seat's side instead of stacking on one bearing. Distinctness does not depend
 * on it (radius already grows), but it keeps the rings visibly a formation. */
#define ADVENTURE_PARTY_RING_FAN (ADVENTURE_PARTY_PI / 6.0f)

AdventurePartyFormationResult adventure_party_plan_formation(
    const AdventurePartyFormationParams *params,
    AdventurePartyFormationPlan *out) {
    float h, fx, fz, rx, rz;
    int extra, cap, j, i;

    if (!params || !out)
        return ADVENTURE_PARTY_FORMATION_ERR_ARGUMENT;
    if (params->participant_count < ADVENTURE_PARTY_MIN_PARTICIPANTS ||
        params->participant_count > ADVENTURE_PARTY_MAX_PARTICIPANTS)
        return ADVENTURE_PARTY_FORMATION_ERR_COUNT;
    /* `!(x > 0)` also rejects NaN, not just zero/negative. */
    if (!(params->side_gap > 0.0f) || !(params->rear_stagger > 0.0f) ||
        !(params->ring_step > 0.0f))
        return ADVENTURE_PARTY_FORMATION_ERR_SPACING;
    if (params->max_candidates_per_seat < 1 ||
        params->max_candidates_per_seat > ADVENTURE_PARTY_MAX_CANDIDATES)
        return ADVENTURE_PARTY_FORMATION_ERR_CAPACITY;

    memset(out, 0, sizeof *out);

    h = params->heading;
    fx = sinf(h); fz = cosf(h);   /* forward: +z at heading 0 */
    rx = cosf(h); rz = -sinf(h);  /* right:   +x at heading 0 */

    extra = (int)params->participant_count - 1;
    cap = (int)params->max_candidates_per_seat;
    out->seat_count = (uint8_t)extra;

    for (j = 0; j < extra; j++) {
        AdventurePartySeatPlan *seat = &out->seats[j];
        int rank = j / 2 + 1;
        float sign = (j % 2 == 0) ? 1.0f : -1.0f;
        float lateral = sign * (float)rank * params->side_gap;
        float rear_radius = sqrtf(lateral * lateral +
                                  params->rear_stagger * params->rear_stagger);

        seat->seat = (uint8_t)(j + 1);
        seat->candidate_count = (uint8_t)cap;

        for (i = 0; i < cap; i++) {
            AdventurePartyFormationCandidate *c = &seat->candidates[i];
            float px, pz;
            if (i == 0) {
                /* Abreast of player one along the right axis. */
                px = params->setup.x + rx * lateral;
                pz = params->setup.z + rz * lateral;
                c->tier = ADVENTURE_PARTY_FORMATION_TIER_SIDE;
            } else if (i == 1) {
                /* The side slot pushed back one stagger along -forward. */
                px = params->setup.x + rx * lateral - fx * params->rear_stagger;
                pz = params->setup.z + rz * lateral - fz * params->rear_stagger;
                c->tier = ADVENTURE_PARTY_FORMATION_TIER_REAR;
            } else {
                /* Expanding fallback rings behind the party. Radius grows past
                 * the rear line every step, so these never collide with the
                 * side/rear slots or one another. */
                int t = i - 2;
                float radius =
                    rear_radius + params->ring_step * (float)(t + 1);
                float angle = h + ADVENTURE_PARTY_PI +
                              sign * ADVENTURE_PARTY_RING_FAN * (float)(t + 1);
                px = params->setup.x + sinf(angle) * radius;
                pz = params->setup.z + cosf(angle) * radius;
                c->tier = ADVENTURE_PARTY_FORMATION_TIER_RING;
            }
            c->point.x = px;
            c->point.y = params->setup.y; /* grounding is the adapter's job */
            c->point.z = pz;
        }
    }
    return ADVENTURE_PARTY_FORMATION_OK;
}

const char *adventure_party_formation_result_name(
    AdventurePartyFormationResult result) {
    switch (result) {
    case ADVENTURE_PARTY_FORMATION_OK:           return "OK";
    case ADVENTURE_PARTY_FORMATION_ERR_ARGUMENT: return "ERR_ARGUMENT";
    case ADVENTURE_PARTY_FORMATION_ERR_COUNT:    return "ERR_COUNT";
    case ADVENTURE_PARTY_FORMATION_ERR_SPACING:  return "ERR_SPACING";
    case ADVENTURE_PARTY_FORMATION_ERR_CAPACITY: return "ERR_CAPACITY";
    default:                                     return "INVALID";
    }
}

const char *adventure_party_formation_tier_name(
    AdventurePartyFormationTier tier) {
    switch (tier) {
    case ADVENTURE_PARTY_FORMATION_TIER_SIDE: return "SIDE";
    case ADVENTURE_PARTY_FORMATION_TIER_REAR: return "REAR";
    case ADVENTURE_PARTY_FORMATION_TIER_RING: return "RING";
    default:                                  return "INVALID";
    }
}
