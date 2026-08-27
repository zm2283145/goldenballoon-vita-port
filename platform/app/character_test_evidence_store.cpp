#include "character_test_evidence_store.h"

#include "modern_character_capture_projection.h"
#include "sha256.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <set>
#include <tuple>
#include <utility>

namespace {

constexpr const char *kHeaderV1 = "mdkr-character-test-evidence-v1";
constexpr const char *kHeaderV2 = "mdkr-character-test-evidence-v2";
constexpr const char *kHeaderV3 = "mdkr-character-test-evidence-v3";
constexpr const char *kHeaderV4 = "mdkr-character-test-evidence-v4";
constexpr const char *kHeaderV5 = "mdkr-character-test-evidence-v5";
constexpr size_t kFieldsPerRowV1 = 39u;
constexpr size_t kFieldsPerRowV2 = 52u;
constexpr size_t kFieldsPerRowV3 = 105u;
constexpr size_t kFieldsPerRowV4 = 125u;
constexpr size_t kFieldsPerRowV5 = 164u;
constexpr size_t      kMaximumRowBytes =
    (CharacterTestEvidenceStore::kMaximumBuildVersionBytes * 2u) +
    (CharacterTestEvidenceStore::kMaximumBackendBytes * 2u) +
    (CharacterTestEvidenceStore::kMaximumAdapterBytes * 2u) +
    (CharacterTestEvidenceStore::kMaximumDriverBytes * 2u) + 4096u;
constexpr size_t kMaximumSerializedBytes =
    CharacterTestEvidenceStore::kMaximumRecords * kMaximumRowBytes + 256u;

bool slugValid(const std::string &value) {
    if (value.size() < 2u || value.size() > 64u ||
        !((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) {
        return false;
    }
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
        if (digit > maximum || parsed > (maximum - digit) / 10u) {
            return false;
        }
        parsed = parsed * 10u + digit;
    }
    value = parsed;
    return true;
}

bool parseSigned(const std::string &text, int64_t minimum, int64_t maximum,
                 int64_t &value) {
    if (text.empty() || text == "-0") return false;
    const bool negative = text[0] == '-';
    const size_t begin = negative ? 1u : 0u;
    if (begin == text.size() ||
        (text.size() - begin > 1u && text[begin] == '0')) return false;
    uint64_t magnitude = 0u;
    const uint64_t limit = negative
        ? static_cast<uint64_t>(-(minimum + 1)) + 1u
        : static_cast<uint64_t>(maximum);
    if (!parseUnsigned(text.substr(begin), limit, magnitude)) return false;
    if (negative) {
        if (magnitude == static_cast<uint64_t>(INT64_MAX) + 1u) {
            value = INT64_MIN;
        } else {
            value = -static_cast<int64_t>(magnitude);
        }
    } else {
        value = static_cast<int64_t>(magnitude);
    }
    return value >= minimum && value <= maximum;
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
        const unsigned char first =
            static_cast<unsigned char>(text[index++]);
        uint32_t codepoint    = 0u;
        uint32_t minimum      = 0u;
        unsigned continuation = 0u;
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
            codepoint == 0xFEFFu) {
            return false;
        }
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

std::string recordDigest(const std::vector<std::string> &fields) {
    MdkrSha256 digest;
    mdkr_sha256_init(&digest);
    for (const std::string &field : fields) {
        mdkr_sha256_update(&digest, field.data(), field.size());
        const unsigned char separator = 0u;
        mdkr_sha256_update(&digest, &separator, 1u);
    }
    return finishDigest(digest);
}

std::string inventoryDigest(const std::string &header,
                            const std::string &count,
                            const std::string &body) {
    MdkrSha256 digest;
    mdkr_sha256_init(&digest);
    const auto add = [&digest](const std::string &value) {
        mdkr_sha256_update(&digest, value.data(), value.size());
        const unsigned char separator = 0u;
        mdkr_sha256_update(&digest, &separator, 1u);
    };
    add(header);
    add(count);
    mdkr_sha256_update(&digest, body.data(), body.size());
    return finishDigest(digest);
}

bool keyComesBefore(const CharacterTestEvidenceStore::Evidence &left,
                    const CharacterTestEvidenceStore::Evidence &right) {
    return std::tie(left.packageId, left.context, left.players, left.kind) <
           std::tie(right.packageId, right.context, right.players, right.kind);
}

bool sameKey(const CharacterTestEvidenceStore::Evidence &left,
             const CharacterTestEvidenceStore::Evidence &right) {
    return left.packageId == right.packageId &&
           left.context == right.context && left.players == right.players &&
           left.kind == right.kind;
}

bool evidenceValid(const CharacterTestEvidenceStore::Evidence &evidence,
                   std::string                                &error) {
    using namespace CharacterTestEvidenceStore;
    const bool timingStateValid = evidence.intervalSamples == 0u
        ? evidence.intervalP50Us == 0u && evidence.intervalP95Us == 0u &&
              evidence.intervalP99Us == 0u &&
              evidence.intervalMeanUs == 0u &&
              evidence.intervalMaxUs == 0u
        : evidence.warmupComplete && evidence.intervalP50Us != 0u &&
              evidence.displayedFrames >= evidence.intervalSamples &&
              evidence.intervalP50Us <= evidence.intervalP95Us &&
              evidence.intervalP95Us <= evidence.intervalP99Us &&
              evidence.intervalP99Us <= evidence.intervalMaxUs &&
              evidence.intervalMeanUs <= evidence.intervalMaxUs;
    const bool tickwallStateValid = evidence.tickwallSamples == 0u
        ? evidence.tickwallMeanNs == 0u
        : evidence.warmupComplete && evidence.tickwallMeanNs != 0u;
    const auto gpuDistributionEmpty = [](
        const MdkrModernCharacterGpuDistribution &distribution) {
        return distribution.samples == 0u &&
            distribution.percentile_window_samples == 0u &&
            distribution.p50_ns == 0u && distribution.p95_ns == 0u &&
            distribution.p99_ns == 0u && distribution.mean_ns == 0u &&
            distribution.max_ns == 0u;
    };
    const bool legacyGpuTimingEmpty = evidence.gpuTiming.version == 0u &&
        evidence.gpuTiming.status ==
            MDKR_MODERN_CHARACTER_GPU_TIMING_UNSUPPORTED &&
        evidence.gpuTiming.supported_scopes == 0u &&
        evidence.gpuTiming.ring_full_frames == 0u &&
        evidence.gpuTiming.pending_frames == 0u &&
        evidence.gpuTiming.invalid_samples == 0u &&
        gpuDistributionEmpty(evidence.gpuTiming.scene_pass) &&
        gpuDistributionEmpty(evidence.gpuTiming.character_draws);
    const bool gpuTimingStateValid = evidence.resultVersion >= 11u
        ? mdkr_modern_character_gpu_timing_metrics_valid(
              &evidence.gpuTiming) != 0
        : legacyGpuTimingEmpty;
    const bool drawStateValid =
        (evidence.replacementDraws != 0u ||
         evidence.replacementPrimitives == 0u) &&
        (evidence.warmupComplete ||
         (evidence.replacementDraws == 0u &&
          evidence.replacementPrimitives == 0u &&
          evidence.hiddenDonorBatches == 0u &&
          evidence.contactSolves == 0u &&
          evidence.contactErrorMeanMicrometres == 0u &&
          evidence.contactErrorMaxMicrometres == 0u));
    const bool contactStateValid = evidence.contactSolves == 0u
        ? evidence.contactErrorMeanMicrometres == 0u &&
              evidence.contactErrorMaxMicrometres == 0u
        : evidence.context != 1u &&
              evidence.contactErrorMeanMicrometres <=
                  evidence.contactErrorMaxMicrometres;
    constexpr uint32_t kAllContactWitnesses = 0xFu;
    constexpr int64_t kMaximumContactMicrometres = 1000000000LL;
    bool contactWitnessStateValid =
        evidence.contactWitnessMask == 0u ||
        evidence.contactWitnessMask == kAllContactWitnesses;
    uint64_t latestContactMaximum = 0u;
    if (evidence.context == 1u && evidence.contactWitnessMask != 0u) {
        contactWitnessStateValid = false;
    }
    if (evidence.contactWitnessMask != 0u &&
        evidence.replacementDraws == 0u) {
        contactWitnessStateValid = false;
    }
    if (evidence.resultVersion >= 10u && evidence.contactSolves != 0u &&
        evidence.contactWitnessMask != kAllContactWitnesses) {
        contactWitnessStateValid = false;
    }
    for (size_t contact = 0u; contact < 4u; ++contact) {
        const bool valid =
            (evidence.contactWitnessMask & (1u << contact)) != 0u;
        for (size_t axis = 0u; axis < 3u; ++axis) {
            const int64_t values[] = {
                evidence.contactChainRootMicrometres[contact][axis],
                evidence.contactBendMicrometres[contact][axis],
                evidence.contactTargetMicrometres[contact][axis],
                evidence.contactEndMicrometres[contact][axis],
            };
            for (int64_t value : values) {
                contactWitnessStateValid = contactWitnessStateValid &&
                    (valid || value == 0) &&
                    value >= -kMaximumContactMicrometres &&
                    value <= kMaximumContactMicrometres;
            }
        }
        const uint64_t reported =
            evidence.contactWitnessErrorMicrometres[contact];
        if (!valid) {
            contactWitnessStateValid =
                contactWitnessStateValid && reported == 0u;
            continue;
        }
        long double squared = 0.0L;
        for (size_t axis = 0u; axis < 3u; ++axis) {
            const long double difference = static_cast<long double>(
                evidence.contactEndMicrometres[contact][axis] -
                evidence.contactTargetMicrometres[contact][axis]);
            squared += difference * difference;
        }
        const uint64_t measured =
            static_cast<uint64_t>(std::sqrt(squared) + 0.5L);
        const uint64_t delta = measured > reported
            ? measured - reported : reported - measured;
        contactWitnessStateValid = contactWitnessStateValid &&
            reported <= static_cast<uint64_t>(kMaximumContactMicrometres) &&
            delta <= 3u;
        latestContactMaximum = std::max(latestContactMaximum, reported);
    }
    if (evidence.contactSolves != 0u &&
        latestContactMaximum > evidence.contactErrorMaxMicrometres &&
        latestContactMaximum - evidence.contactErrorMaxMicrometres > 3u) {
        contactWitnessStateValid = false;
    }
    const bool dimensionsValid =
        (evidence.outputWidth == 0u) == (evidence.outputHeight == 0u) &&
        (evidence.renderWidth == 0u) == (evidence.renderHeight == 0u);
    bool fitStateValid = true;
    int64_t forwardLengthSquared = 0;
    constexpr int64_t kMaximumFitMicrometres = 1000000000LL;
    for (unsigned axis = 0u; axis < 3u; ++axis) {
        if (!evidence.fitDiagnosticsValid) {
            fitStateValid = fitStateValid &&
                evidence.fitBoundsMinMicrometres[axis] == 0 &&
                evidence.fitBoundsMaxMicrometres[axis] == 0 &&
                evidence.fitAnchorMicrometres[axis] == 0 &&
                evidence.fitForwardMilli[axis] == 0;
            continue;
        }
        const int64_t minimum = evidence.fitBoundsMinMicrometres[axis];
        const int64_t maximum = evidence.fitBoundsMaxMicrometres[axis];
        const int64_t anchor = evidence.fitAnchorMicrometres[axis];
        const int64_t forward = evidence.fitForwardMilli[axis];
        const bool forwardInRange = forward >= -1001 && forward <= 1001;
        fitStateValid = fitStateValid && evidence.replacementDraws != 0u &&
            minimum <= maximum &&
            minimum >= -kMaximumFitMicrometres &&
            maximum <= kMaximumFitMicrometres &&
            anchor >= -kMaximumFitMicrometres &&
            anchor <= kMaximumFitMicrometres &&
            forwardInRange;
        if (forwardInRange) forwardLengthSquared += forward * forward;
    }
    if (evidence.fitDiagnosticsValid) {
        fitStateValid = fitStateValid && forwardLengthSquared >= 995000LL &&
            forwardLengthSquared <= 1005000LL;
    }
    constexpr uint32_t kAllLandmarks = 0x7u;
    bool landmarkStateValid =
        (evidence.fitLandmarkMask & ~kAllLandmarks) == 0u;
    for (size_t landmark = 0u; landmark < 3u; ++landmark) {
        const bool valid =
            (evidence.fitLandmarkMask & (1u << landmark)) != 0u;
        for (size_t axis = 0u; axis < 3u; ++axis) {
            const int64_t value =
                evidence.fitLandmarkMicrometres[landmark][axis];
            landmarkStateValid = landmarkStateValid &&
                (valid || value == 0) &&
                value >= -kMaximumFitMicrometres &&
                value <= kMaximumFitMicrometres;
        }
    }
    if (!evidence.fitDiagnosticsValid && evidence.fitLandmarkMask != 0u) {
        landmarkStateValid = false;
    }
    if (evidence.resultVersion >= 15u && evidence.warmupComplete &&
        evidence.replacementDraws != 0u &&
        (evidence.fitLandmarkMask & 0x4u) == 0u) {
        landmarkStateValid = false;
    }
    const auto cameraValuesZero = [&evidence]() {
        if (evidence.cameraProjectionWidth != 0u ||
            evidence.cameraProjectionHeight != 0u ||
            evidence.cameraProjectionPrimitiveDraws != 0u) return false;
        for (size_t component = 0u; component < 4u; ++component) {
            if (evidence.cameraProjectionViewport[component] != 0 ||
                evidence.cameraProjectionScissor[component] != 0 ||
                evidence.cameraBoundsPixelMilli[component] != 0) return false;
        }
        for (size_t landmark = 0u; landmark < 3u; ++landmark) {
            if (evidence.cameraLandmarkPixelMilli[landmark][0] != 0 ||
                evidence.cameraLandmarkPixelMilli[landmark][1] != 0 ||
                evidence.cameraLandmarkDepthMillionths[landmark] != 0 ||
                evidence.cameraLandmarkClipFlags[landmark] != 0u) return false;
        }
        if (evidence.cameraBoundsClipFlags != 0u) return false;
        return true;
    };
    bool cameraStateValid = evidence.cameraProjectionValid ||
        cameraValuesZero();
    if (evidence.cameraProjectionValid) {
        const auto rectValid = [&evidence](const int32_t rect[4]) {
            return rect[0] >= 0 && rect[1] >= 0 && rect[2] > 0 && rect[3] > 0 &&
                static_cast<int64_t>(rect[0]) + rect[2] <=
                    evidence.cameraProjectionWidth &&
                static_cast<int64_t>(rect[1]) + rect[3] <=
                    evidence.cameraProjectionHeight;
        };
        constexpr uint32_t kKnownClipFlags =
            MDKR_MODERN_CHARACTER_PROJECTION_CLIP_LEFT |
            MDKR_MODERN_CHARACTER_PROJECTION_CLIP_RIGHT |
            MDKR_MODERN_CHARACTER_PROJECTION_CLIP_TOP |
            MDKR_MODERN_CHARACTER_PROJECTION_CLIP_BOTTOM |
            MDKR_MODERN_CHARACTER_PROJECTION_CLIP_NEAR |
            MDKR_MODERN_CHARACTER_PROJECTION_CLIP_FAR |
            MDKR_MODERN_CHARACTER_PROJECTION_CLIP_SCISSOR;
        cameraStateValid = cameraStateValid &&
            evidence.fitDiagnosticsValid &&
            evidence.cameraProjectionWidth != 0u &&
            evidence.cameraProjectionHeight != 0u &&
            evidence.cameraProjectionWidth == evidence.renderWidth &&
            evidence.cameraProjectionHeight == evidence.renderHeight &&
            evidence.cameraProjectionWidth <= 16384u &&
            evidence.cameraProjectionHeight <= 16384u &&
            evidence.cameraProjectionPrimitiveDraws != 0u &&
            evidence.cameraProjectionPrimitiveDraws <= 4096u &&
            rectValid(evidence.cameraProjectionViewport) &&
            rectValid(evidence.cameraProjectionScissor) &&
            evidence.cameraBoundsPixelMilli[0] <
                evidence.cameraBoundsPixelMilli[2] &&
            evidence.cameraBoundsPixelMilli[1] <
                evidence.cameraBoundsPixelMilli[3];
        cameraStateValid = cameraStateValid &&
            (evidence.cameraBoundsClipFlags & ~kKnownClipFlags) == 0u;
        const int64_t viewportLeft =
            static_cast<int64_t>(evidence.cameraProjectionViewport[0]) * 1000;
        const int64_t viewportTop =
            static_cast<int64_t>(evidence.cameraProjectionViewport[1]) * 1000;
        const int64_t viewportRight =
            (static_cast<int64_t>(evidence.cameraProjectionViewport[0]) +
             evidence.cameraProjectionViewport[2]) * 1000;
        const int64_t viewportBottom =
            (static_cast<int64_t>(evidence.cameraProjectionViewport[1]) +
             evidence.cameraProjectionViewport[3]) * 1000;
        const int64_t scissorLeft =
            static_cast<int64_t>(evidence.cameraProjectionScissor[0]) * 1000;
        const int64_t scissorTop =
            static_cast<int64_t>(evidence.cameraProjectionScissor[1]) * 1000;
        const int64_t scissorRight =
            (static_cast<int64_t>(evidence.cameraProjectionScissor[0]) +
             evidence.cameraProjectionScissor[2]) * 1000;
        const int64_t scissorBottom =
            (static_cast<int64_t>(evidence.cameraProjectionScissor[1]) +
             evidence.cameraProjectionScissor[3]) * 1000;
        constexpr int64_t kProjectionTolerance = 1;
        const bool boundsOutsideScissor =
            evidence.cameraBoundsPixelMilli[0] <
                scissorLeft - kProjectionTolerance ||
            evidence.cameraBoundsPixelMilli[2] >
                scissorRight + kProjectionTolerance ||
            evidence.cameraBoundsPixelMilli[1] <
                scissorTop - kProjectionTolerance ||
            evidence.cameraBoundsPixelMilli[3] >
                scissorBottom + kProjectionTolerance;
        cameraStateValid = cameraStateValid &&
            ((evidence.cameraBoundsPixelMilli[0] <
                  viewportLeft - kProjectionTolerance) ==
             ((evidence.cameraBoundsClipFlags &
               MDKR_MODERN_CHARACTER_PROJECTION_CLIP_LEFT) != 0u)) &&
            ((evidence.cameraBoundsPixelMilli[2] >
                  viewportRight + kProjectionTolerance) ==
             ((evidence.cameraBoundsClipFlags &
               MDKR_MODERN_CHARACTER_PROJECTION_CLIP_RIGHT) != 0u)) &&
            ((evidence.cameraBoundsPixelMilli[1] <
                  viewportTop - kProjectionTolerance) ==
             ((evidence.cameraBoundsClipFlags &
               MDKR_MODERN_CHARACTER_PROJECTION_CLIP_TOP) != 0u)) &&
            ((evidence.cameraBoundsPixelMilli[3] >
                  viewportBottom + kProjectionTolerance) ==
             ((evidence.cameraBoundsClipFlags &
               MDKR_MODERN_CHARACTER_PROJECTION_CLIP_BOTTOM) != 0u)) &&
            (boundsOutsideScissor ==
             ((evidence.cameraBoundsClipFlags &
               MDKR_MODERN_CHARACTER_PROJECTION_CLIP_SCISSOR) != 0u));
        for (size_t landmark = 0u; landmark < 3u; ++landmark) {
            const bool valid =
                (evidence.fitLandmarkMask & (1u << landmark)) != 0u;
            const bool valuesZero =
                evidence.cameraLandmarkPixelMilli[landmark][0] == 0 &&
                evidence.cameraLandmarkPixelMilli[landmark][1] == 0 &&
                evidence.cameraLandmarkDepthMillionths[landmark] == 0 &&
                evidence.cameraLandmarkClipFlags[landmark] == 0u;
            cameraStateValid = cameraStateValid &&
                (valid || valuesZero) &&
                (evidence.cameraLandmarkClipFlags[landmark] &
                 ~kKnownClipFlags) == 0u;
            if (valid) {
                const int64_t x =
                    evidence.cameraLandmarkPixelMilli[landmark][0];
                const int64_t y =
                    evidence.cameraLandmarkPixelMilli[landmark][1];
                const int32_t depth =
                    evidence.cameraLandmarkDepthMillionths[landmark];
                const uint32_t flags =
                    evidence.cameraLandmarkClipFlags[landmark];
                const bool outsideScissor =
                    x < scissorLeft - kProjectionTolerance ||
                    x > scissorRight + kProjectionTolerance ||
                    y < scissorTop - kProjectionTolerance ||
                    y > scissorBottom + kProjectionTolerance;
                cameraStateValid = cameraStateValid &&
                    ((x < viewportLeft - kProjectionTolerance) ==
                     ((flags &
                       MDKR_MODERN_CHARACTER_PROJECTION_CLIP_LEFT) != 0u)) &&
                    ((x > viewportRight + kProjectionTolerance) ==
                     ((flags &
                       MDKR_MODERN_CHARACTER_PROJECTION_CLIP_RIGHT) != 0u)) &&
                    ((y < viewportTop - kProjectionTolerance) ==
                     ((flags &
                       MDKR_MODERN_CHARACTER_PROJECTION_CLIP_TOP) != 0u)) &&
                    ((y > viewportBottom + kProjectionTolerance) ==
                     ((flags &
                       MDKR_MODERN_CHARACTER_PROJECTION_CLIP_BOTTOM) != 0u)) &&
                    ((depth < 0) ==
                     ((flags &
                       MDKR_MODERN_CHARACTER_PROJECTION_CLIP_NEAR) != 0u)) &&
                    ((depth > 1000000) ==
                     ((flags &
                       MDKR_MODERN_CHARACTER_PROJECTION_CLIP_FAR) != 0u)) &&
                    (outsideScissor ==
                     ((flags &
                       MDKR_MODERN_CHARACTER_PROJECTION_CLIP_SCISSOR) != 0u));
            }
        }
    }
    if (evidence.resultVersion >= 15u && evidence.warmupComplete &&
        evidence.replacementDraws != 0u &&
        !evidence.cameraProjectionValid) {
        cameraStateValid = false;
    }
    if (evidence.kind != Kind::Latest && evidence.kind != Kind::Baseline)
        error = "test evidence kind is invalid";
    else if (!slugValid(evidence.packageId))
        error = "test evidence package id is invalid";
    else if (!digestValid(evidence.sourceSha256) ||
             !digestValid(evidence.fitSha256) ||
             !digestValid(evidence.presentationSha256))
        error = "test evidence fingerprint is invalid";
    else if (!printableUtf8(evidence.buildVersion,
                            kMaximumBuildVersionBytes,
                            true))
        error = "test evidence build version is invalid";
    else if (evidence.context < 1u || evidence.context > 4u ||
             evidence.players < 1u || evidence.players > 4u ||
             evidence.resultVersion == 0u ||
             evidence.resultVersion > 65535u)
        error = "test evidence context, layout, or result version is invalid";
    else if (evidence.warmupComplete &&
             (!evidence.started || evidence.warmupTicks == 0u))
        error = "test evidence warm-up state is inconsistent";
    else if (!timingStateValid || !tickwallStateValid ||
             !gpuTimingStateValid)
        error = "test evidence timing state or distribution is inconsistent";
    else if (!drawStateValid)
        error = "test evidence draw state is inconsistent";
    else if (!printableUtf8(evidence.backend, kMaximumBackendBytes, evidence.started) ||
             !printableUtf8(evidence.adapter, kMaximumAdapterBytes, false) ||
             !printableUtf8(evidence.driver, kMaximumDriverBytes, false))
        error = "test evidence device text is invalid";
    else if (!dimensionsValid)
        error = "test evidence dimensions are incomplete";
    else if (evidence.started &&
             (evidence.outputWidth == 0u || evidence.renderWidth == 0u))
        error = "started test evidence has no render dimensions";
    else if (!contactStateValid)
        error = "test evidence contact state or distribution is inconsistent";
    else if (!contactWitnessStateValid)
        error = "test evidence contact witnesses are inconsistent";
    else if (!fitStateValid)
        error = "test evidence fit diagnostics are inconsistent";
    else if (!landmarkStateValid || !cameraStateValid)
        error = "test evidence camera or anatomy diagnostics are inconsistent";
    else if (evidence.kind == Kind::Baseline && !qualified(evidence))
        error = "a comparison baseline must be qualified evidence";
    else
        return true;
    return false;
}

std::vector<std::string> recordFields(
    const CharacterTestEvidenceStore::Evidence &evidence) {
    const auto number = [](uint64_t value) { return std::to_string(value); };
    const auto signedNumber = [](int64_t value) {
        return std::to_string(value);
    };
    std::vector<std::string> fields = {
        number(static_cast<uint32_t>(evidence.kind)),
        evidence.packageId,
        number(evidence.capturedUnix),
        evidence.sourceSha256,
        evidence.fitSha256,
        evidence.presentationSha256,
        encodeHex(evidence.buildVersion),
        number(evidence.context),
        number(evidence.players),
        number(evidence.resultVersion),
        evidence.started ? "1" : "0",
        evidence.warmupComplete ? "1" : "0",
        evidence.realtime ? "1" : "0",
        number(evidence.warmupTicks),
        number(evidence.intervalSamples),
        number(evidence.displayedFrames),
        number(evidence.intervalP50Us),
        number(evidence.intervalP95Us),
        number(evidence.intervalP99Us),
        number(evidence.intervalMeanUs),
        number(evidence.intervalMaxUs),
        number(evidence.tickwallSamples),
        number(evidence.tickwallMeanNs),
        number(evidence.replacementDraws),
        number(evidence.replacementPrimitives),
        number(evidence.hiddenDonorBatches),
        number(evidence.contactSolves),
        number(evidence.contactErrorMeanMicrometres),
        number(evidence.contactErrorMaxMicrometres),
        encodeHex(evidence.backend),
        encodeHex(evidence.adapter),
        encodeHex(evidence.driver),
        number(evidence.vendorId),
        number(evidence.deviceId),
        number(evidence.outputWidth),
        number(evidence.outputHeight),
        number(evidence.renderWidth),
        number(evidence.renderHeight),
    };
    fields.push_back(evidence.fitDiagnosticsValid ? "1" : "0");
    for (int64_t value : evidence.fitBoundsMinMicrometres) {
        fields.push_back(signedNumber(value));
    }
    for (int64_t value : evidence.fitBoundsMaxMicrometres) {
        fields.push_back(signedNumber(value));
    }
    for (int64_t value : evidence.fitAnchorMicrometres) {
        fields.push_back(signedNumber(value));
    }
    for (int32_t value : evidence.fitForwardMilli) {
        fields.push_back(signedNumber(value));
    }
    fields.push_back(number(evidence.contactWitnessMask));
    const auto appendContactPoints = [&](const int64_t points[4][3]) {
        for (size_t contact = 0u; contact < 4u; ++contact) {
            for (size_t axis = 0u; axis < 3u; ++axis) {
                fields.push_back(signedNumber(points[contact][axis]));
            }
        }
    };
    appendContactPoints(evidence.contactChainRootMicrometres);
    appendContactPoints(evidence.contactBendMicrometres);
    appendContactPoints(evidence.contactTargetMicrometres);
    appendContactPoints(evidence.contactEndMicrometres);
    for (uint64_t value : evidence.contactWitnessErrorMicrometres) {
        fields.push_back(number(value));
    }
    fields.push_back(number(evidence.gpuTiming.version));
    fields.push_back(number(
        static_cast<uint32_t>(evidence.gpuTiming.status)));
    fields.push_back(number(evidence.gpuTiming.supported_scopes));
    fields.push_back(number(evidence.gpuTiming.ring_full_frames));
    fields.push_back(number(evidence.gpuTiming.pending_frames));
    fields.push_back(number(evidence.gpuTiming.invalid_samples));
    const auto appendGpuDistribution =
        [&](const MdkrModernCharacterGpuDistribution &distribution) {
            fields.push_back(number(distribution.samples));
            fields.push_back(number(
                distribution.percentile_window_samples));
            fields.push_back(number(distribution.p50_ns));
            fields.push_back(number(distribution.p95_ns));
            fields.push_back(number(distribution.p99_ns));
            fields.push_back(number(distribution.mean_ns));
            fields.push_back(number(distribution.max_ns));
        };
    appendGpuDistribution(evidence.gpuTiming.scene_pass);
    appendGpuDistribution(evidence.gpuTiming.character_draws);
    fields.push_back(number(evidence.fitLandmarkMask));
    for (const auto &landmark : evidence.fitLandmarkMicrometres) {
        for (int64_t value : landmark) {
            fields.push_back(signedNumber(value));
        }
    }
    fields.push_back(evidence.cameraProjectionValid ? "1" : "0");
    fields.push_back(number(evidence.cameraProjectionWidth));
    fields.push_back(number(evidence.cameraProjectionHeight));
    for (int32_t value : evidence.cameraProjectionViewport) {
        fields.push_back(signedNumber(value));
    }
    for (int32_t value : evidence.cameraProjectionScissor) {
        fields.push_back(signedNumber(value));
    }
    fields.push_back(number(evidence.cameraProjectionPrimitiveDraws));
    for (int32_t value : evidence.cameraBoundsPixelMilli) {
        fields.push_back(signedNumber(value));
    }
    for (const auto &landmark : evidence.cameraLandmarkPixelMilli) {
        for (int32_t value : landmark) {
            fields.push_back(signedNumber(value));
        }
    }
    for (int32_t value : evidence.cameraLandmarkDepthMillionths) {
        fields.push_back(signedNumber(value));
    }
    for (uint32_t value : evidence.cameraLandmarkClipFlags) {
        fields.push_back(number(value));
    }
    fields.push_back(number(evidence.cameraBoundsClipFlags));
    return fields;
}

} // namespace

namespace CharacterTestEvidenceStore {

bool qualified(const Evidence &evidence) {
    return evidence.started && evidence.warmupComplete &&
           evidence.realtime && evidence.intervalSamples >= 60u &&
           evidence.replacementDraws != 0u &&
           !evidence.backend.empty() && !evidence.adapter.empty() &&
           evidence.outputWidth != 0u && evidence.outputHeight != 0u &&
           evidence.renderWidth != 0u && evidence.renderHeight != 0u;
}

PerformanceTarget performanceTarget(uint32_t players) {
    // Every supported local layout targets a 60 Hz presentation. The p95
    // allowance covers one millisecond of ordinary scheduling variance; the
    // p99 ceiling catches sustained hitches without pretending that a single
    // platform maximum is meaningful across all devices.
    (void)players;
    return {18334u, 25000u};
}

PerformanceResult performanceResult(const Evidence &evidence) {
    if (!qualified(evidence) || evidence.players < 1u ||
        evidence.players > 4u || evidence.intervalP95Us == 0u ||
        evidence.intervalP99Us == 0u) {
        return PerformanceResult::Unqualified;
    }
    const PerformanceTarget target = performanceTarget(evidence.players);
    return evidence.intervalP95Us <= target.p95IntervalUs &&
                   evidence.intervalP99Us <= target.p99IntervalUs
               ? PerformanceResult::TargetMet
               : PerformanceResult::OverBudget;
}

bool performanceTargetMet(const Evidence &evidence) {
    return performanceResult(evidence) == PerformanceResult::TargetMet;
}

bool comparable(const Evidence &latest, const Evidence &baseline) {
    return qualified(latest) && qualified(baseline) &&
           latest.kind == Kind::Latest && baseline.kind == Kind::Baseline &&
           latest.packageId == baseline.packageId &&
           latest.context == baseline.context &&
           latest.players == baseline.players &&
           latest.resultVersion == baseline.resultVersion &&
           latest.presentationSha256 == baseline.presentationSha256 &&
           latest.buildVersion == baseline.buildVersion &&
           latest.backend == baseline.backend &&
           latest.adapter == baseline.adapter &&
           latest.driver == baseline.driver &&
           latest.vendorId == baseline.vendorId &&
           latest.deviceId == baseline.deviceId &&
           latest.outputWidth == baseline.outputWidth &&
           latest.outputHeight == baseline.outputHeight &&
           latest.renderWidth == baseline.renderWidth &&
           latest.renderHeight == baseline.renderHeight;
}

bool parse(const std::string &text, Inventory &output, std::string &error) {
    error.clear();
    Inventory                parsed;
    std::vector<std::string> fields;
    std::set<std::string>    packages;
    size_t                   begin     = 0u;
    size_t                   end       = text.find('\n');
    const size_t             bodyBegin = end == std::string::npos ? 0u : end + 1u;
    uint64_t                 count     = 0u;
    if (text.size() > kMaximumSerializedBytes || end == std::string::npos ||
        !split(text.substr(0u, end), 3u, fields) ||
        (fields[0] != kHeaderV1 && fields[0] != kHeaderV2 &&
         fields[0] != kHeaderV3 && fields[0] != kHeaderV4 &&
         fields[0] != kHeaderV5) ||
        !parseUnsigned(fields[1], kMaximumRecords, count) ||
        !digestValid(fields[2])) {
        error = "test evidence inventory header is invalid";
        return false;
    }
    const std::string header = fields[0];
    const bool legacyV1 = header == kHeaderV1;
    const bool legacyV2 = header == kHeaderV2;
    const bool legacyV3 = header == kHeaderV3;
    const bool legacyV4 = header == kHeaderV4;
    const size_t rowFields = legacyV1 ? kFieldsPerRowV1
        : legacyV2 ? kFieldsPerRowV2
        : legacyV3 ? kFieldsPerRowV3
        : legacyV4 ? kFieldsPerRowV4 : kFieldsPerRowV5;
    const std::string countText         = fields[1];
    const std::string inventoryChecksum = fields[2];
    begin                               = end + 1u;
    for (uint64_t index = 0u; index < count; ++index) {
        end = text.find('\n', begin);
        if (end == std::string::npos ||
            !split(text.substr(begin, end - begin), rowFields, fields)) {
            error = "test evidence inventory row is malformed";
            return false;
        }
        Evidence     evidence;
        uint64_t     numbers[30]{};
        const size_t numericFields[] = {
            0u,
            2u,
            7u,
            8u,
            9u,
            10u,
            11u,
            12u,
            13u,
            14u,
            15u,
            16u,
            17u,
            18u,
            19u,
            20u,
            21u,
            22u,
            23u,
            24u,
            25u,
            26u,
            27u,
            28u,
            32u,
            33u,
            34u,
            35u,
            36u,
            37u,
        };
        static_assert(sizeof(numericFields) / sizeof(numericFields[0]) == 30u,
                      "numeric field parser must cover every numeric column");
        bool numericValid = true;
        for (size_t numberIndex = 0u; numberIndex < 30u; ++numberIndex) {
            uint64_t     maximum = UINT64_MAX;
            const size_t field   = numericFields[numberIndex];
            if (field == 0u || field == 10u || field == 11u || field == 12u) {
                maximum = 1u;
            } else if (field == 7u || field == 8u) {
                maximum = 4u;
            } else if (field == 9u) {
                maximum = 65535u;
            } else if (field >= 32u) {
                maximum = UINT32_MAX;
            }
            if (!parseUnsigned(fields[field], maximum, numbers[numberIndex])) {
                numericValid = false;
                break;
            }
        }
        if (!numericValid || !digestValid(fields[3]) ||
            !digestValid(fields[4]) || !digestValid(fields[5]) ||
            !decodeHex(fields[6], kMaximumBuildVersionBytes, evidence.buildVersion) ||
            !decodeHex(fields[29], kMaximumBackendBytes, evidence.backend) ||
            !decodeHex(fields[30], kMaximumAdapterBytes, evidence.adapter) ||
            !decodeHex(fields[31], kMaximumDriverBytes, evidence.driver) ||
            !digestValid(fields.back())) {
            error = "test evidence inventory row fields are invalid";
            return false;
        }
        size_t n                             = 0u;
        evidence.kind                        = static_cast<Kind>(numbers[n++]);
        evidence.packageId                   = fields[1];
        evidence.capturedUnix                = numbers[n++];
        evidence.sourceSha256                = fields[3];
        evidence.fitSha256                   = fields[4];
        evidence.presentationSha256          = fields[5];
        evidence.context                     = static_cast<uint32_t>(numbers[n++]);
        evidence.players                     = static_cast<uint32_t>(numbers[n++]);
        evidence.resultVersion               = static_cast<uint32_t>(numbers[n++]);
        evidence.started                     = numbers[n++] != 0u;
        evidence.warmupComplete              = numbers[n++] != 0u;
        evidence.realtime                    = numbers[n++] != 0u;
        evidence.warmupTicks                 = numbers[n++];
        evidence.intervalSamples             = numbers[n++];
        evidence.displayedFrames             = numbers[n++];
        evidence.intervalP50Us               = numbers[n++];
        evidence.intervalP95Us               = numbers[n++];
        evidence.intervalP99Us               = numbers[n++];
        evidence.intervalMeanUs              = numbers[n++];
        evidence.intervalMaxUs               = numbers[n++];
        evidence.tickwallSamples             = numbers[n++];
        evidence.tickwallMeanNs              = numbers[n++];
        evidence.replacementDraws            = numbers[n++];
        evidence.replacementPrimitives       = numbers[n++];
        evidence.hiddenDonorBatches          = numbers[n++];
        evidence.contactSolves               = numbers[n++];
        evidence.contactErrorMeanMicrometres = numbers[n++];
        evidence.contactErrorMaxMicrometres  = numbers[n++];
        evidence.vendorId                    = static_cast<uint32_t>(numbers[n++]);
        evidence.deviceId                    = static_cast<uint32_t>(numbers[n++]);
        evidence.outputWidth                 = static_cast<uint32_t>(numbers[n++]);
        evidence.outputHeight                = static_cast<uint32_t>(numbers[n++]);
        evidence.renderWidth                 = static_cast<uint32_t>(numbers[n++]);
        evidence.renderHeight                = static_cast<uint32_t>(numbers[n++]);
        if (!legacyV1) {
            uint64_t fitValid = 0u;
            int64_t signedValue = 0;
            bool fitFieldsValid = parseUnsigned(fields[38], 1u, fitValid);
            evidence.fitDiagnosticsValid = fitValid != 0u;
            for (size_t axis = 0u; axis < 3u && fitFieldsValid; ++axis) {
                fitFieldsValid = parseSigned(
                    fields[39u + axis], -1000000000LL, 1000000000LL,
                    signedValue);
                evidence.fitBoundsMinMicrometres[axis] = signedValue;
            }
            for (size_t axis = 0u; axis < 3u && fitFieldsValid; ++axis) {
                fitFieldsValid = parseSigned(
                    fields[42u + axis], -1000000000LL, 1000000000LL,
                    signedValue);
                evidence.fitBoundsMaxMicrometres[axis] = signedValue;
            }
            for (size_t axis = 0u; axis < 3u && fitFieldsValid; ++axis) {
                fitFieldsValid = parseSigned(
                    fields[45u + axis], -1000000000LL, 1000000000LL,
                    signedValue);
                evidence.fitAnchorMicrometres[axis] = signedValue;
            }
            for (size_t axis = 0u; axis < 3u && fitFieldsValid; ++axis) {
                fitFieldsValid = parseSigned(
                    fields[48u + axis], -1001, 1001, signedValue);
                evidence.fitForwardMilli[axis] =
                    static_cast<int32_t>(signedValue);
            }
            if (!fitFieldsValid) {
                error = "test evidence fit fields are invalid";
                return false;
            }
        }
        if (!legacyV1 && !legacyV2) {
            uint64_t witnessMask = 0u;
            bool witnessFieldsValid =
                parseUnsigned(fields[51], 0xFu, witnessMask);
            evidence.contactWitnessMask =
                static_cast<uint32_t>(witnessMask);
            size_t field = 52u;
            const auto parseContactPoints =
                [&](int64_t points[4][3]) {
                    for (size_t contact = 0u;
                         contact < 4u && witnessFieldsValid; ++contact) {
                        for (size_t axis = 0u;
                             axis < 3u && witnessFieldsValid; ++axis) {
                            witnessFieldsValid = parseSigned(
                                fields[field++], -1000000000LL,
                                1000000000LL, points[contact][axis]);
                        }
                    }
                };
            parseContactPoints(evidence.contactChainRootMicrometres);
            parseContactPoints(evidence.contactBendMicrometres);
            parseContactPoints(evidence.contactTargetMicrometres);
            parseContactPoints(evidence.contactEndMicrometres);
            for (size_t contact = 0u;
                 contact < 4u && witnessFieldsValid; ++contact) {
                witnessFieldsValid = parseUnsigned(
                    fields[field++], 1000000000ULL,
                    evidence.contactWitnessErrorMicrometres[contact]);
            }
            if (!witnessFieldsValid || field != 104u) {
                error = "test evidence contact witness fields are invalid";
                return false;
            }
        }
        if (!legacyV1 && !legacyV2 && !legacyV3) {
            uint64_t values[20]{};
            bool gpuFieldsValid = true;
            for (size_t index = 0u; index < 20u; ++index) {
                uint64_t maximum = UINT64_MAX;
                if (index == 0u) {
                    maximum = UINT32_MAX;
                } else if (index == 1u) {
                    maximum = MDKR_MODERN_CHARACTER_GPU_TIMING_ERROR;
                } else if (index == 2u) {
                    maximum = MDKR_MODERN_CHARACTER_GPU_SCOPE_SCENE_PASS |
                        MDKR_MODERN_CHARACTER_GPU_SCOPE_CHARACTER_DRAWS;
                } else if (index == 4u) {
                    maximum = 64u;
                }
                gpuFieldsValid = parseUnsigned(
                    fields[104u + index], maximum, values[index]);
                if (!gpuFieldsValid) break;
            }
            if (!gpuFieldsValid) {
                error = "test evidence GPU timing fields are invalid";
                return false;
            }
            size_t index = 0u;
            evidence.gpuTiming.version =
                static_cast<uint32_t>(values[index++]);
            evidence.gpuTiming.status =
                static_cast<MdkrModernCharacterGpuTimingStatus>(
                    values[index++]);
            evidence.gpuTiming.supported_scopes =
                static_cast<uint32_t>(values[index++]);
            evidence.gpuTiming.ring_full_frames = values[index++];
            evidence.gpuTiming.pending_frames = values[index++];
            evidence.gpuTiming.invalid_samples = values[index++];
            const auto assignDistribution =
                [&](MdkrModernCharacterGpuDistribution &distribution) {
                    distribution.samples = values[index++];
                    distribution.percentile_window_samples = values[index++];
                    distribution.p50_ns = values[index++];
                    distribution.p95_ns = values[index++];
                    distribution.p99_ns = values[index++];
                    distribution.mean_ns = values[index++];
                    distribution.max_ns = values[index++];
                };
            assignDistribution(evidence.gpuTiming.scene_pass);
            assignDistribution(evidence.gpuTiming.character_draws);
        }
        if (header == kHeaderV5) {
            size_t field = 124u;
            uint64_t unsignedValue = 0u;
            int64_t signedValue = 0;
            bool spatialFieldsValid = parseUnsigned(
                fields[field++], 0x7u, unsignedValue);
            evidence.fitLandmarkMask =
                static_cast<uint32_t>(unsignedValue);
            for (size_t landmark = 0u;
                 landmark < 3u && spatialFieldsValid; ++landmark) {
                for (size_t axis = 0u;
                     axis < 3u && spatialFieldsValid; ++axis) {
                    spatialFieldsValid = parseSigned(
                        fields[field++], -1000000000LL, 1000000000LL,
                        evidence.fitLandmarkMicrometres[landmark][axis]);
                }
            }
            spatialFieldsValid = spatialFieldsValid && parseUnsigned(
                fields[field++], 1u, unsignedValue);
            evidence.cameraProjectionValid = unsignedValue != 0u;
            spatialFieldsValid = spatialFieldsValid && parseUnsigned(
                fields[field++], 16384u, unsignedValue);
            evidence.cameraProjectionWidth =
                static_cast<uint32_t>(unsignedValue);
            spatialFieldsValid = spatialFieldsValid && parseUnsigned(
                fields[field++], 16384u, unsignedValue);
            evidence.cameraProjectionHeight =
                static_cast<uint32_t>(unsignedValue);
            const auto parseSigned32 = [&](int32_t &output) {
                if (!spatialFieldsValid) return;
                spatialFieldsValid = parseSigned(
                    fields[field++], INT32_MIN, INT32_MAX, signedValue);
                output = static_cast<int32_t>(signedValue);
            };
            for (int32_t &value : evidence.cameraProjectionViewport) {
                parseSigned32(value);
            }
            for (int32_t &value : evidence.cameraProjectionScissor) {
                parseSigned32(value);
            }
            spatialFieldsValid = spatialFieldsValid && parseUnsigned(
                fields[field++], 4096u, unsignedValue);
            evidence.cameraProjectionPrimitiveDraws =
                static_cast<uint32_t>(unsignedValue);
            for (int32_t &value : evidence.cameraBoundsPixelMilli) {
                parseSigned32(value);
            }
            for (auto &landmark : evidence.cameraLandmarkPixelMilli) {
                for (int32_t &value : landmark) parseSigned32(value);
            }
            for (int32_t &value :
                 evidence.cameraLandmarkDepthMillionths) {
                parseSigned32(value);
            }
            for (uint32_t &value : evidence.cameraLandmarkClipFlags) {
                spatialFieldsValid = spatialFieldsValid && parseUnsigned(
                    fields[field++], 0x7Fu, unsignedValue);
                value = static_cast<uint32_t>(unsignedValue);
            }
            spatialFieldsValid = spatialFieldsValid && parseUnsigned(
                fields[field++], 0x7Fu, unsignedValue);
            evidence.cameraBoundsClipFlags =
                static_cast<uint32_t>(unsignedValue);
            if (!spatialFieldsValid || field != 163u) {
                error = "test evidence camera or anatomy fields are invalid";
                return false;
            }
        }
        const std::string checksum           = fields.back();
        fields.pop_back();
        if (!evidenceValid(evidence, error) ||
            recordDigest(fields) != checksum ||
            (!packages.count(evidence.packageId) &&
             packages.size() == kMaximumPackages) ||
            (!parsed.records.empty() &&
             !keyComesBefore(parsed.records.back(), evidence))) {
            if (error.empty()) {
                error = "test evidence checksum, key, or order is invalid";
            }
            return false;
        }
        packages.insert(evidence.packageId);
        parsed.records.push_back(std::move(evidence));
        begin = end + 1u;
    }
    if (begin != text.size() ||
        inventoryDigest(header, countText, text.substr(bodyBegin)) !=
            inventoryChecksum) {
        error = begin != text.size()
                    ? "test evidence inventory has trailing data"
                    : "test evidence inventory checksum is invalid";
        return false;
    }
    output = std::move(parsed);
    error.clear();
    return true;
}

bool serialize(const Inventory &inventory, std::string &output, std::string &error) {
    error.clear();
    if (inventory.records.size() > kMaximumRecords) {
        error = "test evidence inventory exceeds its record bound";
        return false;
    }
    Inventory ordered = inventory;
    std::sort(ordered.records.begin(), ordered.records.end(), keyComesBefore);
    std::string           body;
    std::set<std::string> packages;
    for (size_t index = 0u; index < ordered.records.size(); ++index) {
        const Evidence &evidence = ordered.records[index];
        if (!evidenceValid(evidence, error) ||
            (!packages.count(evidence.packageId) &&
             packages.size() == kMaximumPackages) ||
            (index != 0u && sameKey(ordered.records[index - 1u], evidence))) {
            if (error.empty()) {
                error = "test evidence inventory has duplicate keys or too many packages";
            }
            return false;
        }
        packages.insert(evidence.packageId);
        const std::vector<std::string> fields = recordFields(evidence);
        for (const std::string &field : fields) {
            body += field;
            body.push_back('\t');
        }
        body += recordDigest(fields);
        body.push_back('\n');
        if (body.size() > kMaximumSerializedBytes) {
            error = "serialized test evidence exceeds its byte bound";
            return false;
        }
    }
    const std::string count  = std::to_string(ordered.records.size());
    std::string       result = std::string(kHeaderV5) + "\t" + count + "\t" +
                               inventoryDigest(kHeaderV5, count, body) +
                               "\n" + body;
    if (result.size() > kMaximumSerializedBytes) {
        error = "serialized test evidence exceeds its byte bound";
        return false;
    }
    output = std::move(result);
    error.clear();
    return true;
}

LoadResult load(const MdkrTextStateStorage &storage, Inventory &output, std::string &error) {
    if (storage.read == nullptr) {
        error = "test evidence storage has no read callback";
        return LoadResult::IoError;
    }
    std::string text(kMaximumSerializedBytes + 1u, '\0');
    size_t      length = 0u;
    const int   result = storage.read(storage.context, text.data(), text.size(), &length);
    if (result == 0) {
        output = Inventory{};
        error.clear();
        return LoadResult::Missing;
    }
    if (result < 0 || length > kMaximumSerializedBytes) {
        error = "test evidence inventory could not be read within its byte bound";
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
            error = "test evidence storage has no write callback";
        }
        return false;
    }
    if (storage.write(storage.context, text.data(), text.size()) != 1) {
        error = "test evidence inventory could not be atomically replaced";
        return false;
    }
    error.clear();
    return true;
}

bool upsert(Inventory &inventory, Evidence evidence, std::string &error) {
    if (!evidenceValid(evidence, error)) return false;
    Evidence *existing = find(inventory, evidence.packageId,
                              evidence.context, evidence.players,
                              evidence.kind);
    if (existing == nullptr) {
        if (inventory.records.size() == kMaximumRecords) {
            error = "test evidence inventory is full";
            return false;
        }
        const bool packageExists = std::any_of(
            inventory.records.begin(),
            inventory.records.end(),
            [&](const Evidence &record) {
                return record.packageId == evidence.packageId;
            });
        if (!packageExists) {
            std::set<std::string> packages;
            for (const Evidence &record : inventory.records) {
                packages.insert(record.packageId);
            }
            if (packages.size() == kMaximumPackages) {
                error = "test evidence package inventory is full";
                return false;
            }
        }
        inventory.records.push_back(std::move(evidence));
    } else {
        *existing = std::move(evidence);
    }
    error.clear();
    return true;
}

bool erase(Inventory &inventory, const std::string &packageId,
           uint32_t context, uint32_t players, Kind kind) {
    const auto found = std::find_if(
        inventory.records.begin(),
        inventory.records.end(),
        [&](const Evidence &evidence) {
            return evidence.packageId == packageId &&
                   evidence.context == context &&
                   evidence.players == players && evidence.kind == kind;
        });
    if (found == inventory.records.end()) return false;
    inventory.records.erase(found);
    return true;
}

size_t erasePackage(Inventory &inventory, const std::string &packageId) {
    const size_t before = inventory.records.size();
    inventory.records.erase(
        std::remove_if(
            inventory.records.begin(),
            inventory.records.end(),
            [&](const Evidence &evidence) {
                return evidence.packageId == packageId;
            }),
        inventory.records.end());
    return before - inventory.records.size();
}

const Evidence *find(const Inventory &inventory,
                     const std::string &packageId, uint32_t context,
                     uint32_t players, Kind kind) {
    const auto found = std::find_if(
        inventory.records.begin(),
        inventory.records.end(),
        [&](const Evidence &evidence) {
            return evidence.packageId == packageId &&
                   evidence.context == context &&
                   evidence.players == players && evidence.kind == kind;
        });
    return found == inventory.records.end() ? nullptr : &*found;
}

Evidence *find(Inventory &inventory, const std::string &packageId,
               uint32_t context, uint32_t players, Kind kind) {
    const auto found = std::find_if(
        inventory.records.begin(),
        inventory.records.end(),
        [&](const Evidence &evidence) {
            return evidence.packageId == packageId &&
                   evidence.context == context &&
                   evidence.players == players && evidence.kind == kind;
        });
    return found == inventory.records.end() ? nullptr : &*found;
}

} // namespace CharacterTestEvidenceStore
