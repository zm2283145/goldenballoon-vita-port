#include "character_revision_index.h"

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
    const std::string a(64u, 'a');
    const std::string b(64u, 'b');
    CharacterRevisionIndex::Inventory inventory;
    const std::string valid =
        "mdkr-character-revisions-v1\t3\t2\n" +
        a + "\t1\t0\t1787760000\n" +
        b + "\t0\t0\t0\n";
    expect(CharacterRevisionIndex::parse(valid, inventory),
           "valid bounded revision index parses");
    expect(inventory.total == 3u && inventory.rows.size() == 2u &&
               inventory.rows[0].current && !inventory.rows[0].enabled &&
               inventory.rows[0].installedUnix == 1787760000u &&
               inventory.rows[1].sourceSha256 == b,
           "revision fields retain exact values");
    const CharacterRevisionIndex::Inventory before = inventory;
    expect(!CharacterRevisionIndex::parse(
               "mdkr-character-revisions-v1\t1\t1\n" +
                   a + "\t0\t1\t0\n",
               inventory) &&
               inventory.rows[0].sourceSha256 == before.rows[0].sourceSha256,
           "missing current row fails without changing output");
    expect(!CharacterRevisionIndex::parse(
               "mdkr-character-revisions-v1\t1\t1\n" +
                   std::string(64u, 'A') + "\t1\t1\t0\n",
               inventory),
           "uppercase digest is rejected");
    expect(!CharacterRevisionIndex::parse(
               "mdkr-character-revisions-v1\t1\t257\n", inventory),
           "published row count cannot exceed the UI bound");
    expect(!CharacterRevisionIndex::parse(valid + "trailing", inventory),
           "trailing data is rejected");
    if (failures != 0) return 1;
    std::puts("character revision index passed");
    return 0;
}
