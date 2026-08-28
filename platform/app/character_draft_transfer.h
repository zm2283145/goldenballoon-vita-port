#ifndef MDKR_APP_CHARACTER_DRAFT_TRANSFER_H
#define MDKR_APP_CHARACTER_DRAFT_TRANSFER_H

#include "character_draft_store.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace CharacterDraftTransfer {

constexpr size_t kMaximumBundleBytes =
    CharacterDraftStore::kMaximumDrafts *
        (CharacterDraftStore::kMaximumPayloadBytes * 2u + 640u) + 1024u;

struct Bundle {
    std::string packageId;
    std::string baseSourceDigest;
    uint64_t createdUnix = 0u;
    CharacterDraftStore::Inventory inventory;
};

struct MergeSummary {
    size_t input = 0u;
    size_t additions = 0u;
    size_t duplicates = 0u;
    size_t renamed = 0u;
};

// Create one exact-base transfer. Drafts on retained source revisions remain
// local and can be exported after restoring that revision. The transferable
// snapshots are decoded and re-encoded after clearing local portrait paths;
// exact framed pixels and source records remain self-contained.
bool create(const CharacterDraftStore::Inventory &source,
            const std::string &packageId,
            const std::string &baseSourceDigest,
            uint64_t createdUnix, Bundle &output, std::string &error);

// The outer digest covers the complete deterministic draft inventory. Parsing
// is fail-atomic and also validates every opaque snapshot before publishing it.
bool encode(const Bundle &bundle, std::string &output, std::string &error);
bool parse(const std::string &input, Bundle &output, std::string &error);

// Additive merge. Exact semantic duplicates are skipped; an id collision with
// different content receives a fresh deterministic local id. The destination
// is changed only if the whole bounded merge fits and validates.
bool merge(const CharacterDraftStore::Inventory &current,
           const Bundle &bundle,
           CharacterDraftStore::Inventory &output,
           MergeSummary &summary, std::string &error);

}  // namespace CharacterDraftTransfer

#endif  // MDKR_APP_CHARACTER_DRAFT_TRANSFER_H
