/* Focused proof for the net_roster ownership guard (the local-Play beach-ball
 * fix). Assert-driven, so NDEBUG must not compile the checks -- and the install
 * calls the asserts wrap -- away. Built + run standalone by
 * tests/check_net_roster_owner_guard.py; needs no CMake target.
 *
 * The guard's PURE decision (mdkr_net_roster_guard_decides_clear) lives in
 * net_roster_runtime.h and is exercised directly here. The mutable owner token
 * and the install/clear it drives live in the beta wiring layer
 * (platform/app/online_live_wiring.cpp, OnlineRoom_guardRosterOwner); this test
 * mirrors that tiny token state locally and drives the REAL net_roster
 * install/active/clear primitives, reproducing the exact beach-ball scenario:
 * an online roster is installed, then a local-Play boot must NOT inherit it. */
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

/* Local mirror of the beta wiring layer's owner state + guard, so the standalone
 * test drives the identical decision (mdkr_net_roster_guard_decides_clear) and
 * the identical net_roster primitives OnlineRoom_guardRosterOwner uses. */
static uint64_t g_owner;

static void set_owner(uint64_t token) {
    g_owner = mdkr_net_roster_runtime_active() ? token : 0u;
}
static uint64_t owner(void) {
    return mdkr_net_roster_runtime_active() ? g_owner : 0u;
}
static bool guard_owner(uint64_t token) {
    if (mdkr_net_roster_guard_decides_clear(mdkr_net_roster_runtime_active(),
                                            g_owner, token)) {
        mdkr_net_roster_runtime_clear();
        g_owner = 0u;
        return true;
    }
    return false;
}

int main(void) {
    /* --- The pure decision table (the code the beta guard actually runs). --- */
    assert(!mdkr_net_roster_guard_decides_clear(false, 0u, 0u));    /* nothing installed */
    assert(!mdkr_net_roster_guard_decides_clear(false, 7u, 0u));
    assert(mdkr_net_roster_guard_decides_clear(true, 7u, 0u));      /* local boot: clear any active */
    assert(mdkr_net_roster_guard_decides_clear(true, 7u, 9u));      /* different owner: clear */
    assert(!mdkr_net_roster_guard_decides_clear(true, 7u, 7u));     /* same owner: keep */

    /* --- The end-to-end beach-ball scenario over the REAL net_roster. --- */
    MdkrMatchManifestV1 spec = manifest();
    MdkrNetRoster online;
    const uint8_t seat0[] = {0u};
    assert(mdkr_net_roster_init(&online, &spec));
    assert(mdkr_net_roster_configure_local(&online, seat0, 1u));
    assert(mdkr_net_roster_set_viewports(&online, seat0, 1u));

    assert(!mdkr_net_roster_runtime_active());
    assert(owner() == 0u);

    /* An online session installs the process-global roster and tags ownership. */
    const uint64_t online_token = UINT64_C(0xABCDEF0123456789);
    assert(mdkr_net_roster_runtime_install(&spec, &online));
    set_owner(online_token);
    assert(mdkr_net_roster_runtime_active());
    assert(owner() == online_token);

    /* The online boot itself owns the roster and keeps it. */
    assert(!guard_owner(online_token));
    assert(mdkr_net_roster_runtime_active());

    /* THE FIX: a local-Play boot (token 0) force-clears the foreign online
     * roster BEFORE booting, so the engine cannot inherit it and stall waiting
     * for network input a local race never sends. */
    assert(guard_owner(0u));
    assert(!mdkr_net_roster_runtime_active());
    assert(owner() == 0u);

    /* Idempotent no-op once nothing is installed. */
    assert(!guard_owner(0u));
    assert(!mdkr_net_roster_runtime_active());

    /* A different session's boot also refuses to inherit a stale foreign roster. */
    assert(mdkr_net_roster_runtime_install(&spec, &online));
    set_owner(online_token);
    assert(guard_owner(UINT64_C(0x1111111111111111)));
    assert(!mdkr_net_roster_runtime_active());

    /* set_owner on an inactive roster never resurrects ownership. */
    set_owner(online_token);
    assert(owner() == 0u);

    /* A fresh online install after a guarded clear behaves like the first. */
    assert(mdkr_net_roster_runtime_install(&spec, &online));
    set_owner(online_token);
    assert(mdkr_net_roster_runtime_active() && owner() == online_token);
    mdkr_net_roster_runtime_clear();

    puts("test_net_roster_owner_guard: PASS");
    return 0;
}
