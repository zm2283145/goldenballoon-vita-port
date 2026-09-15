#ifndef MDKR_ONLINE_ROOM_TAKEOVER_POLICY_H
#define MDKR_ONLINE_ROOM_TAKEOVER_POLICY_H

#include "online/match_live_adapter.h"

// Read the same narrow adapter boundary used by the shell. Do not service,
// dispatch, or inspect an uninitialized adapter to answer a presentation query.
// A failed composition cannot turn its untouched output into a known ENTRY.
inline MdkrOnlineViewKind OnlineRoom_readViewKind(
    const IMdkrOnlineAdapter *adapter, bool initialized) {
    if (adapter == nullptr || !initialized) {
        return static_cast<MdkrOnlineViewKind>(0);
    }
    MdkrOnlineViewModel model{};
    if (!adapter->view(&model)) {
        return static_cast<MdkrOnlineViewKind>(0);
    }
    return model.kind;
}

// A missing presentation is not a released session. Zero is the caller's
// sentinel for an unavailable view (including an uninitialized owned adapter).
// Only a known entry view may expose the ordinary shell while an adapter is
// owned. Releasing the adapter restores the shell regardless of the old view.
constexpr bool OnlineRoom_takeoverRequired(bool ownsAdapter,
                                          MdkrOnlineViewKind viewKind) {
    return ownsAdapter && viewKind != MDKR_ONLINE_VIEW_ENTRY;
}

#endif
