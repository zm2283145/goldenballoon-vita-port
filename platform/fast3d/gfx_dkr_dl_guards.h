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
 * THE OPCODE SET, ONCE.
 *
 * Every opcode the DKR display-list interpreter walks. Both consumers derive
 * from this list, so neither can name an opcode the other does not:
 *
 *   - dkr_dl_opcode_implemented() below expands it into its case labels;
 *   - dkr_run_dl() (gfx_pc_dkr.c) tests that predicate before its own dispatch
 *     switch, so a `case` label added there and not added here is never
 *     reached -- the interpreter refuses the command loudly on its first
 *     frame instead of the prepass quietly stopping on legal content, which
 *     is the drift that matters. The reverse (listed here, no case there) is
 *     an opcode the switch ignores, and its `default:` arm names it.
 *
 * The prepass (dkr_scan_overlay_order) needs the set explicitly because its own
 * switch reads only the handful of commands that affect draw-space ordering and
 * ignores the rest, so it cannot tell a command it does not care about from a
 * byte that is not a command at all.
 *
 * The set is bound to real content by tests/check_webgpu_content_census.py,
 * which walks 46 routes under MDKR_DL_STRICT=1 on the native lane and requires
 * zero faults: an opcode reachable content uses but this list omits stops the
 * interpreter there, and strict mode turns that into a failure.
 * tests/check_fast3d_dl_hardening.py additionally requires zero [DL] lines on
 * a well-authored party route and a retail 1P route.
 */
#define DKR_DL_IMPLEMENTED_OPCODES(X)                                        \
    X(G_SPNOOP) X(G_DL) X(G_DMADL) X(G_ENDDL)                                \
    X(G_MTX) X(G_POPMTX) X(G_MOVEMEM) X(G_MOVEWORD)                          \
    X(G_VTX) X(G_TRIN) X(G_CULLDL) X(G_PERSPNORMALIZE)                       \
    X(G_TEXTURE) X(G_SETGEOMETRYMODE) X(G_CLEARGEOMETRYMODE)                 \
    X(G_SETOTHERMODE_H) X(G_SETOTHERMODE_L) X(G_RDPSETOTHERMODE)             \
    X(G_SETTIMG) X(G_SETCIMG) X(G_SETZIMG)                                   \
    X(G_SETTILE) X(G_SETTILESIZE)                                            \
    X(G_LOADBLOCK) X(G_LOADTILE) X(G_LOADTLUT)                               \
    X(G_SETCOMBINE) X(G_SETENVCOLOR) X(G_SETPRIMCOLOR)                       \
    X(G_SETBLENDCOLOR) X(G_SETFOGCOLOR) X(G_SETFILLCOLOR)                    \
    X(G_FILLRECT) X(G_SETSCISSOR)                                            \
    X(G_TEXRECT) X(G_TEXRECTFLIP) X(G_RDPHALF_1) X(G_RDPHALF_2)              \
    X(G_RDPFULLSYNC) X(G_RDPTILESYNC) X(G_RDPPIPESYNC) X(G_RDPLOADSYNC)      \
    X(G_NOOP)

/* Is `op` one of them? */
static inline bool dkr_dl_opcode_implemented(uint8_t op) {
#define DKR_DL_OPCODE_CASE(name) case (uint8_t)(name):
    switch (op) {
        DKR_DL_IMPLEMENTED_OPCODES(DKR_DL_OPCODE_CASE)
            return true;
        default:
            return false;
    }
#undef DKR_DL_OPCODE_CASE
}

#endif /* GFX_DKR_DL_GUARDS_H */
