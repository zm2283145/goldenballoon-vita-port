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
