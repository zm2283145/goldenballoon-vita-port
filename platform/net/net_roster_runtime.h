/* Engine-lifetime publication of a launcher-owned, validated network roster. */
#ifndef MDKR_NET_ROSTER_RUNTIME_H
#define MDKR_NET_ROSTER_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "net_roster.h"
#include "match_launch_descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install is intentionally copy-based: the engine never borrows launcher
 * memory and cannot mutate matchmaking/session state. Install before engine
 * threads start; clear only after they have joined. */
bool mdkr_net_roster_runtime_install(
    const MdkrMatchManifestV1 *manifest, const MdkrNetRoster *roster);
/* Player-visible online path. The validated descriptor is copied with the
 * roster; launcher memory is never borrowed by game code. */
bool mdkr_net_roster_runtime_install_launch(
    const MdkrMatchLaunchDescriptorV1 *launch,
    const MdkrNetRoster *roster);
void mdkr_net_roster_runtime_clear(void);
bool mdkr_net_roster_runtime_active(void);

/* Ownership guard (issue: local-Play beach-ball).
 *
 * The installed roster is process-global and engine-lifetime. Historically the
 * launcher only cleared it on engine EXIT and never checked on ENTRY, so a stray
 * local-Play boot that started while an online session's roster was still
 * installed would inherit it, put the engine into online-race mode, and then
 * stall forever waiting for network input that a local race never delivers.
 *
 * Ownership makes that collision impossible to inherit silently. The session
 * that installs a roster tags it with a nonzero owner token; any boot that is
 * NOT that owner must guard itself BEFORE booting. These are additive and inert
 * unless called: they never run on the release path (only the beta online engine
 * boot and the local-Play guard reference them), so a non-beta build dead-strips
 * them and stays byte-identical. */
void mdkr_net_roster_runtime_set_owner(uint64_t owner_token);
uint64_t mdkr_net_roster_runtime_owner(void);
/* Defensive pre-boot guard. If a roster is active and is NOT owned by
 * owner_token, force-clear it and return true (i.e. a foreign roster was
 * discarded so this boot starts clean). owner_token == 0 means "this boot
 * installs no roster" (ordinary local Play), which force-clears ANY active
 * roster. A boot that DID install its own roster passes its own token and is
 * left untouched. */
bool mdkr_net_roster_runtime_guard_owner(uint64_t owner_token);

const MdkrNetRoster *mdkr_net_roster_runtime_get(void);
const MdkrMatchManifestV1 *mdkr_net_roster_runtime_manifest(void);
const MdkrMatchLaunchDescriptorV1 *
mdkr_net_roster_runtime_launch_descriptor(void);
const MdkrMatchSeatSelectionV1 *mdkr_net_roster_runtime_selection(
    unsigned canonical_slot);

/* Authoritative gameplay calls this with its ordinary local-play value. The
 * fallback preserves retail/local behavior when no online roster is active. */
uint8_t mdkr_net_roster_runtime_canonical_player_count(uint8_t fallback);
uint8_t mdkr_net_roster_runtime_viewport_count(uint8_t fallback);
bool mdkr_net_roster_runtime_local_to_canonical(
    unsigned local_seat, uint8_t *canonical_slot);
bool mdkr_net_roster_runtime_viewport_to_canonical(
    unsigned viewport, uint8_t *canonical_slot);
bool mdkr_net_roster_runtime_canonical_to_local(
    unsigned canonical_slot, uint8_t *local_seat);

#ifdef __cplusplus
}
#endif
#endif
