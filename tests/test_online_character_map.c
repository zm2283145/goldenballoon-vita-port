/* All-cells proof of the online-catalog-id -> engine Character-enum mapping that
 * the direct-boot racer spawn (menu.c get_character_id_from_slot ->
 * mdkr_online_character_to_engine) rides. Assert-driven: NDEBUG (Release) would
 * compile every check away. */
#undef NDEBUG

#include <assert.h>
#include <stdio.h>

#include "online/online_character_map.h" /* mdkr_online_character_to_engine + Character */

/* Ground truth, by RACER, not by id: for each online-catalog cell the grid
 * offers (index == the launcher's kCharacters strip == the published
 * hover_character the reducer validates), the racer that must SPAWN. Verified
 * against the engine Character enum (enums.h) by identity of the racer, never by
 * a shared symbol name -- the online catalog and the engine enum are two
 * different orderings, so trusting position is exactly the bug. */
struct Cell {
    int online_id;
    int expect_engine; /* the engine Character-enum value that must spawn */
    const char *racer;
};

static const struct Cell kCells[] = {
    {0, CHARACTER_DIDDY, "Diddy"},
    {1, CHARACTER_TIMBER, "Timber"},
    {2, CHARACTER_PIPSY, "Pipsy"},
    {3, CHARACTER_TIPTUP, "Tiptup"},
    {4, CHARACTER_CONKER, "Conker"},
    {5, CHARACTER_BUMPER, "Bumper"},
    {6, CHARACTER_BANJO, "Banjo"},
    {7, CHARACTER_KRUNCH, "Krunch"},
    {8, CHARACTER_DRUMSTICK, "Drumstick"},
    {9, CHARACTER_TT, "T.T."},
};

int main(void) {
    const int n = (int)(sizeof(kCells) / sizeof(kCells[0]));
    int i;

    assert(n == MDKR_ONLINE_CHARACTER_MAP_COUNT);
    assert(n == NUMBER_OF_CHARACTERS); /* all ten base racers are covered */

    /* Every grid cell spawns its OWN racer, on either endpoint (a pure,
     * endpoint-independent map over the shared consensus descriptor). */
    for (i = 0; i < n; i++) {
        const int got = mdkr_online_character_to_engine(kCells[i].online_id);
        if (got != kCells[i].expect_engine) {
            fprintf(stderr,
                    "FAIL cell online_id=%d (%s): spawned engine char %d, "
                    "expected %d\n",
                    kCells[i].online_id, kCells[i].racer, got,
                    kCells[i].expect_engine);
            return 1;
        }
    }

    /* The map is a bijection over the ten base characters (no two cells collapse
     * onto one racer -- a renumbering bug would). */
    for (i = 0; i < n; i++) {
        int j;
        const int a = mdkr_online_character_to_engine(kCells[i].online_id);
        for (j = i + 1; j < n; j++) {
            assert(a != mdkr_online_character_to_engine(kCells[j].online_id));
        }
    }

    /* Red guard: pin the exact defect the fix corrects. Passing the raw online
     * id straight through (the old behavior) mis-spawns the reported pair --
     * Tiptup(3)->CONKER, T.T.(9)->DIDDY -- so for those cells the mapped value
     * MUST differ from the raw id. If a future change makes the map an identity
     * again, this fails loudly. */
    assert(3 == CHARACTER_CONKER);                 /* raw 3 is Conker's slot */
    assert(9 == CHARACTER_DIDDY);                  /* raw 9 is Diddy's slot */
    assert(mdkr_online_character_to_engine(3) == CHARACTER_TIPTUP);
    assert(mdkr_online_character_to_engine(3) != 3);
    assert(mdkr_online_character_to_engine(9) == CHARACTER_TT);
    assert(mdkr_online_character_to_engine(9) != 9);

    /* Out-of-range ids pass through unchanged (defensive; callers bound first). */
    assert(mdkr_online_character_to_engine(-1) == -1);
    assert(mdkr_online_character_to_engine(NUMBER_OF_CHARACTERS) ==
           NUMBER_OF_CHARACTERS);

    puts("test_online_character_map: PASS (all 10 cells spawn their own racer)");
    return 0;
}
