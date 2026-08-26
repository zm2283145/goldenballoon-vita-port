#include "character_portrait_studio.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>

namespace {

using namespace CharacterPortraitStudio;

uint8_t *pixel(Canvas &canvas, int x, int y) {
    return canvas.data() + static_cast<size_t>(y * kSize + x) * 4u;
}

const uint8_t *pixel(const Canvas &canvas, int x, int y) {
    return canvas.data() + static_cast<size_t>(y * kSize + x) * 4u;
}

void setPixel(Canvas &canvas, int x, int y, uint8_t red, uint8_t green,
              uint8_t blue, uint8_t alpha = 255u) {
    const uint8_t rgba[4] = {red, green, blue, alpha};
    std::memcpy(pixel(canvas, x, y), rgba, sizeof(rgba));
}

std::set<uint32_t> visibleColours(const Canvas &canvas) {
    std::set<uint32_t> result;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const uint8_t *rgba = pixel(canvas, x, y);
            if (rgba[3] == 0u) continue;
            result.insert(static_cast<uint32_t>(rgba[0]) |
                          static_cast<uint32_t>(rgba[1]) << 8u |
                          static_cast<uint32_t>(rgba[2]) << 16u);
        }
    }
    return result;
}

}  // namespace

int main() {
    Recipe defaults;
    assert(validRecipe(defaults));
    Recipe invalid = defaults;
    invalid.paletteColors = 15u;
    assert(!validRecipe(invalid));
    invalid = defaults;
    invalid.ditherStrength = 1.01f;
    assert(!validRecipe(invalid));
    invalid = defaults;
    invalid.zoomPercent = 0;
    assert(!validRecipe(invalid));

    Canvas gradient{};
    for (int y = 8; y < 32; ++y) {
        for (int x = 8; x < 32; ++x) {
            setPixel(gradient, x, y, static_cast<uint8_t>(x * 6),
                     static_cast<uint8_t>(y * 6),
                     static_cast<uint8_t>((x + y) * 3));
        }
    }
    assert(applyRecipe(gradient, invalid) == gradient);

    Recipe reduced;
    reduced.paletteColors = 16u;
    reduced.ditherStrength = 0.4f;
    reduced.outlinePixels = 1;
    const Canvas first = applyRecipe(gradient, reduced);
    const Canvas second = applyRecipe(gradient, reduced);
    assert(first == second);
    assert(visibleColours(first).size() <= reduced.paletteColors);
    assert(pixel(first, 7, 8)[3] != 0u);
    assert(pixel(first, 6, 8)[3] == 0u);

    Recipe crisp = reduced;
    crisp.sampling = Sampling::Crisp;
    crisp.zoomPercent = 200;
    crisp.panX = 2;
    crisp.panY = -3;
    const Canvas reframed = applyRecipe(gradient, crisp);
    assert(reframed != first);
    assert(!analyse(reframed).empty);

    Canvas hole{};
    for (int y = 10; y <= 12; ++y) {
        for (int x = 10; x <= 12; ++x) {
            if (x != 11 || y != 11) setPixel(hole, x, y, 200, 100, 50);
        }
    }
    const Analysis beforeFill = analyse(hole);
    assert(beforeFill.enclosedTransparentPixels == 1u);
    Recipe fill;
    fill.paletteColors = 16u;
    fill.outlinePixels = 0;
    fill.ditherStrength = 0.0f;
    fill.fillPinholes = true;
    const Canvas filled = applyRecipe(hole, fill);
    assert(pixel(filled, 11, 11)[3] == 255u);
    assert(analyse(filled).enclosedTransparentPixels == 0u);

    Canvas clipped{};
    setPixel(clipped, 0, 0, 120, 120, 120);
    Analysis clippedAnalysis = analyse(clipped);
    assert(clippedAnalysis.touchesEdge);
    assert(clippedAnalysis.lowOccupancy);
    assert(clippedAnalysis.lowContrast);
    assert(clippedAnalysis.visiblePixels == 1u);
    assert(clippedAnalysis.minimumX == 0 && clippedAnalysis.maximumX == 0);
    assert(analyse(Canvas{}).empty);
    assert(analyse(Canvas{}, 0u).empty);

    Canvas moved{};
    setPixel(moved, 2, 2, 255, 0, 0);
    setPixel(moved, 3, 2, 0, 255, 0);
    assert(moveSelection(moved, {2, 2, 2, 1}, 1, 1, false));
    assert(pixel(moved, 2, 2)[3] == 0u);
    assert(pixel(moved, 3, 3)[0] == 255u);
    assert(pixel(moved, 4, 3)[1] == 255u);
    const Canvas movedOnce = moved;
    assert(!moveSelection(moved, {0, 0, 0, 1}, 1, 0, false));
    assert(!moveSelection(moved, {INT_MAX, 0, INT_MAX, 1}, 1, 0, false));
    assert(moved == movedOnce);
    assert(moveSelection(moved, {3, 3, 2, 1}, -2, 0, true));
    assert(pixel(moved, 1, 3)[0] == 255u);
    assert(pixel(moved, 3, 3)[0] == 255u);
    Canvas clippedMove{};
    setPixel(clippedMove, 0, 0, 1, 2, 3);
    assert(moveSelection(clippedMove, {0, 0, 1, 1}, -40, -40, false));
    assert(clippedMove == Canvas{});
    assert(!moveSelection(clippedMove, {0, 0, 1, 1}, 1, 0, true));

    Canvas alphaVariants{};
    setPixel(alphaVariants, 1, 1, 9, 8, 7, 40);
    setPixel(alphaVariants, 2, 1, 9, 8, 7, 200);
    assert(analyse(alphaVariants).uniqueVisibleColors == 1u);

    Canvas palette{};
    setPixel(palette, 1, 1, 100, 110, 120, 255);
    setPixel(palette, 2, 1, 103, 110, 120, 255);
    setPixel(palette, 3, 1, 150, 110, 120, 255);
    const uint8_t from[4] = {100, 110, 120, 255};
    const uint8_t to[4] = {10, 20, 30, 255};
    assert(replaceColour(palette, from, to, 3u) == 2u);
    assert(std::memcmp(pixel(palette, 1, 1), to, 4u) == 0);
    assert(std::memcmp(pixel(palette, 2, 1), to, 4u) == 0);
    assert(pixel(palette, 3, 1)[0] == 150u);
    assert(replaceColour(palette, to, to, 0u) == 0u);
    assert(replaceColour(palette, nullptr, to, 0u) == 0u);

    std::cout << "character_portrait_studio: PASS\n";
    return 0;
}
