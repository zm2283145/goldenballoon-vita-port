#include "character_candidate_index.h"

#include <cstdio>
#include <string>

namespace {
int failures;
void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

std::string zeroFields(unsigned count) {
    std::string fields;
    while (count-- != 0u) fields += "\t0";
    return fields;
}
}  // namespace

int main() {
    const std::string digest(64u, 'a');
    const std::string fields =
        "org.example.hero\t4865726fc2ae\t" + digest + "\t" + digest +
        "\t9\t7\t1000\t500\t3\t2\t2\t4\t20\t1\t16\t8\t30\t400"
        "\t1\t2\t1\t16\t4096\t16384"
        "\t700\t300\t0\t0\t350\t150\t0\t0\t2\t1\t0\t0"
        "\t1\t43432d42592d342e30"
        "\t416e6120c2a9204578616d706c65"
        "\t68747470733a2f2f6578616d706c652e696e76616c69642f6865726f";
    const std::string valid = "mdkr-character-candidate-v2\n" + fields + "\n";
    CharacterCandidateIndex::Candidate candidate;
    expect(CharacterCandidateIndex::parse(valid, candidate),
           "valid candidate index parses");
    expect(candidate.id == "org.example.hero" &&
               candidate.displayName == "Hero®" && candidate.donor == 9u &&
               candidate.vehicleMask == 7u && candidate.vertices == 1000u &&
               candidate.identityPresent && candidate.rigMode == 2u &&
               candidate.rigReviewed && candidate.rigRoles == 16u &&
               candidate.animationChannels == 30u &&
               candidate.animationKeys == 400u &&
               candidate.decodedTextureBytes == 16384u &&
               candidate.lodVertices[0] == 700u &&
               candidate.lodTriangles[1] == 150u &&
               candidate.lodPrimitives[1] == 1u &&
               candidate.provenancePresent &&
               candidate.licenseSpdx == "CC-BY-4.0" &&
               candidate.attribution == "Ana \xC2\xA9 Example" &&
               candidate.sourceUrl == "https://example.invalid/hero",
           "candidate index retains exact comparison fields");
    const CharacterCandidateIndex::Candidate before = candidate;
    expect(!CharacterCandidateIndex::parse(
               "mdkr-character-candidate-v2\n" +
                   std::string("org.example.hero\tff\t") + digest + "\t" +
                   digest + "\t9\t7" + zeroFields(30u) + "\t0\t\t\t\n",
               candidate) && candidate.displayName == before.displayName,
           "invalid UTF-8 fails without changing output");
    expect(!CharacterCandidateIndex::parse(valid + "trailing", candidate),
           "trailing candidate data is rejected");
    expect(!CharacterCandidateIndex::parse(
               "mdkr-character-candidate-v2\n" +
                   std::string("org.example.hero\t4865726f\t") + digest + "\t" +
                   digest + "\t10\t7" + zeroFields(30u) + "\t0\t\t\t\n",
               candidate),
           "out-of-range donor is rejected");
    expect(!CharacterCandidateIndex::parse(
               valid.substr(0u, valid.find("\t1000\t") + 1u) +
                   "01000" + valid.substr(valid.find("\t1000\t") + 5u),
               candidate),
           "non-canonical leading-zero numbers are rejected");
    {
        std::string inconsistent = valid;
        const size_t rig = inconsistent.find("\t1\t2\t1\t16\t4096");
        expect(rig != std::string::npos, "rig fixture is present");
        inconsistent.replace(rig, 9u, "\t1\t0\t1\t16");
        expect(!CharacterCandidateIndex::parse(inconsistent, candidate),
               "inconsistent absent-rig review data is rejected");
    }
    {
        const size_t provenance = valid.rfind("\t1\t43432d42592d342e30");
        expect(provenance != std::string::npos,
               "provenance fixture is present");
        const std::string legacy = valid.substr(0u, provenance) +
            "\t0\t\t\t\n";
        CharacterCandidateIndex::Candidate legacyCandidate;
        expect(CharacterCandidateIndex::parse(legacy, legacyCandidate) &&
                   !legacyCandidate.provenancePresent &&
                   legacyCandidate.licenseSpdx.empty(),
               "legacy cache metadata absence is represented explicitly");
        expect(!CharacterCandidateIndex::parse(
                   valid.substr(0u, provenance) +
                       "\t0\t4343302d312e30\t\t\n", candidate),
               "absent provenance cannot carry hidden text");
        expect(!CharacterCandidateIndex::parse(
                   valid.substr(0u, provenance) +
                       "\t1\t4343300a\t41\t42\n", candidate),
               "control characters in provenance are rejected");
    }
    if (failures != 0) return 1;
    std::puts("character candidate index passed");
    return 0;
}
