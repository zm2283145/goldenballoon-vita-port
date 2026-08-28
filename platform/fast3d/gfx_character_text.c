#include "gfx_character_text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <hb.h>
#include <hb-ot.h>
#include <SheenBidi/SheenBidi.h>

/* stb_truetype is intentionally isolated in this translation unit. Its input
 * is the exact compile-time font below, never a package or host-system font. */
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "gfx_character_text_face.h"
#include "gfx_character_text_arabic_face.h"
#include "gfx_character_text_hebrew_face.h"

enum {
    TEXT_FACE_PRIMARY = 0,
    TEXT_FACE_ARABIC,
    TEXT_FACE_HEBREW,
    TEXT_FACE_COUNT,
};

typedef struct DecodeState {
    const uint8_t *input_begin;
    const uint8_t *input_end;
    uint8_t *output_begin;
    uint8_t *output_at;
    uint8_t *output_end;
} DecodeState;

typedef struct TextGlyph {
    int glyph;
    uint8_t face;
    float pen_x;
    float pen_y;
} TextGlyph;

typedef struct TextLayout {
    TextGlyph glyphs[GFX_CHARACTER_TEXT_MAX_GLYPHS];
    uint32_t count;
    float advance_width;
    int baseline;
} TextLayout;

typedef struct EmbeddedFace {
    const char *encoded;
    uint8_t *bytes;
    size_t bytes_size;
    stbtt_fontinfo stb;
    hb_blob_t *blob;
    hb_face_t *hb_face;
    hb_font_t *hb_font;
    float scale;
} EmbeddedFace;

static EmbeddedFace s_faces[TEXT_FACE_COUNT];
static bool s_fonts_ready;
static bool s_fonts_failed;
static uint8_t *s_mask;
static size_t s_mask_capacity;

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u) |
           ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static bool decode_copy(DecodeState *state, const uint8_t *source,
                        size_t length) {
    size_t available;
    if (source < state->input_begin || source > state->input_end ||
        length > (size_t)(state->input_end - source) ||
        state->output_at > state->output_end) {
        return false;
    }
    available = (size_t)(state->output_end - state->output_at);
    if (length > available) return false;
    memcpy(state->output_at, source, length);
    state->output_at += length;
    return true;
}

static bool decode_match(DecodeState *state, size_t distance,
                         size_t length) {
    uint8_t *source;
    size_t index;
    if (distance == 0u || state->output_at < state->output_begin ||
        distance > (size_t)(state->output_at - state->output_begin) ||
        length > (size_t)(state->output_end - state->output_at)) {
        return false;
    }
    source = state->output_at - distance;
    for (index = 0u; index < length; ++index) {
        *state->output_at++ = *source++;
    }
    return true;
}

static uint32_t adler32_bytes(const uint8_t *bytes, size_t length) {
    const uint32_t mod = 65521u;
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    while (length != 0u) {
        size_t block = length > 5552u ? 5552u : length;
        size_t index;
        for (index = 0u; index < block; ++index) {
            s1 += bytes[index];
            s2 += s1;
        }
        s1 %= mod;
        s2 %= mod;
        bytes += block;
        length -= block;
    }
    return (s2 << 16u) | s1;
}

/* Bounded decoder for the public-domain stb_compress stream emitted by Dear
 * ImGui's pinned binary_to_compressed_c. Unlike the upstream convenience copy,
 * every token read and output span is checked before use. */
static bool decompress_stb(uint8_t *output, size_t output_size,
                           const uint8_t *input, size_t input_size) {
    DecodeState state;
    const uint8_t *at;
    uint32_t expected_adler;
    if (output == NULL || input == NULL || input_size < 18u ||
        read_be32(input) != 0x57BC0000u || read_be32(input + 4u) != 0u ||
        read_be32(input + 8u) != output_size) {
        return false;
    }
    state.input_begin = input;
    state.input_end = input + input_size;
    state.output_begin = output;
    state.output_at = output;
    state.output_end = output + output_size;
    at = input + 16u;
    for (;;) {
        uint8_t token;
        size_t distance;
        size_t length;
        if (at >= state.input_end) return false;
        token = *at;
        if (token >= 0x80u) {
            if ((size_t)(state.input_end - at) < 2u) return false;
            distance = (size_t)at[1] + 1u;
            length = (size_t)token - 0x80u + 1u;
            at += 2u;
            if (!decode_match(&state, distance, length)) return false;
        } else if (token >= 0x40u) {
            if ((size_t)(state.input_end - at) < 3u) return false;
            distance = ((((size_t)at[0] << 8u) | at[1]) - 0x4000u) + 1u;
            length = (size_t)at[2] + 1u;
            at += 3u;
            if (!decode_match(&state, distance, length)) return false;
        } else if (token >= 0x20u) {
            length = (size_t)token - 0x20u + 1u;
            if ((size_t)(state.input_end - at) < 1u + length ||
                !decode_copy(&state, at + 1u, length)) return false;
            at += 1u + length;
        } else if (token >= 0x18u) {
            if ((size_t)(state.input_end - at) < 4u) return false;
            distance = ((((size_t)at[0] << 16u) |
                         ((size_t)at[1] << 8u) | at[2]) - 0x180000u) + 1u;
            length = (size_t)at[3] + 1u;
            at += 4u;
            if (!decode_match(&state, distance, length)) return false;
        } else if (token >= 0x10u) {
            if ((size_t)(state.input_end - at) < 5u) return false;
            distance = ((((size_t)at[0] << 16u) |
                         ((size_t)at[1] << 8u) | at[2]) - 0x100000u) + 1u;
            length = ((size_t)at[3] << 8u) + at[4] + 1u;
            at += 5u;
            if (!decode_match(&state, distance, length)) return false;
        } else if (token >= 0x08u) {
            if ((size_t)(state.input_end - at) < 2u) return false;
            length = (((size_t)at[0] << 8u) | at[1]) - 0x0800u + 1u;
            if ((size_t)(state.input_end - at) < 2u + length ||
                !decode_copy(&state, at + 2u, length)) return false;
            at += 2u + length;
        } else if (token == 0x07u) {
            if ((size_t)(state.input_end - at) < 3u) return false;
            length = ((size_t)at[1] << 8u) + at[2] + 1u;
            if ((size_t)(state.input_end - at) < 3u + length ||
                !decode_copy(&state, at + 3u, length)) return false;
            at += 3u + length;
        } else if (token == 0x06u) {
            if ((size_t)(state.input_end - at) < 5u) return false;
            distance = ((size_t)at[1] << 16u) |
                       ((size_t)at[2] << 8u) | at[3];
            length = (size_t)at[4] + 1u;
            at += 5u;
            if (!decode_match(&state, distance + 1u, length)) return false;
        } else if (token == 0x04u) {
            if ((size_t)(state.input_end - at) < 6u) return false;
            distance = ((size_t)at[1] << 16u) |
                       ((size_t)at[2] << 8u) | at[3];
            length = ((size_t)at[4] << 8u) + at[5] + 1u;
            at += 6u;
            if (!decode_match(&state, distance + 1u, length)) return false;
        } else if (token == 0x05u) {
            if ((size_t)(state.input_end - at) < 6u || at[1] != 0xFAu ||
                state.output_at != state.output_end) return false;
            expected_adler = read_be32(at + 2u);
            return adler32_bytes(output, output_size) == expected_adler;
        } else {
            return false;
        }
    }
}

static int base85_value(unsigned char value) {
    if (value < 35u || value > 121u || value == '\\') return -1;
    return value >= '\\' ? (int)value - 36 : (int)value - 35;
}

static void release_face(EmbeddedFace *face) {
    if (face->hb_font != NULL) hb_font_destroy(face->hb_font);
    if (face->hb_face != NULL) hb_face_destroy(face->hb_face);
    if (face->blob != NULL) hb_blob_destroy(face->blob);
    free(face->bytes);
    memset(face, 0, sizeof(*face));
}

static bool load_face(EmbeddedFace *face, const char *encoded) {
    size_t encoded_size;
    size_t compressed_size;
    uint8_t *compressed = NULL;
    size_t source_at;
    size_t output_at;
    uint32_t decompressed_size;
    encoded_size = strlen(encoded);
    if (encoded_size == 0u || encoded_size % 5u != 0u) goto fail;
    compressed_size = encoded_size / 5u * 4u;
    compressed = (uint8_t *)malloc(compressed_size);
    if (compressed == NULL) goto fail;
    for (source_at = 0u, output_at = 0u; source_at < encoded_size;
         source_at += 5u, output_at += 4u) {
        uint32_t value = 0u;
        uint32_t multiplier = 1u;
        size_t index;
        for (index = 0u; index < 5u; ++index) {
            int digit = base85_value((unsigned char)encoded[source_at + index]);
            if (digit < 0) goto fail;
            value += (uint32_t)digit * multiplier;
            multiplier *= 85u;
        }
        compressed[output_at] = (uint8_t)value;
        compressed[output_at + 1u] = (uint8_t)(value >> 8u);
        compressed[output_at + 2u] = (uint8_t)(value >> 16u);
        compressed[output_at + 3u] = (uint8_t)(value >> 24u);
    }
    if (compressed_size < 12u) goto fail;
    decompressed_size = read_be32(compressed + 8u);
    if (decompressed_size == 0u || decompressed_size > 1024u * 1024u) goto fail;
    face->bytes = (uint8_t *)malloc(decompressed_size);
    if (face->bytes == NULL ||
        !decompress_stb(face->bytes, decompressed_size,
                        compressed, compressed_size) ||
        stbtt_InitFont(&face->stb, face->bytes,
                       stbtt_GetFontOffsetForIndex(face->bytes, 0)) == 0) {
        goto fail;
    }
    face->encoded = encoded;
    face->bytes_size = decompressed_size;
    face->blob = hb_blob_create((const char *)face->bytes,
                                (unsigned int)face->bytes_size,
                                HB_MEMORY_MODE_READONLY, NULL, NULL);
    face->hb_face = hb_face_create(face->blob, 0u);
    face->hb_font = hb_font_create(face->hb_face);
    if (face->blob == hb_blob_get_empty() ||
        face->hb_face == hb_face_get_empty() ||
        face->hb_font == hb_font_get_empty()) {
        goto fail;
    }
    hb_ot_font_set_funcs(face->hb_font);
    hb_font_set_scale(face->hb_font,
                      (int)hb_face_get_upem(face->hb_face),
                      (int)hb_face_get_upem(face->hb_face));
    free(compressed);
    return true;
fail:
    free(compressed);
    release_face(face);
    return false;
}

static bool load_fonts(void) {
    if (s_fonts_ready) return true;
    if (s_fonts_failed) return false;
    if (!load_face(&s_faces[TEXT_FACE_PRIMARY],
                   MdkrCharacterText_compressed_data_base85) ||
        !load_face(&s_faces[TEXT_FACE_ARABIC],
                   MdkrCharacterTextArabic_compressed_data_base85) ||
        !load_face(&s_faces[TEXT_FACE_HEBREW],
                   MdkrCharacterTextHebrew_compressed_data_base85)) {
        size_t index;
        for (index = 0u; index < TEXT_FACE_COUNT; ++index) {
            release_face(&s_faces[index]);
        }
        s_fonts_failed = true;
        return false;
    }
    s_fonts_ready = true;
    return true;
}

static bool utf8_next(const char *source, size_t capacity, size_t *at,
                      uint32_t *codepoint, bool *terminated) {
    const unsigned char *bytes = (const unsigned char *)source;
    unsigned char first;
    size_t length;
    uint32_t value;
    uint32_t minimum;
    size_t index;
    if (*at >= capacity) return false;
    first = bytes[*at];
    if (first == 0u) {
        *terminated = true;
        return false;
    }
    if (first < 0x80u) {
        *codepoint = first;
        (*at)++;
        return true;
    }
    if (first >= 0xC2u && first <= 0xDFu) {
        length = 2u; value = first & 0x1Fu; minimum = 0x80u;
    } else if (first >= 0xE0u && first <= 0xEFu) {
        length = 3u; value = first & 0x0Fu; minimum = 0x800u;
    } else if (first >= 0xF0u && first <= 0xF4u) {
        length = 4u; value = first & 0x07u; minimum = 0x10000u;
    } else {
        return false;
    }
    if (length > capacity - *at || memchr(source + *at, '\0', length)) {
        return false;
    }
    for (index = 1u; index < length; ++index) {
        unsigned char next = bytes[*at + index];
        if ((next & 0xC0u) != 0x80u) return false;
        value = (value << 6u) | (next & 0x3Fu);
    }
    if (value < minimum || value > 0x10FFFFu ||
        (value >= 0xD800u && value <= 0xDFFFu)) return false;
    *codepoint = value;
    *at += length;
    return true;
}

static bool admitted_primary(uint32_t cp) {
    if (cp >= 0x20u && cp <= 0x7Eu) return true;
    if (cp >= 0x00A0u && cp <= 0x024Fu) return true;
    if (cp >= 0x0370u && cp <= 0x052Fu) return true;
    if (cp >= 0x1E00u && cp <= 0x1EFFu) return true;
    if (cp >= 0x2000u && cp <= 0x200Au) return true;
    if (cp >= 0x2010u && cp <= 0x2027u) return true;
    if (cp >= 0x2030u && cp <= 0x205Eu) return true;
    if (cp >= 0x20A0u && cp <= 0x20CFu) return true;
    if (cp >= 0x2100u && cp <= 0x214Fu) return true;
    if (cp >= 0xA640u && cp <= 0xA69Fu) return true;
    return false;
}

static bool unsafe_control(uint32_t cp) {
    return cp < 0x20u || (cp >= 0x7Fu && cp <= 0x9Fu) || cp == 0x00ADu ||
           cp == 0x034Fu || cp == 0x061Cu || cp == 0x200Bu ||
           cp == 0x200Eu || cp == 0x200Fu ||
           (cp >= 0x2028u && cp <= 0x202Eu) ||
           (cp >= 0x2060u && cp <= 0x206Fu) ||
           (cp >= 0xFE00u && cp <= 0xFE0Fu) || cp == 0xFEFFu ||
           (cp >= 0xE0100u && cp <= 0xE01EFu);
}

static bool arabic_codepoint(uint32_t cp) {
    return (cp >= 0x0600u && cp <= 0x06FFu) ||
           (cp >= 0x0750u && cp <= 0x077Fu) ||
           (cp >= 0x0870u && cp <= 0x089Fu) ||
           (cp >= 0x08A0u && cp <= 0x08FFu) ||
           (cp >= 0xFB50u && cp <= 0xFDFFu) ||
           (cp >= 0xFE70u && cp <= 0xFEFFu);
}

static bool hebrew_codepoint(uint32_t cp) {
    return (cp >= 0x0590u && cp <= 0x05FFu) ||
           (cp >= 0xFB1Du && cp <= 0xFB4Fu);
}

static bool combining_codepoint(uint32_t cp) {
    hb_unicode_general_category_t category = hb_unicode_general_category(
        hb_unicode_funcs_get_default(), cp);
    return category == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK ||
           category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK ||
           category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK;
}

static uint8_t preferred_face(uint32_t cp) {
    if (arabic_codepoint(cp)) return TEXT_FACE_ARABIC;
    if (hebrew_codepoint(cp)) return TEXT_FACE_HEBREW;
    return TEXT_FACE_PRIMARY;
}

static bool visible_identity_base(uint32_t cp) {
    hb_unicode_general_category_t category = hb_unicode_general_category(
        hb_unicode_funcs_get_default(), cp);
    return category != HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR &&
           category != HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR &&
           category != HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR &&
           category != HB_UNICODE_GENERAL_CATEGORY_FORMAT &&
           !combining_codepoint(cp);
}

static bool joiner_has_arabic_context(const uint32_t *codepoints,
                                      uint32_t count, uint32_t index) {
    uint32_t left = index;
    uint32_t right = index + 1u;
    while (left != 0u) {
        left--;
        if (!combining_codepoint(codepoints[left])) break;
    }
    while (right < count && combining_codepoint(codepoints[right])) right++;
    return left < index && right < count &&
           arabic_codepoint(codepoints[left]) &&
           arabic_codepoint(codepoints[right]);
}

static void set_fallback(GfxCharacterTextMetrics *metrics,
                         GfxCharacterTextFallbackReason reason) {
    if (metrics->fallback_reason == GFX_CHARACTER_TEXT_FALLBACK_NONE) {
        metrics->fallback_reason = reason;
    }
}

static uint32_t position_layout(TextLayout *layout) {
    float left = 1.0f;
    float right = 1.0f;
    float shift = 0.0f;
    uint32_t index;
    for (index = 0u; index < layout->count; ++index) {
        EmbeddedFace *face = &s_faces[layout->glyphs[index].face];
        int x0;
        int y0;
        int x1;
        int y1;
        float glyph_left;
        float glyph_right;
        stbtt_GetGlyphBitmapBox(&face->stb, layout->glyphs[index].glyph,
                                face->scale, face->scale,
                                &x0, &y0, &x1, &y1);
        (void)y0;
        (void)y1;
        glyph_left = floorf(layout->glyphs[index].pen_x) + (float)x0;
        glyph_right = floorf(layout->glyphs[index].pen_x) + (float)x1;
        if (glyph_left < left) left = glyph_left;
        if (glyph_right > right) right = glyph_right;
    }
    if (left < 1.0f) shift = 1.0f - left;
    if (shift != 0.0f) {
        for (index = 0u; index < layout->count; ++index) {
            layout->glyphs[index].pen_x += shift;
        }
        right += shift;
    }
    if (layout->advance_width + shift + 1.0f > right) {
        right = layout->advance_width + shift + 1.0f;
    }
    return (uint32_t)ceilf(right) + 1u;
}

static bool shape_segment(const uint32_t *codepoints, uint32_t total,
                          uint32_t offset, uint32_t length, uint8_t face_index,
                          hb_script_t script, hb_direction_t direction,
                          TextLayout *layout) {
    EmbeddedFace *face = &s_faces[face_index];
    hb_buffer_t *buffer = hb_buffer_create();
    hb_glyph_info_t *infos;
    hb_glyph_position_t *positions;
    unsigned int glyph_count = 0u;
    unsigned int index;
    float pen_x = layout->advance_width;
    if (buffer == hb_buffer_get_empty()) return false;
    hb_buffer_set_cluster_level(buffer,
        HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    hb_buffer_set_direction(buffer, direction);
    hb_buffer_set_script(buffer, script);
    hb_buffer_set_language(buffer, hb_language_from_string(
        script == HB_SCRIPT_ARABIC ? "ar" :
        script == HB_SCRIPT_HEBREW ? "he" : "und", -1));
    hb_buffer_add_utf32(buffer, codepoints, (int)total, offset, (int)length);
    hb_shape(face->hb_font, buffer, NULL, 0u);
    infos = hb_buffer_get_glyph_infos(buffer, &glyph_count);
    positions = hb_buffer_get_glyph_positions(buffer, NULL);
    if (glyph_count > GFX_CHARACTER_TEXT_MAX_GLYPHS - layout->count) {
        hb_buffer_destroy(buffer);
        return false;
    }
    for (index = 0u; index < glyph_count; ++index) {
        TextGlyph *glyph = &layout->glyphs[layout->count++];
        glyph->glyph = (int)infos[index].codepoint;
        glyph->face = face_index;
        glyph->pen_x = pen_x + (float)positions[index].x_offset * face->scale;
        glyph->pen_y = -(float)positions[index].y_offset * face->scale;
        pen_x += (float)positions[index].x_advance * face->scale;
    }
    layout->advance_width = pen_x;
    hb_buffer_destroy(buffer);
    return true;
}

static bool shape_codepoints(const uint32_t *codepoints, uint32_t count,
                             const uint8_t *faces, const hb_script_t *scripts,
                             uint32_t height,
                             TextLayout *layout,
                             GfxCharacterTextMetrics *metrics) {
    SBCodepointSequence sequence;
    SBAlgorithmRef algorithm = NULL;
    SBParagraphRef paragraph = NULL;
    SBLineRef line = NULL;
    const SBRun *runs;
    SBUInteger run_count;
    SBUInteger run_index;
    float top = 0.0f;
    float bottom = 0.0f;
    uint32_t face_index;
    memset(layout, 0, sizeof(*layout));
    for (face_index = 0u; face_index < TEXT_FACE_COUNT; ++face_index) {
        int ascent;
        int descent;
        int line_gap;
        EmbeddedFace *face = &s_faces[face_index];
        face->scale = stbtt_ScaleForPixelHeight(&face->stb,
                                                (float)height - 4.0f);
        stbtt_GetFontVMetrics(&face->stb, &ascent, &descent, &line_gap);
        (void)line_gap;
        if ((float)ascent * face->scale > top) {
            top = (float)ascent * face->scale;
        }
        if ((float)-descent * face->scale > bottom) {
            bottom = (float)-descent * face->scale;
        }
    }
    layout->baseline = (int)floorf(
        ((float)height - top - bottom) * 0.5f + top + 0.5f);
    sequence.stringEncoding = SBStringEncodingUTF32;
    sequence.stringBuffer = codepoints;
    sequence.stringLength = count;
    algorithm = SBAlgorithmCreate(&sequence);
    if (algorithm == NULL) goto fail;
    paragraph = SBAlgorithmCreateParagraph(algorithm, 0u, count,
                                           SBLevelDefaultLTR);
    if (paragraph == NULL) goto fail;
    line = SBParagraphCreateLine(paragraph, 0u, count);
    if (line == NULL) goto fail;
    run_count = SBLineGetRunCount(line);
    runs = SBLineGetRunsPtr(line);
    metrics->bidi_runs = (uint32_t)run_count;
    for (run_index = 0u; run_index < run_count; ++run_index) {
        const SBRun *run = &runs[run_index];
        uint32_t segment_starts[GFX_CHARACTER_TEXT_MAX_CODEPOINTS];
        uint32_t segment_lengths[GFX_CHARACTER_TEXT_MAX_CODEPOINTS];
        uint8_t segment_faces[GFX_CHARACTER_TEXT_MAX_CODEPOINTS];
        hb_script_t segment_scripts[GFX_CHARACTER_TEXT_MAX_CODEPOINTS];
        uint32_t segment_count = 0u;
        uint32_t at = (uint32_t)run->offset;
        uint32_t end = at + (uint32_t)run->length;
        while (at < end) {
            uint32_t start = at;
            uint8_t selected = faces[at++];
            hb_script_t script = scripts[start];
            while (at < end && faces[at] == selected &&
                   scripts[at] == script) at++;
            segment_starts[segment_count] = start;
            segment_lengths[segment_count] = at - start;
            segment_faces[segment_count] = selected;
            segment_scripts[segment_count] = script;
            segment_count++;
        }
        if ((run->level & 1u) != 0u) {
            metrics->right_to_left = true;
            while (segment_count != 0u) {
                uint32_t segment = --segment_count;
                if (!shape_segment(codepoints, count,
                                   segment_starts[segment],
                                   segment_lengths[segment],
                                   segment_faces[segment],
                                   segment_scripts[segment], HB_DIRECTION_RTL,
                                   layout)) goto fail;
            }
        } else {
            uint32_t segment;
            for (segment = 0u; segment < segment_count; ++segment) {
                if (!shape_segment(codepoints, count,
                                   segment_starts[segment],
                                   segment_lengths[segment],
                                   segment_faces[segment],
                                   segment_scripts[segment], HB_DIRECTION_LTR,
                                   layout)) goto fail;
            }
        }
    }
    SBLineRelease(line);
    SBParagraphRelease(paragraph);
    SBAlgorithmRelease(algorithm);
    return true;
fail:
    if (line != NULL) SBLineRelease(line);
    if (paragraph != NULL) SBParagraphRelease(paragraph);
    if (algorithm != NULL) SBAlgorithmRelease(algorithm);
    return false;
}

static void assign_context_faces(const uint32_t *codepoints, uint32_t count,
                                 uint8_t *faces) {
    uint32_t index;
    for (index = 0u; index < count; ++index) {
        faces[index] = preferred_face(codepoints[index]);
    }
    for (index = 0u; index < count; ++index) {
        hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(),
                                               codepoints[index]);
        bool contextual = script == HB_SCRIPT_COMMON ||
                          script == HB_SCRIPT_INHERITED ||
                          codepoints[index] == 0x200Cu ||
                          codepoints[index] == 0x200Du;
        if (contextual && index != 0u && faces[index - 1u] != TEXT_FACE_PRIMARY &&
            stbtt_FindGlyphIndex(&s_faces[faces[index - 1u]].stb,
                                 (int)codepoints[index]) != 0) {
            faces[index] = faces[index - 1u];
        }
    }
    for (index = count; index-- > 0u;) {
        hb_script_t script = hb_unicode_script(hb_unicode_funcs_get_default(),
                                               codepoints[index]);
        if ((script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED) &&
            faces[index] == TEXT_FACE_PRIMARY && index + 1u < count &&
            faces[index + 1u] != TEXT_FACE_PRIMARY &&
            stbtt_FindGlyphIndex(&s_faces[faces[index + 1u]].stb,
                                 (int)codepoints[index]) != 0) {
            faces[index] = faces[index + 1u];
        }
    }
}

static void assign_context_scripts(const uint32_t *codepoints, uint32_t count,
                                   hb_script_t *scripts) {
    uint32_t index;
    for (index = 0u; index < count; ++index) {
        scripts[index] = hb_unicode_script(hb_unicode_funcs_get_default(),
                                           codepoints[index]);
    }
    for (index = 1u; index < count; ++index) {
        if (scripts[index] == HB_SCRIPT_COMMON ||
            scripts[index] == HB_SCRIPT_INHERITED) {
            if (scripts[index - 1u] != HB_SCRIPT_COMMON &&
                scripts[index - 1u] != HB_SCRIPT_INHERITED) {
                scripts[index] = scripts[index - 1u];
            }
        }
    }
    for (index = count; index-- > 0u;) {
        if ((scripts[index] == HB_SCRIPT_COMMON ||
             scripts[index] == HB_SCRIPT_INHERITED) &&
            index + 1u < count) {
            scripts[index] = scripts[index + 1u];
        }
    }
}

static bool build_layout(const char *source, size_t source_capacity,
                         uint32_t max_width, uint32_t height,
                         TextLayout *layout, GfxCharacterTextMetrics *metrics) {
    size_t at = 0u;
    bool terminated = false;
    uint32_t codepoints[GFX_CHARACTER_TEXT_MAX_CODEPOINTS + 1u];
    uint8_t faces[GFX_CHARACTER_TEXT_MAX_CODEPOINTS + 1u];
    hb_script_t scripts[GFX_CHARACTER_TEXT_MAX_CODEPOINTS + 1u];
    uint32_t count = 0u;
    memset(metrics, 0, sizeof(*metrics));
    metrics->valid_utf8 = true;
    metrics->height = height;
    memset(layout, 0, sizeof(*layout));
    if (source == NULL || source_capacity == 0u || max_width == 0u ||
        max_width > GFX_CHARACTER_TEXT_MAX_WIDTH || height < 8u ||
        height > GFX_CHARACTER_TEXT_MAX_HEIGHT) {
        set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_INVALID_ARGUMENT);
        return false;
    }
    if (!load_fonts()) {
        set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_FONT_UNAVAILABLE);
        return false;
    }
    while (!terminated) {
        uint32_t cp;
        if (at >= source_capacity) {
            set_fallback(metrics,
                         GFX_CHARACTER_TEXT_FALLBACK_UNTERMINATED);
            return false;
        }
        if (!utf8_next(source, source_capacity, &at, &cp, &terminated)) {
            if (terminated) break;
            metrics->valid_utf8 = false;
            set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_INVALID_UTF8);
            return false;
        }
        metrics->input_codepoints++;
        if (cp >= 0x80u) metrics->non_ascii_codepoints++;
        if (unsafe_control(cp)) {
            set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_CONTROL);
            return false;
        }
        if (arabic_codepoint(cp) || hebrew_codepoint(cp) ||
            combining_codepoint(cp) || cp == 0x200Cu || cp == 0x200Du) {
            metrics->shaping_codepoints++;
        }
        if (!admitted_primary(cp) && !arabic_codepoint(cp) &&
            !hebrew_codepoint(cp) && cp != 0x200Cu && cp != 0x200Du &&
            !combining_codepoint(cp)) {
            metrics->missing_codepoints++;
            set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_MISSING_GLYPH);
            return false;
        }
        if (count >= GFX_CHARACTER_TEXT_MAX_CODEPOINTS) {
            set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_TOO_MANY_GLYPHS);
            return false;
        }
        codepoints[count++] = cp;
    }
    metrics->terminated = terminated;
    if (!terminated) {
        set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_UNTERMINATED);
        return false;
    }
    if (count == 0u) {
        set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_INVALID_ARGUMENT);
        return false;
    }
    {
        bool has_visible_base = false;
        bool has_mark_base = false;
        uint32_t index;
        for (index = 0u; index < count; ++index) {
            uint32_t cp = codepoints[index];
            if (cp == 0x200Cu || cp == 0x200Du) {
                if (!joiner_has_arabic_context(codepoints, count, index)) {
                    set_fallback(metrics,
                        GFX_CHARACTER_TEXT_FALLBACK_INVISIBLE_SEQUENCE);
                    return false;
                }
                continue;
            }
            if (combining_codepoint(cp)) {
                if (!has_mark_base) {
                    set_fallback(metrics,
                        GFX_CHARACTER_TEXT_FALLBACK_INVISIBLE_SEQUENCE);
                    return false;
                }
                continue;
            }
            has_mark_base = visible_identity_base(cp);
            if (has_mark_base) has_visible_base = true;
        }
        if (!has_visible_base) {
            set_fallback(metrics,
                         GFX_CHARACTER_TEXT_FALLBACK_INVISIBLE_SEQUENCE);
            return false;
        }
    }
    assign_context_faces(codepoints, count, faces);
    assign_context_scripts(codepoints, count, scripts);
    {
        uint32_t index;
        for (index = 0u; index < count; ++index) {
            if (stbtt_FindGlyphIndex(&s_faces[faces[index]].stb,
                                     (int)codepoints[index]) == 0 &&
                codepoints[index] != 0x200Cu && codepoints[index] != 0x200Du) {
                metrics->missing_codepoints++;
                set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_MISSING_GLYPH);
                return false;
            }
        }
    }
    if (!shape_codepoints(codepoints, count, faces, scripts, height, layout,
                          metrics)) {
        set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_OUT_OF_MEMORY);
        return false;
    }
    metrics->width = position_layout(layout);
    if (metrics->width > max_width) {
        uint32_t keep = count - 1u;
        for (;;) {
            while (keep != 0u &&
                   (combining_codepoint(codepoints[keep]) ||
                    codepoints[keep] == 0x200Cu || codepoints[keep] == 0x200Du)) {
                keep--;
            }
            codepoints[keep] = 0x2026u;
            assign_context_faces(codepoints, keep + 1u, faces);
            assign_context_scripts(codepoints, keep + 1u, scripts);
            if (!shape_codepoints(codepoints, keep + 1u, faces, scripts, height,
                                  layout, metrics)) {
                set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_OUT_OF_MEMORY);
                return false;
            }
            metrics->width = position_layout(layout);
            if (metrics->width <= max_width) break;
            if (keep == 0u) {
                set_fallback(metrics,
                             GFX_CHARACTER_TEXT_FALLBACK_TOO_NARROW);
                return false;
            }
            keep--;
        }
        metrics->truncated = true;
    }
    if (metrics->width == 0u) metrics->width = 1u;
    if (metrics->width > max_width) metrics->width = max_width;
    metrics->rendered_glyphs = layout->count;
    metrics->native_renderable = true;
    metrics->shaping_applied = true;
    metrics->direct_renderable = true;
    return true;
}

bool gfx_character_text_measure(const char *source, size_t source_capacity,
                                uint32_t max_width, uint32_t height,
                                GfxCharacterTextMetrics *metrics) {
    GfxCharacterTextMetrics local;
    TextLayout layout;
    bool result = build_layout(source, source_capacity, max_width, height,
                               &layout, &local);
    if (metrics != NULL) *metrics = local;
    return result;
}

static bool ensure_mask(size_t bytes) {
    uint8_t *grown;
    if (bytes <= s_mask_capacity) return s_mask != NULL;
    grown = (uint8_t *)realloc(s_mask, bytes);
    if (grown == NULL) return false;
    s_mask = grown;
    s_mask_capacity = bytes;
    return true;
}

bool gfx_character_text_render_rgba(
    const char *source, size_t source_capacity, uint32_t max_width,
    uint32_t height, uint8_t *output, size_t output_size,
    size_t output_stride, GfxCharacterTextMetrics *metrics) {
    GfxCharacterTextMetrics local;
    TextLayout layout;
    size_t mask_size;
    uint32_t glyph_index;
    uint32_t x;
    uint32_t y;
    size_t needed = 0u;
    bool output_valid = output != NULL && max_width != 0u &&
        max_width <= GFX_CHARACTER_TEXT_MAX_WIDTH && height != 0u &&
        height <= GFX_CHARACTER_TEXT_MAX_HEIGHT &&
        output_stride >= (size_t)max_width * 4u &&
        (size_t)height <= SIZE_MAX / output_stride;
    if (output_valid) needed = output_stride * height;
    if (!output_valid || output_size < needed ||
        !build_layout(source, source_capacity, max_width, height,
                      &layout, &local)) {
        if (metrics != NULL) {
            if (!output_valid || output_size < needed) {
                memset(&local, 0, sizeof(local));
                local.fallback_reason =
                    GFX_CHARACTER_TEXT_FALLBACK_INVALID_ARGUMENT;
            }
            *metrics = local;
        }
        return false;
    }
    memset(output, 0, needed);
    mask_size = (size_t)max_width * height;
    if (!ensure_mask(mask_size)) {
        local.direct_renderable = false;
        local.fallback_reason = GFX_CHARACTER_TEXT_FALLBACK_OUT_OF_MEMORY;
        if (metrics != NULL) *metrics = local;
        return false;
    }
    memset(s_mask, 0, mask_size);
    for (glyph_index = 0u; glyph_index < layout.count; ++glyph_index) {
        EmbeddedFace *face = &s_faces[layout.glyphs[glyph_index].face];
        int x0;
        int y0;
        int x1;
        int y1;
        int draw_x;
        int draw_y;
        int width;
        int glyph_height;
        stbtt_GetGlyphBitmapBox(&face->stb,
                                layout.glyphs[glyph_index].glyph,
                                face->scale, face->scale,
                                &x0, &y0, &x1, &y1);
        width = x1 - x0;
        glyph_height = y1 - y0;
        draw_x = (int)floorf(layout.glyphs[glyph_index].pen_x) + x0;
        draw_y = layout.baseline +
                 (int)floorf(layout.glyphs[glyph_index].pen_y) + y0;
        if (width <= 0 || glyph_height <= 0 || draw_x < 0 || draw_y < 0 ||
            draw_x + width > (int)max_width ||
            draw_y + glyph_height > (int)height) continue;
        stbtt_MakeGlyphBitmap(&face->stb,
            s_mask + (size_t)draw_y * max_width + (size_t)draw_x,
            width, glyph_height, (int)max_width,
            face->scale, face->scale, layout.glyphs[glyph_index].glyph);
    }
    /* Black one-pixel dilation first, then white antialiased fill. */
    for (y = 0u; y < height; ++y) {
        for (x = 0u; x < local.width; ++x) {
            uint8_t outline = 0u;
            int dy;
            int dx;
            uint8_t *pixel = output + (size_t)y * output_stride + x * 4u;
            for (dy = -1; dy <= 1; ++dy) {
                int sample_y = (int)y + dy;
                if (sample_y < 0 || sample_y >= (int)height) continue;
                for (dx = -1; dx <= 1; ++dx) {
                    int sample_x = (int)x + dx;
                    uint8_t sample;
                    if (sample_x < 0 || sample_x >= (int)max_width) continue;
                    sample = s_mask[(size_t)sample_y * max_width +
                                    (size_t)sample_x];
                    if (sample > outline) outline = sample;
                }
            }
            if (outline != 0u) pixel[3] = outline;
        }
    }
    for (y = 0u; y < height; ++y) {
        for (x = 0u; x < local.width; ++x) {
            uint8_t fill = s_mask[(size_t)y * max_width + x];
            uint8_t *pixel = output + (size_t)y * output_stride + x * 4u;
            if (fill == 0u) continue;
            pixel[0] = 255u;
            pixel[1] = 255u;
            pixel[2] = 255u;
            if (fill > pixel[3]) pixel[3] = fill;
        }
    }
    if (metrics != NULL) *metrics = local;
    return true;
}

const char *gfx_character_text_fallback_reason_name(
    GfxCharacterTextFallbackReason reason) {
    switch (reason) {
        case GFX_CHARACTER_TEXT_FALLBACK_NONE: return "none";
        case GFX_CHARACTER_TEXT_FALLBACK_INVALID_ARGUMENT: return "invalid argument";
        case GFX_CHARACTER_TEXT_FALLBACK_UNTERMINATED: return "unterminated text";
        case GFX_CHARACTER_TEXT_FALLBACK_INVALID_UTF8: return "invalid UTF-8";
        case GFX_CHARACTER_TEXT_FALLBACK_CONTROL: return "control character";
        case GFX_CHARACTER_TEXT_FALLBACK_INVISIBLE_SEQUENCE: return "unsafe invisible sequence";
        case GFX_CHARACTER_TEXT_FALLBACK_MISSING_GLYPH: return "missing glyph";
        case GFX_CHARACTER_TEXT_FALLBACK_TOO_MANY_GLYPHS: return "too many glyphs";
        case GFX_CHARACTER_TEXT_FALLBACK_FONT_UNAVAILABLE: return "font unavailable";
        case GFX_CHARACTER_TEXT_FALLBACK_OUT_OF_MEMORY: return "out of memory";
        case GFX_CHARACTER_TEXT_FALLBACK_TOO_NARROW: return "destination too narrow";
        default: return "unknown";
    }
}

void gfx_character_text_shutdown(void) {
    size_t index;
    for (index = 0u; index < TEXT_FACE_COUNT; ++index) {
        release_face(&s_faces[index]);
    }
    free(s_mask);
    s_fonts_ready = false;
    s_fonts_failed = false;
    s_mask = NULL;
    s_mask_capacity = 0u;
}
