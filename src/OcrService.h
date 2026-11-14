#pragma once

#include "Types.h"
#include "ScreenCapture.h"
#include <string>
#include <vector>

namespace ScreenTextify {

class OcrService {
public:
    // Runs Windows Built-in OCR (Windows.Media.Ocr) on the given RawBitmap.
    // Maps all detected line & word bounding boxes back to the overlay coordinate space
    // using bitmap.screenOriginX/Y and bitmap.scaleFactor.
    static OcrDocument RecognizeBitmap(const RawBitmap& bitmap, std::wstring* outError = nullptr);

    // Returns the name of the active Windows OCR language profile (e.g. "English (United States)")
    static std::wstring GetActiveLanguageName();
};

} // namespace ScreenTextify
