#include "ScreenCapture.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>

namespace ScreenTextify {

RawBitmap ScreenCapture::CaptureVirtualScreen() {
    RawBitmap result;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vw <= 0 || vh <= 0) {
        vw = GetSystemMetrics(SM_CXSCREEN);
        vh = GetSystemMetrics(SM_CYSCREEN);
        vx = 0;
        vy = 0;
    }

    HDC hScreenDC = GetDC(nullptr);
    if (!hScreenDC) return result;

    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    if (!hMemDC) {
        ReleaseDC(nullptr, hScreenDC);
        return result;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDib = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    if (!hDib || !pBits) {
        if (hDib) DeleteObject(hDib);
        DeleteDC(hMemDC);
        ReleaseDC(nullptr, hScreenDC);
        return result;
    }

    HGDIOBJ hOld = SelectObject(hMemDC, hDib);
    BitBlt(hMemDC, 0, 0, vw, vh, hScreenDC, vx, vy, SRCCOPY | CAPTUREBLT);
    GdiFlush();

    result.width = vw;
    result.height = vh;
    result.stride = vw * 4;
    result.screenOriginX = vx;
    result.screenOriginY = vy;
    result.scaleFactor = 1.0f;
    result.pixels.resize(static_cast<size_t>(vw) * vh * 4);
    memcpy(result.pixels.data(), pBits, result.pixels.size());

    // Ensure alpha byte is 255 for all captured pixels
    uint8_t* ptr = result.pixels.data();
    size_t totalPixels = static_cast<size_t>(vw) * vh;
    for (size_t i = 0; i < totalPixels; ++i) {
        ptr[i * 4 + 3] = 255;
    }

    SelectObject(hMemDC, hOld);
    DeleteObject(hDib);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);

    return result;
}

namespace {

// Keys' cubic kernel (B = 0, C = 0.30): interpolatory (exact at sample centers, zero blur)
// with gentle negative lobes so 1px punctuation ('.', ';', '//') stays razor-sharp without ringing.
inline float BicubicWeight(float x) {
    x = std::fabs(x);
    constexpr float B = 0.0f;
    constexpr float C = 0.30f;
    if (x < 1.0f) {
        return ((12.0f - 9.0f * B - 6.0f * C) * x * x * x +
                (-18.0f + 12.0f * B + 6.0f * C) * x * x +
                (6.0f - 2.0f * B)) / 6.0f;
    }
    if (x < 2.0f) {
        return ((-B - 6.0f * C) * x * x * x +
                (6.0f * B + 30.0f * C) * x * x +
                (-12.0f * B - 48.0f * C) * x +
                (8.0f * B + 24.0f * C)) / 6.0f;
    }
    return 0.0f;
}

} // namespace

RectF ScreenCapture::SmartExpandCropRegion(const RawBitmap& source, const RectF& region, int maxPadPx) {
    RectF norm = region.normalized();
    if (!source.valid() || maxPadPx <= 0) return norm;

    int x0 = std::clamp(static_cast<int>(std::floor(norm.x)), 0, source.width - 1);
    int y0 = std::clamp(static_cast<int>(std::floor(norm.y)), 0, source.height - 1);
    int x1 = std::clamp(static_cast<int>(std::ceil(norm.right())), x0 + 1, source.width);
    int y1 = std::clamp(static_cast<int>(std::ceil(norm.bottom())), y0 + 1, source.height);

    // Estimate background color inside the crop using luminance quantiles
    struct Px { int lum, r, g, b; };
    std::vector<Px> samples;
    int stepX = std::max(1, (x1 - x0) / 32);
    int stepY = std::max(1, (y1 - y0) / 24);
    samples.reserve(800);
    for (int y = y0; y < y1; y += stepY) {
        const uint8_t* row = source.pixels.data() + static_cast<size_t>(y) * source.stride;
        for (int x = x0; x < x1; x += stepX) {
            int b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2];
            int lum = (r * 77 + g * 150 + b * 29) >> 8;
            samples.push_back({ lum, r, g, b });
        }
    }
    if (samples.empty()) return norm;

    int64_t lumSum = 0;
    for (const auto& s : samples) lumSum += s.lum;
    bool isDark = (lumSum / static_cast<int64_t>(samples.size())) < 140;
    size_t bgIdx = isDark ? (samples.size() / 4) : ((samples.size() * 3) / 4);
    std::nth_element(samples.begin(), samples.begin() + bgIdx, samples.end(),
                     [](const Px& a, const Px& b) { return a.lum < b.lum; });
    const Px bg = samples[bgIdx];

    auto isInkPixel = [&](int x, int y) -> bool {
        if (x < 0 || x >= source.width || y < 0 || y >= source.height) return false;
        const uint8_t* p = source.pixels.data() + static_cast<size_t>(y) * source.stride + static_cast<size_t>(x) * 4;
        int dR = isDark ? std::max(0, static_cast<int>(p[2]) - bg.r) : std::max(0, bg.r - static_cast<int>(p[2]));
        int dG = isDark ? std::max(0, static_cast<int>(p[1]) - bg.g) : std::max(0, bg.g - static_cast<int>(p[1]));
        int dB = isDark ? std::max(0, static_cast<int>(p[0]) - bg.b) : std::max(0, bg.b - static_cast<int>(p[0]));
        return std::max({ dR, dG, dB }) >= 38;
    };

    auto colHasInk = [&](int x, int ry0, int ry1) -> bool {
        int cnt = 0;
        for (int y = ry0; y < ry1; ++y) {
            if (isInkPixel(x, y) && ++cnt >= 2) return true;
        }
        return false;
    };
    auto rowHasInk = [&](int y, int rx0, int rx1) -> bool {
        int cnt = 0;
        for (int x = rx0; x < rx1; ++x) {
            if (isInkPixel(x, y) && ++cnt >= 2) return true;
        }
        return false;
    };

    // Expand each edge outward while foreground ink touches its border (stops once a 2px clean margin is reached)
    int padL = 0;
    while (padL < maxPadPx && x0 > 0 && (colHasInk(x0, y0, y1) || colHasInk(std::min(x1 - 1, x0 + 1), y0, y1))) {
        --x0; ++padL;
    }
    int padR = 0;
    while (padR < maxPadPx && x1 < source.width && (colHasInk(x1 - 1, y0, y1) || colHasInk(std::max(x0, x1 - 2), y0, y1))) {
        ++x1; ++padR;
    }
    int maxPadV = std::max(4, maxPadPx - 2);
    int padT = 0;
    while (padT < maxPadV && y0 > 0 && (rowHasInk(y0, x0, x1) || rowHasInk(std::min(y1 - 1, y0 + 1), x0, x1))) {
        --y0; ++padT;
    }
    int padB = 0;
    while (padB < maxPadV && y1 < source.height && (rowHasInk(y1 - 1, x0, x1) || rowHasInk(std::max(y0, y1 - 2), x0, x1))) {
        ++y1; ++padB;
    }

    return RectF{
        static_cast<float>(x0),
        static_cast<float>(y0),
        static_cast<float>(x1 - x0),
        static_cast<float>(y1 - y0)
    };
}

RawBitmap ScreenCapture::CropAndScale(const RawBitmap& source, const RectF& region, int upscaleFactor) {
    RawBitmap out;
    if (!source.valid()) return out;

    RectF norm = region.normalized();
    int x0 = std::clamp(static_cast<int>(std::floor(norm.x)), 0, source.width - 1);
    int y0 = std::clamp(static_cast<int>(std::floor(norm.y)), 0, source.height - 1);
    int x1 = std::clamp(static_cast<int>(std::ceil(norm.right())), x0 + 1, source.width);
    int y1 = std::clamp(static_cast<int>(std::ceil(norm.bottom())), y0 + 1, source.height);

    int cropW = x1 - x0;
    int cropH = y1 - y0;
    if (cropW <= 2 || cropH <= 2) return out;

    upscaleFactor = std::clamp(upscaleFactor, 1, 3);
    if (cropW * upscaleFactor > 3840 || cropH * upscaleFactor > 2400) {
        upscaleFactor = 1;
    }

    out.width = cropW * upscaleFactor;
    out.height = cropH * upscaleFactor;
    out.stride = out.width * 4;
    out.screenOriginX = x0;
    out.screenOriginY = y0;
    out.scaleFactor = static_cast<float>(upscaleFactor);
    out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);

    if (upscaleFactor == 1) {
        for (int y = 0; y < cropH; ++y) {
            const uint8_t* srcRow = source.pixels.data() + static_cast<size_t>(y0 + y) * source.stride + static_cast<size_t>(x0) * 4;
            uint8_t* dstRow = out.pixels.data() + static_cast<size_t>(y) * out.stride;
            memcpy(dstRow, srcRow, static_cast<size_t>(cropW) * 4);
        }
    } else {
        // Separable 2-pass Catmull-Rom Bicubic upscaling (preserves crisp 1px punctuation & thin strokes)
        std::vector<float> tmpH(static_cast<size_t>(cropH) * out.width * 3);

        // Precompute horizontal 4-tap Catmull-Rom weights
        struct Tap4 { int idx[4]; float w[4]; };
        std::vector<Tap4> xTaps(static_cast<size_t>(out.width));
        for (int dx = 0; dx < out.width; ++dx) {
            float sx = (static_cast<float>(dx) + 0.5f) / out.scaleFactor - 0.5f;
            int ix = static_cast<int>(std::floor(sx));
            float fx = sx - static_cast<float>(ix);
            float sumW = 0.0f;
            for (int k = 0; k < 4; ++k) {
                xTaps[dx].idx[k] = std::clamp(ix + k - 1, 0, cropW - 1);
                float wk = BicubicWeight(fx - static_cast<float>(k - 1));
                xTaps[dx].w[k] = wk;
                sumW += wk;
            }
            if (std::fabs(sumW) > 1e-5f) {
                for (int k = 0; k < 4; ++k) xTaps[dx].w[k] /= sumW;
            }
        }

        // Pass 1: Horizontal bicubic
        for (int sy = 0; sy < cropH; ++sy) {
            const uint8_t* srcRow = source.pixels.data() + static_cast<size_t>(y0 + sy) * source.stride + static_cast<size_t>(x0) * 4;
            float* dstRow = tmpH.data() + static_cast<size_t>(sy) * out.width * 3;
            for (int dx = 0; dx < out.width; ++dx) {
                const auto& t = xTaps[dx];
                float c0 = 0.0f, c1 = 0.0f, c2 = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const uint8_t* p = srcRow + static_cast<size_t>(t.idx[k]) * 4;
                    float w = t.w[k];
                    c0 += p[0] * w;
                    c1 += p[1] * w;
                    c2 += p[2] * w;
                }
                dstRow[dx * 3 + 0] = std::clamp(c0, 0.0f, 255.0f);
                dstRow[dx * 3 + 1] = std::clamp(c1, 0.0f, 255.0f);
                dstRow[dx * 3 + 2] = std::clamp(c2, 0.0f, 255.0f);
            }
        }

        // Pass 2: Vertical bicubic into out.pixels
        for (int dy = 0; dy < out.height; ++dy) {
            float sy = (static_cast<float>(dy) + 0.5f) / out.scaleFactor - 0.5f;
            int iy = static_cast<int>(std::floor(sy));
            float fy = sy - static_cast<float>(iy);
            int yIdx[4];
            float yW[4];
            float sumW = 0.0f;
            for (int k = 0; k < 4; ++k) {
                yIdx[k] = std::clamp(iy + k - 1, 0, cropH - 1);
                yW[k] = BicubicWeight(fy - static_cast<float>(k - 1));
                sumW += yW[k];
            }
            if (std::fabs(sumW) > 1e-5f) {
                for (int k = 0; k < 4; ++k) yW[k] /= sumW;
            }

            const float* r0 = tmpH.data() + static_cast<size_t>(yIdx[0]) * out.width * 3;
            const float* r1 = tmpH.data() + static_cast<size_t>(yIdx[1]) * out.width * 3;
            const float* r2 = tmpH.data() + static_cast<size_t>(yIdx[2]) * out.width * 3;
            const float* r3 = tmpH.data() + static_cast<size_t>(yIdx[3]) * out.width * 3;
            uint8_t* dstRow = out.pixels.data() + static_cast<size_t>(dy) * out.stride;

            for (int dx = 0; dx < out.width; ++dx) {
                size_t o = static_cast<size_t>(dx) * 3;
                for (int c = 0; c < 3; ++c) {
                    float v = r0[o + c] * yW[0] + r1[o + c] * yW[1] + r2[o + c] * yW[2] + r3[o + c] * yW[3];
                    dstRow[dx * 4 + c] = static_cast<uint8_t>(std::clamp(v + 0.5f, 0.0f, 255.0f));
                }
                dstRow[dx * 4 + 3] = 255;
            }
        }
    }

    return out;
}

RawBitmap ScreenCapture::CreateSyntheticCodeFrame(int width, int height) {
    RawBitmap result;
    HDC hScreenDC = GetDC(nullptr);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDib = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HGDIOBJ hOldBmp = SelectObject(hMemDC, hDib);

    // Fill VS Code Dark+ video background (#18181B)
    RECT rc = { 0, 0, width, height };
    HBRUSH hBg = CreateSolidBrush(RGB(24, 24, 28));
    FillRect(hMemDC, &rc, hBg);
    DeleteObject(hBg);

    // Draw VS Code blue selection highlight (#264F78) behind lines 7-9 (just like the user's video!)
    RECT rcSel1 = { 35, 150, 620, 172 };
    RECT rcSel2 = { 35, 172, 480, 194 };
    RECT rcSel3 = { 35, 194, 495, 216 };
    HBRUSH hSelBrush = CreateSolidBrush(RGB(28, 68, 114));
    FillRect(hMemDC, &rcSel1, hSelBrush);
    FillRect(hMemDC, &rcSel2, hSelBrush);
    FillRect(hMemDC, &rcSel3, hSelBrush);
    DeleteObject(hSelBrush);

    HFONT hFont = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas"
    );
    HGDIOBJ hOldFont = SelectObject(hMemDC, hFont);
    SetBkMode(hMemDC, TRANSPARENT);

    struct ColoredToken {
        const wchar_t* text;
        COLORREF color;
    };
    auto drawTokenLine = [&](int x, int y, const std::vector<ColoredToken>& tokens) {
        int curX = x;
        for (const auto& tk : tokens) {
            SetTextColor(hMemDC, tk.color);
            TextOutW(hMemDC, curX, y, tk.text, static_cast<int>(wcslen(tk.text)));
            SIZE sz = {};
            GetTextExtentPoint32W(hMemDC, tk.text, static_cast<int>(wcslen(tk.text)), &sz);
            curX += sz.cx;
        }
    };

    COLORREF cPurple = RGB(197, 134, 192);
    COLORREF cBlue   = RGB(86, 156, 214);
    COLORREF cCyan   = RGB(156, 220, 254);
    COLORREF cTeal   = RGB(78, 201, 176);
    COLORREF cYellow = RGB(220, 220, 170);
    COLORREF cOrange = RGB(206, 145, 120);
    COLORREF cGreen  = RGB(181, 206, 168);
    COLORREF cWhite  = RGB(212, 212, 212);
    COLORREF cGold   = RGB(255, 215, 0);

    // Exact 16-line code block from the user's video screenshot
    drawTokenLine(40, 20,  { { L"  ", cWhite }, { L"if ", cPurple }, { L"(", cPurple }, { L"message", cCyan }, { L"?.", cWhite }, { L"content", cCyan }, { L") {", cPurple } });
    drawTokenLine(40, 42,  { { L"    ", cWhite }, { L"conversation", cCyan }, { L".", cWhite }, { L"lastMessage ", cCyan }, { L"= ", cWhite }, { L"message", cCyan }, { L"?.", cWhite }, { L"id", cCyan }, { L";", cWhite } });
    drawTokenLine(40, 64,  { { L"  }", cPurple } });
    drawTokenLine(40, 86,  { { L"  ", cWhite }, { L"conversation", cCyan }, { L".", cWhite }, { L"unreadCount ", cCyan }, { L"+= ", cWhite }, { L"1", cGreen }, { L";", cWhite } });
    drawTokenLine(40, 108, { { L"  ", cWhite }, { L"await ", cPurple }, { L"conversation", cCyan }, { L".", cWhite }, { L"save", cYellow }, { L"()", cPurple }, { L";", cWhite } });

    drawTokenLine(40, 152, { { L"  ", cWhite }, { L"const ", cBlue }, { L"populatedMessage ", cCyan }, { L"= ", cWhite }, { L"await ", cPurple }, { L"Message", cTeal }, { L".", cWhite }, { L"findOne", cYellow }, { L"(", cGold }, { L"message", cCyan }, { L"?.", cWhite }, { L"_id", cCyan }, { L")", cGold } });
    drawTokenLine(40, 174, { { L"    .", cWhite }, { L"populate", cYellow }, { L"(", cPurple }, { L"\"sender\"", cOrange }, { L", ", cWhite }, { L"\"username profilePicture\"", cOrange }, { L")", cPurple } });
    drawTokenLine(40, 196, { { L"    .", cWhite }, { L"populate", cYellow }, { L"(", cPurple }, { L"\"receiver\"", cOrange }, { L", ", cWhite }, { L"\"username profilePicture\"", cOrange }, { L")", cPurple }, { L";", cWhite } });

    drawTokenLine(40, 240, { { L"  ", cWhite }, { L"return ", cPurple }, { L"response", cYellow }, { L"(", cPurple }, { L"res", cCyan }, { L", ", cWhite }, { L"201", cGreen }, { L", ", cWhite }, { L"\"Message send successfully\"", cOrange }, { L", ", cWhite }, { L"populatedMessage", cCyan }, { L")", cPurple }, { L";", cWhite } });
    drawTokenLine(40, 262, { { L"} ", cGold }, { L"catch ", cPurple }, { L"(", cGold }, { L"error", cCyan }, { L") {", cGold } });
    drawTokenLine(40, 284, { { L"  ", cWhite }, { L"console", cCyan }, { L".", cWhite }, { L"error", cYellow }, { L"(", cPurple }, { L"error", cCyan }, { L")", cPurple }, { L";", cWhite } });
    drawTokenLine(40, 306, { { L"  ", cWhite }, { L"return ", cPurple }, { L"response", cYellow }, { L"(", cPurple }, { L"res", cCyan }, { L", ", cWhite }, { L"500", cGreen }, { L", ", cWhite }, { L"\"Internal server error\"", cOrange }, { L")", cPurple }, { L";", cWhite } });
    drawTokenLine(40, 328, { { L"}", cGold } });
    drawTokenLine(24, 350, { { L"};", cGold } });

    GdiFlush();

    result.width = width;
    result.height = height;
    result.stride = width * 4;
    result.pixels.resize(static_cast<size_t>(width) * height * 4);
    memcpy(result.pixels.data(), pBits, result.pixels.size());
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        result.pixels[i * 4 + 3] = 255;
    }

    SelectObject(hMemDC, hOldFont);
    DeleteObject(hFont);
    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hDib);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);

    return result;
}

RawBitmap ScreenCapture::CreateSyntheticReactFrame(int width, int height) {
    RawBitmap result;
    HDC hScreenDC = GetDC(nullptr);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hDib = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    HGDIOBJ hOldBmp = SelectObject(hMemDC, hDib);

    RECT rc = { 0, 0, width, height };
    HBRUSH hBg = CreateSolidBrush(RGB(20, 22, 27));
    FillRect(hMemDC, &rc, hBg);
    DeleteObject(hBg);

    HFONT hFont = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas"
    );
    HGDIOBJ hOldFont = SelectObject(hMemDC, hFont);
    SetBkMode(hMemDC, TRANSPARENT);

    struct ColoredToken {
        const wchar_t* text;
        COLORREF color;
    };
    auto drawTokenLine = [&](int x, int y, const std::vector<ColoredToken>& tokens) {
        int curX = x;
        for (const auto& tk : tokens) {
            SetTextColor(hMemDC, tk.color);
            TextOutW(hMemDC, curX, y, tk.text, static_cast<int>(wcslen(tk.text)));
            SIZE sz = {};
            GetTextExtentPoint32W(hMemDC, tk.text, static_cast<int>(wcslen(tk.text)), &sz);
            curX += sz.cx;
        }
    };

    COLORREF cPurple = RGB(197, 134, 192);
    COLORREF cBlue   = RGB(86, 156, 214);
    COLORREF cCyan   = RGB(156, 220, 254);
    COLORREF cYellow = RGB(220, 220, 170);
    COLORREF cOrange = RGB(206, 145, 120);
    COLORREF cGreen  = RGB(181, 206, 168);
    COLORREF cWhite  = RGB(212, 212, 212);
    COLORREF cGold   = RGB(255, 215, 0);
    COLORREF cGray   = RGB(128, 128, 128);

    drawTokenLine(24, 16,  { { L"const ", cBlue }, { L"Login ", cYellow }, { L"= ", cWhite }, { L"() ", cGold }, { L"=> ", cBlue }, { L"{", cGold } });
    drawTokenLine(24, 38,  { { L"  const ", cBlue }, { L"{", cPurple }, { L"step,setStep,setUserPhoneData,userPhoneData,resetLoginState", cCyan }, { L"} = ", cWhite }, { L"useLoginStore", cYellow }, { L"();", cPurple } });
    drawTokenLine(24, 60,  { { L"  const ", cBlue }, { L"[", cPurple }, { L"phoneNumber,setPhoneNumber", cCyan }, { L"] = ", cWhite }, { L"useState", cYellow }, { L"(", cPurple }, { L"\"\"", cOrange }, { L");", cPurple } });
    drawTokenLine(24, 82,  { { L"  const ", cBlue }, { L"[", cPurple }, { L"selectedCountry,setSelectedCountry", cCyan }, { L"] = ", cWhite }, { L"useState", cYellow }, { L"(", cPurple }, { L"countries", cCyan }, { L"[", cBlue }, { L"0", cGreen }, { L"]);", cPurple } });
    drawTokenLine(24, 104, { { L"  const ", cBlue }, { L"[", cPurple }, { L"otp,setOtp", cCyan }, { L"] = ", cWhite }, { L"useState", cYellow }, { L"([", cPurple }, { L"\"\", \"\", \"\", \"\", \"\", \"\"", cOrange }, { L"])", cPurple } });
    drawTokenLine(24, 126, { { L"  const ", cBlue }, { L"[", cPurple }, { L"email,setEmail", cCyan }, { L"] = ", cWhite }, { L"useState", cYellow }, { L"(", cPurple }, { L"\"\"", cOrange }, { L")", cPurple } });
    drawTokenLine(24, 148, { { L"  return ", cPurple }, { L"(", cPurple } });
    drawTokenLine(24, 170, { { L"    <", cGray }, { L"div", cBlue }, { L">", cGray }, { L"Login", cWhite }, { L"</", cGray }, { L"div", cBlue }, { L">", cGray } });
    drawTokenLine(24, 192, { { L"  )", cPurple } });
    drawTokenLine(24, 214, { { L"}", cGold } });
    drawTokenLine(24, 258, { { L"export ", cPurple }, { L"default ", cPurple }, { L"Login", cYellow } });

    GdiFlush();

    result.width = width;
    result.height = height;
    result.stride = width * 4;
    result.pixels.resize(static_cast<size_t>(width) * height * 4);
    memcpy(result.pixels.data(), pBits, result.pixels.size());
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        result.pixels[i * 4 + 3] = 255;
    }

    SelectObject(hMemDC, hOldFont);
    DeleteObject(hFont);
    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hDib);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);

    return result;
}

RawBitmap ScreenCapture::LoadBitmapFromFile(const std::wstring& filePath) {
    RawBitmap result;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
        return result;
    }

    {
        Gdiplus::Bitmap bmp(filePath.c_str());
        if (bmp.GetLastStatus() == Gdiplus::Ok && bmp.GetWidth() > 0 && bmp.GetHeight() > 0) {
            int w = static_cast<int>(bmp.GetWidth());
            int h = static_cast<int>(bmp.GetHeight());
            Gdiplus::Rect lockRect(0, 0, w, h);
            Gdiplus::BitmapData data = {};
            if (bmp.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
                result.width = w;
                result.height = h;
                result.stride = w * 4;
                result.scaleFactor = 1.0f;
                result.pixels.resize(static_cast<size_t>(w) * h * 4);
                for (int y = 0; y < h; ++y) {
                    const uint8_t* srcRow = static_cast<const uint8_t*>(data.Scan0) + y * data.Stride;
                    uint8_t* dstRow = result.pixels.data() + static_cast<size_t>(y) * result.stride;
                    memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
                }
                bmp.UnlockBits(&data);
            }
        }
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return result;
}

RawBitmap ScreenCapture::LoadBitmapFromClipboard(HWND hwndOwner) {
    RawBitmap result;
    if (!OpenClipboard(hwndOwner)) {
        return result;
    }

    // 1. Check if the clipboard holds a bitmap (Win+Shift+S, Copy Image from browser, PrintScreen, etc.)
    if (IsClipboardFormatAvailable(CF_BITMAP)) {
        HBITMAP hBmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
        if (hBmp) {
            BITMAP bm = {};
            if (GetObjectW(hBmp, sizeof(bm), &bm) > 0 && bm.bmWidth > 0 && bm.bmHeight > 0) {
                int w = bm.bmWidth;
                int h = bm.bmHeight;

                BITMAPINFO bmi = {};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = w;
                bmi.bmiHeader.biHeight = -h; // Top-down DIB
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                result.width = w;
                result.height = h;
                result.stride = w * 4;
                result.scaleFactor = 1.0f;
                result.pixels.resize(static_cast<size_t>(w) * h * 4);

                HDC hScreenDC = GetDC(nullptr);
                if (hScreenDC) {
                    int rows = GetDIBits(hScreenDC, hBmp, 0, static_cast<UINT>(h), result.pixels.data(), &bmi, DIB_RGB_COLORS);
                    ReleaseDC(nullptr, hScreenDC);
                    if (rows > 0) {
                        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
                            result.pixels[i * 4 + 3] = 255;
                        }
                    } else {
                        result = RawBitmap{};
                    }
                }
            }
        }
    }

    // 2. Fallback: Check if the user copied an image file in File Explorer (CF_HDROP)
    std::wstring droppedFile;
    if (!result.valid() && IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (hDrop && DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0) > 0) {
            wchar_t pathBuf[MAX_PATH * 2] = {};
            if (DragQueryFileW(hDrop, 0, pathBuf, static_cast<UINT>(std::size(pathBuf))) > 0) {
                droppedFile = pathBuf;
            }
        }
    }

    CloseClipboard();

    if (!result.valid() && !droppedFile.empty()) {
        result = LoadBitmapFromFile(droppedFile);
    }
    return result;
}

} // namespace ScreenTextify
