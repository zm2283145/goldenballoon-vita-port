#include "gfx_character_text.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct DecodeState {
    const uint8_t *input_begin;
    const uint8_t *input_end;
    uint8_t *output_begin;
    uint8_t *output_at;
    uint8_t *output_end;
} DecodeState;

typedef struct TextGlyph {
    int glyph;
    uint32_t codepoint;
    float pen_x;
} TextGlyph;

typedef struct TextLayout {
    TextGlyph glyphs[GFX_CHARACTER_TEXT_MAX_GLYPHS];
    uint32_t count;
    float scale;
    float advance_width;
    int baseline;
} TextLayout;

static uint8_t *s_font_bytes;
static stbtt_fontinfo s_font;
static bool s_font_ready;
static bool s_font_failed;
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

static bool load_font(void) {
    const char *encoded = MdkrCharacterText_compressed_data_base85;
    size_t encoded_size;
    size_t compressed_size;
    uint8_t *compressed = NULL;
    size_t source_at;
    size_t output_at;
    uint32_t decompressed_size;
    if (s_font_ready) return true;
    if (s_font_failed) return false;
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
    s_font_bytes = (uint8_t *)malloc(decompressed_size);
    if (s_font_bytes == NULL ||
        !decompress_stb(s_font_bytes, decompressed_size,
                        compressed, compressed_size) ||
        stbtt_InitFont(&s_font, s_font_bytes,
                       stbtt_GetFontOffsetForIndex(s_font_bytes, 0)) == 0) {
        goto fail;
    }
    free(compressed);
    s_font_ready = true;
    return true;
fail:
    free(compressed);
    free(s_font_bytes);
    s_font_bytes = NULL;
    s_font_failed = true;
    return false;
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

static bool requires_shaping(uint32_t cp) {
    return (cp >= 0x0300u && cp <= 0x036Fu) ||
           (cp >= 0x0483u && cp <= 0x0489u) ||
           (cp >= 0x0590u && cp <= 0x08FFu) ||
           (cp >= 0x1AB0u && cp <= 0x1AFFu) ||
           (cp >= 0x1DC0u && cp <= 0x1DFFu) ||
           (cp >= 0x20D0u && cp <= 0x20FFu) ||
           (cp >= 0x2DE0u && cp <= 0x2DFFu) ||
           (cp >= 0xA674u && cp <= 0xA67Du) ||
           (cp >= 0xFE00u && cp <= 0xFE2Fu) ||
           (cp >= 0x1F1E6u && cp <= 0x1FAFFu);
}

static bool admitted_ltr(uint32_t cp) {
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
    layout->advance_width = 0.0f;
    for (index = 0u; index < layout->count; ++index) {
        int advance;
        int bearing;
        int x0;
        int y0;
        int x1;
        int y1;
        float glyph_left;
        float glyph_right;
        layout->glyphs[index].pen_x = layout->advance_width + 1.0f;
        stbtt_GetGlyphBitmapBox(&s_font, layout->glyphs[index].glyph,
                                layout->scale, layout->scale,
                                &x0, &y0, &x1, &y1);
        (void)y0;
        (void)y1;
        glyph_left = floorf(layout->glyphs[index].pen_x) + (float)x0;
        glyph_right = floorf(layout->glyphs[index].pen_x) + (float)x1;
        if (glyph_left < left) left = glyph_left;
        if (glyph_right > right) right = glyph_right;
        stbtt_GetGlyphHMetrics(&s_font, layout->glyphs[index].glyph,
                              &advance, &bearing);
        (void)bearing;
        layout->advance_width += (float)advance * layout->scale;
        if (index + 1u < layout->count) {
            layout->advance_width += (float)stbtt_GetGlyphKernAdvance(
                &s_font, layout->glyphs[index].glyph,
                layout->glyphs[index + 1u].glyph) * layout->scale;
        }
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

static bool build_layout(const char *source, size_t source_capacity,
                         uint32_t max_width, uint32_t height,
                         TextLayout *layout, GfxCharacterTextMetrics *metrics) {
    size_t at = 0u;
    bool terminated = false;
    int ascent;
    int descent;
    int line_gap;
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
    if (!load_font()) {
        set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_FONT_UNAVAILABLE);
        return false;
    }
    while (!terminated) {
        uint32_t cp;
        int glyph;
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
        if (cp < 0x20u || cp == 0x7Fu ||
            (cp >= 0x2028u && cp <= 0x202Fu) ||
            (cp >= 0x2060u && cp <= 0x206Fu)) {
            set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_CONTROL);
            return false;
        }
        if (requires_shaping(cp) || !admitted_ltr(cp)) {
            metrics->shaping_codepoints++;
            set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_SHAPING_REQUIRED);
            return false;
        }
        glyph = stbtt_FindGlyphIndex(&s_font, (int)cp);
        if (glyph == 0) {
            metrics->missing_codepoints++;
            set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_MISSING_GLYPH);
            return false;
        }
        if (layout->count >= GFX_CHARACTER_TEXT_MAX_GLYPHS) {
            set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_TOO_MANY_GLYPHS);
            return false;
        }
        layout->glyphs[layout->count].glyph = glyph;
        layout->glyphs[layout->count].codepoint = cp;
        layout->count++;
    }
    metrics->terminated = terminated;
    if (!terminated) {
        set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_UNTERMINATED);
        return false;
    }
    if (layout->count == 0u) {
        set_fallback(metrics, GFX_CHARACTER_TEXT_FALLBACK_INVALID_ARGUMENT);
        return false;
    }
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &line_gap);
    (void)line_gap;
    layout->scale = stbtt_ScaleForPixelHeight(&s_font, (float)height - 4.0f);
    layout->baseline = (int)floorf(
        ((float)height - (float)(ascent - descent) * layout->scale) * 0.5f +
        (float)ascent * layout->scale + 0.5f);
    metrics->width = position_layout(layout);
    if (metrics->width > max_width) {
        int ellipsis = stbtt_FindGlyphIndex(&s_font, 0x2026);
        uint32_t keep = layout->count - 1u;
        if (ellipsis == 0) {
            metrics->missing_codepoints++;
            set_fallback(metrics,
                         GFX_CHARACTER_TEXT_FALLBACK_MISSING_GLYPH);
            return false;
        }
        for (;;) {
            layout->count = keep + 1u;
            layout->glyphs[keep].glyph = ellipsis;
            layout->glyphs[keep].codepoint = 0x2026u;
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
        (size_t)max_width <= SIZE_MAX / 4u &&
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
        int x0;
        int y0;
        int x1;
        int y1;
        int draw_x;
        int draw_y;
        int width;
        int glyph_height;
        stbtt_GetGlyphBitmapBox(&s_font, layout.glyphs[glyph_index].glyph,
                                layout.scale, layout.scale,
                                &x0, &y0, &x1, &y1);
        width = x1 - x0;
        glyph_height = y1 - y0;
        draw_x = (int)floorf(layout.glyphs[glyph_index].pen_x) + x0;
        draw_y = layout.baseline + y0;
        if (width <= 0 || glyph_height <= 0 || draw_x < 0 || draw_y < 0 ||
            draw_x + width > (int)max_width ||
            draw_y + glyph_height > (int)height) continue;
        stbtt_MakeGlyphBitmap(&s_font,
            s_mask + (size_t)draw_y * max_width + (size_t)draw_x,
            width, glyph_height, (int)max_width,
            layout.scale, layout.scale, layout.glyphs[glyph_index].glyph);
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
        case GFX_CHARACTER_TEXT_FALLBACK_SHAPING_REQUIRED: return "shaping or bidi required";
        case GFX_CHARACTER_TEXT_FALLBACK_MISSING_GLYPH: return "missing glyph";
        case GFX_CHARACTER_TEXT_FALLBACK_TOO_MANY_GLYPHS: return "too many glyphs";
        case GFX_CHARACTER_TEXT_FALLBACK_FONT_UNAVAILABLE: return "font unavailable";
        case GFX_CHARACTER_TEXT_FALLBACK_OUT_OF_MEMORY: return "out of memory";
        case GFX_CHARACTER_TEXT_FALLBACK_TOO_NARROW: return "destination too narrow";
        default: return "unknown";
    }
}

void gfx_character_text_shutdown(void) {
    free(s_font_bytes);
    free(s_mask);
    s_font_bytes = NULL;
    s_font_ready = false;
    s_font_failed = false;
    s_mask = NULL;
    s_mask_capacity = 0u;
    memset(&s_font, 0, sizeof(s_font));
}
