#include "character_spdx_expression.h"

#include <cctype>
#include <string>
#include <vector>

namespace CharacterSpdxExpression {
namespace {

enum class TokenKind { Word, LeftParen, RightParen, Plus };

struct Token {
    TokenKind kind;
    std::string text;
    size_t offset;
};

bool identifierPart(char byte) {
    const unsigned char value = static_cast<unsigned char>(byte);
    return std::isalnum(value) != 0 || byte == '-' || byte == '.';
}

bool referenceName(const std::string &text) {
    if (text.empty()) return false;
    for (char byte : text) {
        if (!identifierPart(byte)) return false;
    }
    return true;
}

bool ordinaryIdentifier(const std::string &text) {
    return referenceName(text) && text != "AND" && text != "OR" &&
        text != "WITH" && text != "and" && text != "or" &&
        text != "with" && text != "NONE" && text != "NOASSERTION" &&
        std::isalnum(static_cast<unsigned char>(text.front())) != 0;
}

bool licenseIdentifier(const std::string &text) {
    static const std::string licenseRef = "LicenseRef-";
    static const std::string additionRef = "AdditionRef-";
    static const std::string documentRef = "DocumentRef-";
    if (text.rfind(licenseRef, 0u) == 0u) {
        return referenceName(text.substr(licenseRef.size()));
    }
    if (text.rfind(documentRef, 0u) == 0u) {
        const size_t separator = text.find(':', documentRef.size());
        return separator != std::string::npos &&
            text.find(':', separator + 1u) == std::string::npos &&
            referenceName(text.substr(
                documentRef.size(), separator - documentRef.size())) &&
            text.compare(separator + 1u, licenseRef.size(), licenseRef) == 0 &&
            referenceName(text.substr(separator + 1u + licenseRef.size()));
    }
    if (text.rfind(additionRef, 0u) == 0u) return false;
    return ordinaryIdentifier(text);
}

bool additionIdentifier(const std::string &text) {
    static const std::string additionRef = "AdditionRef-";
    static const std::string documentRef = "DocumentRef-";
    if (text.rfind(additionRef, 0u) == 0u) {
        return referenceName(text.substr(additionRef.size()));
    }
    if (text.rfind(documentRef, 0u) == 0u) {
        const size_t separator = text.find(':', documentRef.size());
        return separator != std::string::npos &&
            text.find(':', separator + 1u) == std::string::npos &&
            referenceName(text.substr(
                documentRef.size(), separator - documentRef.size())) &&
            text.compare(separator + 1u, additionRef.size(), additionRef) == 0 &&
            referenceName(text.substr(separator + 1u + additionRef.size()));
    }
    return ordinaryIdentifier(text) &&
        text.rfind("LicenseRef-", 0u) != 0u;
}

std::string quoted(const Token &token) {
    return token.kind == TokenKind::LeftParen ? "'('" :
        token.kind == TokenKind::RightParen ? "')'" :
        token.kind == TokenKind::Plus ? "'+'" : "'" + token.text + "'";
}

class Parser {
public:
    explicit Parser(const std::vector<Token> &tokens) : m_tokens(tokens) {}

    bool parse(std::string &error) {
        if (!parseOr(error)) return false;
        if (m_index != m_tokens.size()) {
            error = "Unexpected SPDX token " + quoted(m_tokens[m_index]) +
                " at byte " + std::to_string(m_tokens[m_index].offset) + ".";
            return false;
        }
        return true;
    }

private:
    bool operatorWord(const char *upper, const char *lower) const {
        return m_index < m_tokens.size() &&
            m_tokens[m_index].kind == TokenKind::Word &&
            (m_tokens[m_index].text == upper ||
             m_tokens[m_index].text == lower);
    }

    bool parseOr(std::string &error) {
        if (!parseAnd(error)) return false;
        while (operatorWord("OR", "or")) {
            ++m_index;
            if (!parseAnd(error)) return false;
        }
        return true;
    }

    bool parseAnd(std::string &error) {
        if (!parseWith(error)) return false;
        while (operatorWord("AND", "and")) {
            ++m_index;
            if (!parseWith(error)) return false;
        }
        return true;
    }

    bool parseWith(std::string &error) {
        bool simple = false;
        if (!parsePrimary(simple, error)) return false;
        if (!operatorWord("WITH", "with")) return true;
        const Token &with = m_tokens[m_index++];
        if (!simple) {
            error = "SPDX WITH at byte " + std::to_string(with.offset) +
                " must follow one license identifier, not a parenthesized "
                "expression.";
            return false;
        }
        if (m_index >= m_tokens.size() ||
            m_tokens[m_index].kind != TokenKind::Word ||
            !additionIdentifier(m_tokens[m_index].text)) {
            error = "SPDX WITH must be followed by a license exception or "
                "AdditionRef identifier.";
            return false;
        }
        ++m_index;
        return true;
    }

    bool parsePrimary(bool &simple, std::string &error) {
        simple = false;
        if (m_index >= m_tokens.size()) {
            error = "Expected an SPDX license identifier or '('.";
            return false;
        }
        const Token &token = m_tokens[m_index];
        if (token.kind == TokenKind::LeftParen) {
            ++m_index;
            if (!parseOr(error)) return false;
            if (m_index >= m_tokens.size() ||
                m_tokens[m_index].kind != TokenKind::RightParen) {
                error = "SPDX parenthesized expression is missing ')'.";
                return false;
            }
            ++m_index;
            return true;
        }
        if (token.kind != TokenKind::Word ||
            !licenseIdentifier(token.text)) {
            error = "Invalid SPDX license identifier " + quoted(token) +
                " at byte " + std::to_string(token.offset) + ".";
            return false;
        }
        ++m_index;
        if (m_index < m_tokens.size() &&
            m_tokens[m_index].kind == TokenKind::Plus) {
            if (token.text.rfind("LicenseRef-", 0u) == 0u ||
                token.text.rfind("DocumentRef-", 0u) == 0u) {
                error = "SPDX '+' cannot qualify a custom LicenseRef.";
                return false;
            }
            if (m_tokens[m_index].offset != token.offset + token.text.size()) {
                error = "SPDX '+' must immediately follow its license "
                    "identifier.";
                return false;
            }
            ++m_index;
        }
        simple = true;
        return true;
    }

    const std::vector<Token> &m_tokens;
    size_t m_index = 0u;
};

} // namespace

bool validate(const std::string &expression, std::string &error) {
    error.clear();
    if (expression.empty()) {
        error = "Enter an SPDX license expression.";
        return false;
    }
    if (expression.size() > kMaximumBytes) {
        error = "SPDX expression exceeds 128 bytes.";
        return false;
    }
    std::vector<Token> tokens;
    for (size_t offset = 0u; offset < expression.size();) {
        const unsigned char byte =
            static_cast<unsigned char>(expression[offset]);
        if (byte > 0x7Fu) {
            error = "SPDX expressions use ASCII identifiers and operators.";
            return false;
        }
        if (std::isspace(byte) != 0) {
            ++offset;
            continue;
        }
        const size_t tokenOffset = offset;
        if (expression[offset] == '(' || expression[offset] == ')' ||
            expression[offset] == '+') {
            const char value = expression[offset++];
            tokens.push_back({
                value == '(' ? TokenKind::LeftParen :
                    value == ')' ? TokenKind::RightParen : TokenKind::Plus,
                std::string(1u, value), tokenOffset,
            });
            continue;
        }
        while (offset < expression.size()) {
            const char value = expression[offset];
            if (std::isspace(static_cast<unsigned char>(value)) != 0 ||
                value == '(' || value == ')' || value == '+') break;
            ++offset;
        }
        tokens.push_back({TokenKind::Word,
                          expression.substr(tokenOffset, offset - tokenOffset),
                          tokenOffset});
    }
    if (tokens.empty()) {
        error = "Enter an SPDX license expression.";
        return false;
    }
    return Parser(tokens).parse(error);
}

} // namespace CharacterSpdxExpression
