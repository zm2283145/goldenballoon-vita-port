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
    source.testViewPitchDegrees = 90;
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
    source.disabledSemanticMask = 1u << 11u;
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
    source.portraitSourceRecord.kind =
        CharacterPortraitImport::SourceKind::ExactRenderer;
    source.portraitSourceRecord.sha256 = std::string(64u, 'c');
    source.portraitSourceRecord.width = 1280u;
    source.portraitSourceRecord.height = 720u;
    source.portraitSourceRecord.recipe.cropX = 420u;
    source.portraitSourceRecord.recipe.cropY = 80u;
    source.portraitSourceRecord.recipe.cropSize = 600u;
    source.portraitSourceRecord.recipe.edgeMatteTolerance = 12u;
    source.portraitSourceRecord.recipe.sampling =
        CharacterPortraitImport::Sampling::Area;
    source.portraitSourceRecord.recipe.background =
        CharacterPortraitImport::Background::Charcoal;
    source.portraitSourceRecord.subjectMask.enabled = true;
    source.portraitSourceRecord.subjectMask.alpha[0] = 0u;
    source.portraitSourceRecord.subjectMask.alpha[799] = 96u;
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
               parsed.testViewPitchDegrees == 90 &&
               parsed.testLighting ==
                   MDKR_WORKSHOP_PREVIEW_LIGHTING_BACKLIT &&
               parsed.reviewedContexts == 3u && parsed.scale == 1.25f &&
               parsed.offset[1] == -12.5f &&
               parsed.contexts[1].contacts[0][0] == -0.25f &&
               parsed.rigReviewed &&
               parsed.animationIntentPresent &&
               parsed.disabledSemanticMask == (1u << 11u) &&
               parsed.roles[15].node == 30u &&
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
               parsed.portraitSourceRecord.kind ==
                   CharacterPortraitImport::SourceKind::ExactRenderer &&
               parsed.portraitSourceRecord.sha256 ==
                   source.portraitSourceRecord.sha256 &&
               parsed.portraitSourceRecord.width == 1280u &&
               parsed.portraitSourceRecord.height == 720u &&
               parsed.portraitSourceRecord.recipe.cropX == 420u &&
               parsed.portraitSourceRecord.recipe.cropY == 80u &&
               parsed.portraitSourceRecord.recipe.cropSize == 600u &&
               parsed.portraitSourceRecord.recipe.edgeMatteTolerance == 12u &&
               parsed.portraitSourceRecord.recipe.background ==
                   CharacterPortraitImport::Background::Charcoal &&
               parsed.portraitSourceRecord.subjectMask.enabled &&
               parsed.portraitSourceRecord.subjectMask.alpha[0] == 0u &&
               parsed.portraitSourceRecord.subjectMask.alpha[799] == 96u &&
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
    constexpr size_t subjectMaskTailBytes =
        4u + CharacterPortraitImport::kSubjectMaskPixels;
    constexpr size_t sourceRecordTailBytes = 9u * 4u + 64u;
    constexpr size_t animationIntentTailBytes = 4u;
    std::string versionEight = encoded.substr(
        0u, encoded.size() - animationIntentTailBytes);
    writeU32(versionEight, 4u, 8u);
    writeU32(versionEight, 8u,
             static_cast<uint32_t>(versionEight.size()));
    expect(decode(versionEight, parsed, error) &&
               !parsed.animationIntentPresent &&
               parsed.disabledSemanticMask == 0u &&
               parsed.testViewPitchDegrees == source.testViewPitchDegrees,
           "version-eight drafts migrate with active animation mappings");
    std::string versionSeven = encoded.substr(
        0u, encoded.size() - animationIntentTailBytes);
    writeU32(versionSeven, 4u, 7u);
    writeU32(versionSeven, 8u,
             static_cast<uint32_t>(versionSeven.size()));
    /* Version seven stored pitch as unsigned degrees plus 45. Give the legacy
     * fixture an in-range nonzero value while version eight proves exact top. */
    writeU32(versionSeven,
             versionSeven.size() - subjectMaskTailBytes -
                 sourceRecordTailBytes - 8u,
             27u + 45u);
    Snapshot versionSevenParsed;
    expect(decode(versionSeven, versionSevenParsed, error) &&
               versionSevenParsed.testViewPitchDegrees == 27 &&
               versionSevenParsed.portraitSourceRecord.subjectMask.enabled,
           "version-seven drafts retain legacy pitch encoding and subject mask");
    std::string versionSix = versionSeven.substr(
        0u, versionSeven.size() - subjectMaskTailBytes);
    writeU32(versionSix, 4u, 6u);
    writeU32(versionSix, 8u,
             static_cast<uint32_t>(versionSix.size()));
    Snapshot versionSixParsed;
    expect(decode(versionSix, versionSixParsed, error) &&
               versionSixParsed.portraitSourceRecord.kind ==
                   source.portraitSourceRecord.kind &&
               !versionSixParsed.portraitSourceRecord.subjectMask.enabled &&
               versionSixParsed.portraitSourceRecord.subjectMask.alpha[0] ==
                   255u,
           "version-six drafts retain source recipes and gain an all-keep subject mask");

    std::string versionFive = versionSix.substr(
        0u, versionSix.size() - sourceRecordTailBytes);
    writeU32(versionFive, 4u, 5u);
    writeU32(versionFive, 8u,
             static_cast<uint32_t>(versionFive.size()));
    Snapshot versionFiveParsed;
    expect(decode(versionFive, versionFiveParsed, error) &&
               versionFiveParsed.testViewYawDegrees ==
                   source.testViewYawDegrees &&
               versionFiveParsed.testViewPitchDegrees == 27 &&
               versionFiveParsed.portraitSourceRecord.kind ==
                   CharacterPortraitImport::SourceKind::Canvas,
           "version-five drafts retain inspection state and gain a safe canvas source record");

    std::string versionFour = versionFive.substr(
        0u, versionFive.size() - 12u);
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

    std::string versionThree = versionFive.substr(
        0u, versionFive.size() - 20u);
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
    std::string versionTwo = versionFive.substr(
        0u, versionFive.size() - styleTailBytes);
    writeU32(versionTwo, 4u, 2u);
    writeU32(versionTwo, 8u, static_cast<uint32_t>(versionTwo.size()));
    Snapshot versionTwoParsed;
    expect(decode(versionTwo, versionTwoParsed, error) &&
               versionTwoParsed.displayName == source.displayName &&
               versionTwoParsed.portraitStyleSource == source.portrait &&
               CharacterPortraitStudio::validRecipe(
                   versionTwoParsed.portraitRecipe),
           "version-two drafts retain names and gain a safe style baseline");

    std::string legacy = versionFive.substr(
        0u, versionFive.size() - styleTailBytes - identityTailBytes);
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
    std::string badMaskEnabled = encoded;
    writeU32(badMaskEnabled,
             badMaskEnabled.size() - animationIntentTailBytes -
                 subjectMaskTailBytes,
             2u);
    expect(!decode(badMaskEnabled, parsed, error),
           "subject-mask enabled state is strictly bounded");
    std::string badReserved = encoded;
    badReserved[27] = 1;
    expect(!decode(badReserved, parsed, error),
           "nonzero reserved header byte is rejected");

    Snapshot hostile = source;
    hostile.enabledVehicleMask = 2u;
    expect(!encode(hostile, encoded, error),
           "enabled vehicles cannot exceed package compatibility");
    hostile = source;
    hostile.disabledSemanticMask = 1u;
    expect(!encode(hostile, encoded, error),
           "fallback cannot be disabled in a persisted animation decision");
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
    hostile.testViewPitchDegrees = -91;
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
    hostile.portraitSourceRecord.recipe.cropSize = 721u;
    expect(!encode(hostile, encoded, error),
           "portrait source crops cannot exceed their recorded source image");
    hostile = source;
    hostile.portraitSourceRecord.sha256[0] = 'A';
    expect(!encode(hostile, encoded, error),
           "portrait source records require canonical content digests");
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
