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

    facts.performance =
        CharacterWorkshopPerformanceState::OverTargetAccepted;
    const auto acceptedOverTarget = CharacterWorkshop_evaluate(facts);
    assert(acceptedOverTarget.readyToEnable);
    assert(acceptedOverTarget.readyToPlay);
    assert(row(acceptedOverTarget,
               CharacterWorkshopReadinessId::Performance).status ==
           CharacterWorkshopReadinessStatus::Accepted);

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
    assert(std::strcmp(CharacterWorkshop_statusLabel(
                           CharacterWorkshopReadinessStatus::Accepted),
                       "Exception") == 0);
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

    CharacterWorkshopFitMeasurement assessed = vehicle;
    assessed.boundsMinimumMicrometres = {-400000, -250000, -250000};
    assessed.boundsMaximumMicrometres = {400000, 750000, 250000};
    assessed.forwardMilli = {0, 0, 1000};
    auto assessment = CharacterWorkshop_assessFit(assessed);
    assert(assessment.valid && assessment.vehicleContext);
    assert(assessment.datum == CharacterWorkshopQualitySeverity::Nominal);
    assert(assessment.facing == CharacterWorkshopQualitySeverity::Nominal);
    assert(assessment.proportions ==
           CharacterWorkshopQualitySeverity::Nominal);
    assert(std::fabs(assessment.datumErrorMetres) < 1.0e-6f);

    assessed.boundsMinimumMicrometres[1] = -350000;
    assessed.boundsMaximumMicrometres[1] = 650000;
    assessed.forwardMilli = {342, 0, 940};
    assessment = CharacterWorkshop_assessFit(assessed);
    assert(assessment.datum == CharacterWorkshopQualitySeverity::Review);
    assert(assessment.facing == CharacterWorkshopQualitySeverity::Review);

    assessed.boundsMinimumMicrometres = {-1600000, -800000, -200000};
    assessed.boundsMaximumMicrometres = {1600000, 200000, 200000};
    assessed.forwardMilli = {0, 0, -1000};
    assessment = CharacterWorkshop_assessFit(assessed);
    assert(assessment.datum == CharacterWorkshopQualitySeverity::Critical);
    assert(assessment.facing == CharacterWorkshopQualitySeverity::Critical);
    assert(assessment.proportions ==
           CharacterWorkshopQualitySeverity::Critical);
    invalid.valid = false;
    assert(!CharacterWorkshop_assessFit(invalid).valid);
}

void testExactFitReviewGate() {
    CharacterWorkshopFitReviewFacts facts;
    auto decision = CharacterWorkshop_reviewFit(facts);
    assert(!decision.ready && !decision.visibilityBlocksReview);

    facts.exactContract = true;
    facts.opaqueVisibilityQualified = true;
    decision = CharacterWorkshop_reviewFit(facts);
    assert(decision.visibilityBlocksReview && !decision.ready);

    facts.opaqueVisibilityQualified = false;
    facts.sceneReviewed = true;
    decision = CharacterWorkshop_reviewFit(facts);
    assert(!decision.visibilityBlocksReview && decision.warnings &&
           decision.ready);

    facts.opaqueVisibilityQualified = true;
    facts.isolatedVisibleTiles = 10u;
    facts.sceneVisibleTiles = 2u;
    decision = CharacterWorkshop_reviewFit(facts);
    assert(decision.warnings && decision.ready);
    facts.sceneVisibleTiles = 6u;
    decision = CharacterWorkshop_reviewFit(facts);
    assert(!decision.warnings && decision.ready);

    facts.contactExceptionRequired = true;
    decision = CharacterWorkshop_reviewFit(facts);
    assert(!decision.ready);
    facts.contactExceptionApproved = true;
    decision = CharacterWorkshop_reviewFit(facts);
    assert(decision.ready);

    facts.advisoryFitWarning = true;
    assert(CharacterWorkshop_reviewFit(facts).warnings);
    facts.advisoryFitWarning = false;
    facts.cameraWarning = true;
    assert(CharacterWorkshop_reviewFit(facts).warnings);
    facts.cameraWarning = false;
    facts.surfaceWarning = true;
    assert(CharacterWorkshop_reviewFit(facts).warnings);
    facts.surfaceWarning = false;
    facts.attachmentVisibilityWarning = true;
    assert(CharacterWorkshop_reviewFit(facts).warnings);
}

void testExactContactSuggestions() {
    std::array<std::array<float, 3>, 4> current{};
    auto suggestion = CharacterWorkshop_suggestContacts(
        current, nullptr, 0u);
    assert(suggestion.valid && !suggestion.available &&
           !suggestion.adjustment && !suggestion.withinLimits);

    std::array<CharacterWorkshopContactMeasurement, 2> measurements{};
    measurements[0].witnessMask = 0x3u;
    measurements[0].targetMicrometres[0][0] = 100000;
    measurements[0].endpointMicrometres[0][0] = 80000;
    measurements[0].targetMicrometres[1][1] = 200000;
    measurements[0].endpointMicrometres[1][1] = 210000;
    measurements[1].witnessMask = 0x1u;
    measurements[1].targetMicrometres[0][0] = 100000;
    measurements[1].endpointMicrometres[0][0] = 60000;
    suggestion = CharacterWorkshop_suggestContacts(
        current, measurements.data(), measurements.size());
    assert(suggestion.valid && suggestion.available &&
           suggestion.adjustment && suggestion.withinLimits &&
           suggestion.contactMask == 0x3u &&
           suggestion.witnessSamples == 2u &&
           suggestion.sampleCounts[0] == 2u &&
           suggestion.sampleCounts[1] == 1u);
    assert(std::fabs(suggestion.deltaMetres[0][0] + 0.03f) < 1.0e-6f);
    assert(std::fabs(suggestion.deltaMetres[1][1] - 0.01f) < 1.0e-6f);

    measurements[0].witnessMask = 0x1u;
    measurements[0].targetMicrometres[0] = {1, 2, 3};
    measurements[0].endpointMicrometres[0] = {1, 2, 3};
    suggestion = CharacterWorkshop_suggestContacts(
        current, measurements.data(), 1u);
    assert(suggestion.valid && suggestion.available &&
           !suggestion.adjustment && suggestion.withinLimits);

    current[0][0] = 0.99f;
    measurements[0].targetMicrometres[0][0] = 0;
    measurements[0].endpointMicrometres[0][0] = 20000;
    suggestion = CharacterWorkshop_suggestContacts(
        current, measurements.data(), 1u);
    assert(suggestion.valid && suggestion.available &&
           suggestion.adjustment && !suggestion.withinLimits);

    measurements[0].witnessMask = 0x10u;
    assert(!CharacterWorkshop_suggestContacts(
                current, measurements.data(), 1u).valid);
    measurements[0].witnessMask = 0x1u;
    measurements[0].targetMicrometres[0][0] = INT64_C(1000000001);
    assert(!CharacterWorkshop_suggestContacts(
                current, measurements.data(), 1u).valid);
    current[0][0] = NAN;
    assert(!CharacterWorkshop_suggestContacts(
                current, measurements.data(), 1u).valid);
    assert(!CharacterWorkshop_suggestContacts(
                {}, nullptr, 65u).valid);
}

void testSceneReviewProgression() {
    constexpr uint32_t allThree = 0x7u;
    assert(CharacterWorkshop_nextSceneReview(3u, 0u, allThree, true) == 1u);
    assert(CharacterWorkshop_nextSceneReview(3u, 1u, allThree, true) == 2u);
    assert(CharacterWorkshop_nextSceneReview(3u, 2u, allThree, true) == 3u);
    assert(CharacterWorkshop_nextSceneReview(3u, 0u, 0x1u, false) == 1u);
    assert(CharacterWorkshop_nextSceneReview(3u, 1u, 0x3u, false) == 2u);
    assert(CharacterWorkshop_nextSceneReview(3u, 2u, allThree, false) == 3u);
    assert(CharacterWorkshop_nextSceneReview(1u, 0u, 0x1u, false) == 1u);
    assert(CharacterWorkshop_nextSceneReview(0u, 0u, 0u, false) == 0u);
    assert(CharacterWorkshop_nextSceneReview(33u, 0u, 0u, false) == 33u);
}

void testSourceTransformDiagnosis() {
    float correction = 321.0f;
    assert(CharacterWorkshop_facingCorrectionDegrees(0u, correction) &&
           correction == 0.0f);
    assert(CharacterWorkshop_facingCorrectionDegrees(1u, correction) &&
           correction == 180.0f);
    assert(CharacterWorkshop_facingCorrectionDegrees(2u, correction) &&
           correction == -90.0f);
    assert(CharacterWorkshop_facingCorrectionDegrees(3u, correction) &&
           correction == 90.0f);
    correction = 321.0f;
    assert(!CharacterWorkshop_facingCorrectionDegrees(4u, correction) &&
           correction == 321.0f);

    CharacterWorkshopSourceTransformFacts ordinary;
    ordinary.meshLocalMinimum = {-0.4, 0.0, -0.2};
    ordinary.meshLocalMaximum = {0.4, 1.8, 0.2};
    ordinary.sceneWorldMinimum = {-0.4, 0.0, -0.2};
    ordinary.sceneWorldMaximum = {0.4, 1.8, 0.2};
    ordinary.targetHeightMetres = 1.25;
    auto review = CharacterWorkshop_reviewSourceTransform(ordinary);
    assert(review.valid);
    assert(review.severity == CharacterWorkshopTransformSeverity::Nominal);
    assert(std::fabs(review.hierarchyScale - 1.0) < 1.0e-9);
    assert(std::fabs(review.targetHeightMultiplier - 1.25 / 1.8) < 1.0e-9);

    CharacterWorkshopSourceTransformFacts centimetreNested = ordinary;
    centimetreNested.meshLocalMinimum = {-126.0, -1.0, -23.0};
    centimetreNested.meshLocalMaximum = {126.0, 194.0, 45.0};
    centimetreNested.sceneWorldMinimum = {-0.0126, -0.0001, -0.0023};
    centimetreNested.sceneWorldMaximum = {0.0126, 0.0194, 0.0045};
    review = CharacterWorkshop_reviewSourceTransform(centimetreNested);
    assert(review.valid);
    assert(review.severity == CharacterWorkshopTransformSeverity::Critical);
    assert(review.suspiciousHierarchyScale);
    assert(review.suspiciousWorldHeight);
    assert(std::fabs(review.hierarchyScale - 0.0001) < 1.0e-12);
    assert(std::fabs(review.targetHeightMultiplier -
                     (1.25 / 0.0195)) < 1.0e-9);

    CharacterWorkshopSourceTransformFacts zUp = ordinary;
    zUp.meshLocalMinimum = {0.0, 0.0, 0.0};
    zUp.meshLocalMaximum = {100.0, 0.0, 100.0};
    zUp.sceneWorldMinimum = {0.0, 0.0, 0.0};
    zUp.sceneWorldMaximum = {1.0, 1.0, 0.0};
    review = CharacterWorkshop_reviewSourceTransform(zUp);
    assert(review.valid);
    assert(review.severity == CharacterWorkshopTransformSeverity::Review);
    assert(std::fabs(review.hierarchyScale - 0.01) < 1.0e-12);

    CharacterWorkshopSourceTransformFacts invalid = ordinary;
    invalid.sceneWorldMaximum[1] = invalid.sceneWorldMinimum[1];
    assert(!CharacterWorkshop_reviewSourceTransform(invalid).valid);
    invalid = ordinary;
    invalid.targetHeightMetres = NAN;
    assert(!CharacterWorkshop_reviewSourceTransform(invalid).valid);
}

void testStructuralRigInference() {
    const auto joint = [](const char *name, int parent, float x, float y) {
        CharacterWorkshopRigJoint value;
        value.name = name;
        value.parent = parent;
        value.bindPosition = {x, y, 0.0f};
        return value;
    };
    const std::vector<CharacterWorkshopRigJoint> siblingPelvis = {
        joint("Skl_Root", -1, 0.0f, 0.9f),
        joint("Hip", 0, 0.0f, 0.9f),
        joint("Spine1", 0, 0.0f, 1.1f),
        joint("Spine2", 2, 0.0f, 1.35f),
        joint("Head", 3, 0.0f, 1.7f),
        joint("ArmL", 3, 0.25f, 1.45f),
        joint("ElbowL", 5, 0.55f, 1.4f),
        joint("HandL", 6, 0.8f, 1.35f),
        joint("ArmR", 3, -0.25f, 1.45f),
        joint("ElbowR", 8, -0.55f, 1.4f),
        joint("HandR", 9, -0.8f, 1.35f),
        joint("LegL", 0, 0.15f, 0.8f),
        joint("KneeL", 11, 0.15f, 0.45f),
        joint("FootL", 12, 0.15f, 0.05f),
        joint("LegR", 0, -0.15f, 0.8f),
        joint("KneeR", 14, -0.15f, 0.45f),
        joint("FootR", 15, -0.15f, 0.05f),
    };
    const auto suggestion =
        CharacterWorkshop_suggestHumanoidRig(siblingPelvis);
    assert(suggestion.complete);
    assert(suggestion.hierarchyValid);
    assert(suggestion.roles[0].joint == 0);
    assert(suggestion.roles[0].evidence ==
           CharacterWorkshopRigEvidence::HierarchyCommonAncestor);
    assert(suggestion.commonAncestorRepairs == 1u);
    assert(suggestion.roles[5].joint == 6);
    assert(suggestion.roles[11].joint == 12);
    assert(suggestion.restBasisRoles == 16u);
    assert(suggestion.bendAxisRoles == 0u);
    for (const auto &role : suggestion.roles) {
        assert(role.restBasisAvailable);
        assert(std::fabs(role.restRotation[0]) < 1.0e-6f);
        assert(std::fabs(role.restRotation[1]) < 1.0e-6f);
        assert(std::fabs(role.restRotation[2]) < 1.0e-6f);
        assert(std::fabs(role.restRotation[3] - 1.0f) < 1.0e-6f);
    }

    auto anonymous = siblingPelvis;
    for (auto &value : anonymous) value.name.clear();
    const auto geometric = CharacterWorkshop_suggestHumanoidRig(anonymous);
    assert(geometric.complete && geometric.hierarchyValid);
    assert(geometric.namedRoles == 0u);
    assert(geometric.geometryRoles == 16u);
    assert(geometric.roles[0].joint == 0);
    assert(geometric.roles[2].joint == 3);
    assert(geometric.roles[3].joint == 4);
    assert(geometric.roles[6].joint == 7);
    assert(geometric.roles[15].joint == 16);
    assert(geometric.roles[3].evidence ==
           CharacterWorkshopRigEvidence::GeometrySymmetry);
    const auto geometricReoriented =
        CharacterWorkshop_suggestHumanoidRig(anonymous, 1u);
    assert(geometricReoriented.complete);
    assert(geometricReoriented.roles[6].joint == 10);
    assert(geometricReoriented.roles[9].joint == 7);

    auto unsafeHair = anonymous;
    unsafeHair.push_back(joint("", 4, 0.0f, 1.8f));
    unsafeHair.push_back(joint("", 17, 0.0f, 0.5f));
    assert(!CharacterWorkshop_suggestHumanoidRig(unsafeHair).complete);

    auto bentLegs = siblingPelvis;
    bentLegs[13].bindPosition[2] = 0.15f;
    bentLegs[16].bindPosition[2] = 0.15f;
    const auto sideways = CharacterWorkshop_suggestHumanoidRig(bentLegs, 2u);
    assert(sideways.complete && sideways.hierarchyValid);
    assert(sideways.restBasisRoles == 16u);
    assert(sideways.bendAxisRoles == 4u);
    assert(sideways.roles[10].bendAxisAvailable);
    assert(sideways.roles[11].bendAxisAvailable);
    assert(sideways.roles[13].bendAxisAvailable);
    assert(sideways.roles[14].bendAxisAvailable);
    assert(!sideways.roles[4].bendAxisAvailable);
    assert(std::fabs(sideways.roles[0].restRotation[1] -
                     std::sqrt(0.5f)) < 1.0e-5f);
    assert(std::fabs(sideways.roles[0].restRotation[3] -
                     std::sqrt(0.5f)) < 1.0e-5f);
    assert(std::fabs(sideways.roles[10].bendAxis[0] + 1.0f) < 1.0e-5f);

    auto rotatedBind = siblingPelvis;
    rotatedBind[0].bindRotation = {
        0.0f, 0.0f, std::sqrt(0.5f), std::sqrt(0.5f)};
    const auto rotated = CharacterWorkshop_suggestHumanoidRig(rotatedBind);
    assert(rotated.roles[0].restBasisAvailable);
    assert(std::fabs(rotated.roles[0].restRotation[2] +
                     std::sqrt(0.5f)) < 1.0e-5f);
    assert(std::fabs(rotated.roles[0].restRotation[3] -
                     std::sqrt(0.5f)) < 1.0e-5f);

    std::array<int, 16> partialMapping;
    partialMapping.fill(-1);
    partialMapping[0] = 0;
    const auto partialBases = CharacterWorkshop_suggestHumanoidBases(
        siblingPelvis, partialMapping, 3u);
    assert(!partialBases.complete && !partialBases.hierarchyValid);
    assert(partialBases.restBasisRoles == 1u);
    assert(partialBases.roles[0].restBasisAvailable);
    assert(std::fabs(partialBases.roles[0].restRotation[1] +
                     std::sqrt(0.5f)) < 1.0e-5f);

    auto invalidBasis = siblingPelvis;
    invalidBasis[0].bindRotation = {0.0f, 0.0f, 0.0f, 0.0f};
    const auto unavailable = CharacterWorkshop_suggestHumanoidRig(
        invalidBasis);
    assert(unavailable.complete && unavailable.hierarchyValid);
    assert(unavailable.restBasisRoles == 15u);
    assert(!unavailable.roles[0].restBasisAvailable);

    auto ambiguous = siblingPelvis;
    ambiguous.push_back(joint("Head", 3, 0.0f, 1.7f));
    assert(!CharacterWorkshop_suggestHumanoidRig(ambiguous).complete);
    auto cyclic = siblingPelvis;
    cyclic[0].parent = 3;
    const auto invalid = CharacterWorkshop_suggestHumanoidRig(cyclic);
    assert(!invalid.complete && !invalid.hierarchyValid);
    assert(CharacterWorkshop_suggestHumanoidBases(
               cyclic, partialMapping).restBasisRoles == 0u);
    assert(!CharacterWorkshop_suggestHumanoidRig(siblingPelvis, 4u).complete);
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
    testExactContactSuggestions();
    testExactFitReviewGate();
    testSceneReviewProgression();
    testSourceTransformDiagnosis();
    testStructuralRigInference();
    return 0;
}
