/*
 * Void-curtain pair-walker unit hook (issue #53 investigation fallout).
 *
 * White-box by design: this target links the PRODUCTION game/src/tracks.c
 * translation unit (dead-stripped down to the void subsystem) so the walker
 * under test is the shipped one, not a copy. It exercises the regime the
 * port's raised caps newly allow (D_8011D4BA 175 -> 351 entries) and pins two
 * latent defects found during the issue-53 Walrus Cove investigation:
 *
 *  1. func_80026E54 narrowed pair ids through s8 locals. The 1.4.0 widening
 *     (6f7a081) covered unk7, the walker's sp7C list and the list parameter,
 *     but not the walker's own `temp`/`swapByte` locals: with a pair id
 *     > 127 (needs > 256 entries in one tick, reachable only above the
 *     retail cap), `temp = arg1[i]` wrapped negative and indexed
 *     D_8011D47C[-256] -- an out-of-bounds read ASan turns red in case A1 --
 *     and the bubble sort's swapByte wrote the wrapped id back into the
 *     caller's open list (case A2).
 *
 *  2. Exact entry-table saturation orphans a half-pair: func_80026C14 drops
 *     only the overflowing push, so the accepted first-of-pair leaves its
 *     partner slot at -1 (or stale) and the walker then dereferences
 *     D_8011D478[-1] (case B1) or a wild in-range entry (case B2). Today
 *     void_check's own `D_8011D49E >= D_8011D4BA` bail keeps this
 *     unreachable; the walker-side skip is degrade-don't-corrupt armour so
 *     no future cap or gate change can turn saturation into corruption.
 *
 * The tables are allocated here as two separate heap blocks with exactly the
 * capacities void_init carves out of its single pool, so ASan redzones sit
 * where the pool would silently absorb an underflow. Below 128 pairs the
 * widened walker is bit-identical to the shipped one (render_purity arms
 * pin that on the full renderer, as they did for 6f7a081).
 *
 * Run with no arguments for every case (the ctest registration), or with a
 * case name (A1, A2, B1, B2) to observe one red at a time under ASan.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tracks.h"

/* File-scope state of game/src/tracks.c consumed by the functions under
 * test. tracks.c publishes these without header declarations; the types
 * mirror the definitions at tracks.c's head. */
extern unk8011D478 *D_8011D478;
extern VoidPairIndex *D_8011D47C;
extern s16 D_8011D49C;
extern s16 D_8011D49E;
extern s16 D_8011D4BA;
extern s16 gVoidVertCount;
extern s16 gVoidPrimCount;
extern s16 gVoidPrimLimit;
extern Vertex *gTrackVtxPtr;
extern Triangle *gTrackTriPtr;
extern Vertex *gVoidCurrVerts;
extern Triangle *gVoidCurrTris;
extern Gfx *gTrackDL;
extern f32 D_8011D4A0;
extern f32 D_8011D4A4;
extern f32 D_8011D4AC;
extern f32 D_8011D4B0;
extern u8 gVoidColourR;
extern u8 gVoidColourG;
extern u8 gVoidColourB;

/* Link stubs for the three renderer symbols on the walker's live path
 * (void_generate_primitive's flush). Everything else tracks.c references is
 * dead-stripped -- see the target's -dead_strip/--gc-sections link flags. */
u32 dkr_k0_to_physical(const void *x) {
    return (u32) (uintptr_t) x;
}

void dkr_dl_register_host_ptr(const void *x) {
    (void) x;
}

bool gfx_shadow_caster_exclude_mark(const void *source) {
    (void) source;
    return false;
}

static int failures;

#define EXPECT(cond, message)                                          \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__,   \
                    __LINE__);                                         \
            failures++;                                                \
        }                                                              \
    } while (0)

/* void_init's own capacities (NATIVE_PORT): 351 entries + 6 spare,
 * 351 + 5 pair-index slots. Allocated as separate blocks so a negative or
 * past-the-end index lands in an ASan redzone instead of elsewhere in the
 * shared pool. */
#define TEST_ENTRY_CAP 351
#define TEST_ENTRY_SLOTS (TEST_ENTRY_CAP + 6)
#define TEST_PAIR_SLOTS (TEST_ENTRY_CAP + 5)

static unk8011D478 *entry_block;
static VoidPairIndex *pair_block;
static Vertex vert_buffer[64];
static Triangle tri_buffer[64];
static Gfx dl_buffer[64];

static void harness_reset(void) {
    free(entry_block);
    free(pair_block);
    entry_block = calloc(TEST_ENTRY_SLOTS, sizeof(unk8011D478));
    pair_block = calloc(TEST_PAIR_SLOTS, sizeof(VoidPairIndex));
    if (entry_block == NULL || pair_block == NULL) {
        fprintf(stderr, "FAIL: harness allocation\n");
        exit(1);
    }
    D_8011D478 = entry_block;
    D_8011D47C = pair_block;
    D_8011D4BA = TEST_ENTRY_CAP;
    D_8011D49C = 0;
    D_8011D49E = 0;

    memset(vert_buffer, 0, sizeof(vert_buffer));
    memset(tri_buffer, 0, sizeof(tri_buffer));
    memset(dl_buffer, 0, sizeof(dl_buffer));
    gTrackVtxPtr = vert_buffer;
    gTrackTriPtr = tri_buffer;
    gVoidCurrVerts = vert_buffer;
    gVoidCurrTris = tri_buffer;
    gTrackDL = dl_buffer;
    gVoidVertCount = 0;
    gVoidPrimCount = 0;
    gVoidPrimLimit = 180;

    /* Camera-plane basis: x = -t, z = 0 -- primitive x positions become the
     * negated strip coordinate, which keeps expectations hand-checkable. */
    D_8011D4A0 = -1.0f;
    D_8011D4A4 = 0.0f;
    D_8011D4AC = 0.0f;
    D_8011D4B0 = 0.0f;

    /* Walrus Cove's voidColour, the byte-exact blue from issue #53. */
    gVoidColourR = 0;
    gVoidColourG = 83;
    gVoidColourB = 133;
}

/* One void edge = two crossings pushed through the production inserter,
 * exactly like func_80026430/func_80026070 do. A level span here is a
 * horizontal line from (x1, y) to (x2, y) so interpolated band heights stay
 * equal to y at every strip coordinate. */
static void push_flat_pair(s16 x1, s16 x2, s16 y, s32 isFloor) {
    func_80026C14(x1, y, isFloor);
    func_80026C14(x2, y, isFloor);
}

/* Mirror of void_check's pair-slot fill loop (tracks.c, after the bubble
 * sort): first table-order sighting of a pair id claims the even slot and
 * the first-of-pair mark; the partner takes the odd slot. */
static void fill_pair_slots(void) {
    s16 i;
    s16 j;

    for (i = 0; i < D_8011D49E; i++) {
        j = D_8011D478[i].unk7 * 2;
        if (D_8011D47C[j] == -1) {
            D_8011D478[i].unk6 |= 2;
            D_8011D47C[j] = i;
        } else {
            D_8011D47C[j + 1] = i;
        }
    }
}

/* Build the >128-pair table both A cases share: pair ids 0..127 are filler
 * edges far to the left of the strip under test; id 128 is a floor span at
 * y=100 and id 129 a ceiling span at y=400, both covering x 100..300. */
static void build_wide_table(void) {
    s16 p;

    harness_reset();
    for (p = 0; p < 128; p++) {
        push_flat_pair((s16) (-2000 + p * 4), (s16) (-2000 + p * 4 + 2),
                       (s16) (-500 + p), 1);
    }
    push_flat_pair(100, 300, 100, 1); /* pair id 128, floor */
    push_flat_pair(100, 300, 400, 0); /* pair id 129, ceiling */
    EXPECT(D_8011D49E == 260, "wide table holds 260 entries");
    EXPECT(D_8011D49C == 130, "wide table holds 130 pairs");
    fill_pair_slots();
}

/* Case A1: a pair id past 127 must survive the walker's locals. Before the
 * widening, `temp = arg1[0]` wraps 128 to -128 and the very first lookup
 * reads D_8011D47C[-256] -- ASan heap-buffer-underflow (RED). After it, the
 * floor/ceiling pair emits one hand-checkable primitive (GREEN). */
static void case_a1_walker_locals_wide(void) {
    VoidPairIndex open_list[2] = { 128, 129 };

    build_wide_table();
    func_80026E54(2, open_list, 250.0f, 150.0f);

    EXPECT(gVoidPrimCount == 1, "A1: exactly one curtain primitive");
    EXPECT(gVoidVertCount == 4, "A1: exactly four vertices");
    EXPECT(vert_buffer[0].y == 102 && vert_buffer[1].y == 102,
           "A1: floor band at y=100 plus the +2 bias");
    EXPECT(vert_buffer[2].y == 398 && vert_buffer[3].y == 398,
           "A1: ceiling band at y=400 minus the -2 bias");
    EXPECT(vert_buffer[0].x == -150 && vert_buffer[1].x == -250,
           "A1: strip endpoints projected through the void plane basis");
    EXPECT(vert_buffer[0].r == 0 && vert_buffer[0].g == 83 &&
               vert_buffer[0].b == 133,
           "A1: primitive carries the level voidColour");
}

/* Case A2: the bubble sort must move wide ids through a wide temporary.
 * Passing the same two pairs in the wrong order forces one swap; before the
 * widening `swapByte = arg1[i]` truncates 128/129 on the way through (and
 * the walker had already gone red on the A1 lookup). After it, the caller's
 * list comes back correctly sorted and the primitive still emits. */
static void case_a2_swap_path_wide(void) {
    VoidPairIndex open_list[2] = { 129, 128 };

    build_wide_table();
    func_80026E54(2, open_list, 250.0f, 150.0f);

    EXPECT(open_list[0] == 128 && open_list[1] == 129,
           "A2: sort returned the wide ids intact");
    EXPECT(gVoidPrimCount == 1, "A2: swapped call still emits one primitive");
    EXPECT(gVoidVertCount == 4, "A2: swapped call still emits four vertices");
}

/* Shared B-case table: two complete edges (floor id 0, ceiling id 1) plus a
 * lone first-of-pair (id 2), the shape exact saturation leaves behind when
 * func_80026C14 accepts the first push of an edge and drops the second. */
static void build_orphan_table(void) {
    harness_reset();
    push_flat_pair(100, 300, 100, 1); /* pair id 0, floor */
    push_flat_pair(100, 300, 400, 0); /* pair id 1, ceiling */
    func_80026C14(100, 250, 1);       /* pair id 2: orphan first-of-pair */
    EXPECT(D_8011D49E == 5, "orphan table holds 5 entries");
    fill_pair_slots();
    EXPECT((D_8011D478[D_8011D47C[4]].unk6 & 2) != 0,
           "orphan entry is marked first-of-pair");
}

/* Case B1: the orphan's partner slot holds -1 (its push-time init value).
 * Before the walker skip, `next = &D_8011D478[-1]` reads one entry before
 * the table -- ASan heap-buffer-underflow (RED). After it, the orphan is
 * dropped from the open list and the surviving pair still emits. */
static void case_b1_orphan_minus_one(void) {
    VoidPairIndex open_list[3] = { 0, 2, 1 };

    build_orphan_table();
    D_8011D47C[5] = -1; /* what saturation leaves for the dropped partner */
    func_80026E54(3, open_list, 250.0f, 150.0f);

    EXPECT(gVoidPrimCount == 1, "B1: surviving pair still emits");
    EXPECT(gVoidVertCount == 4, "B1: surviving pair emits four vertices");
    EXPECT(vert_buffer[0].y == 102 && vert_buffer[2].y == 398,
           "B1: surviving band heights are the complete pair's");
    EXPECT(open_list[0] == 0 && open_list[1] == 1,
           "B1: orphan id compacted out of the caller's open list");
}

/* Case B2: the partner slot can also be stale garbage -- slot 2p+1 of the
 * orphan pair is one past the -1 initialisation run, so whatever the pool
 * held last frame is still there. A stale in-range index aliased the walker
 * onto an unrelated entry: staged here so the aliased entry shares the
 * orphan's x, which made the pre-fix walker take its equal-x bail and
 * silently drop EVERY band in the strip (RED as a plain assertion failure).
 * The bounds-validating skip ignores the stale slot instead (GREEN). */
static void case_b2_orphan_stale_slot(void) {
    VoidPairIndex open_list[3] = { 0, 2, 1 };

    build_orphan_table();
    D_8011D47C[5] = 300; /* stale index, >= D_8011D49E but inside the block */
    D_8011D478[300].unk0 = 100; /* equal-x with the orphan's first entry */
    D_8011D478[300].unk2 = 0;
    func_80026E54(3, open_list, 250.0f, 150.0f);

    EXPECT(gVoidPrimCount == 1,
           "B2: stale partner slot must not suppress the surviving pair");
    EXPECT(gVoidVertCount == 4, "B2: surviving pair emits four vertices");
}

int main(int argc, char **argv) {
    struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        { "A1", case_a1_walker_locals_wide },
        { "A2", case_a2_swap_path_wide },
        { "B1", case_b1_orphan_minus_one },
        { "B2", case_b2_orphan_stale_slot },
    };
    size_t i;
    int ran = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) {
            continue;
        }
        cases[i].run();
        ran++;
    }
    if (ran == 0) {
        fprintf(stderr, "FAIL: unknown case '%s'\n", argv[1]);
        return 1;
    }
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("void pair walker: %d case(s) passed\n", ran);
    return 0;
}
