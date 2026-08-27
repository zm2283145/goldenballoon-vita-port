#ifndef MDKR_APP_CHARACTER_RAW_INTAKE_INDEX_H
#define MDKR_APP_CHARACTER_RAW_INTAKE_INDEX_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace CharacterRawIntakeIndex {

constexpr unsigned kMaximumClips = 256u;
constexpr unsigned kMaximumNodes = 4096u;

struct Inventory {
    std::string modelSha256;
    uint32_t vertices = 0u;
    uint32_t triangles = 0u;
    uint32_t materials = 0u;
    uint32_t textures = 0u;
    uint32_t skins = 0u;
    uint32_t joints = 0u;
    double sourceHeightM = 0.0;
    bool detailedBounds = false;
    std::array<double, 3> meshLocalMinimum{};
    std::array<double, 3> meshLocalMaximum{};
    std::array<double, 3> sceneWorldMinimum{};
    std::array<double, 3> sceneWorldMaximum{};
    std::string fallback;
    std::string seat;
    std::string head;
    std::vector<std::string> clips;
    std::vector<std::string> nodes;
};

// Parse the bounded, hex-escaped helper protocol. Output changes only after a
// complete parse, and defaults must name exact published choices.
bool parse(const std::string &text, Inventory &output);

}  // namespace CharacterRawIntakeIndex

#endif  // MDKR_APP_CHARACTER_RAW_INTAKE_INDEX_H
