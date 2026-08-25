/* Focused proof for the net_roster ownership guard (the local-Play beach-ball
 * fix). Assert-driven, so NDEBUG must not compile the checks -- and the install
 * calls the asserts wrap -- away. Built + run standalone by
 * tests/check_net_roster_owner_guard.py; needs no CMake target. */
#undef NDEBUG

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "platform/net/net_roster.h"
#include "platform/net/net_roster_runtime.h"

static MdkrMatchManifestV1 manifest(void) {
    MdkrMatchManifestV1 value;
    memset(&value, 0, sizeof(value));
    value.match_epoch = 1u;
    value.protocol_version = 1u;
    memset(value.build_id, 1, sizeof(value.build_id));
    memset(value.gameplay_digest, 2, sizeof(value.gameplay_digest));
    for (unsigned slot = 0u; slot < 4u; slot++) value.slot_owner[slot] = 100u + slot;
    value.rng_seed = 3u;
    value.track_id = 1u;
    value.rom_revision = MDKR_ROM_US_11;
    value.cadence_hz = 30u;
    value.slot_count = 4u;
    value.rules = 1u;
    value.vehicle_mask = 1u;
    value.input_delay = 2u;
    return value;
}

int main(void) {
    MdkrMatchManifestV1 spec = manifest();
    MdkrNetRoster online;
    const uint8_t seat0[] = {0u};

    assert(mdkr_net_roster_init(&online, &spec));
    assert(mdkr_net_roster_configure_local(&online, seat0, 1u));
    assert(mdkr_net_roster_set_viewports(&online, seat0, 1u));

    /* Baseline: nothing installed, unowned. */
    assert(!mdkr_net_roster_runtime_active());
    assert(mdkr_net_roster_runtime_owner() == 0u);

    /* An online session installs the process-global roster and tags ownership. */
    const uint64_t online_token = UINT64_C(0xABCDEF0123456789);
    assert(mdkr_net_roster_runtime_install(&spec, &online));
    mdkr_net_roster_runtime_set_owner(online_token);
    assert(mdkr_net_roster_runtime_active());
    assert(mdkr_net_roster_runtime_owner() == online_token);

    /* A boot that OWNS the installed roster keeps it (the online boot itself). */
    assert(!mdkr_net_roster_runtime_guard_owner(online_token));
    assert(mdkr_net_roster_runtime_active());
    assert(mdkr_net_roster_runtime_owner() == online_token);

    /* THE BEACH-BALL FIX: a local-Play boot (owner token 0 == "installs no
     * roster") must force-clear the foreign online roster BEFORE it boots, so
     * the engine cannot inherit it, flip into online-race mode, and stall
     * forever waiting for network input a local race never sends. */
    assert(mdkr_net_roster_runtime_guard_owner(0u));
    assert(!mdkr_net_roster_runtime_active());
    assert(mdkr_net_roster_runtime_owner() == 0u);

    /* Idempotent: guarding with no roster active is a safe no-op. */
    assert(!mdkr_net_roster_runtime_guard_owner(0u));
    assert(!mdkr_net_roster_runtime_active());

    /* A DIFFERENT session's boot also refuses to inherit a stale foreign roster
     * (ownership is explicit, not merely "any roster is fine"). */
    assert(mdkr_net_roster_runtime_install(&spec, &online));
    mdkr_net_roster_runtime_set_owner(online_token);
    assert(mdkr_net_roster_runtime_guard_owner(UINT64_C(0x1111111111111111)));
    assert(!mdkr_net_roster_runtime_active());

    /* set_owner on an inactive roster never resurrects ownership. */
    mdkr_net_roster_runtime_set_owner(online_token);
    assert(mdkr_net_roster_runtime_owner() == 0u);

    /* A fresh online install after a guarded clear behaves exactly as the first:
     * the guard leaves no residual state that would refuse a legitimate boot. */
    assert(mdkr_net_roster_runtime_install(&spec, &online));
    mdkr_net_roster_runtime_set_owner(online_token);
    assert(mdkr_net_roster_runtime_active());
    assert(mdkr_net_roster_runtime_owner() == online_token);
    mdkr_net_roster_runtime_clear();
    assert(!mdkr_net_roster_runtime_active());

    puts("test_net_roster_owner_guard: PASS");
    return 0;
}
