#ifndef MDKR_APP_CHARACTER_CANDIDATE_INDEX_H
#define MDKR_APP_CHARACTER_CANDIDATE_INDEX_H

#include <cstdint>
#include <string>

namespace CharacterCandidateIndex {

struct Candidate {
    std::string id;
    std::string displayName;
    std::string shortName;
    std::string narrationName;
    std::string sortLabel;
    std::string packageSha256;
    std::string sourceDigest;
    uint32_t donor = 0u;
    uint32_t vehicleMask = 0u;
    uint32_t vertices = 0u;
    uint32_t triangles = 0u;
    uint32_t primitives = 0u;
    uint32_t lodLevels = 0u;
    uint32_t materials = 0u;
    uint32_t textures = 0u;
    uint32_t nodes = 0u;
    uint32_t skins = 0u;
    uint32_t joints = 0u;
    uint32_t animations = 0u;
    uint32_t animationChannels = 0u;
    uint32_t animationKeys = 0u;
    uint32_t semanticMask = 0u;
    uint32_t disabledSemanticMask = 0u;
    bool semanticIntentPresent = false;
    bool identityPresent = false;
    uint32_t rigMode = 0u;
    bool rigReviewed = false;
    uint32_t rigRoles = 0u;
    uint32_t jointConstraints = 0u;
    uint32_t secondaryChains = 0u;
    uint32_t secondaryJoints = 0u;
    bool tangentDiagnosticsPresent = false;
    uint32_t authoredTangentPrimitives = 0u;
    uint32_t generatedTangentPrimitives = 0u;
    uint32_t authoredTangentRepairedVertices = 0u;
    uint32_t generatedTangentDegenerateUvTriangles = 0u;
    uint32_t tangentFallbackVertices = 0u;
    uint32_t normalMapTangentFallbackVertices = 0u;
    uint64_t encodedTextureBytes = 0u;
    uint64_t decodedTextureBytes = 0u;
    bool provenancePresent = false;
    std::string licenseSpdx;
    std::string attribution;
    std::string sourceUrl;
    uint32_t lodVertices[4] = {};
    uint32_t lodTriangles[4] = {};
    uint32_t lodPrimitives[4] = {};
};

// Strictly parse the bounded fixed-field helper protocol. Output changes only
// on complete success, preventing spoofed compiler output from enabling an
// install confirmation.
bool parse(const std::string &text, Candidate &output);

}  // namespace CharacterCandidateIndex

#endif  // MDKR_APP_CHARACTER_CANDIDATE_INDEX_H
