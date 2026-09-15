/* Assert-driven test: release builds must not compile its checks away. */
#undef NDEBUG

#include "online_room_takeover_policy.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

constexpr MdkrOnlineViewKind unavailable = static_cast<MdkrOnlineViewKind>(0);

// Compile-time checks exercise the production policy without running a test,
// window, adapter, transport, or renderer. Runtime shell/leave qualification is
// separately owned by check_online_lobby_takeover and native acceptance.
static_assert(!OnlineRoom_takeoverRequired(false, unavailable), "idle shell");
static_assert(!OnlineRoom_takeoverRequired(false, MDKR_ONLINE_VIEW_RACING),
              "released ownership overrides stale view");
static_assert(!OnlineRoom_takeoverRequired(true, MDKR_ONLINE_VIEW_ENTRY),
              "known chooser retains shell");
static_assert(OnlineRoom_takeoverRequired(true, unavailable),
              "unavailable or uninitialized owned session retains safe exit");

constexpr bool everyActiveViewTakesOver() {
    for (int kind = MDKR_ONLINE_VIEW_CONNECTING;
         kind <= MDKR_ONLINE_VIEW_RECOVERY; ++kind) {
        const auto view = static_cast<MdkrOnlineViewKind>(kind);
        if (!OnlineRoom_takeoverRequired(true, view) ||
            OnlineRoom_takeoverRequired(false, view)) return false;
    }
    return true;
}
static_assert(everyActiveViewTakesOver(), "all active states and released states");

namespace {

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "online room takeover: %s\n", message);
        std::exit(1);
    }
}

// A deterministic implementation of the actual launcher adapter interface,
// not a replacement policy. Never constructs a service, mesh, or native UI.
class ViewAdapter final : public IMdkrOnlineAdapter {
public:
    explicit ViewAdapter(unsigned &destructions) : destructions_(destructions) {}
    ~ViewAdapter() override { ++destructions_; }
    MdkrOnlineViewKind kind = MDKR_ONLINE_VIEW_ENTRY;
    bool viewAvailable = true;
    bool failAfterService = false;
    mutable unsigned views = 0u;
    unsigned services = 0u;
    unsigned submissions = 0u;

    bool view(MdkrOnlineViewModel *out) const override {
        ++views;
        if (!viewAvailable) return false; // fail-atomic, like the real seam
        *out = {};
        out->kind = kind;
        return true;
    }
    void service() override {
        ++services;
        if (failAfterService) viewAvailable = false;
    }
    MdkrOnlineAdapterStep submit(const MdkrOnlineAdapterCommand &) override {
        ++submissions;
        return {};
    }
    MdkrOnlineJourney journey() const override { return MDKR_ONLINE_JOURNEY_CREATE; }
    uint32_t revision() const override { return 1u; }
    MdkrPlayIntent sessionIntent() const override { return MDKR_INTENT_NONE; }
    bool raceAdmissionEnabled() const override { return false; }
    bool timeoutExpired() const override { return false; }

private:
    unsigned &destructions_;
};

void testAdapterObservationAndRelease() {
    unsigned destructions = 0u;
    auto adapter = std::make_unique<ViewAdapter>(destructions);
    expect(OnlineRoom_readViewKind(nullptr, true) == unavailable,
           "missing adapter must yield unavailable");
    expect(OnlineRoom_readViewKind(adapter.get(), false) == unavailable,
           "uninitialized owned adapter must yield unavailable");
    expect(adapter->views == 0u, "uninitialized adapter must not be queried");
    expect(OnlineRoom_takeoverRequired(true,
               OnlineRoom_readViewKind(adapter.get(), false)),
           "uninitialized ownership must retain safe exit");
    expect(!OnlineRoom_takeoverRequired(true,
               OnlineRoom_readViewKind(adapter.get(), true)),
           "valid entry keeps the ordinary shell");

    for (int kind = MDKR_ONLINE_VIEW_CONNECTING;
         kind <= MDKR_ONLINE_VIEW_RECOVERY; ++kind) {
        adapter->kind = static_cast<MdkrOnlineViewKind>(kind);
        expect(OnlineRoom_readViewKind(adapter.get(), true) == adapter->kind,
               "successful view must preserve the exact kind");
        expect(OnlineRoom_takeoverRequired(true,
                   OnlineRoom_readViewKind(adapter.get(), true)),
               "active adapter view must take over");
    }
    expect(adapter->services == 0u && adapter->submissions == 0u,
           "shell observation must not service or submit");

    adapter->viewAvailable = false;
    adapter->kind = MDKR_ONLINE_VIEW_ENTRY;
    expect(OnlineRoom_readViewKind(adapter.get(), true) == unavailable,
           "failed composition must not reuse an earlier entry view");
    expect(OnlineRoom_takeoverRequired(true,
               OnlineRoom_readViewKind(adapter.get(), true)),
           "failure at frame start must retain takeover");

    adapter->viewAvailable = true;
    adapter->kind = MDKR_ONLINE_VIEW_ROOM;
    adapter->failAfterService = true;
    expect(OnlineRoom_readViewKind(adapter.get(), true) == MDKR_ONLINE_VIEW_ROOM,
           "pre-service room observation");
    adapter->service();
    expect(OnlineRoom_readViewKind(adapter.get(), true) == unavailable,
           "post-service failure must discard the prior room observation");
    expect(OnlineRoom_takeoverRequired(true,
               OnlineRoom_readViewKind(adapter.get(), true)),
           "post-service failure must retain takeover");
    expect(adapter->services == 1u && adapter->submissions == 0u,
           "observation must add no state-changing calls");

    // Test ownership semantics, not the production mesh/registry teardown.
    adapter.reset();
    expect(destructions == 1u, "test owner must release exactly once");
    expect(!OnlineRoom_takeoverRequired(adapter != nullptr,
               OnlineRoom_readViewKind(adapter.get(), true)),
           "released adapter must restore shell");
    adapter = std::make_unique<ViewAdapter>(destructions);
    expect(!OnlineRoom_takeoverRequired(true,
               OnlineRoom_readViewKind(adapter.get(), true)),
           "fresh entry must not inherit failed view state");
}

} // namespace

int main() {
    testAdapterObservationAndRelease();
    return 0;
}
