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
    }
    source.portraitSourcePath = "/tmp/portrait-\xC2\xA9.png";

    std::string encoded;
    std::string error;
    expect(encode(source, encoded, error), "valid full snapshot encodes");
    Snapshot parsed;
    expect(decode(encoded, parsed, error), "valid full snapshot decodes");
    expect(parsed.donor == 7u && parsed.packageVehicleMask == 5u &&
               parsed.enabledVehicleMask == 1u &&
               parsed.assemblyPlayers == 3 && parsed.testPlayers == 4 &&
               parsed.reviewedContexts == 3u && parsed.scale == 1.25f &&
               parsed.offset[1] == -12.5f &&
               parsed.contexts[1].contacts[0][0] == -0.25f &&
               parsed.rigReviewed && parsed.roles[15].node == 30u &&
               parsed.roles[1].inferred &&
               parsed.portrait == source.portrait &&
               parsed.portraitSourcePath == source.portraitSourcePath,
           "all authoring surfaces survive the snapshot round trip");
    std::string second;
    expect(encode(parsed, second, error) && encoded == second,
           "snapshot encoding is deterministic");

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

    if (failures != 0) return 1;
    std::puts("character draft snapshot passed");
    return 0;
}
