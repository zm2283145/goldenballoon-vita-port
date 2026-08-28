#ifndef MDKR_APP_CHARACTER_ADAPTER_OUTPUT_INDEX_H
#define MDKR_APP_CHARACTER_ADAPTER_OUTPUT_INDEX_H

#include <cstdint>
#include <string>

namespace CharacterAdapterOutputIndex {

struct Review {
    std::string artifactSha256;
    std::string modelSha256;
    std::string sourceSha256;
    uint64_t artifactBytes = 0u;
    uint64_t modelBytes = 0u;
    uint32_t vertices = 0u;
    uint32_t triangles = 0u;
    uint32_t joints = 0u;
    uint32_t materials = 0u;
    uint32_t textures = 0u;
    bool licensePresent = false;
    std::string licenseSha256;
    std::string adapterName;
    std::string adapterVersion;
    std::string adapterHomepage;
    std::string sourceFormat;
    std::string conversionProfile;
    std::string conversionSettingsSha256;
    std::string licenseSpdx;
    std::string attribution;
    std::string sourceUrl;
};

// Parses the frozen, bounded launcher review protocol. Output changes only
// after the complete row, relationships, bounds, and trailing byte pass.
bool parse(const std::string &text, Review &output);

}  // namespace CharacterAdapterOutputIndex

#endif  // MDKR_APP_CHARACTER_ADAPTER_OUTPUT_INDEX_H
