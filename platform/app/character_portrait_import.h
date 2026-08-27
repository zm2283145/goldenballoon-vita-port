#ifndef MDKR_APP_CHARACTER_PORTRAIT_IMPORT_H
#define MDKR_APP_CHARACTER_PORTRAIT_IMPORT_H

#include "character_portrait_studio.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace CharacterPortraitImport {

constexpr uint32_t kMinimumDimension = 16u;
constexpr uint32_t kMaximumDimension = 4096u;
constexpr uint32_t kThumbnailMaximumDimension = 96u;
constexpr size_t kSubjectMaskPixels =
    static_cast<size_t>(CharacterPortraitStudio::kSize) *
    CharacterPortraitStudio::kSize;

enum class Sampling : uint32_t {
    Crisp = 0u,
    Area = 1u,
};

enum class Background : uint32_t {
    Transparent = 0u,
    Sunset = 1u,
    Sky = 2u,
    Charcoal = 3u,
};

enum class SourceKind : uint32_t {
    Canvas = 0u,
    LocalPng = 1u,
    ExactRenderer = 2u,
};

struct Image {
    uint32_t width = 0u;
    uint32_t height = 0u;
    std::vector<uint8_t> rgba;
    std::string sha256;
};

struct Recipe {
    uint32_t cropX = 0u;
    uint32_t cropY = 0u;
    uint32_t cropSize = 0u;
    uint32_t edgeMatteTolerance = 0u;
    Sampling sampling = Sampling::Area;
    Background background = Background::Transparent;
};

/* Non-destructive target-pixel matte applied after source sampling and before
 * a project-owned background is composited. Old drafts gain an all-keep
 * default; disabling a mask preserves its authored pixels for later reuse. */
struct SubjectMask {
    bool enabled = false;
    std::array<uint8_t, kSubjectMaskPixels> alpha;

    SubjectMask() { alpha.fill(255u); }
};

struct SourceRecord {
    SourceKind kind = SourceKind::Canvas;
    std::string sha256;
    uint32_t width = 0u;
    uint32_t height = 0u;
    Recipe recipe{};
    SubjectMask subjectMask{};
};

struct Thumbnail {
    uint32_t width = 0u;
    uint32_t height = 0u;
    std::vector<uint8_t> rgba;
};

/* Read one bounded, non-animated, non-interlaced, 8-bit RGB/RGBA PNG through
 * the port's UTF-8 filesystem seam. Output changes only after the complete
 * container, dimensions, CRCs and decoded pixel count agree. */
bool loadPng(const std::string &path, Image &output, std::string &error);

bool validImage(const Image &image);
Recipe centredRecipe(const Image &image);
bool validRecipe(const Image &image, const Recipe &recipe);
bool validSubjectMask(const SubjectMask &mask);
bool validSourceRecord(const SourceRecord &record);
SourceRecord sourceRecord(const Image &image, const Recipe &recipe,
                          SourceKind kind,
                          const SubjectMask &subjectMask = SubjectMask{});

/* Deterministically turn a square source selection into the exact 40x40 game
 * canvas. Edge matte removal is flood-filled from the source corners, so it
 * cannot erase a similarly coloured island enclosed by the subject. */
bool render(const Image &image, const Recipe &recipe,
            CharacterPortraitStudio::Canvas &output, std::string &error);
bool render(const Image &image, const Recipe &recipe,
            const SubjectMask &subjectMask,
            CharacterPortraitStudio::Canvas &output, std::string &error);

/* Bounded whole-source thumbnail for the crop manipulator. This is a UI view,
 * never package or draft authority. */
bool makeThumbnail(const Image &image, Thumbnail &output);

}  // namespace CharacterPortraitImport

#endif  // MDKR_APP_CHARACTER_PORTRAIT_IMPORT_H
