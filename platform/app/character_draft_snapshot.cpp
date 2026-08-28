#include "character_draft_snapshot.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <set>
#include <utility>

namespace {

constexpr uint32_t kFitSceneReviewVersion = 13u;
constexpr uint32_t kFitSemanticReviewVersion = 15u;
constexpr uint32_t kFitMultiSceneReviewVersion = 16u;
constexpr uint32_t kFitContactStabilityVersion = 17u;
constexpr uint32_t kFitExpandedSceneVersion = 18u;
constexpr uint32_t kFitMotionReviewVersion = 14u;
constexpr uint32_t kTransitionInspectionVersion = 12u;
constexpr uint32_t kContactExceptionsVersion = 11u;
constexpr uint32_t kRigReviewTasksVersion = 10u;
constexpr uint32_t kAnimationIntentVersion = 9u;
constexpr uint32_t kTopInspectionVersion = 8u;
constexpr uint32_t kPortraitSubjectMaskVersion = 7u;
constexpr uint32_t kVersion = kFitExpandedSceneVersion;
constexpr uint32_t kPortraitSourceVersion = 6u;
constexpr uint32_t kVisualInspectionVersion = 5u;
constexpr uint32_t kPoseInspectionVersion = 4u;
constexpr uint32_t kPortraitStyleVersion = 3u;
constexpr uint32_t kNamesVersion = 2u;
constexpr uint32_t kLegacyVersion = 1u;
constexpr size_t kHeaderBytes = 40u;
constexpr size_t kTuningBytes = 9u * 4u + 4u +
    CharacterDraftSnapshot::kContexts * (19u * 4u);
constexpr size_t kRigBytes = 8u +
    CharacterDraftSnapshot::kRoles * (8u + 8u * 4u);
constexpr size_t kLegacyFixedBytes = kHeaderBytes + kTuningBytes + kRigBytes +
    CharacterDraftSnapshot::kPortraitBytes + 4u;
constexpr size_t kPortraitRecipeBytes = 9u * 4u;
constexpr size_t kPortraitStyleFixedBytes = kLegacyFixedBytes +
    CharacterPortraitStudio::kBytes + kPortraitRecipeBytes;
constexpr size_t kPoseInspectionFixedBytes = kPortraitStyleFixedBytes + 8u;
constexpr size_t kVisualInspectionFixedBytes =
    kPoseInspectionFixedBytes + 12u;
constexpr size_t kPortraitSourceRecordBytes = 9u * 4u + 64u;
constexpr size_t kPortraitSubjectMaskBytes =
    4u + CharacterPortraitImport::kSubjectMaskPixels;
constexpr size_t kTopInspectionFixedBytes =
    kVisualInspectionFixedBytes + kPortraitSourceRecordBytes +
    kPortraitSubjectMaskBytes;
constexpr size_t kFixedBytes = kTopInspectionFixedBytes + 28u;
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

void appendI32(std::string &out, int value) {
    static_assert(sizeof(int32_t) == sizeof(uint32_t),
                  "signed editor integers must be 32-bit");
    const int32_t signedValue = static_cast<int32_t>(value);
    uint32_t bits;
    std::memcpy(&bits, &signedValue, sizeof(bits));
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

bool readI32(const std::string &input, size_t &offset, int &value) {
    uint32_t bits;
    int32_t signedValue;
    if (!readU32(input, offset, bits)) return false;
    std::memcpy(&signedValue, &bits, sizeof(signedValue));
    value = static_cast<int>(signedValue);
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
        snapshot.testPose == 0u ||
        snapshot.testPose >
            MDKR_MODERN_CHARACTER_INSPECTION_SEMANTIC_COUNT ||
        snapshot.testPosePhaseMilli > 1000u ||
        snapshot.testTransitionFromPose == 0u ||
        snapshot.testTransitionFromPose >
            MDKR_MODERN_CHARACTER_INSPECTION_SEMANTIC_COUNT ||
        snapshot.testTransitionFromPhaseMilli > 1000u ||
        (snapshot.testTransition &&
         snapshot.testTransitionFromPose == snapshot.testPose) ||
        snapshot.testViewYawDegrees < -180 ||
        snapshot.testViewYawDegrees > 180 ||
        snapshot.testViewPitchDegrees <
            MDKR_WORKSHOP_PREVIEW_PITCH_MIN_DEGREES ||
        snapshot.testViewPitchDegrees >
            MDKR_WORKSHOP_PREVIEW_PITCH_MAX_DEGREES ||
        snapshot.testLighting >= MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT ||
        (snapshot.reviewedContexts & ~0xFu) != 0u ||
        (snapshot.contactExceptionContexts & ~0xEu) != 0u ||
        (snapshot.contactExceptionContexts & ~snapshot.reviewedContexts) != 0u) {
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
        (snapshot.rigMode == 0u && snapshot.rigReviewed) ||
        (snapshot.rigReviewTaskMask & ~0x1Fu) != 0u ||
        (snapshot.rigReviewed && snapshot.rigReviewTaskMask != 0x1Fu) ||
        (snapshot.disabledSemanticMask & ~kAnimationSemanticMask) != 0u) {
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
    if (!CharacterPortraitStudio::validRecipe(snapshot.portraitRecipe)) {
        error = "draft portrait style recipe is invalid";
        return false;
    }
    if (!CharacterPortraitImport::validSourceRecord(
            snapshot.portraitSourceRecord)) {
        error = "draft portrait source record is invalid";
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
    result.append(
        reinterpret_cast<const char *>(snapshot.portraitStyleSource.data()),
        snapshot.portraitStyleSource.size());
    appendI32(result, snapshot.portraitRecipe.zoomPercent);
    appendI32(result, snapshot.portraitRecipe.panX);
    appendI32(result, snapshot.portraitRecipe.panY);
    appendU32(result, snapshot.portraitRecipe.paletteColors);
    appendF32(result, snapshot.portraitRecipe.ditherStrength);
    appendI32(result, snapshot.portraitRecipe.outlinePixels);
    appendI32(result, snapshot.portraitRecipe.alphaThreshold);
    appendU32(result, static_cast<uint32_t>(
        snapshot.portraitRecipe.sampling));
    appendU32(result, snapshot.portraitRecipe.fillPinholes ? 1u : 0u);
    appendU32(result, snapshot.testPose);
    appendU32(result, snapshot.testPosePhaseMilli);
    appendU32(result,
              static_cast<uint32_t>(snapshot.testViewYawDegrees + 180));
    appendU32(result,
              static_cast<uint32_t>(snapshot.testViewPitchDegrees + 90));
    appendU32(result, snapshot.testLighting);
    appendU32(result, static_cast<uint32_t>(
        snapshot.portraitSourceRecord.kind));
    appendU32(result, snapshot.portraitSourceRecord.width);
    appendU32(result, snapshot.portraitSourceRecord.height);
    appendU32(result, snapshot.portraitSourceRecord.recipe.cropX);
    appendU32(result, snapshot.portraitSourceRecord.recipe.cropY);
    appendU32(result, snapshot.portraitSourceRecord.recipe.cropSize);
    appendU32(result,
              snapshot.portraitSourceRecord.recipe.edgeMatteTolerance);
    appendU32(result, static_cast<uint32_t>(
        snapshot.portraitSourceRecord.recipe.sampling));
    appendU32(result, static_cast<uint32_t>(
        snapshot.portraitSourceRecord.recipe.background));
    if (snapshot.portraitSourceRecord.sha256.empty()) {
        result.append(64u, '\0');
    } else {
        result += snapshot.portraitSourceRecord.sha256;
    }
    appendU32(result,
              snapshot.portraitSourceRecord.subjectMask.enabled ? 1u : 0u);
    result.append(
        reinterpret_cast<const char *>(
            snapshot.portraitSourceRecord.subjectMask.alpha.data()),
        snapshot.portraitSourceRecord.subjectMask.alpha.size());
    appendU32(result, snapshot.disabledSemanticMask);
    appendU32(result, snapshot.rigReviewTaskMask);
    appendU32(result, snapshot.contactExceptionContexts);
    appendU32(result, snapshot.testTransition ? 1u : 0u);
    appendU32(result, snapshot.testTransitionFromPose);
    appendU32(result, snapshot.testTransitionFromPhaseMilli);
    appendU32(result, 1u);
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
    if (payload.size() < kLegacyFixedBytes ||
        payload.compare(0u, 4u, "MDWD") != 0 ||
        !readU32(payload, offset, version) ||
        (version != kVersion &&
         version != kFitContactStabilityVersion &&
         version != kFitMultiSceneReviewVersion &&
         version != kFitSemanticReviewVersion &&
         version != kFitMotionReviewVersion &&
         version != kFitSceneReviewVersion &&
         version != kTransitionInspectionVersion &&
         version != kContactExceptionsVersion &&
         version != kRigReviewTasksVersion &&
         version != kAnimationIntentVersion &&
         version != kTopInspectionVersion &&
         version != kPortraitSubjectMaskVersion &&
         version != kPortraitSourceVersion &&
         version != kVisualInspectionVersion &&
         version != kPoseInspectionVersion &&
         version != kPortraitStyleVersion &&
         version != kNamesVersion &&
         version != kLegacyVersion) ||
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
    if (version >= kNamesVersion) {
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
    if (version >= kPortraitStyleVersion) {
        uint32_t sampling;
        uint32_t fillPinholes;
        if (offset > payload.size() ||
            parsed.portraitStyleSource.size() > payload.size() - offset) {
            goto malformed;
        }
        std::memcpy(parsed.portraitStyleSource.data(),
                    payload.data() + offset,
                    parsed.portraitStyleSource.size());
        offset += parsed.portraitStyleSource.size();
        if (!readI32(payload, offset, parsed.portraitRecipe.zoomPercent) ||
            !readI32(payload, offset, parsed.portraitRecipe.panX) ||
            !readI32(payload, offset, parsed.portraitRecipe.panY) ||
            !readU32(payload, offset,
                     parsed.portraitRecipe.paletteColors) ||
            !readF32(payload, offset,
                     parsed.portraitRecipe.ditherStrength) ||
            !readI32(payload, offset,
                     parsed.portraitRecipe.outlinePixels) ||
            !readI32(payload, offset,
                     parsed.portraitRecipe.alphaThreshold) ||
            !readU32(payload, offset, sampling) || sampling > 1u ||
            !readU32(payload, offset, fillPinholes) || fillPinholes > 1u) {
            goto malformed;
        }
        parsed.portraitRecipe.sampling =
            static_cast<CharacterPortraitStudio::Sampling>(sampling);
        parsed.portraitRecipe.fillPinholes = fillPinholes != 0u;
    } else {
        parsed.portraitStyleSource = parsed.portrait;
    }
    if (version >= kPoseInspectionVersion &&
        (!readU32(payload, offset, parsed.testPose) ||
         !readU32(payload, offset, parsed.testPosePhaseMilli))) {
        goto malformed;
    }
    if (version >= kVisualInspectionVersion) {
        uint32_t yaw;
        uint32_t pitch;
        if (!readU32(payload, offset, yaw) ||
            !readU32(payload, offset, pitch) ||
            !readU32(payload, offset, parsed.testLighting)) {
            goto malformed;
        }
        if (yaw > 360u ||
            pitch > (version >= kTopInspectionVersion ? 180u : 90u)) {
            goto malformed;
        }
        parsed.testViewYawDegrees = static_cast<int32_t>(yaw) - 180;
        parsed.testViewPitchDegrees = static_cast<int32_t>(pitch) -
            (version >= kTopInspectionVersion ? 90 : 45);
    }
    if (version >= kPortraitSourceVersion) {
        uint32_t kind;
        uint32_t sampling;
        uint32_t background;
        if (!readU32(payload, offset, kind) || kind > 2u ||
            !readU32(payload, offset,
                     parsed.portraitSourceRecord.width) ||
            !readU32(payload, offset,
                     parsed.portraitSourceRecord.height) ||
            !readU32(payload, offset,
                     parsed.portraitSourceRecord.recipe.cropX) ||
            !readU32(payload, offset,
                     parsed.portraitSourceRecord.recipe.cropY) ||
            !readU32(payload, offset,
                     parsed.portraitSourceRecord.recipe.cropSize) ||
            !readU32(payload, offset,
                     parsed.portraitSourceRecord.recipe.edgeMatteTolerance) ||
            !readU32(payload, offset, sampling) || sampling > 1u ||
            !readU32(payload, offset, background) || background > 3u ||
            offset > payload.size() || payload.size() - offset < 64u) {
            goto malformed;
        }
        parsed.portraitSourceRecord.kind =
            static_cast<CharacterPortraitImport::SourceKind>(kind);
        parsed.portraitSourceRecord.recipe.sampling =
            static_cast<CharacterPortraitImport::Sampling>(sampling);
        parsed.portraitSourceRecord.recipe.background =
            static_cast<CharacterPortraitImport::Background>(background);
        const bool emptyDigest = std::all_of(
            payload.begin() + static_cast<std::ptrdiff_t>(offset),
            payload.begin() + static_cast<std::ptrdiff_t>(offset + 64u),
            [](char byte) { return byte == '\0'; });
        if (!emptyDigest) {
            parsed.portraitSourceRecord.sha256.assign(
                payload.data() + offset, 64u);
        }
        offset += 64u;
    }
    if (version >= kPortraitSubjectMaskVersion) {
        uint32_t enabled;
        auto &mask = parsed.portraitSourceRecord.subjectMask;
        if (!readU32(payload, offset, enabled) || enabled > 1u ||
            offset > payload.size() ||
            mask.alpha.size() > payload.size() - offset) {
            goto malformed;
        }
        mask.enabled = enabled != 0u;
        std::memcpy(mask.alpha.data(), payload.data() + offset,
                    mask.alpha.size());
        offset += mask.alpha.size();
    }
    if (version >= kAnimationIntentVersion &&
        !readU32(payload, offset, parsed.disabledSemanticMask)) {
        goto malformed;
    }
    parsed.animationIntentPresent = version >= kAnimationIntentVersion;
    if (version >= kRigReviewTasksVersion) {
        if (!readU32(payload, offset, parsed.rigReviewTaskMask)) {
            goto malformed;
        }
    } else {
        parsed.rigReviewTaskMask = parsed.rigReviewed ? 0x1Fu : 0u;
    }
    if (version >= kContactExceptionsVersion) {
        if (!readU32(payload, offset,
                     parsed.contactExceptionContexts)) goto malformed;
    }
    if (version >= kTransitionInspectionVersion) {
        uint32_t enabled;
        if (!readU32(payload, offset, enabled) || enabled > 1u ||
            !readU32(payload, offset, parsed.testTransitionFromPose) ||
            !readU32(payload, offset,
                     parsed.testTransitionFromPhaseMilli)) goto malformed;
        parsed.testTransition = enabled != 0u;
    }
    if (version >= kFitSceneReviewVersion) {
        uint32_t present;
        if (!readU32(payload, offset, present) || present != 1u) {
            goto malformed;
        }
        parsed.fitSceneReviewContractPresent = true;
    } else {
        /* A legacy draft remains fully editable, but approval must not be
         * upgraded silently into the stronger exact-scene contract. */
        parsed.reviewedContexts = 0u;
        parsed.contactExceptionContexts = 0u;
    }
    if (version >= kFitSemanticReviewVersion) {
        parsed.fitSemanticReviewContractPresent = true;
    } else if (parsed.fitSceneReviewContractPresent) {
        /* v14 proved three vehicle contexts with a five-state subset and v13
         * proved select with one pose. Neither can silently inherit the v15
         * complete semantic batteries, so every fit remains editable but all
         * approvals reopen once. */
        parsed.reviewedContexts = 0u;
        parsed.contactExceptionContexts = 0u;
    }
    if (version >= kFitMultiSceneReviewVersion) {
        parsed.fitMultiSceneReviewContractPresent = true;
    } else {
        /* v15 proved every semantic in one fixed course per vehicle. It cannot
         * inherit v16's open/dense/alternate course battery. */
        parsed.reviewedContexts = 0u;
        parsed.contactExceptionContexts = 0u;
    }
    if (version >= kFitContactStabilityVersion) {
        parsed.fitContactStabilityContractPresent = true;
    } else {
        /* v16 proves every semantic on all qualified course families, but it
         * has no settled frame-to-frame contact residual witness. */
        parsed.reviewedContexts = 0u;
        parsed.contactExceptionContexts = 0u;
    }
    if (version >= kFitExpandedSceneVersion) {
        parsed.fitExpandedSceneContractPresent = true;
    } else {
        /* v17 has settled contact stability, but only on three course
         * families. It cannot inherit the dark/enclosed and effects-heavy
         * review rows. */
        parsed.reviewedContexts = 0u;
        parsed.contactExceptionContexts = 0u;
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
