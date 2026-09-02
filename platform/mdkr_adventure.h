#ifndef MDKR_ADVENTURE_H
#define MDKR_ADVENTURE_H

#include "structs.h"

void mdkr_adventure_drive(Object *obj, Object_Racer *racer, s32 updateRate);
void mdkr_autopilot_unstick(Object *obj, Object_Racer *racer, s32 updateRate);
void mdkr_cpu_unstick(Object *obj, Object_Racer *racer, s32 updateRate);
void mdkr_adventure_force_verdict(Object *humanObj, Object **racers,
                                  s32 numRacers);
void mdkr_trophy_control_order(Settings *settings, s32 round, s32 racerCount);
void mdkr_trophy_control_menu_option(s32 completedRound, s32 *menuOption);
void mdkr_trophy_control_world(Settings *settings);
void mdkr_trophy_control_collision(Object *cabinet, Object *player);
void mdkr_trophy_complete_race(Object **racers, s32 racerCount,
                               s32 requiredLaps);

/* AP-10 test injector: 1 if seat's controller should read ABSENT now
 * (MDKR_AP_DROP_PAD). No-op (returns 0) with no env set; always defined. */
int mdkr_test_pad_absent(int seat);

/* AP-12 test injector: force which racer finishes first in a multi-human party
 * race, so the race-loop gate can drive a host win, a non-host-human win, and a
 * CPU win through the same return machinery. MDKR_AP_RACE_WINNER="<seat>" forces
 * the human on that controller port to first; "cpu" forces a computer racer.
 * No-op with no env set. It waits until the port-1 human genuinely finishes,
 * then test-only code can mark the requested unfinished target as finished and
 * reassign finish positions. It therefore selects a deterministic verdict
 * without steering the live race. humanCount is the number of party humans
 * (racerIndex 0..humanCount-1); CPUs are racerIndex >= humanCount. Racers are
 * matched by stable racerIndex, not playerIndex, because a finished human is
 * flipped to PLAYER_COMPUTER by update_player_racer. */
/* TRUE only on the last cycle of a REPEATING MDKR_DRIVE_ROUTE: a route that
 * orders two or more door entries and has no step left on any level leg --
 * i.e. it has made its last ordered move of a loop it drove more than once.
 * Read-only; FALSE with no route set, and FALSE for a single-entry route at
 * any point. Lets a closed-loop fixture tell its LAST cycle from every earlier
 * one without knowing a frame number. */
int mdkr_adventure_route_cycles_exhausted(void);

void mdkr_ap_force_race_winner(Object **racers, s32 numRacers, s32 humanCount);

#endif
