#include "vita_relay_protocol.h"

#include <string.h>

static const unsigned char s_relayMagic[4] = { 'D', 'K', 'R', 'R' };

static void store_u32_be(unsigned char *output, unsigned int value) {
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

static unsigned int load_u32_be(const unsigned char *input) {
    return ((unsigned int)input[0] << 24) |
           ((unsigned int)input[1] << 16) |
           ((unsigned int)input[2] << 8) | input[3];
}

static int channel_valid(unsigned int channel) {
    return channel >= MDKR_VITA_RELAY_CHANNEL_SESSION &&
           channel <= MDKR_VITA_RELAY_CHANNEL_REPAIR;
}

MdkrVitaRelayChannel mdkr_vita_relay_channel_for_lan_packet(
    unsigned int packetType) {
    if (packetType == 12u) {
        return MDKR_VITA_RELAY_CHANNEL_INPUT;
    }
    if (packetType == 15u) {
        return MDKR_VITA_RELAY_CHANNEL_REPAIR;
    }
    if (packetType == 13u || packetType == 14u) {
        return MDKR_VITA_RELAY_CHANNEL_CONTROL;
    }
    return MDKR_VITA_RELAY_CHANNEL_SESSION;
}

unsigned int mdkr_vita_relay_client_websocket_frame_bytes(
    unsigned int payloadLength) {
    unsigned int envelopeLength;
    if (payloadLength > MDKR_VITA_RELAY_MAX_PAYLOAD) {
        return 0u;
    }
    envelopeLength = MDKR_VITA_RELAY_HEADER_BYTES + payloadLength;
    return envelopeLength + (envelopeLength <= 125u ? 6u : 8u);
}

int mdkr_vita_relay_encode_client_frame(
    unsigned int localSlot, unsigned int destinationMask,
    unsigned int channel, unsigned int sequence,
    const unsigned char *payload, unsigned int payloadLength,
    unsigned char *output, unsigned int outputCapacity,
    unsigned int *outputLength) {
    unsigned int length = MDKR_VITA_RELAY_HEADER_BYTES + payloadLength;
    if (localSlot >= MDKR_VITA_RELAY_MAX_SLOTS ||
        destinationMask == 0u || (destinationMask & ~0x0Fu) != 0u ||
        (destinationMask & (1u << localSlot)) != 0u ||
        !channel_valid(channel) || sequence == 0u ||
        payloadLength > MDKR_VITA_RELAY_MAX_PAYLOAD ||
        (payloadLength != 0u && payload == NULL) ||
        output == NULL || outputLength == NULL ||
        outputCapacity < length) {
        return 0;
    }
    memset(output, 0, length);
    memcpy(output, s_relayMagic, sizeof(s_relayMagic));
    output[4] = 1u;
    output[5] = 1u;
    output[6] = MDKR_VITA_RELAY_CLIENT_SLOT;
    output[7] = (unsigned char)destinationMask;
    output[8] = (unsigned char)channel;
    store_u32_be(output + 12, sequence);
    if (payloadLength != 0u) {
        memcpy(output + MDKR_VITA_RELAY_HEADER_BYTES, payload,
               payloadLength);
    }
    *outputLength = length;
    return 1;
}

int mdkr_vita_relay_decode_server_frame(
    const unsigned char *frame, unsigned int frameLength,
    unsigned int localSlot, unsigned int memberMask,
    const unsigned int lastSequence[MDKR_VITA_RELAY_MAX_SLOTS],
    MdkrVitaRelayFrameView *view) {
    unsigned int sender;
    unsigned int destinationMask;
    unsigned int sequence;
    if (frame == NULL || lastSequence == NULL || view == NULL ||
        localSlot >= MDKR_VITA_RELAY_MAX_SLOTS ||
        frameLength < MDKR_VITA_RELAY_HEADER_BYTES ||
        frameLength > MDKR_VITA_RELAY_MAX_FRAME_BYTES ||
        memcmp(frame, s_relayMagic, sizeof(s_relayMagic)) != 0 ||
        frame[4] != 1u || frame[5] != 1u ||
        frame[9] != 0u || frame[10] != 0u || frame[11] != 0u) {
        return 0;
    }
    sender = frame[6];
    destinationMask = frame[7];
    sequence = load_u32_be(frame + 12);
    if (sender >= MDKR_VITA_RELAY_MAX_SLOTS || sender == localSlot ||
        (memberMask & (1u << sender)) == 0u ||
        destinationMask == 0u || (destinationMask & ~0x0Fu) != 0u ||
        (destinationMask & (1u << localSlot)) == 0u ||
        (destinationMask & (1u << sender)) != 0u ||
        !channel_valid(frame[8]) || sequence == 0u ||
        sequence <= lastSequence[sender]) {
        return 0;
    }
    view->senderSlot = (unsigned char)sender;
    view->destinationMask = (unsigned char)destinationMask;
    view->channel = frame[8];
    view->sequence = sequence;
    view->payload = frame + MDKR_VITA_RELAY_HEADER_BYTES;
    view->payloadLength = frameLength - MDKR_VITA_RELAY_HEADER_BYTES;
    return 1;
}
