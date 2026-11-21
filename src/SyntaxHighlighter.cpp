#include "SyntaxHighlighter.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <unordered_set>

namespace ScreenTextify {

namespace {

const std::unordered_set<std::wstring>& GetKeywords() {
    static const std::unordered_set<std::wstring> kWords = {
        // JS / TS / React
        L"const", L"let", L"var", L"function", L"return", L"async", L"await", L"import", L"export",
        L"default", L"from", L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"break",
        L"continue", L"try", L"catch", L"finally", L"throw", L"new", L"typeof", L"instanceof",
        L"class", L"extends", L"implements", L"interface", L"type", L"enum", L"public", L"private",
        L"protected", L"readonly", L"static", L"true", L"false", L"null", L"undefined", L"this", L"super",
        // C / C++
        L"auto", L"bool", L"char", L"wchar_t", L"int", L"long", L"short", L"float", L"double", L"void",
        L"signed", L"unsigned", L"struct", L"namespace", L"using", L"template", L"typename", L"constexpr",
        L"nullptr", L"virtual", L"override", L"final", L"inline", L"explicit", L"noexcept", L"sizeof",
        L"include", L"pragma", L"define", L"requires", L"concept", L"delete",
        // Python
        L"def", L"lambda", L"pass", L"yield", L"with", L"as", L"in", L"is", L"not", L"and", L"or",
        L"elif", L"except", L"raise", L"True", L"False", L"None", L"self",
        // Rust / Go / Java / SQL
        L"fn", L"pub", L"mut", L"match", L"impl", L"trait", L"mod", L"use", L"crate", L"where",
        L"func", L"package", L"defer", L"go", L"chan", L"select", L"SELECT", L"FROM", L"WHERE",
        L"INSERT", L"UPDATE", L"DELETE", L"JOIN", L"CREATE", L"TABLE", L"ORDER", L"GROUP", L"BY"
    };
    return kWords;
}

const std::unordered_set<std::wstring>& GetBuiltinTypes() {
    static const std::unordered_set<std::wstring> kTypes = {
        L"std", L"string", L"wstring", L"vector", L"map", L"set", L"unique_ptr", L"shared_ptr",
        L"uint8_t", L"uint16_t", L"uint32_t", L"uint64_t", L"int32_t", L"int64_t", L"size_t",
        L"String", L"Number", L"Boolean", L"Promise", L"Array", L"Object", L"Error", L"React",
        L"Result", L"Option", L"Vec", L"Box", L"u8", L"u16", L"u32", L"u64", L"i32", L"i64", L"f32", L"f64", L"str",
        L"dict", L"list", L"tuple", L"unknown", L"any", L"number", L"boolean"
    };
    return kTypes;
}

bool IsIdentStart(wchar_t c) {
    return std::iswalpha(c) || c == L'_' || c == L'$';
}

bool IsIdentPart(wchar_t c) {
    return std::iswalnum(c) || c == L'_' || c == L'$';
}

} // namespace

std::wstring SyntaxHighlighter::DetectLanguage(const std::wstring& code) {
    if (code.empty()) return L"Plain Text";

    int jsScore = 0, cppScore = 0, pyScore = 0, rsScore = 0;

    if (code.find(L"=>") != std::wstring::npos) jsScore += 3;
    if (code.find(L"const ") != std::wstring::npos || code.find(L"let ") != std::wstring::npos) jsScore += 2;
    if (code.find(L"useState") != std::wstring::npos || code.find(L"</div>") != std::wstring::npos || code.find(L"export default") != std::wstring::npos) jsScore += 4;
    if (code.find(L"await ") != std::wstring::npos || code.find(L"console.") != std::wstring::npos) jsScore += 2;
    if (code.find(L"?.") != std::wstring::npos) jsScore += 2;

    if (code.find(L"std::") != std::wstring::npos || code.find(L"#include") != std::wstring::npos) cppScore += 5;
    if (code.find(L"template <") != std::wstring::npos || code.find(L"nullptr") != std::wstring::npos) cppScore += 3;
    if (code.find(L"int ") != std::wstring::npos && code.find(L";") != std::wstring::npos) cppScore += 2;

    if (code.find(L"def ") != std::wstring::npos && code.find(L":") != std::wstring::npos) pyScore += 4;
    if (code.find(L"elif ") != std::wstring::npos || code.find(L"None") != std::wstring::npos) pyScore += 3;
    if (code.find(L"import ") != std::wstring::npos && code.find(L";") == std::wstring::npos && code.find(L"{") == std::wstring::npos) pyScore += 2;

    if (code.find(L"pub fn ") != std::wstring::npos || code.find(L"fn ") != std::wstring::npos) rsScore += 4;
    if (code.find(L"Result<") != std::wstring::npos || code.find(L"::<") != std::wstring::npos || code.find(L"match ") != std::wstring::npos) rsScore += 3;

    int best = std::max({ jsScore, cppScore, pyScore, rsScore });
    if (best < 2) return L"Plain Text";
    if (rsScore == best) return L"Rust";
    if (cppScore == best) return L"C++";
    if (pyScore == best) return L"Python";
    if (code.find(L"</") != std::wstring::npos || code.find(L"useState") != std::wstring::npos) return L"React JSX";
    if (code.find(L": string") != std::wstring::npos || code.find(L": unknown") != std::wstring::npos || code.find(L"interface ") != std::wstring::npos) return L"TypeScript";
    return L"JavaScript";
}

std::vector<SyntaxSpan> SyntaxHighlighter::TokenizeLine(const std::wstring& line) {
    std::vector<SyntaxSpan> spans;
    const size_t n = line.size();
    size_t i = 0;

    const auto& keywords = GetKeywords();
    const auto& types = GetBuiltinTypes();

    while (i < n) {
        wchar_t c = line[i];

        if (std::iswspace(c)) {
            ++i;
            continue;
        }

        // Line comments: // ... or # ...
        if ((c == L'/' && i + 1 < n && line[i + 1] == L'/') ||
            (c == L'#' && (i == 0 || std::iswspace(line[i - 1])))) {
            spans.push_back({ static_cast<uint32_t>(i), static_cast<uint32_t>(n - i), SyntaxKind::Comment });
            break;
        }

        // String literals: "...", '...', `...`
        if (c == L'"' || c == L'\'' || c == L'`') {
            wchar_t quote = c;
            size_t start = i++;
            while (i < n) {
                if (line[i] == L'\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (line[i] == quote) {
                    ++i;
                    break;
                }
                ++i;
            }
            spans.push_back({ static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxKind::String });
            continue;
        }

        // JSX / HTML tag names like <div> or </div>
        if (c == L'<' && i + 1 < n && (std::iswalpha(line[i + 1]) || line[i + 1] == L'/')) {
            size_t start = i;
            size_t j = i + 1;
            if (j < n && line[j] == L'/') ++j;
            size_t tagStart = j;
            while (j < n && (std::iswalnum(line[j]) || line[j] == L'-' || line[j] == L'_')) ++j;
            if (j > tagStart) {
                spans.push_back({ static_cast<uint32_t>(start), static_cast<uint32_t>(tagStart - start), SyntaxKind::Operator });
                spans.push_back({ static_cast<uint32_t>(tagStart), static_cast<uint32_t>(j - tagStart), SyntaxKind::Tag });
                i = j;
                continue;
            }
        }

        // Numeric literals
        if (std::iswdigit(c)) {
            size_t start = i++;
            while (i < n && (std::iswalnum(line[i]) || line[i] == L'.' || line[i] == L'_')) ++i;
            spans.push_back({ static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxKind::Number });
            continue;
        }

        // Identifiers, Keywords, Builtin Types, PascalCase Types, Function Calls
        if (IsIdentStart(c)) {
            size_t start = i++;
            while (i < n && IsIdentPart(line[i])) ++i;
            std::wstring word = line.substr(start, i - start);

            SyntaxKind kind = SyntaxKind::Plain;
            if (keywords.count(word)) {
                kind = SyntaxKind::Keyword;
            } else if (types.count(word)) {
                kind = SyntaxKind::Type;
            } else {
                // Look ahead for '(' to colorize function/method calls
                size_t look = i;
                while (look < n && line[look] == L' ') ++look;
                if (look < n && (line[look] == L'(' || line[look] == L'!')) {
                    kind = SyntaxKind::Function;
                } else if (word.size() >= 2 && std::iswupper(word[0]) && std::iswlower(word[1])) {
                    kind = SyntaxKind::Type;
                }
            }
            spans.push_back({ static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), kind });
            continue;
        }

        // Operators & brackets
        if (wcschr(L"=+-*/%&|!<>?:;,.()[]{}", c) != nullptr) {
            size_t start = i++;
            while (i < n && wcschr(L"=+-*/%&|!<>?:", line[i]) != nullptr) ++i;
            spans.push_back({ static_cast<uint32_t>(start), static_cast<uint32_t>(i - start), SyntaxKind::Operator });
            continue;
        }

        ++i;
    }

    return spans;
}

} // namespace ScreenTextify
