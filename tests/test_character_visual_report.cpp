#include "character_visual_report.h"

#include "fs_utf8.h"
#include "sha256.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

constexpr unsigned char kOnePixelPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0x00,
    0x00, 0x03, 0x01, 0x01, 0x00, 0xc9, 0xfe, 0x92,
    0xef,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
    0xae, 0x42, 0x60, 0x82,
};

constexpr unsigned char kChangedOnePixelPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
    0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x9c, 0x63, 0x60, 0xf8, 0xcf, 0x00,
    0x00, 0x02, 0x02, 0x01, 0x00, 0x7b, 0x09, 0x81,
    0x78, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82,
};

constexpr unsigned char kOnePixelRgbaPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
    0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xd0,
    0x00, 0x00, 0x04, 0x81, 0x01, 0x80, 0x2c, 0x55,
    0xce, 0xb0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82,
};

bool writeBytes(const std::string &path, const unsigned char *bytes,
                size_t size) {
    FILE *file = mdkr_fopen_utf8(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool written = std::fwrite(bytes, 1u, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    return written && closed;
}

std::string readText(const std::string &path) {
    FILE *file = mdkr_fopen_utf8(path.c_str(), "rb");
    if (file == nullptr) return {};
    std::string text;
    char block[4096];
    size_t count;
    while ((count = std::fread(block, 1u, sizeof(block), file)) != 0u) {
        text.append(block, count);
    }
    (void)std::fclose(file);
    return text;
}

} // namespace

int main() {
    using namespace CharacterVisualReport;
    const std::string pngPath = "character-visual-report-input.png";
    const std::string truncatedPath =
        "character-visual-report-truncated.png";
    const std::string rgbaPath =
        "character-visual-report-alpha.png";
    const std::string corruptPath =
        "character-visual-report-corrupt.png";
    const std::string reportPath = "character-visual-report-output.html";
    const std::string otherReportPath =
        "character-visual-report-other.html";
    (void)mdkr_remove_utf8(pngPath.c_str());
    (void)mdkr_remove_utf8(truncatedPath.c_str());
    (void)mdkr_remove_utf8(rgbaPath.c_str());
    (void)mdkr_remove_utf8(corruptPath.c_str());
    (void)mdkr_remove_utf8(reportPath.c_str());
    (void)mdkr_remove_utf8(otherReportPath.c_str());
    expect(writeBytes(pngPath, kOnePixelPng, sizeof(kOnePixelPng)),
           "PNG fixture is written");
    expect(writeBytes(truncatedPath, kOnePixelPng,
                      sizeof(kOnePixelPng) - 12u),
           "truncated PNG fixture is written");
    expect(writeBytes(
               rgbaPath, kOnePixelRgbaPng, sizeof(kOnePixelRgbaPng)),
           "RGBA PNG fixture is written");
    std::vector<unsigned char> corrupt(
        std::begin(kOnePixelPng), std::end(kOnePixelPng));
    corrupt[45] ^= 0x40u;
    expect(writeBytes(corruptPath, corrupt.data(), corrupt.size()),
           "CRC-corrupt PNG fixture is written");

    Capture capture{};
    capture.pngPath = pngPath;
    capture.sourceSha256 = std::string(64u, 'a');
    capture.fitSha256 = std::string(64u, 'b');
    capture.presentationSha256 = std::string(64u, 'e');
    capture.context = "Car <script>";
    capture.pose = "Race steer & lean";
    capture.lighting = "Bright";
    capture.players = 4u;
    capture.phaseMilli = 875u;
    capture.viewYawDegrees = -90;
    capture.viewPitchDegrees = 90;
    capture.width = 1u;
    capture.height = 1u;
    capture.stableFrames = 12u;
    capture.exactPose = true;
    Capture alphaCapture = capture;
    alphaCapture.pngPath = rgbaPath;
    alphaCapture.renderProduct = RenderProduct::ModelAlpha;
    alphaCapture.fitProjection.valid = true;
    alphaCapture.fitProjection.width = 1u;
    alphaCapture.fitProjection.height = 1u;
    alphaCapture.fitProjection.primitiveDraws = 2u;
    alphaCapture.fitProjection.viewport = {0, 0, 1, 1};
    alphaCapture.fitProjection.scissor = {0, 0, 1, 1};
    for (size_t point = 0u; point < kFitProjectionPoints; ++point) {
        alphaCapture.fitProjection.pixelMilli[point] = {500, 500};
        alphaCapture.fitProjection.depthMillionths[point] = 500000;
    }
    alphaCapture.fitProjection.pixelMilli[9] = {750, 500};
    std::string error;
    expect(bindPng(capture, error) && bindPng(alphaCapture, error),
           "capture publication binds each exact typed PNG digest");
    expect(capture.pngSha256.size() == 64u &&
               alphaCapture.pngSha256.size() == 64u &&
               capture.pngSha256 != alphaCapture.pngSha256,
           "RGB and RGBA products retain distinct immutable image identities");
    expect(validateBoundPng(capture, error) &&
               validateBoundPng(alphaCapture, error),
           "unchanged bound capture files remain eligible for downstream use");
    Capture donorCapture = capture;
    donorCapture.pngSha256.clear();
    donorCapture.subject = Subject::RetailDonor;
    donorCapture.referenceDonor = "Diddy Kong";
    donorCapture.sceneRegistrationSha256 = std::string(64u, 'c');
    donorCapture.pose = "Race steer & lean";
    donorCapture.lighting = "Neutral";
    donorCapture.players = 1u;
    donorCapture.phaseMilli = 500u;
    donorCapture.viewYawDegrees = 180;
    donorCapture.viewPitchDegrees = 0;
    donorCapture.exactPose = false;
    expect(bindPng(donorCapture, error),
           "a composed retail donor reference binds as comparison-only evidence");
    Capture unregisteredDonor = donorCapture;
    unregisteredDonor.pngSha256.clear();
    unregisteredDonor.sceneRegistrationSha256.clear();
    expect(!bindPng(unregisteredDonor, error),
           "a donor reference without an exact scene witness fails closed");
    Capture donorAlpha = alphaCapture;
    donorAlpha.pngSha256.clear();
    donorAlpha.subject = Subject::RetailDonor;
    donorAlpha.exactPose = false;
    expect(!bindPng(donorAlpha, error),
           "a retail donor reference cannot claim a custom model-only product");
    Capture registeredAlpha = alphaCapture;
    registeredAlpha.pngSha256.clear();
    registeredAlpha.sceneRegistrationSha256 = std::string(64u, 'c');
    expect(!bindPng(registeredAlpha, error),
           "a model-only product cannot claim composed-scene registration");
    Capture donorExactPose = donorCapture;
    donorExactPose.pngSha256.clear();
    donorExactPose.exactPose = true;
    expect(!bindPng(donorExactPose, error),
           "donor pixels cannot claim an exact custom semantic phase");
    Capture unnamedDonor = donorCapture;
    unnamedDonor.pngSha256.clear();
    unnamedDonor.referenceDonor.clear();
    expect(!bindPng(unnamedDonor, error),
           "a portable retail reference must name the selected donor");
    Capture registeredCustom = donorCapture;
    registeredCustom.subject = Subject::CustomCharacter;
    registeredCustom.referenceDonor.clear();
    registeredCustom.exactPose = true;
    expect(registeredComparison(registeredCustom, donorCapture),
           "matching exact scene witnesses admit a pixel-registered comparison");
    Capture shiftedDonor = donorCapture;
    shiftedDonor.sceneRegistrationSha256 = std::string(64u, 'd');
    expect(!registeredComparison(registeredCustom, shiftedDonor),
           "a different camera or fit witness refuses inferred registration");
    Capture changedPresentation = donorCapture;
    changedPresentation.presentationSha256 = std::string(64u, 'f');
    expect(!registeredComparison(registeredCustom, changedPresentation),
           "changed renderer presentation refuses a stale comparison");
    Capture changedScene = donorCapture;
    changedScene.scene = 1u;
    expect(!registeredComparison(registeredCustom, changedScene),
           "captures from different qualified scenes cannot be overlaid");
    Capture missingProjection = alphaCapture;
    missingProjection.fitProjection = FitProjection{};
    expect(!validateBoundPng(missingProjection, error),
           "model-alpha captures require a registered fit projection");
    Capture inconsistentProjection = alphaCapture;
    inconsistentProjection.fitProjection.clipFlags[0] = 1u;
    expect(!validateBoundPng(inconsistentProjection, error),
           "projection coordinates and clip flags cannot contradict each other");
    Capture alreadyBound = capture;
    expect(!bindPng(alreadyBound, error) &&
               alreadyBound.pngSha256 == capture.pngSha256,
           "published captures cannot be silently rebound to later bytes");

    std::vector<Capture> slotCaptures;
    Capture firstSlot = capture;
    firstSlot.pngSha256.clear();
    expect(bindAndStore(slotCaptures, firstSlot, error) ==
               StoreResult::Added &&
               slotCaptures.size() == 1u,
           "a create-only capture slot is added to the session tray");
    const std::string firstSlotDigest = slotCaptures.front().pngSha256;
    expect(writeBytes(pngPath, kChangedOnePixelPng,
                      sizeof(kChangedOnePixelPng)),
           "replacement slot PNG is written");
    Capture replacementSlot = capture;
    replacementSlot.pngSha256.clear();
    expect(bindAndStore(slotCaptures, replacementSlot, error) ==
               StoreResult::Replaced &&
               slotCaptures.size() == 1u &&
               slotCaptures.front().pngSha256 != firstSlotDigest,
           "recapturing an owned path replaces its stale digest record");
    expect(writeBytes(pngPath, kOnePixelPng,
                      sizeof(kOnePixelPng) - 12u),
           "invalid replacement slot PNG is written");
    Capture invalidSlot = capture;
    invalidSlot.pngSha256.clear();
    expect(bindAndStore(slotCaptures, invalidSlot, error) ==
               StoreResult::Invalid &&
               slotCaptures.empty(),
           "an invalid recapture removes the now-stale same-path record");
    std::vector<Capture> fullTray(kMaximumCaptures, capture);
    Capture overflowSlot = alphaCapture;
    overflowSlot.pngSha256.clear();
    expect(bindAndStore(fullTray, overflowSlot, error) ==
               StoreResult::Full &&
               fullTray.size() == kMaximumCaptures,
           "a new capture cannot silently exceed the report safety capacity");
    Capture fullTrayReplacement = alphaCapture;
    fullTrayReplacement.pngSha256.clear();
    expect(bindAndReplace(
               fullTray, 0u, fullTrayReplacement, error) ==
               StoreResult::Replaced &&
               fullTray.size() == kMaximumCaptures &&
               fullTray.front().pngPath == alphaCapture.pngPath,
           "a caller-qualified inline slot remains refreshable at tray capacity");
    const Capture retainedReplacement = fullTray[1u];
    Capture invalidFullTrayReplacement = capture;
    invalidFullTrayReplacement.pngSha256.clear();
    expect(bindAndReplace(
               fullTray, 1u, invalidFullTrayReplacement, error) ==
               StoreResult::Invalid &&
               fullTray[1u].pngSha256 == retainedReplacement.pngSha256 &&
               fullTray[1u].pngPath == retainedReplacement.pngPath,
           "an invalid qualified replacement preserves the last good tray entry");
    expect(bindAndReplace(
               fullTray, fullTray.size(), fullTrayReplacement, error) ==
               StoreResult::Invalid &&
               fullTray.size() == kMaximumCaptures,
           "an out-of-range replacement index fails without mutating the tray");
    expect(writeBytes(pngPath, kOnePixelPng, sizeof(kOnePixelPng)),
           "capture slot fixture is restored after replacement tests");

    std::vector<Capture> captures{capture, alphaCapture, donorCapture};
    const std::string hostileName =
        "Dixie </script><script>alert('x')</script> & friends";
    const bool initialExport = exportHtml(
        reportPath, "dixie.cc0", hostileName, captures, error);
    if (!initialExport) {
        std::fprintf(stderr, "report export error: %s\n", error.c_str());
    }
    expect(initialExport,
           "valid captures export to an exclusive self-contained report");
    const std::string report = readText(reportPath);
    char pngSha[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(kOnePixelPng, sizeof(kOnePixelPng), pngSha);
    expect(report.find("data:image/png;base64,") != std::string::npos &&
               report.find(std::string("PNG SHA-256 ") + pngSha) !=
                   std::string::npos,
           "report embeds image bytes and their exact SHA-256");
    expect(report.find("Dixie &lt;/script&gt;&lt;script&gt;") !=
                   std::string::npos &&
               report.find("\\u003c/script\\u003e\\u003cscript\\u003e") !=
                   std::string::npos &&
               report.find("</script><script>alert") == std::string::npos,
           "display metadata is safe in both HTML and embedded JSON contexts");
    expect(report.find("\"version\":5") != std::string::npos &&
               report.find("\"renderProduct\":\"scene\"") !=
                   std::string::npos &&
               report.find("\"renderProduct\":\"model-alpha\"") !=
                   std::string::npos &&
               report.find("\"subject\":\"custom-character\"") !=
                   std::string::npos &&
               report.find("\"subject\":\"retail-donor-reference\"") !=
                   std::string::npos &&
               report.find("\"referenceDonor\":\"Diddy Kong\"") !=
                   std::string::npos &&
               report.find("\"sceneRegistrationSha256\":\"" +
                           donorCapture.sceneRegistrationSha256 +
                           "\"") != std::string::npos &&
               report.find("Comparison only · registered held camera") !=
                   std::string::npos &&
               report.find("\"sourceSha256\":\"") != std::string::npos &&
               report.find("\"fitSha256\":\"") != std::string::npos &&
               report.find("\"presentationSha256\":\"") !=
                   std::string::npos &&
               report.find(
                   "\"scene\":0,\"sceneRegistrationSha256\"") !=
                   std::string::npos &&
               report.find("\"pngSha256\":\"" + capture.pngSha256 +
                           "\"") != std::string::npos &&
               report.find("\"stableFrames\":12") != std::string::npos &&
               report.find("\"exactPose\":true") != std::string::npos &&
               report.find("\"fitProjection\":{") != std::string::npos &&
               report.find("Registered calibrated bounds") !=
                   std::string::npos &&
               report.find("viewBox=\"0 0 1000 1000\"") !=
                   std::string::npos,
           "portable report includes registered machine-readable fit identity");
    expect(report.find(pngPath) == std::string::npos &&
               report.find("pngPath") == std::string::npos,
           "report does not disclose original capture paths");

    const std::string firstReport = report;
    expect(!exportHtml(reportPath, "dixie.cc0", hostileName,
                       captures, error) && readText(reportPath) == firstReport,
           "existing reports are never overwritten");

    expect(writeBytes(pngPath, kChangedOnePixelPng,
                      sizeof(kChangedOnePixelPng)),
           "same-format replacement PNG is written");
    expect(!validateBoundPng(capture, error) &&
               error.find("changed after") != std::string::npos,
           "downstream use detects same-size RGB file replacement by digest");
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {capture}, error),
           "same-size same-product replacement cannot enter a report");
    expect(writeBytes(pngPath, kOnePixelPng, sizeof(kOnePixelPng)) &&
               validateBoundPng(capture, error),
           "restoring the exact captured bytes restores eligibility");

    Capture invalid = capture;
    invalid.pngPath = truncatedPath;
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "truncated PNGs cannot enter a report");
    invalid = capture;
    invalid.renderProduct = RenderProduct::ModelAlpha;
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "render-product metadata cannot mislabel an RGB scene as RGBA");
    invalid = capture;
    invalid.pngPath = corruptPath;
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "PNG pixel corruption cannot enter a report even when framed");
    invalid = capture;
    invalid.width = 2u;
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "changed PNG dimensions fail their recorded contract");
    invalid = capture;
    invalid.sourceSha256[0] = 'A';
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "non-canonical source digests are rejected");
    invalid = capture;
    invalid.pngSha256[0] = invalid.pngSha256[0] == '0' ? '1' : '0';
    expect(!validateBoundPng(invalid, error),
           "downstream capture use refuses a changed image identity");
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "capture bytes changed after publication cannot enter a report");
    invalid = capture;
    invalid.stableFrames = 0u;
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "unstabilized captures cannot enter a qualification report");
    invalid = capture;
    invalid.stableFrames = 11u;
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "partially stabilized captures cannot enter a qualification report");
    invalid = capture;
    invalid.viewPitchDegrees = 91;
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "out-of-range vertical camera metadata is rejected");
    invalid = capture;
    invalid.context = "line\nbreak";
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "control characters cannot enter report metadata");
    invalid = capture;
    invalid.pose = std::string("bad") + static_cast<char>(0xC0) +
                   static_cast<char>(0xAF);
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie",
                       {invalid}, error),
           "malformed UTF-8 cannot enter report metadata");
    expect(!exportHtml("wrong-suffix.htm", "dixie.cc0", "Dixie",
                       captures, error),
           "report destination requires the canonical lowercase suffix");
    expect(!exportHtml(otherReportPath, "Dixie/unsafe", "Dixie",
                       captures, error),
           "package identity is validated independently of display text");
    expect(!exportHtml(otherReportPath, "dixie.cc0", "Dixie", {}, error),
           "empty contact sheets are rejected");

    (void)mdkr_remove_utf8(pngPath.c_str());
    (void)mdkr_remove_utf8(truncatedPath.c_str());
    (void)mdkr_remove_utf8(rgbaPath.c_str());
    (void)mdkr_remove_utf8(corruptPath.c_str());
    (void)mdkr_remove_utf8(reportPath.c_str());
    (void)mdkr_remove_utf8(otherReportPath.c_str());
    if (failures != 0) return 1;
    std::puts("character visual report passed");
    return 0;
}
