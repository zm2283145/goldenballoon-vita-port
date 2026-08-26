#include "character_draft_store.h"

#include "sha256.h"

#include <algorithm>
#include <climits>
#include <set>
#include <utility>

namespace {

constexpr const char *kHeader = "mdkr-character-drafts-v1";
constexpr size_t kMaximumSerializedBytes =
    CharacterDraftStore::kMaximumDrafts *
        (CharacterDraftStore::kMaximumPayloadBytes * 2u + 640u) + 64u;

bool slugValid(const std::string &value) {
    if (value.size() < 2u || value.size() > 64u ||
        !((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) return false;
    return std::all_of(value.begin() + 1, value.end(), [](char byte) {
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

int hexNibble(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

bool decodeHex(const std::string &encoded, size_t maximum,
               bool allowEmpty, std::string &output) {
    if ((encoded.size() & 1u) != 0u || encoded.size() > maximum * 2u ||
        (!allowEmpty && encoded.empty())) return false;
    std::string decoded;
    decoded.reserve(encoded.size() / 2u);
    for (size_t index = 0u; index < encoded.size(); index += 2u) {
        const int high = hexNibble(encoded[index]);
        const int low = hexNibble(encoded[index + 1u]);
        if (high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    output = std::move(decoded);
    return true;
}

std::string encodeHex(const std::string &value) {
    static const char digits[] = "0123456789abcdef";
    std::string encoded(value.size() * 2u, '0');
    for (size_t index = 0u; index < value.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        encoded[index * 2u] = digits[byte >> 4u];
        encoded[index * 2u + 1u] = digits[byte & 0xFu];
    }
    return encoded;
}

bool printableUtf8(const std::string &text) {
    size_t index = 0u;
    bool nonSpace = false;
    if (text.empty() ||
        text.size() > CharacterDraftStore::kMaximumNameBytes) return false;
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index++]);
        uint32_t codepoint;
        uint32_t minimum;
        unsigned continuation;
        if (first < 0x80u) {
            codepoint = first;
            minimum = 0u;
            continuation = 0u;
        } else if (first >= 0xC2u && first <= 0xDFu) {
            codepoint = first & 0x1Fu;
            minimum = 0x80u;
            continuation = 1u;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            codepoint = first & 0x0Fu;
            minimum = 0x800u;
            continuation = 2u;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            codepoint = first & 0x07u;
            minimum = 0x10000u;
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
    return nonSpace;
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

std::string recordDigest(const CharacterDraftStore::Draft &draft) {
    MdkrSha256 digest;
    uint8_t bytes[MDKR_SHA256_DIGEST_SIZE];
    static const char digits[] = "0123456789abcdef";
    std::string updated = std::to_string(draft.updatedUnix);
    char hex[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_init(&digest);
    const auto add = [&digest](const std::string &value) {
        mdkr_sha256_update(&digest, value.data(), value.size());
        const unsigned char separator = 0u;
        mdkr_sha256_update(&digest, &separator, 1u);
    };
    add(draft.id);
    add(draft.packageId);
    add(draft.baseSourceDigest);
    add(updated);
    add(draft.name);
    mdkr_sha256_update(&digest, draft.payload.data(), draft.payload.size());
    mdkr_sha256_final(&digest, bytes);
    for (size_t index = 0u; index < sizeof(bytes); ++index) {
        hex[index * 2u] = digits[bytes[index] >> 4u];
        hex[index * 2u + 1u] = digits[bytes[index] & 0xFu];
    }
    hex[64] = '\0';
    return hex;
}

bool draftValid(const CharacterDraftStore::Draft &draft,
                std::string &error) {
    if (!slugValid(draft.id)) error = "draft id is not a lowercase slug";
    else if (!slugValid(draft.packageId))
        error = "draft package id is not a lowercase slug";
    else if (!digestValid(draft.baseSourceDigest))
        error = "draft base source digest is invalid";
    else if (!printableUtf8(draft.name))
        error = "draft name is not bounded printable UTF-8";
    else if (draft.payload.empty() ||
             draft.payload.size() >
                 CharacterDraftStore::kMaximumPayloadBytes)
        error = "draft payload is empty or oversized";
    else return true;
    return false;
}

}  // namespace

namespace CharacterDraftStore {

bool parse(const std::string &text, Inventory &output, std::string &error) {
    Inventory parsed;
    std::vector<std::string> fields;
    size_t begin = 0u;
    size_t end = text.find('\n');
    uint64_t count = 0u;
    if (text.size() > kMaximumSerializedBytes || end == std::string::npos ||
        !split(text.substr(0u, end), 2u, fields) || fields[0] != kHeader ||
        !parseUnsigned(fields[1], kMaximumDrafts, count)) {
        error = "draft inventory header is invalid";
        return false;
    }
    begin = end + 1u;
    for (uint64_t index = 0u; index < count; ++index) {
        end = text.find('\n', begin);
        Draft draft;
        std::string checksum;
        if (end == std::string::npos ||
            !split(text.substr(begin, end - begin), 7u, fields) ||
            !parseUnsigned(fields[3], UINT64_MAX, draft.updatedUnix) ||
            !decodeHex(fields[4], kMaximumNameBytes, false, draft.name) ||
            !decodeHex(fields[5], kMaximumPayloadBytes, false, draft.payload) ||
            !digestValid(fields[6])) {
            error = "draft inventory row is malformed";
            return false;
        }
        draft.id = fields[0];
        draft.packageId = fields[1];
        draft.baseSourceDigest = fields[2];
        checksum = fields[6];
        if (!draftValid(draft, error) || recordDigest(draft) != checksum ||
            find(parsed, draft.id) != nullptr) {
            if (error.empty()) error = "draft row checksum or id is invalid";
            return false;
        }
        parsed.drafts.push_back(std::move(draft));
        begin = end + 1u;
    }
    if (begin != text.size()) {
        error = "draft inventory has trailing data";
        return false;
    }
    output = std::move(parsed);
    error.clear();
    return true;
}

bool serialize(const Inventory &inventory, std::string &output,
               std::string &error) {
    if (inventory.drafts.size() > kMaximumDrafts) {
        error = "draft inventory exceeds its entry bound";
        return false;
    }
    Inventory ordered = inventory;
    std::sort(ordered.drafts.begin(), ordered.drafts.end(),
              [](const Draft &left, const Draft &right) {
                  if (left.updatedUnix != right.updatedUnix) {
                      return left.updatedUnix > right.updatedUnix;
                  }
                  return left.id < right.id;
              });
    std::string result = std::string(kHeader) + "\t" +
        std::to_string(ordered.drafts.size()) + "\n";
    std::set<std::string> ids;
    for (const Draft &draft : ordered.drafts) {
        if (!draftValid(draft, error) || !ids.insert(draft.id).second) {
            if (error.empty()) error = "draft inventory contains duplicate ids";
            return false;
        }
        result += draft.id + "\t" + draft.packageId + "\t" +
            draft.baseSourceDigest + "\t" +
            std::to_string(draft.updatedUnix) + "\t" +
            encodeHex(draft.name) + "\t" + encodeHex(draft.payload) + "\t" +
            recordDigest(draft) + "\n";
        if (result.size() > kMaximumSerializedBytes) {
            error = "serialized draft inventory exceeds its byte bound";
            return false;
        }
    }
    output = std::move(result);
    error.clear();
    return true;
}

LoadResult load(const MdkrTextStateStorage &storage, Inventory &output,
                std::string &error) {
    if (storage.read == nullptr) {
        error = "draft storage has no read callback";
        return LoadResult::IoError;
    }
    std::string text(kMaximumSerializedBytes + 1u, '\0');
    size_t length = 0u;
    const int result = storage.read(
        storage.context, text.data(), text.size(), &length);
    if (result == 0) {
        output = Inventory{};
        error.clear();
        return LoadResult::Missing;
    }
    if (result < 0 || length > kMaximumSerializedBytes) {
        error = "draft inventory could not be read within its byte bound";
        return LoadResult::IoError;
    }
    text.resize(length);
    if (!parse(text, output, error)) return LoadResult::Invalid;
    return LoadResult::Loaded;
}

bool save(const MdkrTextStateStorage &storage, const Inventory &inventory,
          std::string &error) {
    std::string text;
    if (storage.write == nullptr || !serialize(inventory, text, error)) {
        if (storage.write == nullptr) error = "draft storage has no write callback";
        return false;
    }
    if (storage.write(storage.context, text.data(), text.size()) != 1) {
        error = "draft inventory could not be atomically replaced";
        return false;
    }
    return true;
}

bool upsert(Inventory &inventory, Draft draft, std::string &error) {
    if (!draftValid(draft, error)) return false;
    auto found = std::find_if(
        inventory.drafts.begin(), inventory.drafts.end(),
        [&draft](const Draft &existing) { return existing.id == draft.id; });
    if (found == inventory.drafts.end()) {
        if (inventory.drafts.size() == kMaximumDrafts) {
            error = "draft inventory is full";
            return false;
        }
        inventory.drafts.push_back(std::move(draft));
    } else {
        *found = std::move(draft);
    }
    error.clear();
    return true;
}

bool erase(Inventory &inventory, const std::string &draftId) {
    const auto found = std::find_if(
        inventory.drafts.begin(), inventory.drafts.end(),
        [&draftId](const Draft &draft) { return draft.id == draftId; });
    if (found == inventory.drafts.end()) return false;
    inventory.drafts.erase(found);
    return true;
}

const Draft *find(const Inventory &inventory, const std::string &draftId) {
    const auto found = std::find_if(
        inventory.drafts.begin(), inventory.drafts.end(),
        [&draftId](const Draft &draft) { return draft.id == draftId; });
    return found == inventory.drafts.end() ? nullptr : &*found;
}

}  // namespace CharacterDraftStore
