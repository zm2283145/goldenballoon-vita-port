/* test_adventure_party_spawn.c — AP-07 formation planner invariants.
 *
 * ROM-free: links only platform/adventure_party/adventure_party_spawn.c. The
 * planner is pure geometry, so every property the plan names — symmetry,
 * deterministic ordering, heading rotation invariance, bounded fallback, no
 * duplicate candidates, count coverage, and typed rejection of degenerate
 * params — is decidable here with no game, no ROM, and no window. */
#include "adventure_party/adventure_party_spawn.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
static void expect(int c, const char *w) {
    if (!c) { printf("FAIL %s\n", w); failures++; } else printf("ok   %s\n", w);
}

/* --- small vector helpers (test-side, so the planner owns no math API) --- */

typedef struct V3 { float x, y, z; } V3;

static V3 pt(AdventurePartySpawnPoint p) { V3 v = { p.x, p.y, p.z }; return v; }

static float dist_xz(V3 a, V3 b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}

/* Rotate delta (P-setup) about setup by A, consistent with the planner's
 * forward=(sin h, cos h): heading h -> h+A must move a point this way. */
static V3 rotate_about(V3 p, V3 setup, float a) {
    float dx = p.x - setup.x, dz = p.z - setup.z;
    float ca = cosf(a), sa = sinf(a);
    V3 out;
    out.x = setup.x + (dx * ca + dz * sa);
    out.y = p.y;
    out.z = setup.z + (-dx * sa + dz * ca);
    return out;
}

/* Reflect P across the heading axis through setup (negate the right-component,
 * keep the forward-component) — the mirror the symmetry property asserts. */
static V3 mirror_across_heading(V3 p, V3 setup, float h) {
    float fx = sinf(h), fz = cosf(h);   /* forward */
    float rx = cosf(h), rz = -sinf(h);  /* right */
    float dx = p.x - setup.x, dz = p.z - setup.z;
    float f = dx * fx + dz * fz;        /* forward-component */
    float r = dx * rx + dz * rz;        /* right-component */
    V3 out;
    out.x = setup.x + f * fx - r * rx;
    out.y = p.y;
    out.z = setup.z + f * fz - r * rz;
    return out;
}

static const float EPS = 1e-2f;
static int near_pt(V3 a, V3 b) {
    return fabsf(a.x - b.x) < EPS && fabsf(a.y - b.y) < EPS &&
           fabsf(a.z - b.z) < EPS;
}

/* A valid baseline: setup off the origin, a non-axis heading, distinct spacing
 * and a candidate budget deep enough to reach the ring tier. */
static AdventurePartyFormationParams base_params(int count, int max_cand) {
    AdventurePartyFormationParams p;
    memset(&p, 0, sizeof p);
    p.setup.x = 100.0f;
    p.setup.y = 5.0f;
    p.setup.z = 200.0f;
    p.heading = 0.5f;
    p.participant_count = (uint8_t)count;
    p.side_gap = 3.0f;
    p.rear_stagger = 2.0f;
    p.ring_step = 4.0f;
    p.max_candidates_per_seat = (uint8_t)max_cand;
    return p;
}

/* --- tests ------------------------------------------------------------- */

static void test_count_coverage(void) {
    for (int count = 2; count <= 4; count++) {
        AdventurePartyFormationParams p = base_params(count, 6);
        AdventurePartyFormationPlan plan;
        AdventurePartyFormationResult r =
            adventure_party_plan_formation(&p, &plan);
        expect(r == ADVENTURE_PARTY_FORMATION_OK, "count: OK");
        expect(plan.seat_count == count - 1, "count: seat_count == N-1");
        for (int j = 0; j < plan.seat_count; j++) {
            expect(plan.seats[j].seat == (uint8_t)(j + 1),
                   "count: seat number is dense 1..N-1");
            expect(plan.seats[j].candidate_count == 6,
                   "count: candidate list filled to the cap");
        }
    }
}

static void test_ordering_tiers(void) {
    AdventurePartyFormationParams p = base_params(4, 6);
    AdventurePartyFormationPlan plan;
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_OK, "tiers: OK");
    for (int j = 0; j < plan.seat_count; j++) {
        const AdventurePartySeatPlan *s = &plan.seats[j];
        int rings = 0;
        expect(s->candidates[0].tier == ADVENTURE_PARTY_FORMATION_TIER_SIDE,
               "tiers: first candidate is side-by-side");
        expect(s->candidates[1].tier == ADVENTURE_PARTY_FORMATION_TIER_REAR,
               "tiers: second candidate is staggered rear");
        for (int i = 2; i < s->candidate_count; i++) {
            if (s->candidates[i].tier == ADVENTURE_PARTY_FORMATION_TIER_RING)
                rings++;
        }
        expect(rings == s->candidate_count - 2,
               "tiers: remaining candidates are fallback rings");
    }
}

static void test_symmetry(void) {
    /* N=3: seats 1 and 2 share rank 1 on opposite sides, so their entire
     * candidate lists are mirror images across the heading axis. */
    AdventurePartyFormationParams p = base_params(3, 6);
    AdventurePartyFormationPlan plan;
    V3 setup = { p.setup.x, p.setup.y, p.setup.z };
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_OK, "symmetry: OK");
    expect(plan.seat_count == 2, "symmetry: two additional seats");
    for (int i = 0; i < plan.seats[0].candidate_count; i++) {
        V3 a = pt(plan.seats[0].candidates[i].point);
        V3 b = pt(plan.seats[1].candidates[i].point);
        V3 m = mirror_across_heading(a, setup, p.heading);
        expect(near_pt(m, b), "symmetry: seat 2 mirrors seat 1 across heading");
        expect(plan.seats[0].candidates[i].tier ==
                   plan.seats[1].candidates[i].tier,
               "symmetry: mirrored candidates share a tier");
    }
}

static void test_deterministic(void) {
    AdventurePartyFormationParams p = base_params(4, 7);
    AdventurePartyFormationPlan a, b;
    expect(adventure_party_plan_formation(&p, &a) ==
               ADVENTURE_PARTY_FORMATION_OK, "determinism: first OK");
    expect(adventure_party_plan_formation(&p, &b) ==
               ADVENTURE_PARTY_FORMATION_OK, "determinism: second OK");
    expect(memcmp(&a, &b, sizeof a) == 0,
           "determinism: two calls are byte-identical");
}

static void test_rotation_invariance(void) {
    const float angles[] = { 0.7f, 2.0f, -1.3f };
    AdventurePartyFormationParams base = base_params(4, 6);
    AdventurePartyFormationPlan p0;
    V3 setup = { base.setup.x, base.setup.y, base.setup.z };
    expect(adventure_party_plan_formation(&base, &p0) ==
               ADVENTURE_PARTY_FORMATION_OK, "rotation: base OK");
    for (size_t k = 0; k < sizeof angles / sizeof angles[0]; k++) {
        AdventurePartyFormationParams pr = base;
        AdventurePartyFormationPlan plan;
        pr.heading = base.heading + angles[k];
        expect(adventure_party_plan_formation(&pr, &plan) ==
                   ADVENTURE_PARTY_FORMATION_OK, "rotation: rotated OK");
        for (int j = 0; j < plan.seat_count; j++) {
            for (int i = 0; i < plan.seats[j].candidate_count; i++) {
                V3 orig = pt(p0.seats[j].candidates[i].point);
                V3 want = rotate_about(orig, setup, angles[k]);
                V3 got = pt(plan.seats[j].candidates[i].point);
                expect(near_pt(want, got),
                       "rotation: candidate rotates rigidly about setup");
            }
        }
    }
}

static void test_bounded_and_unique(void) {
    for (int max_cand = 1; max_cand <= ADVENTURE_PARTY_MAX_CANDIDATES;
         max_cand++) {
        AdventurePartyFormationParams p = base_params(4, max_cand);
        AdventurePartyFormationPlan plan;
        expect(adventure_party_plan_formation(&p, &plan) ==
                   ADVENTURE_PARTY_FORMATION_OK, "bounded: OK");
        for (int j = 0; j < plan.seat_count; j++) {
            const AdventurePartySeatPlan *s = &plan.seats[j];
            expect(s->candidate_count == max_cand,
                   "bounded: count equals configured cap");
            for (int a = 0; a < s->candidate_count; a++) {
                for (int b = a + 1; b < s->candidate_count; b++) {
                    float d = dist_xz(pt(s->candidates[a].point),
                                      pt(s->candidates[b].point));
                    expect(d > 1e-3f, "unique: no duplicate candidate in a seat");
                }
            }
        }
    }
}

static void test_degenerate_rejected(void) {
    AdventurePartyFormationPlan plan, sentinel;
    AdventurePartyFormationParams p;

    /* NULL arguments. */
    expect(adventure_party_plan_formation(NULL, &plan) ==
               ADVENTURE_PARTY_FORMATION_ERR_ARGUMENT, "reject: NULL params");
    p = base_params(3, 4);
    expect(adventure_party_plan_formation(&p, NULL) ==
               ADVENTURE_PARTY_FORMATION_ERR_ARGUMENT, "reject: NULL out");

    /* Participant count out of range (0..5 covered by 1 and 5). */
    p = base_params(1, 4);
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_ERR_COUNT, "reject: count 1");
    p = base_params(5, 4);
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_ERR_COUNT, "reject: count 5");

    /* Non-positive spacing, each parameter in turn. */
    p = base_params(3, 4); p.side_gap = 0.0f;
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_ERR_SPACING, "reject: zero side_gap");
    p = base_params(3, 4); p.rear_stagger = -1.0f;
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_ERR_SPACING, "reject: negative rear_stagger");
    p = base_params(3, 4); p.ring_step = 0.0f;
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_ERR_SPACING, "reject: zero ring_step");

    /* Candidate cap out of range. */
    p = base_params(3, 0);
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_ERR_CAPACITY, "reject: zero cap");
    p = base_params(3, ADVENTURE_PARTY_MAX_CANDIDATES + 1);
    expect(adventure_party_plan_formation(&p, &plan) ==
               ADVENTURE_PARTY_FORMATION_ERR_CAPACITY, "reject: cap over ceiling");

    /* A refusal must not touch *out. */
    memset(&sentinel, 0xAB, sizeof sentinel);
    plan = sentinel;
    p = base_params(9, 4);
    (void)adventure_party_plan_formation(&p, &plan);
    expect(memcmp(&plan, &sentinel, sizeof plan) == 0,
           "reject: out is left untouched on error");
}

int main(void) {
    test_count_coverage();
    test_ordering_tiers();
    test_symmetry();
    test_deterministic();
    test_rotation_invariance();
    test_bounded_and_unique();
    test_degenerate_rejected();

    /* Name helpers are non-NULL for traces/diagnostics. */
    expect(adventure_party_formation_result_name(ADVENTURE_PARTY_FORMATION_OK) !=
               NULL, "names: result name non-NULL");
    expect(adventure_party_formation_tier_name(
               ADVENTURE_PARTY_FORMATION_TIER_SIDE) != NULL,
           "names: tier name non-NULL");

    if (failures) {
        printf("\n%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("\nall formation planner checks passed\n");
    return 0;
}
