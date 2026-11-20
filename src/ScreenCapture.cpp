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
    // Avoid upscaling if the region is already huge (> 2400px wide)
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
        // Bilinear interpolation for smooth character edges during OCR
        for (int dy = 0; dy < out.height; ++dy) {
            float sy = (dy + 0.5f) / out.scaleFactor - 0.5f;
            int iy = std::clamp(static_cast<int>(std::floor(sy)), 0, cropH - 1);
            int iy1 = std::min(iy + 1, cropH - 1);
            float fy = std::clamp(sy - iy, 0.0f, 1.0f);

            uint8_t* dstRow = out.pixels.data() + static_cast<size_t>(dy) * out.stride;
            for (int dx = 0; dx < out.width; ++dx) {
                float sx = (dx + 0.5f) / out.scaleFactor - 0.5f;
                int ix = std::clamp(static_cast<int>(std::floor(sx)), 0, cropW - 1);
                int ix1 = std::min(ix + 1, cropW - 1);
                float fx = std::clamp(sx - ix, 0.0f, 1.0f);

                const uint8_t* p00 = source.pixels.data() + static_cast<size_t>(y0 + iy) * source.stride + static_cast<size_t>(x0 + ix) * 4;
                const uint8_t* p10 = source.pixels.data() + static_cast<size_t>(y0 + iy) * source.stride + static_cast<size_t>(x0 + ix1) * 4;
                const uint8_t* p01 = source.pixels.data() + static_cast<size_t>(y0 + iy1) * source.stride + static_cast<size_t>(x0 + ix) * 4;
                const uint8_t* p11 = source.pixels.data() + static_cast<size_t>(y0 + iy1) * source.stride + static_cast<size_t>(x0 + ix1) * 4;

                for (int c = 0; c < 3; ++c) {
                    float top = p00[c] * (1.0f - fx) + p10[c] * fx;
                    float bot = p01[c] * (1.0f - fx) + p11[c] * fx;
                    float val = top * (1.0f - fy) + bot * fy;
                    dstRow[dx * 4 + c] = static_cast<uint8_t>(std::clamp(val + 0.5f, 0.0f, 255.0f));
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
