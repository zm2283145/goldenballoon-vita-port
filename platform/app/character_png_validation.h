#ifndef MDKR_APP_CHARACTER_PNG_VALIDATION_H
#define MDKR_APP_CHARACTER_PNG_VALIDATION_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace CharacterPngValidation {

struct Info {
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint8_t bitDepth = 0u;
    uint8_t colourType = 0u;
};

/* Validate the complete bounded PNG container used as Workshop evidence.
 * Zero expected dimensions are wildcards. This proves chunk bounds, ordering,
 * names, CRCs, canonical non-interlaced IHDR fields, palette requirements,
 * at least one nonempty IDAT, and a terminal IEND with no trailing bytes. */
bool validate(const unsigned char *bytes, size_t size,
              uint32_t expectedWidth, uint32_t expectedHeight,
              Info &info, std::string &error);

} // namespace CharacterPngValidation

#endif // MDKR_APP_CHARACTER_PNG_VALIDATION_H
