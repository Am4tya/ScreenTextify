#include "Types.h"
#include "AppStorage.h"
#include "ScreenCapture.h"
#include "OcrService.h"
#include "PaddleOcr.h"
#include "TextFormatter.h"
#include "OverlayWindow.h"
#include "HistoryWindow.h"

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace ScreenTextify {

namespace {
constexpr wchar_t TRAY_WND_CLASS[] = L"ScreenTextifyTrayHost";
constexpr UINT WM_TRAYICON = WM_APP + 10;

enum HotkeyId {
    HK_REGION_CAPTURE     = 1001,
    HK_FULLSCREEN_CAPTURE = 1002,
    HK_QUICKSNIP_CAPTURE  = 1003,
    HK_STITCH_CAPTURE     = 1004,
    HK_SHOW_HISTORY       = 1005
};

enum TrayMenuId {
    MENU_CAPTURE_REGION    = 3001,
    MENU_CAPTURE_FULL      = 3002,
    MENU_CAPTURE_QUICKSNIP = 3003,
    MENU_CAPTURE_STITCH    = 3004,
    MENU_OPEN_DASHBOARD    = 3005,
    MENU_TOGGLE_CODE       = 3006,
    MENU_TOGGLE_STARTWIN   = 3007,
    MENU_EXIT              = 3008
};

HICON CreateSelectextTrayIcon() {
    const int size = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    uint32_t* pixels = nullptr;
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    HBITMAP hMask = CreateBitmap(size, size, 1, 1, nullptr);
    ReleaseDC(nullptr, hdc);

    if (hColor && pixels) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                bool inside = (x >= 2 && x < 30 && y >= 2 && y < 30);
                if (!inside) {
                    pixels[y * size + x] = 0x00000000;
                    continue;
                }
                uint32_t col = 0xFF0F172A; // Deep slate navy
                bool topB = (y >= 4 && y <= 6) && ((x >= 4 && x <= 11) || (x >= 20 && x <= 27));
                bool botB = (y >= 25 && y <= 27) && ((x >= 4 && x <= 11) || (x >= 20 && x <= 27));
                bool leftB = (x >= 4 && x <= 6) && ((y >= 4 && y <= 11) || (y >= 20 && y <= 27));
                bool rightB = (x >= 25 && x <= 27) && ((y >= 4 && y <= 11) || (y >= 20 && y <= 27));

                bool tTop = (y >= 9 && y <= 12) && (x >= 9 && x <= 22);
                bool tStem = (y >= 12 && y <= 23) && (x >= 14 && x <= 17);

                if (topB || botB || leftB || rightB) {
                    col = 0xFF38BDF8; // Glowing cyan
                } else if (tTop || tStem) {
                    col = 0xFFF8FAFC; // Crisp white
                }
                pixels[y * size + x] = col;
            }
        }
    }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;
    HICON hIcon = CreateIconIndirect(&ii);

    if (hColor) DeleteObject(hColor);
    if (hMask) DeleteObject(hMask);
    return hIcon;
}

class AppController {
public:
    explicit AppController(HINSTANCE hInstance) : m_hInstance(hInstance) {}

    ~AppController() {
        RemoveTrayIcon();
        if (m_hwndTray) {
            UnregisterHotKey(m_hwndTray, HK_REGION_CAPTURE);
            UnregisterHotKey(m_hwndTray, HK_FULLSCREEN_CAPTURE);
            UnregisterHotKey(m_hwndTray, HK_QUICKSNIP_CAPTURE);
            UnregisterHotKey(m_hwndTray, HK_STITCH_CAPTURE);
            UnregisterHotKey(m_hwndTray, HK_SHOW_HISTORY);
            DestroyWindow(m_hwndTray);
            m_hwndTray = nullptr;
        }
        if (m_trayIcon) {
            DestroyIcon(m_trayIcon);
            m_trayIcon = nullptr;
        }
    }

    bool Initialize(bool showDashboardOnStart, bool showTrayBalloon = true, std::wstring* outInitError = nullptr, bool loadPersistedSettings = true) {
        if (loadPersistedSettings) {
            AppStorage::LoadSettings(m_settings);
        }
        m_trayIcon = CreateSelectextTrayIcon();

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = AppController::TrayWndProc;
        wc.hInstance = m_hInstance;
        wc.lpszClassName = TRAY_WND_CLASS;
        RegisterClassExW(&wc);

        m_hwndTray = CreateWindowExW(
            0, TRAY_WND_CLASS, L"ScreenTextify Tray Host",
            0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, m_hInstance, this
        );
        if (!m_hwndTray) {
            if (outInitError) *outInitError = L"Failed to create Tray Host message window.";
            return false;
        }

        m_overlay = std::make_unique<OverlayWindow>(
            m_hInstance,
            &m_settings,
            [this](const std::wstring& text, bool codeMode, size_t wordCount, double ocrTimeMs) {
                if (m_historyWnd) {
                    m_historyWnd->AddHistoryEntry(text, codeMode, wordCount, ocrTimeMs);
                }
            }
        );
        if (!m_overlay->Initialize()) {
            if (outInitError) *outInitError = L"Failed to initialize Direct2D OverlayWindow.";
            return false;
        }

        m_historyWnd = std::make_unique<HistoryWindow>(
            m_hInstance,
            &m_settings,
            [this](CaptureLaunchMode mode) {
                m_overlay->StartCaptureSession(mode);
            }
        );
        if (!m_historyWnd->Initialize()) {
            if (outInitError) *outInitError = L"Failed to initialize Direct2D Studio Dashboard window.";
            return false;
        }

        if (m_trayIcon && m_historyWnd->GetHwnd()) {
            SendMessageW(m_historyWnd->GetHwnd(), WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_trayIcon));
            SendMessageW(m_historyWnd->GetHwnd(), WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_trayIcon));
        }

        SetupTrayIcon(showTrayBalloon);
        RegisterGlobalHotkeys();

        if (showDashboardOnStart) {
            m_historyWnd->ShowAndFocus();
            UpdateWindow(m_historyWnd->GetHwnd());
        }
        return true;
    }

private:
    void SetupTrayIcon(bool showBalloon) {
        m_nid = {};
        m_nid.cbSize = sizeof(NOTIFYICONDATAW);
        m_nid.hWnd = m_hwndTray;
        m_nid.uID = 1;
        m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | (showBalloon ? NIF_INFO : 0);
        m_nid.uCallbackMessage = WM_TRAYICON;
        m_nid.hIcon = m_trayIcon ? m_trayIcon : LoadIcon(nullptr, IDI_APPLICATION);
        wcscpy_s(m_nid.szTip, L"ScreenTextify Studio \x2014 Alt+Shift+S to Copy Text from Videos");
        if (showBalloon) {
            wcscpy_s(m_nid.szInfoTitle, L"ScreenTextify Studio Active");
            wcscpy_s(m_nid.szInfo, L"Alt+Shift+S: Region Selectext | Alt+Shift+D: Full Screen | Alt+Shift+C: Quick Copy");
            m_nid.dwInfoFlags = NIIF_INFO;
        }
        Shell_NotifyIconW(NIM_ADD, &m_nid);
    }

    void RemoveTrayIcon() {
        if (m_nid.hWnd) {
            Shell_NotifyIconW(NIM_DELETE, &m_nid);
            m_nid.hWnd = nullptr;
        }
    }

    void RegisterGlobalHotkeys() {
        RegisterHotKey(m_hwndTray, HK_REGION_CAPTURE, m_settings.hotkeyRegionModifiers | MOD_NOREPEAT, m_settings.hotkeyRegionVk);
        RegisterHotKey(m_hwndTray, HK_FULLSCREEN_CAPTURE, m_settings.hotkeyFullScreenModifiers | MOD_NOREPEAT, m_settings.hotkeyFullScreenVk);
        RegisterHotKey(m_hwndTray, HK_QUICKSNIP_CAPTURE, m_settings.hotkeyQuickSnipModifiers | MOD_NOREPEAT, m_settings.hotkeyQuickSnipVk);
        RegisterHotKey(m_hwndTray, HK_STITCH_CAPTURE, m_settings.hotkeyStitchModifiers | MOD_NOREPEAT, m_settings.hotkeyStitchVk);
        RegisterHotKey(m_hwndTray, HK_SHOW_HISTORY, m_settings.hotkeyHistoryModifiers | MOD_NOREPEAT, m_settings.hotkeyHistoryVk);
    }

    void ShowTrayContextMenu() {
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, MENU_CAPTURE_REGION, L"Interactive Region Selectext\tAlt+Shift+S");
        AppendMenuW(hMenu, MF_STRING, MENU_CAPTURE_FULL, L"Instant Full-Screen Selectext\tAlt+Shift+D");
        AppendMenuW(hMenu, MF_STRING, MENU_CAPTURE_QUICKSNIP, L"Quick Snip & Instant Copy\tAlt+Shift+C");
        AppendMenuW(hMenu, MF_STRING, MENU_CAPTURE_STITCH, L"Stitch & Append Scrolled Code\tAlt+Shift+A");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING | (m_settings.codeIndentationMode ? MF_CHECKED : MF_UNCHECKED),
                    MENU_TOGGLE_CODE, L"Preserve Code Indentation (Code Mode)");
        AppendMenuW(hMenu, MF_STRING | (m_settings.startWithWindows ? MF_CHECKED : MF_UNCHECKED),
                    MENU_TOGGLE_STARTWIN, L"Start with Windows (System Tray)");
        AppendMenuW(hMenu, MF_STRING, MENU_OPEN_DASHBOARD, L"Open Studio Dashboard\tAlt+Shift+H");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, MENU_EXIT, L"Exit ScreenTextify");

        POINT pt = {};
        GetCursorPos(&pt);
        SetForegroundWindow(m_hwndTray);
        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, m_hwndTray, nullptr);
        DestroyMenu(hMenu);
    }

    static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        AppController* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<AppController*>(cs->lpCreateParams);
            if (self) {
                self->m_hwndTray = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
        } else {
            self = reinterpret_cast<AppController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self) return self->HandleTrayMessage(hwnd, msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT HandleTrayMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_HOTKEY: {
            if (wParam == HK_REGION_CAPTURE) {
                m_overlay->StartCaptureSession(CaptureLaunchMode::RegionInteractive);
                return 0;
            }
            if (wParam == HK_FULLSCREEN_CAPTURE) {
                m_overlay->StartCaptureSession(CaptureLaunchMode::FullScreenSelect);
                return 0;
            }
            if (wParam == HK_QUICKSNIP_CAPTURE) {
                m_overlay->StartCaptureSession(CaptureLaunchMode::QuickSnipCopy);
                return 0;
            }
            if (wParam == HK_STITCH_CAPTURE) {
                m_overlay->StartCaptureSession(CaptureLaunchMode::StitchAppend);
                return 0;
            }
            if (wParam == HK_SHOW_HISTORY) {
                m_historyWnd->ShowAndFocus();
                return 0;
            }
            break;
        }

        case WM_TRAYICON: {
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                ShowTrayContextMenu();
                return 0;
            }
            if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
                m_historyWnd->ShowAndFocus();
                return 0;
            }
            break;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
            case MENU_CAPTURE_REGION:
                m_overlay->StartCaptureSession(CaptureLaunchMode::RegionInteractive);
                return 0;
            case MENU_CAPTURE_FULL:
                m_overlay->StartCaptureSession(CaptureLaunchMode::FullScreenSelect);
                return 0;
            case MENU_CAPTURE_QUICKSNIP:
                m_overlay->StartCaptureSession(CaptureLaunchMode::QuickSnipCopy);
                return 0;
            case MENU_CAPTURE_STITCH:
                m_overlay->StartCaptureSession(CaptureLaunchMode::StitchAppend);
                return 0;
            case MENU_OPEN_DASHBOARD:
                m_historyWnd->ShowAndFocus();
                return 0;
            case MENU_TOGGLE_CODE:
                m_settings.codeIndentationMode = !m_settings.codeIndentationMode;
                AppStorage::SaveSettings(m_settings);
                m_historyWnd->RefreshControls();
                return 0;
            case MENU_TOGGLE_STARTWIN: {
                bool target = !m_settings.startWithWindows;
                if (AppStorage::SetStartWithWindows(target)) {
                    m_settings.startWithWindows = target;
                    AppStorage::SaveSettings(m_settings);
                    m_historyWnd->RefreshControls();
                }
                return 0;
            }
            case MENU_EXIT:
                PostQuitMessage(0);
                return 0;
            }
            break;
        }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HINSTANCE m_hInstance = nullptr;
    HWND m_hwndTray = nullptr;
    HICON m_trayIcon = nullptr;
    NOTIFYICONDATAW m_nid = {};
    AppSettings m_settings;
    std::unique_ptr<OverlayWindow> m_overlay;
    std::unique_ptr<HistoryWindow> m_historyWnd;
};

void WriteStdout(const std::wstring& outStr) {
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hStdOut || hStdOut == INVALID_HANDLE_VALUE) return;
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, outStr.c_str(), static_cast<int>(outStr.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(utf8Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, outStr.c_str(), static_cast<int>(outStr.size()), utf8.data(), utf8Len, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(hStdOut, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

void AttachParentConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fOut = nullptr;
        if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr) {
            freopen_s(&fOut, "CONOUT$", "w", stdout);
            freopen_s(&fOut, "CONOUT$", "w", stderr);
        }
    }
}

// ScreenTextify.exe --ocr <image> [x y w h] [--scale N] [--win]
// Prints the code-formatted OCR result of an image file (or a sub-rectangle of it).
int RunOcrCli() {
    AttachParentConsole();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring path;
    std::vector<float> rect;
    int scale = 0;
    bool useWindows = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--ocr" && i + 1 < argc) { path = argv[++i]; }
        else if (a == L"--scale" && i + 1 < argc) { scale = _wtoi(argv[++i]); }
        else if (a == L"--win") { useWindows = true; }
        else if (!a.empty() && (iswdigit(a[0]) || a[0] == L'.')) { rect.push_back(static_cast<float>(_wtof(a.c_str()))); }
    }
    LocalFree(argv);

    RawBitmap img = ScreenCapture::LoadBitmapFromFile(path);
    if (!img.valid()) {
        WriteStdout(L"Could not load image: " + path + L"\r\n");
        return 1;
    }
    RectF region{ 0.0f, 0.0f, static_cast<float>(img.width), static_cast<float>(img.height) };
    if (rect.size() == 4) region = { rect[0], rect[1], rect[2], rect[3] };
    if (scale <= 0) scale = (region.width < 950.0f && region.height < 600.0f) ? 2 : 1;

    RawBitmap crop = ScreenCapture::CropAndScale(img, region, scale);
    std::wstring err;
    if (!useWindows) PaddleOcr::EnsureInitialized(&err);  // exclude one-time model loading from timing
    OcrDocument doc = useWindows ? OcrService::RecognizeWithWindowsOcr(crop, &err) : OcrService::RecognizeBitmap(crop, &err);
    std::wstringstream ss;
    ss << L"[engine: " << (useWindows ? L"Windows OCR" : OcrService::GetLastEngineName())
       << L", " << doc.recognitionTimeMs << L" ms, " << doc.totalWords() << L" words]\r\n";
    if (!err.empty()) ss << L"[error: " << err << L"]\r\n";
    ss << TextFormatter::FormatDocument(doc, false, true, nullptr) << L"\r\n";
    WriteStdout(ss.str());
    return 0;
}

size_t LevenshteinDistance(const std::wstring& a, const std::wstring& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<size_t> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        prev.swap(cur);
    }
    return prev[m];
}

// Simulates YouTube 720p H.264/VP9 video compression: YUV 4:2:0 chroma subsampling + 0.84x downscale + re-expand
RawBitmap DegradeLikeYouTubeVideo(const RawBitmap& src) {
    RawBitmap yuv = src;
    // 2x2 chroma subsampling in YCbCr space + 8x8 block quantization jitter
    for (int y = 0; y + 1 < yuv.height; y += 2) {
        for (int x = 0; x + 1 < yuv.width; x += 2) {
            float cbSum = 0.0f, crSum = 0.0f;
            float ys[2][2] = {};
            for (int dy = 0; dy < 2; ++dy) {
                const uint8_t* row = src.pixels.data() + static_cast<size_t>(y + dy) * src.stride;
                for (int dx = 0; dx < 2; ++dx) {
                    float b = row[(x + dx) * 4 + 0];
                    float g = row[(x + dx) * 4 + 1];
                    float r = row[(x + dx) * 4 + 2];
                    ys[dy][dx] = 0.299f * r + 0.587f * g + 0.114f * b;
                    cbSum += 128.0f - 0.168736f * r - 0.331264f * g + 0.5f * b;
                    crSum += 128.0f + 0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            }
            float cb = std::round((cbSum * 0.25f) / 4.0f) * 4.0f;
            float cr = std::round((crSum * 0.25f) / 4.0f) * 4.0f;
            for (int dy = 0; dy < 2; ++dy) {
                uint8_t* dRow = yuv.pixels.data() + static_cast<size_t>(y + dy) * yuv.stride;
                for (int dx = 0; dx < 2; ++dx) {
                    float yVal = std::round(ys[dy][dx] / 2.0f) * 2.0f;
                    int r = static_cast<int>(std::clamp(yVal + 1.402f * (cr - 128.0f), 0.0f, 255.0f));
                    int g = static_cast<int>(std::clamp(yVal - 0.344136f * (cb - 128.0f) - 0.714136f * (cr - 128.0f), 0.0f, 255.0f));
                    int b = static_cast<int>(std::clamp(yVal + 1.772f * (cb - 128.0f), 0.0f, 255.0f));
                    dRow[(x + dx) * 4 + 0] = static_cast<uint8_t>(b);
                    dRow[(x + dx) * 4 + 1] = static_cast<uint8_t>(g);
                    dRow[(x + dx) * 4 + 2] = static_cast<uint8_t>(r);
                    dRow[(x + dx) * 4 + 3] = 255;
                }
            }
        }
    }
    return yuv;
}

RawBitmap RenderCodeSnippet(const std::vector<std::wstring>& lines, const wchar_t* fontName, int fontSize, bool lightTheme) {
    int width = 880;
    int height = static_cast<int>(lines.size()) * 24 + 32;
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
    HBRUSH hBg = CreateSolidBrush(lightTheme ? RGB(248, 249, 250) : RGB(22, 24, 30));
    FillRect(hMemDC, &rc, hBg);
    DeleteObject(hBg);

    HFONT hFont = CreateFontW(
        fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, fontName
    );
    HGDIOBJ hOldFont = SelectObject(hMemDC, hFont);
    SetBkMode(hMemDC, TRANSPARENT);

    COLORREF paletteDark[] = { RGB(156, 220, 254), RGB(206, 145, 120), RGB(197, 134, 192), RGB(220, 220, 170), RGB(86, 156, 214) };
    COLORREF paletteLight[] = { RGB(0, 16, 128), RGB(163, 21, 21), RGB(121, 94, 38), RGB(9, 134, 88), RGB(175, 0, 219) };

    for (size_t i = 0; i < lines.size(); ++i) {
        int curX = 24;
        int curY = 16 + static_cast<int>(i) * 24;
        const std::wstring& ln = lines[i];
        size_t pos = 0;
        int colorIdx = static_cast<int>(i);
        while (pos < ln.size()) {
            size_t next = pos + 1;
            if (std::iswspace(ln[pos])) {
                while (next < ln.size() && std::iswspace(ln[next])) ++next;
            } else if (std::iswalnum(ln[pos]) || ln[pos] == L'_') {
                while (next < ln.size() && (std::iswalnum(ln[next]) || ln[next] == L'_')) ++next;
            }
            std::wstring chunk = ln.substr(pos, next - pos);
            SetTextColor(hMemDC, lightTheme ? paletteLight[colorIdx % 5] : paletteDark[colorIdx % 5]);
            TextOutW(hMemDC, curX, curY, chunk.c_str(), static_cast<int>(chunk.size()));
            SIZE sz = {};
            GetTextExtentPoint32W(hMemDC, chunk.c_str(), static_cast<int>(chunk.size()), &sz);
            curX += sz.cx;
            pos = next;
            if (!std::iswspace(chunk[0])) ++colorIdx;
        }
    }

    GdiFlush();
    result.width = width;
    result.height = height;
    result.stride = width * 4;
    result.pixels.resize(static_cast<size_t>(width) * height * 4);
    memcpy(result.pixels.data(), pBits, result.pixels.size());
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) result.pixels[i * 4 + 3] = 255;

    SelectObject(hMemDC, hOldFont);
    DeleteObject(hFont);
    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hDib);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);
    return result;
}

int RunBenchmark() {
    AttachParentConsole();
    PaddleOcr::EnsureInitialized();

    struct BenchCase {
        const wchar_t* name;
        const wchar_t* font;
        int fontSize;
        bool lightTheme;
        std::vector<std::wstring> lines;
    };

    std::vector<BenchCase> suite = {
        { L"Python 3 (Consolas 14px Dark + YouTube YUV420)", L"Consolas", -14, false, {
            L"def fetch_metrics(items: list[dict]) -> dict:",
            L"  active = [x for x in items if x.get(\"active\") != False]",
            L"  total = sum(float(x.get(\"score\", 0.0)) for x in active)",
            L"  return {\"count\": len(active), \"total\": round(total, 2)}"
        }},
        { L"C++20 STL (Cascadia Code 15px Dark + YouTube YUV420)", L"Cascadia Code", -15, false, {
            L"template <typename T>",
            L"auto filter_positive(const std::vector<T>& input) {",
            L"  std::vector<T> out;",
            L"  for (const auto& val : input) {",
            L"    if (val > 0) out.push_back(val * 2);",
            L"  }",
            L"  return out;",
            L"}"
        }},
        { L"Rust Match (Consolas 15px Light + YouTube YUV420)", L"Consolas", -15, true, {
            L"pub fn parse_port(raw: &str) -> Result<u16, String> {",
            L"  match raw.trim().parse::<u16>() {",
            L"    Ok(p) if p >= 1024 => Ok(p),",
            L"    _ => Err(format!(\"invalid port: {}\", raw)),",
            L"  }",
            L"}"
        }},
        { L"TypeScript Fetch (Cascadia Mono 14px Dark + YouTube YUV420)", L"Cascadia Mono", -14, false, {
            L"export async function postJson(url: string, payload: unknown) {",
            L"  const headers = { \"Content-Type\": \"application/json\" };",
            L"  const res = await fetch(url, { method: \"POST\", headers });",
            L"  if (!res.ok || res.status >= 400) {",
            L"    throw new Error(\"HTTP error: \" + res.status);",
            L"  }",
            L"  return await res.json();",
            L"}"
        }}
    };

    std::wstringstream report;
    report << L"=== Held-Out Multi-Language Degraded YouTube Video Benchmark ===\r\n";

    for (const wchar_t* modelOpt : { L"v4", L"v5", L"ensemble" }) {
        PaddleOcr::Options opts;
        opts.model = modelOpt;
        opts.constrainToCodeAscii = true;
        opts.dualViewVerify = ( std::wstring(modelOpt) == L"ensemble" );
        PaddleOcr::SetOptions(opts);

        size_t totalChars = 0;
        size_t totalEdits = 0;
        double totalMs = 0.0;

        report << L"\r\n--- Configuration: [" << modelOpt << L"] ---\r\n";
        for (const auto& tc : suite) {
            RawBitmap raw = RenderCodeSnippet(tc.lines, tc.font, tc.fontSize, tc.lightTheme);
            RawBitmap degraded = DegradeLikeYouTubeVideo(raw);
            RawBitmap scaled = ScreenCapture::CropAndScale(degraded, { 0.0f, 0.0f, static_cast<float>(degraded.width), static_cast<float>(degraded.height) }, 2);

            std::wstring err;
            OcrDocument doc = OcrService::RecognizeBitmap(scaled, &err);
            totalMs += doc.recognitionTimeMs;
            std::wstring got = TextFormatter::FormatDocument(doc, false, true, nullptr);

            std::wstring expected;
            for (size_t i = 0; i < tc.lines.size(); ++i) {
                if (i > 0) expected += L"\r\n";
                expected += tc.lines[i];
            }

            size_t dist = LevenshteinDistance(expected, got);
            totalChars += expected.size();
            totalEdits += dist;
            double acc = 100.0 * (1.0 - static_cast<double>(dist) / std::max<size_t>(1, expected.size()));
            report << L"  * " << tc.name << L": " << std::fixed << std::setprecision(2) << acc
                   << L"% (" << dist << L" edits / " << expected.size() << L" chars, " << doc.recognitionTimeMs << L" ms)\r\n";
            if (dist > 0) {
                report << L"    [GOT]:\r\n" << got << L"\r\n";
            }
        }
        double overallAcc = 100.0 * (1.0 - static_cast<double>(totalEdits) / std::max<size_t>(1, totalChars));
        report << L"==> TOTAL [" << modelOpt << L"]: " << std::fixed << std::setprecision(2) << overallAcc
               << L"% character accuracy (" << totalEdits << L" edits / " << totalChars << L" chars, "
               << totalMs << L" ms total)\r\n";
    }

    WriteStdout(report.str());
    return 0;
}

int RunSelfTest(HINSTANCE hInstance) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fOut = nullptr;
        if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr) {
            freopen_s(&fOut, "CONOUT$", "w", stdout);
            freopen_s(&fOut, "CONOUT$", "w", stderr);
        }
    }

    std::wstringstream report;
    report << L"=== ScreenTextify Studio v2.0 End-to-End Verification ===\r\n";

    // 1. Test full GUI & Direct2D Window Initialization
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring initErr;
    {
        AppController testApp(hInstance);
        if (!testApp.Initialize(true, false, &initErr, false)) {
            report << L"FAILED GUI Initialization: " << initErr << L"\r\n";
            std::wcerr << report.str();
            CoUninitialize();
            return 1;
        }
        report << L"1. GUI & Direct2D Studio Initialization: OK (TrayHost, OverlayWindow, StudioDashboard created & rendered)\r\n";
    }

    // 2. Test Windows Neural OCR & Code Indentation Engine on Synthetic Multi-Colored VS Code Dark+ Frame
    report << L"2. Active OCR Language: " << OcrService::GetActiveLanguageName() << L"\r\n";

    RawBitmap codeFrame = ScreenCapture::CreateSyntheticCodeFrame(800, 390);
    RawBitmap upscaledFrame = ScreenCapture::CropAndScale(codeFrame, { 0.0f, 0.0f, 800.0f, 390.0f }, 2);
    report << L"3. Generated 16-line VS Code Dark+ frame with #264F78 blue selection highlight (" << codeFrame.width << L"x" << codeFrame.height << L", 2x upscaled)\r\n";

    std::wstring err;
    OcrDocument doc = OcrService::RecognizeBitmap(upscaledFrame, &err);
    if (!err.empty()) {
        report << L"OCR Error: " << err << L"\r\n";
        std::wcerr << report.str();
        CoUninitialize();
        return 1;
    }

    report << L"4. OCR completed in " << doc.recognitionTimeMs << L" ms. Lines: "
           << doc.lines.size() << L", Words: " << doc.totalWords() << L"\r\n";

    if (doc.empty()) {
        report << L"FAILED: No OCR lines detected on synthetic code frame!\r\n";
        std::wcerr << report.str();
        CoUninitialize();
        return 1;
    }

    size_t wordCount = 0;
    std::wstring formattedCode = TextFormatter::FormatDocument(doc, false, true, &wordCount);
    report << L"5. Reconstructed Multi-Color Code Block (Indentation ON, " << wordCount << L" tokens):\r\n"
           << L"----------------------------------------\r\n"
           << formattedCode << L"\r\n"
           << L"----------------------------------------\r\n";

    RawBitmap reactFrame = ScreenCapture::CreateSyntheticReactFrame(820, 300);
    RawBitmap upscaledReact = ScreenCapture::CropAndScale(reactFrame, { 0.0f, 0.0f, 820.0f, 300.0f }, 2);
    OcrDocument reactDoc = OcrService::RecognizeBitmap(upscaledReact, &err);
    size_t reactWords = 0;
    std::wstring formattedReact = TextFormatter::FormatDocument(reactDoc, false, true, &reactWords);
    report << L"6. Reconstructed React Login.jsx Block (" << reactDoc.recognitionTimeMs << L" ms, " << reactWords << L" tokens):\r\n"
           << L"----------------------------------------\r\n"
           << formattedReact << L"\r\n"
           << L"----------------------------------------\r\n";

    // Test the untouched crops from the user's screenshots
    RawBitmap userShot2 = ScreenCapture::LoadBitmapFromFile(
        L"C:/Users/Admin/.gemini/antigravity/brain/9d56429c-94b7-4d98-b805-0cb32399cdc1/.user_uploaded/media_1794651879100_22c51b90.jpg"
    );
    if (userShot2.valid()) {
        RawBitmap senderCrop = ScreenCapture::CropAndScale(userShot2, { 96.0f, 226.0f, 172.0f, 17.0f }, 3);
        OcrDocument senderDoc = OcrService::RecognizeBitmap(senderCrop, &err);
        report << L"7. User Screenshot #2 [.populate(\"sender\",] on Blue Highlight: "
               << TextFormatter::FormatDocument(senderDoc, false, true, nullptr) << L"\r\n";

        RawBitmap msgIdCrop = ScreenCapture::CropAndScale(userShot2, { 489.0f, 202.0f, 119.0f, 21.0f }, 3);
        OcrDocument msgIdDoc = OcrService::RecognizeBitmap(msgIdCrop, &err);
        report << L"8. User Screenshot #2 [(message?._id)] on Blue Highlight: "
               << TextFormatter::FormatDocument(msgIdDoc, false, true, nullptr) << L"\r\n";

        RawBitmap bracesCrop = ScreenCapture::CropAndScale(userShot2, { 42.0f, 374.0f, 55.0f, 52.0f }, 3);
        OcrDocument bracesDoc = OcrService::RecognizeBitmap(bracesCrop, &err);
        report << L"9. User Screenshot #2 Standalone Braces [} and };]:\r\n"
               << TextFormatter::FormatDocument(bracesDoc, false, true, nullptr) << L"\r\n";
    }

    RawBitmap userShot3 = ScreenCapture::LoadBitmapFromFile(
        L"C:/Users/Admin/.gemini/antigravity/brain/9d56429c-94b7-4d98-b805-0cb32399cdc1/.user_uploaded/media_1794894842461_27ef79e7.jpg"
    );
    if (userShot3.valid()) {
        RawBitmap importsCrop = ScreenCapture::CropAndScale(userShot3, { 80.0f, 58.0f, 470.0f, 70.0f }, 2);
        OcrDocument importsDoc = OcrService::RecognizeBitmap(importsCrop, &err);
        report << L"10. User Screenshot #3 Dimmed Imports (" << importsDoc.recognitionTimeMs << L" ms):\r\n"
               << TextFormatter::FormatDocument(importsDoc, false, true, nullptr) << L"\r\n";
    }

    // 11. Verify Multi-Frame Stitch & Append fuzzy overlap deduplication
    {
        std::wstring frame1 =
            L"async function loadUser(id) {\r\n"
            L"  const res = await fetch('/api/users/' + id);\r\n"
            L"  if (!res.ok) throw new Error('Failed');\r\n"
            L"  const data = await res.json();";
        std::wstring frame2 =
            L"  if (!res.ok) throw new Error('Failed')\r\n" // slight 1-char diff (dropped ';') to test fuzzy overlap!
            L"  const data = await res.json();\r\n"
            L"  return data.user;\r\n"
            L"}";
        int overlap = 0, appended = 0;
        std::wstring stitched = TextFormatter::StitchCodeBlocks(frame1, frame2, &overlap, &appended);
        if (overlap != 2 || appended != 2) {
            report << L"FAILED StitchCodeBlocks: expected overlap=2, appended=2, got overlap="
                   << overlap << L", appended=" << appended << L"\r\n";
            WriteStdout(report.str());
            CoUninitialize();
            return 1;
        }
        report << L"11. Multi-Frame Stitch & Append Deduplication: OK (merged " << overlap
               << L" overlapping lines, appended " << appended << L" new lines)\r\n";
    }

    report << L"=== ALL GUI & OCR SELF-TESTS PASSED ===\r\n";

    std::wstring outStr = report.str();
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut && hStdOut != INVALID_HANDLE_VALUE) {
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, outStr.c_str(), static_cast<int>(outStr.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(utf8Len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, outStr.c_str(), static_cast<int>(outStr.size()), utf8.data(), utf8Len, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(hStdOut, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }

    CoUninitialize();
    return 0;
}

} // namespace
} // namespace ScreenTextify

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring cmdLine = lpCmdLine ? lpCmdLine : L"";
    if (cmdLine.find(L"--benchmark") != std::wstring::npos) {
        return ScreenTextify::RunBenchmark();
    }
    if (cmdLine.find(L"--ocr") != std::wstring::npos) {
        return ScreenTextify::RunOcrCli();
    }
    if (cmdLine.find(L"--self-test") != std::wstring::npos) {
        return ScreenTextify::RunSelfTest(hInstance);
    }

    // Load the OCR models in the background so the first capture is instant
    ScreenTextify::PaddleOcr::WarmUpAsync();

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)hr;

    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\ScreenTextify_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"ScreenTextifyDashboardWnd", nullptr);
        if (existing) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            else ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        if (hMutex) CloseHandle(hMutex);
        CoUninitialize();
        return 0;
    }

    bool startHiddenInTray = (cmdLine.find(L"--tray") != std::wstring::npos);

    ScreenTextify::AppController app(hInstance);
    std::wstring initErr;
    if (!app.Initialize(!startHiddenInTray, true, &initErr)) {
        std::wstring msg = L"Failed to initialize ScreenTextify:\n" + initErr;
        MessageBoxW(nullptr, msg.c_str(), L"ScreenTextify Error", MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        CoUninitialize();
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hMutex) CloseHandle(hMutex);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
