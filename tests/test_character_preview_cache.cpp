#include "character_preview_cache.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void writeText(const fs::path &path, const char *text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string utf8(const fs::path &path) {
    return path.u8string();
}

}  // namespace

int main() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("mdkr-character-preview-cache-" + std::to_string(nonce));
    std::error_code ec;
    expect(fs::create_directory(root, ec) && !ec,
           "creates an isolated test root");
    if (ec) return 1;

    const std::string packageId = "artist.example-character";
    std::string capture;
    std::string error;
    expect(CharacterPreviewCache::prepare(
               utf8(root), packageId, 2u, capture, error),
           "prepares a managed context path");
    expect(error.empty(), "successful preparation has no error");
    expect(!capture.empty() && capture.size() < 1024u,
           "managed path satisfies the renderer bound");
    expect(capture.find(packageId) == std::string::npos,
           "managed filename does not disclose a package ID");
    expect(CharacterPreviewCache::owns(
               utf8(root), packageId, capture),
           "recognizes its exact derived path");
    expect(!CharacterPreviewCache::owns(
               utf8(root), "another-character", capture),
           "does not transfer ownership across packages");
    expect(!fs::exists(fs::u8path(capture)),
           "preparation leaves the destination absent for exclusive creation");

    writeText(fs::u8path(capture), "old preview");
    expect(!CharacterPreviewCache::removeOwnedPath(
               utf8(root), "another-character", capture),
           "foreign package cleanup refuses the derived path");
    expect(fs::exists(fs::u8path(capture)),
           "foreign package cleanup preserves the file");
    expect(CharacterPreviewCache::prepare(
               utf8(root), packageId, 2u, capture, error),
           "re-prepares the same owned slot");
    expect(!fs::exists(fs::u8path(capture)),
           "re-preparation removes only the prior regular cache file");

    const fs::path unrelated = root / ".workshop-preview-cache" /
        "keep-me.txt";
    writeText(unrelated, "unrelated");
    for (uint32_t context = CharacterPreviewCache::kFirstContext;
         context <= CharacterPreviewCache::kLastContext; ++context) {
        std::string path;
        expect(CharacterPreviewCache::prepare(
                   utf8(root), packageId, context, path, error),
               "prepares every bounded context");
        writeText(fs::u8path(path), "preview");
    }
    CharacterPreviewCache::removePackage(utf8(root), packageId);
    expect(fs::exists(unrelated),
           "package cleanup preserves unrelated cache-directory files");
    for (uint32_t context = CharacterPreviewCache::kFirstContext;
         context <= CharacterPreviewCache::kLastContext; ++context) {
        std::string path;
        expect(CharacterPreviewCache::prepare(
                   utf8(root), packageId, context, path, error),
               "package cleanup leaves every slot reusable");
        expect(!fs::exists(fs::u8path(path)),
               "package cleanup removed the prior exact slot");
    }

    expect(!CharacterPreviewCache::prepare(
               utf8(root), packageId, 0u, capture, error),
           "rejects a context below the contract");
    expect(!CharacterPreviewCache::prepare(
               utf8(root), std::string(65u, 'a'), 1u, capture, error),
           "rejects an overlong package identity");

    std::string firstStale;
    std::string secondStale;
    expect(CharacterPreviewCache::prepare(
               utf8(root), packageId, 1u, firstStale, error) &&
           CharacterPreviewCache::prepare(
               utf8(root), "removed-out-of-band", 4u,
               secondStale, error),
           "prepares stale-startup fixtures across packages");
    writeText(fs::u8path(firstStale), "stale one");
    writeText(fs::u8path(secondStale), "stale two");
    const fs::path lookalike = root / ".workshop-preview-cache" /
        (std::string(64u, 'g') + "-1.png");
    writeText(lookalike, "not managed");
    CharacterPreviewCache::removeAll(utf8(root));
    expect(!fs::exists(fs::u8path(firstStale)) &&
               !fs::exists(fs::u8path(secondStale)),
           "startup cleanup removes exact managed files for known and removed packages");
    expect(fs::exists(unrelated) && fs::exists(lookalike),
           "startup cleanup preserves unrelated and lookalike files");

    expect(CharacterPreviewCache::prepare(
               utf8(root), packageId, 3u, capture, error),
           "prepares a slot for the link-safety case");
    const fs::path outside = root / "outside.txt";
    writeText(outside, "outside");
    ec.clear();
    fs::create_symlink(outside, fs::u8path(capture), ec);
    if (!ec) {
        expect(!CharacterPreviewCache::prepare(
                   utf8(root), packageId, 3u, capture, error),
               "refuses to remove a link at an owned filename");
        expect(fs::exists(outside),
               "link refusal preserves the external target");
    }

    const fs::path linkedRoot = root / "linked-root";
    const fs::path linkedTarget = root / "linked-target";
    fs::create_directory(linkedRoot, ec);
    ec.clear();
    fs::create_directory(linkedTarget, ec);
    ec.clear();
    fs::create_directory_symlink(
        linkedTarget, linkedRoot / ".workshop-preview-cache", ec);
    if (!ec) {
        expect(!CharacterPreviewCache::prepare(
                   utf8(linkedRoot), packageId, 1u, capture, error),
               "refuses a linked cache directory");
        expect(fs::is_empty(linkedTarget),
               "linked-directory refusal does not write through the link");
    }

    fs::remove_all(root, ec);
    expect(!ec, "cleans the isolated test root");
    return failures == 0 ? 0 : 1;
}
