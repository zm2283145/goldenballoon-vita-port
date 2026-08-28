#include "character_failure_index.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace {

bool splitLine(const std::string &line, size_t expected,
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
    return fields.size() == expected;
}

bool parseUnsigned(const std::string &text, uint64_t maximum,
                   uint64_t &output) {
    if (text.empty() || (text.size() > 1u && text[0] == '0')) return false;
    uint64_t value = 0u;
    for (char byte : text) {
        if (byte < '0' || byte > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(byte - '0');
        if (digit > maximum || value > (maximum - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    output = value;
    return true;
}

bool digestValid(const std::string &digest) {
    return digest.size() == 64u &&
        std::all_of(digest.begin(), digest.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') ||
                   (byte >= 'a' && byte <= 'f');
        });
}

int hexNibble(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

bool safeUtf8(const std::string &text, size_t maximum) {
    size_t index = 0u;
    if (text.empty() || text.size() > maximum) return false;
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index++]);
        uint32_t codepoint = 0u;
        unsigned continuation = 0u;
        if (first < 0x80u) {
            codepoint = first;
        } else if (first >= 0xC2u && first <= 0xDFu) {
            codepoint = first & 0x1Fu;
            continuation = 1u;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            codepoint = first & 0x0Fu;
            continuation = 2u;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            codepoint = first & 0x07u;
            continuation = 3u;
        } else {
            return false;
        }
        if (continuation > text.size() - index) return false;
        for (unsigned count = 0u; count < continuation; ++count) {
            const unsigned char next = static_cast<unsigned char>(text[index++]);
            if ((next & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3Fu);
        }
        if ((continuation == 1u && codepoint < 0x80u) ||
            (continuation == 2u && codepoint < 0x800u) ||
            (continuation == 3u && codepoint < 0x10000u) ||
            codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
            codepoint == 0u || (codepoint >= 0x200Bu && codepoint <= 0x200Fu) ||
            (codepoint >= 0x2028u && codepoint <= 0x202Eu) ||
            (codepoint >= 0x2060u && codepoint <= 0x206Fu) ||
            codepoint == 0xFEFFu) {
            return false;
        }
    }
    return true;
}

bool decodeHex(const std::string &hex, size_t maximum, std::string &output) {
    if (hex.empty() || (hex.size() & 1u) != 0u || hex.size() > maximum * 2u) {
        return false;
    }
    std::string decoded;
    decoded.reserve(hex.size() / 2u);
    for (size_t index = 0u; index < hex.size(); index += 2u) {
        const int high = hexNibble(hex[index]);
        const int low = hexNibble(hex[index + 1u]);
        if (high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    if (!safeUtf8(decoded, maximum)) return false;
    output = std::move(decoded);
    return true;
}

bool decodeOptionalHex(const std::string &hex, size_t maximum,
                       std::string &output) {
    if (hex == "-") {
        output.clear();
        return true;
    }
    return decodeHex(hex, maximum, output);
}

bool parseBoolean(const std::string &text, bool &output) {
    if (text == "0") {
        output = false;
        return true;
    }
    if (text == "1") {
        output = true;
        return true;
    }
    return false;
}

bool parseRetryKind(const std::string &text,
                    CharacterFailureIndex::RetryKind &output) {
    if (text == "raw-inspect") {
        output = CharacterFailureIndex::RetryKind::RawInspect;
    } else if (text == "package-inspect") {
        output = CharacterFailureIndex::RetryKind::PackageInspect;
    } else if (text == "convert") {
        output = CharacterFailureIndex::RetryKind::Convert;
    } else if (text == "adapter-inspect") {
        output = CharacterFailureIndex::RetryKind::AdapterInspect;
    } else {
        return false;
    }
    return true;
}

}  // namespace

namespace CharacterFailureIndex {

bool parse(const std::string &text, Inventory &output) {
    if (text.empty() || text.size() > 64u * 1024u || text.back() != '\n') {
        return false;
    }
    Inventory parsed;
    std::vector<std::string> fields;
    size_t begin = 0u;
    size_t end = text.find('\n');
    uint64_t total = 0u;
    uint64_t visible = 0u;
    if (end == std::string::npos ||
        !splitLine(text.substr(0u, end), 3u, fields) ||
        fields[0] != "mdkr-character-import-failures-v1" ||
        !parseUnsigned(fields[1], 4096u, total) ||
        !parseUnsigned(fields[2], kMaximumRows, visible) || visible > total) {
        return false;
    }
    parsed.total = static_cast<unsigned>(total);
    begin = end + 1u;
    std::set<std::string> identities;
    for (uint64_t index = 0u; index < visible; ++index) {
        end = text.find('\n', begin);
        Row row;
        uint64_t attempts = 0u;
        if (end == std::string::npos ||
            !splitLine(text.substr(begin, end - begin), 10u, fields) ||
            !digestValid(fields[0]) || !identities.insert(fields[0]).second ||
            !parseUnsigned(fields[1], std::numeric_limits<uint64_t>::max(),
                           row.lastFailedUnix) ||
            !parseUnsigned(fields[2], std::numeric_limits<uint32_t>::max(),
                           attempts) || attempts == 0u ||
            !parseBoolean(fields[3], row.sourceAvailable) ||
            !parseBoolean(fields[4], row.sourceChanged) ||
            !parseBoolean(fields[5], row.reportAvailable) ||
            !parseRetryKind(fields[6], row.retryKind) ||
            !decodeHex(fields[7], 4096u, row.sourcePath) ||
            !decodeOptionalHex(fields[8], 4096u, row.outputPath) ||
            !decodeHex(fields[9], 8192u, row.error) ||
            (!row.sourceAvailable && row.sourceChanged) ||
            (row.retryKind == RetryKind::Convert && row.outputPath.empty()) ||
            (row.retryKind != RetryKind::Convert && !row.outputPath.empty())) {
            return false;
        }
        row.recordId = fields[0];
        row.attempts = static_cast<uint32_t>(attempts);
        parsed.rows.push_back(std::move(row));
        begin = end + 1u;
    }
    if (begin != text.size()) return false;
    output = std::move(parsed);
    return true;
}

}  // namespace CharacterFailureIndex
