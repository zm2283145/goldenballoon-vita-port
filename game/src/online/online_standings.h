#ifndef MDKR_ONLINE_STANDINGS_H
#define MDKR_ONLINE_STANDINGS_H

/* SEPARATED-BOOT-PATH shared final-standings ordering.
 *
 * The ONE selection-sort both the native online RESULTS/STANDINGS screen
 * (online_results.c) and the native online champion CEREMONY (online_ceremony.c)
 * run to rank the room's occupied seats by cup points. Keeping it here
 * DRY guarantees the two screens can NEVER disagree about who is in the lead: the
 * ceremony's champion (order[0]) is byte-for-byte the same seat the STANDINGS
 * screen just crowned #1. It is a pure function of the party_link forward-feed
 * snapshot -- it reads points[] / last_placements[] / seats[].occupied and mutates
 * nothing -- so both callers stay on the "read the snapshot fresh" discipline
 * every other online screen follows.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and it is only ever included by the beta-gated online TUs
 * (game/src/online/ is NOT auto-globbed). The sort is a static inline (each TU
 * gets its own copy -- no linkage change, no new object): collect occupied seats in
 * seat order, then selection-sort by points DESCENDING, tie-broken on this race's
 * finish (lower last_placement wins) so equal totals are not host-biased by seat
 * order.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "types.h"
#include "net/party_link.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Occupied seats ranked best-first. order[i] is the canonical seat slot, with
 * points[i] / lastpl[i] / char_id[i] parallel to it; count is the number of
 * occupied seats. After compute(), order[0] / points[0] is the champion + their
 * final total, and char_id[0] is the champion's captured character id. char_id is
 * latched HERE (not re-read from a live seat) so the champion CEREMONY can still
 * resolve the winner's portrait + canonical name after that seat has DISCONNECTED
 * -- the departed winner keeps their real face/name instead of degrading to a "Pn"
 * slot fallback. */
typedef struct MdkrOnlineStandings {
    u8 order[MDKR_PARTY_LINK_SEATS];
    u16 points[MDKR_PARTY_LINK_SEATS];
    u8 lastpl[MDKR_PARTY_LINK_SEATS];
    u8 char_id[MDKR_PARTY_LINK_SEATS];
    unsigned count;
} MdkrOnlineStandings;

/* Rank the snapshot's occupied seats by cup points (descending, tie-broken on the
 * ascending last_placement). `haveSnap` false yields an empty ranking (count 0),
 * so a feed-less endpoint degrades to "no champion resolvable" rather than reading
 * garbage. Pure: never mutates *snap. */
static inline void mdkr_online_standings_compute(
    const MdkrPartyLinkSnapshot *snap, bool haveSnap, MdkrOnlineStandings *out) {
    unsigned i, j;

    if (out == NULL) {
        return;
    }
    out->count = 0u;
    for (i = 0u; i < MDKR_PARTY_LINK_SEATS; i++) {
        if (haveSnap && snap != NULL && snap->seats[i].occupied) {
            out->order[out->count] = (u8) i;
            out->points[out->count] = snap->points[i];
            out->lastpl[out->count] = snap->last_placements[i];
            out->char_id[out->count] = snap->seats[i].character_id;
            out->count++;
        }
    }
    for (i = 0u; i + 1u < out->count; i++) {
        for (j = i + 1u; j < out->count; j++) {
            bool swap = (out->points[j] > out->points[i]) ||
                        (out->points[j] == out->points[i] &&
                         out->lastpl[j] < out->lastpl[i]);
            if (swap) {
                u16 tp = out->points[i];
                out->points[i] = out->points[j];
                out->points[j] = tp;
                {
                    u8 to = out->order[i];
                    out->order[i] = out->order[j];
                    out->order[j] = to;
                }
                {
                    u8 tl = out->lastpl[i];
                    out->lastpl[i] = out->lastpl[j];
                    out->lastpl[j] = tl;
                }
                {
                    u8 tc = out->char_id[i];
                    out->char_id[i] = out->char_id[j];
                    out->char_id[j] = tc;
                }
            }
        }
    }
}

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_STANDINGS_H */
