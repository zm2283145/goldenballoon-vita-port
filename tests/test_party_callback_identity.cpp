#include "party/party_callback_identity.h"

#include <cstdio>
#include <cstdlib>

namespace {
void require(bool condition, const char *name) {
    if (!condition) {
        std::fprintf(stderr, "party callback identity: %s\n", name);
        std::abort();
    }
}

// Deterministic pause/resume model: parsing and RTC construction run between
// capture and commit. The production transport applies these same predicates
// while holding its mutex; the source contract pins those commit bindings.
struct CommitModel {
    bool stopping = false;
    uint64_t generation = 4u;
    unsigned peer = 1u;
    bool initializing = false;
    uint64_t admissionGeneration = 4u;
    unsigned commits = 0u;

    bool commit(uint64_t capturedGeneration, unsigned capturedPeer = 0u) {
        const bool matches = capturedPeer == 0u ||
            (capturedPeer == peer && mdkr_party_peer_initialization_current(
                initializing, generation, admissionGeneration));
        if (!mdkr_party_callback_current(stopping, generation, capturedGeneration, matches))
            return false;
        ++commits;
        return true;
    }
};
} // namespace

int main() {
    CommitModel model;
    require(model.commit(4u), "current socket commits");
    const uint64_t enteredSocket = model.generation;
    ++model.generation; // onClosed invalidates before scheduling reconnect
    require(!model.commit(enteredSocket), "already-entered old socket cannot commit");
    require(model.commit(0u, 1u), "completed direct peer survives signaling close");
    ++model.generation; // next socket starts
    require(!model.commit(enteredSocket), "new socket does not revive old callback");
    require(model.commit(model.generation), "replacement socket commits");
    model.peer = 2u;
    require(!model.commit(0u, 1u), "retired direct peer cannot publish after replacement");
    require(model.commit(0u, 2u), "replacement direct peer commits");
    model.stopping = true;
    require(!model.commit(0u, 2u), "shutdown rejects direct callback");
    require(!model.commit(model.generation), "shutdown rejects signaling callback");
    require(model.commits == 4u, "rejected work never changes commit count");

    CommitModel pending;
    pending.initializing = true;
    require(pending.commit(0u, 1u), "current pending peer can send initial offer");
    ++pending.generation;
    require(!pending.commit(0u, 1u), "old pending peer cannot send into resumed socket");
    require(!mdkr_party_peer_initialization_current(pending.initializing,
                pending.generation, pending.admissionGeneration),
            "room update and hello must replace old pending placeholder");
    pending.peer = 2u;
    pending.admissionGeneration = pending.generation;
    require(pending.commit(0u, 2u), "fresh placeholder is admitted in resumed epoch");
    require(!pending.commit(0u, 1u), "old construction cannot reclaim replacement slot");
    pending.initializing = false;
    ++pending.generation;
    require(pending.commit(0u, 2u), "completed replacement survives later reconnect");
    require(mdkr_party_peer_initialization_current(true, 99u, 0u),
            "direct-peer recovery is not tied to a signaling callback");

    require(mdkr_party_retry_owner_current(true, true, true, true),
            "unchanged unanswered peer admits its queued retry");
    require(!mdkr_party_retry_owner_current(true, false, true, true),
            "replacement or removed peer rejects old retry");
    require(!mdkr_party_retry_owner_current(true, true, false, true),
            "authentication, failure, mismatch or give-up revokes retry");
    require(!mdkr_party_retry_owner_current(true, true, true, false),
            "resent offer or changed attempt count revokes old retry deadline");
    require(mdkr_party_retry_owner_current(false, false, false, false),
            "ordinary hello admission does not require retry identity");
    require(mdkr_party_ping_timeout_current(true, true), "unchanged expired ping disconnects");
    require(!mdkr_party_ping_timeout_current(true, false),
            "pong or a newer ping revokes queued expiry decision");
    require(mdkr_party_ping_timeout_current(false, false),
            "RTC failure callback does not depend on a pending ping");
    return 0;
}
