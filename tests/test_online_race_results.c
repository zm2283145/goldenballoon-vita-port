/* Contract test for platform/net/online_race_results.c: the one-shot,
 * race-epoch-guarded handoff of a finished online race's placements from the
 * engine's race-finish hook to the launcher.
 *
 * The offline guarantee is structural and is asserted here from the
 * consumer's side: the engine hook that calls publish() is gated on
 * mdkr_net_roster_runtime_active(), so an offline race never publishes --
 * which must mean a poll that was never preceded by a publish returns false
 * forever, and a result that was read once is never handed out again. */
#include "platform/net/online_race_results.h"

#include <stdio.h>

static int failures;

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    uint8_t out[MDKR_ONLINE_RACE_RESULT_SLOTS];
    uint8_t race1[MDKR_ONLINE_RACE_RESULT_SLOTS] = {
        1u, 0u, MDKR_ONLINE_RACE_RESULT_NONE, MDKR_ONLINE_RACE_RESULT_NONE};
    uint8_t race2[MDKR_ONLINE_RACE_RESULT_SLOTS] = {
        0u, 1u, MDKR_ONLINE_RACE_RESULT_NONE, MDKR_ONLINE_RACE_RESULT_NONE};
    uint8_t race3[MDKR_ONLINE_RACE_RESULT_SLOTS] = {3u, 2u, 1u, 0u};

    /* No race recorded (== every offline race, whose gated hook never
     * publishes): the poll must stay silent, and must not touch out[]. */
    out[0] = 0x5au;
    expect(!mdkr_online_race_results_poll(out),
           "poll before any publish must return false");
    expect(out[0] == 0x5au, "a false poll must not write the output");
    expect(!mdkr_online_race_results_poll(NULL),
           "a NULL output poll must return false");

    /* One recorded race reads back exactly once. */
    mdkr_online_race_results_publish(race1);
    expect(mdkr_online_race_results_poll(out),
           "poll after publish must return true");
    expect(out[0] == 1u && out[1] == 0u &&
               out[2] == MDKR_ONLINE_RACE_RESULT_NONE &&
               out[3] == MDKR_ONLINE_RACE_RESULT_NONE,
           "poll must return the published placements, 0xFF for empty slots");
    expect(!mdkr_online_race_results_poll(out),
           "a second poll of the same race must return false (one-shot)");

    /* A NULL publish is ignored and must not fake a new race. */
    mdkr_online_race_results_publish(NULL);
    expect(!mdkr_online_race_results_poll(out),
           "a NULL publish must not arm the poll");

    /* The next race re-arms the poll with the new placements. */
    mdkr_online_race_results_publish(race2);
    expect(mdkr_online_race_results_poll(out),
           "a later race must be pollable again (epoch advanced)");
    expect(out[0] == 0u && out[1] == 1u,
           "the later race's placements must be the ones read");

    /* An unread race is overwritten by the next one: the launcher only ever
     * sees the newest recorded race, never a stale one. */
    mdkr_online_race_results_publish(race1);
    mdkr_online_race_results_publish(race3);
    expect(mdkr_online_race_results_poll(out),
           "the newest of two unread races must be pollable");
    expect(out[0] == 3u && out[1] == 2u && out[2] == 1u && out[3] == 0u,
           "an unread race must have been replaced by the newest one");
    expect(!mdkr_online_race_results_poll(out),
           "the overwritten stale race must never be re-read");

    if (failures == 0) {
        printf("PASS online_race_results: %s\n",
               "one-shot epoch-guarded placements handoff");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
