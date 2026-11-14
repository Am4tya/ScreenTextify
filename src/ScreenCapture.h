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
    // If upscaleFactor > 1, performs smooth bilinear upscaling for higher OCR accuracy on small video text
    static RawBitmap CropAndScale(const RawBitmap& source, const RectF& region, int upscaleFactor = 1);

    // Creates a synthetic dark-mode video code frame for self-testing OCR and indentation
    static RawBitmap CreateSyntheticCodeFrame(int width = 800, int height = 420);

    // Loads an image file (PNG/JPG/BMP) into a RawBitmap using GDI+
    static RawBitmap LoadBitmapFromFile(const std::wstring& filePath);
};

} // namespace ScreenTextify