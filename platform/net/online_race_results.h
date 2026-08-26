/* Observation-only handoff of a finished ONLINE race's placements from the
 * engine to the launcher.
 *
 * The engine side is the race-finish write in game/src/objects.c (the point
 * where DKR itself commits the final finish order into
 * settings->racers[].starting_position). That hook is compiled only under
 * MDKR_ENABLE_ONLINE_BETA and fires only while the online roster is active
 * (mdkr_net_roster_runtime_active()), so offline play never publishes and
 * never observes this module. The launcher side polls after the engine
 * session returns and owns the online results/standings UI.
 */
#ifndef MDKR_ONLINE_RACE_RESULTS_H
#define MDKR_ONLINE_RACE_RESULTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ONLINE_RACE_RESULT_SLOTS 4u
/* This canonical slot had no human racer in the finished race. */
#define MDKR_ONLINE_RACE_RESULT_NONE 0xFFu

/* Engine side: publish the finished race's per-CANONICAL-SLOT placements
 * (0 == first place; MDKR_ONLINE_RACE_RESULT_NONE for slots without a human
 * racer). Each publish starts a new race epoch: it overwrites any unread
 * previous race and re-arms the poll below. Emits the tagged witness line
 * "[online-results] placements=..." on stderr. */
void mdkr_online_race_results_publish(
    const uint8_t placements[MDKR_ONLINE_RACE_RESULT_SLOTS]);

/* Launcher side: copy the recorded placements into out[] and return true
 * exactly once per recorded race (a race-epoch guard, so a result that was
 * already read -- or no result at all -- is never handed out again). */
bool mdkr_online_race_results_poll(
    uint8_t out[MDKR_ONLINE_RACE_RESULT_SLOTS]);

#ifdef __cplusplus
}
#endif
#endif
