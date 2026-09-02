#include "match_input_repair.h"

#include <string.h>

static const uint8_t kMagic[2] = {'M', 'R'};

/* Cell zero sits here; the trailing CRC covers everything before it. */
#define MDKR_MATCH_INPUT_REPAIR_SAMPLES_OFFSET 14u
#define MDKR_MATCH_INPUT_REPAIR_CRC_OFFSET 62u

static void store_u16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void store_u32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static uint16_t load_u16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
}

static uint32_t load_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

static uint16_t crc16_ccitt(const uint8_t *bytes, size_t length) {
    uint16_t value = UINT16_C(0xffff);
    size_t index;
    for (index = 0u; index < length; index++) {
        unsigned bit;
        value ^= (uint16_t)bytes[index] << 8;
        for (bit = 0u; bit < 8u; bit++) {
            value = (uint16_t)(value << 1) ^
                (uint16_t)(UINT16_C(0x1021) &
                           (uint16_t)-(int16_t)(value >> 15));
        }
    }
    return value;
}

/* A carried sample is an ordinary canonical pad frame, held to exactly the
 * shape the bundle carrier holds; a cell past `count` is absent and zero. */
static bool sample_valid(const MdkrPadSample *sample, bool carried) {
    if (!carried) {
        return sample->buttons == 0u && sample->stick_x == 0 &&
            sample->stick_y == 0 && sample->present == 0u;
    }
    return sample->present == 1u && sample->stick_x >= -80 &&
        sample->stick_x <= 80 && sample->stick_y >= -80 &&
        sample->stick_y <= 80;
}

static bool message_valid(const MdkrMatchInputRepair *message) {
    const bool answer = message->kind == MDKR_MATCH_INPUT_REPAIR_ANSWER;
    unsigned index;
    if (message->match_epoch == 0u || message->count == 0u) return false;
    if (message->kind != MDKR_MATCH_INPUT_REPAIR_REQUEST && !answer)
        return false;
    if (answer) {
        if (message->slot >= MDKR_SESSION_MAX_PLAYERS ||
            message->count > MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS) return false;
    } else if (message->slot != 0u ||
               message->count > MDKR_MATCH_INPUT_REPAIR_MAX_TICKS) {
        return false;
    }
    for (index = 0u; index < MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS; index++) {
        const bool carried = answer && index < message->count;
        if (!sample_valid(&message->samples[index], carried)) return false;
    }
    return true;
}

bool mdkr_match_input_repair_encode(
    const MdkrMatchInputRepair *message, uint8_t *bytes, size_t length) {
    uint8_t encoded[MDKR_MATCH_INPUT_REPAIR_BYTES];
    unsigned index;
    if (message == NULL || bytes == NULL ||
        length != MDKR_MATCH_INPUT_REPAIR_BYTES || !message_valid(message))
        return false;
    memset(encoded, 0, sizeof(encoded));
    memcpy(encoded, kMagic, sizeof(kMagic));
    encoded[2] = MDKR_MATCH_INPUT_REPAIR_VERSION;
    encoded[3] = message->kind;
    encoded[4] = message->slot;
    encoded[5] = message->count;
    store_u32(&encoded[6], message->match_epoch);
    store_u32(&encoded[10], message->first_tick);
    if (message->kind == MDKR_MATCH_INPUT_REPAIR_ANSWER) {
        for (index = 0u; index < message->count; index++) {
            const size_t offset =
                MDKR_MATCH_INPUT_REPAIR_SAMPLES_OFFSET + index * 4u;
            store_u16(&encoded[offset], message->samples[index].buttons);
            encoded[offset + 2u] = (uint8_t)message->samples[index].stick_x;
            encoded[offset + 3u] = (uint8_t)message->samples[index].stick_y;
        }
    }
    store_u16(&encoded[MDKR_MATCH_INPUT_REPAIR_CRC_OFFSET],
              crc16_ccitt(encoded, MDKR_MATCH_INPUT_REPAIR_CRC_OFFSET));
    memcpy(bytes, encoded, sizeof(encoded));
    return true;
}

bool mdkr_match_input_repair_decode(
    const uint8_t *bytes, size_t length, MdkrMatchInputRepair *message) {
    MdkrMatchInputRepair decoded;
    unsigned index;
    if (bytes == NULL || message == NULL ||
        length != MDKR_MATCH_INPUT_REPAIR_BYTES ||
        memcmp(bytes, kMagic, sizeof(kMagic)) != 0 ||
        bytes[2] != MDKR_MATCH_INPUT_REPAIR_VERSION ||
        load_u16(&bytes[MDKR_MATCH_INPUT_REPAIR_CRC_OFFSET]) !=
            crc16_ccitt(bytes, MDKR_MATCH_INPUT_REPAIR_CRC_OFFSET))
        return false;
    memset(&decoded, 0, sizeof(decoded));
    decoded.kind = bytes[3];
    decoded.slot = bytes[4];
    decoded.count = bytes[5];
    decoded.match_epoch = load_u32(&bytes[6]);
    decoded.first_tick = load_u32(&bytes[10]);
    if (decoded.kind == MDKR_MATCH_INPUT_REPAIR_ANSWER &&
        decoded.count <= MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS) {
        for (index = 0u; index < decoded.count; index++) {
            const size_t offset =
                MDKR_MATCH_INPUT_REPAIR_SAMPLES_OFFSET + index * 4u;
            decoded.samples[index].buttons = load_u16(&bytes[offset]);
            decoded.samples[index].stick_x = (int8_t)bytes[offset + 2u];
            decoded.samples[index].stick_y = (int8_t)bytes[offset + 3u];
            decoded.samples[index].present = 1u;
        }
    }
    /* Trailing cells must be zero on the wire, or two encodings of one run
     * would differ and a sender could smuggle bytes past the CRC. */
    for (index = decoded.kind == MDKR_MATCH_INPUT_REPAIR_ANSWER &&
                 decoded.count <= MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS
             ? decoded.count
             : 0u;
         index < MDKR_MATCH_INPUT_REPAIR_ANSWER_TICKS; index++) {
        const size_t offset =
            MDKR_MATCH_INPUT_REPAIR_SAMPLES_OFFSET + index * 4u;
        if (bytes[offset] != 0u || bytes[offset + 1u] != 0u ||
            bytes[offset + 2u] != 0u || bytes[offset + 3u] != 0u) return false;
    }
    if (!message_valid(&decoded)) return false;
    *message = decoded;
    return true;
}
