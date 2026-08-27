#ifndef MDKR_APP_CHARACTER_FAILURE_INDEX_H
#define MDKR_APP_CHARACTER_FAILURE_INDEX_H

#include <cstdint>
#include <string>
#include <vector>

namespace CharacterFailureIndex {

constexpr unsigned kMaximumRows = 256u;

enum class RetryKind : uint8_t {
    RawInspect,
    PackageInspect,
    Convert,
};

struct Row {
    std::string recordId;
    uint64_t lastFailedUnix = 0u;
    uint32_t attempts = 0u;
    bool sourceAvailable = false;
    bool sourceChanged = false;
    bool reportAvailable = false;
    RetryKind retryKind = RetryKind::RawInspect;
    std::string sourcePath;
    std::string outputPath;
    std::string error;
};

struct Inventory {
    unsigned total = 0u;
    std::vector<Row> rows;
};

// Parses the bounded, hex-escaped recovery protocol transactionally. The
// source model is never present in this inventory—only local path, state,
// diagnostic text, and the opaque record identity needed for explicit actions.
bool parse(const std::string &text, Inventory &output);

}  // namespace CharacterFailureIndex

#endif  // MDKR_APP_CHARACTER_FAILURE_INDEX_H
