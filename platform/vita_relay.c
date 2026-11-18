#include "vita_relay.h"

#if defined(__vita__) && defined(MDKR_ENABLE_VITA_RELAY)

#include <stdio.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "vita_network_runtime.h"
#if defined(MDKR_ENABLE_VITA_LAN)
#include "vita_lan.h"
#endif

extern void mdkr_vita_boot_log(const char *message);

#ifndef MDKR_VITA_RELAY_HOST
#define MDKR_VITA_RELAY_HOST "10.1.1.146"
#endif
#ifndef MDKR_VITA_RELAY_PORT
#define MDKR_VITA_RELAY_PORT 8080
#endif
#ifndef MDKR_VITA_RELAY_PATH
#define MDKR_VITA_RELAY_PATH "/relay"
#endif

#define MDKR_RELAY_PROTOCOL 1
#define MDKR_RELAY_SUBPROTOCOL "mdkr-relay-v1"
#define MDKR_RELAY_GAME_ID "golden-balloon"
#define MDKR_RELAY_GAME_VERSION 1
#define MDKR_RELAY_PLAYER_NAME "VITA PLAYER"
#define MDKR_RELAY_MAX_ROOMS 16
#define MDKR_RELAY_VISIBLE_ROOMS 3
#define MDKR_RELAY_ROOM_CODE_BYTES 9
#define MDKR_RELAY_ROOM_NAME_BYTES 21
#define MDKR_RELAY_PLAYER_NAME_BYTES 17
#define MDKR_RELAY_STATUS_BYTES 128
#define MDKR_RELAY_LABEL_BYTES 64
#define MDKR_RELAY_HTTP_LIMIT 2048
#define MDKR_RELAY_MESSAGE_LIMIT 24576
#define MDKR_RELAY_RX_BYTES 32768
#define MDKR_RELAY_TX_BYTES 8192
#define MDKR_RELAY_READS_PER_UPDATE 4
#define MDKR_RELAY_TIMEOUT_US 10000000ULL
#define MDKR_RELAY_TX_CONTROL_RESERVE 131u

typedef struct MdkrRelayRoom {
    char code[MDKR_RELAY_ROOM_CODE_BYTES];
    char name[MDKR_RELAY_ROOM_NAME_BYTES];
    char host[MDKR_RELAY_PLAYER_NAME_BYTES];
    char label[MDKR_RELAY_LABEL_BYTES];
    unsigned int players;
    unsigned int maxPlayers;
} MdkrRelayRoom;

typedef struct MdkrJsonCursor {
    const char *at;
    const char *end;
} MdkrJsonCursor;

typedef struct MdkrRelayJsonMessage {
    char type[20];
    char code[24];
    char gameId[32];
    char roomCode[MDKR_RELAY_ROOM_CODE_BYTES];
    char endpointId[21];
    MdkrRelayRoom room;
    MdkrRelayRoom rooms[MDKR_RELAY_MAX_ROOMS];
    unsigned int roomCount;
    unsigned int memberCount;
    unsigned int memberMask;
    unsigned int maxPlayers;
    unsigned int slot;
    unsigned int revision;
    unsigned int protocolVersion;
    unsigned int gameVersion;
    int haveProtocolVersion;
    int haveGameId;
    int haveGameVersion;
    int haveRoom;
    int haveRooms;
    int haveMembers;
    int haveEndpointId;
    int haveSlot;
    int haveHost;
    int host;
    int haveRevision;
} MdkrRelayJsonMessage;

typedef struct MdkrRelayReceivedPacket {
    unsigned char senderSlot;
    unsigned char channel;
    unsigned short payloadLength;
    unsigned char payload[MDKR_VITA_RELAY_MAX_PAYLOAD];
} MdkrRelayReceivedPacket;

static MdkrVitaRelayState s_state;
static int s_socket = -1;
static int s_networkOwned;
static int s_webSocketOpen;
static unsigned long long s_deadline;
static unsigned char s_rx[MDKR_RELAY_RX_BYTES];
static unsigned int s_rxLength;
static unsigned char s_tx[MDKR_RELAY_TX_BYTES];
static unsigned int s_txOffset;
static unsigned int s_txLength;
static unsigned char s_message[MDKR_RELAY_MESSAGE_LIMIT + 1];
static unsigned int s_messageLength;
static unsigned char s_messageOpcode;
static char s_status[MDKR_RELAY_STATUS_BYTES] = "ONLINE";
static MdkrRelayRoom s_rooms[MDKR_RELAY_MAX_ROOMS];
static unsigned int s_roomCount;
static unsigned int s_memberCount;
static unsigned int s_memberMask;
static unsigned int s_maxPlayers = 4;
static unsigned int s_localSlot = MDKR_VITA_RELAY_CLIENT_SLOT;
static unsigned int s_membershipRevision;
static unsigned int s_sendSequence;
static unsigned int s_receiveSequence[MDKR_VITA_RELAY_MAX_SLOTS];
static char s_joinedRoomCode[MDKR_RELAY_ROOM_CODE_BYTES];
static MdkrRelayReceivedPacket
    s_receiveQueue[MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH];
static unsigned int s_receiveQueueHead;
static unsigned int s_receiveQueueCount;
static int s_synchronizationLost;
static unsigned int s_receiveBackpressureLogCount;
static unsigned int s_sendBackpressureLogCount;
static char s_roomName[MDKR_RELAY_ROOM_NAME_BYTES] = "GOLDEN ROOM";
static char s_webSocketKey[25];
static char s_expectedAccept[29];
static SceWChar16 s_imeTitle[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static SceWChar16 s_imeInitial[MDKR_RELAY_ROOM_NAME_BYTES];
static SceWChar16 s_imeOutput[MDKR_RELAY_ROOM_NAME_BYTES];
static int s_imeModuleOwned;
static int s_imeTerminated;
static int s_imeApiResult;
static int s_imeDialogResult;
static int s_imeButton;

static int relay_socket_error(void) {
    int *error = sceNetErrnoLoc();
    return error != NULL ? *error : -1;
}

static int relay_would_block(int error) {
    return error == SCE_NET_EAGAIN || error == SCE_NET_EWOULDBLOCK ||
           error == SCE_NET_EINPROGRESS || error == SCE_NET_EALREADY ||
           error == SCE_NET_ENOTCONN;
}

static unsigned int rotate_left(unsigned int value, unsigned int count) {
    return (value << count) | (value >> (32u - count));
}

static void sha1(const unsigned char *input, unsigned int inputLength,
                 unsigned char output[20]) {
    unsigned int state[5] = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u
    };
    unsigned char block[64];
    unsigned long long bitLength = (unsigned long long)inputLength * 8u;
    unsigned int offset = 0;
    unsigned int blockLength;

    do {
        unsigned int words[80];
        unsigned int a;
        unsigned int b;
        unsigned int c;
        unsigned int d;
        unsigned int e;
        unsigned int index;

        blockLength = inputLength - offset;
        if (blockLength > 64) {
            blockLength = 64;
        }
        memset(block, 0, sizeof(block));
        if (blockLength != 0) {
            memcpy(block, input + offset, blockLength);
            offset += blockLength;
        }
        if (blockLength < 64) {
            block[blockLength++] = 0x80;
            if (blockLength > 56) {
                for (index = 0; index < 16; index++) {
                    words[index] = ((unsigned int)block[index * 4] << 24) |
                                   ((unsigned int)block[index * 4 + 1] << 16) |
                                   ((unsigned int)block[index * 4 + 2] << 8) |
                                   block[index * 4 + 3];
                }
                for (index = 16; index < 80; index++) {
                    words[index] = rotate_left(
                        words[index - 3] ^ words[index - 8] ^
                            words[index - 14] ^ words[index - 16],
                        1);
                }
                a = state[0];
                b = state[1];
                c = state[2];
                d = state[3];
                e = state[4];
                for (index = 0; index < 80; index++) {
                    unsigned int function;
                    unsigned int constant;
                    unsigned int temporary;
                    if (index < 20) {
                        function = (b & c) | ((~b) & d);
                        constant = 0x5A827999u;
                    } else if (index < 40) {
                        function = b ^ c ^ d;
                        constant = 0x6ED9EBA1u;
                    } else if (index < 60) {
                        function = (b & c) | (b & d) | (c & d);
                        constant = 0x8F1BBCDCu;
                    } else {
                        function = b ^ c ^ d;
                        constant = 0xCA62C1D6u;
                    }
                    temporary = rotate_left(a, 5) + function + e +
                                constant + words[index];
                    e = d;
                    d = c;
                    c = rotate_left(b, 30);
                    b = a;
                    a = temporary;
                }
                state[0] += a;
                state[1] += b;
                state[2] += c;
                state[3] += d;
                state[4] += e;
                memset(block, 0, sizeof(block));
            }
            block[56] = (unsigned char)(bitLength >> 56);
            block[57] = (unsigned char)(bitLength >> 48);
            block[58] = (unsigned char)(bitLength >> 40);
            block[59] = (unsigned char)(bitLength >> 32);
            block[60] = (unsigned char)(bitLength >> 24);
            block[61] = (unsigned char)(bitLength >> 16);
            block[62] = (unsigned char)(bitLength >> 8);
            block[63] = (unsigned char)bitLength;
        }

        for (index = 0; index < 16; index++) {
            words[index] = ((unsigned int)block[index * 4] << 24) |
                           ((unsigned int)block[index * 4 + 1] << 16) |
                           ((unsigned int)block[index * 4 + 2] << 8) |
                           block[index * 4 + 3];
        }
        for (index = 16; index < 80; index++) {
            words[index] = rotate_left(
                words[index - 3] ^ words[index - 8] ^
                    words[index - 14] ^ words[index - 16],
                1);
        }
        a = state[0];
        b = state[1];
        c = state[2];
        d = state[3];
        e = state[4];
        for (index = 0; index < 80; index++) {
            unsigned int function;
            unsigned int constant;
            unsigned int temporary;
            if (index < 20) {
                function = (b & c) | ((~b) & d);
                constant = 0x5A827999u;
            } else if (index < 40) {
                function = b ^ c ^ d;
                constant = 0x6ED9EBA1u;
            } else if (index < 60) {
                function = (b & c) | (b & d) | (c & d);
                constant = 0x8F1BBCDCu;
            } else {
                function = b ^ c ^ d;
                constant = 0xCA62C1D6u;
            }
            temporary = rotate_left(a, 5) + function + e +
                        constant + words[index];
            e = d;
            d = c;
            c = rotate_left(b, 30);
            b = a;
            a = temporary;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    } while (blockLength == 64);

    for (offset = 0; offset < 5; offset++) {
        output[offset * 4] = (unsigned char)(state[offset] >> 24);
        output[offset * 4 + 1] = (unsigned char)(state[offset] >> 16);
        output[offset * 4 + 2] = (unsigned char)(state[offset] >> 8);
        output[offset * 4 + 3] = (unsigned char)state[offset];
    }
}

static int base64_encode(const unsigned char *input, unsigned int length,
                         char *output, unsigned int capacity) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned int inputIndex = 0;
    unsigned int outputIndex = 0;
    unsigned int required = ((length + 2) / 3) * 4 + 1;
    if (capacity < required) {
        return 0;
    }
    while (inputIndex < length) {
        unsigned int remaining = length - inputIndex;
        unsigned int value = (unsigned int)input[inputIndex++] << 16;
        if (remaining > 1) {
            value |= (unsigned int)input[inputIndex++] << 8;
        }
        if (remaining > 2) {
            value |= input[inputIndex++];
        }
        output[outputIndex++] = alphabet[(value >> 18) & 63];
        output[outputIndex++] = alphabet[(value >> 12) & 63];
        output[outputIndex++] = remaining > 1 ? alphabet[(value >> 6) & 63] : '=';
        output[outputIndex++] = remaining > 2 ? alphabet[value & 63] : '=';
    }
    output[outputIndex] = '\0';
    return 1;
}

static int room_code_valid(const char *code) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    unsigned int index;
    if (strlen(code) != 8) {
        return 0;
    }
    for (index = 0; index < 8; index++) {
        if (strchr(alphabet, code[index]) == NULL) {
            return 0;
        }
    }
    return 1;
}

static int ascii_equal_ignore_case(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return 0;
        }
    }
    return *left == '\0' && *right == '\0';
}

static int ascii_token_contains(const char *value, const char *token) {
    char item[32];
    while (*value != '\0') {
        unsigned int length = 0;
        while (*value == ' ' || *value == '\t' || *value == ',') {
            value++;
        }
        while (*value != '\0' && *value != ',' && length + 1 < sizeof(item)) {
            item[length++] = *value++;
        }
        while (length > 0 &&
               (item[length - 1] == ' ' || item[length - 1] == '\t')) {
            length--;
        }
        item[length] = '\0';
        if (ascii_equal_ignore_case(item, token)) {
            return 1;
        }
        while (*value != '\0' && *value != ',') {
            value++;
        }
    }
    return 0;
}

static void relay_finish_ime(void) {
    SceCommonDialogStatus dialogStatus;
    if (s_state != MDKR_VITA_RELAY_NAMING) {
        return;
    }
    dialogStatus = sceImeDialogGetStatus();
    if (dialogStatus == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        sceImeDialogAbort();
    }
    if (dialogStatus != SCE_COMMON_DIALOG_STATUS_NONE) {
        sceImeDialogTerm();
    }
    if (s_imeModuleOwned) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
        s_imeModuleOwned = 0;
    }
}

static void relay_close_transport(int sendClose) {
    if (s_socket >= 0) {
        if (sendClose && s_webSocketOpen) {
            unsigned char frame[8];
            unsigned char mask[4];
            unsigned int index;
            if (sceKernelGetRandomNumber(mask, sizeof(mask)) >= 0) {
                frame[0] = 0x88;
                frame[1] = 0x80;
                for (index = 0; index < sizeof(mask); index++) {
                    frame[2 + index] = mask[index];
                }
                (void)sceNetSend(s_socket, frame, 6, SCE_NET_MSG_DONTWAIT);
            }
        }
        sceNetSocketClose(s_socket);
        s_socket = -1;
    }
    if (s_networkOwned) {
        mdkr_vita_network_release();
        s_networkOwned = 0;
    }
    s_webSocketOpen = 0;
    s_rxLength = 0;
    s_txOffset = 0;
    s_txLength = 0;
    s_messageLength = 0;
    s_messageOpcode = 0;
    s_receiveQueueHead = 0;
    s_receiveQueueCount = 0;
    s_sendSequence = 0;
    memset(s_receiveSequence, 0, sizeof(s_receiveSequence));
}

static void relay_set_error(const char *message) {
    char line[160];
    snprintf(line, sizeof(line),
             "relay: error=%s tx=%u rx=%u queued=%u send-seq=%u "
             "recv-seq=%u,%u,%u,%u",
             message, s_txLength, s_rxLength, s_receiveQueueCount,
             s_sendSequence, s_receiveSequence[0], s_receiveSequence[1],
             s_receiveSequence[2], s_receiveSequence[3]);
    mdkr_vita_boot_log(line);
    snprintf(s_status, sizeof(s_status),
             "%s tx=%u rx=%u q=%u seq=%u/%u,%u",
             message, s_txLength, s_rxLength, s_receiveQueueCount,
             s_sendSequence, s_receiveSequence[0], s_receiveSequence[1]);
    relay_finish_ime();
    relay_close_transport(1);
    s_state = MDKR_VITA_RELAY_ERROR;
}

static void relay_set_socket_error(const char *stage) {
    int error = relay_socket_error();
    char message[40];
    snprintf(message, sizeof(message), "%s %08X", stage, (unsigned int)error);
    relay_set_error(message);
}

static int queue_bytes(const void *bytes, unsigned int length) {
    if (s_txOffset != 0) {
        memmove(s_tx, s_tx + s_txOffset, s_txLength);
        s_txOffset = 0;
    }
    if (length > sizeof(s_tx) - s_txLength) {
        return 0;
    }
    memcpy(s_tx + s_txLength, bytes, length);
    s_txLength += length;
    return 1;
}

static int queue_frame(unsigned char opcode, const unsigned char *payload,
                       unsigned int length) {
    unsigned char frame[MDKR_VITA_RELAY_MAX_FRAME_BYTES + 8u];
    unsigned char mask[4];
    unsigned int headerLength;
    unsigned int index;
    if (length > MDKR_VITA_RELAY_MAX_FRAME_BYTES ||
        (opcode >= 8u && length > 125u) ||
        sceKernelGetRandomNumber(mask, sizeof(mask)) < 0) {
        return 0;
    }
    frame[0] = (unsigned char)(0x80 | opcode);
    if (length <= 125u) {
        frame[1] = (unsigned char)(0x80u | length);
        headerLength = 2;
    } else {
        frame[1] = 0xFEu;
        frame[2] = (unsigned char)(length >> 8);
        frame[3] = (unsigned char)length;
        headerLength = 4;
    }
    memcpy(frame + headerLength, mask, sizeof(mask));
    headerLength += sizeof(mask);
    for (index = 0; index < length; index++) {
        frame[headerLength + index] = payload[index] ^ mask[index & 3];
    }
    return queue_bytes(frame, headerLength + length);
}

static int queue_text(const char *text) {
    unsigned int length = (unsigned int)strlen(text);
    return queue_frame(1, (const unsigned char *)text, length);
}

static int relay_data_frame_fits(unsigned int payloadLength) {
    unsigned int frameBytes =
        mdkr_vita_relay_client_websocket_frame_bytes(payloadLength);
    return frameBytes != 0u &&
           frameBytes + MDKR_RELAY_TX_CONTROL_RESERVE <=
               sizeof(s_tx) - s_txLength;
}

static int flush_tx(void) {
    int result;
    int error;
    if (s_socket < 0 || s_txLength == 0) {
        return 1;
    }
    result = sceNetSend(s_socket, s_tx + s_txOffset, s_txLength,
                        SCE_NET_MSG_DONTWAIT);
    if (result < 0) {
        error = relay_socket_error();
        if (relay_would_block(error)) {
            return 1;
        }
        relay_set_socket_error("SEND");
        return 0;
    }
    if (result == 0) {
        relay_set_error("CONNECTION CLOSED");
        return 0;
    }
    s_txOffset += (unsigned int)result;
    s_txLength -= (unsigned int)result;
    if (s_txLength == 0) {
        s_txOffset = 0;
    }
    return 1;
}

static void json_space(MdkrJsonCursor *cursor) {
    while (cursor->at < cursor->end &&
           (*cursor->at == ' ' || *cursor->at == '\t' ||
            *cursor->at == '\r' || *cursor->at == '\n')) {
        cursor->at++;
    }
}

static int json_take(MdkrJsonCursor *cursor, char expected) {
    json_space(cursor);
    if (cursor->at >= cursor->end || *cursor->at != expected) {
        return 0;
    }
    cursor->at++;
    return 1;
}

static int json_string(MdkrJsonCursor *cursor, char *output,
                       unsigned int capacity) {
    unsigned int length = 0;
    if (!json_take(cursor, '"')) {
        return 0;
    }
    while (cursor->at < cursor->end && *cursor->at != '"') {
        unsigned char value = (unsigned char)*cursor->at++;
        if (value < 0x20 || value >= 0x80 || value == '\\') {
            return 0;
        }
        if (output != NULL) {
            if (length + 1 >= capacity) {
                return 0;
            }
            output[length] = (char)value;
        }
        length++;
    }
    if (cursor->at >= cursor->end || *cursor->at++ != '"') {
        return 0;
    }
    if (output != NULL) {
        output[length] = '\0';
    }
    return 1;
}

static int json_uint(MdkrJsonCursor *cursor, unsigned int *output) {
    unsigned int value = 0;
    int digits = 0;
    json_space(cursor);
    while (cursor->at < cursor->end &&
           *cursor->at >= '0' && *cursor->at <= '9') {
        unsigned int digit = (unsigned int)(*cursor->at++ - '0');
        if (value > 429496729u ||
            (value == 429496729u && digit > 5)) {
            return 0;
        }
        value = value * 10 + digit;
        digits++;
    }
    if (!digits) {
        return 0;
    }
    *output = value;
    return 1;
}

static int json_bool(MdkrJsonCursor *cursor, int *output) {
    json_space(cursor);
    if ((unsigned int)(cursor->end - cursor->at) >= 4 &&
        memcmp(cursor->at, "true", 4) == 0) {
        cursor->at += 4;
        *output = 1;
        return 1;
    }
    if ((unsigned int)(cursor->end - cursor->at) >= 5 &&
        memcmp(cursor->at, "false", 5) == 0) {
        cursor->at += 5;
        *output = 0;
        return 1;
    }
    return 0;
}

static int json_skip_value(MdkrJsonCursor *cursor, unsigned int depth);

static int json_skip_object(MdkrJsonCursor *cursor, unsigned int depth) {
    char key[32];
    if (!json_take(cursor, '{')) {
        return 0;
    }
    json_space(cursor);
    if (json_take(cursor, '}')) {
        return 1;
    }
    for (;;) {
        if (!json_string(cursor, key, sizeof(key)) ||
            !json_take(cursor, ':') ||
            !json_skip_value(cursor, depth + 1)) {
            return 0;
        }
        json_space(cursor);
        if (json_take(cursor, '}')) {
            return 1;
        }
        if (!json_take(cursor, ',')) {
            return 0;
        }
    }
}

static int json_skip_array(MdkrJsonCursor *cursor, unsigned int depth) {
    if (!json_take(cursor, '[')) {
        return 0;
    }
    json_space(cursor);
    if (json_take(cursor, ']')) {
        return 1;
    }
    for (;;) {
        if (!json_skip_value(cursor, depth + 1)) {
            return 0;
        }
        json_space(cursor);
        if (json_take(cursor, ']')) {
            return 1;
        }
        if (!json_take(cursor, ',')) {
            return 0;
        }
    }
}

static int json_skip_value(MdkrJsonCursor *cursor, unsigned int depth) {
    const char *start;
    if (depth > 8) {
        return 0;
    }
    json_space(cursor);
    if (cursor->at >= cursor->end) {
        return 0;
    }
    if (*cursor->at == '"') {
        return json_string(cursor, NULL, 0);
    }
    if (*cursor->at == '{') {
        return json_skip_object(cursor, depth);
    }
    if (*cursor->at == '[') {
        return json_skip_array(cursor, depth);
    }
    start = cursor->at;
    while (cursor->at < cursor->end &&
           *cursor->at != ',' && *cursor->at != '}' &&
           *cursor->at != ']' && *cursor->at != ' ' &&
           *cursor->at != '\t' && *cursor->at != '\r' &&
           *cursor->at != '\n') {
        cursor->at++;
    }
    return cursor->at != start;
}

static int parse_room(MdkrJsonCursor *cursor, MdkrRelayRoom *room) {
    char key[24];
    unsigned int fields = 0;
    memset(room, 0, sizeof(*room));
    if (!json_take(cursor, '{')) {
        return 0;
    }
    for (;;) {
        json_space(cursor);
        if (json_take(cursor, '}')) {
            break;
        }
        if (!json_string(cursor, key, sizeof(key)) ||
            !json_take(cursor, ':')) {
            return 0;
        }
        if (strcmp(key, "roomCode") == 0) {
            if (!json_string(cursor, room->code, sizeof(room->code))) {
                return 0;
            }
            fields |= 1;
        } else if (strcmp(key, "roomName") == 0) {
            if (!json_string(cursor, room->name, sizeof(room->name))) {
                return 0;
            }
            fields |= 2;
        } else if (strcmp(key, "hostName") == 0) {
            if (!json_string(cursor, room->host, sizeof(room->host))) {
                return 0;
            }
            fields |= 4;
        } else if (strcmp(key, "playerCount") == 0) {
            if (!json_uint(cursor, &room->players)) {
                return 0;
            }
            fields |= 8;
        } else if (strcmp(key, "maxPlayers") == 0) {
            if (!json_uint(cursor, &room->maxPlayers)) {
                return 0;
            }
            fields |= 16;
        } else if (!json_skip_value(cursor, 1)) {
            return 0;
        }
        json_space(cursor);
        if (json_take(cursor, '}')) {
            break;
        }
        if (!json_take(cursor, ',')) {
            return 0;
        }
    }
    if (fields != 31 || !room_code_valid(room->code) ||
        room->name[0] == '\0' || room->host[0] == '\0' ||
        room->maxPlayers < 2 || room->maxPlayers > 4 ||
        room->players > room->maxPlayers) {
        return 0;
    }
    snprintf(room->label, sizeof(room->label), "%s / %s %u/%u",
             room->name, room->host, room->players, room->maxPlayers);
    return 1;
}

static int parse_rooms(MdkrJsonCursor *cursor, MdkrRelayJsonMessage *message) {
    unsigned int total = 0;
    if (!json_take(cursor, '[')) {
        return 0;
    }
    json_space(cursor);
    if (json_take(cursor, ']')) {
        message->haveRooms = 1;
        return 1;
    }
    for (;;) {
        MdkrRelayRoom room;
        if (!parse_room(cursor, &room)) {
            return 0;
        }
        if (total < MDKR_RELAY_MAX_ROOMS) {
            message->rooms[total] = room;
        }
        total++;
        if (total > 128) {
            return 0;
        }
        json_space(cursor);
        if (json_take(cursor, ']')) {
            break;
        }
        if (!json_take(cursor, ',')) {
            return 0;
        }
    }
    message->roomCount =
        total < MDKR_RELAY_MAX_ROOMS ? total : MDKR_RELAY_MAX_ROOMS;
    message->haveRooms = 1;
    return 1;
}

static int parse_members(MdkrJsonCursor *cursor,
                         MdkrRelayJsonMessage *message) {
    unsigned int count = 0;
    unsigned int mask = 0;
    if (!json_take(cursor, '[')) {
        return 0;
    }
    json_space(cursor);
    if (json_take(cursor, ']')) {
        message->haveMembers = 1;
        return 1;
    }
    for (;;) {
        char key[24];
        unsigned int slot = MDKR_VITA_RELAY_MAX_SLOTS;
        int host = 0;
        int haveSlot = 0;
        int haveHost = 0;
        if (!json_take(cursor, '{')) {
            return 0;
        }
        for (;;) {
            json_space(cursor);
            if (json_take(cursor, '}')) {
                break;
            }
            if (!json_string(cursor, key, sizeof(key)) ||
                !json_take(cursor, ':')) {
                return 0;
            }
            if (strcmp(key, "slot") == 0) {
                if (!json_uint(cursor, &slot)) {
                    return 0;
                }
                haveSlot = 1;
            } else if (strcmp(key, "host") == 0) {
                if (!json_bool(cursor, &host)) {
                    return 0;
                }
                haveHost = 1;
            } else if (!json_skip_value(cursor, 2)) {
                return 0;
            }
            json_space(cursor);
            if (json_take(cursor, '}')) {
                break;
            }
            if (!json_take(cursor, ',')) {
                return 0;
            }
        }
        if (!haveSlot || !haveHost ||
            slot >= MDKR_VITA_RELAY_MAX_SLOTS ||
            host != (slot == 0u) || (mask & (1u << slot)) != 0u ||
            ++count > MDKR_VITA_RELAY_MAX_SLOTS) {
            return 0;
        }
        mask |= 1u << slot;
        json_space(cursor);
        if (json_take(cursor, ']')) {
            break;
        }
        if (!json_take(cursor, ',')) {
            return 0;
        }
    }
    message->memberCount = count;
    message->memberMask = mask;
    message->haveMembers = 1;
    return 1;
}

static int parse_message(const char *text, unsigned int length,
                         MdkrRelayJsonMessage *message) {
    MdkrJsonCursor cursor;
    char key[24];
    memset(message, 0, sizeof(*message));
    cursor.at = text;
    cursor.end = text + length;
    if (!json_take(&cursor, '{')) {
        return 0;
    }
    for (;;) {
        json_space(&cursor);
        if (json_take(&cursor, '}')) {
            break;
        }
        if (!json_string(&cursor, key, sizeof(key)) ||
            !json_take(&cursor, ':')) {
            return 0;
        }
        if (strcmp(key, "protocolVersion") == 0) {
            if (!json_uint(&cursor, &message->protocolVersion)) {
                return 0;
            }
            message->haveProtocolVersion = 1;
        } else if (strcmp(key, "gameId") == 0) {
            if (!json_string(&cursor, message->gameId,
                             sizeof(message->gameId))) {
                return 0;
            }
            message->haveGameId = 1;
        } else if (strcmp(key, "gameVersion") == 0) {
            if (!json_uint(&cursor, &message->gameVersion)) {
                return 0;
            }
            message->haveGameVersion = 1;
        } else if (strcmp(key, "type") == 0) {
            if (!json_string(&cursor, message->type,
                             sizeof(message->type))) {
                return 0;
            }
        } else if (strcmp(key, "code") == 0) {
            if (!json_string(&cursor, message->code,
                             sizeof(message->code))) {
                return 0;
            }
        } else if (strcmp(key, "roomCode") == 0) {
            if (!json_string(&cursor, message->roomCode,
                             sizeof(message->roomCode))) {
                return 0;
            }
        } else if (strcmp(key, "endpointId") == 0) {
            if (!json_string(&cursor, message->endpointId,
                             sizeof(message->endpointId))) {
                return 0;
            }
            message->haveEndpointId = 1;
        } else if (strcmp(key, "maxPlayers") == 0) {
            if (!json_uint(&cursor, &message->maxPlayers)) {
                return 0;
            }
        } else if (strcmp(key, "slot") == 0) {
            if (!json_uint(&cursor, &message->slot)) {
                return 0;
            }
            message->haveSlot = 1;
        } else if (strcmp(key, "revision") == 0) {
            if (!json_uint(&cursor, &message->revision)) {
                return 0;
            }
            message->haveRevision = 1;
        } else if (strcmp(key, "host") == 0) {
            if (!json_bool(&cursor, &message->host)) {
                return 0;
            }
            message->haveHost = 1;
        } else if (strcmp(key, "room") == 0) {
            if (!parse_room(&cursor, &message->room)) {
                return 0;
            }
            message->haveRoom = 1;
        } else if (strcmp(key, "rooms") == 0) {
            if (!parse_rooms(&cursor, message)) {
                return 0;
            }
        } else if (strcmp(key, "members") == 0) {
            if (!parse_members(&cursor, message)) {
                return 0;
            }
        } else if (!json_skip_value(&cursor, 1)) {
            return 0;
        }
        json_space(&cursor);
        if (json_take(&cursor, '}')) {
            break;
        }
        if (!json_take(&cursor, ',')) {
            return 0;
        }
    }
    json_space(&cursor);
    return cursor.at == cursor.end && message->haveProtocolVersion &&
           message->protocolVersion == MDKR_RELAY_PROTOCOL &&
           (!message->haveGameId ||
            strcmp(message->gameId, MDKR_RELAY_GAME_ID) == 0) &&
           (!message->haveGameVersion ||
            message->gameVersion == MDKR_RELAY_GAME_VERSION) &&
           message->type[0] != '\0';
}

static void room_upsert(const MdkrRelayRoom *room) {
    unsigned int index;
    for (index = 0; index < s_roomCount; index++) {
        if (strcmp(s_rooms[index].code, room->code) == 0) {
            s_rooms[index] = *room;
            return;
        }
    }
    if (s_roomCount < MDKR_RELAY_MAX_ROOMS) {
        s_rooms[s_roomCount++] = *room;
    }
}

static void room_remove(const char *code) {
    unsigned int index;
    for (index = 0; index < s_roomCount; index++) {
        if (strcmp(s_rooms[index].code, code) == 0) {
            s_roomCount--;
            if (index < s_roomCount) {
                memmove(&s_rooms[index], &s_rooms[index + 1],
                        (s_roomCount - index) * sizeof(s_rooms[0]));
            }
            memset(&s_rooms[s_roomCount], 0, sizeof(s_rooms[0]));
            return;
        }
    }
}

static void relay_update_lobby_status(void) {
    if (s_memberCount > 2u) {
        snprintf(s_status, sizeof(s_status),
                 "ONLINE: %u PLAYERS - 2 PLAYER ONLY", s_memberCount);
    } else if (s_memberCount == 2u && (s_memberMask & 0x03u) == 0x03u) {
        snprintf(s_status, sizeof(s_status),
                 "ONLINE: CONNECTING GAMEPLAY");
    } else {
        snprintf(s_status, sizeof(s_status),
                 "ONLINE: WAITING FOR SECOND PLAYER");
    }
}

static int handle_text_message(const unsigned char *payload,
                               unsigned int length) {
    MdkrRelayJsonMessage message;
    if (!parse_message((const char *)payload, length, &message)) {
        relay_set_error("INVALID SERVER MESSAGE");
        return 0;
    }
    if (strcmp(message.type, "room_list") == 0) {
        if (!message.haveRooms || s_state == MDKR_VITA_RELAY_LOBBY) {
            relay_set_error("INVALID ROOM LIST");
            return 0;
        }
        memcpy(s_rooms, message.rooms, sizeof(s_rooms));
        s_roomCount = message.roomCount;
        s_state = MDKR_VITA_RELAY_BROWSING;
        s_deadline = 0;
        snprintf(s_status, sizeof(s_status), "ONLINE: %u ROOM%s",
                 s_roomCount, s_roomCount == 1 ? "" : "S");
        return 1;
    }
    if (strcmp(message.type, "room_upsert") == 0) {
        if (!message.haveRoom || s_state == MDKR_VITA_RELAY_LOBBY) {
            relay_set_error("INVALID ROOM UPDATE");
            return 0;
        }
        room_upsert(&message.room);
        if (s_state == MDKR_VITA_RELAY_BROWSING) {
            snprintf(s_status, sizeof(s_status), "ONLINE: %u ROOM%s",
                     s_roomCount, s_roomCount == 1 ? "" : "S");
        }
        return 1;
    }
    if (strcmp(message.type, "room_remove") == 0) {
        if (!room_code_valid(message.roomCode) ||
            s_state == MDKR_VITA_RELAY_LOBBY) {
            relay_set_error("INVALID ROOM REMOVE");
            return 0;
        }
        room_remove(message.roomCode);
        if (s_state == MDKR_VITA_RELAY_BROWSING) {
            snprintf(s_status, sizeof(s_status), "ONLINE: %u ROOM%s",
                     s_roomCount, s_roomCount == 1 ? "" : "S");
        }
        return 1;
    }
    if (strcmp(message.type, "welcome") == 0) {
        unsigned int endpointIndex;
        if (!message.haveMembers || !message.haveEndpointId ||
            !message.haveSlot || !message.haveHost ||
            !message.haveRevision || !room_code_valid(message.roomCode) ||
            message.maxPlayers < 2 || message.maxPlayers > 4 ||
            message.memberCount == 0 || message.slot >= message.maxPlayers ||
            message.host != (message.slot == 0) || message.revision == 0 ||
            (message.memberMask & (1u << message.slot)) == 0u ||
            s_state != MDKR_VITA_RELAY_ADMITTING) {
            relay_set_error("INVALID WELCOME");
            return 0;
        }
        for (endpointIndex = 0;
             message.endpointId[endpointIndex] != '\0'; endpointIndex++) {
            if (message.endpointId[endpointIndex] < '0' ||
                message.endpointId[endpointIndex] > '9') {
                relay_set_error("INVALID WELCOME");
                return 0;
            }
        }
        if (endpointIndex == 0) {
            relay_set_error("INVALID WELCOME");
            return 0;
        }
        s_memberCount = message.memberCount;
        s_memberMask = message.memberMask;
        s_maxPlayers = message.maxPlayers;
        s_localSlot = message.slot;
        s_membershipRevision = message.revision;
        snprintf(s_joinedRoomCode, sizeof(s_joinedRoomCode), "%s",
                 message.roomCode);
        s_state = MDKR_VITA_RELAY_LOBBY;
        s_deadline = 0;
        relay_update_lobby_status();
        return 1;
    }
    if (strcmp(message.type, "membership") == 0) {
        if (s_state != MDKR_VITA_RELAY_LOBBY ||
            !message.haveMembers || !message.haveRevision ||
            message.revision <= s_membershipRevision ||
            (message.memberMask & (1u << s_localSlot)) == 0u) {
            relay_set_error("INVALID MEMBERSHIP");
            return 0;
        }
        s_memberCount = message.memberCount;
        s_memberMask = message.memberMask;
        s_membershipRevision = message.revision;
        relay_update_lobby_status();
        return 1;
    }
    if (strcmp(message.type, "error") == 0) {
        relay_set_error(message.code[0] != '\0' ? message.code : "SERVER");
        return 0;
    }
    relay_set_error("UNEXPECTED SERVER MESSAGE");
    return 0;
}

static int handle_binary_message(const unsigned char *payload,
                                 unsigned int length) {
    MdkrVitaRelayFrameView view;
    MdkrRelayReceivedPacket *packet;
    unsigned int rawSender = 0xFFu;
    unsigned int rawChannel = 0xFFu;
    unsigned int rawSequence = 0u;
    unsigned int rawLastSequence = 0u;
    unsigned int tail;
    if (s_state != MDKR_VITA_RELAY_LOBBY ||
        !mdkr_vita_relay_decode_server_frame(
            payload, length, s_localSlot, s_memberMask,
            s_receiveSequence, &view)) {
        if (payload != NULL &&
            length >= MDKR_VITA_RELAY_HEADER_BYTES) {
            rawSender = payload[6];
            rawChannel = payload[8];
            rawSequence = ((unsigned int)payload[12] << 24) |
                          ((unsigned int)payload[13] << 16) |
                          ((unsigned int)payload[14] << 8) |
                          (unsigned int)payload[15];
            if (rawSender < MDKR_VITA_RELAY_MAX_SLOTS) {
                rawLastSequence = s_receiveSequence[rawSender];
            }
        }
        s_synchronizationLost = 1;
        if (s_state != MDKR_VITA_RELAY_LOBBY) {
            relay_set_error("RELAY PACKET OUTSIDE ROOM");
        } else if (rawSender < MDKR_VITA_RELAY_MAX_SLOTS &&
                   rawSequence != 0u &&
                   rawSequence <= rawLastSequence) {
            char reason[96];
            snprintf(reason, sizeof(reason),
                     "RELAY SEQUENCE sender=%u got=%u last=%u",
                     rawSender, rawSequence, rawLastSequence);
            relay_set_error(reason);
        } else {
            char reason[96];
            snprintf(reason, sizeof(reason),
                     "INVALID RELAY PACKET l=%u s=%u c=%u q=%u last=%u",
                     length, rawSender, rawChannel, rawSequence,
                     rawLastSequence);
            relay_set_error(reason);
        }
        return 0;
    }
    if (s_receiveQueueCount >= MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH) {
        s_synchronizationLost = 1;
        relay_set_error("RECEIVE QUEUE OVERFLOW");
        return 0;
    }
    tail = (s_receiveQueueHead + s_receiveQueueCount) %
           MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH;
    packet = &s_receiveQueue[tail];
    packet->senderSlot = view.senderSlot;
    packet->channel = view.channel;
    packet->payloadLength = (unsigned short)view.payloadLength;
    if (view.payloadLength != 0u) {
        memcpy(packet->payload, view.payload, view.payloadLength);
    }
    s_receiveSequence[view.senderSlot] = view.sequence;
    s_receiveQueueCount++;
    return 1;
}

static int append_message(unsigned char opcode, const unsigned char *payload,
                          unsigned int length, int final) {
    if (s_messageOpcode == 0) {
        if (opcode != 1 && opcode != 2) {
            return 0;
        }
        s_messageOpcode = opcode;
    } else if (opcode != 0) {
        return 0;
    }
    if (length > MDKR_RELAY_MESSAGE_LIMIT - s_messageLength) {
        return 0;
    }
    memcpy(s_message + s_messageLength, payload, length);
    s_messageLength += length;
    if (!final) {
        return 1;
    }
    if (s_messageOpcode == 1) {
        s_message[s_messageLength] = '\0';
        if (!handle_text_message(s_message, s_messageLength)) {
            return 0;
        }
    } else if (!handle_binary_message(s_message, s_messageLength)) {
        return 0;
    }
    s_messageLength = 0;
    s_messageOpcode = 0;
    return 1;
}

static int process_frames(void) {
    while (s_rxLength >= 2) {
        unsigned char first = s_rx[0];
        unsigned char second = s_rx[1];
        unsigned char opcode = first & 0x0F;
        unsigned long long payloadLength = second & 0x7F;
        unsigned int headerLength = 2;
        unsigned int frameLength;
        int final = (first & 0x80) != 0;
        if (s_receiveQueueCount >= MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH) {
            if (s_receiveBackpressureLogCount < 4u) {
                char line[96];
                snprintf(line, sizeof(line),
                         "relay: rx-backpressure queued=%u buffered=%u",
                         s_receiveQueueCount, s_rxLength);
                mdkr_vita_boot_log(line);
                s_receiveBackpressureLogCount++;
            }
            return 1;
        }
        if ((first & 0x70) != 0 || (second & 0x80) != 0) {
            relay_set_error("INVALID WEBSOCKET FRAME");
            return 0;
        }
        if (payloadLength == 126) {
            if (s_rxLength < 4) {
                return 1;
            }
            payloadLength = ((unsigned int)s_rx[2] << 8) | s_rx[3];
            if (payloadLength < 126) {
                relay_set_error("INVALID WEBSOCKET LENGTH");
                return 0;
            }
            headerLength = 4;
        } else if (payloadLength == 127) {
            unsigned int index;
            if (s_rxLength < 10) {
                return 1;
            }
            payloadLength = 0;
            for (index = 0; index < 8; index++) {
                payloadLength = (payloadLength << 8) | s_rx[2 + index];
            }
            if (payloadLength <= 65535) {
                relay_set_error("INVALID WEBSOCKET LENGTH");
                return 0;
            }
            headerLength = 10;
        }
        if (payloadLength > MDKR_RELAY_MESSAGE_LIMIT ||
            payloadLength > (unsigned long long)(sizeof(s_rx) - headerLength)) {
            relay_set_error("WEBSOCKET FRAME TOO LARGE");
            return 0;
        }
        frameLength = headerLength + (unsigned int)payloadLength;
        if (s_rxLength < frameLength) {
            return 1;
        }
        if (opcode >= 8) {
            if (!final || payloadLength > 125) {
                relay_set_error("INVALID WEBSOCKET CONTROL");
                return 0;
            }
            if (opcode == 8) {
                unsigned char reply[125];
                unsigned int replyLength = (unsigned int)payloadLength;
                unsigned int closeCode = 0u;
                if (replyLength == 1) {
                    relay_set_error("INVALID WEBSOCKET CLOSE");
                    return 0;
                }
                if (replyLength >= 2u) {
                    closeCode =
                        ((unsigned int)s_rx[headerLength] << 8) |
                        s_rx[headerLength + 1u];
                }
                memcpy(reply, s_rx + headerLength, replyLength);
                (void)queue_frame(8, reply, replyLength);
                (void)flush_tx();
                snprintf(s_status, sizeof(s_status),
                         "SERVER CLOSE code=%u tx=%u rx=%u q=%u seq=%u/%u,%u",
                         closeCode, s_txLength, s_rxLength,
                         s_receiveQueueCount, s_sendSequence,
                         s_receiveSequence[0], s_receiveSequence[1]);
                relay_close_transport(0);
                s_state = MDKR_VITA_RELAY_ERROR;
                return 0;
            }
            if (opcode == 9 &&
                !queue_frame(10, s_rx + headerLength,
                             (unsigned int)payloadLength)) {
                relay_set_error("PONG QUEUE FULL");
                return 0;
            }
            if (opcode != 9 && opcode != 10) {
                relay_set_error("INVALID WEBSOCKET CONTROL");
                return 0;
            }
        } else if (!append_message(opcode, s_rx + headerLength,
                                   (unsigned int)payloadLength, final)) {
            if (s_state != MDKR_VITA_RELAY_ERROR) {
                relay_set_error("INVALID WEBSOCKET MESSAGE");
            }
            return 0;
        }
        s_rxLength -= frameLength;
        if (s_rxLength != 0) {
            memmove(s_rx, s_rx + frameLength, s_rxLength);
        }
    }
    return 1;
}

static char *trim_header_value(char *value) {
    char *end;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return value;
}

static int validate_handshake(const unsigned char *bytes,
                              unsigned int length) {
    char header[MDKR_RELAY_HTTP_LIMIT + 1];
    char *line;
    char *next;
    int haveUpgrade = 0;
    int haveConnection = 0;
    int haveProtocol = 0;
    int haveAccept = 0;
    int seenUpgrade = 0;
    int seenConnection = 0;
    int seenProtocol = 0;
    int seenAccept = 0;
    if (length > MDKR_RELAY_HTTP_LIMIT) {
        return 0;
    }
    memcpy(header, bytes, length);
    header[length] = '\0';
    line = header;
    next = strstr(line, "\r\n");
    if (next == NULL) {
        return 0;
    }
    *next = '\0';
    if (strncmp(line, "HTTP/1.1 101 ", 13) != 0 &&
        strcmp(line, "HTTP/1.1 101") != 0) {
        return 0;
    }
    line = next + 2;
    while (*line != '\0') {
        char *colon;
        next = strstr(line, "\r\n");
        if (next == NULL) {
            return 0;
        }
        if (next == line) {
            break;
        }
        *next = '\0';
        colon = strchr(line, ':');
        if (colon == NULL) {
            return 0;
        }
        *colon++ = '\0';
        colon = trim_header_value(colon);
        if (ascii_equal_ignore_case(line, "Upgrade")) {
            if (seenUpgrade) {
                return 0;
            }
            seenUpgrade = 1;
            haveUpgrade = ascii_equal_ignore_case(colon, "websocket");
        } else if (ascii_equal_ignore_case(line, "Connection")) {
            if (seenConnection) {
                return 0;
            }
            seenConnection = 1;
            haveConnection = ascii_token_contains(colon, "upgrade");
        } else if (ascii_equal_ignore_case(line, "Sec-WebSocket-Protocol")) {
            if (seenProtocol) {
                return 0;
            }
            seenProtocol = 1;
            haveProtocol = strcmp(colon, MDKR_RELAY_SUBPROTOCOL) == 0;
        } else if (ascii_equal_ignore_case(line, "Sec-WebSocket-Accept")) {
            if (seenAccept) {
                return 0;
            }
            seenAccept = 1;
            haveAccept = strcmp(colon, s_expectedAccept) == 0;
        }
        line = next + 2;
    }
    return haveUpgrade && haveConnection && haveProtocol && haveAccept;
}

static int process_handshake(void) {
    unsigned int index;
    unsigned int headerLength = 0;
    for (index = 3; index < s_rxLength; index++) {
        if (s_rx[index - 3] == '\r' && s_rx[index - 2] == '\n' &&
            s_rx[index - 1] == '\r' && s_rx[index] == '\n') {
            headerLength = index + 1;
            break;
        }
    }
    if (headerLength == 0) {
        if (s_rxLength > MDKR_RELAY_HTTP_LIMIT) {
            relay_set_error("HANDSHAKE TOO LARGE");
            return 0;
        }
        return 1;
    }
    if (!validate_handshake(s_rx, headerLength)) {
        relay_set_error("HANDSHAKE REJECTED");
        return 0;
    }
    s_rxLength -= headerLength;
    if (s_rxLength != 0) {
        memmove(s_rx, s_rx + headerLength, s_rxLength);
    }
    s_webSocketOpen = 1;
    if (!queue_text(
            "{\"protocolVersion\":1,\"type\":\"browse\","
            "\"gameId\":\"golden-balloon\",\"gameVersion\":1}")) {
        relay_set_error("BROWSE QUEUE FULL");
        return 0;
    }
    s_state = MDKR_VITA_RELAY_CONNECTING;
    s_deadline = sceKernelGetProcessTimeWide() + MDKR_RELAY_TIMEOUT_US;
    snprintf(s_status, sizeof(s_status), "ONLINE: LOADING ROOMS");
    return process_frames();
}

static int receive_data(void) {
    unsigned int reads;
    if (s_webSocketOpen && s_rxLength != 0u) {
        if (!process_frames()) {
            return 0;
        }
        if (s_receiveQueueCount >= MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH) {
            return 1;
        }
    }
    for (reads = 0; reads < MDKR_RELAY_READS_PER_UPDATE; reads++) {
        int result;
        int error;
        if (s_rxLength == sizeof(s_rx)) {
            relay_set_error("RECEIVE BUFFER FULL");
            return 0;
        }
        result = sceNetRecv(s_socket, s_rx + s_rxLength,
                            sizeof(s_rx) - s_rxLength,
                            SCE_NET_MSG_DONTWAIT);
        if (result < 0) {
            error = relay_socket_error();
            if (relay_would_block(error)) {
                return 1;
            }
            relay_set_socket_error("RECEIVE");
            return 0;
        }
        if (result == 0) {
            relay_set_error("CONNECTION CLOSED");
            return 0;
        }
        s_rxLength += (unsigned int)result;
        if (!s_webSocketOpen) {
            if (!process_handshake()) {
                return 0;
            }
        } else if (!process_frames()) {
            return 0;
        }
        if (s_receiveQueueCount >= MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH) {
            return 1;
        }
        if ((unsigned int)result < sizeof(s_rx) - s_rxLength) {
            continue;
        }
    }
    return 1;
}

static int queue_handshake(void) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char nonce[16];
    unsigned char digest[20];
    char acceptSource[sizeof(s_webSocketKey) + sizeof(guid)];
    char request[512];
    int length;
    if (sceKernelGetRandomNumber(nonce, sizeof(nonce)) < 0 ||
        !base64_encode(nonce, sizeof(nonce), s_webSocketKey,
                       sizeof(s_webSocketKey))) {
        return 0;
    }
    snprintf(acceptSource, sizeof(acceptSource), "%s%s", s_webSocketKey, guid);
    sha1((const unsigned char *)acceptSource,
         (unsigned int)strlen(acceptSource), digest);
    if (!base64_encode(digest, sizeof(digest), s_expectedAccept,
                       sizeof(s_expectedAccept))) {
        return 0;
    }
    length = snprintf(
        request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: %s\r\n\r\n",
        MDKR_VITA_RELAY_PATH, MDKR_VITA_RELAY_HOST,
        MDKR_VITA_RELAY_PORT, s_webSocketKey, MDKR_RELAY_SUBPROTOCOL);
    return length > 0 && (unsigned int)length < sizeof(request) &&
           queue_bytes(request, (unsigned int)length);
}

static int ensure_app_util(void) {
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    int value;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON,
                                    &value) >= 0) {
        return 0;
    }
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    return sceAppUtilInit(&init, &boot);
}

static void ascii_to_wide(const char *source, SceWChar16 *destination,
                          unsigned int capacity) {
    unsigned int index = 0;
    while (source[index] != '\0' && index + 1 < capacity) {
        destination[index] = (SceWChar16)(unsigned char)source[index];
        index++;
    }
    destination[index] = 0;
}

static int begin_room_name_dialog(void) {
    SceCommonDialogConfigParam common;
    SceImeDialogParam parameter;
    int enterButton;
    int language;
    int result = ensure_app_util();
    if (result < 0) {
        return result;
    }
    sceCommonDialogConfigParamInit(&common);
    result = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &language);
    if (result < 0) {
        return result;
    }
    result = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON,
                                         &enterButton);
    if (result < 0) {
        return result;
    }
    common.language = (SceSystemParamLang)language;
    common.enterButtonAssign = (SceSystemParamEnterButtonAssign)enterButton;
    result = sceCommonDialogSetConfigParam(&common);
    if (result < 0) {
        return result;
    }
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_IME) != SCE_SYSMODULE_LOADED) {
        result = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
        if (result < 0) {
            return result;
        }
        s_imeModuleOwned = 1;
    }
    memset(s_imeTitle, 0, sizeof(s_imeTitle));
    memset(s_imeInitial, 0, sizeof(s_imeInitial));
    memset(s_imeOutput, 0, sizeof(s_imeOutput));
    ascii_to_wide("ONLINE ROOM NAME", s_imeTitle,
                  sizeof(s_imeTitle) / sizeof(s_imeTitle[0]));
    ascii_to_wide(s_roomName, s_imeInitial,
                  sizeof(s_imeInitial) / sizeof(s_imeInitial[0]));
    sceImeDialogParamInit(&parameter);
    parameter.supportedLanguages = 0;
    parameter.languagesForced = 0;
    parameter.type = SCE_IME_TYPE_BASIC_LATIN;
    parameter.option = 0;
    parameter.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    parameter.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    parameter.title = s_imeTitle;
    parameter.maxTextLength = MDKR_RELAY_ROOM_NAME_BYTES - 1;
    parameter.initialText = s_imeInitial;
    parameter.inputTextBuffer = s_imeOutput;
    result = sceImeDialogInit(&parameter);
    if (result < 0) {
        if (s_imeModuleOwned) {
            sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
            s_imeModuleOwned = 0;
        }
        return result;
    }
    s_imeTerminated = 0;
    s_imeApiResult = 0;
    s_imeDialogResult = SCE_COMMON_DIALOG_RESULT_ABORTED;
    s_imeButton = SCE_IME_DIALOG_BUTTON_NONE;
    s_state = MDKR_VITA_RELAY_NAMING;
    snprintf(s_status, sizeof(s_status), "ONLINE: NAME ROOM");
    return 0;
}

static void update_room_name_dialog(void) {
    SceImeDialogResult result;
    char request[192];
    char roomName[MDKR_RELAY_ROOM_NAME_BYTES];
    unsigned int inputIndex;
    unsigned int outputLength = 0;
    int invalid = 0;
    int requestLength;
    if (!s_imeTerminated) {
        if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
            return;
        }
        memset(&result, 0, sizeof(result));
        s_imeApiResult = sceImeDialogGetResult(&result);
        s_imeDialogResult = result.result;
        s_imeButton = result.button;
        sceImeDialogTerm();
        s_imeTerminated = 1;
        return;
    }
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_NONE) {
        return;
    }
    if (s_imeModuleOwned) {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_IME);
        s_imeModuleOwned = 0;
    }
    if (s_imeApiResult < 0) {
        relay_set_error("ROOM NAME");
        return;
    }
    if (s_imeDialogResult != SCE_COMMON_DIALOG_RESULT_OK ||
        s_imeButton != SCE_IME_DIALOG_BUTTON_ENTER) {
        s_state = MDKR_VITA_RELAY_BROWSING;
        snprintf(s_status, sizeof(s_status), "ONLINE: %u ROOM%s",
                 s_roomCount, s_roomCount == 1 ? "" : "S");
        return;
    }
    for (inputIndex = 0;
         inputIndex < MDKR_RELAY_ROOM_NAME_BYTES - 1 &&
         s_imeOutput[inputIndex] != 0; inputIndex++) {
        SceWChar16 value = s_imeOutput[inputIndex];
        if ((value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') ||
            value == ' ' || value == '_' || value == '-') {
            roomName[outputLength++] = (char)value;
        } else {
            invalid = 1;
        }
    }
    if (invalid || outputLength == 0) {
        relay_set_error("INVALID ROOM NAME");
        return;
    }
    roomName[outputLength] = '\0';
    memcpy(s_roomName, roomName, outputLength + 1);
    requestLength = snprintf(
        request, sizeof(request),
        "{\"protocolVersion\":1,\"type\":\"create\","
        "\"gameId\":\"golden-balloon\",\"gameVersion\":1,"
        "\"playerName\":\"%s\",\"roomName\":\"%s\",\"maxPlayers\":4}",
        MDKR_RELAY_PLAYER_NAME, s_roomName);
    if (requestLength <= 0 || (unsigned int)requestLength >= sizeof(request) ||
        !queue_text(request)) {
        relay_set_error("CREATE QUEUE FULL");
        return;
    }
    s_state = MDKR_VITA_RELAY_ADMITTING;
    s_deadline = sceKernelGetProcessTimeWide() + MDKR_RELAY_TIMEOUT_US;
    snprintf(s_status, sizeof(s_status), "ONLINE: CREATING ROOM");
}

int mdkr_vita_relay_start_browser(void) {
    SceNetSockaddrIn address;
    int enabled = 1;
    int netState;
    int result;
    int error;
#if defined(MDKR_ENABLE_VITA_LAN)
    mdkr_vita_lan_stop();
#endif
    mdkr_vita_relay_stop();
    result = mdkr_vita_network_acquire();
    if (result < 0) {
        relay_set_error("NETWORK INIT");
        return result;
    }
    s_networkOwned = 1;
    result = sceNetCtlInetGetState(&netState);
    if (result < 0 || netState != SCE_NETCTL_STATE_CONNECTED) {
        relay_set_error("NETWORK OFFLINE");
        return result < 0 ? result : -SCE_NET_ENETDOWN;
    }
    s_socket = sceNetSocket("mdkr-relay", SCE_NET_AF_INET,
                            SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
    if (s_socket < 0) {
        error = relay_socket_error();
        relay_set_socket_error("SOCKET");
        return error > 0 ? -error : -1;
    }
    result = sceNetSetsockopt(s_socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                              &enabled, sizeof(enabled));
    if (result < 0) {
        error = relay_socket_error();
        relay_set_socket_error("NONBLOCK");
        return error > 0 ? -error : -1;
    }
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = SCE_NET_AF_INET;
    address.sin_port = sceNetHtons((unsigned short)MDKR_VITA_RELAY_PORT);
    if (sceNetInetPton(SCE_NET_AF_INET, MDKR_VITA_RELAY_HOST,
                       &address.sin_addr) != 1) {
        relay_set_error("INVALID HOST");
        return -1;
    }
    result = sceNetConnect(s_socket, (const SceNetSockaddr *)&address,
                           sizeof(address));
    if (result < 0 && !relay_would_block(relay_socket_error())) {
        error = relay_socket_error();
        relay_set_socket_error("CONNECT");
        return error > 0 ? -error : -1;
    }
    if (!queue_handshake()) {
        relay_set_error("HANDSHAKE SETUP");
        return -1;
    }
    s_state = MDKR_VITA_RELAY_CONNECTING;
    s_deadline = sceKernelGetProcessTimeWide() + MDKR_RELAY_TIMEOUT_US;
    snprintf(s_status, sizeof(s_status), "ONLINE: CONNECTING");
    return 0;
}

int mdkr_vita_relay_start_host(void) {
    int result;
    if (s_state != MDKR_VITA_RELAY_BROWSING) {
        return -1;
    }
    result = begin_room_name_dialog();
    if (result < 0) {
        relay_set_error("ROOM NAME");
    }
    return result;
}

int mdkr_vita_relay_join_room(unsigned int index) {
    char request[192];
    int length;
    if (s_state != MDKR_VITA_RELAY_BROWSING || index >= s_roomCount) {
        return -1;
    }
    length = snprintf(
        request, sizeof(request),
        "{\"protocolVersion\":1,\"type\":\"join\","
        "\"gameId\":\"golden-balloon\",\"gameVersion\":1,"
        "\"playerName\":\"%s\",\"roomCode\":\"%s\"}",
        MDKR_RELAY_PLAYER_NAME, s_rooms[index].code);
    if (length <= 0 || (unsigned int)length >= sizeof(request) ||
        !queue_text(request)) {
        relay_set_error("JOIN QUEUE FULL");
        return -1;
    }
    s_state = MDKR_VITA_RELAY_ADMITTING;
    s_deadline = sceKernelGetProcessTimeWide() + MDKR_RELAY_TIMEOUT_US;
    snprintf(s_status, sizeof(s_status), "ONLINE: JOINING");
    return 0;
}

void mdkr_vita_relay_update(void) {
    unsigned long long now;
    if (s_state == MDKR_VITA_RELAY_IDLE ||
        s_state == MDKR_VITA_RELAY_ERROR) {
        return;
    }
    if (!flush_tx() || !receive_data()) {
        return;
    }
    if (s_state == MDKR_VITA_RELAY_NAMING) {
        update_room_name_dialog();
    }
    if (!flush_tx()) {
        return;
    }
    now = sceKernelGetProcessTimeWide();
    if (s_deadline != 0 && now >= s_deadline &&
        (s_state == MDKR_VITA_RELAY_CONNECTING ||
         s_state == MDKR_VITA_RELAY_ADMITTING)) {
        relay_set_error("TIMED OUT");
    }
}

void mdkr_vita_relay_stop(void) {
    relay_finish_ime();
    relay_close_transport(1);
    s_state = MDKR_VITA_RELAY_IDLE;
    s_deadline = 0;
    s_roomCount = 0;
    s_memberCount = 0;
    s_memberMask = 0;
    s_maxPlayers = 4;
    s_localSlot = MDKR_VITA_RELAY_CLIENT_SLOT;
    s_membershipRevision = 0;
    s_synchronizationLost = 0;
    s_receiveBackpressureLogCount = 0;
    s_sendBackpressureLogCount = 0;
    s_joinedRoomCode[0] = '\0';
    memset(s_rooms, 0, sizeof(s_rooms));
    snprintf(s_status, sizeof(s_status), "ONLINE");
}

MdkrVitaRelayState mdkr_vita_relay_state(void) {
    return s_state;
}

unsigned int mdkr_vita_relay_room_count(void) {
    return s_roomCount < MDKR_RELAY_VISIBLE_ROOMS
               ? s_roomCount
               : MDKR_RELAY_VISIBLE_ROOMS;
}

unsigned int mdkr_vita_relay_member_count(void) {
    return s_memberCount;
}

unsigned int mdkr_vita_relay_member_mask(void) {
    return s_memberMask;
}

int mdkr_vita_relay_local_slot(void) {
    return s_localSlot < MDKR_VITA_RELAY_MAX_SLOTS
               ? (int)s_localSlot
               : -1;
}

int mdkr_vita_relay_is_host(void) {
    return s_state == MDKR_VITA_RELAY_LOBBY && s_localSlot == 0u;
}

int mdkr_vita_relay_synchronization_lost(void) {
    return s_synchronizationLost;
}

const char *mdkr_vita_relay_room_code(void) {
    return s_joinedRoomCode;
}

int mdkr_vita_relay_send(unsigned int destinationMask,
                         MdkrVitaRelayChannel channel,
                         const void *payload, unsigned int payloadLength) {
    unsigned char envelope[MDKR_VITA_RELAY_MAX_FRAME_BYTES];
    unsigned int envelopeLength;
    unsigned int sequence;
    if (s_state != MDKR_VITA_RELAY_LOBBY ||
        (destinationMask & ~s_memberMask) != 0u ||
        s_sendSequence == 0xFFFFFFFFu) {
        return -1;
    }
    sequence = s_sendSequence + 1u;
    if (!mdkr_vita_relay_encode_client_frame(
            s_localSlot, destinationMask, channel, sequence,
            (const unsigned char *)payload, payloadLength,
            envelope, sizeof(envelope), &envelopeLength)) {
        return -1;
    }
    if (!flush_tx()) {
        return -1;
    }
    if (!relay_data_frame_fits(payloadLength)) {
        if (s_sendBackpressureLogCount < 4u) {
            char line[96];
            snprintf(line, sizeof(line),
                     "relay: tx-backpressure queued=%u payload=%u",
                     s_txLength, payloadLength);
            mdkr_vita_boot_log(line);
            s_sendBackpressureLogCount++;
        }
        return 0;
    }
    if (!queue_frame(2, envelope, envelopeLength)) {
        s_synchronizationLost = 1;
        relay_set_error("SEND FRAME FAILED");
        return -1;
    }
    s_sendSequence = sequence;
    return 1;
}

int mdkr_vita_relay_send_ready(unsigned int payloadLength) {
    return s_state == MDKR_VITA_RELAY_LOBBY &&
           s_sendSequence != 0xFFFFFFFFu &&
           relay_data_frame_fits(payloadLength);
}

int mdkr_vita_relay_receive(unsigned int *senderSlot,
                            MdkrVitaRelayChannel *channel,
                            void *payload, unsigned int payloadCapacity,
                            unsigned int *payloadLength) {
    MdkrRelayReceivedPacket *packet;
    if (senderSlot == NULL || channel == NULL || payloadLength == NULL ||
        (payloadCapacity != 0u && payload == NULL)) {
        return -1;
    }
    if (s_receiveQueueCount == 0u) {
        return 0;
    }
    packet = &s_receiveQueue[s_receiveQueueHead];
    if (packet->payloadLength > payloadCapacity) {
        s_synchronizationLost = 1;
        relay_set_error("RECEIVE PAYLOAD TOO LARGE");
        return -1;
    }
    *senderSlot = packet->senderSlot;
    *channel = (MdkrVitaRelayChannel)packet->channel;
    *payloadLength = packet->payloadLength;
    if (packet->payloadLength != 0u) {
        memcpy(payload, packet->payload, packet->payloadLength);
    }
    memset(packet, 0, sizeof(*packet));
    s_receiveQueueHead =
        (s_receiveQueueHead + 1u) %
        MDKR_VITA_RELAY_RECEIVE_QUEUE_DEPTH;
    s_receiveQueueCount--;
    return 1;
}

const char *mdkr_vita_relay_status(void) {
    return s_status;
}

void mdkr_vita_relay_format_diagnostics(
    char *output, unsigned int outputCapacity) {
    if (output == NULL || outputCapacity == 0u) {
        return;
    }
    snprintf(output, outputCapacity,
             "tx=%u rx=%u q=%u seq=%u/%u,%u state=%u",
             s_txLength, s_rxLength, s_receiveQueueCount, s_sendSequence,
             s_receiveSequence[0], s_receiveSequence[1],
             (unsigned int)s_state);
}

const char *mdkr_vita_relay_room_label(unsigned int index) {
    if (index >= s_roomCount) {
        return "";
    }
    return s_rooms[index].label;
}

#endif
