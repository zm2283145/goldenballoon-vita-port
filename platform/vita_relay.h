#ifndef MDKR_VITA_RELAY_H
#define MDKR_VITA_RELAY_H

#if defined(__vita__) && defined(MDKR_ENABLE_VITA_RELAY)

#include "vita_relay_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH 16u

typedef enum MdkrVitaRelayState {
    MDKR_VITA_RELAY_IDLE = 0,
    MDKR_VITA_RELAY_CONNECTING,
    MDKR_VITA_RELAY_BROWSING,
    MDKR_VITA_RELAY_NAMING,
    MDKR_VITA_RELAY_ADMITTING,
    MDKR_VITA_RELAY_LOBBY,
    MDKR_VITA_RELAY_ERROR
} MdkrVitaRelayState;

int mdkr_vita_relay_start_browser(void);
int mdkr_vita_relay_start_host(void);
int mdkr_vita_relay_join_room(unsigned int index);
void mdkr_vita_relay_update(void);
void mdkr_vita_relay_stop(void);
MdkrVitaRelayState mdkr_vita_relay_state(void);
unsigned int mdkr_vita_relay_room_count(void);
unsigned int mdkr_vita_relay_member_count(void);
unsigned int mdkr_vita_relay_member_mask(void);
int mdkr_vita_relay_local_slot(void);
int mdkr_vita_relay_is_host(void);
int mdkr_vita_relay_synchronization_lost(void);
const char *mdkr_vita_relay_room_code(void);
int mdkr_vita_relay_send(unsigned int destinationMask,
                         MdkrVitaRelayChannel channel,
                         const void *payload, unsigned int payloadLength);
int mdkr_vita_relay_send_ready(unsigned int payloadLength);
int mdkr_vita_relay_receive(unsigned int *senderSlot,
                            MdkrVitaRelayChannel *channel,
                            void *payload, unsigned int payloadCapacity,
                            unsigned int *payloadLength);
const char *mdkr_vita_relay_status(void);
void mdkr_vita_relay_format_diagnostics(char *output,
                                        unsigned int outputCapacity);
const char *mdkr_vita_relay_room_label(unsigned int index);

#ifdef __cplusplus
}
#endif

#endif

#endif
