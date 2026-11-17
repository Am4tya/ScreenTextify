#pragma once

#include "Types.h"
#include "ScreenCapture.h"
#include <string>

namespace ScreenTextify {

// Offline PP-OCRv4 text detection (DBNet) + recognition (SVTR-LCNet, English/ASCII alphabet)
// running on ONNX Runtime (CPU). Unlike Windows.Media.Ocr, the recognizer decodes every printable
// ASCII symbol character-by-character, so symbol-heavy code (=>, <div>, "", [0], {}) is not dropped.
//
// Files expected next to the executable:
//   onnxruntime.dll, onnxruntime_providers_shared.dll
//   models\det.onnx, models\rec_en.onnx, models\en_dict.txt
class PaddleOcr {
public:
    // Loads ONNX Runtime and both models (thread-safe, runs once). Returns false if anything is missing.
    static bool EnsureInitialized(std::wstring* outError = nullptr);

    // True once EnsureInitialized() has succeeded.
    static bool IsAvailable();

    // Starts model loading on a background thread so the first capture is fast.
    static void WarmUpAsync();

    // Detects + recognizes all text in the bitmap. Coordinates are mapped to overlay space using
    // bitmap.screenOriginX/Y and bitmap.scaleFactor (same convention as OcrService).
    static bool Recognize(const RawBitmap& bitmap, OcrDocument& outDoc, std::wstring* outError = nullptr);
};

} // namespace ScreenTextify
