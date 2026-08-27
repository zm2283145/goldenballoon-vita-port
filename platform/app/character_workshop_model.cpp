#include "character_workshop_model.h"

#include "modern_character_lod.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t requiredContextMask(uint32_t vehicleMask) {
    return 1u | ((vehicleMask & 7u) << 1u);
}

void setRow(CharacterWorkshopReadiness      &readiness,
            CharacterWorkshopReadinessId     id,
            CharacterWorkshopReadinessStatus status,
            CharacterWorkshopTab             tab) {
    const size_t index    = static_cast<size_t>(id);
    readiness.rows[index] = {id, status, tab};
    if (status == CharacterWorkshopReadinessStatus::Ready) {
        ++readiness.readyCount;
    }
}

} // namespace

CharacterWorkshopReadiness CharacterWorkshop_evaluate(
    const CharacterWorkshopFacts &facts) {
    CharacterWorkshopReadiness result;

    setRow(result, CharacterWorkshopReadinessId::Identity, facts.identityReady ? CharacterWorkshopReadinessStatus::Ready : CharacterWorkshopReadinessStatus::Missing, CharacterWorkshopTab::Identity);

    CharacterWorkshopReadinessStatus calibration =
        CharacterWorkshopReadinessStatus::Ready;
    if (!facts.geometryAvailable || !facts.anchorsReady ||
        !facts.attachmentSocketsReady) {
        calibration = CharacterWorkshopReadinessStatus::Missing;
    } else if (!facts.normalized) {
        calibration = CharacterWorkshopReadinessStatus::Review;
    }
    setRow(result, CharacterWorkshopReadinessId::Calibration, calibration, CharacterWorkshopTab::Vehicles);

    CharacterWorkshopReadinessStatus rigMotion =
        CharacterWorkshopReadinessStatus::Ready;
    if (!facts.motionReady) {
        rigMotion = facts.rigPresent
                        ? CharacterWorkshopReadinessStatus::Review
                        : CharacterWorkshopReadinessStatus::Missing;
    } else if (facts.rigPresent && !facts.rigReviewed) {
        // Complete authored motion can play without retargeting. An unreviewed
        // optional rig remains visible work, but is not misreported as absent.
        rigMotion = CharacterWorkshopReadinessStatus::Review;
    }
    setRow(result, CharacterWorkshopReadinessId::RigMotion, rigMotion, CharacterWorkshopTab::RigMotion);

    setRow(result, CharacterWorkshopReadinessId::GameplayProfile, facts.donorQualified ? CharacterWorkshopReadinessStatus::Ready : CharacterWorkshopReadinessStatus::Unavailable, CharacterWorkshopTab::Vehicles);

    CharacterWorkshopReadinessStatus vehicleFit =
        CharacterWorkshopReadinessStatus::Ready;
    if ((facts.supportedVehicleMask & 7u) == 0u) {
        vehicleFit = CharacterWorkshopReadinessStatus::Missing;
    } else {
        const uint32_t required = requiredContextMask(
            facts.supportedVehicleMask);
        if ((facts.reviewedContextMask & required) != required) {
            vehicleFit = CharacterWorkshopReadinessStatus::Review;
        }
    }
    setRow(result, CharacterWorkshopReadinessId::VehicleFit, vehicleFit, CharacterWorkshopTab::Vehicles);

    CharacterWorkshopReadinessStatus performance =
        CharacterWorkshopReadinessStatus::Ready;
    if (!facts.performanceAssemblyReady) {
        performance = CharacterWorkshopReadinessStatus::Missing;
    } else if (facts.performance !=
               CharacterWorkshopPerformanceState::TargetMet) {
        performance = CharacterWorkshopReadinessStatus::Review;
    }
    setRow(result, CharacterWorkshopReadinessId::Performance, performance,
           CharacterWorkshopTab::Performance);

    result.readyToPreview = facts.geometryAvailable;
    result.readyToPlay    = facts.geometryAvailable && facts.identityReady &&
                            facts.normalized && facts.anchorsReady &&
                            facts.attachmentSocketsReady && facts.motionReady &&
                            facts.donorQualified &&
                            vehicleFit == CharacterWorkshopReadinessStatus::Ready &&
                            performance == CharacterWorkshopReadinessStatus::Ready &&
                            facts.enabled;

    if (!facts.identityReady) {
        result.nextActionTab   = CharacterWorkshopTab::Identity;
        result.nextActionLabel = "Create roster identity";
    } else if (!facts.geometryAvailable || !facts.normalized ||
               !facts.anchorsReady || !facts.attachmentSocketsReady) {
        result.nextActionTab   = CharacterWorkshopTab::Vehicles;
        result.nextActionLabel = "Calibrate model and anchors";
    } else if (!facts.motionReady ||
               (facts.rigPresent && !facts.rigReviewed)) {
        result.nextActionTab   = CharacterWorkshopTab::RigMotion;
        result.nextActionLabel = "Review rig and motion";
    } else if (!facts.donorQualified) {
        result.nextActionTab   = CharacterWorkshopTab::Vehicles;
        result.nextActionLabel = "Choose a qualified gameplay profile";
    } else if (vehicleFit != CharacterWorkshopReadinessStatus::Ready) {
        result.nextActionTab   = CharacterWorkshopTab::Vehicles;
        result.nextActionLabel = "Review every supported context";
    } else if (!facts.performanceAssemblyReady) {
        result.nextActionTab   = CharacterWorkshopTab::Performance;
        result.nextActionLabel = "Complete performance assembly";
    } else if (facts.performance ==
               CharacterWorkshopPerformanceState::NotMeasured) {
        result.nextActionTab   = CharacterWorkshopTab::Test;
        result.nextActionLabel = "Run the performance matrix";
    } else if (facts.performance ==
               CharacterWorkshopPerformanceState::OverTarget) {
        result.nextActionTab   = CharacterWorkshopTab::Performance;
        result.nextActionLabel = "Tune performance to target";
    } else if (!facts.enabled) {
        result.nextActionTab   = CharacterWorkshopTab::Package;
        result.nextActionLabel = "Enable validated character";
    } else {
        result.nextActionTab   = CharacterWorkshopTab::Test;
        result.nextActionLabel = "Run an exact-context test";
    }
    return result;
}

const char *CharacterWorkshop_tabLabel(CharacterWorkshopTab tab) {
    static constexpr const char *kLabels[] = {
        "Overview",
        "Identity",
        "Rig & Motion",
        "Vehicles",
        "Performance",
        "Test",
        "Package",
    };
    const size_t index = static_cast<size_t>(tab);
    return index < static_cast<size_t>(CharacterWorkshopTab::Count)
               ? kLabels[index]
               : "Overview";
}

const char *CharacterWorkshop_tabStorageId(CharacterWorkshopTab tab) {
    static constexpr const char *kIds[] = {
        "overview",
        "identity",
        "rig-motion",
        "vehicles",
        "performance",
        "test",
        "package",
    };
    const size_t index = static_cast<size_t>(tab);
    return index < static_cast<size_t>(CharacterWorkshopTab::Count)
               ? kIds[index]
               : "overview";
}

CharacterWorkshopTab CharacterWorkshop_parseTab(const char *stored) {
    if (stored == nullptr) return CharacterWorkshopTab::Overview;
    for (size_t index = 0u;
         index < static_cast<size_t>(CharacterWorkshopTab::Count);
         ++index) {
        const CharacterWorkshopTab tab =
            static_cast<CharacterWorkshopTab>(index);
        if (std::strcmp(stored, CharacterWorkshop_tabStorageId(tab)) == 0) {
            return tab;
        }
    }
    return CharacterWorkshopTab::Overview;
}

const char *CharacterWorkshop_readinessLabel(
    CharacterWorkshopReadinessId id) {
    static constexpr const char *kLabels[] = {
        "Roster identity",
        "Calibration and anchors",
        "Rig and motion",
        "Gameplay profile",
        "Vehicle fit",
        "Performance evidence",
    };
    const size_t index = static_cast<size_t>(id);
    return index < static_cast<size_t>(CharacterWorkshopReadinessId::Count)
               ? kLabels[index]
               : "Unknown requirement";
}

const char *CharacterWorkshop_statusLabel(
    CharacterWorkshopReadinessStatus status) {
    static constexpr const char *kLabels[] = {
        "Ready",
        "Review",
        "Missing",
        "Unavailable",
    };
    const size_t index = static_cast<size_t>(status);
    return index < 4u ? kLabels[index] : "Unavailable";
}

const CharacterWorkshopPerformancePreset *
CharacterWorkshop_performancePreset(
    CharacterWorkshopPerformanceTarget target) {
    static constexpr CharacterWorkshopPerformancePreset kPresets[] = {
        {"Quality",
         "Keeps the most detailed authored LOD longer for a one-player layout.",
         1, 2.0f},
        {"Balanced",
         "Uses authored distance bands unchanged and inspects a two-player layout.",
         2, 0.0f},
        {"Performance",
         "Moves two authored LOD bands toward lower geometry for a one-player layout.",
         1, -2.0f},
        {"Four-player",
         "Uses the strongest bounded lower-detail preference and a four-player layout.",
         4, -3.0f},
    };
    const size_t index = static_cast<size_t>(target);
    return index < static_cast<size_t>(
                       CharacterWorkshopPerformanceTarget::Count)
        ? &kPresets[index] : nullptr;
}

CharacterWorkshopPerformanceTarget CharacterWorkshop_performanceTarget(
    int players, float lodBias) {
    if (!std::isfinite(lodBias)) {
        return CharacterWorkshopPerformanceTarget::Count;
    }
    for (size_t index = 0u;
         index < static_cast<size_t>(
                     CharacterWorkshopPerformanceTarget::Count);
         ++index) {
        const CharacterWorkshopPerformancePreset *preset =
            CharacterWorkshop_performancePreset(
                static_cast<CharacterWorkshopPerformanceTarget>(index));
        if (preset != nullptr && players == preset->players &&
            std::fabs(lodBias - preset->lodBias) <= 1.0e-6f) {
            return static_cast<CharacterWorkshopPerformanceTarget>(index);
        }
    }
    return CharacterWorkshopPerformanceTarget::Count;
}

uint32_t CharacterWorkshop_selectLod(
    float viewDistance, float sourceLodBias, float localLodBias,
    uint32_t authoredLodMask) {
    return mdkr_modern_character_select_lod(
        viewDistance, sourceLodBias, localLodBias, authoredLodMask);
}

size_t CharacterWorkshop_lodBands(
    float sourceLodBias, float localLodBias, uint32_t authoredLodMask,
    CharacterWorkshopLodBand output[MDKR_MODERN_CHARACTER_LOD_LEVELS]) {
    static constexpr float minimums[] = {0.0f, 650.0f, 1300.0f, 2400.0f};
    static constexpr float maximums[] = {
        650.0f, 1300.0f, 2400.0f, INFINITY,
    };
    CharacterWorkshopLodBand resolved[MDKR_MODERN_CHARACTER_LOD_LEVELS];
    size_t count = 0u;
    if (output == nullptr) return 0u;
    for (size_t base = 0u; base < MDKR_MODERN_CHARACTER_LOD_LEVELS; ++base) {
        const uint32_t selected = CharacterWorkshop_selectLod(
            minimums[base], sourceLodBias, localLodBias, authoredLodMask);
        if (selected == UINT32_MAX) return 0u;
        if (count != 0u && resolved[count - 1u].lod == selected) {
            resolved[count - 1u].maximumDistance = maximums[base];
        } else {
            resolved[count++] = {
                minimums[base], maximums[base], selected,
            };
        }
    }
    std::copy(resolved, resolved + count, output);
    return count;
}
