/* SEPARATED-BOOT-PATH online race boot.
 *
 * The direct online race-boot body lives here rather than in the shared,
 * battle-tested offline game/src/thread3_main.c, keeping that file free of online
 * code -- an isolation win. thread3_main.c's beta-OFF object is unaffected: none
 * of this boot body is present there in any build.
 *
 * The whole TU is #if MDKR_ENABLE_ONLINE_BETA and it is added to the build ONLY
 * inside the beta CMake gate (game/src/online/ is not globbed), so a normal (beta
 * OFF) build never compiles a byte of it and the release engine object is
 * untouched.
 */
#include "online/online_race_boot.h"

#if MDKR_ENABLE_ONLINE_BETA

/* Game/PR headers FIRST (same sprintf-ordering rationale as online_charselect.c /
 * thread3_main.c: a game header can declare sprintf()/fprintf() as a plain
 * function via PR/os_libc.h, which a preceding system <stdio.h> fortify macro
 * would break). */
#include "types.h"
#include "thread3_main.h" /* init_racer_headers, load_next_ingame_level,
                             get_level_default_vehicle, GAMEMODE_INGAME, Vehicle */
#include "menu.h"         /* menu_online_versus_race_setup */
#include "math_util.h"    /* set_rng_seed */
#include "structs.h"      /* CUTSCENE_NONE */
#include "net/net_roster_runtime.h" /* mdkr_net_roster_runtime_canonical_player_count */

#include <stdio.h>

/* The engine's live game-state globals. Defined (external linkage) in
 * thread3_main.c; no shared header declares them, so this TU declares the exact
 * globals it drives -- the same discipline online_session.c uses for gGameMode. */
extern s32 gGameMode;
extern s32 gGameCurrentEntrance;
extern s32 gGameCurrentCutscene;

/* Implementation notes (the contract lives in online_race_boot.h): the reused
 * tracks-mode versus race-start is assembled from the game's own helpers --
 * menu_online_versus_race_setup() lays down the same mode/track/count globals the
 * menu walk left behind, init_racer_headers() bakes the manifest's per-seat
 * characters into the racer table, and the ordinary in-game loader
 * (load_next_ingame_level -> load_level_game -> level_load) does the rest.
 * gGameCurrentCutscene stays CUTSCENE_NONE (0) so the launch-descriptor seam in
 * level_load() applies the manifest track/vehicle. */
void mdkr_online_boot_direct_race(
    const MdkrMatchLaunchDescriptorV1 *launch) {
    s32 canonicalPlayers = (s32) mdkr_net_roster_runtime_canonical_player_count(2u);
    s32 trackId = (s32) launch->manifest.track_id;

    if (canonicalPlayers < 1) {
        canonicalPlayers = 1;
    }

    fprintf(stderr, "[online-boot] direct race: track=%d players=%d\n", trackId,
            canonicalPlayers);

    /* Manifest RNG seed (hostile m1). The frozen manifest's rng_seed is
     * derived identically on both endpoints, but the engine otherwise boots
     * on the compile-time constant seed 'QAVM' (platform/math_util_native.c)
     * and nothing re-seeds it -- set_rng_seed()'s only other caller is
     * waves.c, bracketed by save_rng_seed()/load_rng_seed() -- so every
     * online race replayed one fixed item/AI random stream. Seeding here,
     * before the level loads, gives each race its own per-race item/RNG
     * variety while keeping both endpoints identical: each applies the same
     * fold at the same boot point and every authoritative draw afterwards is
     * lockstep (presentation randomness runs on its own separate stream).
     * Fold the u64 to the generator's 32-bit width by XOR of the halves.
     * Online-only by construction: this function runs only for a validated
     * online launch descriptor. */
    {
        u64 manifestSeed = launch->manifest.rng_seed;
        s32 foldedSeed = (s32) (u32) (manifestSeed ^ (manifestSeed >> 32));

        set_rng_seed(foldedSeed);
        fprintf(stderr, "[online-boot] rng seed applied: %08x\n",
                (unsigned) (u32) foldedSeed);
    }

    menu_online_versus_race_setup(trackId, canonicalPlayers);
    init_racer_headers();

    gGameCurrentEntrance = 0;
    gGameCurrentCutscene = CUTSCENE_NONE;
    gGameMode = GAMEMODE_INGAME;
    load_next_ingame_level(canonicalPlayers, -1, get_level_default_vehicle());
}

#endif /* MDKR_ENABLE_ONLINE_BETA */
