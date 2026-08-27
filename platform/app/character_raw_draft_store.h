#ifndef MDKR_APP_CHARACTER_RAW_DRAFT_STORE_H
#define MDKR_APP_CHARACTER_RAW_DRAFT_STORE_H

#include "text_state_file.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace CharacterRawDraftStore {

constexpr size_t kMaximumDrafts           = 64u;
constexpr size_t kMaximumPathBytes        = 4095u;
constexpr size_t kMaximumPackageIdBytes   = 64u;
constexpr size_t kMaximumDisplayNameBytes = 96u;
constexpr size_t kMaximumSpdxBytes        = 128u;
constexpr size_t kMaximumAttributionBytes = 256u;
constexpr size_t kMaximumSourceUrlBytes   = 2048u;
constexpr size_t kMaximumMappingNameBytes = 256u;

struct Draft {
    std::string id;
    uint64_t    updatedUnix = 0u;
    std::string modelPath;
    std::string licensePath;
    std::string packageId;
    std::string displayName;
    std::string spdx;
    std::string attribution;
    std::string sourceUrl;
    uint32_t    donor         = 9u;
    uint32_t    vehicleMask   = 7u;
    uint32_t    sourceForward = 0u;
    float       targetHeight  = 1.25f;
    std::string mappingModelSha256;
    std::string fallback;
    std::string seat;
    std::string head;
    // Digest of model fingerprint + chosen forward axis + target height. Empty
    // means the transform proposal has not been explicitly accepted.
    std::string transformReviewSignature;
};

struct Inventory {
    std::string        selectedId;
    std::vector<Draft> drafts;
};

enum class LoadResult {
    Loaded,
    Missing,
    Invalid,
    IoError,
};

// The file is canonical, bounded, row- and inventory-checksummed, and
// transactional: output is not changed unless every row, selection, ordering,
// count, and whole-inventory checksum passes.
bool         parse(const std::string &text, Inventory &output, std::string &error);
bool         serialize(const Inventory &inventory, std::string &output, std::string &error);

LoadResult   load(const MdkrTextStateStorage &storage, Inventory &output, std::string &error);
bool         save(const MdkrTextStateStorage &storage, const Inventory &inventory, std::string &error);

bool         upsert(Inventory &inventory, Draft draft, std::string &error);
bool         erase(Inventory &inventory, const std::string &draftId);
const Draft *find(const Inventory &inventory, const std::string &draftId);
Draft       *find(Inventory &inventory, const std::string &draftId);

} // namespace CharacterRawDraftStore

#endif // MDKR_APP_CHARACTER_RAW_DRAFT_STORE_H
