#ifndef MDKR64_CHARACTER_PREVIEW_CACHE_H
#define MDKR64_CHARACTER_PREVIEW_CACHE_H

#include <cstdint>
#include <string>

namespace CharacterPreviewCache {

constexpr uint32_t kFirstContext = 1u;
constexpr uint32_t kLastContext = 4u;

enum class Subject : uint8_t {
    CustomCharacter = 0,
    RetailDonor,
};

// Reserve the one launcher-owned PNG location for this package/context. Any
// prior regular file at that exact derived location is removed; unrelated
// files, directories, and links are never touched. The renderer still creates
// the PNG exclusively, so this function does not weaken its no-overwrite rule.
bool prepare(const std::string &charactersDirectory,
             const std::string &packageId,
             uint32_t context,
             Subject subject,
             std::string &capturePath,
             std::string &error);

bool prepare(const std::string &charactersDirectory,
             const std::string &packageId,
             uint32_t context,
             std::string &capturePath,
             std::string &error);

// True only for one of the four exact paths derived from this package ID.
bool owns(const std::string &charactersDirectory,
          const std::string &packageId,
          const std::string &capturePath);

// Remove one exact package/context cache file if it is a regular, non-link
// file. Missing files are success. Suspicious filesystem entries are retained.
bool remove(const std::string &charactersDirectory,
            const std::string &packageId,
            uint32_t context);

bool removeOwnedPath(const std::string &charactersDirectory,
                     const std::string &packageId,
                     const std::string &capturePath);

// Best-effort bounded cleanup for package removal and first launcher load.
void removePackage(const std::string &charactersDirectory,
                   const std::string &packageId);

// Remove every regular file matching the exact managed filename grammar from
// the verified cache directory. Used once at launcher start to reconcile files
// left by a normal close, crash, or out-of-band package removal.
bool removeAll(const std::string &charactersDirectory);

}  // namespace CharacterPreviewCache

#endif
