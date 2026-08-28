#ifndef MDKR_GFX_CHARACTER_TEXT_H
#define MDKR_GFX_CHARACTER_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GFX_CHARACTER_TEXT_MAX_WIDTH = 240,
    GFX_CHARACTER_TEXT_MAX_HEIGHT = 24,
    /* Package identity fields admit at most 96 UTF-8 bytes, so this preserves
     * the entire worst-case printable-ASCII field without a hidden lower cap. */
    GFX_CHARACTER_TEXT_MAX_GLYPHS = 96,
};

typedef enum GfxCharacterTextFallbackReason {
    GFX_CHARACTER_TEXT_FALLBACK_NONE = 0,
    GFX_CHARACTER_TEXT_FALLBACK_INVALID_ARGUMENT,
    GFX_CHARACTER_TEXT_FALLBACK_UNTERMINATED,
    GFX_CHARACTER_TEXT_FALLBACK_INVALID_UTF8,
    GFX_CHARACTER_TEXT_FALLBACK_CONTROL,
    GFX_CHARACTER_TEXT_FALLBACK_SHAPING_REQUIRED,
    GFX_CHARACTER_TEXT_FALLBACK_MISSING_GLYPH,
    GFX_CHARACTER_TEXT_FALLBACK_TOO_MANY_GLYPHS,
    GFX_CHARACTER_TEXT_FALLBACK_FONT_UNAVAILABLE,
    GFX_CHARACTER_TEXT_FALLBACK_OUT_OF_MEMORY,
    GFX_CHARACTER_TEXT_FALLBACK_TOO_NARROW,
} GfxCharacterTextFallbackReason;

typedef struct GfxCharacterTextMetrics {
    uint32_t input_codepoints;
    uint32_t rendered_glyphs;
    uint32_t width;
    uint32_t height;
    uint32_t non_ascii_codepoints;
    uint32_t missing_codepoints;
    uint32_t shaping_codepoints;
    bool valid_utf8;
    bool terminated;
    bool direct_renderable;
    bool truncated;
    GfxCharacterTextFallbackReason fallback_reason;
} GfxCharacterTextMetrics;

/* Measure a bounded UTF-8 custom-character name against the project-owned
 * font. This direct path intentionally supports only non-joining LTR scripts;
 * callers must use their documented fallback when direct_renderable is false.
 * max_width and height are destination pixels, not source-font units. */
bool gfx_character_text_measure(const char *source, size_t source_capacity,
                                uint32_t max_width, uint32_t height,
                                GfxCharacterTextMetrics *metrics);

/* Render straight-alpha RGBA32. Fully transparent pixels are all-zero; glyph
 * fill is white and the one-pixel readability outline is black, allowing the
 * game texture path to tint the fill without introducing a hidden matte.
 * output_stride is bytes per row and must be at least max_width * 4. */
bool gfx_character_text_render_rgba(
    const char *source, size_t source_capacity, uint32_t max_width,
    uint32_t height, uint8_t *output, size_t output_size,
    size_t output_stride, GfxCharacterTextMetrics *metrics);

const char *gfx_character_text_fallback_reason_name(
    GfxCharacterTextFallbackReason reason);

/* Releases the lazily decompressed face and raster scratch storage. */
void gfx_character_text_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_GFX_CHARACTER_TEXT_H */
