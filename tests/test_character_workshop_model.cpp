#include "character_workshop_model.h"

#include <cassert>
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
    facts.performanceMeasured    = true;
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
    facts.performanceMeasured    = false;
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
    assert(readiness.nextActionTab == CharacterWorkshopTab::Vehicles);
    assert(std::strcmp(readiness.nextActionLabel,
                       "Choose a qualified gameplay profile") == 0);

    facts.donorQualified = true;
    readiness            = CharacterWorkshop_evaluate(facts);
    assert(std::strcmp(readiness.nextActionLabel,
                       "Review every supported context") == 0);

    facts.reviewedContextMask = 0xFu;
    readiness                 = CharacterWorkshop_evaluate(facts);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Package);

    facts.enabled = true;
    readiness     = CharacterWorkshop_evaluate(facts);
    assert(readiness.readyToPlay);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Performance);

    facts.performanceMeasured = true;
    readiness                 = CharacterWorkshop_evaluate(facts);
    assert(readiness.readyToPlay);
    assert(readiness.nextActionTab == CharacterWorkshopTab::Test);
}

void testVehicleReviewMaskAndOptionalPerformance() {
    CharacterWorkshopFacts facts = completeFacts();
    facts.supportedVehicleMask   = 1u; // select + car only
    facts.reviewedContextMask    = 0x3u;
    facts.performanceMeasured    = false;
    const auto readiness         = CharacterWorkshop_evaluate(facts);
    assert(readiness.readyToPlay);
    assert(row(readiness, CharacterWorkshopReadinessId::VehicleFit).status ==
           CharacterWorkshopReadinessStatus::Ready);
    assert(row(readiness, CharacterWorkshopReadinessId::Performance).status ==
           CharacterWorkshopReadinessStatus::Review);

    facts.reviewedContextMask = 0x1u;
    const auto missingCar     = CharacterWorkshop_evaluate(facts);
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

} // namespace

int main() {
    testEmptyCharacter();
    testReadinessOrdering();
    testVehicleReviewMaskAndOptionalPerformance();
    testAuthoredMotionDoesNotRequireOptionalRig();
    testTabStorageRoundTrip();
    return 0;
}
