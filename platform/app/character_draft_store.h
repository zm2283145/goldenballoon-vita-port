#ifndef MDKR_APP_CHARACTER_DRAFT_STORE_H
#define MDKR_APP_CHARACTER_DRAFT_STORE_H

#include "text_state_file.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace CharacterDraftStore {

constexpr size_t kMaximumDrafts = 64u;
constexpr size_t kMaximumNameBytes = 96u;
constexpr size_t kMaximumPayloadBytes = 128u * 1024u;

struct Draft {
    std::string id;
    std::string packageId;
    std::string baseSourceDigest;
    uint64_t updatedUnix = 0u;
    std::string name;
    std::string payload;
};

struct Inventory {
    std::vector<Draft> drafts;
};

enum class LoadResult {
    Loaded,
    Missing,
    Invalid,
    IoError,
};

// Parser output is changed only after the complete bounded file, per-record
// digest, UTF-8 profile and uniqueness constraints have passed.
bool parse(const std::string &text, Inventory &output, std::string &error);
bool serialize(const Inventory &inventory, std::string &output,
               std::string &error);

LoadResult load(const MdkrTextStateStorage &storage, Inventory &output,
                std::string &error);
bool save(const MdkrTextStateStorage &storage, const Inventory &inventory,
          std::string &error);

bool upsert(Inventory &inventory, Draft draft, std::string &error);
bool erase(Inventory &inventory, const std::string &draftId);
const Draft *find(const Inventory &inventory, const std::string &draftId);

}  // namespace CharacterDraftStore

#endif  // MDKR_APP_CHARACTER_DRAFT_STORE_H
