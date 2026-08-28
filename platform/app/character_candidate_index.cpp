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

bool splitFields(const std::string &line, size_t expected,
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

}  // namespace

namespace CharacterCandidateIndex {

bool parse(const std::string &text, Candidate &output) {
    static const std::string textureHeader =
        "mdkr-character-candidate-v7\n";
    static const std::string tangentHeader =
        "mdkr-character-candidate-v6\n";
    static const std::string authoredMotionHeader =
        "mdkr-character-candidate-v5\n";
    static const std::string currentHeader =
        "mdkr-character-candidate-v4\n";
    static const std::string legacyHeader =
        "mdkr-character-candidate-v3\n";
    Candidate parsed;
    std::vector<std::string> fields;
    uint64_t numbers[49] = {};
    const bool textureDiagnostics =
        text.compare(0u, textureHeader.size(), textureHeader) == 0;
    const bool tangentDiagnostics = textureDiagnostics ||
        text.compare(0u, tangentHeader.size(), tangentHeader) == 0;
    const bool authoredMotion =
        text.compare(0u, authoredMotionHeader.size(), authoredMotionHeader) == 0;
    const bool current =
        text.compare(0u, currentHeader.size(), currentHeader) == 0;
    const std::string &header = textureDiagnostics ? textureHeader
        : tangentDiagnostics ? tangentHeader
        : authoredMotion ? authoredMotionHeader
        : current ? currentHeader : legacyHeader;
    const bool hasAuthoredMotion = tangentDiagnostics || authoredMotion;
    const bool hasSemanticIntent = hasAuthoredMotion || current;
    const size_t numberCount = textureDiagnostics ? 49u
        : tangentDiagnostics ? 43u
        : authoredMotion ? 37u : current ? 34u : 32u;
    const size_t provenanceField = textureDiagnostics ? 56u
        : tangentDiagnostics ? 50u
        : authoredMotion ? 44u : current ? 41u : 39u;
    const size_t encodedIndex = hasAuthoredMotion ? 21u : 18u;
    const size_t decodedIndex = hasAuthoredMotion ? 22u : 19u;
    const size_t lodVertexIndex = hasAuthoredMotion ? 23u : 20u;
    const size_t lodTriangleIndex = hasAuthoredMotion ? 27u : 24u;
    const size_t lodPrimitiveIndex = hasAuthoredMotion ? 31u : 28u;
    const size_t semanticIndex = hasAuthoredMotion ? 35u : 32u;
    if ((!tangentDiagnostics && !authoredMotion && !current &&
         text.compare(0u, legacyHeader.size(), legacyHeader) != 0) ||
        text.empty() || text.back() != '\n' ||
        text.find('\n', header.size()) != text.size() - 1u ||
        !splitFields(
            text.substr(header.size(), text.size() - header.size() - 1u),
            textureDiagnostics ? 60u : tangentDiagnostics ? 54u
                : authoredMotion ? 48u : current ? 45u : 43u,
            fields) ||
        !idValid(fields[0]) ||
        !decodeText(fields[1], 96u, parsed.displayName) ||
        !decodeText(fields[2], 96u, parsed.shortName) ||
        !decodeText(fields[3], 96u, parsed.narrationName) ||
        !decodeText(fields[4], 96u, parsed.sortLabel) ||
        !digestValid(fields[5]) || !digestValid(fields[6])) return false;
    for (size_t index = 0u; index < numberCount; ++index) {
        const uint64_t maximum = index == encodedIndex ||
            index == decodedIndex || (textureDiagnostics && index == 44u)
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
        (hasAuthoredMotion &&
         (numbers[18] > 16u || numbers[19] > 8u ||
          numbers[20] > 64u || numbers[18] > numbers[17] ||
          (numbers[15] == 0u && numbers[18] != 0u) ||
          ((numbers[19] == 0u) != (numbers[20] == 0u)) ||
          numbers[19] > numbers[20])) ||
        (tangentDiagnostics &&
         (numbers[37] > 512u || numbers[38] > 512u ||
          numbers[39] > 1000000u || numbers[40] > 2000000u ||
          numbers[41] > 1000000u || numbers[42] > numbers[41])) ||
        (textureDiagnostics &&
         (numbers[43] > numbers[7] ||
          numbers[44] > numbers[encodedIndex] ||
          numbers[45] + numbers[46] != numbers[43] ||
          ((numbers[43] == 0u) !=
           (numbers[44] == 0u && numbers[45] == 0u &&
            numbers[46] == 0u && numbers[47] == 0u &&
            numbers[48] == 0u)) ||
          (numbers[43] != 0u &&
           (numbers[44] == 0u || numbers[47] == 0u ||
            numbers[47] > numbers[48] ||
            numbers[48] > 13u)))) ||
        numbers[decodedIndex] > 512u * 1024u * 1024u ||
        (numbers[15] == 0u && (numbers[16] != 0u || numbers[17] != 0u)) ||
        (numbers[15] == 1u && numbers[17] != 0u) ||
        (numbers[15] == 2u && numbers[17] != 16u) ||
        numbers[lodVertexIndex] == 0u ||
        numbers[lodTriangleIndex] == 0u ||
        numbers[lodPrimitiveIndex] == 0u) {
        return false;
    }
    constexpr uint64_t semanticMask = 0x3FFFu;
    if (hasSemanticIntent &&
        ((numbers[semanticIndex] & ~semanticMask) != 0u ||
         (numbers[semanticIndex + 1u] & ~(semanticMask & ~1u)) != 0u ||
         (numbers[semanticIndex] & numbers[semanticIndex + 1u]) != 0u ||
         (numbers[semanticIndex] & 1u) == 0u)) {
        return false;
    }
    for (size_t lod = static_cast<size_t>(numbers[5]); lod < 4u; ++lod) {
        if (numbers[lodVertexIndex + lod] != 0u ||
            numbers[lodTriangleIndex + lod] != 0u ||
            numbers[lodPrimitiveIndex + lod] != 0u) return false;
    }
    uint64_t provenancePresent = 0u;
    if (!parseUnsigned(fields[provenanceField], 1u, provenancePresent)) {
        return false;
    }
    if (provenancePresent != 0u) {
        if (!decodeText(fields[provenanceField + 1u], 128u,
                        parsed.licenseSpdx) ||
            !decodeText(fields[provenanceField + 2u], 256u,
                        parsed.attribution) ||
            !decodeText(fields[provenanceField + 3u], 2048u,
                        parsed.sourceUrl)) return false;
        parsed.provenancePresent = true;
    } else if (!fields[provenanceField + 1u].empty() ||
               !fields[provenanceField + 2u].empty() ||
               !fields[provenanceField + 3u].empty()) {
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
    if (hasSemanticIntent) {
        parsed.semanticMask = static_cast<uint32_t>(numbers[semanticIndex]);
        parsed.disabledSemanticMask =
            static_cast<uint32_t>(numbers[semanticIndex + 1u]);
        parsed.semanticIntentPresent = true;
    }
    parsed.identityPresent = numbers[14] != 0u;
    parsed.rigMode = static_cast<uint32_t>(numbers[15]);
    parsed.rigReviewed = numbers[16] != 0u;
    parsed.rigRoles = static_cast<uint32_t>(numbers[17]);
    if (hasAuthoredMotion) {
        parsed.jointConstraints = static_cast<uint32_t>(numbers[18]);
        parsed.secondaryChains = static_cast<uint32_t>(numbers[19]);
        parsed.secondaryJoints = static_cast<uint32_t>(numbers[20]);
    }
    if (tangentDiagnostics) {
        parsed.authoredTangentPrimitives =
            static_cast<uint32_t>(numbers[37]);
        parsed.generatedTangentPrimitives =
            static_cast<uint32_t>(numbers[38]);
        parsed.authoredTangentRepairedVertices =
            static_cast<uint32_t>(numbers[39]);
        parsed.generatedTangentDegenerateUvTriangles =
            static_cast<uint32_t>(numbers[40]);
        parsed.tangentFallbackVertices =
            static_cast<uint32_t>(numbers[41]);
        parsed.normalMapTangentFallbackVertices =
            static_cast<uint32_t>(numbers[42]);
        parsed.tangentDiagnosticsPresent = true;
    }
    if (textureDiagnostics) {
        parsed.ktx2Textures = static_cast<uint32_t>(numbers[43]);
        parsed.ktx2SourceBytes = numbers[44];
        parsed.ktx2Etc1sTextures = static_cast<uint32_t>(numbers[45]);
        parsed.ktx2UastcTextures = static_cast<uint32_t>(numbers[46]);
        parsed.ktx2MipLevelsMin = static_cast<uint32_t>(numbers[47]);
        parsed.ktx2MipLevelsMax = static_cast<uint32_t>(numbers[48]);
        parsed.textureFormatDiagnosticsPresent = true;
    }
    parsed.encodedTextureBytes = numbers[encodedIndex];
    parsed.decodedTextureBytes = numbers[decodedIndex];
    for (size_t lod = 0u; lod < 4u; ++lod) {
        parsed.lodVertices[lod] =
            static_cast<uint32_t>(numbers[lodVertexIndex + lod]);
        parsed.lodTriangles[lod] =
            static_cast<uint32_t>(numbers[lodTriangleIndex + lod]);
        parsed.lodPrimitives[lod] =
            static_cast<uint32_t>(numbers[lodPrimitiveIndex + lod]);
    }
    output = std::move(parsed);
    return true;
}

}  // namespace CharacterCandidateIndex
