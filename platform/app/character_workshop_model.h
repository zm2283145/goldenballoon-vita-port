// Deterministic navigation/readiness policy for the Custom Character Workshop.
//
// This file deliberately knows nothing about ImGui, AppConfig, filesystem
// state, or the renderer. Callers reduce their current package/draft/editor
// state to CharacterWorkshopFacts, then render the returned named rows. Keeping
// the policy pure makes "Ready to preview", "Ready to play", and the single
// suggested next action consistent at every UI size and input method.
#ifndef MDKR64_CHARACTER_WORKSHOP_MODEL_H
#define MDKR64_CHARACTER_WORKSHOP_MODEL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class CharacterWorkshopTab : uint8_t {
    Overview = 0,
    Identity,
    RigMotion,
    Profile,
    Vehicles,
    Performance,
    Test,
    Package,
    Count,
};

enum class CharacterWorkshopReadinessId : uint8_t {
    Identity = 0,
    Calibration,
    RigMotion,
    GameplayProfile,
    VehicleFit,
    Performance,
    Count,
};

enum class CharacterWorkshopReadinessStatus : uint8_t {
    Ready = 0,
    Accepted,
    Review,
    Missing,
    Unavailable,
};

enum class CharacterWorkshopPerformanceState : uint8_t {
    NotMeasured = 0,
    OverTarget,
    OverTargetAccepted,
    TargetMet,
};

struct CharacterWorkshopFacts {
    bool     geometryAvailable      = false;
    bool     identityReady          = false;
    bool     normalized             = false;
    bool     anchorsReady           = false;
    bool     attachmentSocketsReady = false;
    bool     rigPresent             = false;
    bool     rigReviewed            = false;
    bool     motionReady            = false;
    bool     donorQualified         = false;
    bool     performanceAssemblyReady = false;
    CharacterWorkshopPerformanceState performance =
        CharacterWorkshopPerformanceState::NotMeasured;
    bool     enabled                = false;

    // Bit zero is character select. Bits one through three are car,
    // hovercraft, and plane. A zero vehicle mask is invalid source state.
    uint32_t supportedVehicleMask = 0u;
    uint32_t reviewedContextMask  = 0u;
};

struct CharacterWorkshopReadinessRow {
    CharacterWorkshopReadinessId id =
        CharacterWorkshopReadinessId::Identity;
    CharacterWorkshopReadinessStatus status =
        CharacterWorkshopReadinessStatus::Missing;
    CharacterWorkshopTab actionTab = CharacterWorkshopTab::Overview;
};

struct CharacterWorkshopReadiness {
    std::array<CharacterWorkshopReadinessRow,
               static_cast<size_t>(CharacterWorkshopReadinessId::Count)>
                         rows{};
    unsigned             readyCount      = 0u;
    bool                 readyToPreview  = false;
    // All authoring, fit, and measured-performance gates are satisfied. This
    // is intentionally independent of the local normal-play enable switch so
    // a disabled draft can be tested and can truthfully say it is ready to
    // enable.
    bool                 readyToEnable   = false;
    bool                 readyToPlay     = false;
    CharacterWorkshopTab nextActionTab   = CharacterWorkshopTab::Overview;
    const char          *nextActionLabel = "Inspect character";
};

enum class CharacterWorkshopPerformanceTarget : uint8_t {
    Quality = 0,
    Balanced,
    Performance,
    FourPlayer,
    Count,
};

struct CharacterWorkshopPerformancePreset {
    const char *label;
    const char *description;
    int players;
    float lodBias;
};

struct CharacterWorkshopLodBand {
    float minimumDistance = 0.0f;
    float maximumDistance = 0.0f; // +infinity is the final open interval.
    uint32_t lod = 0u;
};

// A conservative first-pass correction derived from the exact renderer's
// target-space measurements. The proposal intentionally changes only the
// vertical context offset and facing yaw: X/Z placement and contact targets
// require vehicle geometry or author judgement and must never be guessed from
// a character bounds box alone.
struct CharacterWorkshopFitMeasurement {
    bool valid = false;
    bool vehicleContext = false;
    std::array<int64_t, 3> boundsMinimumMicrometres{};
    std::array<int64_t, 3> boundsMaximumMicrometres{};
    std::array<int32_t, 3> forwardMilli{};
};

struct CharacterWorkshopFitSuggestion {
    bool available = false;
    bool verticalAdjustment = false;
    bool facingMeasured = false;
    bool facingAdjustment = false;
    float verticalDeltaMetres = 0.0f;
    float yawDeltaDegrees = 0.0f;
    float measuredHeightMetres = 0.0f;
    float measuredMinimumYMetres = 0.0f;
    float targetMinimumYMetres = 0.0f;
};

/* A bounded exact-pose witness for one or more procedural contacts. The
 * suggested constant target correction is the arithmetic mean of endpoint -
 * target for each contact, which is the least-squares solution across the
 * supplied states. It is a reversible starting point, not an anatomy guess. */
struct CharacterWorkshopContactMeasurement {
    uint32_t witnessMask = 0u;
    std::array<std::array<int64_t, 3>, 4> targetMicrometres{};
    std::array<std::array<int64_t, 3>, 4> endpointMicrometres{};
};

struct CharacterWorkshopContactSuggestion {
    bool valid = false;
    bool available = false;
    bool adjustment = false;
    bool withinLimits = false;
    uint32_t contactMask = 0u;
    uint32_t witnessSamples = 0u;
    std::array<uint32_t, 4> sampleCounts{};
    std::array<std::array<float, 3>, 4> deltaMetres{};
};

enum class CharacterWorkshopQualitySeverity : uint8_t {
    Nominal = 0,
    Review,
    Critical,
    Invalid,
};

/* Plain-language quality bands derived from the same exact renderer evidence
 * as the fit proposal. They guide review and never block unusual anatomy. */
struct CharacterWorkshopFitAssessment {
    bool valid = false;
    bool vehicleContext = false;
    bool facingMeasured = false;
    CharacterWorkshopQualitySeverity datum =
        CharacterWorkshopQualitySeverity::Invalid;
    CharacterWorkshopQualitySeverity facing =
        CharacterWorkshopQualitySeverity::Invalid;
    CharacterWorkshopQualitySeverity proportions =
        CharacterWorkshopQualitySeverity::Invalid;
    float datumErrorMetres = 0.0f;
    float facingDegrees = 0.0f;
    float widthToHeight = 0.0f;
    float depthToHeight = 0.0f;
};

/* Value-only approval policy for one exact fit context. Renderer/UI code owns
 * contract validation and reduces it to these facts; this policy keeps the
 * author-agency exception path distinct from a concrete invisible-character
 * failure and is directly regression tested. */
struct CharacterWorkshopFitReviewFacts {
    bool exactContract = false;
    bool opaqueVisibilityQualified = false;
    uint32_t isolatedVisibleTiles = 0u;
    uint32_t sceneVisibleTiles = 0u;
    bool advisoryFitWarning = false;
    bool cameraWarning = false;
    bool surfaceWarning = false;
    bool attachmentVisibilityWarning = false;
    bool sceneReviewed = false;
    bool contactExceptionRequired = false;
    bool contactExceptionApproved = false;
};

struct CharacterWorkshopFitReviewDecision {
    bool visibilityBlocksReview = false;
    bool warnings = false;
    bool ready = false;
};

enum class CharacterWorkshopTransformSeverity : uint8_t {
    Nominal = 0,
    Review,
    Critical,
    Invalid,
};

// Pure intake diagnosis from the two coordinate spaces exposed by glTF. It
// deliberately reports evidence and proposed multipliers; it never guesses an
// artist's unit intent or mutates the package.
struct CharacterWorkshopSourceTransformFacts {
    std::array<double, 3> meshLocalMinimum{};
    std::array<double, 3> meshLocalMaximum{};
    std::array<double, 3> sceneWorldMinimum{};
    std::array<double, 3> sceneWorldMaximum{};
    double targetHeightMetres = 1.25;
};

struct CharacterWorkshopSourceTransformReview {
    bool valid = false;
    CharacterWorkshopTransformSeverity severity =
        CharacterWorkshopTransformSeverity::Invalid;
    // Diagonal spans are rotation-invariant and expose aggregate hierarchy
    // scaling even when an authoring format is Z-up or X-up.
    double meshLocalSpan = 0.0;
    double sceneWorldSpanMetres = 0.0;
    double sceneWorldHeightMetres = 0.0;
    double hierarchyScale = 0.0;
    double normalizeToOneMultiplier = 0.0;
    double targetHeightMultiplier = 0.0;
    double sceneGroundYMetres = 0.0;
    double widthToHeight = 0.0;
    double depthToHeight = 0.0;
    bool suspiciousHierarchyScale = false;
    bool suspiciousWorldHeight = false;
    bool unusualProportions = false;
};

enum class CharacterWorkshopRigEvidence : uint8_t {
    None = 0,
    Name,
    HierarchyCommonAncestor,
    HierarchyChain,
    GeometrySymmetry,
};

struct CharacterWorkshopRigJoint {
    std::string name;
    int parent = -1; // nearest skin-joint parent, or -1 at a skin root
    std::array<float, 3> bindPosition{};
    // Normalized node-local-to-source-model bind orientation.
    std::array<float, 4> bindRotation{{0.0f, 0.0f, 0.0f, 1.0f}};
};

struct CharacterWorkshopRigRoleSuggestion {
    int joint = -1;
    float confidence = 0.0f;
    CharacterWorkshopRigEvidence evidence = CharacterWorkshopRigEvidence::None;
    std::array<float, 4> restRotation{{0.0f, 0.0f, 0.0f, 1.0f}};
    std::array<float, 3> bendAxis{};
    bool restBasisAvailable = false;
    bool bendAxisAvailable = false;
};

struct CharacterWorkshopRigSuggestion {
    std::array<CharacterWorkshopRigRoleSuggestion, 16> roles{};
    bool complete = false;
    bool hierarchyValid = false;
    unsigned namedRoles = 0u;
    unsigned hierarchyRoles = 0u;
    unsigned geometryRoles = 0u;
    unsigned commonAncestorRepairs = 0u;
    unsigned restBasisRoles = 0u;
    unsigned bendAxisRoles = 0u;
};

CharacterWorkshopReadiness CharacterWorkshop_evaluate(
    const CharacterWorkshopFacts &facts);

const char          *CharacterWorkshop_tabLabel(CharacterWorkshopTab tab);
const char          *CharacterWorkshop_tabStorageId(CharacterWorkshopTab tab);
CharacterWorkshopTab CharacterWorkshop_parseTab(const char *stored);
const char          *CharacterWorkshop_readinessLabel(
    CharacterWorkshopReadinessId id);
const char *CharacterWorkshop_statusLabel(
    CharacterWorkshopReadinessStatus status);
const CharacterWorkshopPerformancePreset *
CharacterWorkshop_performancePreset(
    CharacterWorkshopPerformanceTarget target);
CharacterWorkshopPerformanceTarget CharacterWorkshop_performanceTarget(
    int players, float lodBias);
/* Uses the renderer's shared policy: distance chooses a base band, source and
 * local biases shift it, and sparse authored levels fall back toward the
 * nearest more-detailed level. UINT32_MAX means invalid input. */
uint32_t CharacterWorkshop_selectLod(
    float viewDistance, float sourceLodBias, float localLodBias,
    uint32_t authoredLodMask);
/* Returns the renderer policy's merged distance intervals. Adjacent base
 * bands that resolve to the same authored level after bias, clamping, and
 * sparse fallback are intentionally coalesced. Zero means invalid input. */
size_t CharacterWorkshop_lodBands(
    float sourceLodBias, float localLodBias, uint32_t authoredLodMask,
    CharacterWorkshopLodBand output[4]);

CharacterWorkshopFitSuggestion CharacterWorkshop_suggestFit(
    const CharacterWorkshopFitMeasurement &measurement);
CharacterWorkshopContactSuggestion CharacterWorkshop_suggestContacts(
    const std::array<std::array<float, 3>, 4> &currentContacts,
    const CharacterWorkshopContactMeasurement *measurements,
    size_t measurementCount);
CharacterWorkshopFitAssessment CharacterWorkshop_assessFit(
    const CharacterWorkshopFitMeasurement &measurement);
CharacterWorkshopFitReviewDecision CharacterWorkshop_reviewFit(
    const CharacterWorkshopFitReviewFacts &facts);
/* Returns sceneCount when the guided review is complete or the input is
 * malformed. A requested full refresh advances in order even when older
 * evidence is current; a resume selects the first missing scene. */
uint32_t CharacterWorkshop_nextSceneReview(
    uint32_t sceneCount, uint32_t justCompletedScene,
    uint32_t currentSceneMask, bool refreshAll);
/* Matches the compiler's +Z/-Z/+X/-X source-forward convention. Output is
 * unchanged for an invalid candidate. */
bool CharacterWorkshop_facingCorrectionDegrees(
    uint32_t sourceForward, float &outputDegrees);
CharacterWorkshopSourceTransformReview CharacterWorkshop_reviewSourceTransform(
    const CharacterWorkshopSourceTransformFacts &facts);
CharacterWorkshopRigSuggestion CharacterWorkshop_suggestHumanoidRig(
    const std::vector<CharacterWorkshopRigJoint> &joints,
    uint32_t sourceForward = 0u);
/* Derives bases for an explicit author mapping without re-running name
 * inference. Valid mapped roles receive independent rest bases even while the
 * complete anatomy graph is still being assembled; bend evidence remains
 * conditional on a valid, non-degenerate three-joint limb chain. */
CharacterWorkshopRigSuggestion CharacterWorkshop_suggestHumanoidBases(
    const std::vector<CharacterWorkshopRigJoint> &joints,
    const std::array<int, 16> &roleJoints, uint32_t sourceForward = 0u);

#endif // MDKR64_CHARACTER_WORKSHOP_MODEL_H
