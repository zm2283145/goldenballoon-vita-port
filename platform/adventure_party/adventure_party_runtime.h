/* adventure_party_runtime.h — the one process-wide Adventure Party session.
 *
 * Controller ruling R12: exactly ONE AdventurePartySession exists per process,
 * and adapters reach it only through this seam. The reducer and its value
 * struct live in adventure_party_state.h; this module owns the single instance
 * of that struct and nothing else — no policy, no reducer logic, no globals
 * beyond the one session.
 *
 * The OMIT arm (MDKR_ADVENTURE_PARTY_OMIT) compiles the whole module out of the
 * game binary. A header-level static-inline fallback then keeps
 * adventure_party_runtime_is_active() answering a flat 0 and
 * adventure_party_runtime_session() answering NULL, so a consumer such as
 * platform/save_state.c compiles and behaves correctly on BOTH arms without an
 * #ifdef of its own: it asks one question and gets an honest answer either way.
 */
#ifndef MDKR64_ADVENTURE_PARTY_RUNTIME_H
#define MDKR64_ADVENTURE_PARTY_RUNTIME_H

#include <stddef.h>

#include "adventure_party/adventure_party_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef MDKR_ADVENTURE_PARTY_OMIT

/* Module compiled out: the feature cannot be active and there is no session. */
static inline AdventurePartySession *adventure_party_runtime_session(void) {
    return NULL;
}
static inline int adventure_party_runtime_is_active(void) { return 0; }

#else

/* The one process-wide session, as a mutable pointer for the adapters that
 * drive it through adventure_party_session_apply(). Never NULL: a fresh
 * process holds a zeroed, OFF (inert) session. */
AdventurePartySession *adventure_party_runtime_session(void);

/* Non-zero exactly while a session exists (state != OFF); 0 when off. 0 also in
 * the OMIT arm via the fallback above, so save_state and any other consumer ask
 * one question with one answer on both arms. */
int adventure_party_runtime_is_active(void);

/* Return the singleton to a zeroed, OFF session, so a stale roster cannot
 * outlive its session. The instance itself never moves. */
void adventure_party_runtime_reset(void);

#endif /* MDKR_ADVENTURE_PARTY_OMIT */

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_ADVENTURE_PARTY_RUNTIME_H */
