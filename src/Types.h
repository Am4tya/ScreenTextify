#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <chrono>

namespace ScreenTextify {

struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    float right() const { return x + width; }
    float bottom() const { return y + height; }
    float centerX() const { return x + width * 0.5f; }
    float centerY() const { return y + height * 0.5f; }

    bool contains(float px, float py) const {
        return px >= x && px <= right() && py >= y && py <= bottom();
    }

    bool intersects(const RectF& other) const {
        return !(x > other.right() || right() < other.x ||
                 y > other.bottom() || bottom() < other.y);
    }

    RectF normalized() const {
        RectF r = *this;
        if (r.width < 0.0f) {
            r.x += r.width;
            r.width = -r.width;
        }
        if (r.height < 0.0f) {
            r.y += r.height;
            r.height = -r.height;
        }
        return r;
    }
};

struct OcrWordBox {
    std::wstring text;
    RectF bounds;       // Coordinates relative to the virtual screen overlay
    int lineIndex = 0;
    int wordIndex = 0;
    float confidence = 1.0f;    // Mean character CTC probability [0.0..1.0]
    float minCharConf = 1.0f;   // Lowest single-character probability in this word [0.0..1.0]
    bool isLineNumber = false;  // Auto-detected editor line-number gutter token
    bool selected = false;
    bool hovered = false;

    bool isUncertain() const {
        return !isLineNumber && (confidence < 0.86f || minCharConf < 0.72f);
    }

    bool isVeryLowConfidence() const {
        return !isLineNumber && (confidence < 0.75f || minCharConf < 0.55f);
    }
};

struct OcrLineBox {
    std::wstring text;
    RectF bounds;
    std::vector<OcrWordBox> words;
};

struct OcrDocument {
    std::vector<OcrLineBox> lines;
    RectF scanRegion;
    double recognitionTimeMs = 0.0;

    bool empty() const {
        return lines.empty();
    }

    size_t totalWords() const {
        size_t count = 0;
        for (const auto& line : lines) {
            count += line.words.size();
        }
        return count;
    }

    size_t uncertainWordCount() const {
        size_t count = 0;
        for (const auto& line : lines) {
            for (const auto& w : line.words) {
                if (w.isUncertain()) ++count;
            }
        }
        return count;
    }

    size_t gutterWordCount() const {
        size_t count = 0;
        for (const auto& line : lines) {
            for (const auto& w : line.words) {
                if (w.isLineNumber) ++count;
            }
        }
        return count;
    }
};

struct CaptureHistoryItem {
    std::wstring text;
    std::wstring timestamp;
    bool wasCodeMode = true;
    size_t wordCount = 0;
    double ocrTimeMs = 0.0;
    std::vector<std::wstring> uncertainWords;
};

enum class CaptureLaunchMode {
    RegionInteractive,  // Alt+Shift+S: Crop region -> interactive word/line selection
    FullScreenSelect,   // Alt+Shift+D: Instant full-screen interactive word/line selection
    QuickSnipCopy,      // Alt+Shift+C: Crop region -> immediately copy all text & close
    StitchAppend        // Alt+Shift+A: Crop scrolled code -> stitch & deduplicate onto clipboard
};

struct AppSettings {
    bool codeIndentationMode = true;    // Smart code block indentation preservation
    bool lineSelectionMode = false;     // Select whole lines on hover/drag vs individual words
    bool autoCopyOnRelease = true;      // Copy immediately when mouse drag finishes (Selectext style)
    bool autoCloseAfterCopy = false;    // Close overlay immediately after copying in interactive mode
    bool upscaleForOcr = true;          // 2x super-resolution upscale + dark-mode boost for video text
    bool stripLineNumbers = true;       // Auto-detect & strip editor line-number gutters (1, 2, 3...)
    bool showConfidenceHeatmap = true;  // Amber warning highlights on uncertain OCR words
    bool showLivePreviewCard = true;    // Show live formatted code/text card in overlay
    bool startWithWindows = false;      // Launch silently in System Tray on Windows login

    UINT hotkeyRegionModifiers = MOD_ALT | MOD_SHIFT;
    UINT hotkeyRegionVk = 'S';          // Alt+Shift+S: Region -> Selectext Overlay

    UINT hotkeyFullScreenModifiers = MOD_ALT | MOD_SHIFT;
    UINT hotkeyFullScreenVk = 'D';      // Alt+Shift+D: Instant Full-Screen Selectext

    UINT hotkeyQuickSnipModifiers = MOD_ALT | MOD_SHIFT;
    UINT hotkeyQuickSnipVk = 'C';       // Alt+Shift+C: Quick Snip & Instant Copy

    UINT hotkeyStitchModifiers = MOD_ALT | MOD_SHIFT;
    UINT hotkeyStitchVk = 'A';          // Alt+Shift+A: Stitch & Append scrolled code

    UINT hotkeyHistoryModifiers = MOD_ALT | MOD_SHIFT;
    UINT hotkeyHistoryVk = 'H';         // Alt+Shift+H: Open Studio Dashboard
};

} // namespace ScreenTextify
