/*
 * The two DKR display-list walkers' shared rules (platform/fast3d/
 * gfx_dkr_dl_guards.h), exercised directly because they are pure.
 *
 * The rule exists because of the four-viewport party-hub crash settled under
 * AddressSanitizer: a misauthored stream sent both walkers through bytes that
 * are not commands, and each read past the end of storage it had no extent
 * for. The cases below are the claim that bounds them -- an unowned extent is
 * not room.
 */

#include "gfx_dkr_dl_guards.h"

#include <stdio.h>

static int s_failures;

static void expect(const char *name, bool condition) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        s_failures++;
    }
}

/* What the guards used to say: SIZE_MAX compares greater than every `need`, so
 * the whole non-arena domain answered "room available". Every case that this
 * test claims is now refused is asserted against this rule too, so a refusal
 * that the old code would also have made cannot be mistaken for coverage. */
static bool legacy_room_rule(size_t room, size_t need) {
    return room >= need;
}

int main(void) {
    unsigned char storage[64];
    const void *owned = storage;

    /* Plausibility: null, and on LP64 the sign-extended truncation form. A
     * 32-bit host has no such form -- a truncated pointer there is the
     * pointer -- so those cases are compiled out rather than aliased onto
     * null, which would make them pass without measuring anything. */
    expect("null is not plausible", !dkr_ptr_plausible(NULL));
    expect("a host pointer is plausible", dkr_ptr_plausible(owned));

    /* An owned extent decides on its own bytes. */
    expect("owned extent admits a read it covers",
           dkr_room_admits(sizeof(storage), owned, sizeof(storage)));
    expect("owned extent refuses a read one byte too long",
           !dkr_room_admits(sizeof(storage), owned, sizeof(storage) + 1u));

    /* Zero is what dkr_arena_room() answers for the overrun band above the
     * arena -- an address the arena's own arithmetic produced, not a global. */
    expect("an exhausted extent refuses every read",
           !dkr_room_admits(0u, owned, 8u));

    /* An unowned extent decides on provenance. Globals and rodata display
     * lists stay readable: refusing them would refuse content the retail
     * walk depends on. */
    expect("an unowned extent admits a plausible pointer",
           dkr_room_admits((size_t)-1, owned, sizeof(storage) * 4u));
    expect("an unowned extent refuses null",
           !dkr_room_admits((size_t)-1, NULL, 8u));
#if UINTPTR_MAX > UINT32_MAX
    {
        const void *sign_extended =
            (const void *)(uintptr_t)UINT64_C(0xffffffff12345678);
        expect("a sign-extended token is not plausible",
               !dkr_ptr_plausible(sign_extended));
        expect("an unowned extent refuses a sign-extended pointer",
               !dkr_room_admits((size_t)-1, sign_extended, 8u));
    }
#endif

    /* Positive control: the rule those refusals replaced admitted every
     * unowned pointer whatever the read length, so they are measuring the new
     * rule and not an unrelated bound. */
    expect("the replaced rule admitted an unowned pointer of any length",
           legacy_room_rule((size_t)-1, 8u) &&
               legacy_room_rule((size_t)-1, (size_t)-1));

    if (s_failures != 0) {
        fprintf(stderr, "test_fast3d_dl_guards: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_fast3d_dl_guards: all guards hold\n");
    return 0;
}
