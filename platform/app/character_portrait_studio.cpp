#include "character_portrait_studio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using CharacterPortraitStudio::Canvas;
using CharacterPortraitStudio::Recipe;
using CharacterPortraitStudio::Selection;

constexpr int kSize = CharacterPortraitStudio::kSize;
constexpr int kPixels = kSize * kSize;

size_t pixelOffset(int x, int y) {
    return static_cast<size_t>(y * kSize + x) * 4u;
}

uint8_t clampByte(float value) {
    return static_cast<uint8_t>(std::lround(
        std::clamp(value, 0.0f, 255.0f)));
}

std::array<uint8_t, 4> sampleNearest(const Canvas &source, float x, float y) {
    const int sourceX = static_cast<int>(std::floor(x + 0.5f));
    const int sourceY = static_cast<int>(std::floor(y + 0.5f));
    if (sourceX < 0 || sourceX >= kSize || sourceY < 0 || sourceY >= kSize) {
        return {};
    }
    std::array<uint8_t, 4> result{};
    std::memcpy(result.data(), source.data() + pixelOffset(sourceX, sourceY),
                result.size());
    return result;
}

std::array<uint8_t, 4> sampleSmooth(const Canvas &source, float x, float y) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    float premultiplied[4] = {};
    for (int offsetY = 0; offsetY <= 1; ++offsetY) {
        for (int offsetX = 0; offsetX <= 1; ++offsetX) {
            const int sourceX = x0 + offsetX;
            const int sourceY = y0 + offsetY;
            if (sourceX < 0 || sourceX >= kSize ||
                sourceY < 0 || sourceY >= kSize) continue;
            const float weight = (offsetX == 0 ? 1.0f - fx : fx) *
                                 (offsetY == 0 ? 1.0f - fy : fy);
            const uint8_t *pixel =
                source.data() + pixelOffset(sourceX, sourceY);
            const float alpha = static_cast<float>(pixel[3]) / 255.0f;
            for (int component = 0; component < 3; ++component) {
                premultiplied[component] +=
                    static_cast<float>(pixel[component]) * alpha * weight;
            }
            premultiplied[3] += static_cast<float>(pixel[3]) * weight;
        }
    }
    std::array<uint8_t, 4> result{};
    result[3] = clampByte(premultiplied[3]);
    if (premultiplied[3] > 0.0f) {
        const float inverseAlpha = 255.0f / premultiplied[3];
        for (int component = 0; component < 3; ++component) {
            result[component] = clampByte(
                premultiplied[component] * inverseAlpha);
        }
    }
    return result;
}

Canvas reframe(const Canvas &source, const Recipe &recipe) {
    Canvas result{};
    const float scale = 100.0f / static_cast<float>(recipe.zoomPercent);
    constexpr float centre = (kSize - 1) * 0.5f;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float sourceX = centre +
                (static_cast<float>(x) - centre) * scale - recipe.panX;
            const float sourceY = centre +
                (static_cast<float>(y) - centre) * scale - recipe.panY;
            const std::array<uint8_t, 4> pixel =
                recipe.sampling == CharacterPortraitStudio::Sampling::Crisp
                    ? sampleNearest(source, sourceX, sourceY)
                    : sampleSmooth(source, sourceX, sourceY);
            std::memcpy(result.data() + pixelOffset(x, y), pixel.data(),
                        pixel.size());
        }
    }
    return result;
}

void applyAlphaThreshold(Canvas &canvas, int threshold) {
    for (int pixel = 0; pixel < kPixels; ++pixel) {
        uint8_t *rgba = canvas.data() + pixel * 4;
        if (rgba[3] < threshold) {
            std::memset(rgba, 0, 4u);
        }
    }
}

void fillPinholes(Canvas &canvas) {
    const Canvas before = canvas;
    for (int y = 1; y + 1 < kSize; ++y) {
        for (int x = 1; x + 1 < kSize; ++x) {
            uint8_t *destination = canvas.data() + pixelOffset(x, y);
            if (destination[3] != 0u) continue;
            const int neighbours[4][2] = {
                {x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1},
            };
            unsigned visible = 0u;
            unsigned sums[4] = {};
            for (const auto &neighbour : neighbours) {
                const uint8_t *source = before.data() +
                    pixelOffset(neighbour[0], neighbour[1]);
                if (source[3] == 0u) continue;
                ++visible;
                for (int component = 0; component < 4; ++component) {
                    sums[component] += source[component];
                }
            }
            if (visible == 4u) {
                for (int component = 0; component < 4; ++component) {
                    destination[component] = static_cast<uint8_t>(
                        (sums[component] + visible / 2u) / visible);
                }
            }
        }
    }
}

void addOutline(Canvas &canvas, int pixels) {
    for (int pass = 0; pass < pixels; ++pass) {
        const Canvas before = canvas;
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                uint8_t *destination = canvas.data() + pixelOffset(x, y);
                if (destination[3] != 0u) continue;
                unsigned bestAlpha = 0u;
                const uint8_t *best = nullptr;
                for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                        if (offsetX == 0 && offsetY == 0) continue;
                        const int sourceX = x + offsetX;
                        const int sourceY = y + offsetY;
                        if (sourceX < 0 || sourceX >= kSize ||
                            sourceY < 0 || sourceY >= kSize) continue;
                        const uint8_t *candidate = before.data() +
                            pixelOffset(sourceX, sourceY);
                        if (candidate[3] > bestAlpha) {
                            bestAlpha = candidate[3];
                            best = candidate;
                        }
                    }
                }
                if (best != nullptr && bestAlpha >= 16u) {
                    destination[0] = static_cast<uint8_t>(best[0] / 4u);
                    destination[1] = static_cast<uint8_t>(best[1] / 4u);
                    destination[2] = static_cast<uint8_t>(best[2] / 4u);
                    destination[3] = best[3];
                }
            }
        }
    }
}

struct PaletteBox {
    std::vector<int> pixels;
    uint8_t minimum[3] = {};
    uint8_t maximum[3] = {};
};

void measureBox(PaletteBox &box, const Canvas &canvas) {
    std::fill(std::begin(box.minimum), std::end(box.minimum), 255u);
    std::fill(std::begin(box.maximum), std::end(box.maximum), 0u);
    for (int pixel : box.pixels) {
        const uint8_t *rgba = canvas.data() + pixel * 4;
        for (int component = 0; component < 3; ++component) {
            box.minimum[component] = std::min(box.minimum[component],
                                               rgba[component]);
            box.maximum[component] = std::max(box.maximum[component],
                                               rgba[component]);
        }
    }
}

std::vector<std::array<uint8_t, 3>> buildPalette(const Canvas &canvas,
                                                  uint32_t maximumColors) {
    PaletteBox initial;
    for (int pixel = 0; pixel < kPixels; ++pixel) {
        if (canvas[static_cast<size_t>(pixel) * 4u + 3u] != 0u) {
            initial.pixels.push_back(pixel);
        }
    }
    if (initial.pixels.empty()) return {};
    measureBox(initial, canvas);
    std::vector<PaletteBox> boxes;
    boxes.push_back(std::move(initial));
    while (boxes.size() < maximumColors) {
        size_t splitIndex = boxes.size();
        unsigned bestRange = 0u;
        size_t bestPopulation = 0u;
        for (size_t index = 0u; index < boxes.size(); ++index) {
            if (boxes[index].pixels.size() < 2u) continue;
            unsigned range = 0u;
            for (int component = 0; component < 3; ++component) {
                range = std::max(range, static_cast<unsigned>(
                    boxes[index].maximum[component] -
                    boxes[index].minimum[component]));
            }
            if (splitIndex == boxes.size() || range > bestRange ||
                (range == bestRange &&
                 boxes[index].pixels.size() > bestPopulation)) {
                splitIndex = index;
                bestRange = range;
                bestPopulation = boxes[index].pixels.size();
            }
        }
        if (splitIndex == boxes.size() || bestRange == 0u) break;
        PaletteBox &source = boxes[splitIndex];
        int channel = 0;
        for (int component = 1; component < 3; ++component) {
            if (source.maximum[component] - source.minimum[component] >
                source.maximum[channel] - source.minimum[channel]) {
                channel = component;
            }
        }
        std::stable_sort(source.pixels.begin(), source.pixels.end(),
            [&canvas, channel](int left, int right) {
                const uint8_t *a = canvas.data() + left * 4;
                const uint8_t *b = canvas.data() + right * 4;
                if (a[channel] != b[channel]) return a[channel] < b[channel];
                if (a[(channel + 1) % 3] != b[(channel + 1) % 3]) {
                    return a[(channel + 1) % 3] < b[(channel + 1) % 3];
                }
                if (a[(channel + 2) % 3] != b[(channel + 2) % 3]) {
                    return a[(channel + 2) % 3] < b[(channel + 2) % 3];
                }
                return left < right;
            });
        const size_t middle = source.pixels.size() / 2u;
        PaletteBox upper;
        upper.pixels.assign(source.pixels.begin() + middle,
                            source.pixels.end());
        source.pixels.erase(source.pixels.begin() + middle,
                            source.pixels.end());
        measureBox(source, canvas);
        measureBox(upper, canvas);
        boxes.push_back(std::move(upper));
    }
    std::vector<std::array<uint8_t, 3>> palette;
    palette.reserve(boxes.size());
    for (const PaletteBox &box : boxes) {
        uint64_t sums[3] = {};
        uint64_t weight = 0u;
        for (int pixel : box.pixels) {
            const uint8_t *rgba = canvas.data() + pixel * 4;
            const uint64_t alpha = std::max<uint8_t>(rgba[3], 1u);
            weight += alpha;
            for (int component = 0; component < 3; ++component) {
                sums[component] += static_cast<uint64_t>(rgba[component]) *
                                   alpha;
            }
        }
        std::array<uint8_t, 3> colour{};
        for (int component = 0; component < 3; ++component) {
            colour[component] = static_cast<uint8_t>(
                (sums[component] + weight / 2u) / weight);
        }
        palette.push_back(colour);
    }
    return palette;
}

void quantize(Canvas &canvas, uint32_t maximumColors, float strength) {
    const auto palette = buildPalette(canvas, maximumColors);
    if (palette.empty()) return;
    static constexpr int kBayer[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6},
        {3, 11, 1, 9}, {15, 7, 13, 5},
    };
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            uint8_t *rgba = canvas.data() + pixelOffset(x, y);
            if (rgba[3] == 0u) continue;
            const float bias =
                (static_cast<float>(kBayer[y & 3][x & 3]) - 7.5f) *
                (32.0f / 7.5f) * strength;
            size_t best = 0u;
            uint64_t bestDistance = std::numeric_limits<uint64_t>::max();
            for (size_t index = 0u; index < palette.size(); ++index) {
                uint64_t distance = 0u;
                for (int component = 0; component < 3; ++component) {
                    const int adjusted = std::clamp(
                        static_cast<int>(std::lround(rgba[component] + bias)),
                        0, 255);
                    const int delta = adjusted - palette[index][component];
                    distance += static_cast<uint64_t>(delta * delta);
                }
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = index;
                }
            }
            for (int component = 0; component < 3; ++component) {
                rgba[component] = palette[best][component];
            }
        }
    }
}

uint8_t luminance(const uint8_t *rgba) {
    return static_cast<uint8_t>((54u * rgba[0] + 183u * rgba[1] +
                                 19u * rgba[2] + 128u) >> 8u);
}

}  // namespace

namespace CharacterPortraitStudio {

bool validRecipe(const Recipe &recipe) {
    return recipe.zoomPercent >= 50 && recipe.zoomPercent <= 250 &&
           recipe.panX >= -40 && recipe.panX <= 40 &&
           recipe.panY >= -40 && recipe.panY <= 40 &&
           (recipe.paletteColors == 16u || recipe.paletteColors == 32u ||
            recipe.paletteColors == 64u) &&
           std::isfinite(recipe.ditherStrength) &&
           recipe.ditherStrength >= 0.0f && recipe.ditherStrength <= 1.0f &&
           recipe.outlinePixels >= 0 && recipe.outlinePixels <= 2 &&
           recipe.alphaThreshold >= 0 && recipe.alphaThreshold <= 255 &&
           (recipe.sampling == Sampling::Crisp ||
            recipe.sampling == Sampling::Smooth);
}

Recipe presetRecipe(const Recipe &base, StylePreset preset) {
    if (!validRecipe(base)) return base;
    Recipe result = base;
    switch (preset) {
        case StylePreset::Clean64:
            result.paletteColors = 64u;
            result.ditherStrength = 0.0f;
            result.outlinePixels = 1;
            result.sampling = Sampling::Smooth;
            break;
        case StylePreset::Classic32:
            result.paletteColors = 32u;
            result.ditherStrength = 0.25f;
            result.outlinePixels = 1;
            result.sampling = Sampling::Smooth;
            break;
        case StylePreset::Bold16:
            result.paletteColors = 16u;
            result.ditherStrength = 0.15f;
            result.outlinePixels = 2;
            result.sampling = Sampling::Smooth;
            break;
        case StylePreset::Crisp32:
            result.paletteColors = 32u;
            result.ditherStrength = 0.0f;
            result.outlinePixels = 1;
            result.sampling = Sampling::Crisp;
            break;
        case StylePreset::Dithered32:
            result.paletteColors = 32u;
            result.ditherStrength = 0.65f;
            result.outlinePixels = 1;
            result.sampling = Sampling::Smooth;
            break;
        case StylePreset::Soft64:
            result.paletteColors = 64u;
            result.ditherStrength = 0.10f;
            result.outlinePixels = 0;
            result.sampling = Sampling::Smooth;
            break;
        case StylePreset::Count:
        default:
            return base;
    }
    return result;
}

Canvas applyRecipe(const Canvas &source, const Recipe &recipe) {
    if (!validRecipe(recipe)) return source;
    Canvas result = reframe(source, recipe);
    applyAlphaThreshold(result, recipe.alphaThreshold);
    if (recipe.fillPinholes) fillPinholes(result);
    addOutline(result, recipe.outlinePixels);
    quantize(result, recipe.paletteColors, recipe.ditherStrength);
    return result;
}

Analysis analyse(const Canvas &canvas, uint8_t visibleAlpha) {
    Analysis result;
    const uint8_t threshold = std::max<uint8_t>(visibleAlpha, 1u);
    result.minimumX = kSize;
    result.minimumY = kSize;
    uint8_t minimumLuminance = 255u;
    uint8_t maximumLuminance = 0u;
    std::vector<uint32_t> colours;
    colours.reserve(kPixels);
    std::array<uint8_t, kPixels> exterior{};
    std::array<int, kPixels> queue{};
    int read = 0;
    int write = 0;
    const auto enqueueExterior = [&](int x, int y) {
        const int index = y * kSize + x;
        if (exterior[index] != 0u ||
            canvas[static_cast<size_t>(index) * 4u + 3u] >= threshold) {
            return;
        }
        exterior[index] = 1u;
        queue[write++] = index;
    };
    for (int coordinate = 0; coordinate < kSize; ++coordinate) {
        enqueueExterior(coordinate, 0);
        enqueueExterior(coordinate, kSize - 1);
        enqueueExterior(0, coordinate);
        enqueueExterior(kSize - 1, coordinate);
    }
    while (read < write) {
        const int index = queue[read++];
        const int x = index % kSize;
        const int y = index / kSize;
        if (x > 0) enqueueExterior(x - 1, y);
        if (x + 1 < kSize) enqueueExterior(x + 1, y);
        if (y > 0) enqueueExterior(x, y - 1);
        if (y + 1 < kSize) enqueueExterior(x, y + 1);
    }
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const int index = y * kSize + x;
            const uint8_t *rgba = canvas.data() + index * 4;
            if (rgba[3] < threshold) {
                if (rgba[3] != 0u) ++result.semitransparentPixels;
                if (exterior[index] == 0u) {
                    ++result.enclosedTransparentPixels;
                }
                continue;
            }
            result.empty = false;
            ++result.visiblePixels;
            result.minimumX = std::min(result.minimumX, x);
            result.minimumY = std::min(result.minimumY, y);
            result.maximumX = std::max(result.maximumX, x);
            result.maximumY = std::max(result.maximumY, y);
            if (x == 0 || x + 1 == kSize || y == 0 || y + 1 == kSize) {
                result.touchesEdge = true;
            }
            const uint8_t value = luminance(rgba);
            minimumLuminance = std::min(minimumLuminance, value);
            maximumLuminance = std::max(maximumLuminance, value);
            colours.push_back(static_cast<uint32_t>(rgba[0]) |
                              static_cast<uint32_t>(rgba[1]) << 8u |
                              static_cast<uint32_t>(rgba[2]) << 16u);
        }
    }
    if (result.empty) {
        result.minimumX = result.minimumY = 0;
        return result;
    }
    std::sort(colours.begin(), colours.end());
    result.uniqueVisibleColors = static_cast<uint32_t>(
        std::unique(colours.begin(), colours.end()) - colours.begin());
    result.canvasOccupancy = static_cast<float>(result.visiblePixels) /
                             static_cast<float>(kPixels);
    const int boundsPixels = (result.maximumX - result.minimumX + 1) *
                             (result.maximumY - result.minimumY + 1);
    result.boundsOccupancy = static_cast<float>(result.visiblePixels) /
                             static_cast<float>(boundsPixels);
    result.luminanceRange = maximumLuminance - minimumLuminance;
    result.lowOccupancy = result.canvasOccupancy < 0.18f;
    result.lowContrast = result.luminanceRange < 48u;
    return result;
}

bool moveSelection(Canvas &canvas, Selection selection, int deltaX,
                   int deltaY, bool copy) {
    if (selection.width <= 0 || selection.height <= 0 ||
        selection.x < 0 || selection.x >= kSize ||
        selection.y < 0 || selection.y >= kSize ||
        selection.width > kSize - selection.x ||
        selection.height > kSize - selection.y ||
        deltaX < -kSize || deltaX > kSize ||
        deltaY < -kSize || deltaY > kSize ||
        (deltaX == 0 && deltaY == 0)) return false;
    Canvas source = canvas;
    if (!copy) {
        for (int y = 0; y < selection.height; ++y) {
            for (int x = 0; x < selection.width; ++x) {
                std::memset(canvas.data() +
                    pixelOffset(selection.x + x, selection.y + y), 0, 4u);
            }
        }
    }
    for (int y = 0; y < selection.height; ++y) {
        for (int x = 0; x < selection.width; ++x) {
            const int targetX = selection.x + x + deltaX;
            const int targetY = selection.y + y + deltaY;
            if (targetX < 0 || targetX >= kSize ||
                targetY < 0 || targetY >= kSize) continue;
            std::memcpy(canvas.data() + pixelOffset(targetX, targetY),
                        source.data() + pixelOffset(selection.x + x,
                                                    selection.y + y), 4u);
        }
    }
    return canvas != source;
}

uint32_t replaceColour(Canvas &canvas, const uint8_t from[4],
                       const uint8_t to[4], uint8_t tolerance) {
    if (from == nullptr || to == nullptr) return 0u;
    const uint32_t maximumDistance =
        static_cast<uint32_t>(tolerance) * tolerance * 4u;
    uint32_t replacements = 0u;
    for (int pixel = 0; pixel < kPixels; ++pixel) {
        uint8_t *rgba = canvas.data() + pixel * 4;
        uint32_t distance = 0u;
        for (int component = 0; component < 4; ++component) {
            const int delta = static_cast<int>(rgba[component]) -
                              static_cast<int>(from[component]);
            distance += static_cast<uint32_t>(delta * delta);
        }
        if (distance <= maximumDistance) {
            if (std::memcmp(rgba, to, 4u) == 0) continue;
            std::memcpy(rgba, to, 4u);
            ++replacements;
        }
    }
    return replacements;
}

}  // namespace CharacterPortraitStudio
