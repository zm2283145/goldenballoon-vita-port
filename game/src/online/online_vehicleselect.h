#ifndef MDKR_ONLINE_VEHICLESELECT_H
#define MDKR_ONLINE_VEHICLESELECT_H

/* SEPARATED-BOOT-PATH (Strategy D2) native online VEHICLE select.
 *
 * The player-facing screen inserted between the native CHARSELECT
 * (online_charselect.{c,h}) and the native TRACKSELECT (online_trackselect.{c,h}):
 * the player CHOOSES car / hovercraft / plane. Before this screen existed the
 * native flow only auto-narrowed a default vehicle to the resolved track's legal
 * mask -- the player never picked. It is driven by the online session
 * (game/src/online/online_session.c) as its MDKR_ONLINE_SESSION_VEHICLESELECT
 * phase, NOT by the offline menu state machine: it deliberately does NOT call any
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
    MDKR_ONLINE_VEHICLESELECT_STAY = 0, /* keep showing the screen */
    MDKR_ONLINE_VEHICLESELECT_ADVANCE,  /* authoritative lobby left LOBBY: boot */
    MDKR_ONLINE_VEHICLESELECT_LEAVE     /* B: back one level, to CHARSELECT */
} MdkrOnlineVehicleselectResult;

/* Load the screen's borrowed game assets (portraits + fonts + sky are
 * process-global) and reset the local cursor/confirm state. Symmetric with
 * _exit(). Seeds the committed vehicle to a mask-legal default. */
void mdkr_online_vehicleselect_enter(void);

/* Free the borrowed portrait/sky assets (mirrors CHARSELECT). Safe to call more
 * than once. */
void mdkr_online_vehicleselect_exit(void);

/* One per-frame step: read the party_link snapshot, resolve the legal vehicle
 * mask for the resolved track, apply local pad input to the cursor / confirm,
 * publish the FULL local intent (continuously) with the chosen (always mask-legal)
 * vehicle_id, and render the native screen. Returns whether the session should
 * stay, advance (boot) or leave (back to CHARSELECT). */
MdkrOnlineVehicleselectResult mdkr_online_vehicleselect_tick(s32 updateRate);

/* True when the headless VEHICLESELECT test seam is armed (env
 * MDKR_TEST_ONLINE_VEHICLESELECT). Ordinary runs always return false. */
u8 mdkr_online_vehicleselect_test_active(void);

/* True once the LOCAL player has confirmed a (legal) vehicle on THIS screen (the
 * screen's own latch, not the lagging lobby snapshot). The session gates the
 * VEHICLESELECT -> TRACKSELECT hand-off on this so a back-out cannot one-frame
 * bounce forward before the player re-confirms. Resets to false on _enter(). */
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
