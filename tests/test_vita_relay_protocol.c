#include <assert.h>
#include <string.h>

#include "vita_relay_protocol.h"

static void test_round_trip_shape(void) {
    const unsigned char payload[] = { 0x10, 0x20, 0x30 };
    unsigned char frame[MDKR_VITA_RELAY_MAX_FRAME_BYTES];
    unsigned int frameLength = 0;
    unsigned int lastSequence[MDKR_VITA_RELAY_MAX_SLOTS] = { 0 };
    MdkrVitaRelayFrameView view;

    assert(mdkr_vita_relay_encode_client_frame(
        0, 1u << 1, MDKR_VITA_RELAY_CHANNEL_INPUT, 7,
        payload, sizeof(payload), frame, sizeof(frame), &frameLength));
    assert(frameLength == MDKR_VITA_RELAY_HEADER_BYTES + sizeof(payload));
    assert(frame[6] == MDKR_VITA_RELAY_CLIENT_SLOT);

    frame[6] = 1;
    frame[7] = 1;
    assert(mdkr_vita_relay_decode_server_frame(
        frame, frameLength, 0, 0x03u, lastSequence, &view));
    assert(view.senderSlot == 1);
    assert(view.destinationMask == 1);
    assert(view.channel == MDKR_VITA_RELAY_CHANNEL_INPUT);
    assert(view.sequence == 7);
    assert(view.payloadLength == sizeof(payload));
    assert(memcmp(view.payload, payload, sizeof(payload)) == 0);
}

static void test_identity_destination_and_sequence_guards(void) {
    unsigned char frame[MDKR_VITA_RELAY_MAX_FRAME_BYTES];
    unsigned int frameLength = 0;
    unsigned int lastSequence[MDKR_VITA_RELAY_MAX_SLOTS] = { 0, 9, 0, 0 };
    MdkrVitaRelayFrameView view;

    assert(!mdkr_vita_relay_encode_client_frame(
        0, 1, MDKR_VITA_RELAY_CHANNEL_SESSION, 1,
        NULL, 0, frame, sizeof(frame), &frameLength));
    assert(mdkr_vita_relay_encode_client_frame(
        0, 2, MDKR_VITA_RELAY_CHANNEL_SESSION, 10,
        NULL, 0, frame, sizeof(frame), &frameLength));
    frame[6] = 1;
    frame[7] = 1;
    assert(mdkr_vita_relay_decode_server_frame(
        frame, frameLength, 0, 3, lastSequence, &view));
    assert(!mdkr_vita_relay_decode_server_frame(
        frame, frameLength, 0, 1, lastSequence, &view));
    lastSequence[1] = 10;
    assert(!mdkr_vita_relay_decode_server_frame(
        frame, frameLength, 0, 3, lastSequence, &view));
    lastSequence[1] = 5;
    assert(mdkr_vita_relay_decode_server_frame(
        frame, frameLength, 0, 3, lastSequence, &view));
    lastSequence[1] = 0;
    frame[7] = 2;
    assert(!mdkr_vita_relay_decode_server_frame(
        frame, frameLength, 0, 3, lastSequence, &view));
}

static void test_lan_channel_mapping(void) {
    unsigned int packetType;
    for (packetType = 1; packetType <= 11; packetType++) {
        assert(mdkr_vita_relay_channel_for_lan_packet(packetType) ==
               MDKR_VITA_RELAY_CHANNEL_SESSION);
    }
    assert(mdkr_vita_relay_channel_for_lan_packet(12) ==
           MDKR_VITA_RELAY_CHANNEL_INPUT);
    assert(mdkr_vita_relay_channel_for_lan_packet(13) ==
           MDKR_VITA_RELAY_CHANNEL_CONTROL);
    assert(mdkr_vita_relay_channel_for_lan_packet(14) ==
           MDKR_VITA_RELAY_CHANNEL_CONTROL);
    assert(mdkr_vita_relay_channel_for_lan_packet(15) ==
           MDKR_VITA_RELAY_CHANNEL_REPAIR);
}

static void test_websocket_frame_sizes(void) {
    assert(mdkr_vita_relay_client_websocket_frame_bytes(0) == 22u);
    assert(mdkr_vita_relay_client_websocket_frame_bytes(109) == 131u);
    assert(mdkr_vita_relay_client_websocket_frame_bytes(110) == 134u);
    assert(mdkr_vita_relay_client_websocket_frame_bytes(1024) == 1048u);
    assert(mdkr_vita_relay_client_websocket_frame_bytes(1025) == 0u);
}

int main(void) {
    test_round_trip_shape();
    test_identity_destination_and_sequence_guards();
    test_lan_channel_mapping();
    test_websocket_frame_sizes();
    return 0;
}
