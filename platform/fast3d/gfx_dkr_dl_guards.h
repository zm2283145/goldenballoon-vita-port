#ifndef GFX_DKR_DL_GUARDS_H
#define GFX_DKR_DL_GUARDS_H

/*
 * Rules shared by the two DKR display-list walkers in gfx_pc_dkr.c: the
 * interpreter (dkr_run_dl) and the output-overlay prepass
 * (dkr_scan_overlay_order). Both fetch commands from game-authored memory, so
 * both need the same answer to "may I read here" and "is this still a display
 * list". Keeping the answers here rather than open-coded at each fetch is what
 * makes them the same answer; they are pure, so tests/test_fast3d_dl_guards.c
 * exercises them directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <PR/gbi.h>
#include "f3ddkr.h"

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

/*
 * Is `op` an opcode dkr_run_dl dispatches?
 *
 * The list mirrors that function's top-level switch; its `default:` arm is the
 * same refusal, reported through dkr_dl_fault as "unknown display-list opcode".
 * The prepass needs the set explicitly because its own switch reads only the
 * handful of commands that affect draw-space ordering and ignores the rest, so
 * it cannot tell a command it does not care about from a byte that is not a
 * command at all. tests/check_fast3d_dl_hardening.py binds this set to real
 * content: every opcode an MDKR_DL_CENSUS route counts must be in it.
 */
static inline bool dkr_dl_opcode_implemented(uint8_t op) {
    switch (op) {
        case (uint8_t)G_SPNOOP:
        case (uint8_t)G_DL:
        case (uint8_t)G_DMADL:
        case (uint8_t)G_ENDDL:
        case (uint8_t)G_MTX:
        case (uint8_t)G_POPMTX:
        case (uint8_t)G_MOVEMEM:
        case (uint8_t)G_MOVEWORD:
        case (uint8_t)G_VTX:
        case (uint8_t)G_TRIN:
        case (uint8_t)G_CULLDL:
        case (uint8_t)G_PERSPNORMALIZE:
        case (uint8_t)G_TEXTURE:
        case (uint8_t)G_SETGEOMETRYMODE:
        case (uint8_t)G_CLEARGEOMETRYMODE:
        case (uint8_t)G_SETOTHERMODE_H:
        case (uint8_t)G_SETOTHERMODE_L:
        case (uint8_t)G_RDPSETOTHERMODE:
        case (uint8_t)G_SETTIMG:
        case (uint8_t)G_SETCIMG:
        case (uint8_t)G_SETZIMG:
        case (uint8_t)G_SETTILE:
        case (uint8_t)G_SETTILESIZE:
        case (uint8_t)G_LOADBLOCK:
        case (uint8_t)G_LOADTILE:
        case (uint8_t)G_LOADTLUT:
        case (uint8_t)G_SETCOMBINE:
        case (uint8_t)G_SETENVCOLOR:
        case (uint8_t)G_SETPRIMCOLOR:
        case (uint8_t)G_SETBLENDCOLOR:
        case (uint8_t)G_SETFOGCOLOR:
        case (uint8_t)G_SETFILLCOLOR:
        case (uint8_t)G_FILLRECT:
        case (uint8_t)G_SETSCISSOR:
        case (uint8_t)G_TEXRECT:
        case (uint8_t)G_TEXRECTFLIP:
        case (uint8_t)G_RDPHALF_1:
        case (uint8_t)G_RDPHALF_2:
        case (uint8_t)G_RDPFULLSYNC:
        case (uint8_t)G_RDPTILESYNC:
        case (uint8_t)G_RDPPIPESYNC:
        case (uint8_t)G_RDPLOADSYNC:
        case (uint8_t)G_NOOP:
            return true;
        default:
            return false;
    }
}

#endif /* GFX_DKR_DL_GUARDS_H */
