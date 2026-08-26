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
Canvas applyRecipe(const Canvas &source, const Recipe &recipe);
Analysis analyse(const Canvas &canvas, uint8_t visibleAlpha = 16u);

bool moveSelection(Canvas &canvas, Selection selection, int deltaX,
                   int deltaY, bool copy);
uint32_t replaceColour(Canvas &canvas, const uint8_t from[4],
                       const uint8_t to[4], uint8_t tolerance);

}  // namespace CharacterPortraitStudio

#endif  // MDKR_APP_CHARACTER_PORTRAIT_STUDIO_H
