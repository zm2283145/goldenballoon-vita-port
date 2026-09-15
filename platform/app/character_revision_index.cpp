#include "character_revision_index.h"

#include <algorithm>
#include <climits>
#include <utility>

namespace {

bool parseUnsigned(const std::string &text, uint64_t maximum,
                   uint64_t &value) {
    uint64_t parsed = 0u;
    if (text.empty()) return false;
    for (char byte : text) {
        if (byte < '0' || byte > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(byte - '0');
        if (digit > maximum || parsed > (maximum - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

bool splitLine(const std::string &line, size_t expectedFields,
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
    return fields.size() == expectedFields;
}

bool digestValid(const std::string &digest) {
    if (digest.size() != 64u) return false;
    return std::all_of(digest.begin(), digest.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') ||
               (byte >= 'a' && byte <= 'f');
    });
}

}  // namespace

namespace CharacterRevisionIndex {

bool parse(const std::string &text, Inventory &output) {
    Inventory parsed;
    std::vector<std::string> fields;
    size_t begin = 0u;
    size_t end = text.find('\n');
    uint64_t total = 0u;
    uint64_t published = 0u;
    if (end == std::string::npos ||
        !splitLine(text.substr(0u, end), 3u, fields) ||
        fields[0] != "mdkr-character-revisions-v1" ||
        !parseUnsigned(fields[1], UINT_MAX, total) ||
        !parseUnsigned(fields[2], kMaximumRows, published) ||
        published == 0u || total < published) return false;
    begin = end + 1u;
    for (uint64_t index = 0u; index < published; ++index) {
        end = text.find('\n', begin);
        if (end == std::string::npos ||
            !splitLine(text.substr(begin, end - begin), 4u, fields) ||
            !digestValid(fields[0]) ||
            (fields[1] != "0" && fields[1] != "1") ||
            (fields[2] != "0" && fields[2] != "1")) return false;
        Row row;
        row.sourceSha256 = fields[0];
        row.current = fields[1] == "1";
        row.enabled = fields[2] == "1";
        if (!parseUnsigned(fields[3], UINT64_MAX, row.installedUnix)) {
            return false;
        }
        parsed.rows.push_back(std::move(row));
        begin = end + 1u;
    }
    if (begin != text.size() || parsed.rows.empty() ||
        std::none_of(parsed.rows.begin(), parsed.rows.end(),
                     [](const Row &row) { return row.current; })) {
        return false;
    }
    parsed.total = static_cast<unsigned>(total);
    output = std::move(parsed);
    return true;
}

}  // namespace CharacterRevisionIndex
