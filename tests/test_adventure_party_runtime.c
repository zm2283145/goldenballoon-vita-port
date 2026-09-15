/*
 * test_adventure_party_runtime.c — the one process-wide Adventure Party
 * session (controller ruling R12).
 *
 * The runtime owns a single AdventurePartySession and hands adapters a mutable
 * pointer to it. This test pins the init/reset/is_active lifecycle and the
 * singleton property, ROM-free: a fresh process is inert, a formed session
 * reads active through the runtime query, and reset returns it to inert. The
 * reducer's own edge cases live in test_adventure_party_state.c; this test only
 * proves the runtime is one live instance driven through that reducer.
 */
#include "adventure_party/adventure_party_runtime.h"
#include "adventure_party/adventure_party_state.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void expect(int c, const char *w) {
    if (!c) { printf("FAIL %s\n", w); failures++; } else printf("ok   %s\n", w);
}

/* A minimal, legal two-seat FORM event — the same shape the state test builds,
 * kept local so this test needs nothing but the public reducer surface. */
static AdventurePartyEvent form_event(void) {
    AdventurePartyEvent e;
    memset(&e, 0, sizeof e);
    e.kind = ADVENTURE_PARTY_EVENT_FORM;
    e.enabled = 1;
    e.adventure_selected = 1;
    e.winner_seat = ADVENTURE_PARTY_NO_SEAT;
    e.roster.participant_count = 2;
    e.roster.seat[0] = 0;
    e.roster.seat[1] = 1;
    e.roster.character[0] = 3;
    e.roster.character[1] = 7;
    return e;
}

int main(void) {
    /* Start from a known state rather than trusting load order. */
    adventure_party_runtime_reset();

    /* A fresh runtime is inert: no session, so no party is active. */
    expect(adventure_party_runtime_is_active() == 0,
           "a reset runtime reports no active party");

    /* One instance: the pointer is stable and never NULL. */
    AdventurePartySession *a = adventure_party_runtime_session();
    AdventurePartySession *b = adventure_party_runtime_session();
    expect(a != NULL, "runtime session pointer is never NULL");
    expect(a == b, "runtime hands back the SAME session every call (singleton)");

    /* Driving the session through the reducer via that mutable pointer makes the
     * runtime query report active — the adapter seam actually reaches the one
     * instance. */
    AdventurePartyEvent form = form_event();
    expect(adventure_party_session_apply(a, &form) == ADVENTURE_PARTY_OK,
           "FORM applied through the runtime pointer is accepted");
    expect(adventure_party_runtime_is_active() == 1,
           "a formed session reads active through the runtime query");
    expect(adventure_party_is_active(adventure_party_runtime_session()) == 1,
           "the same fact is visible on the shared session struct");

    /* Reset returns it to inert, so a stale roster cannot outlive its session. */
    adventure_party_runtime_reset();
    expect(adventure_party_runtime_is_active() == 0,
           "reset returns the runtime to no active party");
    expect(adventure_party_runtime_session()->state == ADVENTURE_PARTY_STATE_OFF,
           "reset leaves the session in the OFF (zeroed) state");
    /* Reset does not move the instance. */
    expect(adventure_party_runtime_session() == a,
           "reset keeps the one session instance in place");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all adventure party runtime checks passed\n");
    return 0;
}
