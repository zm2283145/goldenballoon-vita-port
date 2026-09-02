#ifndef MDKR64_MODERN_CHARACTER_TEXT_H
#define MDKR64_MODERN_CHARACTER_TEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdkrModernCharacterTextProjection {
    uint32_t input_codepoints;
    uint32_t folded_codepoints;
    uint32_t unsupported_codepoints;
    uint32_t replaced_controls;
    uint32_t output_bytes;
    int valid_utf8;
    int output_truncated;
} MdkrModernCharacterTextProjection;

/* Project bounded UTF-8 identity text into the retail game-font repertoire.
 * Printable ASCII survives exactly. Latin letters with diacritics, common
 * Latin ligatures, combining marks attached to Latin letters, typographic
 * punctuation, and full-width ASCII are folded deterministically. Each other
 * valid codepoint is one '?', so a UTF-8 sequence never expands into several
 * mystery cells. Malformed input is handled safely but reported through
 * valid_utf8. The source must terminate inside source_capacity, and output
 * always terminates when output_capacity is nonzero. */
/* Copy bounded identity text into a fixed-size buffer.
 *
 * A value that fits is copied exactly. A longer one is cut and gains a literal
 * "..." so the loss is visible. The cut always lands on a UTF-8 codepoint
 * boundary: a plain snprintf("%s", ...) stops at whatever byte the capacity
 * falls on and can publish half a multibyte sequence, which no reader can
 * decode and no font can draw.
 *
 * `capacity` includes the terminator. A capacity under five bytes leaves no
 * room for a cut plus a visible ellipsis, so nothing is published. Returns the
 * bytes written, excluding the terminator. */
size_t mdkr_modern_character_copy_bounded_name(char *output, size_t capacity,
                                               const char *value);

int mdkr_modern_character_text_project(
    const char *source, size_t source_capacity,
    char *output, size_t output_capacity,
    MdkrModernCharacterTextProjection *projection);

#ifdef __cplusplus
}
#endif

#endif /* MDKR64_MODERN_CHARACTER_TEXT_H */
