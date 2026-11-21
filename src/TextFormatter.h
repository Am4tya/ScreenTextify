#pragma once

#include "Types.h"
#include <string>
#include <vector>

namespace ScreenTextify {

class TextFormatter {
public:
    // Detects a sequential code-editor line-number gutter on the left edge (e.g. VS Code / JetBrains / GitHub)
    // and marks those tokens with OcrWordBox::isLineNumber = true. Returns the number of gutter tokens marked.
    static size_t MarkLineNumberGutter(OcrDocument& doc);

    // Formats selected words (or all words if onlySelected == false) into a clipboard-ready wstring.
    // When preserveIndentation is true, reconstructs leading spaces and blank lines from pixel coordinates.
    // When stripLineNumbers is true, excludes auto-detected editor line-number gutter tokens.
    static std::wstring FormatDocument(
        const OcrDocument& doc,
        bool onlySelected,
        bool preserveIndentation,
        size_t* outWordCount = nullptr,
        bool stripLineNumbers = true
    );

    // Stitches a newly captured scrolled code block onto an existing clipboard code block,
    // automatically detecting and deduplicating overlapping suffix/prefix lines (fuzzy match).
    static std::wstring StitchCodeBlocks(
        const std::wstring& existingText,
        const std::wstring& newText,
        int* outOverlapLines = nullptr,
        int* outAppendedLines = nullptr
    );
};

} // namespace ScreenTextify
