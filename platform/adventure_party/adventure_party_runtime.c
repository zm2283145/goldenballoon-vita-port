/** adventure_party_runtime.c — see adventure_party_runtime.h.
 *
 * Only compiled into the game binary on the non-OMIT arm (CMakeLists gates it);
 * on the OMIT arm the header's static-inline fallback stands in, so this file
 * needs no #ifdef of its own. The unit tests and platform/save_state.c link it
 * directly and always see the real singleton.
 */
#include "adventure_party/adventure_party_runtime.h"

/* The one instance. Zero-initialised, which adventure_party_state.h documents as
 * a valid, inert (OFF) session, so is_active() answers 0 before anything ever
 * forms a party — no explicit boot-time init required. */
static AdventurePartySession s_session;

AdventurePartySession *adventure_party_runtime_session(void) {
    return &s_session;
}

int adventure_party_runtime_is_active(void) {
    return adventure_party_is_active(&s_session);
}

void adventure_party_runtime_reset(void) {
    adventure_party_session_init(&s_session);
}
