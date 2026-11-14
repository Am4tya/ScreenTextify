#pragma once

#include "Types.h"
#include <string>
#include <vector>

namespace ScreenTextify {

class TextFormatter {
public:
    // Formats selected words (or all words if onlySelected == false) into a clipboard-ready wstring.
    // When preserveIndentation is true, reconstructs leading spaces and blank lines from pixel coordinates.
    static std::wstring FormatDocument(
        const OcrDocument& doc,
        bool onlySelected,
        bool preserveIndentation,
        size_t* outWordCount = nullptr
    );
};

} // namespace ScreenTextify
