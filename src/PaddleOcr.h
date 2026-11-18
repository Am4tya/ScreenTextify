#pragma once

#include "Types.h"
#include "ScreenCapture.h"
#include <string>

namespace ScreenTextify {

// Offline PP-OCRv5 + PP-OCRv4 Dual-Model / Dual-View Code Recognition Engine on ONNX Runtime (CPU).
//
// Combines:
//   * 2D 8-Connected Component Line & Clause Detection on a contrast-equalized + unsharp-masked bitmap
//   * ASCII-Constrained CTC Decoding (masks out non-ASCII symbols in PP-OCRv5's 436-class head)
//   * Adaptive Confidence Ensemble (verifies low-confidence lines across v5/v4 and clean/raw views)
//   * Topological 2-Bar '=' Verifier & Pushdown Automaton (PDA) Delimiter Stack
class PaddleOcr {
public:
    struct Options {
        std::wstring model = L"ensemble";  // "ensemble" (default), "v5", or "v4"
        bool constrainToCodeAscii = true;  // restrict argmax to printable ASCII classes
        bool dualViewVerify = true;        // verify lines with meanProb < 0.92 against raw-inverted view
    };

    static void SetOptions(const Options& opts);
    static Options GetOptions();

    // Loads ONNX Runtime and available recognition models (thread-safe, runs once).
    static bool EnsureInitialized(std::wstring* outError = nullptr);

    // True once EnsureInitialized() has succeeded.
    static bool IsAvailable();

    // Returns a description of the active model configuration (e.g. "PP-OCRv5 + v4 Ensemble").
    static std::wstring GetActiveEngineDescription();

    // Starts model loading on a background thread so the first capture is fast.
    static void WarmUpAsync();

    // Detects + recognizes all text in the bitmap. Coordinates are mapped to overlay space using
    // bitmap.screenOriginX/Y and bitmap.scaleFactor.
    static bool Recognize(const RawBitmap& bitmap, OcrDocument& outDoc, std::wstring* outError = nullptr);
};

} // namespace ScreenTextify
