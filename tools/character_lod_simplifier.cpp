/* Bounded stdin/stdout bridge to the pinned meshoptimizer simplifier. */
#include "meshoptimizer.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr uint32_t kVersion = 1u;
constexpr uint32_t kMaximumVertices = 1000000u;
constexpr uint32_t kMaximumIndices = 6000000u;
constexpr uint32_t kMaximumAttributes = 32u;
constexpr size_t kMaximumInputBytes = 256u * 1024u * 1024u;
constexpr unsigned char kMagic[8] = {'M', 'D', 'K', 'R', 'L', 'O', 'D', '1'};

bool checked_add(size_t left, size_t right, size_t &result) {
    if (right > std::numeric_limits<size_t>::max() - left) return false;
    result = left + right;
    return true;
}

bool checked_multiply(size_t left, size_t right, size_t &result) {
    if (left != 0u && right > std::numeric_limits<size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

uint32_t read_u32(const unsigned char *bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8u) |
        (static_cast<uint32_t>(bytes[2]) << 16u) |
        (static_cast<uint32_t>(bytes[3]) << 24u);
}

float read_f32(const unsigned char *bytes) {
    const uint32_t bits = read_u32(bytes);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits), "FLOAT must be binary32");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void write_u32(uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value),
        static_cast<unsigned char>(value >> 8u),
        static_cast<unsigned char>(value >> 16u),
        static_cast<unsigned char>(value >> 24u),
    };
    (void)std::fwrite(bytes, 1u, sizeof(bytes), stdout);
}

void write_f32(float value) {
    uint32_t bits = 0u;
    std::memcpy(&bits, &value, sizeof(bits));
    write_u32(bits);
}

bool read_stdin(std::vector<unsigned char> &bytes) {
    unsigned char chunk[16384];
    while (!std::feof(stdin)) {
        const size_t count = std::fread(chunk, 1u, sizeof(chunk), stdin);
        if (count != 0u) {
            if (bytes.size() > kMaximumInputBytes - count) return false;
            bytes.insert(bytes.end(), chunk, chunk + count);
        }
        if (std::ferror(stdin)) return false;
    }
    return true;
}

int fail(const char *message) {
    std::fprintf(stderr, "mdkr-character-lod: %s\n", message);
    return 2;
}

} // namespace

int main() {
    std::vector<unsigned char> input;
    if (!read_stdin(input)) {
        return fail("input exceeds 256 MiB or could not be read");
    }
    if (input.size() < 36u ||
        std::memcmp(input.data(), kMagic, sizeof(kMagic)) != 0) {
        return fail("request header is invalid");
    }
    const uint32_t version = read_u32(input.data() + 8u);
    const uint32_t vertex_count = read_u32(input.data() + 12u);
    const uint32_t index_count = read_u32(input.data() + 16u);
    const uint32_t attribute_count = read_u32(input.data() + 20u);
    const uint32_t target_index_count = read_u32(input.data() + 24u);
    const uint32_t options = read_u32(input.data() + 28u);
    const float target_error = read_f32(input.data() + 32u);
    const uint32_t supported_options =
        meshopt_SimplifyLockBorder | meshopt_SimplifyRegularizeLight;
    if (version != kVersion || vertex_count < 3u ||
        vertex_count > kMaximumVertices || index_count < 3u ||
        index_count > kMaximumIndices || index_count % 3u != 0u ||
        attribute_count == 0u || attribute_count > kMaximumAttributes ||
        target_index_count < 3u || target_index_count > index_count ||
        target_index_count % 3u != 0u ||
        !std::isfinite(target_error) || target_error < 0.0f ||
        target_error > 1.0f || (options & ~supported_options) != 0u) {
        return fail("request limits or simplification settings are invalid");
    }

    size_t position_values = 0u;
    size_t attribute_values = 0u;
    size_t expected_size = 36u;
    if (!checked_multiply(vertex_count, 3u, position_values) ||
        !checked_multiply(vertex_count, attribute_count, attribute_values)) {
        return fail("request size overflows the host address space");
    }
    const size_t value_counts[] = {
        position_values, attribute_values,
        static_cast<size_t>(attribute_count),
        static_cast<size_t>(index_count),
    };
    for (const size_t values : value_counts) {
        size_t bytes = 0u;
        if (!checked_multiply(values, 4u, bytes) ||
            !checked_add(expected_size, bytes, expected_size)) {
            return fail("request size overflows the host address space");
        }
    }
    if (expected_size != input.size()) {
        return fail("request length is not canonical");
    }

    size_t offset = 36u;
    std::vector<float> positions(position_values);
    std::vector<float> attributes(attribute_values);
    std::vector<float> weights(attribute_count);
    std::vector<unsigned int> indices(index_count);
    for (float &value : positions) {
        value = read_f32(input.data() + offset);
        offset += 4u;
        if (!std::isfinite(value)) return fail("positions must be finite");
    }
    for (float &value : attributes) {
        value = read_f32(input.data() + offset);
        offset += 4u;
        if (!std::isfinite(value)) return fail("attributes must be finite");
    }
    bool has_weight = false;
    for (float &value : weights) {
        value = read_f32(input.data() + offset);
        offset += 4u;
        if (!std::isfinite(value) || value < 0.0f) {
            return fail("attribute weights must be finite and non-negative");
        }
        has_weight = has_weight || value > 0.0f;
    }
    if (!has_weight) {
        return fail("at least one attribute weight is required");
    }
    for (unsigned int &index : indices) {
        index = read_u32(input.data() + offset);
        offset += 4u;
        if (index >= vertex_count) {
            return fail("an index exceeds the vertex count");
        }
    }

    std::vector<unsigned int> simplified(index_count);
    float result_error = 0.0f;
    const size_t result_count = meshopt_simplifyWithAttributes(
        simplified.data(), indices.data(), indices.size(), positions.data(),
        vertex_count, sizeof(float) * 3u, attributes.data(),
        sizeof(float) * attribute_count, weights.data(), attribute_count,
        nullptr, target_index_count, target_error, options, &result_error);
    if (result_count < 3u || result_count > index_count ||
        result_count % 3u != 0u || !std::isfinite(result_error) ||
        result_error < 0.0f) {
        return fail("simplifier returned an invalid bounded result");
    }

    (void)std::fwrite(kMagic, 1u, sizeof(kMagic), stdout);
    write_u32(kVersion);
    write_u32(static_cast<uint32_t>(result_count));
    write_f32(result_error);
    for (size_t index = 0u; index < result_count; ++index) {
        write_u32(simplified[index]);
    }
    if (std::ferror(stdout) || std::fflush(stdout) != 0) {
        return fail("result could not be written");
    }
    return 0;
}
