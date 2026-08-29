#ifndef MDKR_ROLLBACK_GAME_AUTHORITY_H
#define MDKR_ROLLBACK_GAME_AUTHORITY_H

#include <stdbool.h>

#include "rollback_snapshot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_ROLLBACK_TAG_OBJECT_LIST UINT32_C(0x00410001)

/* Register the currently implemented real-game authority foundation. This is
 * valid only after object pools, object lists, settings and the level maps have
 * been initialized, and before match tick zero. Registration is atomic. */
bool mdkr_rollback_game_authority_register(
    MdkrRollbackSnapshotRegistry *registry);
/* Reject a newly reachable POOL_MAIN behaviour allocation that was not part of
 * the frozen registry. This turns dynamic authority growth into an immediate,
 * named unsupported-mode failure instead of a later replay desync. */
bool mdkr_rollback_game_authority_validate_dynamic_coverage(
    const MdkrRollbackSnapshotRegistry *registry);
bool mdkr_rollback_game_authority_is_input_tag(uint32_t tag);

#if defined(MDKR_ENABLE_ONLINE_BETA)
#include <stdint.h>
/* Monotonic count of snapshot RESTORES observed by the game-authority rebuild
 * hook (every rollback correction runs exactly one restore before its resim).
 * Diagnostic read-only feed for the presentation-side camera census; nothing
 * branches on it. Beta-only so the beta-off objects stay byte-identical. */
uint64_t mdkr_rollback_game_authority_restore_serial(void);
/* rcp_dkr.c's authored-tick accessor, host-safe. Its own header (rcp_dkr.h)
 * needs the full <ultra64.h> chain, which host rollback TUs cannot include;
 * this spelling is the identical type, not merely a compatible one, because
 * ultratypes.h:32 typedefs u64 = uint64_t under NATIVE_PORT. The rebuild hook
 * uses it to re-arm the authored-camera latch for the in-flight tick. */
uint64_t presentation_task_authoring_tick(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
