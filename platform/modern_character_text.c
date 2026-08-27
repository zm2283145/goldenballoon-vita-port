#include "modern_character_text.h"

#include <string.h>

static int continuation(unsigned char byte) {
    return (byte & 0xC0u) == 0x80u;
}

static void emit(char value, char *output, size_t output_capacity,
                 size_t *used, int *truncated) {
    if (*used + 1u < output_capacity) {
        output[(*used)++] = value;
    } else {
        *truncated = 1;
    }
}

int mdkr_modern_character_text_project(
    const char *source, size_t source_capacity,
    char *output, size_t output_capacity,
    MdkrModernCharacterTextProjection *projection) {
    MdkrModernCharacterTextProjection result;
    size_t input = 0u;
    size_t used = 0u;
    int terminated = 0;
    memset(&result, 0, sizeof(result));
    result.valid_utf8 = 1;
    if (output != NULL && output_capacity != 0u) output[0] = '\0';
    if (source == NULL || source_capacity == 0u || output == NULL ||
        output_capacity == 0u) {
        if (projection != NULL) *projection = result;
        return 0;
    }
    while (input < source_capacity) {
        const unsigned char first = (unsigned char)source[input];
        size_t length = 1u;
        uint32_t codepoint = first;
        uint32_t minimum = 0u;
        int valid_codepoint = 1;
        if (first == 0u) {
            terminated = 1;
            break;
        }
        if (first < 0x80u) {
            if (first < 0x20u || first == 0x7Fu) {
                emit(' ', output, output_capacity, &used,
                     &result.output_truncated);
                result.replaced_controls++;
            } else {
                emit((char)first, output, output_capacity, &used,
                     &result.output_truncated);
            }
        } else {
            if (first >= 0xC2u && first <= 0xDFu) {
                length = 2u;
                codepoint = first & 0x1Fu;
                minimum = 0x80u;
            } else if (first >= 0xE0u && first <= 0xEFu) {
                length = 3u;
                codepoint = first & 0x0Fu;
                minimum = 0x800u;
            } else if (first >= 0xF0u && first <= 0xF4u) {
                length = 4u;
                codepoint = first & 0x07u;
                minimum = 0x10000u;
            } else {
                valid_codepoint = 0;
            }
            if (valid_codepoint &&
                (length > source_capacity - input ||
                 memchr(source + input, '\0', length) != NULL)) {
                valid_codepoint = 0;
            }
            if (valid_codepoint) {
                size_t continuation_index;
                for (continuation_index = 1u;
                     continuation_index < length; ++continuation_index) {
                    const unsigned char next =
                        (unsigned char)source[input + continuation_index];
                    if (!continuation(next)) {
                        valid_codepoint = 0;
                        break;
                    }
                    codepoint = (codepoint << 6u) | (next & 0x3Fu);
                }
                if (codepoint < minimum || codepoint > 0x10FFFFu ||
                    (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
                    valid_codepoint = 0;
                }
            }
            emit('?', output, output_capacity, &used,
                 &result.output_truncated);
            result.unsupported_codepoints++;
            if (!valid_codepoint) {
                result.valid_utf8 = 0;
                length = 1u;
            }
        }
        input += length;
        result.input_codepoints++;
    }
    output[used] = '\0';
    result.output_bytes = (uint32_t)used;
    if (projection != NULL) *projection = result;
    return terminated;
}
