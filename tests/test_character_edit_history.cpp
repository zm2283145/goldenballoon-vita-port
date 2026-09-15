#include "character_edit_history.h"

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
    CharacterEditHistory::Track history;
    expect(CharacterEditHistory::observe(history, "A", "B", false) &&
               history.undo.size() == 1u && history.undo.back() == "A",
           "a discrete edit records its exact prior state");
    expect(CharacterEditHistory::observe(history, "B", "C", true) &&
               CharacterEditHistory::observe(history, "C", "D", true) &&
               history.undo.size() == 2u && history.undo.back() == "B",
           "one active gesture coalesces repeated frames");
    expect(CharacterEditHistory::observe(history, "D", "D", false),
           "an idle frame closes the active gesture");
    expect(CharacterEditHistory::observe(history, "D", "E", true) &&
               history.undo.size() == 3u && history.undo.back() == "D",
           "the next gesture receives an independent checkpoint");

    std::string target;
    expect(CharacterEditHistory::undoTarget(history, target) && target == "D",
           "undo peeks without consuming history");
    expect(CharacterEditHistory::commitUndo(history, "E") &&
               CharacterEditHistory::redoTarget(history, target) &&
               target == "E",
           "successful undo makes the exact current state redoable");
    expect(CharacterEditHistory::commitRedo(history, "D") &&
               CharacterEditHistory::undoTarget(history, target) &&
               target == "D",
           "redo restores the inverse undo checkpoint");

    expect(CharacterEditHistory::commitUndo(history, "E"),
           "second undo setup succeeds");
    expect(CharacterEditHistory::observe(history, "D", "new", false) &&
               !CharacterEditHistory::canRedo(history),
           "a divergent edit clears redo history");

    CharacterEditHistory::Track bounded;
    for (size_t index = 0u;
         index < CharacterEditHistory::kMaximumEntries + 12u; ++index) {
        const std::string before = "state-" + std::to_string(index);
        const std::string after = "state-" + std::to_string(index + 1u);
        expect(CharacterEditHistory::observe(bounded, before, after, false),
               "bounded history accepts a normal snapshot");
    }
    expect(bounded.undo.size() == CharacterEditHistory::kMaximumEntries,
           "history evicts the oldest entry at its count bound");
    expect(!CharacterEditHistory::observe(
               bounded,
               std::string(CharacterEditHistory::kMaximumSnapshotBytes + 1u,
                           'x'),
               "small", false),
           "oversized snapshots fail without entering history");
    expect(bounded.undo.size() == CharacterEditHistory::kMaximumEntries,
           "a refused snapshot leaves existing history intact");

    CharacterEditHistory::Track byteBounded;
    const size_t substantialSize =
        CharacterEditHistory::kMaximumTrackBytes / 3u;
    for (char marker : {'a', 'b', 'c', 'd'}) {
        const std::string before(substantialSize, marker);
        std::string after = before;
        after.back() = static_cast<char>(marker + 1);
        expect(CharacterEditHistory::observe(
                   byteBounded, before, after, false),
               "history accepts an individually bounded large snapshot");
    }
    size_t retainedBytes = 0u;
    for (const std::string &snapshot : byteBounded.undo) {
        retainedBytes += snapshot.size();
    }
    for (const std::string &snapshot : byteBounded.redo) {
        retainedBytes += snapshot.size();
    }
    expect(retainedBytes <= CharacterEditHistory::kMaximumTrackBytes,
           "history evicts oldest snapshots at its aggregate byte bound");
    expect(!byteBounded.undo.empty() &&
               byteBounded.undo.back().front() == 'd',
           "byte-bound eviction preserves the newest checkpoint");

    CharacterEditHistory::Track deferred;
    expect(CharacterEditHistory::observe(deferred, "old", "current", false),
           "deferred-commit setup succeeds");
    const CharacterEditHistory::Track beforeFailure = deferred;
    expect(!CharacterEditHistory::commitUndo(
               deferred,
               std::string(CharacterEditHistory::kMaximumSnapshotBytes + 1u,
                           'x')) &&
               deferred.undo == beforeFailure.undo &&
               deferred.redo == beforeFailure.redo,
           "a refused commit does not consume or rewrite history");

    if (failures != 0) return 1;
    std::puts("character edit history passed");
    return 0;
}
