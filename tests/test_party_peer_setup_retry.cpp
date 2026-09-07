#include "party/party_peer_setup_retry.h"

#include <cstdio>
#include <cstdlib>

namespace {
void require(bool value, const char *name) {
    if (!value) {
        std::fprintf(stderr, "party setup retry: %s\n", name);
        std::abort();
    }
}
} // namespace

int main() {
    MdkrPartyPeerSetupRetry state;
    require(!mdkr_party_setup_due(state, 100u), "fresh controller does not busy retry");
    const auto first = mdkr_party_setup_begin(state, 100u, 1u);
    require(first == 1u && state.active, "initial attempt owns token before allocation");
    require(mdkr_party_setup_begin(state, 101u, 2u) == 0u, "duplicate hello cannot overlap setup");
    mdkr_party_setup_failed(state, first, 100u);
    require(state.failures == 1u && state.retryAtMs == 400u, "first failure backs off 300ms");
    mdkr_party_setup_failed(state, first, 101u);
    require(state.failures == 1u, "duplicate failure completion costs no extra attempt");
    require(!mdkr_party_setup_due(state, 399u), "no per-frame retry before deadline");
    require(mdkr_party_setup_begin(state, 399u, 2u) == 0u, "hello cannot bypass backoff");
    require(mdkr_party_setup_due(state, 400u), "retry needs no additional room or hello");
    const auto second = mdkr_party_setup_begin(state, 400u, 2u);
    mdkr_party_setup_failed(state, second, 400u);
    require(state.failures == 2u && state.retryAtMs == 1000u, "second failure backs off 600ms");
    require(mdkr_party_setup_begin(state, 999u, 3u) == 0u, "second backoff remains bounded");
    const auto third = mdkr_party_setup_begin(state, 1000u, 3u);
    mdkr_party_setup_failed(state, third, 1000u);
    require(state.exhausted && state.failures == 3u && state.retryAtMs == 0u,
            "third failure exhausts rather than starting a busy loop");
    require(!state.exhaustionReported, "exhaustion report is pending until successfully queued");
    require(!mdkr_party_setup_due(state, 90000u), "exhausted setup never retries itself");
    require(mdkr_party_setup_begin(state, 90000u, 4u) == 0u, "hello cannot reset exhausted lifecycle");
    mdkr_party_setup_reauthorize(state, 90000u);
    require(state.exhausted && state.failures == 3u, "signaling reconnect preserves exhaustion");

    MdkrPartyPeerSetupRetry recover;
    recover.offerAttempts = 2u;
    const auto interrupted = mdkr_party_setup_begin(recover, 10u, 10u);
    mdkr_party_setup_reauthorize(recover, 20u);
    require(!recover.active && recover.failures == 0u && recover.retryAtMs == 320u,
            "new signaling epoch cancels old reservation without inventing failure");
    mdkr_party_setup_failed(recover, interrupted, 30u);
    require(recover.failures == 0u, "old epoch completion cannot charge new admission");
    const auto current = mdkr_party_setup_begin(recover, 320u, 11u);
    mdkr_party_setup_failed(recover, current, 320u);
    const auto successful = mdkr_party_setup_begin(recover, 620u, 12u);
    mdkr_party_setup_succeeded(recover, successful);
    require(!recover.active && !mdkr_party_setup_due(recover, 10000u), "successful setup cancels timer");
    require(recover.failures == 1u && recover.offerAttempts == 2u,
            "success preserves lifecycle failure and unanswered-offer budgets");
    mdkr_party_setup_failed(recover, successful, 700u);
    require(recover.failures == 1u, "late duplicate failure cannot undo success");

    // Only a fresh lifecycle resets the setup-failure budget. Production may
    // separately begin a new unanswered-offer episode after an authenticated
    // peer disconnects; mere successful construction above is not authentication.
    // Transport-wide serials prevent ABA when the same controller tuple returns.
    MdkrPartyPeerSetupRetry fresh;
    const auto freshToken = mdkr_party_setup_begin(fresh, 1000u, 13u);
    mdkr_party_setup_failed(fresh, interrupted, 1001u);
    require(fresh.active && fresh.failures == 0u && fresh.offerAttempts == 0u,
            "retired lifecycle token cannot mutate fresh controller");
    mdkr_party_setup_succeeded(fresh, freshToken);
    require(!fresh.active, "fresh lifecycle can succeed after old exhaustion");
    MdkrPartyPeerSetupRetry finalOffer;
    finalOffer.offerAttempts = 2u;
    const auto last = mdkr_party_setup_begin(finalOffer, 1000u, 14u);
    finalOffer.offerAttempts = 3u; // emitted before a later registration failure
    mdkr_party_setup_failed(finalOffer, last, 1000u);
    require(finalOffer.exhausted && finalOffer.failures == 1u,
            "late setup failure cannot grant a fourth distinct offer");
    require(mdkr_party_setup_begin(finalOffer, 2000u, 15u) == 0u,
            "exhausted offer budget stops autonomous and hello retry");
    require(mdkr_party_setup_after(UINT64_MAX - 10u, 300u) == UINT64_MAX,
            "deadline arithmetic saturates instead of wrapping to immediate retry");
    return 0;
}
