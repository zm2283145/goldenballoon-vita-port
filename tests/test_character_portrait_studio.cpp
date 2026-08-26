#include "character_portrait_studio.h"
#include "character_portrait_import.h"
#include "fs_utf8.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

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

bool sameRecipe(const Recipe &left, const Recipe &right) {
    return left.zoomPercent == right.zoomPercent &&
           left.panX == right.panX && left.panY == right.panY &&
           left.paletteColors == right.paletteColors &&
           left.ditherStrength == right.ditherStrength &&
           left.outlinePixels == right.outlinePixels &&
           left.alphaThreshold == right.alphaThreshold &&
           left.sampling == right.sampling &&
           left.fillPinholes == right.fillPinholes;
}

uint32_t pngCrc(const unsigned char *bytes, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0u; index < size; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1u) ^
                ((crc & 1u) != 0u ? 0xEDB88320u : 0u);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void appendBigEndian(std::vector<unsigned char> &bytes, uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value >> 24u));
    bytes.push_back(static_cast<unsigned char>(value >> 16u));
    bytes.push_back(static_cast<unsigned char>(value >> 8u));
    bytes.push_back(static_cast<unsigned char>(value));
}

std::vector<unsigned char> withAnimationControl(
    const unsigned char *png, size_t size) {
    assert(size >= 33u);
    std::vector<unsigned char> result(png, png + 33u);
    std::vector<unsigned char> chunk = {
        'a', 'c', 'T', 'L',
        0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u,
    };
    appendBigEndian(result, 8u);
    result.insert(result.end(), chunk.begin(), chunk.end());
    appendBigEndian(result, pngCrc(chunk.data(), chunk.size()));
    result.insert(result.end(), png + 33u, png + size);
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

    const std::array<StylePreset, kStylePresetCount> stylePresets = {
        StylePreset::Clean64,
        StylePreset::Classic32,
        StylePreset::Bold16,
        StylePreset::Crisp32,
        StylePreset::Dithered32,
        StylePreset::Soft64,
    };
    Recipe framed = defaults;
    framed.zoomPercent = 175;
    framed.panX = -7;
    framed.panY = 11;
    framed.alphaThreshold = 29;
    framed.fillPinholes = false;
    for (StylePreset preset : stylePresets) {
        const Recipe candidate = presetRecipe(framed, preset);
        assert(validRecipe(candidate));
        assert(candidate.zoomPercent == framed.zoomPercent);
        assert(candidate.panX == framed.panX && candidate.panY == framed.panY);
        assert(candidate.alphaThreshold == framed.alphaThreshold);
        assert(candidate.fillPinholes == framed.fillPinholes);
        const Recipe repeated = presetRecipe(framed, preset);
        assert(sameRecipe(candidate, repeated));
    }
    const Recipe clean = presetRecipe(framed, StylePreset::Clean64);
    assert(clean.paletteColors == 64u && clean.ditherStrength == 0.0f &&
           clean.outlinePixels == 1 && clean.sampling == Sampling::Smooth);
    const Recipe classic = presetRecipe(framed, StylePreset::Classic32);
    assert(classic.paletteColors == 32u &&
           classic.ditherStrength == 0.25f && classic.outlinePixels == 1 &&
           classic.sampling == Sampling::Smooth);
    const Recipe bold = presetRecipe(framed, StylePreset::Bold16);
    assert(bold.paletteColors == 16u && bold.ditherStrength == 0.15f &&
           bold.outlinePixels == 2 && bold.sampling == Sampling::Smooth);
    const Recipe crispPreset = presetRecipe(framed, StylePreset::Crisp32);
    assert(crispPreset.paletteColors == 32u &&
           crispPreset.ditherStrength == 0.0f &&
           crispPreset.outlinePixels == 1 &&
           crispPreset.sampling == Sampling::Crisp);
    const Recipe dithered = presetRecipe(framed, StylePreset::Dithered32);
    assert(dithered.paletteColors == 32u &&
           dithered.ditherStrength == 0.65f &&
           dithered.outlinePixels == 1 &&
           dithered.sampling == Sampling::Smooth);
    const Recipe soft = presetRecipe(framed, StylePreset::Soft64);
    assert(soft.paletteColors == 64u && soft.ditherStrength == 0.10f &&
           soft.outlinePixels == 0 && soft.sampling == Sampling::Smooth);
    const Recipe unknown = presetRecipe(
        framed, static_cast<StylePreset>(UINT32_MAX));
    assert(sameRecipe(unknown, framed));
    const Recipe invalidPreset = presetRecipe(invalid, StylePreset::Clean64);
    assert(sameRecipe(invalidPreset, invalid));

    Canvas gradient{};
    for (int y = 8; y < 32; ++y) {
        for (int x = 8; x < 32; ++x) {
            setPixel(gradient, x, y, static_cast<uint8_t>(x * 6),
                     static_cast<uint8_t>(y * 6),
                     static_cast<uint8_t>((x + y) * 3));
        }
    }
    assert(applyRecipe(gradient, invalid) == gradient);
    for (StylePreset preset : stylePresets) {
        const Recipe candidate = presetRecipe(framed, preset);
        const Canvas styled = applyRecipe(gradient, candidate);
        assert(styled == applyRecipe(gradient, candidate));
        assert(!analyse(styled).empty);
        assert(visibleColours(styled).size() <= candidate.paletteColors);
    }

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

    using namespace CharacterPortraitImport;
    Image wide;
    wide.width = 192u;
    wide.height = 96u;
    wide.sha256 = std::string(64u, 'a');
    wide.rgba.resize(static_cast<size_t>(wide.width) * wide.height * 4u);
    for (uint32_t y = 0u; y < wide.height; ++y) {
        for (uint32_t x = 0u; x < wide.width; ++x) {
            uint8_t *rgba = wide.rgba.data() +
                (static_cast<size_t>(y) * wide.width + x) * 4u;
            rgba[0] = static_cast<uint8_t>(x);
            rgba[1] = static_cast<uint8_t>(y * 2u);
            rgba[2] = static_cast<uint8_t>((x + y) & 0xFFu);
            rgba[3] = 255u;
        }
    }
    assert(validImage(wide));
    CharacterPortraitImport::Recipe importRecipe = centredRecipe(wide);
    assert(importRecipe.cropX == 48u && importRecipe.cropY == 0u &&
           importRecipe.cropSize == 96u && validRecipe(wide, importRecipe));
    Canvas imported{};
    std::string importError;
    assert(render(wide, importRecipe, imported, importError));
    const Canvas importedAgain = imported;
    assert(render(wide, importRecipe, imported, importError) &&
           imported == importedAgain);
    importRecipe.sampling = CharacterPortraitImport::Sampling::Crisp;
    assert(render(wide, importRecipe, imported, importError) &&
           imported != importedAgain);
    Thumbnail thumbnail;
    assert(makeThumbnail(wide, thumbnail) && thumbnail.width == 96u &&
           thumbnail.height == 48u &&
           thumbnail.rgba.size() == 96u * 48u * 4u);
    SourceRecord record = sourceRecord(
        wide, importRecipe, SourceKind::ExactRenderer);
    assert(validSourceRecord(record) && record.width == wide.width &&
           record.sha256 == wide.sha256);
    record.recipe.cropX = wide.width;
    assert(!validSourceRecord(record));
    record = SourceRecord{};
    record.sha256 = std::string(64u, 'a');
    assert(!validSourceRecord(record));

    Image keyed;
    keyed.width = keyed.height = 40u;
    keyed.sha256 = std::string(64u, 'b');
    keyed.rgba.resize(40u * 40u * 4u);
    for (uint32_t y = 0u; y < 40u; ++y) {
        for (uint32_t x = 0u; x < 40u; ++x) {
            uint8_t *rgba = keyed.rgba.data() + (y * 40u + x) * 4u;
            rgba[0] = 20u;
            rgba[1] = 200u;
            rgba[2] = 30u;
            rgba[3] = 255u;
            if (x >= 12u && x < 28u && y >= 8u && y < 32u) {
                rgba[0] = 230u;
                rgba[1] = 40u;
                rgba[2] = 50u;
            }
        }
    }
    importRecipe = centredRecipe(keyed);
    importRecipe.edgeMatteTolerance = 1u;
    assert(render(keyed, importRecipe, imported, importError));
    assert(pixel(imported, 0, 0)[3] == 0u &&
           pixel(imported, 20, 20)[0] == 230u &&
           pixel(imported, 20, 20)[3] == 255u);
    importRecipe.background = Background::Sunset;
    assert(render(keyed, importRecipe, imported, importError));
    assert(pixel(imported, 0, 0)[3] == 255u &&
           pixel(imported, 0, 0)[0] != 20u);
    const Canvas beforeInvalid = imported;
    importRecipe.cropSize = 41u;
    assert(!render(keyed, importRecipe, imported, importError) &&
           imported == beforeInvalid && !importError.empty());

    constexpr unsigned char kRgbPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10,
        0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x91, 0x68, 0x36, 0x00, 0x00, 0x00,
        0x16, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xe0, 0x12, 0x91, 0x23,
        0x09, 0x31, 0x8c, 0x6a, 0x18, 0xd5, 0x30, 0x7c, 0x35, 0x00, 0x00, 0xd4,
        0x65, 0x3c, 0x01, 0xb3, 0xf9, 0xc0, 0xf3, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    const std::string pngPath = "character-portrait-import.png";
    FILE *pngFile = mdkr_fopen_utf8(pngPath.c_str(), "wb");
    assert(pngFile != nullptr);
    assert(std::fwrite(kRgbPng, 1u, sizeof(kRgbPng), pngFile) ==
           sizeof(kRgbPng));
    assert(std::fclose(pngFile) == 0);
    Image decoded;
    assert(loadPng(pngPath, decoded, importError));
    assert(decoded.width == 16u && decoded.height == 16u &&
           decoded.rgba.size() == 16u * 16u * 4u &&
           decoded.rgba[0] == 10u && decoded.rgba[1] == 20u &&
           decoded.rgba[2] == 30u && decoded.rgba[3] == 255u);
    const Image unchanged = decoded;
    std::array<unsigned char, sizeof(kRgbPng)> corrupt{};
    std::copy(std::begin(kRgbPng), std::end(kRgbPng), corrupt.begin());
    corrupt[48] ^= 0x80u;
    pngFile = mdkr_fopen_utf8(pngPath.c_str(), "wb");
    assert(pngFile != nullptr);
    assert(std::fwrite(corrupt.data(), 1u, corrupt.size(), pngFile) ==
           corrupt.size());
    assert(std::fclose(pngFile) == 0);
    assert(!loadPng(pngPath, decoded, importError) &&
           decoded.rgba == unchanged.rgba &&
           decoded.sha256 == unchanged.sha256);
    const std::vector<unsigned char> animated = withAnimationControl(
        kRgbPng, sizeof(kRgbPng));
    pngFile = mdkr_fopen_utf8(pngPath.c_str(), "wb");
    assert(pngFile != nullptr);
    assert(std::fwrite(animated.data(), 1u, animated.size(), pngFile) ==
           animated.size());
    assert(std::fclose(pngFile) == 0);
    assert(!loadPng(pngPath, decoded, importError) &&
           importError.find("Animated PNG") != std::string::npos &&
           decoded.rgba == unchanged.rgba &&
           decoded.sha256 == unchanged.sha256);
    (void)mdkr_remove_utf8(pngPath.c_str());

    std::cout << "character_portrait_studio: PASS\n";
    return 0;
}
