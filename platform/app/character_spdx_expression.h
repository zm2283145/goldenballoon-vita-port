#ifndef MDKR_APP_CHARACTER_SPDX_EXPRESSION_H
#define MDKR_APP_CHARACTER_SPDX_EXPRESSION_H

#include <cstddef>
#include <string>

namespace CharacterSpdxExpression {

constexpr size_t kMaximumBytes = 128u;

// Parses the SPDX expression grammar used by character-source manifests.
// This validates structure and identifier spelling; it intentionally does not
// claim that a syntactically valid identifier exists in a particular revision
// of the evolving SPDX License List or that the declared license grants rights.
bool validate(const std::string &expression, std::string &error);

} // namespace CharacterSpdxExpression

#endif // MDKR_APP_CHARACTER_SPDX_EXPRESSION_H
