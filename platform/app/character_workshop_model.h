// Deterministic navigation/readiness policy for the Custom Character Workshop.
//
// This file deliberately knows nothing about ImGui, AppConfig, filesystem
// state, or the renderer. Callers reduce their current package/draft/editor
// state to CharacterWorkshopFacts, then render the returned named rows. Keeping
// the policy pure makes "Ready to preview", "Ready to play", and the single
// suggested next action consistent at every UI size and input method.
#ifndef MDKR64_CHARACTER_WORKSHOP_MODEL_H
#define MDKR64_CHARACTER_WORKSHOP_MODEL_H

#include <array>
#include <cstddef>
#include <cstdint>

enum class CharacterWorkshopTab : uint8_t {
    Overview = 0,
    Identity,
    RigMotion,
    Vehicles,
    Performance,
    Test,
    Package,
    Count,
};

enum class CharacterWorkshopReadinessId : uint8_t {
    Identity = 0,
    Calibration,
    RigMotion,
    GameplayProfile,
    VehicleFit,
    Performance,
    Count,
};

enum class CharacterWorkshopReadinessStatus : uint8_t {
    Ready = 0,
    Review,
    Missing,
    Unavailable,
};

struct CharacterWorkshopFacts {
    bool     geometryAvailable      = false;
    bool     identityReady          = false;
    bool     normalized             = false;
    bool     anchorsReady           = false;
    bool     attachmentSocketsReady = false;
    bool     rigPresent             = false;
    bool     rigReviewed            = false;
    bool     motionReady            = false;
    bool     donorQualified         = false;
    bool     performanceMeasured    = false;
    bool     enabled                = false;

    // Bit zero is character select. Bits one through three are car,
    // hovercraft, and plane. A zero vehicle mask is invalid source state.
    uint32_t supportedVehicleMask = 0u;
    uint32_t reviewedContextMask  = 0u;
};

struct CharacterWorkshopReadinessRow {
    CharacterWorkshopReadinessId id =
        CharacterWorkshopReadinessId::Identity;
    CharacterWorkshopReadinessStatus status =
        CharacterWorkshopReadinessStatus::Missing;
    CharacterWorkshopTab actionTab = CharacterWorkshopTab::Overview;
};

struct CharacterWorkshopReadiness {
    std::array<CharacterWorkshopReadinessRow,
               static_cast<size_t>(CharacterWorkshopReadinessId::Count)>
                         rows{};
    unsigned             readyCount      = 0u;
    bool                 readyToPreview  = false;
    bool                 readyToPlay     = false;
    CharacterWorkshopTab nextActionTab   = CharacterWorkshopTab::Overview;
    const char          *nextActionLabel = "Inspect character";
};

CharacterWorkshopReadiness CharacterWorkshop_evaluate(
    const CharacterWorkshopFacts &facts);

const char          *CharacterWorkshop_tabLabel(CharacterWorkshopTab tab);
const char          *CharacterWorkshop_tabStorageId(CharacterWorkshopTab tab);
CharacterWorkshopTab CharacterWorkshop_parseTab(const char *stored);
const char          *CharacterWorkshop_readinessLabel(
    CharacterWorkshopReadinessId id);
const char *CharacterWorkshop_statusLabel(
    CharacterWorkshopReadinessStatus status);

#endif // MDKR64_CHARACTER_WORKSHOP_MODEL_H
