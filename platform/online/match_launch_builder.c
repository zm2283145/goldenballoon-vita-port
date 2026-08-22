#include "match_launch_builder.h"

#include <string.h>

_Static_assert(MDKR_ONLINE_CHARACTER_COUNT == MDKR_MATCH_CHARACTER_COUNT,
               "lobby and launch character catalogs must agree");
_Static_assert(
    MDKR_ONLINE_PLAYER_VEHICLE_COUNT == MDKR_MATCH_PLAYER_VEHICLE_COUNT,
    "lobby and launch vehicle catalogs must agree");

static bool mdkr_match_launch_refuse(
    MdkrMatchLaunchRefusal reason, MdkrMatchLaunchRefusal *refusal) {
    if (refusal != NULL) *refusal = reason;
    return false;
}

bool mdkr_match_launch_descriptor_from_lobby(
    const MdkrOnlineLobby *lobby, const MdkrMatchManifestV1 *manifest,
    const MdkrMatchLocalRosterV1 *local_roster,
    MdkrMatchLaunchDescriptorV1 *output, MdkrMatchLaunchRefusal *refusal) {
    MdkrMatchLaunchDescriptorV1 next;
    unsigned source;
    unsigned target = 0u;
    if (lobby == NULL || manifest == NULL || output == NULL ||
        local_roster == NULL ||
        !mdkr_online_lobby_valid(lobby) ||
        lobby->phase != MDKR_ONLINE_LOADING ||
        lobby->match_epoch != manifest->match_epoch ||
        lobby->selected_track != manifest->track_id ||
        lobby->selected_vehicle_mask != manifest->vehicle_mask ||
        lobby->seat_count != manifest->slot_count) {
        return mdkr_match_launch_refuse(
            MDKR_MATCH_LAUNCH_REFUSE_SNAPSHOT, refusal);
    }
    /* Retail-identity clamp (online v1). The manifest carries no roster
     * identity, so a bonus racer would exist only in this machine's local
     * roster and desynchronise the peers at tick zero. Any local player slot
     * that would resolve non-retail refuses admission here, at the Loading
     * barrier, before a level load can reach taj_mod_begin_racer_bindings().
     * Every slot is checked -- not just seated ones -- because the engine's
     * binding pass maps selected local slots onto live racers independently
     * of lobby seat order. A manifest v2 may bind identities instead. */
    for (source = 0u; source < MDKR_MATCH_LOCAL_PLAYER_SLOTS; source++) {
        if (local_roster->player_identity[source] !=
            MDKR_MATCH_IDENTITY_RETAIL) {
            return mdkr_match_launch_refuse(
                MDKR_MATCH_LAUNCH_REFUSE_NON_RETAIL_IDENTITY, refusal);
        }
    }
    memset(&next, 0, sizeof(next));
    next.version = MDKR_MATCH_LAUNCH_DESCRIPTOR_VERSION;
    next.manifest = *manifest;
    for (source = 0u; source < MDKR_ONLINE_MAX_SEATS; source++) {
        const MdkrOnlineSeat *seat = &lobby->seats[source];
        if (!seat->occupied) continue;
        if (target >= MDKR_MATCH_SLOTS) {
            return mdkr_match_launch_refuse(
                MDKR_MATCH_LAUNCH_REFUSE_SELECTIONS, refusal);
        }
        next.selections[target].selection_revision = seat->selection_revision;
        next.selections[target].character_id = seat->character_id;
        next.selections[target].vehicle_id = seat->vehicle_id;
        target++;
    }
    for (; target < MDKR_MATCH_SLOTS; target++) {
        next.selections[target].character_id = MDKR_MATCH_NO_CHARACTER;
        next.selections[target].vehicle_id = MDKR_MATCH_NO_VEHICLE;
    }
    if (!mdkr_match_launch_descriptor_validate(&next)) {
        return mdkr_match_launch_refuse(
            MDKR_MATCH_LAUNCH_REFUSE_SELECTIONS, refusal);
    }
    *output = next;
    if (refusal != NULL) *refusal = MDKR_MATCH_LAUNCH_ADMITTED;
    return true;
}
