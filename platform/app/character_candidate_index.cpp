#include "character_candidate_index.h"

#include <climits>
#include <utility>
#include <vector>

namespace {

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

bool digestValid(const std::string &digest) {
    if (digest.size() != 64u) return false;
    for (char byte : digest) {
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f'))) return false;
    }
    return true;
}

bool idValid(const std::string &id) {
    if (id.size() < 2u || id.size() > 64u ||
        !((id[0] >= 'a' && id[0] <= 'z') ||
          (id[0] >= '0' && id[0] <= '9'))) return false;
    for (size_t index = 1u; index < id.size(); ++index) {
        const char byte = id[index];
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '.' ||
              byte == '_' || byte == '-')) return false;
    }
    return true;
}

int hexNibble(char byte) {
    if (byte >= '0' && byte <= '9') return byte - '0';
    if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
    return -1;
}

bool validUtf8Text(const std::string &text, size_t maximumBytes) {
    size_t index = 0u;
    if (text.empty() || text.size() > maximumBytes) return false;
    while (index < text.size()) {
        const unsigned char first = static_cast<unsigned char>(text[index++]);
        uint32_t codepoint;
        unsigned continuation;
        if (first < 0x80u) {
            codepoint = first;
            continuation = 0u;
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
        for (unsigned offset = 0u; offset < continuation; ++offset) {
            const unsigned char next =
                static_cast<unsigned char>(text[index++]);
            if ((next & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3Fu);
        }
        if ((continuation == 1u && codepoint < 0x80u) ||
            (continuation == 2u && codepoint < 0x800u) ||
            (continuation == 3u && codepoint < 0x10000u) ||
            codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
            codepoint < 0x20u || (codepoint >= 0x7Fu && codepoint <= 0x9Fu) ||
            (codepoint >= 0x200Bu && codepoint <= 0x200Fu) ||
            (codepoint >= 0x2028u && codepoint <= 0x202Eu) ||
            (codepoint >= 0x2060u && codepoint <= 0x206Fu) ||
            codepoint == 0xFEFFu) return false;
    }
    return true;
}

bool decodeText(const std::string &hex, size_t maximumBytes,
                std::string &text) {
    if (hex.empty() || (hex.size() & 1u) != 0u ||
        hex.size() > maximumBytes * 2u) {
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
    if (!validUtf8Text(decoded, maximumBytes)) return false;
    text = std::move(decoded);
    return true;
}

bool splitFields(const std::string &line, std::vector<std::string> &fields) {
    fields.clear();
    size_t begin = 0u;
    while (true) {
        const size_t tab = line.find('\t', begin);
        fields.push_back(line.substr(
            begin, tab == std::string::npos ? std::string::npos : tab - begin));
        if (tab == std::string::npos) break;
        begin = tab + 1u;
    }
    return fields.size() == 43u;
}

}  // namespace

namespace CharacterCandidateIndex {

bool parse(const std::string &text, Candidate &output) {
    static const std::string header = "mdkr-character-candidate-v3\n";
    Candidate parsed;
    std::vector<std::string> fields;
    uint64_t numbers[32] = {};
    if (text.compare(0u, header.size(), header) != 0 ||
        text.empty() || text.back() != '\n' ||
        text.find('\n', header.size()) != text.size() - 1u ||
        !splitFields(text.substr(
            header.size(), text.size() - header.size() - 1u), fields) ||
        !idValid(fields[0]) ||
        !decodeText(fields[1], 96u, parsed.displayName) ||
        !decodeText(fields[2], 96u, parsed.shortName) ||
        !decodeText(fields[3], 96u, parsed.narrationName) ||
        !decodeText(fields[4], 96u, parsed.sortLabel) ||
        !digestValid(fields[5]) || !digestValid(fields[6])) return false;
    for (size_t index = 0u; index < 32u; ++index) {
        const uint64_t maximum = index == 18u || index == 19u
            ? UINT64_MAX : UINT_MAX;
        if (!parseUnsigned(fields[index + 7u], maximum, numbers[index])) {
            return false;
        }
    }
    if (numbers[0] > 9u || numbers[1] == 0u || numbers[1] > 7u ||
        numbers[2] > 1000000u || numbers[3] > 2000000u ||
        numbers[5] == 0u || numbers[5] > 4u || numbers[6] > 256u ||
        numbers[10] > 256u || numbers[14] > 1u || numbers[15] > 2u ||
        numbers[16] > 1u || numbers[17] > 16u ||
        numbers[19] > 512u * 1024u * 1024u ||
        (numbers[15] == 0u && (numbers[16] != 0u || numbers[17] != 0u)) ||
        (numbers[15] == 1u && numbers[17] != 0u) ||
        (numbers[15] == 2u && numbers[17] != 16u) ||
        numbers[20] == 0u || numbers[24] == 0u || numbers[28] == 0u) {
        return false;
    }
    for (size_t lod = static_cast<size_t>(numbers[5]); lod < 4u; ++lod) {
        if (numbers[20u + lod] != 0u || numbers[24u + lod] != 0u ||
            numbers[28u + lod] != 0u) return false;
    }
    uint64_t provenancePresent = 0u;
    if (!parseUnsigned(fields[39], 1u, provenancePresent)) return false;
    if (provenancePresent != 0u) {
        if (!decodeText(fields[40], 128u, parsed.licenseSpdx) ||
            !decodeText(fields[41], 256u, parsed.attribution) ||
            !decodeText(fields[42], 2048u, parsed.sourceUrl)) return false;
        parsed.provenancePresent = true;
    } else if (!fields[40].empty() || !fields[41].empty() ||
               !fields[42].empty()) {
        return false;
    }
    parsed.id = fields[0];
    parsed.packageSha256 = fields[5];
    parsed.sourceDigest = fields[6];
    parsed.donor = static_cast<uint32_t>(numbers[0]);
    parsed.vehicleMask = static_cast<uint32_t>(numbers[1]);
    parsed.vertices = static_cast<uint32_t>(numbers[2]);
    parsed.triangles = static_cast<uint32_t>(numbers[3]);
    parsed.primitives = static_cast<uint32_t>(numbers[4]);
    parsed.lodLevels = static_cast<uint32_t>(numbers[5]);
    parsed.materials = static_cast<uint32_t>(numbers[6]);
    parsed.textures = static_cast<uint32_t>(numbers[7]);
    parsed.nodes = static_cast<uint32_t>(numbers[8]);
    parsed.skins = static_cast<uint32_t>(numbers[9]);
    parsed.joints = static_cast<uint32_t>(numbers[10]);
    parsed.animations = static_cast<uint32_t>(numbers[11]);
    parsed.animationChannels = static_cast<uint32_t>(numbers[12]);
    parsed.animationKeys = static_cast<uint32_t>(numbers[13]);
    parsed.identityPresent = numbers[14] != 0u;
    parsed.rigMode = static_cast<uint32_t>(numbers[15]);
    parsed.rigReviewed = numbers[16] != 0u;
    parsed.rigRoles = static_cast<uint32_t>(numbers[17]);
    parsed.encodedTextureBytes = numbers[18];
    parsed.decodedTextureBytes = numbers[19];
    for (size_t lod = 0u; lod < 4u; ++lod) {
        parsed.lodVertices[lod] =
            static_cast<uint32_t>(numbers[20u + lod]);
        parsed.lodTriangles[lod] =
            static_cast<uint32_t>(numbers[24u + lod]);
        parsed.lodPrimitives[lod] =
            static_cast<uint32_t>(numbers[28u + lod]);
    }
    output = std::move(parsed);
    return true;
}

}  // namespace CharacterCandidateIndex
