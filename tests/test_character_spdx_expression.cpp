#include "character_spdx_expression.h"

#include <cstdio>
#include <string>

namespace {

int failures;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void expectExpression(const char *expression, bool expected) {
    std::string error;
    const bool valid = CharacterSpdxExpression::validate(expression, error);
    if (valid != expected || (valid && !error.empty()) ||
        (!valid && error.empty())) {
        std::fprintf(stderr, "FAIL: SPDX %s should be %s (%s)\n",
                     expression, expected ? "valid" : "invalid",
                     error.c_str());
        ++failures;
    }
}

} // namespace

int main() {
    for (const char *expression : {
             "MIT",
             "CC-BY-4.0",
             "GPL-2.0-or-later+",
             "MIT OR Apache-2.0",
             "MIT or Apache-2.0",
             "MIT AND (Apache-2.0 OR BSD-3-Clause)",
             "GPL-2.0-only WITH Classpath-exception-2.0",
             "GPL-2.0-only with AdditionRef-Artist-Permission",
             "LicenseRef-Community-Grant",
             "LicenseRef-.community-",
             "DocumentRef-Pack:LicenseRef-Artist-Terms",
             "DocumentRef-.pack-:LicenseRef-.artist-terms",
             "GPL-2.0-only WITH DocumentRef-Pack:AdditionRef-Terms",
         }) {
        expectExpression(expression, true);
    }
    for (const char *expression : {
             "",
             "   ",
             "NONE",
             "NOASSERTION",
             "MIT Or Apache-2.0",
             "MIT Apache-2.0",
             "MIT OR",
             "OR MIT",
             "(MIT OR Apache-2.0",
             "MIT OR Apache-2.0)",
             "(MIT OR Apache-2.0) WITH Classpath-exception-2.0",
             "MIT WITH",
             "MIT WITH LicenseRef-Exception",
             "MIT WITH AdditionRef-",
             "AdditionRef-Artist-Permission",
             "LicenseRef-",
             "LicenseRef-Custom+",
             "GPL-2.0 +",
             "DocumentRef-Pack:MIT",
             "DocumentRef-:LicenseRef-Terms",
             "MIT/Apache-2.0",
         }) {
        expectExpression(expression, false);
    }
    std::string nonAscii = "MIT ";
    nonAscii.push_back(static_cast<char>(0xE2));
    nonAscii.push_back(static_cast<char>(0x88));
    nonAscii.push_back(static_cast<char>(0xA8));
    nonAscii += " Apache-2.0";
    expectExpression(nonAscii.c_str(), false);
    std::string error;
    expect(!CharacterSpdxExpression::validate(
               std::string(CharacterSpdxExpression::kMaximumBytes + 1u, 'M'),
               error) && error.find("128 bytes") != std::string::npos,
           "bounded expressions fail with an actionable error");
    return failures == 0 ? 0 : 1;
}
