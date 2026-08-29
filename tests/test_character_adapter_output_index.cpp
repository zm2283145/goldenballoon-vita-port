/* Assert-driven test: release builds must not compile its checks away. */
#undef NDEBUG

#include "character_adapter_output_index.h"

#include <cassert>
#include <string>

namespace {

std::string hex(const std::string &value) {
    static const char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : value) {
        result.push_back(digits[byte >> 4u]);
        result.push_back(digits[byte & 0xFu]);
    }
    return result;
}

std::string row(bool licensed = true) {
    const std::string digest(64u, 'a');
    return "mdkr-character-adapter-output-index-v1\n" +
        digest + "\t" + digest + "\t" + digest +
        "\t4096\t2048\t100\t80\t12\t2\t3\t" +
        (licensed ? "1\t" + digest + "\t" : "0\t-\t") +
        hex("Blender exporter") + "\t" + hex("1.2.3") + "\t" +
        hex("https://example.invalid/adapter") + "\t" + hex("FBX") +
        "\t" + hex("Golden Balloon character-v1") + "\t" + digest +
        "\t" + (licensed ? hex("CC-BY-4.0") : "-") +
        "\t" + (licensed ? hex("Artist") : "-") +
        "\t" + (licensed ? hex("https://example.invalid/model") : "-") +
        "\n";
}

}  // namespace

int main() {
    CharacterAdapterOutputIndex::Review review;
    assert(CharacterAdapterOutputIndex::parse(row(), review));
    assert(review.adapterName == "Blender exporter");
    assert(review.adapterVersion == "1.2.3");
    assert(review.sourceFormat == "FBX");
    assert(review.conversionProfile == "Golden Balloon character-v1");
    assert(review.licensePresent && review.licenseSpdx == "CC-BY-4.0");

    CharacterAdapterOutputIndex::Review unlicensed;
    assert(CharacterAdapterOutputIndex::parse(row(false), unlicensed));
    assert(!unlicensed.licensePresent && unlicensed.licenseSpdx.empty());

    CharacterAdapterOutputIndex::Review unchanged = review;
    std::string trailing = row() + "x";
    assert(!CharacterAdapterOutputIndex::parse(trailing, unchanged));
    assert(unchanged.adapterName == review.adapterName);

    std::string malformed = row();
    malformed.replace(malformed.find("\t4096\t"), 6u, "\t04096\t");
    assert(!CharacterAdapterOutputIndex::parse(malformed, unchanged));

    malformed = row(false);
    const size_t missingLicense = malformed.find("\t0\t-\t");
    malformed.replace(missingLicense, 5u, "\t1\t-\t");
    assert(!CharacterAdapterOutputIndex::parse(malformed, unchanged));
    return 0;
}
