#ifndef MDKR_APP_CHARACTER_TEST_EVIDENCE_STORE_H
#define MDKR_APP_CHARACTER_TEST_EVIDENCE_STORE_H

#include "text_state_file.h"
#include "modern_character_gpu_timing.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace CharacterTestEvidenceStore {

constexpr size_t kMaximumPackages    = 64u;
constexpr size_t kContextsPerPackage = 4u;
constexpr size_t kLayoutsPerContext  = 4u;
constexpr size_t kKindsPerCell       = 2u;
constexpr size_t kMaximumRecords =
    kMaximumPackages * kContextsPerPackage * kLayoutsPerContext *
    kKindsPerCell;
constexpr size_t kMaximumBuildVersionBytes = 64u;
constexpr size_t kMaximumBackendBytes      = 31u;
constexpr size_t kMaximumAdapterBytes      = 191u;
constexpr size_t kMaximumDriverBytes       = 191u;

enum class Kind : uint32_t {
    Latest   = 0u,
    Baseline = 1u,
};

// Evidence quality and performance policy are intentionally separate. A
// complete real-device sample can be trustworthy evidence while still
// missing the playability target; callers must never translate "measured"
// into "passed".
enum class PerformanceResult : uint8_t {
    Unqualified = 0u,
    OverBudget,
    TargetMet,
};

struct PerformanceTarget {
    uint64_t p95IntervalUs = 18334u;
    uint64_t p99IntervalUs = 25000u;
};

// One exact engine result. Source and fit fingerprints decide whether the
// evidence still describes the selected character; presentation/device fields
// decide whether a pinned baseline and a later run are honestly comparable.
struct Evidence {
    Kind        kind = Kind::Latest;
    std::string packageId;
    uint64_t    capturedUnix = 0u;
    std::string sourceSha256;
    std::string fitSha256;
    std::string presentationSha256;
    std::string buildVersion;
    uint32_t    context                     = 0u;
    uint32_t    players                     = 1u;
    uint32_t    resultVersion               = 0u;
    bool        started                     = false;
    bool        warmupComplete              = false;
    bool        realtime                    = false;
    uint64_t    warmupTicks                 = 0u;
    uint64_t    intervalSamples             = 0u;
    uint64_t    displayedFrames             = 0u;
    uint64_t    intervalP50Us               = 0u;
    uint64_t    intervalP95Us               = 0u;
    uint64_t    intervalP99Us               = 0u;
    uint64_t    intervalMeanUs              = 0u;
    uint64_t    intervalMaxUs               = 0u;
    uint64_t    tickwallSamples             = 0u;
    uint64_t    tickwallMeanNs              = 0u;
    /* Version zero means the authenticated v1-v3 record predates GPU timing.
     * Current v9/result-v23 records carry a structurally validated snapshot. */
    MdkrModernCharacterGpuTimingMetrics gpuTiming{};
    uint64_t    replacementDraws            = 0u;
    uint64_t    replacementPrimitives       = 0u;
    uint64_t    hiddenDonorBatches          = 0u;
    uint64_t    contactSolves               = 0u;
    uint64_t    contactErrorMeanMicrometres = 0u;
    uint64_t    contactErrorMaxMicrometres  = 0u;
    uint32_t    contactWitnessMask          = 0u;
    int64_t     contactChainRootMicrometres[4][3] = {};
    int64_t     contactBendMicrometres[4][3]      = {};
    int64_t     contactTargetMicrometres[4][3]    = {};
    int64_t     contactEndMicrometres[4][3]       = {};
    uint64_t    contactWitnessErrorMicrometres[4] = {};
    bool        fitDiagnosticsValid         = false;
    int64_t     fitBoundsMinMicrometres[3]  = {};
    int64_t     fitBoundsMaxMicrometres[3]  = {};
    int64_t     fitAnchorMicrometres[3]     = {};
    int32_t     fitForwardMilli[3]          = {};
    uint32_t    fitLandmarkMask             = 0u;
    int64_t     fitLandmarkMicrometres[3][3] = {};
    bool        cameraProjectionValid       = false;
    uint32_t    cameraProjectionWidth       = 0u;
    uint32_t    cameraProjectionHeight      = 0u;
    int32_t     cameraProjectionViewport[4] = {};
    int32_t     cameraProjectionScissor[4]  = {};
    uint32_t    cameraProjectionPrimitiveDraws = 0u;
    int32_t     cameraBoundsPixelMilli[4]   = {};
    uint32_t    cameraBoundsClipFlags       = 0u;
    int32_t     cameraLandmarkPixelMilli[3][2] = {};
    int32_t     cameraLandmarkDepthMillionths[3] = {};
    uint32_t    cameraLandmarkClipFlags[3]  = {};
    bool        vehicleSurfaceValid         = false;
    uint32_t    vehicleShellTrianglesSubmitted = 0u;
    uint32_t    vehicleShellTrianglesTested = 0u;
    uint32_t    characterSurfaceTrianglesSubmitted = 0u;
    uint32_t    characterSurfaceTrianglesTested = 0u;
    uint32_t    vehicleSurfaceCrossingTriangles = 0u;
    uint32_t    vehicleSurfaceCrossingPairs = 0u;
    int64_t     vehicleSurfaceFirstCrossingMicrometres[3] = {};
    bool        vehicleVolumeQualified = false;
    uint32_t    vehicleShellBoundaryEdges = 0u;
    uint32_t    vehicleShellNonmanifoldEdges = 0u;
    uint32_t    vehicleShellOrientationMismatchEdges = 0u;
    uint32_t    vehicleShellSelfIntersectionPairs = 0u;
    uint32_t    vehicleContainmentSamplesTested = 0u;
    uint32_t    vehicleContainmentInsideSamples = 0u;
    uint32_t    vehicleContainmentBoundarySamples = 0u;
    uint32_t    vehicleContainmentOutsideSamples = 0u;
    uint64_t    vehicleContainmentMaximumDepthMicrometres = 0u;
    int64_t     vehicleContainmentDeepestMicrometres[3] = {};
    bool        opaqueVisibilityValid = false;
    bool        opaqueVisibilityQualified = false;
    uint32_t    opaqueVisibilityWidth = 0u;
    uint32_t    opaqueVisibilityHeight = 0u;
    int32_t     opaqueVisibilityViewport[4] = {};
    int32_t     opaqueVisibilityScissor[4] = {};
    uint32_t    opaqueVisibilityPrimitiveDraws = 0u;
    uint32_t    opaqueVisibilityOpaqueDraws = 0u;
    uint32_t    opaqueVisibilityMaskedDraws = 0u;
    uint32_t    opaqueVisibilityTransparentDraws = 0u;
    uint32_t    opaqueVisibilityGridColumns = 0u;
    uint32_t    opaqueVisibilityGridRows = 0u;
    uint32_t    opaqueVisibilityIsolatedTiles = 0u;
    uint32_t    opaqueVisibilitySceneTiles = 0u;
    uint64_t    opaqueVisibilityIsolatedTileMask = 0u;
    uint64_t    opaqueVisibilitySceneTileMask = 0u;
    uint32_t    opaqueVisibilityOccluderPresentMask = 0u;
    uint32_t    opaqueVisibilityOccluderQualifiedMask = 0u;
    uint32_t    opaqueVisibilityOccluderDraws[3] = {};
    uint32_t    opaqueVisibilityOccluderUnqualifiedDraws[3] = {};
    uint32_t    opaqueVisibilityOccluderOverlapTiles[3] = {};
    uint64_t    opaqueVisibilityOccluderOverlapTileMask[3] = {};
    std::string backend;
    std::string adapter;
    std::string driver;
    uint32_t    vendorId     = 0u;
    uint32_t    deviceId     = 0u;
    uint32_t    outputWidth  = 0u;
    uint32_t    outputHeight = 0u;
    uint32_t    renderWidth  = 0u;
    uint32_t    renderHeight = 0u;
};

struct Inventory {
    std::vector<Evidence> records;
};

// Privacy-bounded workload metadata for a shareable real-device profile.
// The exporter hashes the exact source/fit/presentation tuple into a
// workload-only identifier and never publishes those raw digests, the package
// ID, player-facing names, paths, artwork, model bytes, or ROM bytes.
struct DeviceProfileWorkload {
    std::string hostPlatform;
    std::string sourceSha256;
    std::array<std::string, 4> fitSha256;
    std::string presentationSha256;
    uint32_t vehicleMask = 0u;
    uint32_t vertices = 0u;
    uint32_t triangles = 0u;
    uint32_t primitives = 0u;
    uint32_t joints = 0u;
    uint32_t textures = 0u;
    uint32_t lodCount = 0u;
    uint64_t decodedTextureBytes = 0u;
    std::array<uint32_t, 4> lodVertices{};
    std::array<uint32_t, 4> lodTriangles{};
    std::array<uint32_t, 4> lodPrimitives{};
};

struct DeviceProfileSummary {
    std::string workloadId;
    uint32_t expectedRows = 0u;
    uint32_t targetRows = 0u;
    uint64_t capturedFromUnix = 0u;
    uint64_t capturedToUnix = 0u;
};

enum class LoadResult {
    Loaded,
    Missing,
    Invalid,
    IoError,
};

bool parse(const std::string &text, Inventory &output, std::string &error);
bool serialize(const Inventory &inventory, std::string &output,
               std::string &error);

LoadResult load(const MdkrTextStateStorage &storage, Inventory &output,
                std::string &error);
bool save(const MdkrTextStateStorage &storage, const Inventory &inventory,
          std::string &error);

bool upsert(Inventory &inventory, Evidence evidence, std::string &error);
bool erase(Inventory &inventory, const std::string &packageId,
           uint32_t context, uint32_t players, Kind kind);
size_t erasePackage(Inventory &inventory, const std::string &packageId);

const Evidence *find(const Inventory &inventory, const std::string &packageId,
                     uint32_t context, uint32_t players, Kind kind);
Evidence *find(Inventory &inventory, const std::string &packageId,
               uint32_t context, uint32_t players, Kind kind);

bool qualified(const Evidence &evidence);
PerformanceTarget performanceTarget(uint32_t players);
PerformanceResult performanceResult(const Evidence &evidence);
bool performanceTargetMet(const Evidence &evidence);
bool comparable(const Evidence &latest, const Evidence &baseline);

// Validate and summarize one complete current-device matrix. Select is always
// required; vehicleMask bits 0..2 add car, hovercraft, and plane. Every
// applicable context requires 1P through 4P qualified Latest evidence from
// one exact workload, build, backend, adapter, driver, and physical device.
// Output/render sizes remain row-owned because a user may deliberately qualify
// different window sizes without losing that evidence.
bool summarizeDeviceProfile(
    const DeviceProfileWorkload &workload,
    const std::vector<Evidence> &records,
    DeviceProfileSummary &summary,
    std::string &error);

// Exclusively creates a bounded schema-v1 JSON report. Existing destinations
// are never replaced; a failed write is removed. Callers must collect explicit
// consent before invoking this because adapter/driver and PCI-style IDs are
// deliberately present for corpus stratification.
bool exportDeviceProfileJson(
    const std::string &outputPath,
    const DeviceProfileWorkload &workload,
    const std::vector<Evidence> &records,
    DeviceProfileSummary &summary,
    std::string &error);

} // namespace CharacterTestEvidenceStore

#endif // MDKR_APP_CHARACTER_TEST_EVIDENCE_STORE_H
