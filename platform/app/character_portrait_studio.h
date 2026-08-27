#ifndef MDKR_APP_CHARACTER_PORTRAIT_STUDIO_H
#define MDKR_APP_CHARACTER_PORTRAIT_STUDIO_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace CharacterPortraitStudio {

constexpr int kSize = 40;
constexpr size_t kBytes = static_cast<size_t>(kSize) * kSize * 4u;

using Canvas = std::array<uint8_t, kBytes>;

enum class Sampling : uint32_t {
    Crisp = 0u,
    Smooth = 1u,
};

enum class StylePreset : uint32_t {
    Clean64 = 0u,
    Classic32 = 1u,
    Bold16 = 2u,
    Crisp32 = 3u,
    Dithered32 = 4u,
    Soft64 = 5u,
    Count = 6u,
};

enum class ReadabilitySimulation : uint32_t {
    Standard = 0u,
    Grayscale = 1u,
    Protanopia = 2u,
    Deuteranopia = 3u,
    Tritanopia = 4u,
};

constexpr size_t kStylePresetCount =
    static_cast<size_t>(StylePreset::Count);

struct Recipe {
    int zoomPercent = 100;
    int panX = 0;
    int panY = 0;
    uint32_t paletteColors = 32u;
    float ditherStrength = 0.25f;
    int outlinePixels = 1;
    int alphaThreshold = 8;
    Sampling sampling = Sampling::Smooth;
    bool fillPinholes = true;
};

struct Selection {
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

struct Analysis {
    bool empty = true;
    bool touchesEdge = false;
    bool lowOccupancy = false;
    bool lowContrast = false;
    uint32_t visiblePixels = 0u;
    uint32_t semitransparentPixels = 0u;
    uint32_t enclosedTransparentPixels = 0u;
    uint32_t uniqueVisibleColors = 0u;
    int minimumX = 0;
    int minimumY = 0;
    int maximumX = -1;
    int maximumY = -1;
    float canvasOccupancy = 0.0f;
    float boundsOccupancy = 0.0f;
    uint8_t luminanceRange = 0u;
};

bool validRecipe(const Recipe &recipe);
/* Preserve framing/cleanup while replacing only the visual treatment fields.
 * An invalid recipe or enum is returned field-for-field unchanged. */
Recipe presetRecipe(const Recipe &base, StylePreset preset);
Canvas applyRecipe(const Canvas &source, const Recipe &recipe);
Analysis analyse(const Canvas &canvas, uint8_t visibleAlpha = 16u);

/* Produce one opaque, deterministic authoring stress view. The straight-RGBA
 * portrait is composited over the supplied background before the selected
 * colour-vision transform. These views help authors spot dependence on hue;
 * they are deliberately not presented as clinical simulations or screenshots
 * of a particular game scene. Unknown simulations return the standard view. */
Canvas readabilityProof(
    const Canvas &source, const std::array<uint8_t, 3> &background,
    ReadabilitySimulation simulation);

bool moveSelection(Canvas &canvas, Selection selection, int deltaX,
                   int deltaY, bool copy);
uint32_t replaceColour(Canvas &canvas, const uint8_t from[4],
                       const uint8_t to[4], uint8_t tolerance);

}  // namespace CharacterPortraitStudio

#endif  // MDKR_APP_CHARACTER_PORTRAIT_STUDIO_H
