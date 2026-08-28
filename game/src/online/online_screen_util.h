#ifndef MDKR_ONLINE_SCREEN_UTIL_H
#define MDKR_ONLINE_SCREEN_UTIL_H

/* SEPARATED-BOOT-PATH (Strategy D2) shared native-screen draw/state helpers.
 *
 * The small helper families every native online SCREEN (charselect / trackselect
 * / results / ceremony) needs, lifted DRY so the four screens can never drift:
 *   - mdkr_online_screen_local_seat   which snapshot seat is the local player
 *   - mdkr_online_screen_text         font + colour + draw_text into the frame list
 *   - mdkr_online_screen_pulse        the 0..16 triangle-wave cursor/heartbeat
 *   - mdkr_online_screen_seat_name    one seat's short name (untrusted snapshot
 *                                     name, else the character name, else Pn)
 *   - mdkr_online_screen_seconds_left ceil of a 60ths-of-a-second countdown
 *   - mdkr_online_screen_draw_portrait the guarded racer-portrait blit
 * Each was previously copy-pasted per screen; a single source guarantees the
 * shared visual/state vocabulary is byte-for-byte the same on every screen.
 *
 * gCurrDisplayList (the engine's live 2D frame list) and gRacerPortraits[] (the
 * decoded racer faces) are declared here ONCE, so the text/portrait inlines can
 * reference them without each screen re-declaring the externs. Neither symbol is
 * exposed by menu.h; both already have external linkage in the engine, so this
 * reaches them without editing menu.c. gRacerPortraits[k] is a DrawTexture[2]:
 * element[0] the portrait, element[1] the NULL terminator texrect_draw stops on;
 * the array is in its OWN portrait order (see sOnlineToPortrait[]).
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and it is only ever included by the beta-gated online screen
 * TUs (game/src/online/ is NOT auto-globbed). The helpers are static inline (each
 * TU gets its own copy -- no linkage change, no new object, no ODR risk), the
 * same discipline online_portraits.h / online_standings.h model.
 */
#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST: rcp_dkr.h pulls ultra64.h -> PR/os_libc.h, which declares
 * sprintf/snprintf as plain functions; the system <stdio.h> below MUST follow it
 * (otherwise sprintf would already be a fortify macro and os_libc's declaration
 * would fail) -- the exact ordering the screen TUs rely on. */
#include "types.h"
#include "enums.h"      /* AlignmentFlags */
#include "menu.h"       /* font.h: set_text_font/colour, draw_text; DrawTexture */
#include "rcp_dkr.h"    /* texrect_draw */
#include "net/party_link.h"
#include "online/online_portraits.h" /* sOnlineToPortrait, sOnlineNames,
                                        MDKR_ONLINE_PORTRAIT_COUNT */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The engine's live 2D display list for the current frame, and the decoded racer
 * portraits -- see the header note above for why they are declared here. */
extern Gfx *gCurrDisplayList;
extern DrawTexture *gRacerPortraits[10];

/* Which snapshot seat is the local one (display-only lookups). -1 when none. */
static inline s32 mdkr_online_screen_local_seat(
    const MdkrPartyLinkSnapshot *snap) {
    unsigned i;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (snap->seats[i].occupied && snap->seats[i].is_local) {
            return (s32) i;
        }
    }
    return -1;
}

/* Draw text into the engine frame's display list with the given font + colour. */
static inline void mdkr_online_screen_text(s32 x, s32 y, s32 fontId, char *text,
                                           AlignmentFlags align, s32 r, s32 g,
                                           s32 b) {
    set_text_font(fontId);
    set_text_background_colour(0, 0, 0, 0);
    set_text_colour(r, g, b, 0, 255);
    draw_text(&gCurrDisplayList, x, y, text, align);
}

/* Triangle-wave pulse 0..16 off a tick counter -- the shared native highlight /
 * heartbeat feel with zero assets. */
static inline s32 mdkr_online_screen_pulse(u32 ticks) {
    s32 tri = (s32) (ticks & 31u);
    if (tri > 16) {
        tri = 32 - tri;
    }
    return tri;
}

/* Resolve one seat's short name: the untrusted snapshot name if present, else the
 * character's canonical name, else a "Pn" slot fallback. */
static inline void mdkr_online_screen_seat_name(
    const MdkrPartyLinkSnapshot *snap, bool haveSnap, unsigned slot, char *out,
    size_t cap) {
    if (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
        snap->seats[slot].occupied && snap->seats[slot].name[0] != '\0') {
        (void) snprintf(out, cap, "%.*s", (int) MDKR_PARTY_LINK_NAME_BYTES,
                        snap->seats[slot].name);
        return;
    }
    if (haveSnap && slot < MDKR_PARTY_LINK_SEATS &&
        snap->seats[slot].character_id < MDKR_ONLINE_PORTRAIT_COUNT) {
        (void) snprintf(out, cap, "%s",
                        sOnlineNames[snap->seats[slot].character_id]);
        return;
    }
    (void) snprintf(out, cap, "P%u", slot + 1u);
}

/* Countdown seconds still on the clock (ceil), 0 when elapsed. `done`/`limit` are
 * in 60ths of a second (updateRate accumulates at 60Hz). */
static inline u32 mdkr_online_screen_seconds_left(u32 done, u32 limit) {
    if (done >= limit) {
        return 0u;
    }
    return (limit - done + 59u) / 60u;
}

/* Blit a racer portrait (guarded: out-of-range id and unloaded texture are no-ops,
 * matching the faithful DrawTexture[] NULL-terminator scan). */
static inline void mdkr_online_screen_draw_portrait(u8 character, s32 x, s32 y,
                                                    u8 r, u8 g, u8 b) {
    DrawTexture *portrait;
    if (character >= MDKR_ONLINE_PORTRAIT_COUNT) {
        return;
    }
    portrait = gRacerPortraits[sOnlineToPortrait[character]];
    if (portrait != NULL && portrait[0].texture != NULL) {
        texrect_draw(&gCurrDisplayList, portrait, x, y, r, g, b, 255);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_SCREEN_UTIL_H */
