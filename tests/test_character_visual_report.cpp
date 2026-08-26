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
    capture.context = "Car <script>";
    capture.pose = "Race steer & lean";
    capture.lighting = "Bright";
    capture.players = 4u;
    capture.phaseMilli = 875u;
    capture.viewYawDegrees = -90;
    capture.viewPitchDegrees = 25;
    capture.width = 1u;
    capture.height = 1u;
    capture.stableFrames = 12u;
    capture.exactPose = true;
    Capture alphaCapture = capture;
    alphaCapture.pngPath = rgbaPath;
    alphaCapture.renderProduct = RenderProduct::ModelAlpha;
    std::vector<Capture> captures{capture, alphaCapture};
    std::string error;
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
    expect(report.find("\"version\":2") != std::string::npos &&
               report.find("\"renderProduct\":\"scene\"") !=
                   std::string::npos &&
               report.find("\"renderProduct\":\"model-alpha\"") !=
                   std::string::npos &&
               report.find("\"sourceSha256\":\"") != std::string::npos &&
               report.find("\"fitSha256\":\"") != std::string::npos &&
               report.find("\"stableFrames\":12") != std::string::npos &&
               report.find("\"exactPose\":true") != std::string::npos,
           "portable report includes machine-readable qualification identity");
    expect(report.find(pngPath) == std::string::npos &&
               report.find("pngPath") == std::string::npos,
           "report does not disclose original capture paths");

    const std::string firstReport = report;
    expect(!exportHtml(reportPath, "dixie.cc0", hostileName,
                       captures, error) && readText(reportPath) == firstReport,
           "existing reports are never overwritten");

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
