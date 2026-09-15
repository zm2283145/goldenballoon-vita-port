#include "character_draft_transfer.h"

#include "character_draft_snapshot.h"
#include "sha256.h"

#include <algorithm>
#include <climits>
#include <utility>
#include <vector>

namespace {

constexpr const char *kHeader = "mdkr-character-draft-bundle-v1";

bool digestValid(const std::string &digest) {
    return digest.size() == 64u &&
        std::all_of(digest.begin(), digest.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') ||
                   (byte >= 'a' && byte <= 'f');
        });
}

bool parseUnsigned(const std::string &text, uint64_t maximum,
                   uint64_t &value) {
    uint64_t parsed = 0u;
    if (text.empty() || (text.size() > 1u && text[0] == '0')) return false;
    for (char byte : text) {
        if (byte < '0' || byte > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(byte - '0');
        if (digit > maximum || parsed > (maximum - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

bool split(const std::string &line, size_t count,
           std::vector<std::string> &fields) {
    fields.clear();
    size_t begin = 0u;
    while (true) {
        const size_t tab = line.find('\t', begin);
        fields.push_back(line.substr(
            begin, tab == std::string::npos ? std::string::npos : tab - begin));
        if (tab == std::string::npos) break;
        begin = tab + 1u;
    }
    return fields.size() == count;
}

std::string sha256Hex(const std::string &bytes) {
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(bytes.data(), bytes.size(), digest);
    return digest;
}

bool transferPayload(const std::string &payload, std::string &output,
                     std::string &error) {
    CharacterDraftSnapshot::Snapshot snapshot;
    if (!CharacterDraftSnapshot::decode(payload, snapshot, error)) {
        error = "draft snapshot is not transferable: " + error;
        return false;
    }
    snapshot.portraitSourcePath.clear();
    if (!CharacterDraftSnapshot::encode(snapshot, output, error)) {
        error = "draft snapshot could not be privacy-normalized: " + error;
        return false;
    }
    return true;
}

bool normalizeDraft(const CharacterDraftStore::Draft &source,
                    CharacterDraftStore::Draft &output,
                    std::string &error) {
    output = source;
    return transferPayload(source.payload, output.payload, error);
}

bool sameTransferContent(const CharacterDraftStore::Draft &left,
                         const CharacterDraftStore::Draft &right) {
    if (left.packageId != right.packageId ||
        left.baseSourceDigest != right.baseSourceDigest ||
        left.name != right.name) return false;
    std::string leftPayload;
    std::string rightPayload;
    std::string error;
    return transferPayload(left.payload, leftPayload, error) &&
        transferPayload(right.payload, rightPayload, error) &&
        leftPayload == rightPayload;
}

std::string importedId(const CharacterDraftStore::Draft &draft,
                       uint64_t attempt) {
    std::string seed = draft.id;
    seed.push_back('\0');
    seed += draft.packageId;
    seed.push_back('\0');
    seed += draft.baseSourceDigest;
    seed.push_back('\0');
    seed += draft.name;
    seed.push_back('\0');
    seed += draft.payload;
    seed.push_back('\0');
    seed += std::to_string(attempt);
    return "import-" + sha256Hex(seed).substr(0u, 24u);
}

}  // namespace

namespace CharacterDraftTransfer {

bool create(const CharacterDraftStore::Inventory &source,
            const std::string &packageId,
            const std::string &baseSourceDigest,
            uint64_t createdUnix, Bundle &output, std::string &error) {
    Bundle prepared;
    prepared.packageId = packageId;
    prepared.baseSourceDigest = baseSourceDigest;
    prepared.createdUnix = createdUnix;
    if (!digestValid(baseSourceDigest)) {
        error = "draft bundle source digest is invalid";
        return false;
    }
    for (const CharacterDraftStore::Draft &draft : source.drafts) {
        if (draft.packageId != packageId ||
            draft.baseSourceDigest != baseSourceDigest) continue;
        CharacterDraftStore::Draft normalized;
        if (!normalizeDraft(draft, normalized, error)) return false;
        prepared.inventory.drafts.push_back(std::move(normalized));
    }
    if (prepared.inventory.drafts.empty()) {
        error = "there are no named drafts for the active source revision";
        return false;
    }
    std::string validation;
    if (!CharacterDraftStore::serialize(
            prepared.inventory, validation, error)) return false;
    output = std::move(prepared);
    error.clear();
    return true;
}

bool encode(const Bundle &bundle, std::string &output, std::string &error) {
    if (bundle.inventory.drafts.empty() ||
        bundle.inventory.drafts.size() > CharacterDraftStore::kMaximumDrafts ||
        !digestValid(bundle.baseSourceDigest)) {
        error = "draft bundle metadata is invalid";
        return false;
    }
    CharacterDraftStore::Inventory normalized;
    normalized.drafts.reserve(bundle.inventory.drafts.size());
    for (const CharacterDraftStore::Draft &draft : bundle.inventory.drafts) {
        if (draft.packageId != bundle.packageId ||
            draft.baseSourceDigest != bundle.baseSourceDigest) {
            error = "draft bundle mixes packages or source revisions";
            return false;
        }
        CharacterDraftStore::Draft transferable;
        if (!normalizeDraft(draft, transferable, error)) return false;
        normalized.drafts.push_back(std::move(transferable));
    }
    std::string payload;
    if (!CharacterDraftStore::serialize(normalized, payload, error)) {
        return false;
    }
    const std::string header = std::string(kHeader) + "\t" +
        bundle.packageId + "\t" + bundle.baseSourceDigest + "\t" +
        std::to_string(bundle.createdUnix) + "\t" +
        std::to_string(normalized.drafts.size()) + "\t" +
        std::to_string(payload.size()) + "\t" + sha256Hex(payload) + "\n";
    if (header.size() > kMaximumBundleBytes ||
        payload.size() > kMaximumBundleBytes - header.size()) {
        error = "draft bundle exceeds its byte bound";
        return false;
    }
    output = header + payload;
    error.clear();
    return true;
}

bool parse(const std::string &input, Bundle &output, std::string &error) {
    if (input.size() > kMaximumBundleBytes) {
        error = "draft bundle exceeds its byte bound";
        return false;
    }
    const size_t newline = input.find('\n');
    std::vector<std::string> fields;
    uint64_t created = 0u;
    uint64_t count = 0u;
    uint64_t payloadSize = 0u;
    if (newline == std::string::npos ||
        !split(input.substr(0u, newline), 7u, fields) ||
        fields[0] != kHeader || !digestValid(fields[2]) ||
        !parseUnsigned(fields[3], UINT64_MAX, created) ||
        !parseUnsigned(fields[4], CharacterDraftStore::kMaximumDrafts, count) ||
        count == 0u ||
        !parseUnsigned(fields[5], kMaximumBundleBytes, payloadSize) ||
        !digestValid(fields[6]) || newline + 1u > input.size() ||
        payloadSize != input.size() - newline - 1u) {
        error = "draft bundle header is invalid";
        return false;
    }
    const std::string payload = input.substr(newline + 1u);
    if (sha256Hex(payload) != fields[6]) {
        error = "draft bundle payload digest does not match";
        return false;
    }
    Bundle parsed;
    parsed.packageId = fields[1];
    parsed.baseSourceDigest = fields[2];
    parsed.createdUnix = created;
    if (!CharacterDraftStore::parse(payload, parsed.inventory, error) ||
        parsed.inventory.drafts.size() != count) {
        if (error.empty()) error = "draft bundle count does not match";
        return false;
    }
    for (CharacterDraftStore::Draft &draft : parsed.inventory.drafts) {
        if (draft.packageId != parsed.packageId ||
            draft.baseSourceDigest != parsed.baseSourceDigest) {
            error = "draft bundle mixes packages or source revisions";
            return false;
        }
        CharacterDraftStore::Draft normalized;
        if (!normalizeDraft(draft, normalized, error)) return false;
        CharacterDraftSnapshot::Snapshot snapshot;
        if (!CharacterDraftSnapshot::decode(
                draft.payload, snapshot, error) ||
            !snapshot.portraitSourcePath.empty()) {
            error = "draft bundle contains a local portrait source path";
            return false;
        }
        draft = std::move(normalized);
    }
    output = std::move(parsed);
    error.clear();
    return true;
}

bool merge(const CharacterDraftStore::Inventory &current,
           const Bundle &bundle,
           CharacterDraftStore::Inventory &output,
           MergeSummary &summary, std::string &error) {
    CharacterDraftStore::Inventory merged = current;
    MergeSummary result;
    result.input = bundle.inventory.drafts.size();
    for (const CharacterDraftStore::Draft &incoming :
         bundle.inventory.drafts) {
        if (incoming.packageId != bundle.packageId ||
            incoming.baseSourceDigest != bundle.baseSourceDigest) {
            error = "draft bundle changed after review";
            return false;
        }
        CharacterDraftStore::Draft normalized;
        if (!normalizeDraft(incoming, normalized, error)) return false;
        const auto duplicate = std::find_if(
            merged.drafts.begin(), merged.drafts.end(),
            [&normalized](const CharacterDraftStore::Draft &candidate) {
                return sameTransferContent(candidate, normalized);
            });
        if (duplicate != merged.drafts.end()) {
            ++result.duplicates;
            continue;
        }
        if (CharacterDraftStore::find(merged, normalized.id) != nullptr) {
            bool allocated = false;
            for (uint64_t attempt = 0u;
                 attempt <= CharacterDraftStore::kMaximumDrafts; ++attempt) {
                const std::string candidate = importedId(normalized, attempt);
                if (CharacterDraftStore::find(merged, candidate) == nullptr) {
                    normalized.id = candidate;
                    allocated = true;
                    break;
                }
            }
            if (!allocated) {
                error = "could not allocate a non-overwriting imported draft id";
                return false;
            }
            ++result.renamed;
        }
        if (!CharacterDraftStore::upsert(merged, normalized, error)) {
            return false;
        }
        ++result.additions;
    }
    std::string validation;
    if (!CharacterDraftStore::serialize(merged, validation, error)) {
        return false;
    }
    output = std::move(merged);
    summary = result;
    error.clear();
    return true;
}

}  // namespace CharacterDraftTransfer
