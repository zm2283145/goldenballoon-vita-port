#ifndef MDKR_APP_CHARACTER_VISUAL_REPORT_H
#define MDKR_APP_CHARACTER_VISUAL_REPORT_H

#include <cstdint>
#include <string>
#include <vector>

namespace CharacterVisualReport {

/* A misuse guard rather than a workflow limit. The byte budget below normally
 * binds first, while 1024 still accommodates exhaustive context/pose/light
 * matrices without silently truncating an author's qualification set. */
constexpr size_t kMaximumCaptures = 1024u;

enum class RenderProduct : uint8_t {
    Scene = 0,
    ModelAlpha,
};

struct Capture {
    std::string pngPath;
    std::string sourceSha256;
    std::string fitSha256;
    std::string context;
    std::string pose;
    std::string lighting;
    RenderProduct renderProduct = RenderProduct::Scene;
    uint32_t players = 1u;
    uint32_t phaseMilli = 0u;
    int32_t viewYawDegrees = 0;
    int32_t viewPitchDegrees = 0;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t stableFrames = 0u;
    bool exactPose = false;
};

/* Writes one self-contained, responsive HTML contact sheet. PNG bytes are
 * embedded as data URIs and a JSON record is embedded beside them, so the
 * report can be moved or shared without the model, package, ROM, or original
 * capture paths. The destination is exclusive-create and never overwritten. */
bool exportHtml(const std::string &outputPath,
                const std::string &packageId,
                const std::string &displayName,
                const std::vector<Capture> &captures,
                std::string &error);

} // namespace CharacterVisualReport

#endif // MDKR_APP_CHARACTER_VISUAL_REPORT_H
