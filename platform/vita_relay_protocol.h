#ifndef MDKR_VITA_RELAY_PROTOCOL_H
#define MDKR_VITA_RELAY_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

#define MDKR_VITA_RELAY_HEADER_BYTES 16u
#define MDKR_VITA_RELAY_MAX_PAYLOAD 1024u
#define MDKR_VITA_RELAY_MAX_FRAME_BYTES \
    (MDKR_VITA_RELAY_HEADER_BYTES + MDKR_VITA_RELAY_MAX_PAYLOAD)
#define MDKR_VITA_RELAY_MAX_SLOTS 4u
#define MDKR_VITA_RELAY_CLIENT_SLOT 255u

typedef enum MdkrVitaRelayChannel {
    MDKR_VITA_RELAY_CHANNEL_SESSION = 1,
    MDKR_VITA_RELAY_CHANNEL_INPUT = 2,
    MDKR_VITA_RELAY_CHANNEL_CONTROL = 3,
    MDKR_VITA_RELAY_CHANNEL_REPAIR = 4
} MdkrVitaRelayChannel;

typedef struct MdkrVitaRelayFrameView {
    unsigned char senderSlot;
    unsigned char destinationMask;
    unsigned char channel;
    unsigned int sequence;
    const unsigned char *payload;
    unsigned int payloadLength;
} MdkrVitaRelayFrameView;

int mdkr_vita_relay_encode_client_frame(
    unsigned int localSlot, unsigned int destinationMask,
    unsigned int channel, unsigned int sequence,
    const unsigned char *payload, unsigned int payloadLength,
    unsigned char *output, unsigned int outputCapacity,
    unsigned int *outputLength);

int mdkr_vita_relay_decode_server_frame(
    const unsigned char *frame, unsigned int frameLength,
    unsigned int localSlot, unsigned int memberMask,
    const unsigned int lastSequence[MDKR_VITA_RELAY_MAX_SLOTS],
    MdkrVitaRelayFrameView *view);

MdkrVitaRelayChannel mdkr_vita_relay_channel_for_lan_packet(
    unsigned int packetType);

unsigned int mdkr_vita_relay_client_websocket_frame_bytes(
    unsigned int payloadLength);

#ifdef __cplusplus
}
#endif

#endif
