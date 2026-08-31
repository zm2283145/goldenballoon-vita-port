#include "character_adapter_output_index.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace {

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

bool digestValid(const std::string &value) {
    return value.size() == 64u &&
           std::all_of(value.begin(), value.end(), [](char byte) {
               return (byte >= '0' && byte <= '9') ||
                      (byte >= 'a' && byte <= 'f');
           });
}

bool parseUnsigned(const std::string &text, uint64_t maximum,
                   uint64_t &output) {
    uint64_t value = 0u;
    if (text.empty() || (text.size() > 1u && text[0] == '0')) return false;
    for (char byte : text) {
        if (byte < '0' || byte > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(byte - '0');
        if (digit > maximum || value > (maximum - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    output = value;
    return true;
}

int nibble(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

bool printableUtf8(const std::string &text, size_t maximum,
                   bool requireText) {
    if (text.size() > maximum || (requireText && text.empty())) return false;
    size_t index = 0u;
    bool nonSpace = false;
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index++]);
        uint32_t codepoint = 0u;
        uint32_t minimum = 0u;
        unsigned continuation = 0u;
        if (first < 0x80u) {
            codepoint = first;
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
    return !requireText || nonSpace;
}

bool decode(const std::string &encoded, size_t maximum,
            bool required, std::string &output) {
    if (encoded == "-") {
        if (required) return false;
        output.clear();
        return true;
    }
    if ((encoded.size() & 1u) != 0u || encoded.size() > maximum * 2u) {
        return false;
    }
    std::string value(encoded.size() / 2u, '\0');
    for (size_t index = 0u; index < encoded.size(); index += 2u) {
        const int high = nibble(encoded[index]);
        const int low = nibble(encoded[index + 1u]);
        if (high < 0 || low < 0) return false;
        value[index / 2u] = static_cast<char>((high << 4u) | low);
    }
    if (!printableUtf8(value, maximum, required)) return false;
    output = std::move(value);
    return true;
}

}  // namespace

namespace CharacterAdapterOutputIndex {

bool parse(const std::string &text, Review &output) {
    constexpr const char *kHeader =
        "mdkr-character-adapter-output-index-v1\n";
    constexpr size_t kFields = 21u;
    if (text.size() > 32u * 1024u ||
        text.rfind(kHeader, 0u) != 0u || text.empty() ||
        text.back() != '\n') return false;
    const size_t rowBegin = std::char_traits<char>::length(kHeader);
    if (rowBegin >= text.size() ||
        text.find('\n', rowBegin) != text.size() - 1u) return false;
    std::vector<std::string> fields;
    if (!split(text.substr(rowBegin, text.size() - rowBegin - 1u),
               kFields, fields)) return false;
    Review parsed;
    uint64_t values[8] = {};
    if (!digestValid(fields[0]) || !digestValid(fields[1]) ||
        !digestValid(fields[2])) return false;
    const uint64_t maxima[8] = {
        512ull * 1024ull * 1024ull, 512ull * 1024ull * 1024ull,
        1000000u, 2000000u, 256u, 256u, 256u, 1u,
    };
    // fields 3..10 are bytes, geometry/material facts, and license presence.
    for (size_t index = 0u; index < 8u; ++index) {
        if (!parseUnsigned(fields[index + 3u], maxima[index], values[index])) {
            return false;
        }
    }
    if (values[0] == 0u || values[1] == 0u || values[2] == 0u ||
        values[3] == 0u || values[4] == 0u) return false;
    parsed.artifactSha256 = fields[0];
    parsed.modelSha256 = fields[1];
    parsed.sourceSha256 = fields[2];
    parsed.artifactBytes = values[0];
    parsed.modelBytes = values[1];
    parsed.vertices = static_cast<uint32_t>(values[2]);
    parsed.triangles = static_cast<uint32_t>(values[3]);
    parsed.joints = static_cast<uint32_t>(values[4]);
    parsed.materials = static_cast<uint32_t>(values[5]);
    parsed.textures = static_cast<uint32_t>(values[6]);
    parsed.licensePresent = values[7] != 0u;
    if ((parsed.licensePresent && !digestValid(fields[11])) ||
        (!parsed.licensePresent && fields[11] != "-")) return false;
    if (parsed.licensePresent) parsed.licenseSha256 = fields[11];
    if (!decode(fields[12], 128u, true, parsed.adapterName) ||
        !decode(fields[13], 64u, true, parsed.adapterVersion) ||
        !decode(fields[14], 2048u, false, parsed.adapterHomepage) ||
        !decode(fields[15], 64u, true, parsed.sourceFormat) ||
        !decode(fields[16], 128u, true, parsed.conversionProfile) ||
        !digestValid(fields[17]) ||
        !decode(fields[18], 128u, parsed.licensePresent,
                parsed.licenseSpdx) ||
        !decode(fields[19], 256u, parsed.licensePresent,
                parsed.attribution) ||
        !decode(fields[20], 2048u, parsed.licensePresent,
                parsed.sourceUrl)) return false;
    parsed.conversionSettingsSha256 = fields[17];
    if (!parsed.licensePresent &&
        (!parsed.licenseSpdx.empty() || !parsed.attribution.empty() ||
         !parsed.sourceUrl.empty())) return false;
    output = std::move(parsed);
    return true;
}

}  // namespace CharacterAdapterOutputIndex
