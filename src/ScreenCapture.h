#pragma once

#include "Types.h"
#include <windows.h>
#include <vector>
#include <cstdint>

namespace ScreenTextify {

struct RawBitmap {
    int width = 0;
    int height = 0;
    int stride = 0;
    int screenOriginX = 0;
    int screenOriginY = 0;
    float scaleFactor = 1.0f; // >1.0 if upscaled for OCR
    std::vector<uint8_t> pixels; // BGRA8 format (4 bytes per pixel)

    bool valid() const {
        return width > 0 && height > 0 && !pixels.empty();
    }
};

class ScreenCapture {
public:
    // Captures the entire virtual desktop (all monitors)
    static RawBitmap CaptureVirtualScreen();

    // Crops a sub-region from an existing RawBitmap (coordinates relative to bitmap 0,0)
    // If upscaleFactor > 1, performs Catmull-Rom bicubic upscaling + edge-preserving sharpening
    static RawBitmap CropAndScale(const RawBitmap& source, const RectF& region, int upscaleFactor = 1);

    // Inspects the 4 border edges of `region` inside `source`; if foreground glyph ink touches
    // a crop boundary, expands that edge outward (up to maxPadPx) until a clean margin is reached.
    static RectF SmartExpandCropRegion(const RawBitmap& source, const RectF& region, int maxPadPx = 10);

    // Creates a synthetic dark-mode video code frame for self-testing OCR and indentation
    static RawBitmap CreateSyntheticCodeFrame(int width = 800, int height = 420);

    // Creates a synthetic React JSX component frame matching the user's Login.jsx video
    static RawBitmap CreateSyntheticReactFrame(int width = 820, int height = 340);

    // Loads an image file (PNG/JPG/BMP) into a RawBitmap using GDI+
    static RawBitmap LoadBitmapFromFile(const std::wstring& filePath);

    // Loads an image from the Windows Clipboard (CF_DIBV5/CF_DIB/CF_BITMAP or copied image file via CF_HDROP)
    static RawBitmap LoadBitmapFromClipboard(HWND hwndOwner);
};

} // namespace ScreenTextify