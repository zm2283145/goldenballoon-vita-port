#include "modern_character_text.h"

#include <string.h>

/* These tables are deliberately project-owned and locale-independent. A
 * system transliterator would make package identity vary by host OS. '-' is
 * an unsupported sentinel; all supported entries fold to one ASCII letter. */
static const char s_latin_1_fold[] =
    "AAAAAA-CEEEEIIII-NOOOOO--UUUUY--"
    "aaaaaa-ceeeeiiii-nooooo--uuuuy-y";

static const char s_latin_extended_a_fold[] =
    "AaAaAaCcCcCcCcDd--EeEeEeEeEeGgGg"
    "GgGgHh--IiIiIiIiI---JjKk-LlLlLlL"
    "l--NnNnNnn--OoOoOo--RrRrRrSsSsSs"
    "SsTtTt--UuUuUuUuUuUuWwYyYZzZzZzs";

static const char s_latin_extended_b_fold[] =
    "--------------------------------"
    "Oo-------------Uu---------------"
    "-------------AaIiOoUuUuUuUuUu-Aa"
    "Aa----GgKkOoOo--j---Gg--NnAa----"
    "AaAaEeEeIiIiOoOoRrRrUuUuSsTt--Hh"
    "------AaEeOoOoOoOoYy------------"
    "----------------";

static const char s_latin_extended_additional_fold[] =
    "AaBbBbBbCcDdDdDdDdDdEeEeEeEeEeFf"
    "GgHhHhHhHhHhIiIiKkKkKkLlLlLlLlMm"
    "MmMmNnNnNnNnOoOoOoOoPpPpRrRrRrRr"
    "SsSsSsSsSsTtTtTtTtUuUuUuUuUuVvVv"
    "WwWwWwWwWwXxXxYyZzZzZzhtwyas----"
    "AaAaAaAaAaAaAaAaAaAaAaAaEeEeEeEe"
    "EeEeEeEeIiIiOoOoOoOoOoOoOoOoOoOo"
    "OoOoUuUuUuUuUuUuUuYyYyYyYy------";

_Static_assert(sizeof(s_latin_1_fold) == 65u,
               "Latin-1 fold table must cover U+00C0..U+00FF");
_Static_assert(sizeof(s_latin_extended_a_fold) == 129u,
               "Latin Extended-A fold table must cover U+0100..U+017F");
_Static_assert(sizeof(s_latin_extended_b_fold) == 209u,
               "Latin Extended-B fold table must cover U+0180..U+024F");
_Static_assert(sizeof(s_latin_extended_additional_fold) == 257u,
               "Latin Extended Additional table must cover U+1E00..U+1EFF");

static int continuation(unsigned char byte) {
    return (byte & 0xC0u) == 0x80u;
}

size_t mdkr_modern_character_copy_bounded_name(char *output, size_t capacity,
                                               const char *value) {
    size_t length;
    if (output == NULL || capacity == 0u) return 0u;
    output[0] = '\0';
    if (value == NULL) return 0u;
    length = strlen(value);
    if (length < capacity) {
        memcpy(output, value, length + 1u);
        return length;
    }
    if (capacity < 5u) return 0u;
    length = capacity - 4u;
    /* 0b10xxxxxx is a UTF-8 continuation byte. Walk back until the first byte
     * that will NOT be copied starts a codepoint, so the copied prefix ends on
     * a whole character. Malformed input walks to zero and publishes the
     * ellipsis alone rather than a fragment. */
    while (length > 0u &&
           ((unsigned char)value[length] & 0xC0u) == 0x80u) {
        length--;
    }
    memcpy(output, value, length);
    memcpy(output + length, "...", 4u);
    return length + 3u;
}

static void emit(char value, char *output, size_t output_capacity,
                 size_t *used, int *truncated) {
    if (*used + 1u < output_capacity) {
        output[(*used)++] = value;
    } else {
        *truncated = 1;
    }
}

static void emit_text(const char *text, char *output, size_t output_capacity,
                      size_t *used, int *truncated) {
    while (*text != '\0') {
        emit(*text++, output, output_capacity, used, truncated);
    }
}

static int is_combining_mark(uint32_t codepoint) {
    return (codepoint >= 0x0300u && codepoint <= 0x036Fu) ||
           (codepoint >= 0x1AB0u && codepoint <= 0x1AFFu) ||
           (codepoint >= 0x1DC0u && codepoint <= 0x1DFFu) ||
           (codepoint >= 0x20D0u && codepoint <= 0x20FFu) ||
           (codepoint >= 0xFE20u && codepoint <= 0xFE2Fu);
}

static int ascii_letter(char value) {
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

/* Return a static ASCII replacement, or NULL when no honest projection is
 * defined. Multi-cell replacements are intentional and surfaced by the
 * bounded output_truncated result. */
static const char *fold_special(uint32_t codepoint) {
    switch (codepoint) {
        case 0x00A0u: case 0x3000u: return " ";
        case 0x00ADu:
        case 0x2010u: case 0x2011u: case 0x2012u:
        case 0x2013u: case 0x2014u: case 0x2015u:
        case 0x2212u: return "-";
        case 0x2018u: case 0x2019u: case 0x201Au: case 0x201Bu:
        case 0x2032u: return "'";
        case 0x201Cu: case 0x201Du: case 0x201Eu: case 0x201Fu:
        case 0x2033u: return "\"";
        case 0x2022u: return "*";
        case 0x2026u: return "...";
        case 0x2039u: return "<";
        case 0x203Au: return ">";
        case 0x2044u: return "/";
        case 0x00C6u: return "AE";
        case 0x00D0u: return "D";
        case 0x00D8u: return "O";
        case 0x00DEu: return "Th";
        case 0x00DFu: return "ss";
        case 0x00E6u: return "ae";
        case 0x00F0u: return "d";
        case 0x00F8u: return "o";
        case 0x00FEu: return "th";
        case 0x0110u: return "D";
        case 0x0111u: return "d";
        case 0x0126u: return "H";
        case 0x0127u: return "h";
        case 0x0131u: return "i";
        case 0x0132u: return "IJ";
        case 0x0133u: return "ij";
        case 0x0138u: return "k";
        case 0x0141u: return "L";
        case 0x0142u: return "l";
        case 0x0149u: return "'n";
        case 0x014Au: return "N";
        case 0x014Bu: return "n";
        case 0x0152u: return "OE";
        case 0x0153u: return "oe";
        case 0x0166u: return "T";
        case 0x0167u: return "t";
        case 0x1E9Eu: return "SS";
        default: return NULL;
    }
}

static const char *fold_codepoint(uint32_t codepoint, char single[2]) {
    const char *special = fold_special(codepoint);
    char folded = '-';
    if (special != NULL) return special;
    if (codepoint >= 0xFF01u && codepoint <= 0xFF5Eu) {
        single[0] = (char)(codepoint - 0xFEE0u);
        single[1] = '\0';
        return single;
    }
    if (codepoint >= 0x00C0u && codepoint <= 0x00FFu) {
        folded = s_latin_1_fold[codepoint - 0x00C0u];
    } else if (codepoint >= 0x0100u && codepoint <= 0x017Fu) {
        folded = s_latin_extended_a_fold[codepoint - 0x0100u];
    } else if (codepoint >= 0x0180u && codepoint <= 0x024Fu) {
        folded = s_latin_extended_b_fold[codepoint - 0x0180u];
    } else if (codepoint >= 0x1E00u && codepoint <= 0x1EFFu) {
        folded = s_latin_extended_additional_fold[codepoint - 0x1E00u];
    }
    if (folded == '-' || folded == '\0') return NULL;
    single[0] = folded;
    single[1] = '\0';
    return single;
}

int mdkr_modern_character_text_project(
    const char *source, size_t source_capacity,
    char *output, size_t output_capacity,
    MdkrModernCharacterTextProjection *projection) {
    MdkrModernCharacterTextProjection result;
    size_t input = 0u;
    size_t used = 0u;
    int terminated = 0;
    int previous_was_latin = 0;
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
                previous_was_latin = 0;
            } else {
                emit((char)first, output, output_capacity, &used,
                     &result.output_truncated);
                previous_was_latin = ascii_letter((char)first);
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
            if (!valid_codepoint) {
                emit('?', output, output_capacity, &used,
                     &result.output_truncated);
                result.unsupported_codepoints++;
                result.valid_utf8 = 0;
                length = 1u;
                previous_was_latin = 0;
            } else if (is_combining_mark(codepoint) && previous_was_latin) {
                result.folded_codepoints++;
            } else {
                char single[2];
                const char *folded = fold_codepoint(codepoint, single);
                if (folded != NULL) {
                    emit_text(folded, output, output_capacity, &used,
                              &result.output_truncated);
                    result.folded_codepoints++;
                    previous_was_latin = ascii_letter(
                        folded[strlen(folded) - 1u]);
                } else {
                    emit('?', output, output_capacity, &used,
                         &result.output_truncated);
                    result.unsupported_codepoints++;
                    previous_was_latin = 0;
                }
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
