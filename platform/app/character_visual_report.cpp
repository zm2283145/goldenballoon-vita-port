#include "character_visual_report.h"

#include "engine_entry.h"
#include "character_png_validation.h"
#include "fs_utf8.h"
#include "sha256.h"
#include "workshop_preview_runtime.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr size_t kMaximumPngBytes = 32u * 1024u * 1024u;
constexpr size_t kMaximumTotalPngBytes = 128u * 1024u * 1024u;

bool digestValid(const std::string &value) {
    return value.size() == 64u &&
        std::all_of(value.begin(), value.end(), [](char byte) {
            return (byte >= '0' && byte <= '9') ||
                   (byte >= 'a' && byte <= 'f');
        });
}

bool slugValid(const std::string &value) {
    if (value.size() < 2u || value.size() > 64u) return false;
    return std::all_of(value.begin(), value.end(), [](char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '.' ||
               byte == '_' || byte == '-';
    });
}

bool utf8Valid(const std::string &value) {
    size_t offset = 0u;
    while (offset < value.size()) {
        const unsigned char first =
            static_cast<unsigned char>(value[offset]);
        size_t continuation = 0u;
        uint32_t codepoint = 0u;
        if (first <= 0x7Fu) {
            ++offset;
            continue;
        } else if (first >= 0xC2u && first <= 0xDFu) {
            continuation = 1u;
            codepoint = first & 0x1Fu;
        } else if (first >= 0xE0u && first <= 0xEFu) {
            continuation = 2u;
            codepoint = first & 0x0Fu;
        } else if (first >= 0xF0u && first <= 0xF4u) {
            continuation = 3u;
            codepoint = first & 0x07u;
        } else {
            return false;
        }
        if (continuation > value.size() - offset - 1u) return false;
        for (size_t index = 1u; index <= continuation; ++index) {
            const unsigned char byte =
                static_cast<unsigned char>(value[offset + index]);
            if ((byte & 0xC0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (byte & 0x3Fu);
        }
        if ((continuation == 2u && codepoint < 0x800u) ||
            (continuation == 3u && codepoint < 0x10000u) ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
            codepoint > 0x10FFFFu) return false;
        offset += continuation + 1u;
    }
    return true;
}

bool textValid(const std::string &value, size_t maximum, bool required) {
    if (value.size() > maximum || (required && value.empty()) ||
        !utf8Valid(value)) return false;
    return std::none_of(value.begin(), value.end(), [](char byte) {
        const unsigned char value = static_cast<unsigned char>(byte);
        return value < 0x20u && value != '\t';
    });
}

const char *renderProductName(
    CharacterVisualReport::RenderProduct product) {
    switch (product) {
        case CharacterVisualReport::RenderProduct::Scene:
            return "Gameplay frame";
        case CharacterVisualReport::RenderProduct::ModelAlpha:
            return "Model only · transparent";
        default:
            return nullptr;
    }
}

const char *renderProductToken(
    CharacterVisualReport::RenderProduct product) {
    switch (product) {
        case CharacterVisualReport::RenderProduct::Scene: return "scene";
        case CharacterVisualReport::RenderProduct::ModelAlpha:
            return "model-alpha";
        default: return nullptr;
    }
}

bool readPng(const CharacterVisualReport::Capture &capture,
             std::vector<unsigned char> &bytes, std::string &sha,
             std::string &error) {
    FILE *file = mdkr_fopen_utf8(capture.pngPath.c_str(), "rb");
    if (file == nullptr) {
        error = "A capture PNG could not be opened.";
        return false;
    }
    std::vector<unsigned char> loaded;
    std::array<unsigned char, 8192> block{};
    for (;;) {
        const size_t count = std::fread(block.data(), 1u, block.size(), file);
        if (count != 0u) {
            if (loaded.size() > kMaximumPngBytes - count) {
                (void)std::fclose(file);
                error = "A capture PNG exceeds the 32 MiB report limit.";
                return false;
            }
            loaded.insert(loaded.end(), block.begin(), block.begin() + count);
        }
        if (count != block.size()) break;
    }
    const bool readOk = std::ferror(file) == 0;
    const bool closeOk = std::fclose(file) == 0;
    CharacterPngValidation::Info info;
    if (!readOk || !closeOk) {
        error = "A capture PNG could not be read completely.";
        return false;
    }
    if (!CharacterPngValidation::validate(
            loaded.data(), loaded.size(), capture.width, capture.height,
            info, error)) {
        if (error.empty()) error = "A capture is not a complete PNG.";
        return false;
    }
    const uint8_t expectedColourType =
        capture.renderProduct ==
                CharacterVisualReport::RenderProduct::ModelAlpha
            ? 6u : 2u;
    if (info.colourType != expectedColourType) {
        error = "A capture PNG does not match its recorded render product.";
        return false;
    }
    char digest[MDKR_SHA256_HEX_SIZE];
    mdkr_sha256_hex(loaded.data(), loaded.size(), digest);
    sha = digest;
    if (!capture.pngSha256.empty() && capture.pngSha256 != sha) {
        error = "A capture PNG changed after it entered the session tray.";
        return false;
    }
    bytes = std::move(loaded);
    return true;
}

std::string htmlEscape(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char byte : value) {
        switch (byte) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped.push_back(byte); break;
        }
    }
    return escaped;
}

std::string jsonEscape(const std::string &value) {
    static const char digits[] = "0123456789abcdef";
    std::string escaped;
    for (unsigned char byte : value) {
        switch (byte) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            case '<': escaped += "\\u003c"; break;
            case '>': escaped += "\\u003e"; break;
            case '&': escaped += "\\u0026"; break;
            default:
                if (byte < 0x20u) {
                    escaped += "\\u00";
                    escaped.push_back(digits[byte >> 4u]);
                    escaped.push_back(digits[byte & 0xFu]);
                } else {
                    escaped.push_back(static_cast<char>(byte));
                }
                break;
        }
    }
    return escaped;
}

std::string base64(const std::vector<unsigned char> &bytes) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2u) / 3u) * 4u);
    for (size_t offset = 0u; offset < bytes.size(); offset += 3u) {
        const uint32_t a = bytes[offset];
        const uint32_t b = offset + 1u < bytes.size() ? bytes[offset + 1u] : 0u;
        const uint32_t c = offset + 2u < bytes.size() ? bytes[offset + 2u] : 0u;
        const uint32_t value = (a << 16u) | (b << 8u) | c;
        output.push_back(alphabet[(value >> 18u) & 63u]);
        output.push_back(alphabet[(value >> 12u) & 63u]);
        output.push_back(offset + 1u < bytes.size()
                             ? alphabet[(value >> 6u) & 63u] : '=');
        output.push_back(offset + 2u < bytes.size()
                             ? alphabet[value & 63u] : '=');
    }
    return output;
}

bool captureMetadataValid(const CharacterVisualReport::Capture &capture,
                          bool requirePngDigest) {
    const auto rectValid = [&capture](const std::array<int32_t, 4> &rect) {
        return rect[0] >= 0 && rect[1] >= 0 && rect[2] > 0 && rect[3] > 0 &&
            static_cast<int64_t>(rect[0]) + rect[2] <= capture.width &&
            static_cast<int64_t>(rect[1]) + rect[3] <= capture.height;
    };
    const auto &projection = capture.fitProjection;
    bool projectionValid = false;
    if (projection.valid) {
        projectionValid =
            capture.renderProduct ==
                CharacterVisualReport::RenderProduct::ModelAlpha &&
            projection.width == capture.width &&
            projection.height == capture.height &&
            projection.primitiveDraws >= 1u &&
            projection.primitiveDraws <= 4096u &&
            rectValid(projection.viewport) && rectValid(projection.scissor) &&
            std::all_of(
                projection.clipFlags.begin(), projection.clipFlags.end(),
                [](uint32_t flags) { return (flags & ~0x7fu) == 0u; });
        if (projectionValid) {
            const int64_t viewportLeft =
                static_cast<int64_t>(projection.viewport[0]) * 1000;
            const int64_t viewportTop =
                static_cast<int64_t>(projection.viewport[1]) * 1000;
            const int64_t viewportRight = static_cast<int64_t>(
                projection.viewport[0] + projection.viewport[2]) * 1000;
            const int64_t viewportBottom = static_cast<int64_t>(
                projection.viewport[1] + projection.viewport[3]) * 1000;
            const int64_t scissorLeft =
                static_cast<int64_t>(projection.scissor[0]) * 1000;
            const int64_t scissorTop =
                static_cast<int64_t>(projection.scissor[1]) * 1000;
            const int64_t scissorRight = static_cast<int64_t>(
                projection.scissor[0] + projection.scissor[2]) * 1000;
            const int64_t scissorBottom = static_cast<int64_t>(
                projection.scissor[1] + projection.scissor[3]) * 1000;
            constexpr int64_t kTolerance = 1;
            for (size_t point = 0u;
                 point < CharacterVisualReport::kFitProjectionPoints;
                 ++point) {
                const int64_t x = projection.pixelMilli[point][0];
                const int64_t y = projection.pixelMilli[point][1];
                const int32_t depth = projection.depthMillionths[point];
                const uint32_t flags = projection.clipFlags[point];
                const bool outsideScissor =
                    x < scissorLeft - kTolerance ||
                    x > scissorRight + kTolerance ||
                    y < scissorTop - kTolerance ||
                    y > scissorBottom + kTolerance;
                if ((x < viewportLeft - kTolerance) !=
                        ((flags & 0x01u) != 0u) ||
                    (x > viewportRight + kTolerance) !=
                        ((flags & 0x02u) != 0u) ||
                    (y < viewportTop - kTolerance) !=
                        ((flags & 0x04u) != 0u) ||
                    (y > viewportBottom + kTolerance) !=
                        ((flags & 0x08u) != 0u) ||
                    (depth < 0) != ((flags & 0x10u) != 0u) ||
                    (depth > 1000000) != ((flags & 0x20u) != 0u) ||
                    outsideScissor != ((flags & 0x40u) != 0u)) {
                    projectionValid = false;
                    break;
                }
            }
        }
    } else {
        projectionValid = projection.width == 0u && projection.height == 0u &&
            projection.primitiveDraws == 0u &&
            std::all_of(projection.viewport.begin(), projection.viewport.end(),
                        [](int32_t value) { return value == 0; }) &&
            std::all_of(projection.scissor.begin(), projection.scissor.end(),
                        [](int32_t value) { return value == 0; }) &&
            std::all_of(
                projection.pixelMilli.begin(), projection.pixelMilli.end(),
                [](const std::array<int32_t, 2> &point) {
                    return point[0] == 0 && point[1] == 0;
                }) &&
            std::all_of(
                projection.depthMillionths.begin(),
                projection.depthMillionths.end(),
                [](int32_t value) { return value == 0; }) &&
            std::all_of(
                projection.clipFlags.begin(), projection.clipFlags.end(),
                [](uint32_t value) { return value == 0u; });
    }
    if (capture.renderProduct ==
            CharacterVisualReport::RenderProduct::ModelAlpha &&
        !projection.valid) projectionValid = false;
    return projectionValid &&
           textValid(capture.pngPath, 4095u, true) &&
           (requirePngDigest ? digestValid(capture.pngSha256)
                             : capture.pngSha256.empty()) &&
           digestValid(capture.sourceSha256) &&
           digestValid(capture.fitSha256) &&
           textValid(capture.context, 32u, true) &&
           textValid(capture.pose, 64u, true) &&
           textValid(capture.lighting, 32u, true) &&
           renderProductName(capture.renderProduct) != nullptr &&
           capture.players >= 1u && capture.players <= 4u &&
           capture.phaseMilli <= 1000u &&
           capture.viewYawDegrees >= -180 &&
           capture.viewYawDegrees <= 180 &&
           capture.viewPitchDegrees >=
               MDKR_WORKSHOP_PREVIEW_PITCH_MIN_DEGREES &&
           capture.viewPitchDegrees <=
               MDKR_WORKSHOP_PREVIEW_PITCH_MAX_DEGREES &&
           capture.width >= 1u && capture.width <= 16384u &&
           capture.height >= 1u && capture.height <= 16384u &&
           capture.stableFrames >=
               MDKR_CHARACTER_PREVIEW_CAPTURE_STABLE_FRAMES;
}

} // namespace

namespace CharacterVisualReport {

bool bindPng(Capture &capture, std::string &error) {
    if (!captureMetadataValid(capture, false)) {
        error = "The capture metadata is invalid.";
        return false;
    }
    std::vector<unsigned char> bytes;
    std::string digest;
    if (!readPng(capture, bytes, digest, error)) return false;
    capture.pngSha256 = std::move(digest);
    error.clear();
    return true;
}

bool validateBoundPng(const Capture &capture, std::string &error) {
    if (!captureMetadataValid(capture, true)) {
        error = "The digest-bound capture metadata is invalid.";
        return false;
    }
    std::vector<unsigned char> bytes;
    std::string digest;
    if (!readPng(capture, bytes, digest, error)) return false;
    error.clear();
    return true;
}

bool exportHtml(const std::string &outputPath,
                const std::string &packageId,
                const std::string &displayName,
                const std::vector<Capture> &captures,
                std::string &error) {
    const bool htmlSuffix = outputPath.size() >= 5u &&
        outputPath.compare(outputPath.size() - 5u, 5u, ".html") == 0;
    if (!htmlSuffix || outputPath.size() > 4095u ||
        !slugValid(packageId) || !textValid(displayName, 96u, true) ||
        captures.empty() || captures.size() > kMaximumCaptures ||
        !std::all_of(captures.begin(), captures.end(),
                     [](const Capture &capture) {
                         return captureMetadataValid(capture, true);
                     })) {
        error = "The visual report request is invalid or empty.";
        return false;
    }
    struct Loaded {
        const Capture *capture;
        std::vector<unsigned char> png;
        std::string sha;
    };
    std::vector<Loaded> loaded;
    size_t totalBytes = 0u;
    for (const Capture &capture : captures) {
        Loaded item{&capture, {}, {}};
        if (!readPng(capture, item.png, item.sha, error) ||
            totalBytes > kMaximumTotalPngBytes - item.png.size()) {
            if (error.empty()) {
                error = "The contact sheet exceeds the 128 MiB image limit.";
            }
            return false;
        }
        totalBytes += item.png.size();
        loaded.push_back(std::move(item));
    }

    std::string html;
    html.reserve(totalBytes * 4u / 3u + loaded.size() * 2048u + 4096u);
    html += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
    html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    html += "<title>" + htmlEscape(displayName) + " — visual qualification</title>";
    html += "<style>body{margin:0;background:#11151b;color:#edf2f7;font:16px system-ui,sans-serif}";
    html += "main{max-width:1600px;margin:auto;padding:24px}h1{margin:.2em 0}.sub{color:#aeb9c7;word-break:break-all}";
    html += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:18px;margin-top:24px}";
    html += "figure{margin:0;background:#1d2530;border:1px solid #344252;border-radius:12px;overflow:hidden}";
    html += ".shot{position:relative}.shot img{display:block;width:100%;height:auto;background-color:#161b22;background-image:linear-gradient(45deg,#303844 25%,transparent 25%),linear-gradient(-45deg,#303844 25%,transparent 25%),linear-gradient(45deg,transparent 75%,#303844 75%),linear-gradient(-45deg,transparent 75%,#303844 75%);background-size:24px 24px;background-position:0 0,0 12px,12px -12px,-12px 0}.shot svg{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}.bounds{stroke:#ffd166;stroke-width:2;fill:none;vector-effect:non-scaling-stroke}.anchor{fill:#ff6bd6;stroke:#181d25;stroke-width:1;vector-effect:non-scaling-stroke}.forward{stroke:#5cc8ff;stroke-width:3;vector-effect:non-scaling-stroke}figcaption{padding:14px;line-height:1.45}";
    html += ".ok{color:#8ee6a8}.warn{color:#ffd37a}code{font-size:.8em;word-break:break-all}";
    html += "@media print{body{background:white;color:black}figure{break-inside:avoid;border-color:#aaa}.sub{color:#444}}</style></head><body>";
    html += "<main><h1>" + htmlEscape(displayName) + "</h1>";
    html += "<div class=\"sub\">Package " + htmlEscape(packageId) +
        " · self-contained exact-renderer contact sheet · no model or ROM bytes</div><div class=\"grid\">";
    for (const Loaded &item : loaded) {
        const Capture &capture = *item.capture;
        html += "<figure><div class=\"shot\"><img alt=\"" + htmlEscape(
            capture.context + ", " + capture.pose) +
            "\" src=\"data:image/png;base64," + base64(item.png) + "\">";
        if (capture.fitProjection.valid) {
            static constexpr unsigned edges[][2] = {
                {0, 1}, {2, 3}, {4, 5}, {6, 7},
                {0, 2}, {1, 3}, {4, 6}, {5, 7},
                {0, 4}, {1, 5}, {2, 6}, {3, 7},
            };
            html += "<svg role=\"img\" aria-label=\"Registered calibrated bounds, anchor, and forward direction\" viewBox=\"0 0 " +
                std::to_string(capture.width * 1000) + " " +
                std::to_string(capture.height * 1000) + "\">";
            for (const auto &edge : edges) {
                const auto &a = capture.fitProjection.pixelMilli[edge[0]];
                const auto &b = capture.fitProjection.pixelMilli[edge[1]];
                html += "<line class=\"bounds\" x1=\"" +
                    std::to_string(a[0]) + "\" y1=\"" +
                    std::to_string(a[1]) + "\" x2=\"" +
                    std::to_string(b[0]) + "\" y2=\"" +
                    std::to_string(b[1]) + "\"/>";
            }
            const auto &anchor = capture.fitProjection.pixelMilli[8];
            const auto &forward = capture.fitProjection.pixelMilli[9];
            html += "<line class=\"forward\" x1=\"" +
                std::to_string(anchor[0]) + "\" y1=\"" +
                std::to_string(anchor[1]) + "\" x2=\"" +
                std::to_string(forward[0]) + "\" y2=\"" +
                std::to_string(forward[1]) + "\"/>";
            html += "<circle class=\"anchor\" cx=\"" +
                std::to_string(anchor[0]) + "\" cy=\"" +
                std::to_string(anchor[1]) + "\" r=\"5000\"/></svg>";
        }
        html += "</div>";
        html += "<figcaption><strong>" + htmlEscape(capture.context) +
            " · " + htmlEscape(capture.pose) + "</strong><br>";
        html += htmlEscape(renderProductName(capture.renderProduct)) +
            " · ";
        html += "Phase " + std::to_string(capture.phaseMilli) +
            "/1000 · " + htmlEscape(capture.lighting) +
            " light · view " + std::to_string(capture.viewYawDegrees) +
            "°/" + std::to_string(capture.viewPitchDegrees) +
            "° · " + std::to_string(capture.stableFrames) +
            " stable frames<br>";
        html += capture.exactPose
            ? "<span class=\"ok\">Exact semantic phase</span>"
            : "<span class=\"warn\">Source fallback shown</span>";
        html += "<br><code>PNG SHA-256 " + item.sha + "</code></figcaption></figure>";
    }
    html += "</div><script id=\"mdkr-character-visual-report\" type=\"application/json\">{";
    html += "\"version\":3,\"packageId\":\"" + jsonEscape(packageId) +
        "\",\"displayName\":\"" + jsonEscape(displayName) +
        "\",\"captures\":[";
    for (size_t index = 0u; index < loaded.size(); ++index) {
        const Capture &capture = *loaded[index].capture;
        if (index != 0u) html += ',';
        html += "{\"context\":\"" + jsonEscape(capture.context) +
            "\",\"players\":" + std::to_string(capture.players) +
            ",\"renderProduct\":\"" +
            renderProductToken(capture.renderProduct) + "\"" +
            ",\"pose\":\"" + jsonEscape(capture.pose) +
            "\",\"phaseMilli\":" + std::to_string(capture.phaseMilli) +
            ",\"lighting\":\"" + jsonEscape(capture.lighting) +
            "\",\"viewYawDegrees\":" +
            std::to_string(capture.viewYawDegrees) +
            ",\"viewPitchDegrees\":" +
            std::to_string(capture.viewPitchDegrees) +
            ",\"width\":" + std::to_string(capture.width) +
            ",\"height\":" + std::to_string(capture.height) +
            ",\"stableFrames\":" +
            std::to_string(capture.stableFrames) +
            ",\"exactPose\":" +
            std::string(capture.exactPose ? "true" : "false") +
            ",\"sourceSha256\":\"" + capture.sourceSha256 +
            "\",\"fitSha256\":\"" + capture.fitSha256 +
            "\",\"pngSha256\":\"" + capture.pngSha256 + "\"";
        if (capture.fitProjection.valid) {
            const auto &projection = capture.fitProjection;
            html += ",\"fitProjection\":{\"width\":" +
                std::to_string(projection.width) + ",\"height\":" +
                std::to_string(projection.height) +
                ",\"primitiveDraws\":" +
                std::to_string(projection.primitiveDraws) +
                ",\"viewport\":[";
            for (size_t component = 0u; component < 4u; ++component) {
                if (component != 0u) html += ',';
                html += std::to_string(projection.viewport[component]);
            }
            html += "],\"scissor\":[";
            for (size_t component = 0u; component < 4u; ++component) {
                if (component != 0u) html += ',';
                html += std::to_string(projection.scissor[component]);
            }
            html += "],\"points\":[";
            for (size_t point = 0u; point < kFitProjectionPoints; ++point) {
                if (point != 0u) html += ',';
                html += "[" + std::to_string(projection.pixelMilli[point][0]) +
                    "," + std::to_string(projection.pixelMilli[point][1]) +
                    "," + std::to_string(projection.depthMillionths[point]) +
                    "," + std::to_string(projection.clipFlags[point]) + "]";
            }
            html += "]}";
        }
        html += '}';
    }
    html += "]}</script></main></body></html>\n";

    FILE *file = mdkr_fopen_utf8(outputPath.c_str(), "wbx");
    if (file == nullptr) {
        error = "The report destination exists or cannot be created.";
        return false;
    }
    bool written = std::fwrite(html.data(), 1u, html.size(), file) ==
        html.size();
    if (std::fflush(file) != 0 || mdkr_file_sync(file) != 0) written = false;
    if (std::fclose(file) != 0) written = false;
    if (!written) {
        (void)mdkr_remove_utf8(outputPath.c_str());
        error = "The contact sheet could not be written completely.";
        return false;
    }
    (void)mdkr_parent_directory_sync_utf8(outputPath.c_str());
    error.clear();
    return true;
}

} // namespace CharacterVisualReport
