#include "character_raw_draft_store.h"

#include "sha256.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace {

static_assert(sizeof(float) == sizeof(uint32_t) &&
                  std::numeric_limits<float>::is_iec559,
              "raw draft persistence requires 32-bit IEEE-754 floats");

constexpr const char *kHeader       = "mdkr-character-raw-drafts-v1";
constexpr size_t      kFieldsPerRow = 18u;
constexpr size_t      kMaximumRowBytes =
    (CharacterRawDraftStore::kMaximumPathBytes * 2u) * 2u +
    (CharacterRawDraftStore::kMaximumSourceUrlBytes * 2u) + 4096u;
constexpr size_t kMaximumSerializedBytes =
    CharacterRawDraftStore::kMaximumDrafts * kMaximumRowBytes + 256u;

bool slugValid(const std::string &value) {
    if (value.size() < 2u || value.size() > 64u ||
        !((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) return false;
    return std::all_of(value.begin() + 1u, value.end(), [](char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '.' ||
               byte == '_' || byte == '-';
    });
}

bool digestValid(const std::string &digest) {
    return digest.size() == 64u &&
           std::all_of(digest.begin(), digest.end(), [](char byte) {
               return (byte >= '0' && byte <= '9') ||
                      (byte >= 'a' && byte <= 'f');
           });
}

bool parseUnsigned(const std::string &text, uint64_t maximum, uint64_t &value) {
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

int hexNibble(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

bool decodeHex(const std::string &encoded, size_t maximum, std::string &output) {
    if ((encoded.size() & 1u) != 0u || encoded.size() > maximum * 2u) {
        return false;
    }
    std::string decoded;
    decoded.reserve(encoded.size() / 2u);
    for (size_t index = 0u; index < encoded.size(); index += 2u) {
        const int high = hexNibble(encoded[index]);
        const int low  = hexNibble(encoded[index + 1u]);
        if (high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    output = std::move(decoded);
    return true;
}

std::string encodeHex(const std::string &value) {
    static const char digits[] = "0123456789abcdef";
    std::string       encoded(value.size() * 2u, '0');
    for (size_t index = 0u; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        encoded[index * 2u]      = digits[byte >> 4u];
        encoded[index * 2u + 1u] = digits[byte & 0xFu];
    }
    return encoded;
}

bool printableUtf8(const std::string &text, size_t maximum, bool requireNonSpace) {
    if (text.size() > maximum || (requireNonSpace && text.empty())) {
        return false;
    }
    size_t index    = 0u;
    bool   nonSpace = false;
    while (index < text.size()) {
        const unsigned char first        = static_cast<unsigned char>(text[index++]);
        uint32_t            codepoint    = 0u;
        uint32_t            minimum      = 0u;
        unsigned            continuation = 0u;
        if (first < 0x80u) {
            codepoint = first;
        } else if (first >= 0xC2u && first <= 0xDFu) {
            codepoint    = first & 0x1Fu;
            minimum      = 0x80u;
            continuation = 1u;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            codepoint    = first & 0x0Fu;
            minimum      = 0x800u;
            continuation = 2u;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            codepoint    = first & 0x07u;
            minimum      = 0x10000u;
            continuation = 3u;
        } else {
            return false;
        }
        if (continuation > text.size() - index) return false;
        while (continuation-- != 0u) {
            const unsigned char next =
                static_cast<unsigned char>(text[index++]);
            if ((next & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3Fu);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
            codepoint < 0x20u ||
            (codepoint >= 0x7Fu && codepoint <= 0x9Fu) ||
            (codepoint >= 0x200Bu && codepoint <= 0x200Fu) ||
            (codepoint >= 0x2028u && codepoint <= 0x202Eu) ||
            (codepoint >= 0x2060u && codepoint <= 0x206Fu) ||
            codepoint == 0xFEFFu) return false;
        if (codepoint != ' ' && codepoint != '\t') nonSpace = true;
    }
    return !requireNonSpace || nonSpace;
}

bool split(const std::string &line, size_t count, std::vector<std::string> &fields) {
    fields.clear();
    size_t begin = 0u;
    while (true) {
        const size_t tab = line.find('\t', begin);
        fields.push_back(line.substr(
            begin,
            tab == std::string::npos ? std::string::npos : tab - begin));
        if (tab == std::string::npos) break;
        begin = tab + 1u;
    }
    return fields.size() == count;
}

std::string floatBits(float value) {
    static const char digits[] = "0123456789abcdef";
    uint32_t          bits     = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    std::string result(8u, '0');
    for (size_t index = 0u; index < 8u; ++index) {
        result[7u - index] = digits[bits & 0xFu];
        bits >>= 4u;
    }
    return result;
}

bool parseFloatBits(const std::string &text, float &value) {
    if (text.size() != 8u) return false;
    uint32_t bits = 0u;
    for (char byte : text) {
        const int nibble = hexNibble(byte);
        if (nibble < 0) return false;
        bits = (bits << 4u) | static_cast<uint32_t>(nibble);
    }
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) && value >= 0.1f && value <= 10.0f;
}

bool draftValid(const CharacterRawDraftStore::Draft &draft,
                std::string                         &error) {
    using namespace CharacterRawDraftStore;
    if (!slugValid(draft.id))
        error = "raw draft id is not a lowercase slug";
    else if (!printableUtf8(draft.modelPath, kMaximumPathBytes, true))
        error = "raw draft model path is not bounded printable UTF-8";
    else if (!printableUtf8(draft.licensePath, kMaximumPathBytes, false))
        error = "raw draft license path is not bounded printable UTF-8";
    else if (!draft.packageId.empty() &&
             (!slugValid(draft.packageId) ||
              draft.packageId.size() > kMaximumPackageIdBytes))
        error = "raw draft package id is invalid";
    else if (!printableUtf8(draft.displayName, kMaximumDisplayNameBytes, false))
        error = "raw draft display name is invalid";
    else if (!printableUtf8(draft.spdx, kMaximumSpdxBytes, false))
        error = "raw draft SPDX expression is invalid";
    else if (!printableUtf8(draft.attribution, kMaximumAttributionBytes, false))
        error = "raw draft attribution is invalid";
    else if (!printableUtf8(draft.sourceUrl, kMaximumSourceUrlBytes, false))
        error = "raw draft source URL is invalid";
    else if (draft.donor > 9u || draft.vehicleMask > 7u ||
             draft.sourceForward > 3u)
        error = "raw draft profile choices are out of range";
    else if (!std::isfinite(draft.targetHeight) ||
             draft.targetHeight < 0.1f || draft.targetHeight > 10.0f)
        error = "raw draft target height is out of range";
    else if (!draft.mappingModelSha256.empty() &&
             !digestValid(draft.mappingModelSha256))
        error = "raw draft mapping fingerprint is invalid";
    else if (draft.mappingModelSha256.empty() &&
             (!draft.fallback.empty() || !draft.seat.empty() ||
              !draft.head.empty()))
        error = "raw draft mappings have no source fingerprint";
    else if (!printableUtf8(draft.fallback, kMaximumMappingNameBytes, false) ||
             !printableUtf8(draft.seat, kMaximumMappingNameBytes, false) ||
             !printableUtf8(draft.head, kMaximumMappingNameBytes, false))
        error = "raw draft mapping name is invalid";
    else
        return true;
    return false;
}

bool draftComesBefore(const CharacterRawDraftStore::Draft &left,
                      const CharacterRawDraftStore::Draft &right) {
    if (left.updatedUnix != right.updatedUnix) {
        return left.updatedUnix > right.updatedUnix;
    }
    return left.id < right.id;
}

std::vector<std::string> recordFields(
    const CharacterRawDraftStore::Draft &draft) {
    return {
        draft.id,
        std::to_string(draft.updatedUnix),
        encodeHex(draft.modelPath),
        encodeHex(draft.licensePath),
        encodeHex(draft.packageId),
        encodeHex(draft.displayName),
        encodeHex(draft.spdx),
        encodeHex(draft.attribution),
        encodeHex(draft.sourceUrl),
        std::to_string(draft.donor),
        std::to_string(draft.vehicleMask),
        std::to_string(draft.sourceForward),
        floatBits(draft.targetHeight),
        draft.mappingModelSha256.empty() ? "-" : draft.mappingModelSha256,
        encodeHex(draft.fallback),
        encodeHex(draft.seat),
        encodeHex(draft.head),
    };
}

std::string finishDigest(MdkrSha256 &digest) {
    uint8_t           bytes[MDKR_SHA256_DIGEST_SIZE];
    char              hex[MDKR_SHA256_HEX_SIZE];
    static const char digits[] = "0123456789abcdef";
    mdkr_sha256_final(&digest, bytes);
    for (size_t index = 0u; index < sizeof(bytes); ++index) {
        hex[index * 2u]      = digits[bytes[index] >> 4u];
        hex[index * 2u + 1u] = digits[bytes[index] & 0xFu];
    }
    hex[64] = '\0';
    return hex;
}

std::string recordDigest(const std::string              &selectedId,
                         const std::vector<std::string> &fields) {
    MdkrSha256 digest;
    mdkr_sha256_init(&digest);
    mdkr_sha256_update(&digest, selectedId.data(), selectedId.size());
    const unsigned char selectionSeparator = 0u;
    mdkr_sha256_update(&digest, &selectionSeparator, 1u);
    for (const std::string &field : fields) {
        mdkr_sha256_update(&digest, field.data(), field.size());
        const unsigned char separator = 0u;
        mdkr_sha256_update(&digest, &separator, 1u);
    }
    return finishDigest(digest);
}

std::string inventoryDigest(const std::string &count,
                            const std::string &selected,
                            const std::string &body) {
    MdkrSha256 digest;
    mdkr_sha256_init(&digest);
    const auto add = [&digest](const std::string &value) {
        mdkr_sha256_update(&digest, value.data(), value.size());
        const unsigned char separator = 0u;
        mdkr_sha256_update(&digest, &separator, 1u);
    };
    add(kHeader);
    add(count);
    add(selected);
    mdkr_sha256_update(&digest, body.data(), body.size());
    return finishDigest(digest);
}

} // namespace

namespace CharacterRawDraftStore {

bool parse(const std::string &text, Inventory &output, std::string &error) {
    Inventory                parsed;
    std::vector<std::string> fields;
    size_t                   begin     = 0u;
    size_t                   end       = text.find('\n');
    const size_t             bodyBegin = end == std::string::npos ? 0u : end + 1u;
    uint64_t                 count     = 0u;
    if (text.size() > kMaximumSerializedBytes || end == std::string::npos ||
        !split(text.substr(0u, end), 4u, fields) || fields[0] != kHeader ||
        !parseUnsigned(fields[1], kMaximumDrafts, count) ||
        (fields[2] != "-" && !slugValid(fields[2])) ||
        !digestValid(fields[3])) {
        error = "raw draft inventory header is invalid";
        return false;
    }
    const std::string countText         = fields[1];
    const std::string selectedText      = fields[2];
    const std::string inventoryChecksum = fields[3];
    if (fields[2] != "-") parsed.selectedId = fields[2];
    begin = end + 1u;
    for (uint64_t index = 0u; index < count; ++index) {
        end = text.find('\n', begin);
        if (end == std::string::npos ||
            !split(text.substr(begin, end - begin), kFieldsPerRow, fields)) {
            error = "raw draft inventory row is malformed";
            return false;
        }
        Draft    draft;
        uint64_t donor    = 0u;
        uint64_t vehicles = 0u;
        uint64_t forward  = 0u;
        draft.id          = fields[0];
        if (!parseUnsigned(fields[1], UINT64_MAX, draft.updatedUnix) ||
            !decodeHex(fields[2], kMaximumPathBytes, draft.modelPath) ||
            !decodeHex(fields[3], kMaximumPathBytes, draft.licensePath) ||
            !decodeHex(fields[4], kMaximumPackageIdBytes, draft.packageId) ||
            !decodeHex(fields[5], kMaximumDisplayNameBytes, draft.displayName) ||
            !decodeHex(fields[6], kMaximumSpdxBytes, draft.spdx) ||
            !decodeHex(fields[7], kMaximumAttributionBytes, draft.attribution) ||
            !decodeHex(fields[8], kMaximumSourceUrlBytes, draft.sourceUrl) ||
            !parseUnsigned(fields[9], 9u, donor) ||
            !parseUnsigned(fields[10], 7u, vehicles) ||
            !parseUnsigned(fields[11], 3u, forward) ||
            !parseFloatBits(fields[12], draft.targetHeight) ||
            (fields[13] != "-" && !digestValid(fields[13])) ||
            !decodeHex(fields[14], kMaximumMappingNameBytes, draft.fallback) ||
            !decodeHex(fields[15], kMaximumMappingNameBytes, draft.seat) ||
            !decodeHex(fields[16], kMaximumMappingNameBytes, draft.head) ||
            !digestValid(fields[17])) {
            error = "raw draft inventory row fields are invalid";
            return false;
        }
        draft.donor         = static_cast<uint32_t>(donor);
        draft.vehicleMask   = static_cast<uint32_t>(vehicles);
        draft.sourceForward = static_cast<uint32_t>(forward);
        if (fields[13] != "-") draft.mappingModelSha256 = fields[13];
        const std::string checksum = fields.back();
        fields.pop_back();
        if (!draftValid(draft, error) ||
            recordDigest(parsed.selectedId, fields) != checksum ||
            find(parsed, draft.id) != nullptr ||
            (!parsed.drafts.empty() &&
             !draftComesBefore(parsed.drafts.back(), draft))) {
            if (error.empty()) {
                error = "raw draft row checksum, id, or order is invalid";
            }
            return false;
        }
        parsed.drafts.push_back(std::move(draft));
        begin = end + 1u;
    }
    const bool selectionValid = parsed.drafts.empty()
                                    ? parsed.selectedId.empty()
                                    : !parsed.selectedId.empty() &&
                                          find(parsed, parsed.selectedId) != nullptr;
    if (begin != text.size() || !selectionValid ||
        inventoryDigest(countText, selectedText, text.substr(bodyBegin)) != inventoryChecksum) {
        error = begin != text.size()
                    ? "raw draft inventory has trailing data"
                : !selectionValid
                    ? "raw draft selected id does not exist"
                    : "raw draft inventory checksum is invalid";
        return false;
    }
    output = std::move(parsed);
    error.clear();
    return true;
}

bool serialize(const Inventory &inventory, std::string &output, std::string &error) {
    const bool selectionValid = inventory.drafts.empty()
                                    ? inventory.selectedId.empty()
                                    : !inventory.selectedId.empty() &&
                                          find(inventory, inventory.selectedId) != nullptr;
    if (inventory.drafts.size() > kMaximumDrafts || !selectionValid) {
        error = "raw draft inventory bound or selection is invalid";
        return false;
    }
    Inventory ordered = inventory;
    std::sort(ordered.drafts.begin(), ordered.drafts.end(), draftComesBefore);
    std::string           body;
    std::set<std::string> ids;
    for (const Draft &draft : ordered.drafts) {
        if (!draftValid(draft, error) || !ids.insert(draft.id).second) {
            if (error.empty()) error = "raw draft inventory has duplicate ids";
            return false;
        }
        const std::vector<std::string> fields = recordFields(draft);
        for (const std::string &field : fields) {
            body += field;
            body.push_back('\t');
        }
        body += recordDigest(ordered.selectedId, fields);
        body.push_back('\n');
        if (body.size() > kMaximumSerializedBytes) {
            error = "serialized raw draft inventory exceeds its byte bound";
            return false;
        }
    }
    const std::string count    = std::to_string(ordered.drafts.size());
    const std::string selected = ordered.selectedId.empty()
                                     ? "-"
                                     : ordered.selectedId;
    std::string       result   = std::string(kHeader) + "\t" + count + "\t" +
                         selected + "\t" + inventoryDigest(count, selected, body) + "\n" +
                         body;
    if (result.size() > kMaximumSerializedBytes) {
        error = "serialized raw draft inventory exceeds its byte bound";
        return false;
    }
    output = std::move(result);
    error.clear();
    return true;
}

LoadResult load(const MdkrTextStateStorage &storage, Inventory &output, std::string &error) {
    if (storage.read == nullptr) {
        error = "raw draft storage has no read callback";
        return LoadResult::IoError;
    }
    std::string text(kMaximumSerializedBytes + 1u, '\0');
    size_t      length = 0u;
    const int   result = storage.read(
        storage.context,
        text.data(),
        text.size(),
        &length);
    if (result == 0) {
        output = Inventory{};
        error.clear();
        return LoadResult::Missing;
    }
    if (result < 0 || length > kMaximumSerializedBytes) {
        error = "raw draft inventory could not be read within its byte bound";
        return LoadResult::IoError;
    }
    text.resize(length);
    if (!parse(text, output, error)) return LoadResult::Invalid;
    return LoadResult::Loaded;
}

bool save(const MdkrTextStateStorage &storage, const Inventory &inventory, std::string &error) {
    std::string text;
    if (storage.write == nullptr || !serialize(inventory, text, error)) {
        if (storage.write == nullptr) {
            error = "raw draft storage has no write callback";
        }
        return false;
    }
    if (storage.write(storage.context, text.data(), text.size()) != 1) {
        error = "raw draft inventory could not be atomically replaced";
        return false;
    }
    return true;
}

bool upsert(Inventory &inventory, Draft draft, std::string &error) {
    if (!draftValid(draft, error)) return false;
    Draft *existing = find(inventory, draft.id);
    if (existing == nullptr) {
        if (inventory.drafts.size() == kMaximumDrafts) {
            error = "raw draft inventory is full";
            return false;
        }
        inventory.drafts.push_back(std::move(draft));
    } else {
        *existing = std::move(draft);
    }
    if (inventory.selectedId.empty()) {
        inventory.selectedId = existing == nullptr
                                   ? inventory.drafts.back().id
                                   : existing->id;
    }
    error.clear();
    return true;
}

bool erase(Inventory &inventory, const std::string &draftId) {
    const auto found = std::find_if(
        inventory.drafts.begin(),
        inventory.drafts.end(),
        [&draftId](const Draft &draft) { return draft.id == draftId; });
    if (found == inventory.drafts.end()) return false;
    inventory.drafts.erase(found);
    if (inventory.selectedId == draftId) {
        inventory.selectedId.clear();
        if (!inventory.drafts.empty()) {
            const auto next = std::max_element(
                inventory.drafts.begin(),
                inventory.drafts.end(),
                [](const Draft &left, const Draft &right) {
                    if (left.updatedUnix != right.updatedUnix) {
                        return left.updatedUnix < right.updatedUnix;
                    }
                    return left.id > right.id;
                });
            inventory.selectedId = next->id;
        }
    }
    return true;
}

const Draft *find(const Inventory &inventory, const std::string &draftId) {
    const auto found = std::find_if(
        inventory.drafts.begin(),
        inventory.drafts.end(),
        [&draftId](const Draft &draft) { return draft.id == draftId; });
    return found == inventory.drafts.end() ? nullptr : &*found;
}

Draft *find(Inventory &inventory, const std::string &draftId) {
    const auto found = std::find_if(
        inventory.drafts.begin(),
        inventory.drafts.end(),
        [&draftId](const Draft &draft) { return draft.id == draftId; });
    return found == inventory.drafts.end() ? nullptr : &*found;
}

} // namespace CharacterRawDraftStore
