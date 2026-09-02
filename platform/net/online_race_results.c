#include "online_race_results.h"

#include <stdio.h>
#include <string.h>

static uint8_t sPlacements[MDKR_ONLINE_RACE_RESULT_SLOTS];
/* Monotonic per-publish race epoch vs. the last epoch a poll handed out.
 * Equal means "nothing new": the initial 0 == 0 state is the same guard that
 * later stops an already-read race from being re-read. */
static uint32_t sRaceEpoch;
static uint32_t sPolledEpoch;

void mdkr_online_race_results_publish(
    const uint8_t placements[MDKR_ONLINE_RACE_RESULT_SLOTS]) {
    if (placements == NULL) return;
    memcpy(sPlacements, placements, sizeof(sPlacements));
    sRaceEpoch++;
    fprintf(stderr,
            "[online-results] placements=%u,%u,%u,%u epoch=%u\n",
            (unsigned)sPlacements[0], (unsigned)sPlacements[1],
            (unsigned)sPlacements[2], (unsigned)sPlacements[3],
            (unsigned)sRaceEpoch);
}

bool mdkr_online_race_results_poll(
    uint8_t out[MDKR_ONLINE_RACE_RESULT_SLOTS]) {
    if (out == NULL || sPolledEpoch == sRaceEpoch) return false;
    memcpy(out, sPlacements, sizeof(sPlacements));
    sPolledEpoch = sRaceEpoch;
    return true;
}

#if MDKR_ENABLE_ONLINE_BETA
/* Beta-only so a normal (beta OFF) build's online_race_results.c.o is byte-
 * identical before/after this addition -- the OFF preprocessor emits nothing
 * here, exactly like the beta-gated engine TUs. Its only caller
 * (mdkr_online_session_resume_results) is itself compiled only under the beta
 * gate. */
bool mdkr_online_race_results_available(void) {
    /* Same guard the poll reads, but WITHOUT advancing sPolledEpoch: a peek, not
     * a take. */
    return sPolledEpoch != sRaceEpoch;
}
#endif
