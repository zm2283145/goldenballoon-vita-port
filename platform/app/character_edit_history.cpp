#include "character_edit_history.h"

#include <utility>

namespace {

size_t stackBytes(const std::vector<std::string> &stack) {
    size_t total = 0u;
    for (const std::string &snapshot : stack) total += snapshot.size();
    return total;
}

size_t trackBytes(const CharacterEditHistory::Track &track) {
    return stackBytes(track.undo) + stackBytes(track.redo);
}

void trimOldest(CharacterEditHistory::Track &track) {
    while ((track.undo.size() + track.redo.size() >
                CharacterEditHistory::kMaximumEntries ||
            trackBytes(track) > CharacterEditHistory::kMaximumTrackBytes) &&
           (!track.undo.empty() || !track.redo.empty())) {
        if (!track.undo.empty()) {
            track.undo.erase(track.undo.begin());
        } else {
            track.redo.erase(track.redo.begin());
        }
    }
}

bool snapshotValid(const std::string &snapshot) {
    return !snapshot.empty() &&
        snapshot.size() <= CharacterEditHistory::kMaximumSnapshotBytes;
}

void push(std::vector<std::string> &stack, std::string snapshot) {
    if (!stack.empty() && stack.back() == snapshot) return;
    stack.push_back(std::move(snapshot));
}

}  // namespace

namespace CharacterEditHistory {

bool observe(Track &track, const std::string &before,
             const std::string &after, bool interactionActive) {
    if (!snapshotValid(before) || !snapshotValid(after)) return false;
    if (before == after) {
        if (!interactionActive) track.coalescing = false;
        return true;
    }
    if (!track.coalescing) {
        push(track.undo, before);
        track.redo.clear();
    }
    track.coalescing = interactionActive;
    trimOldest(track);
    return true;
}

void endGesture(Track &track) {
    track.coalescing = false;
}

void clear(Track &track) {
    track = Track{};
}

bool canUndo(const Track &track) {
    return !track.undo.empty();
}

bool canRedo(const Track &track) {
    return !track.redo.empty();
}

bool undoTarget(const Track &track, std::string &target) {
    if (track.undo.empty()) return false;
    target = track.undo.back();
    return true;
}

bool redoTarget(const Track &track, std::string &target) {
    if (track.redo.empty()) return false;
    target = track.redo.back();
    return true;
}

bool commitUndo(Track &track, const std::string &current) {
    if (track.undo.empty() || !snapshotValid(current)) return false;
    track.undo.pop_back();
    push(track.redo, current);
    track.coalescing = false;
    trimOldest(track);
    return true;
}

bool commitRedo(Track &track, const std::string &current) {
    if (track.redo.empty() || !snapshotValid(current)) return false;
    track.redo.pop_back();
    push(track.undo, current);
    track.coalescing = false;
    trimOldest(track);
    return true;
}

}  // namespace CharacterEditHistory
