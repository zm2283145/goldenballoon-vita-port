#ifndef MDKR_ONLINE_RACE_BOOT_H
#define MDKR_ONLINE_RACE_BOOT_H

/* SEPARATED-BOOT-PATH (Strategy D) online race boot.
 *
 * PD-T4 extraction: the direct online race-boot body used to live in the shared,
 * vendored game/src/thread3_main.c, entirely inside a single #if
 * MDKR_ENABLE_ONLINE_BETA. It is relocated here so the battle-tested OFFLINE file
 * carries no online code at all -- a net isolation win. Because the preprocessor
 * emitted NOTHING for that block in a beta-OFF build, deleting it leaves the
 * release engine's thread3_main.o byte-identical.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and the TU is compiled into the engine ONLY under the beta
 * CMake gate (game/src/online/ is NOT auto-globbed). Include is safe without the
 * macro: the body just vanishes.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "types.h"
#include "net/match_launch_descriptor.h" /* MdkrMatchLaunchDescriptorV1 */

#ifdef __cplusplus
extern "C" {
#endif

/* Boot straight into the online race described by the installed launch
 * descriptor, bypassing the entire single-player front-end (title screen, Wizpig
 * hub, Adventure/Time-Trial select, tracks menu, in-game character select). It
 * reuses the game's OWN tracks-mode versus race start rather than re-implementing
 * it, so no race-setup logic is duplicated, and sets gGameMode = GAMEMODE_INGAME.
 *
 * It loads launch->manifest.track_id -- the peer rollback-admission authority
 * (the launcher froze the manifest from the CONVERGED lobby at the LOADING
 * barrier) -- and NEVER a snapshot-derived track: a peer whose manifest differed
 * would reject the race. Called by the separated online session
 * (online_session.c). */
void mdkr_online_boot_direct_race(const MdkrMatchLaunchDescriptorV1 *launch);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_RACE_BOOT_H */
