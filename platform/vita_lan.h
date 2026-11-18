#ifndef MDKR_VITA_LAN_H
#define MDKR_VITA_LAN_H

#if defined(__vita__) && defined(MDKR_ENABLE_VITA_LAN)

#include "net/match_launch_descriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MdkrVitaLanState {
    MDKR_VITA_LAN_IDLE = 0,
    MDKR_VITA_LAN_NAMING,
    MDKR_VITA_LAN_BROWSING,
    MDKR_VITA_LAN_HOSTING,
    MDKR_VITA_LAN_JOINING,
    MDKR_VITA_LAN_CONNECTED,
    MDKR_VITA_LAN_HANDSHAKING,
    MDKR_VITA_LAN_CHARACTER_SELECT,
    MDKR_VITA_LAN_STARTING,
    MDKR_VITA_LAN_RACING,
    MDKR_VITA_LAN_ERROR
} MdkrVitaLanState;

typedef enum MdkrVitaLanFailure {
    MDKR_VITA_LAN_FAILURE_NONE = 0,
    MDKR_VITA_LAN_FAILURE_HOST_LEFT,
    MDKR_VITA_LAN_FAILURE_PEER_DISCONNECTED,
    MDKR_VITA_LAN_FAILURE_VERSION_MISMATCH,
    MDKR_VITA_LAN_FAILURE_SYNCHRONIZATION,
    MDKR_VITA_LAN_FAILURE_STARTUP,
    MDKR_VITA_LAN_FAILURE_GENERIC
} MdkrVitaLanFailure;

typedef enum MdkrVitaLanControl {
    MDKR_VITA_LAN_CONTROL_NONE = 0,
    MDKR_VITA_LAN_CONTROL_RESTART,
    MDKR_VITA_LAN_CONTROL_SELECT_TRACK,
    MDKR_VITA_LAN_CONTROL_QUIT
} MdkrVitaLanControl;

typedef enum MdkrVitaLanFrontendStage {
    MDKR_VITA_LAN_FRONTEND_CHARACTER_SELECT = 0,
    MDKR_VITA_LAN_FRONTEND_GAME_SELECT = 1,
    MDKR_VITA_LAN_FRONTEND_TRACK_SELECT = 2
} MdkrVitaLanFrontendStage;

#define MDKR_VITA_LAN_UNLOCK_DRUMSTICK (1u << 0)
#define MDKR_VITA_LAN_UNLOCK_TT        (1u << 1)

int mdkr_vita_lan_start_browser(void);
int mdkr_vita_lan_start_host(void);
int mdkr_vita_lan_join_room(unsigned int index);
#if defined(MDKR_ENABLE_VITA_RELAY)
int mdkr_vita_lan_start_relay(void);
int mdkr_vita_lan_is_relay(void);
#endif
void mdkr_vita_lan_update(void);
int mdkr_vita_lan_should_stall_race(void);
void mdkr_vita_lan_stop(void);
MdkrVitaLanState mdkr_vita_lan_state(void);
int mdkr_vita_lan_is_host(void);
unsigned int mdkr_vita_lan_room_count(void);
const char *mdkr_vita_lan_status(void);
const char *mdkr_vita_lan_room_label(unsigned int index);
int mdkr_vita_lan_take_race_launch(MdkrMatchLaunchDescriptorV1 *launch);
int mdkr_vita_lan_character_selection(
    unsigned int *localSlot, unsigned char characters[2],
    unsigned char confirmed[2]);
int mdkr_vita_lan_set_character_selection(unsigned int character,
                                          int confirmed);
int mdkr_vita_lan_frontend_state(unsigned int *stage,
                                 unsigned int *selection);
int mdkr_vita_lan_set_frontend_state(unsigned int stage,
                                     unsigned int selection);
int mdkr_vita_lan_character_unlocks(unsigned int *unlockMask);
int mdkr_vita_lan_start_selected_race(unsigned int trackId,
                                      unsigned int vehicleMask);
void mdkr_vita_lan_report_failure(MdkrVitaLanFailure failure);
void mdkr_vita_lan_report_failure_detail(
    MdkrVitaLanFailure failure, const char *detail);
int mdkr_vita_lan_take_failure(MdkrVitaLanFailure *failure,
                               const char **message);
int mdkr_vita_lan_request_control(MdkrVitaLanControl control,
                                  unsigned int trackId,
                                  unsigned int vehicleMask);
int mdkr_vita_lan_take_control(MdkrVitaLanControl *control,
                               unsigned int *trackId,
                               unsigned int *vehicleMask,
                               unsigned int *matchEpoch);
int mdkr_vita_lan_restart_race(unsigned int trackId,
                               unsigned int vehicleMask,
                               unsigned int matchEpoch,
                               MdkrMatchLaunchDescriptorV1 *launch);
unsigned int mdkr_vita_lan_finish_target_tick(void);
int mdkr_vita_lan_finish_barrier_ready(unsigned int confirmedTick);
void mdkr_vita_lan_set_track_picker(int active, unsigned int trackId);
int mdkr_vita_lan_track_picker(unsigned int *trackId);

#ifdef __cplusplus
}
#endif

#endif

#endif
