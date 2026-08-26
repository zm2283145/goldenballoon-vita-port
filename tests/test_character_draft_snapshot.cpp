#include "character_draft_snapshot.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures;
void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void writeU32(std::string &payload, size_t offset, uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        payload[offset++] = static_cast<char>(value >> shift);
    }
}
}  // namespace

int main() {
    using namespace CharacterDraftSnapshot;
    Snapshot source;
    source.donor = 7u;
    source.packageVehicleMask = 5u;
    source.enabledVehicleMask = 1u;
    source.minimapRgb[0] = 17u;
    source.minimapRgb[1] = 34u;
    source.minimapRgb[2] = 51u;
    source.assemblyPlayers = 3;
    source.testPlayers = 4;
    source.testPose = 12u;
    source.testPosePhaseMilli = 875u;
    source.testViewYawDegrees = -135;
    source.testViewPitchDegrees = 27;
    source.testLighting = MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT;
    source.reviewedContexts = 3u;
    source.scale = 1.25f;
    source.offset[1] = -12.5f;
    source.rotation[1] = 180.0f;
    source.animationSpeed = 0.75f;
    source.lodBias = -1.0f;
    source.contexts[1].scale = 0.9f;
    source.contexts[1].offset[2] = 0.125f;
    source.contexts[1].contacts[0][0] = -0.25f;
    source.rigMode = 1u;
    source.rigReviewed = true;
    for (size_t role = 0u; role < kRoles; ++role) {
        source.roles[role].node = static_cast<uint32_t>(role * 2u);
        source.roles[role].inferred = (role & 1u) != 0u;
        source.roles[role].confidence = 0.5f +
            static_cast<float>(role) / 32.0f;
    }
    for (size_t index = 0u; index < source.portrait.size(); ++index) {
        source.portrait[index] = static_cast<uint8_t>(index * 31u);
        source.portraitStyleSource[index] = static_cast<uint8_t>(index * 17u);
    }
    source.portraitRecipe.zoomPercent = 175;
    source.portraitRecipe.panX = -7;
    source.portraitRecipe.panY = 9;
    source.portraitRecipe.paletteColors = 16u;
    source.portraitRecipe.ditherStrength = 0.75f;
    source.portraitRecipe.outlinePixels = 2;
    source.portraitRecipe.alphaThreshold = 31;
    source.portraitRecipe.sampling = CharacterPortraitStudio::Sampling::Crisp;
    source.portraitRecipe.fillPinholes = false;
    source.portraitSourcePath = "/tmp/portrait-\xC2\xA9.png";
    source.displayName = "Dixie Kong";
    source.shortName = "Dixie";
    source.narrationName = "Dixie Kong";
    source.sortLabel = "Kong, Dixie";

    std::string encoded;
    std::string error;
    expect(encode(source, encoded, error), "valid full snapshot encodes");
    Snapshot parsed;
    expect(decode(encoded, parsed, error), "valid full snapshot decodes");
    expect(parsed.donor == 7u && parsed.packageVehicleMask == 5u &&
               parsed.enabledVehicleMask == 1u &&
               parsed.assemblyPlayers == 3 && parsed.testPlayers == 4 &&
               parsed.testPose == 12u &&
               parsed.testPosePhaseMilli == 875u &&
               parsed.testViewYawDegrees == -135 &&
               parsed.testViewPitchDegrees == 27 &&
               parsed.testLighting ==
                   MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT &&
               parsed.reviewedContexts == 3u && parsed.scale == 1.25f &&
               parsed.offset[1] == -12.5f &&
               parsed.contexts[1].contacts[0][0] == -0.25f &&
               parsed.rigReviewed && parsed.roles[15].node == 30u &&
               parsed.roles[1].inferred &&
               parsed.portrait == source.portrait &&
               parsed.portraitStyleSource == source.portraitStyleSource &&
               parsed.portraitRecipe.zoomPercent == 175 &&
               parsed.portraitRecipe.panX == -7 &&
               parsed.portraitRecipe.panY == 9 &&
               parsed.portraitRecipe.paletteColors == 16u &&
               parsed.portraitRecipe.ditherStrength == 0.75f &&
               parsed.portraitRecipe.outlinePixels == 2 &&
               parsed.portraitRecipe.alphaThreshold == 31 &&
               parsed.portraitRecipe.sampling ==
                   CharacterPortraitStudio::Sampling::Crisp &&
               !parsed.portraitRecipe.fillPinholes &&
               parsed.portraitSourcePath == source.portraitSourcePath &&
               parsed.displayName == source.displayName &&
               parsed.shortName == source.shortName &&
               parsed.narrationName == source.narrationName &&
               parsed.sortLabel == source.sortLabel,
           "all authoring surfaces survive the snapshot round trip");
    std::string second;
    expect(encode(parsed, second, error) && encoded == second,
           "snapshot encoding is deterministic");

    const size_t identityTailBytes = 16u + source.displayName.size() +
        source.shortName.size() + source.narrationName.size() +
        source.sortLabel.size();
    std::string versionFour = encoded.substr(0u, encoded.size() - 12u);
    writeU32(versionFour, 4u, 4u);
    writeU32(versionFour, 8u,
             static_cast<uint32_t>(versionFour.size()));
    Snapshot versionFourParsed;
    expect(decode(versionFour, versionFourParsed, error) &&
               versionFourParsed.testPose == source.testPose &&
               versionFourParsed.testPosePhaseMilli ==
                   source.testPosePhaseMilli &&
               versionFourParsed.testViewYawDegrees == 0 &&
               versionFourParsed.testViewPitchDegrees == 0 &&
               versionFourParsed.testLighting ==
                   MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL,
           "version-four drafts retain pose state and gain safe visual defaults");

    std::string versionThree = encoded.substr(0u, encoded.size() - 20u);
    writeU32(versionThree, 4u, 3u);
    writeU32(versionThree, 8u,
             static_cast<uint32_t>(versionThree.size()));
    Snapshot versionThreeParsed;
    expect(decode(versionThree, versionThreeParsed, error) &&
               versionThreeParsed.portraitStyleSource ==
                   source.portraitStyleSource &&
               versionThreeParsed.testPose ==
                   MDKR_MODERN_CHARACTER_INSPECTION_DEFAULT_POSE &&
               versionThreeParsed.testPosePhaseMilli == 500u &&
               versionThreeParsed.testViewYawDegrees == 0 &&
               versionThreeParsed.testViewPitchDegrees == 0 &&
               versionThreeParsed.testLighting ==
                   MDKR_WORKSHOP_PREVIEW_LIGHTING_NEUTRAL,
           "version-three drafts gain safe pose-inspection defaults");

    const size_t styleTailBytes =
        CharacterPortraitStudio::kBytes + 9u * 4u + 8u + 12u;
    std::string versionTwo = encoded.substr(
        0u, encoded.size() - styleTailBytes);
    writeU32(versionTwo, 4u, 2u);
    writeU32(versionTwo, 8u, static_cast<uint32_t>(versionTwo.size()));
    Snapshot versionTwoParsed;
    expect(decode(versionTwo, versionTwoParsed, error) &&
               versionTwoParsed.displayName == source.displayName &&
               versionTwoParsed.portraitStyleSource == source.portrait &&
               CharacterPortraitStudio::validRecipe(
                   versionTwoParsed.portraitRecipe),
           "version-two drafts retain names and gain a safe style baseline");

    std::string legacy = encoded.substr(
        0u, encoded.size() - styleTailBytes - identityTailBytes);
    writeU32(legacy, 4u, 1u);
    writeU32(legacy, 8u, static_cast<uint32_t>(legacy.size()));
    Snapshot legacyParsed;
    expect(decode(legacy, legacyParsed, error) &&
               legacyParsed.displayName.empty() &&
               legacyParsed.shortName.empty() &&
               legacyParsed.narrationName.empty() &&
               legacyParsed.sortLabel.empty() &&
               legacyParsed.portrait == source.portrait &&
               legacyParsed.portraitStyleSource == source.portrait,
           "version-one drafts decode with legacy empty identity names");

    const Snapshot before = parsed;
    std::string truncated = encoded.substr(0u, encoded.size() - 1u);
    expect(!decode(truncated, parsed, error) && parsed.donor == before.donor,
           "truncation fails without changing the caller's snapshot");
    std::string trailing = encoded + "x";
    expect(!decode(trailing, parsed, error),
           "trailing payload bytes are rejected");
    std::string badReserved = encoded;
    badReserved[27] = 1;
    expect(!decode(badReserved, parsed, error),
           "nonzero reserved header byte is rejected");

    Snapshot hostile = source;
    hostile.enabledVehicleMask = 2u;
    expect(!encode(hostile, encoded, error),
           "enabled vehicles cannot exceed package compatibility");
    hostile = source;
    hostile.testPose =
        MDKR_MODERN_CHARACTER_INSPECTION_SEMANTIC_COUNT + 1u;
    expect(!encode(hostile, encoded, error),
           "unknown inspection poses are rejected from persisted state");
    hostile = source;
    hostile.testPosePhaseMilli = 1001u;
    expect(!encode(hostile, encoded, error),
           "out-of-range inspection phases are rejected from persisted state");
    hostile = source;
    hostile.testViewYawDegrees = 181;
    expect(!encode(hostile, encoded, error),
           "out-of-range inspection yaw is rejected from persisted state");
    hostile = source;
    hostile.testViewPitchDegrees = -46;
    expect(!encode(hostile, encoded, error),
           "out-of-range inspection pitch is rejected from persisted state");
    hostile = source;
    hostile.testLighting = MDKR_WORKSHOP_PREVIEW_LIGHTING_COUNT;
    expect(!encode(hostile, encoded, error),
           "unknown inspection lighting is rejected from persisted state");
    hostile = source;
    hostile.portraitRecipe.paletteColors = 17u;
    expect(!encode(hostile, encoded, error),
           "invalid portrait style recipes are rejected");
    hostile = source;
    hostile.contexts[0].offset[0] = INFINITY;
    expect(!encode(hostile, encoded, error),
           "non-finite fit values are rejected");
    hostile = source;
    hostile.roles[1].node = hostile.roles[0].node;
    expect(!encode(hostile, encoded, error),
           "one rig node cannot own two semantic roles");
    hostile = source;
    hostile.roles[0].rest[3] = 0.5f;
    expect(!encode(hostile, encoded, error),
           "non-normalized rig solver bases are rejected");
    hostile = source;
    hostile.portraitSourcePath = "line\nbreak.png";
    expect(!encode(hostile, encoded, error),
           "unsafe portrait paths are rejected from persisted UI state");
    hostile = source;
    hostile.portraitSourcePath = std::string("spoof-") +
        "\xE2\x81\xA6" + "path.png";
    expect(!encode(hostile, encoded, error),
           "bidirectional controls are rejected from persisted paths");
    hostile = source;
    hostile.sortLabel = std::string("Dixie") + "\xE2\x80\xAE" + "Kong";
    expect(!encode(hostile, encoded, error),
           "bidirectional controls are rejected from identity names");
    hostile = source;
    hostile.shortName.clear();
    expect(!encode(hostile, encoded, error),
           "identity name groups cannot be partially absent");
    hostile = source;
    hostile.displayName.clear();
    hostile.shortName.clear();
    hostile.narrationName.clear();
    hostile.sortLabel.clear();
    expect(!encode(hostile, encoded, error),
           "new snapshots cannot omit the complete identity name group");

    if (failures != 0) return 1;
    std::puts("character draft snapshot passed");
    return 0;
}
