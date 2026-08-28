#include "character_failure_index.h"

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

std::string hex(const std::string &text) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() * 2u);
    for (unsigned char byte : text) {
        result.push_back(digits[byte >> 4u]);
        result.push_back(digits[byte & 0x0Fu]);
    }
    return result;
}
}  // namespace

int main() {
    const std::string a(64u, 'a');
    const std::string b(64u, 'b');
    const std::string valid =
        "mdkr-character-import-failures-v1\t3\t2\n" +
        a + "\t1787760000\t2\t1\t0\t1\traw-inspect\t" +
        hex("/tmp/author.glb") + "\t-\t" + hex("Invalid accessor") + "\n" +
        b + "\t0\t1\t0\t0\t0\tpackage-inspect\t" +
        hex("C:\\model.mdkrchar") + "\t-\t" + hex("Source is missing") + "\n";
    CharacterFailureIndex::Inventory inventory;
    expect(CharacterFailureIndex::parse(valid, inventory),
           "valid bounded recovery inventory parses");
    expect(inventory.total == 3u && inventory.rows.size() == 2u &&
               inventory.rows[0].attempts == 2u &&
               inventory.rows[0].sourceAvailable &&
               inventory.rows[0].reportAvailable &&
               inventory.rows[0].sourcePath == "/tmp/author.glb" &&
               inventory.rows[1].retryKind ==
                   CharacterFailureIndex::RetryKind::PackageInspect,
           "recovery fields retain exact values");
    CharacterFailureIndex::Inventory adapterInventory;
    expect(CharacterFailureIndex::parse(
               "mdkr-character-import-failures-v1\t1\t1\n" +
                   a + "\t0\t1\t1\t0\t0\tadapter-inspect\t" +
                   hex("/tmp/artist.mdkrsource") + "\t-\t" +
                   hex("Model digest changed") + "\n",
               adapterInventory) &&
               adapterInventory.rows[0].retryKind ==
                   CharacterFailureIndex::RetryKind::AdapterInspect,
           "data-only adapter inspection recovery parses without output authority");
    const CharacterFailureIndex::Inventory before = inventory;
    expect(!CharacterFailureIndex::parse(
               "mdkr-character-import-failures-v1\t1\t1\n" +
                   a + "\t0\t0\t1\t0\t0\traw-inspect\t61\t-\t62\n",
               inventory) && inventory.rows.size() == before.rows.size(),
           "zero attempts fail without changing output");
    expect(!CharacterFailureIndex::parse(
               "mdkr-character-import-failures-v1\t2\t2\n" +
                   a + "\t0\t1\t1\t0\t0\traw-inspect\t61\t-\t62\n" +
                   a + "\t0\t1\t1\t0\t0\traw-inspect\t61\t-\t62\n",
               inventory),
           "duplicate record identity is rejected");
    expect(!CharacterFailureIndex::parse(
               "mdkr-character-import-failures-v1\t1\t1\n" +
                   std::string(64u, 'A') +
                   "\t0\t1\t1\t0\t0\traw-inspect\t61\t-\t62\n",
               inventory),
           "uppercase record identity is rejected");
    expect(!CharacterFailureIndex::parse(
               "mdkr-character-import-failures-v1\t1\t1\n" +
                   a + "\t0\t1\t1\t0\t0\traw-inspect\tc0af\t-\t62\n",
               inventory),
           "overlong UTF-8 path is rejected");
    expect(!CharacterFailureIndex::parse(
               "mdkr-character-import-failures-v1\t1\t1\n" +
                   a + "\t0\t1\t0\t1\t0\traw-inspect\t61\t-\t62\n",
               inventory),
           "missing source cannot also be marked changed");
    expect(!CharacterFailureIndex::parse(valid + "trailing", inventory),
           "trailing recovery data is rejected");
    if (failures != 0) return 1;
    std::puts("character failure index passed");
    return 0;
}
