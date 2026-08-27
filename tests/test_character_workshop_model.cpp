#include "character_workshop_model.h"
#include "modern_character_lod.h"

#include <cassert>
#include <cmath>
#include <cstring>

namespace {

CharacterWorkshopFacts completeFacts() {
    CharacterWorkshopFacts facts;
    facts.geometryAvailable      = true;
    facts.identityReady          = true;
    facts.normalized             = true;
    facts.anchorsReady           = true;
    facts.attachmentSocketsReady = true;
    facts.rigPresent             = true;
    facts.rigReviewed            = true;
    facts.motionReady            = true;
    facts.donorQualified         = true;
    facts.performanceAssemblyReady = true;
    facts.performance = CharacterWorkshopPerformanceState::TargetMet;
    facts.enabled                = true;
    facts.supportedVehicleMask   = 7u;
    facts.reviewedContextMask    = 0xFu;
    return facts;
}

const CharacterWorkshopReadinessRow &row(
    const CharacterWorkshopReadiness &readiness,
    CharacterWorkshopReadinessId      id) {
    return readiness.rows[static_cast<size_t>(id)];
}

void testEmptyCharacter() {
    const CharacterWorkshopReadiness readiness =
        CharacterWorkshop_evaluate({});
    assert(!readiness.readyToPreview);
    assert(!readiness.readyToEnable);
    assert(!readiness.readyToPlay);
    assert(readiness.readyCount == 0u);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Identity);
    assert(std::strcmp(readiness.nextActionLabel,
                       "Create roster identity") == 0);
    assert(row(readiness, CharacterWorkshopReadinessId::GameplayProfile).status ==
           CharacterWorkshopReadinessStatus::Unavailable);
    assert(row(readiness, CharacterWorkshopReadinessId::VehicleFit).status ==
           CharacterWorkshopReadinessStatus::Missing);
}

void testReadinessOrdering() {
    CharacterWorkshopFacts facts = completeFacts();
    facts.identityReady          = false;
    facts.normalized             = false;
    facts.motionReady            = false;
    facts.donorQualified         = false;
    facts.reviewedContextMask    = 0u;
    facts.performance = CharacterWorkshopPerformanceState::NotMeasured;
    facts.enabled                = false;
    auto readiness               = CharacterWorkshop_evaluate(facts);
    assert(readiness.readyToPreview);
    assert(!readiness.readyToPlay);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Identity);

    facts.identityReady = true;
    readiness           = CharacterWorkshop_evaluate(facts);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Vehicles);
    assert(std::strcmp(readiness.nextActionLabel,
                       "Calibrate model and anchors") == 0);

    facts.normalized = true;
    readiness        = CharacterWorkshop_evaluate(facts);
    assert(readiness.nextActionTab == CharacterWorkshopTab::RigMotion);

    facts.motionReady = true;
    facts.rigReviewed = true;
    readiness         = CharacterWorkshop_evaluate(facts);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Profile);
    assert(std::strcmp(readiness.nextActionLabel,
                       "Choose a qualified gameplay profile") == 0);

    facts.donorQualified = true;
    readiness            = CharacterWorkshop_evaluate(facts);
    assert(std::strcmp(readiness.nextActionLabel,
                       "Review every supported context") == 0);

    facts.reviewedContextMask = 0xFu;
    readiness                 = CharacterWorkshop_evaluate(facts);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Test);
    assert(std::strcmp(readiness.nextActionLabel,
                       "Run the performance matrix") == 0);

    facts.performance = CharacterWorkshopPerformanceState::OverTarget;
    readiness         = CharacterWorkshop_evaluate(facts);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Performance);
    assert(!readiness.readyToPlay);

    facts.performance = CharacterWorkshopPerformanceState::TargetMet;
    readiness         = CharacterWorkshop_evaluate(facts);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Package);
    assert(readiness.readyToEnable);
    assert(!readiness.readyToPlay);

    facts.enabled = true;
    readiness     = CharacterWorkshop_evaluate(facts);
    assert(readiness.readyToEnable);
    assert(readiness.readyToPlay);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Test);
}

void testVehicleReviewMaskAndRequiredPerformance() {
    CharacterWorkshopFacts facts = completeFacts();
    facts.supportedVehicleMask   = 1u; // select + car only
    facts.reviewedContextMask    = 0x3u;
    facts.performance = CharacterWorkshopPerformanceState::NotMeasured;
    const auto readiness         = CharacterWorkshop_evaluate(facts);
    assert(!readiness.readyToPlay);
    assert(row(readiness, CharacterWorkshopReadinessId::VehicleFit).status ==
           CharacterWorkshopReadinessStatus::Ready);
    assert(row(readiness, CharacterWorkshopReadinessId::Performance).status ==
           CharacterWorkshopReadinessStatus::Review);

    facts.performance = CharacterWorkshopPerformanceState::OverTarget;
    const auto overTarget = CharacterWorkshop_evaluate(facts);
    assert(!overTarget.readyToEnable);
    assert(!overTarget.readyToPlay);
    assert(std::strcmp(overTarget.nextActionLabel,
                       "Tune performance to target") == 0);

    facts.performance = CharacterWorkshopPerformanceState::TargetMet;
    const auto targetMet = CharacterWorkshop_evaluate(facts);
    assert(targetMet.readyToEnable);
    assert(targetMet.readyToPlay);

    facts.reviewedContextMask = 0x1u;
    const auto missingCar     = CharacterWorkshop_evaluate(facts);
    assert(!missingCar.readyToEnable);
    assert(!missingCar.readyToPlay);
    assert(row(missingCar, CharacterWorkshopReadinessId::VehicleFit).status ==
           CharacterWorkshopReadinessStatus::Review);
}

void testAuthoredMotionDoesNotRequireOptionalRig() {
    CharacterWorkshopFacts facts = completeFacts();
    facts.rigPresent             = false;
    facts.rigReviewed            = false;
    const auto readiness         = CharacterWorkshop_evaluate(facts);
    assert(readiness.readyToPlay);
    assert(row(readiness, CharacterWorkshopReadinessId::RigMotion).status ==
           CharacterWorkshopReadinessStatus::Ready);
}

void testTabStorageRoundTrip() {
    for (size_t index = 0u;
         index < static_cast<size_t>(CharacterWorkshopTab::Count);
         ++index) {
        const auto tab = static_cast<CharacterWorkshopTab>(index);
        assert(CharacterWorkshop_parseTab(
                   CharacterWorkshop_tabStorageId(tab)) == tab);
        assert(CharacterWorkshop_tabLabel(tab)[0] != '\0');
    }
    assert(CharacterWorkshop_parseTab(nullptr) ==
           CharacterWorkshopTab::Overview);
    assert(CharacterWorkshop_parseTab("future-tab") ==
           CharacterWorkshopTab::Overview);
    assert(std::strcmp(CharacterWorkshop_statusLabel(
                           CharacterWorkshopReadinessStatus::Unavailable),
                       "Unavailable") == 0);
}

void testPerformanceTargets() {
    using Target = CharacterWorkshopPerformanceTarget;
    const auto *quality = CharacterWorkshop_performancePreset(Target::Quality);
    const auto *balanced = CharacterWorkshop_performancePreset(Target::Balanced);
    const auto *performance = CharacterWorkshop_performancePreset(
        Target::Performance);
    const auto *fourPlayer = CharacterWorkshop_performancePreset(
        Target::FourPlayer);
    assert(quality != nullptr && quality->players == 1 &&
           quality->lodBias == 2.0f);
    assert(balanced != nullptr && balanced->players == 2 &&
           balanced->lodBias == 0.0f);
    assert(performance != nullptr && performance->players == 1 &&
           performance->lodBias == -2.0f);
    assert(fourPlayer != nullptr && fourPlayer->players == 4 &&
           fourPlayer->lodBias == -3.0f);
    assert(CharacterWorkshop_performancePreset(Target::Count) == nullptr);
    assert(CharacterWorkshop_performanceTarget(1, 2.0f) == Target::Quality);
    assert(CharacterWorkshop_performanceTarget(2, 0.0f) == Target::Balanced);
    assert(CharacterWorkshop_performanceTarget(1, -2.0f) ==
           Target::Performance);
    assert(CharacterWorkshop_performanceTarget(4, -3.0f) ==
           Target::FourPlayer);
    assert(CharacterWorkshop_performanceTarget(3, -3.0f) == Target::Count);
    assert(CharacterWorkshop_performanceTarget(1, NAN) == Target::Count);
}

void testRuntimeEquivalentLodSelection() {
    CharacterWorkshopLodBand bands[4] = {};
    assert(CharacterWorkshop_selectLod(0.0f, 0.0f, 0.0f, 0xFu) == 0u);
    assert(CharacterWorkshop_selectLod(650.0f, 0.0f, 0.0f, 0xFu) == 1u);
    assert(CharacterWorkshop_selectLod(1300.0f, 0.0f, 0.0f, 0xFu) == 2u);
    assert(CharacterWorkshop_selectLod(2400.0f, 0.0f, 0.0f, 0xFu) == 3u);
    assert(CharacterWorkshop_selectLod(0.0f, 0.0f, -2.0f, 0xFu) == 2u);
    assert(CharacterWorkshop_selectLod(2400.0f, 0.0f, 2.0f, 0xFu) == 1u);
    assert(CharacterWorkshop_selectLod(0.0f, 0.0f, -3.0f, 0x3u) == 1u);
    assert(CharacterWorkshop_selectLod(1300.0f, 0.0f, 0.0f, 0xBu) == 1u);
    assert(CharacterWorkshop_selectLod(650.0f, 0.5f, 0.0f, 0xFu) == 0u);
    assert(CharacterWorkshop_selectLod(-1.0f, 0.0f, 0.0f, 0x1u) == 0u);
    assert(CharacterWorkshop_selectLod(0.0f, 0.0f, 0.0f, 0u) == UINT32_MAX);
    assert(CharacterWorkshop_selectLod(0.0f, 0.0f, 0.0f, 0x10u) ==
           UINT32_MAX);
    assert(CharacterWorkshop_selectLod(NAN, 0.0f, 0.0f, 1u) == UINT32_MAX);
    assert(CharacterWorkshop_selectLod(0.0f, 0.0f, NAN, 1u) == UINT32_MAX);
    assert(CharacterWorkshop_selectLod(0.0f, 5.0f, 0.0f, 1u) == UINT32_MAX);
    assert(CharacterWorkshop_lodBands(0.0f, 0.0f, 0xFu, bands) == 4u &&
           bands[0].minimumDistance == 0.0f &&
           bands[0].maximumDistance == 650.0f && bands[0].lod == 0u &&
           bands[1].minimumDistance == 650.0f && bands[1].lod == 1u &&
           bands[2].minimumDistance == 1300.0f && bands[2].lod == 2u &&
           bands[3].minimumDistance == 2400.0f &&
           std::isinf(bands[3].maximumDistance) && bands[3].lod == 3u);
    assert(CharacterWorkshop_lodBands(0.0f, 0.0f, 0xBu, bands) == 3u &&
           bands[0].lod == 0u && bands[0].maximumDistance == 650.0f &&
           bands[1].lod == 1u && bands[1].maximumDistance == 2400.0f &&
           bands[2].lod == 3u);
    assert(CharacterWorkshop_lodBands(0.0f, 2.0f, 0xFu, bands) == 2u &&
           bands[0].lod == 0u && bands[0].maximumDistance == 2400.0f &&
           bands[1].lod == 1u && std::isinf(bands[1].maximumDistance));
    assert(CharacterWorkshop_lodBands(0.0f, 0.0f, 0x1u, bands) == 1u &&
           bands[0].lod == 0u && std::isinf(bands[0].maximumDistance));
    const CharacterWorkshopLodBand sentinel = bands[0];
    assert(CharacterWorkshop_lodBands(NAN, 0.0f, 0xFu, bands) == 0u &&
           bands[0].minimumDistance == sentinel.minimumDistance &&
           bands[0].maximumDistance == sentinel.maximumDistance &&
           bands[0].lod == sentinel.lod);
    assert(CharacterWorkshop_lodBands(0.0f, 0.0f, 0xFu, nullptr) == 0u);
}

void testRuntimeLodHysteresis() {
    assert(mdkr_modern_character_select_lod_hysteretic(
               650.0f, 0.0f, 0.0f, 0xFu, 0u, 1) == 0u);
    assert(mdkr_modern_character_select_lod_hysteretic(
               706.0f, 0.0f, 0.0f, 0xFu, 0u, 1) == 0u);
    assert(mdkr_modern_character_select_lod_hysteretic(
               707.0f, 0.0f, 0.0f, 0xFu, 0u, 1) == 1u);
    assert(mdkr_modern_character_select_lod_hysteretic(
               649.0f, 0.0f, 0.0f, 0xFu, 1u, 1) == 1u);
    assert(mdkr_modern_character_select_lod_hysteretic(
               601.0f, 0.0f, 0.0f, 0xFu, 1u, 1) == 0u);
    assert(mdkr_modern_character_select_lod_hysteretic(
               2400.0f, 0.0f, 0.0f, 0xBu, 1u, 1) == 1u);
    assert(mdkr_modern_character_select_lod_hysteretic(
               2610.0f, 0.0f, 0.0f, 0xBu, 1u, 1) == 3u);
    assert(mdkr_modern_character_select_lod_hysteretic(
               650.0f, 0.0f, 0.0f, 0xFu, 9u, 1) == 1u);
    assert(mdkr_modern_character_select_lod_hysteretic(
               650.0f, 0.0f, 0.0f, 0xFu, 0u, 0) == 1u);
}

void testExactFitSuggestions() {
    CharacterWorkshopFitMeasurement vehicle;
    vehicle.valid = true;
    vehicle.vehicleContext = true;
    vehicle.boundsMinimumMicrometres = {-300000, -481272, -200000};
    vehicle.boundsMaximumMicrometres = {300000, 518728, 200000};
    vehicle.forwardMilli = {0, 0, 1000};
    auto suggestion = CharacterWorkshop_suggestFit(vehicle);
    assert(suggestion.available);
    assert(suggestion.verticalAdjustment);
    assert(suggestion.facingMeasured);
    assert(!suggestion.facingAdjustment);
    assert(std::fabs(suggestion.measuredHeightMetres - 1.0f) < 1.0e-6f);
    assert(std::fabs(suggestion.targetMinimumYMetres + 0.25f) < 1.0e-6f);
    assert(std::fabs(suggestion.verticalDeltaMetres - 0.231272f) < 1.0e-6f);

    vehicle.boundsMinimumMicrometres[1] = -249000;
    vehicle.boundsMaximumMicrometres[1] = 751000;
    vehicle.forwardMilli = {0, 0, -1000};
    suggestion = CharacterWorkshop_suggestFit(vehicle);
    assert(!suggestion.verticalAdjustment);
    assert(suggestion.facingAdjustment);
    assert(std::fabs(suggestion.yawDeltaDegrees - 180.0f) < 1.0e-6f);

    CharacterWorkshopFitMeasurement select = vehicle;
    select.vehicleContext = false;
    select.boundsMinimumMicrometres[1] = -125000;
    select.boundsMaximumMicrometres[1] = 875000;
    select.forwardMilli = {1000, 0, 0};
    suggestion = CharacterWorkshop_suggestFit(select);
    assert(suggestion.verticalAdjustment);
    assert(std::fabs(suggestion.verticalDeltaMetres - 0.125f) < 1.0e-6f);
    assert(suggestion.facingAdjustment);
    assert(std::fabs(suggestion.yawDeltaDegrees + 90.0f) < 1.0e-6f);

    CharacterWorkshopFitMeasurement invalid;
    assert(!CharacterWorkshop_suggestFit(invalid).available);
    invalid.valid = true;
    invalid.boundsMinimumMicrometres = {0, 20, 0};
    invalid.boundsMaximumMicrometres = {0, 10, 0};
    assert(!CharacterWorkshop_suggestFit(invalid).available);
    invalid.boundsMinimumMicrometres = {0, 0, 0};
    invalid.boundsMaximumMicrometres = {1000, 1000000, 1000};
    invalid.forwardMilli = {0, 1000, 0};
    const auto noForward = CharacterWorkshop_suggestFit(invalid);
    assert(noForward.available && !noForward.facingMeasured &&
           !noForward.facingAdjustment);
}

} // namespace

int main() {
    testEmptyCharacter();
    testReadinessOrdering();
    testVehicleReviewMaskAndRequiredPerformance();
    testAuthoredMotionDoesNotRequireOptionalRig();
    testTabStorageRoundTrip();
    testPerformanceTargets();
    testRuntimeEquivalentLodSelection();
    testRuntimeLodHysteresis();
    testExactFitSuggestions();
    return 0;
}
