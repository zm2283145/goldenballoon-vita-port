#ifndef MDKR_APP_CHARACTER_TEST_EVIDENCE_STORE_H
#define MDKR_APP_CHARACTER_TEST_EVIDENCE_STORE_H

#include "text_state_file.h"

#include <cstddef>
#include <cstdint>
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
    uint64_t    replacementDraws            = 0u;
    uint64_t    replacementPrimitives       = 0u;
    uint64_t    hiddenDonorBatches          = 0u;
    uint64_t    contactSolves               = 0u;
    uint64_t    contactErrorMeanMicrometres = 0u;
    uint64_t    contactErrorMaxMicrometres  = 0u;
    bool        fitDiagnosticsValid         = false;
    int64_t     fitBoundsMinMicrometres[3]  = {};
    int64_t     fitBoundsMaxMicrometres[3]  = {};
    int64_t     fitAnchorMicrometres[3]     = {};
    int32_t     fitForwardMilli[3]          = {};
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
bool comparable(const Evidence &latest, const Evidence &baseline);

} // namespace CharacterTestEvidenceStore

#endif // MDKR_APP_CHARACTER_TEST_EVIDENCE_STORE_H
