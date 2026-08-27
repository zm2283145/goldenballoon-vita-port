#include "character_raw_intake_index.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
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

bool printableUtf8(const std::string &text) {
    size_t index = 0u;
    if (text.empty() || text.size() > 256u) return false;
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
            codepoint == 0xFEFFu) {
            return false;
        }
    }
    return true;
}

bool decode(const std::string &hex, std::string &output, bool optional) {
    if (optional && hex == "-") {
        output.clear();
        return true;
    }
    if (hex.empty() || (hex.size() & 1u) != 0u || hex.size() > 512u) {
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
    if (!printableUtf8(decoded)) return false;
    output = std::move(decoded);
    return true;
}

bool parseHeight(const std::string &text, double &output) {
    if (text.empty() || text.size() > 32u ||
        text[0] < '0' || text[0] > '9' ||
        !std::all_of(text.begin(), text.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') || byte == '.' ||
                   byte == 'e' || byte == 'E' || byte == '+' || byte == '-';
        })) return false;
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end != text.c_str() + text.size() ||
        !std::isfinite(parsed) || parsed <= 0.0 || parsed > 1000000.0) {
        return false;
    }
    output = parsed;
    return true;
}

bool parseBound(const std::string &text, double &output) {
    if (text.empty() || text.size() > 32u ||
        !std::all_of(text.begin(), text.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') || byte == '.' ||
                   byte == 'e' || byte == 'E' || byte == '+' || byte == '-';
        })) return false;
    char *end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end != text.c_str() + text.size() ||
        !std::isfinite(parsed) || std::fabs(parsed) > 1000000000.0) {
        return false;
    }
    output = parsed;
    return true;
}

}  // namespace

namespace CharacterRawIntakeIndex {

bool parse(const std::string &text, Inventory &output) {
    Inventory parsed;
    std::vector<std::string> fields;
    size_t begin = 0u;
    size_t end = text.find('\n');
    uint64_t values[8] = {};
    if (text.size() > 1024u * 1024u || end == std::string::npos) return false;
    const std::string header = text.substr(0u, end);
    const bool detailed =
        header.rfind("mdkr-character-glb-intake-v2\t", 0u) == 0u;
    if (!splitLine(header, detailed ? 23u : 11u, fields) ||
        (!detailed && fields[0] != "mdkr-character-glb-intake-v1") ||
        !digestValid(fields[1])) return false;
    for (size_t index = 0u; index < 6u; ++index) {
        const uint64_t maximum = index < 2u ? 2000000u : 4096u;
        if (!parseUnsigned(fields[index + 2u], maximum, values[index])) {
            return false;
        }
    }
    if (values[0] == 0u || values[1] == 0u || values[4] == 0u ||
        values[5] == 0u || !parseHeight(fields[8], parsed.sourceHeightM) ||
        !parseUnsigned(fields[9], kMaximumClips, values[6]) ||
        !parseUnsigned(fields[10], kMaximumNodes, values[7]) ||
        values[6] == 0u || values[7] == 0u) return false;
    parsed.modelSha256 = fields[1];
    parsed.vertices = static_cast<uint32_t>(values[0]);
    parsed.triangles = static_cast<uint32_t>(values[1]);
    parsed.materials = static_cast<uint32_t>(values[2]);
    parsed.textures = static_cast<uint32_t>(values[3]);
    parsed.skins = static_cast<uint32_t>(values[4]);
    parsed.joints = static_cast<uint32_t>(values[5]);
    if (detailed) {
        double *destinations[] = {
            parsed.meshLocalMinimum.data(), parsed.meshLocalMaximum.data(),
            parsed.sceneWorldMinimum.data(), parsed.sceneWorldMaximum.data(),
        };
        size_t field = 11u;
        for (double *destination : destinations) {
            for (size_t axis = 0u; axis < 3u; ++axis) {
                if (!parseBound(fields[field++], destination[axis])) {
                    return false;
                }
            }
        }
        for (size_t axis = 0u; axis < 3u; ++axis) {
            if (parsed.meshLocalMinimum[axis] > parsed.meshLocalMaximum[axis] ||
                parsed.sceneWorldMinimum[axis] > parsed.sceneWorldMaximum[axis]) {
                return false;
            }
        }
        const double worldHeight =
            parsed.sceneWorldMaximum[1] - parsed.sceneWorldMinimum[1];
        double localSpanSquared = 0.0;
        for (size_t axis = 0u; axis < 3u; ++axis) {
            const double extent = parsed.meshLocalMaximum[axis] -
                                  parsed.meshLocalMinimum[axis];
            localSpanSquared += extent * extent;
        }
        if (worldHeight <= 0.0 || localSpanSquared <= 0.0 ||
            std::fabs(worldHeight - parsed.sourceHeightM) >
                std::max(1.0e-8, worldHeight * 1.0e-6)) {
            return false;
        }
        parsed.detailedBounds = true;
    }

    begin = end + 1u;
    end = text.find('\n', begin);
    if (end == std::string::npos ||
        !splitLine(text.substr(begin, end - begin), 4u, fields) ||
        fields[0] != "defaults" ||
        !decode(fields[1], parsed.fallback, false) ||
        !decode(fields[2], parsed.seat, true) ||
        !decode(fields[3], parsed.head, true)) return false;
    begin = end + 1u;

    std::set<std::string> clips;
    for (uint64_t index = 0u; index < values[6]; ++index) {
        end = text.find('\n', begin);
        std::string name;
        if (end == std::string::npos ||
            !splitLine(text.substr(begin, end - begin), 2u, fields) ||
            fields[0] != "clip" || !decode(fields[1], name, false) ||
            !clips.insert(name).second) return false;
        parsed.clips.push_back(std::move(name));
        begin = end + 1u;
    }
    std::set<std::string> nodes;
    for (uint64_t index = 0u; index < values[7]; ++index) {
        end = text.find('\n', begin);
        std::string name;
        if (end == std::string::npos ||
            !splitLine(text.substr(begin, end - begin), 2u, fields) ||
            fields[0] != "node" || !decode(fields[1], name, false) ||
            !nodes.insert(name).second) return false;
        parsed.nodes.push_back(std::move(name));
        begin = end + 1u;
    }
    if (begin != text.size() || clips.count(parsed.fallback) == 0u ||
        (!parsed.seat.empty() && nodes.count(parsed.seat) == 0u) ||
        (!parsed.head.empty() && nodes.count(parsed.head) == 0u)) return false;
    output = std::move(parsed);
    return true;
}

}  // namespace CharacterRawIntakeIndex
