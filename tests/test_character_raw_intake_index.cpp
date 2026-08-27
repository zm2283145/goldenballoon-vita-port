#include "character_raw_intake_index.h"

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
}  // namespace

int main() {
    const std::string digest(64u, 'a');
    const std::string valid =
        "mdkr-character-glb-intake-v1\t" + digest +
        "\t3000\t2000\t2\t3\t1\t64\t1.75\t2\t3\n"
        "defaults\t69646c65\t68697073\t68656164\n"
        "clip\t69646c65\n"
        "clip\t6472697665\n"
        "node\t68697073\n"
        "node\t68656164\n"
        "node\t68616e642e6c656674\n";
    CharacterRawIntakeIndex::Inventory inventory;
    expect(CharacterRawIntakeIndex::parse(valid, inventory),
           "valid raw intake index parses");
    expect(inventory.modelSha256 == digest && inventory.vertices == 3000u &&
               inventory.sourceHeightM == 1.75 &&
               inventory.fallback == "idle" && inventory.seat == "hips" &&
               inventory.head == "head" && inventory.clips.size() == 2u &&
               inventory.nodes.size() == 3u,
           "raw intake values remain exact");
    const std::string bindOnly =
        "mdkr-character-glb-intake-v1\t" + digest +
        "\t3000\t2000\t2\t3\t1\t64\t1.75\t1\t2\n"
        "defaults\t2462696e64\t68697073\t68656164\n"
        "clip\t2462696e64\n"
        "node\t68697073\n"
        "node\t68656164\n";
    CharacterRawIntakeIndex::Inventory bindInventory;
    expect(CharacterRawIntakeIndex::parse(bindOnly, bindInventory) &&
               bindInventory.fallback == "$bind" &&
               bindInventory.clips.size() == 1u,
           "animationless intake preserves the explicit bind fallback token");
    const std::string detailed =
        "mdkr-character-glb-intake-v2\t" + digest +
        "\t3000\t2000\t2\t3\t1\t64\t0.0195\t1\t2"
        "\t-126\t-1\t-23\t126\t194\t45"
        "\t-0.0126\t-0.0001\t-0.0023\t0.0126\t0.0194\t0.0045\n"
        "defaults\t2462696e64\t68697073\t68656164\n"
        "clip\t2462696e64\n"
        "node\t68697073\n"
        "node\t68656164\n";
    CharacterRawIntakeIndex::Inventory detailedInventory;
    expect(CharacterRawIntakeIndex::parse(detailed, detailedInventory) &&
               detailedInventory.detailedBounds &&
               detailedInventory.meshLocalMinimum[1] == -1.0 &&
               detailedInventory.meshLocalMaximum[1] == 194.0 &&
               detailedInventory.sceneWorldMinimum[1] == -0.0001 &&
               detailedInventory.sceneWorldMaximum[1] == 0.0194,
           "v2 intake retains mesh-local and scene-world transform evidence");
    std::string inconsistentDetailed = detailed;
    inconsistentDetailed.replace(
        inconsistentDetailed.find("\t0.0194\t"), 8u, "\t0.0204\t");
    expect(!CharacterRawIntakeIndex::parse(
               inconsistentDetailed, detailedInventory),
           "v2 intake refuses a source height that contradicts its bounds");
    std::string zUpDetailed = detailed;
    const std::string localBounds = "-126\t-1\t-23\t126\t194\t45";
    zUpDetailed.replace(zUpDetailed.find(localBounds), localBounds.size(),
                        "0\t0\t0\t100\t0\t100");
    expect(CharacterRawIntakeIndex::parse(zUpDetailed, detailedInventory),
           "v2 intake accepts a transformed Z-up mesh with zero local-Y extent");
    const CharacterRawIntakeIndex::Inventory before = inventory;
    expect(!CharacterRawIntakeIndex::parse(valid + "trailing", inventory) &&
               inventory.modelSha256 == before.modelSha256,
           "trailing bytes fail without changing output");
    expect(!CharacterRawIntakeIndex::parse(
               valid.substr(0u, valid.rfind("node\t")) +
                   "node\t68656164\n",
               inventory),
           "duplicate names cannot create ambiguous mappings");
    expect(!CharacterRawIntakeIndex::parse(
               "mdkr-character-glb-intake-v1\t" + digest +
                   "\t1\t1\t1\t1\t1\t1\tnan\t1\t1\n",
               inventory),
           "non-finite height is rejected");
    std::string nonCanonicalHeight = valid;
    nonCanonicalHeight.replace(nonCanonicalHeight.find("1.75"), 4u, " 1.75");
    expect(!CharacterRawIntakeIndex::parse(nonCanonicalHeight, inventory),
           "non-canonical height text is rejected");
    std::string missingDefault = valid;
    missingDefault.replace(missingDefault.find("69646c65"), 8u,
                           "6d697373696e67");
    expect(!CharacterRawIntakeIndex::parse(missingDefault, inventory),
           "defaults must name an exact published choice");
    std::string invisibleName = valid;
    invisibleName.replace(invisibleName.find("69646c65"), 8u,
                          "e2808b");
    expect(!CharacterRawIntakeIndex::parse(invisibleName, inventory),
           "invisible authoring names are rejected");
    std::string excessiveChoices = valid;
    const size_t clipCount = excessiveChoices.find("\t2\t3\n");
    excessiveChoices.replace(clipCount, 5u, "\t257\t3\n");
    expect(!CharacterRawIntakeIndex::parse(excessiveChoices, inventory),
           "choice counts are bounded before allocation");
    if (failures != 0) return 1;
    std::puts("character raw intake index passed");
    return 0;
}
