#ifndef MDKR_APP_CHARACTER_DRAFT_SNAPSHOT_H
#define MDKR_APP_CHARACTER_DRAFT_SNAPSHOT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../modern_character_semantics.h"
#include "../workshop_preview_runtime.h"
#include "character_portrait_studio.h"
#include "character_portrait_import.h"

namespace CharacterDraftSnapshot {

constexpr size_t kPortraitBytes = 40u * 40u * 4u;
constexpr size_t kContexts = 4u;
constexpr size_t kContacts = 4u;
constexpr size_t kRoles = 16u;
constexpr size_t kSecondaryChains = 8u;
constexpr size_t kSecondaryJointsPerChain = 16u;
constexpr uint32_t kNoNode = UINT32_MAX;
constexpr uint32_t kAnimationSemanticMask = 0x3FFEu;

enum Flags : uint32_t {
    Profile = 1u << 0,
    Identity = 1u << 1,
    Rig = 1u << 2,
    Tuning = 1u << 3,
    All = Profile | Identity | Rig | Tuning,
};

struct Context {
    float scale = 1.0f;
    float offset[3] = {};
    float rotation[3] = {};
    float contacts[kContacts][3] = {};
};

struct RigRole {
    uint32_t node = kNoNode;
    bool inferred = false;
    float confidence = 1.0f;
    float rest[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float bend[3] = {};
    bool constraintEnabled = false;
    float twistAxis[3] = {1.0f, 0.0f, 0.0f};
    float swingLimitDegrees = 180.0f;
    float twistMinDegrees = -180.0f;
    float twistMaxDegrees = 180.0f;
};

struct SecondaryChain {
    std::string name;
    uint32_t rootNode = kNoNode;
    uint32_t jointCount = 0u;
    uint32_t joints[kSecondaryJointsPerChain] = {};
    float bendAxis[3] = {0.0f, 0.0f, 1.0f};
    float stiffnessHz = 6.0f;
    float dampingRatio = 0.8f;
    float inertia = 0.65f;
    float maxAngleDegrees = 35.0f;
};

struct Snapshot {
    uint32_t flags = All;
    uint32_t donor = 9u;
    uint32_t packageVehicleMask = 7u;
    uint8_t minimapRgb[3] = {220u, 72u, 144u};
    int assemblyPlayers = 4;
    int testPlayers = 1;
    uint32_t testPose = MDKR_MODERN_CHARACTER_INSPECTION_DEFAULT_POSE;
    uint32_t testPosePhaseMilli = 500u;
    bool testTransition = false;
    uint32_t testTransitionFromPose = 5u;
    uint32_t testTransitionFromPhaseMilli = 500u;
    int32_t testViewYawDegrees = 0;
    int32_t testViewPitchDegrees = 0;
    uint32_t testLighting = MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL;
    uint32_t reviewedContexts = 0u;
    uint32_t contactExceptionContexts = 0u;
    /* Decode-only migration witness. v1-v12 approvals predate exact
     * composed-scene acknowledgement and are reopened on load. */
    bool fitSceneReviewContractPresent = false;
    /* Decode-only migration witness. v15 approvals include the complete
     * select/race semantic battery; every older approval reopens once. */
    bool fitSemanticReviewContractPresent = false;
    /* Decode-only migration witness. v16 vehicle approvals include all three
     * qualified course families; every older approval reopens once. */
    bool fitMultiSceneReviewContractPresent = false;
    /* Decode-only migration witness. v17 vehicle approvals include settled
     * frame-to-frame contact-residual stability; every older approval reopens
     * once. */
    bool fitContactStabilityContractPresent = false;
    /* Decode-only migration witness. v18 vehicle approvals include all five
     * qualified scene pressures; every older approval reopens once. */
    bool fitExpandedSceneContractPresent = false;

    float scale = 1.0f;
    float offset[3] = {};
    float rotation[3] = {};
    float animationSpeed = 1.0f;
    float lodBias = 0.0f;
    uint32_t enabledVehicleMask = 7u;
    Context contexts[kContexts];

    uint32_t rigMode = 0u;
    bool rigReviewed = false;
    /* Five source-bound anatomy-region checks used by native Rig Studio.
     * Older reviewed drafts migrate to all checked; older open drafts to none. */
    uint32_t rigReviewTaskMask = 0u;
    /* Decode-only migration witness. Every newly encoded v13 payload owns an
     * explicit mask; v1-v8 drafts preserve the active source decision. */
    bool animationIntentPresent = false;
    uint32_t disabledSemanticMask = 0u;
    RigRole roles[kRoles];
    /* Decode-only migration witness. Older drafts retain the active source's
     * limits and secondary chains rather than treating absence as deletion. */
    bool motionAuthoringContractPresent = false;
    uint32_t secondaryChainCount = 0u;
    SecondaryChain secondaryChains[kSecondaryChains];

    std::array<uint8_t, kPortraitBytes> portrait{};
    CharacterPortraitStudio::Canvas portraitStyleSource{};
    CharacterPortraitStudio::Recipe portraitRecipe{};
    CharacterPortraitImport::SourceRecord portraitSourceRecord{};
    std::string portraitSourcePath;
    std::string displayName;
    std::string shortName;
    std::string narrationName;
    std::string sortLabel;
};

bool encode(const Snapshot &snapshot, std::string &payload,
            std::string &error);
bool decode(const std::string &payload, Snapshot &snapshot,
            std::string &error);

}  // namespace CharacterDraftSnapshot

#endif  // MDKR_APP_CHARACTER_DRAFT_SNAPSHOT_H
