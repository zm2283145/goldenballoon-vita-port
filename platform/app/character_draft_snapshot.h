#ifndef MDKR_APP_CHARACTER_DRAFT_SNAPSHOT_H
#define MDKR_APP_CHARACTER_DRAFT_SNAPSHOT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace CharacterDraftSnapshot {

constexpr size_t kPortraitBytes = 40u * 40u * 4u;
constexpr size_t kContexts = 4u;
constexpr size_t kContacts = 4u;
constexpr size_t kRoles = 16u;
constexpr uint32_t kNoNode = UINT32_MAX;

enum Flags : uint32_t {
    Profile = 1u << 0,
    Identity = 1u << 1,
    Rig = 1u << 2,
    Tuning = 1u << 3,
    All = Profile | Identity | Rig | Tuning,
};

struct Context {
    float scale = 1.0f;
    float offset[3] = {};
    float rotation[3] = {};
    float contacts[kContacts][3] = {};
};

struct RigRole {
    uint32_t node = kNoNode;
    bool inferred = false;
    float confidence = 1.0f;
    float rest[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float bend[3] = {};
};

struct Snapshot {
    uint32_t flags = All;
    uint32_t donor = 9u;
    uint32_t packageVehicleMask = 7u;
    uint8_t minimapRgb[3] = {220u, 72u, 144u};
    int assemblyPlayers = 4;
    int testPlayers = 1;
    uint32_t reviewedContexts = 0u;

    float scale = 1.0f;
    float offset[3] = {};
    float rotation[3] = {};
    float animationSpeed = 1.0f;
    float lodBias = 0.0f;
    uint32_t enabledVehicleMask = 7u;
    Context contexts[kContexts];

    uint32_t rigMode = 0u;
    bool rigReviewed = false;
    RigRole roles[kRoles];

    std::array<uint8_t, kPortraitBytes> portrait{};
    std::string portraitSourcePath;
};

bool encode(const Snapshot &snapshot, std::string &payload,
            std::string &error);
bool decode(const std::string &payload, Snapshot &snapshot,
            std::string &error);

}  // namespace CharacterDraftSnapshot

#endif  // MDKR_APP_CHARACTER_DRAFT_SNAPSHOT_H
