/* Pure adapter from a consensus lobby snapshot to the engine descriptor. */
#ifndef MDKR_MATCH_LAUNCH_BUILDER_H
#define MDKR_MATCH_LAUNCH_BUILDER_H

#include "lobby_core.h"
#include "net/match_launch_descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Online v1 admits RETAIL identities only. MdkrMatchManifestV1 carries no
 * roster identity: each peer resolves bonus (mod-roster) identity from its
 * LOCAL roster state, so two peers could disagree about who a slot is at
 * tick zero. That the descriptor cannot even express a bonus identity is
 * expected -- the clamp therefore reads local roster state at admission
 * time; a manifest v2 may bind identities into the manifest instead. */
#define MDKR_MATCH_LOCAL_PLAYER_SLOTS 4u
#define MDKR_MATCH_IDENTITY_RETAIL 0u

typedef struct MdkrMatchLocalRosterV1 {
    /* Identity the local mod roster would resolve for each local player
     * slot, in game/src/taj_mod.h ModRacerIdentity values; the slot count
     * mirrors TAJ_MOD_MAX_PLAYERS. MDKR_MATCH_IDENTITY_RETAIL is retail.
     * The launcher fills this from mod_racer_player_identity(), which
     * already folds in session selections and test-environment identities. */
    uint8_t player_identity[MDKR_MATCH_LOCAL_PLAYER_SLOTS];
} MdkrMatchLocalRosterV1;

/* Stable admission refusals in the lobby view-model idiom: a bounded enum,
 * never raw provider or transport strings. */
typedef enum MdkrMatchLaunchRefusal {
    MDKR_MATCH_LAUNCH_ADMITTED = 0,
    /* Lobby/manifest/roster snapshot missing, invalid, or out of phase. */
    MDKR_MATCH_LAUNCH_REFUSE_SNAPSHOT,
    /* Frozen seat selections do not validate against the manifest. */
    MDKR_MATCH_LAUNCH_REFUSE_SELECTIONS,
    /* A local player would resolve to a bonus (non-retail) identity; online
     * v1 races retail identities only. */
    MDKR_MATCH_LAUNCH_REFUSE_NON_RETAIL_IDENTITY
} MdkrMatchLaunchRefusal;

/* Fail-atomic and fail-closed: refusals leave `output` untouched, and a
 * missing local roster refuses rather than assumes retail. This builder is
 * the only seam that produces an online launch descriptor, and it runs in
 * the launcher at the Loading barrier -- strictly before any level load can
 * reach taj_mod_begin_racer_bindings() -- so a refusal here refuses the
 * online match before bindings activation can start. Offline/local play
 * never passes through this builder and is unaffected. `refusal` is
 * optional; when non-NULL it always receives the typed outcome. */
bool mdkr_match_launch_descriptor_from_lobby(
    const MdkrOnlineLobby *lobby, const MdkrMatchManifestV1 *manifest,
    const MdkrMatchLocalRosterV1 *local_roster,
    MdkrMatchLaunchDescriptorV1 *output, MdkrMatchLaunchRefusal *refusal);

#ifdef __cplusplus
}
#endif
#endif
