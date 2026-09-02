#ifndef GFX_DKR_DL_GUARDS_H
#define GFX_DKR_DL_GUARDS_H

/*
 * Rules shared by the two DKR display-list walkers in gfx_pc_dkr.c: the
 * interpreter (dkr_run_dl) and the output-overlay prepass
 * (dkr_scan_overlay_order). Both fetch commands from game-authored memory, so
 * both need the same answer to "may I read here". Keeping the answer here
 * rather than open-coded at each fetch is what makes it the same answer; it is
 * pure, so tests/test_fast3d_dl_guards.c exercises it directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A pointer is host-plausible if it is non-null and not a sign-extended 32-bit
 * token (high 32 bits all ones). User-space host mappings never live at
 * 0xffffffff........, and the arena's high bits are 0x4-0x7, so an all-ones high
 * half is unambiguously a truncated-then-sign-extended pointer. dkr_resolve
 * refuses to hand such a value to any DL consumer — the belt-and-suspenders side
 * of the char-select SIGSEGV fix (the truncation itself is fixed at its source
 * in tracks.c render_level_segment). */
static inline bool dkr_ptr_plausible(const void *p) {
    uintptr_t up = (uintptr_t) p;
    if (up == 0) return false;
#if UINTPTR_MAX > UINT32_MAX
    if ((up >> 32) == UINT32_MAX) return false;
#endif
    return true;
}

/*
 * Does `room` from dkr_arena_room() back a read of `need` bytes at `p`?
 *
 * dkr_arena_room() answers SIZE_MAX for a pointer it does not own — an ordinary
 * global or rodata address, whose extent it cannot know. SIZE_MAX compares
 * greater than every `need`, so a bare `room >= need` test silently admits
 * EVERY non-arena value, a manufactured one included: in-arena-ness was never
 * established by such a test, only assumed. Routing the question through this
 * predicate makes the two cases answer separately — an owned extent decides on
 * its own bytes, an unowned pointer decides on provenance, and no call site can
 * express the check in a way that conflates them.
 */
static inline bool dkr_room_admits(size_t room, const void *p, size_t need) {
    if (room == (size_t)-1) {
        return dkr_ptr_plausible(p);
    }
    return room >= need;
}

#endif /* GFX_DKR_DL_GUARDS_H */
