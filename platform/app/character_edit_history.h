#ifndef MDKR_APP_CHARACTER_EDIT_HISTORY_H
#define MDKR_APP_CHARACTER_EDIT_HISTORY_H

#include <cstddef>
#include <string>
#include <vector>

namespace CharacterEditHistory {

constexpr size_t kMaximumEntries = 32u;
constexpr size_t kMaximumSnapshotBytes = 256u * 1024u;
constexpr size_t kMaximumTrackBytes = 512u * 1024u;

struct Track {
    std::vector<std::string> undo;
    std::vector<std::string> redo;
    bool coalescing = false;
};

// Observe one rendered editor frame. A continuous active widget gesture is a
// single undo step; buttons and other discrete edits form their own steps.
bool observe(Track &track, const std::string &before,
             const std::string &after, bool interactionActive);

void endGesture(Track &track);
void clear(Track &track);

bool canUndo(const Track &track);
bool canRedo(const Track &track);
bool undoTarget(const Track &track, std::string &target);
bool redoTarget(const Track &track, std::string &target);

// Commit only after the caller successfully applied the previously peeked
// target. This keeps persistence failures from consuming history.
bool commitUndo(Track &track, const std::string &current);
bool commitRedo(Track &track, const std::string &current);

}  // namespace CharacterEditHistory

#endif  // MDKR_APP_CHARACTER_EDIT_HISTORY_H
