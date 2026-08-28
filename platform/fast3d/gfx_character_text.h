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
    GFX_CHARACTER_TEXT_MAX_CODEPOINTS = 96,
    /* OpenType substitutions can expand one input character into multiple
     * positioned glyphs. Keep that bounded without imposing an ASCII-shaped
     * limit on joining scripts. */
    GFX_CHARACTER_TEXT_MAX_GLYPHS = 192,
};

typedef enum GfxCharacterTextFallbackReason {
    GFX_CHARACTER_TEXT_FALLBACK_NONE = 0,
    GFX_CHARACTER_TEXT_FALLBACK_INVALID_ARGUMENT,
    GFX_CHARACTER_TEXT_FALLBACK_UNTERMINATED,
    GFX_CHARACTER_TEXT_FALLBACK_INVALID_UTF8,
    GFX_CHARACTER_TEXT_FALLBACK_CONTROL,
    GFX_CHARACTER_TEXT_FALLBACK_INVISIBLE_SEQUENCE,
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
    uint32_t bidi_runs;
    bool valid_utf8;
    bool terminated;
    bool native_renderable;
    bool shaping_applied;
    bool right_to_left;
    /* Compatibility name retained for callers from the original LTR renderer;
     * true means the project-owned native renderer handled the full string. */
    bool direct_renderable;
    bool truncated;
    GfxCharacterTextFallbackReason fallback_reason;
} GfxCharacterTextMetrics;

/* Measure bounded UTF-8 against the project-owned, deterministic OpenType
 * stack. Latin/Greek/Cyrillic, Arabic and Hebrew are shaped natively; malformed,
 * unsafe-control, or uncovered text fails closed to the documented retail-font
 * fallback. max_width and height are destination pixels, not font units. */
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

/* The launcher merges these same immutable script faces into its UI atlas so
 * authored names remain readable in fields and library rows before the exact
 * shaped game preview. The returned base85 storage has process lifetime. */
const char *gfx_character_text_latin_face_base85(void);
const char *gfx_character_text_arabic_face_base85(void);
const char *gfx_character_text_hebrew_face_base85(void);

/* Releases the lazily decompressed face and raster scratch storage. */
void gfx_character_text_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_GFX_CHARACTER_TEXT_H */
