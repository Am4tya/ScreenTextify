#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace ScreenTextify {

enum class SyntaxKind : uint8_t {
    Plain,
    Keyword,
    Type,
    Function,
    String,
    Number,
    Comment,
    Operator,
    Tag
};

struct SyntaxSpan {
    uint32_t start = 0;
    uint32_t length = 0;
    SyntaxKind kind = SyntaxKind::Plain;
};

class SyntaxHighlighter {
public:
    // Auto-detects the programming language from captured code text for display in the Studio badge.
    static std::wstring DetectLanguage(const std::wstring& code);

    // Tokenizes a single line of code into colored spans for DirectWrite rendering.
    static std::vector<SyntaxSpan> TokenizeLine(const std::wstring& line);
};

} // namespace ScreenTextify
