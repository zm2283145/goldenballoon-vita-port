#ifndef MDKR_APP_CHARACTER_REVISION_INDEX_H
#define MDKR_APP_CHARACTER_REVISION_INDEX_H

#include <cstdint>
#include <string>
#include <vector>

namespace CharacterRevisionIndex {

constexpr unsigned kMaximumRows = 256u;

struct Row {
    std::string sourceSha256;
    bool current = false;
    bool enabled = false;
    uint64_t installedUnix = 0u;
};

struct Inventory {
    unsigned total = 0u;
    std::vector<Row> rows;
};

// Parse the launcher's fixed-field, bounded helper protocol. Output is changed
// only on complete success and at least one row must identify the current
// source, so malformed helper output can never enable a restore action.
bool parse(const std::string &text, Inventory &output);

}  // namespace CharacterRevisionIndex

#endif  // MDKR_APP_CHARACTER_REVISION_INDEX_H
