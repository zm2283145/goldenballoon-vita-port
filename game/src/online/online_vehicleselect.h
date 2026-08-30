#ifndef MDKR_ONLINE_VEHICLESELECT_H
#define MDKR_ONLINE_VEHICLESELECT_H

/* SEPARATED-BOOT-PATH (Strategy D2) native online VEHICLE stage of the track
 * screen.
 *
 * Retail 2P picks vehicles AFTER the track, as a stage of the track-select
 * screen; this screen is that stage for the native flow. The session enters it
 * from TRACKSELECT once the host's pick is locked (charselect -> track browse ->
 * lock -> THIS -> OK -> race) as its MDKR_ONLINE_SESSION_VEHICLESELECT phase,
 * NOT by the offline menu state machine: it deliberately does NOT call any
 * menu.c _loop. See the file header in online_vehicleselect.c for the full D2
 * reuse boundary (what game assets it borrows vs. what it owns).
 *
 * The crux is trivial: the party_link reverse feed already carries vehicle_id and
 * the reducer already validates CHOOSE_VEHICLE + ILLEGAL_VEHICLE refusal, so this
 * screen only drives intent.vehicle_id from a player cursor instead of the
 * auto-narrow default -- a new screen writing an existing field, zero new plumbing.
 *
 * The ENTIRE header is #if MDKR_ENABLE_ONLINE_BETA so a normal (beta OFF) build
 * sees nothing here, and the TU is compiled into the engine ONLY under the beta
 * CMake gate (game/src/online/ is NOT auto-globbed). Include is safe without the
 * macro: the body just vanishes.
 */
#if MDKR_ENABLE_ONLINE_BETA

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What one VEHICLESELECT tick tells the session to do next. */
typedef enum MdkrOnlineVehicleselectResult {
    MDKR_ONLINE_VEHICLESELECT_STAY = 0, /* keep showing the stage */
    MDKR_ONLINE_VEHICLESELECT_ADVANCE,  /* authoritative lobby left LOBBY: boot */
    MDKR_ONLINE_VEHICLESELECT_LEAVE     /* B: back to the track browse stage */
} MdkrOnlineVehicleselectResult;

/* Load the screen's borrowed game assets (portraits + fonts + sky are
 * process-global) and reset the local cursor/confirm state. Symmetric with
 * _exit(). Seeds the committed vehicle to a mask-legal default. */
void mdkr_online_vehicleselect_enter(void);

/* Free the borrowed portrait/sky assets (mirrors CHARSELECT). Safe to call more
 * than once. */
void mdkr_online_vehicleselect_exit(void);

/* One per-frame step: read the party_link snapshot, resolve the legal vehicle
 * mask for the LOCKED track/cup, apply local pad input to the pick / confirm /
 * host OK, publish the FULL local intent (continuously) with the always
 * mask-legal vehicle_id + the host's locked config + start_requested, and render
 * the retail setup composition. Returns whether the session should stay, advance
 * (boot) or leave (back to the track browse stage). */
MdkrOnlineVehicleselectResult mdkr_online_vehicleselect_tick(s32 updateRate);

/* True when the headless VEHICLESELECT test seam is armed (env
 * MDKR_TEST_ONLINE_VEHICLESELECT). Ordinary runs always return false. */
u8 mdkr_online_vehicleselect_test_active(void);

/* True once the LOCAL player has confirmed a (legal) vehicle on THIS stage (the
 * screen's own latch, not the lagging lobby snapshot). The stage is the LAST
 * selection stop in the retail order, so this is informational (the race start
 * is the reducer's BEGIN_LOADING). Resets to false on _enter(). */
u8 mdkr_online_vehicleselect_local_confirmed(void);

/* Headless test seam only (inert unless the env above is set): from LOBBY_WAIT,
 * install the party_link forward feed if nothing else has (the CHARSELECT seam
 * normally owns install in this lane). No-op in a normal run, so it never
 * disturbs the direct-boot / session-boot lanes. */
void mdkr_online_vehicleselect_test_lobby_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_ENABLE_ONLINE_BETA */
#endif /* MDKR_ONLINE_VEHICLESELECT_H */
