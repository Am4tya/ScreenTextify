#pragma once

#include "Types.h"
#include "ScreenCapture.h"
#include <string>
#include <vector>

namespace ScreenTextify {

class OcrService {
public:
    // Recognizes all text in the bitmap. Uses the bundled PP-OCRv4 engine (PaddleOcr) when its files
    // are present, otherwise falls back to Windows Built-in OCR (Windows.Media.Ocr).
    // Maps all detected line & word bounding boxes back to the overlay coordinate space
    // using bitmap.screenOriginX/Y and bitmap.scaleFactor.
    static OcrDocument RecognizeBitmap(const RawBitmap& bitmap, std::wstring* outError = nullptr);

    // Windows.Media.Ocr pipeline only (fallback / comparison).
    static OcrDocument RecognizeWithWindowsOcr(const RawBitmap& bitmap, std::wstring* outError = nullptr);

    // Human-readable name of the active engine (e.g. "PP-OCRv4 Code Engine")
    static std::wstring GetActiveLanguageName();

    // Short engine tag for HUD metrics
    static std::wstring GetLastEngineName();
};

} // namespace ScreenTextify
