#include "character_draft_snapshot.h"

#include <cmath>
#include <climits>
#include <cstring>
#include <set>
#include <utility>

namespace {

constexpr uint32_t kVersion = 2u;
constexpr uint32_t kLegacyVersion = 1u;
constexpr size_t kHeaderBytes = 40u;
constexpr size_t kTuningBytes = 9u * 4u + 4u +
    CharacterDraftSnapshot::kContexts * (19u * 4u);
constexpr size_t kRigBytes = 8u +
    CharacterDraftSnapshot::kRoles * (8u + 8u * 4u);
constexpr size_t kFixedBytes = kHeaderBytes + kTuningBytes + kRigBytes +
    CharacterDraftSnapshot::kPortraitBytes + 4u;
constexpr size_t kMaximumPathBytes = 4095u;
constexpr size_t kMaximumNameBytes = 96u;
constexpr size_t kMaximumShortNameBytes = 96u;

void appendU32(std::string &out, uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        out.push_back(static_cast<char>(value >> shift));
    }
}

void appendF32(std::string &out, float value) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "binary32 is required");
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32(out, bits);
}

bool readU32(const std::string &input, size_t &offset, uint32_t &value) {
    if (offset > input.size() || input.size() - offset < 4u) return false;
    value = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        value |= static_cast<uint32_t>(
            static_cast<unsigned char>(input[offset++])) << shift;
    }
    return true;
}

bool readF32(const std::string &input, size_t &offset, float &value) {
    uint32_t bits;
    if (!readU32(input, offset, bits)) return false;
    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

bool inRange(float value, float minimum, float maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool vectorInRange(const float *values, size_t count,
                   float minimum, float maximum) {
    for (size_t index = 0u; index < count; ++index) {
        if (!inRange(values[index], minimum, maximum)) return false;
    }
    return true;
}

bool pathUtf8(const std::string &text) {
    size_t index = 0u;
    if (text.size() > kMaximumPathBytes) return false;
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
        } else return false;
        if (continuation > text.size() - index) return false;
        while (continuation-- != 0u) {
            const unsigned char next =
                static_cast<unsigned char>(text[index++]);
            if ((next & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3Fu);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
            codepoint == 0u || codepoint < 0x20u ||
            (codepoint >= 0x7Fu && codepoint <= 0x9Fu) ||
            (codepoint >= 0x200Bu && codepoint <= 0x200Fu) ||
            (codepoint >= 0x2028u && codepoint <= 0x202Eu) ||
            (codepoint >= 0x2060u && codepoint <= 0x206Fu) ||
            codepoint == 0xFEFFu) {
            return false;
        }
    }
    return true;
}

bool identityText(const std::string &text, size_t maximum, bool allowEmpty) {
    if (text.size() > maximum || (!allowEmpty && text.empty())) return false;
    if (text.empty()) return allowEmpty;
    bool nonSpace = false;
    size_t index = 0u;
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
        } else return false;
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

bool snapshotValid(const CharacterDraftSnapshot::Snapshot &snapshot,
                   std::string &error, bool allowLegacyNames) {
    using namespace CharacterDraftSnapshot;
    if (snapshot.flags == 0u || (snapshot.flags & ~All) != 0u) {
        error = "draft snapshot flags are invalid";
        return false;
    }
    if (snapshot.donor >= 10u || snapshot.packageVehicleMask == 0u ||
        (snapshot.packageVehicleMask & ~7u) != 0u ||
        snapshot.enabledVehicleMask == 0u ||
        (snapshot.enabledVehicleMask & ~snapshot.packageVehicleMask) != 0u) {
        error = "draft vehicle or donor profile is invalid";
        return false;
    }
    if (snapshot.assemblyPlayers < 1 || snapshot.assemblyPlayers > 4 ||
        snapshot.testPlayers < 1 || snapshot.testPlayers > 4 ||
        (snapshot.reviewedContexts & ~0xFu) != 0u) {
        error = "draft test or review state is invalid";
        return false;
    }
    if (!inRange(snapshot.scale, 0.1f, 5.0f) ||
        !vectorInRange(snapshot.offset, 3u, -500.0f, 500.0f) ||
        !vectorInRange(snapshot.rotation, 3u, -180.0f, 180.0f) ||
        !inRange(snapshot.animationSpeed, 0.05f, 4.0f) ||
        !inRange(snapshot.lodBias, -3.0f, 3.0f)) {
        error = "draft global tuning is invalid";
        return false;
    }
    for (const Context &context : snapshot.contexts) {
        if (!inRange(context.scale, 0.1f, 5.0f) ||
            !vectorInRange(context.offset, 3u, -10.0f, 10.0f) ||
            !vectorInRange(context.rotation, 3u, -180.0f, 180.0f)) {
            error = "draft context placement is invalid";
            return false;
        }
        for (const auto &contact : context.contacts) {
            if (!vectorInRange(contact, 3u, -1.0f, 1.0f)) {
                error = "draft contact target is invalid";
                return false;
            }
        }
    }
    if (snapshot.rigMode > 1u ||
        (snapshot.rigMode == 0u && snapshot.rigReviewed)) {
        error = "draft rig mode or review state is invalid";
        return false;
    }
    std::set<uint32_t> nodes;
    for (const RigRole &role : snapshot.roles) {
        const float restLength = role.rest[0] * role.rest[0] +
            role.rest[1] * role.rest[1] + role.rest[2] * role.rest[2] +
            role.rest[3] * role.rest[3];
        const float bendLength = role.bend[0] * role.bend[0] +
            role.bend[1] * role.bend[1] + role.bend[2] * role.bend[2];
        if (role.node != kNoNode && !nodes.insert(role.node).second) {
            error = "draft rig maps one node to multiple roles";
            return false;
        }
        if (!inRange(role.confidence, 0.0f, 1.0f) ||
            !vectorInRange(role.rest, 4u, -1.0f, 1.0f) ||
            !vectorInRange(role.bend, 3u, -1.0f, 1.0f) ||
            !inRange(restLength, 0.999f, 1.001f) ||
            !(bendLength <= 1.0e-12f ||
              inRange(bendLength, 0.999f, 1.001f))) {
            error = "draft rig role basis is invalid";
            return false;
        }
    }
    if (!pathUtf8(snapshot.portraitSourcePath)) {
        error = "draft portrait source path is invalid UTF-8";
        return false;
    }
    const bool legacyNames = snapshot.displayName.empty() &&
        snapshot.shortName.empty() && snapshot.narrationName.empty() &&
        snapshot.sortLabel.empty();
    if (legacyNames && !allowLegacyNames) {
        error = "draft identity names are required";
        return false;
    }
    if (!legacyNames &&
        (!identityText(snapshot.displayName, kMaximumNameBytes, false) ||
         !identityText(snapshot.shortName, kMaximumShortNameBytes, false) ||
         !identityText(snapshot.narrationName, kMaximumNameBytes, false) ||
         !identityText(snapshot.sortLabel, kMaximumNameBytes, false))) {
        error = "draft identity names are invalid UTF-8";
        return false;
    }
    return true;
}

}  // namespace

namespace CharacterDraftSnapshot {

bool encode(const Snapshot &snapshot, std::string &payload,
            std::string &error) {
    if (!snapshotValid(snapshot, error, false)) return false;
    std::string result;
    const size_t namesBytes = snapshot.displayName.size() +
        snapshot.shortName.size() + snapshot.narrationName.size() +
        snapshot.sortLabel.size();
    result.reserve(kFixedBytes + 16u + snapshot.portraitSourcePath.size() +
                   namesBytes);
    result.append("MDWD", 4u);
    appendU32(result, kVersion);
    appendU32(result, static_cast<uint32_t>(
        kFixedBytes + 16u + snapshot.portraitSourcePath.size() + namesBytes));
    appendU32(result, snapshot.flags);
    appendU32(result, snapshot.donor);
    appendU32(result, snapshot.packageVehicleMask);
    result.push_back(static_cast<char>(snapshot.minimapRgb[0]));
    result.push_back(static_cast<char>(snapshot.minimapRgb[1]));
    result.push_back(static_cast<char>(snapshot.minimapRgb[2]));
    result.push_back('\0');
    appendU32(result, static_cast<uint32_t>(snapshot.assemblyPlayers));
    appendU32(result, static_cast<uint32_t>(snapshot.testPlayers));
    appendU32(result, snapshot.reviewedContexts);

    appendF32(result, snapshot.scale);
    for (float value : snapshot.offset) appendF32(result, value);
    for (float value : snapshot.rotation) appendF32(result, value);
    appendF32(result, snapshot.animationSpeed);
    appendF32(result, snapshot.lodBias);
    appendU32(result, snapshot.enabledVehicleMask);
    for (const Context &context : snapshot.contexts) {
        appendF32(result, context.scale);
        for (float value : context.offset) appendF32(result, value);
        for (float value : context.rotation) appendF32(result, value);
        for (const auto &contact : context.contacts) {
            for (float value : contact) appendF32(result, value);
        }
    }
    appendU32(result, snapshot.rigMode);
    appendU32(result, snapshot.rigReviewed ? 1u : 0u);
    for (const RigRole &role : snapshot.roles) {
        appendU32(result, role.node);
        appendU32(result, role.inferred ? 1u : 0u);
        appendF32(result, role.confidence);
        for (float value : role.rest) appendF32(result, value);
        for (float value : role.bend) appendF32(result, value);
    }
    result.append(reinterpret_cast<const char *>(snapshot.portrait.data()),
                  snapshot.portrait.size());
    appendU32(result, static_cast<uint32_t>(snapshot.portraitSourcePath.size()));
    result += snapshot.portraitSourcePath;
    const std::string *names[] = {
        &snapshot.displayName, &snapshot.shortName,
        &snapshot.narrationName, &snapshot.sortLabel,
    };
    for (const std::string *name : names) {
        appendU32(result, static_cast<uint32_t>(name->size()));
        result += *name;
    }
    if (result.size() != kFixedBytes + 16u +
            snapshot.portraitSourcePath.size() + namesBytes) {
        error = "draft snapshot encoder size invariant failed";
        return false;
    }
    payload = std::move(result);
    error.clear();
    return true;
}

bool decode(const std::string &payload, Snapshot &snapshot,
            std::string &error) {
    Snapshot parsed;
    size_t offset = 4u;
    uint32_t version;
    uint32_t declaredSize;
    uint32_t assemblyPlayers;
    uint32_t testPlayers;
    uint32_t rigReviewed;
    if (payload.size() < kFixedBytes || payload.compare(0u, 4u, "MDWD") != 0 ||
        !readU32(payload, offset, version) ||
        (version != kVersion && version != kLegacyVersion) ||
        !readU32(payload, offset, declaredSize) ||
        declaredSize != payload.size() ||
        !readU32(payload, offset, parsed.flags) ||
        !readU32(payload, offset, parsed.donor) ||
        !readU32(payload, offset, parsed.packageVehicleMask) ||
        offset > payload.size() || payload.size() - offset < 4u) {
        error = "draft snapshot header is invalid";
        return false;
    }
    parsed.minimapRgb[0] = static_cast<uint8_t>(payload[offset++]);
    parsed.minimapRgb[1] = static_cast<uint8_t>(payload[offset++]);
    parsed.minimapRgb[2] = static_cast<uint8_t>(payload[offset++]);
    if (payload[offset++] != '\0' ||
        !readU32(payload, offset, assemblyPlayers) ||
        !readU32(payload, offset, testPlayers) ||
        !readU32(payload, offset, parsed.reviewedContexts)) {
        error = "draft snapshot header fields are invalid";
        return false;
    }
    if (assemblyPlayers > INT_MAX || testPlayers > INT_MAX) goto malformed;
    parsed.assemblyPlayers = static_cast<int>(assemblyPlayers);
    parsed.testPlayers = static_cast<int>(testPlayers);
    if (!readF32(payload, offset, parsed.scale)) goto malformed;
    for (float &value : parsed.offset)
        if (!readF32(payload, offset, value)) goto malformed;
    for (float &value : parsed.rotation)
        if (!readF32(payload, offset, value)) goto malformed;
    if (!readF32(payload, offset, parsed.animationSpeed) ||
        !readF32(payload, offset, parsed.lodBias) ||
        !readU32(payload, offset, parsed.enabledVehicleMask)) goto malformed;
    for (Context &context : parsed.contexts) {
        if (!readF32(payload, offset, context.scale)) goto malformed;
        for (float &value : context.offset)
            if (!readF32(payload, offset, value)) goto malformed;
        for (float &value : context.rotation)
            if (!readF32(payload, offset, value)) goto malformed;
        for (auto &contact : context.contacts)
            for (float &value : contact)
                if (!readF32(payload, offset, value)) goto malformed;
    }
    if (!readU32(payload, offset, parsed.rigMode) ||
        !readU32(payload, offset, rigReviewed) || rigReviewed > 1u) {
        goto malformed;
    }
    parsed.rigReviewed = rigReviewed != 0u;
    for (RigRole &role : parsed.roles) {
        uint32_t inferred;
        if (!readU32(payload, offset, role.node) ||
            !readU32(payload, offset, inferred) || inferred > 1u ||
            !readF32(payload, offset, role.confidence)) goto malformed;
        role.inferred = inferred != 0u;
        for (float &value : role.rest)
            if (!readF32(payload, offset, value)) goto malformed;
        for (float &value : role.bend)
            if (!readF32(payload, offset, value)) goto malformed;
    }
    if (offset > payload.size() ||
        payload.size() - offset < parsed.portrait.size() + 4u) goto malformed;
    std::memcpy(parsed.portrait.data(), payload.data() + offset,
                parsed.portrait.size());
    offset += parsed.portrait.size();
    {
        uint32_t pathSize;
        if (!readU32(payload, offset, pathSize) ||
            pathSize > kMaximumPathBytes ||
            pathSize > payload.size() - offset) {
            goto malformed;
        }
        parsed.portraitSourcePath.assign(payload.data() + offset, pathSize);
        offset += pathSize;
    }
    if (version == kVersion) {
        std::string *names[] = {
            &parsed.displayName, &parsed.shortName,
            &parsed.narrationName, &parsed.sortLabel,
        };
        const size_t maxima[] = {
            kMaximumNameBytes, kMaximumShortNameBytes,
            kMaximumNameBytes, kMaximumNameBytes,
        };
        for (size_t name = 0u; name < 4u; ++name) {
            uint32_t nameSize;
            if (!readU32(payload, offset, nameSize) ||
                nameSize > maxima[name] || nameSize > payload.size() - offset) {
                goto malformed;
            }
            names[name]->assign(payload.data() + offset, nameSize);
            offset += nameSize;
        }
    }
    if (offset != payload.size() ||
        !snapshotValid(parsed, error, version == kLegacyVersion)) return false;
    snapshot = std::move(parsed);
    error.clear();
    return true;

malformed:
    error = "draft snapshot payload is truncated or malformed";
    return false;
}

}  // namespace CharacterDraftSnapshot
