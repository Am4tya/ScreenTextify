#include "HistoryWindow.h"
#include "AppStorage.h"
#include "OverlayWindow.h"
#include "OcrService.h"
#include "ScreenCapture.h"
#include "TextFormatter.h"

#include <windowsx.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cmath>

namespace ScreenTextify {

namespace {
constexpr wchar_t HISTORY_CLASS_NAME[] = L"ScreenTextifyDashboardWnd";

enum ActionId {
    ACT_HERO_REGION     = 101,
    ACT_HERO_FULLSCREEN = 102,
    ACT_HERO_QUICKSNIP  = 103,
    ACT_HERO_STITCH     = 104,

    ACT_TOGGLE_CODE      = 201,
    ACT_TOGGLE_LINEMODE  = 202,
    ACT_TOGGLE_AUTOCOPY  = 203,
    ACT_TOGGLE_UPSCALE   = 204,
    ACT_TOGGLE_AUTOCLOSE = 205,
    ACT_TOGGLE_STARTWIN  = 206,

    ACT_BTN_COPY_CODE   = 301,
    ACT_BTN_SAVE_FILE   = 302,
    ACT_BTN_DEMO_OCR    = 303,
    ACT_BTN_CLEAR_HIST  = 304,
    ACT_BTN_PASTE_IMG   = 305,

    ACT_HIST_ITEM_BASE  = 1000
};

void ConfigureSingleLineFormat(IDWriteFactory* factory, IDWriteTextFormat* fmt, bool addEllipsis = true) {
    if (!fmt) return;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (addEllipsis && factory) {
        IDWriteInlineObject* ellipsis = nullptr;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(fmt, &ellipsis)) && ellipsis) {
            DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            fmt->SetTrimming(&trimming, ellipsis);
            ellipsis->Release();
        }
    }
}
} // namespace

HistoryWindow::HistoryWindow(HINSTANCE hInstance, AppSettings* settings, StartCaptureCallback onStartCapture)
    : m_hInstance(hInstance), m_settings(settings), m_onStartCapture(std::move(onStartCapture)) {}

HistoryWindow::~HistoryWindow() {
    DiscardD2DResources();
    if (m_titleFormat) m_titleFormat->Release();
    if (m_subtitleFormat) m_subtitleFormat->Release();
    if (m_cardTitleFormat) m_cardTitleFormat->Release();
    if (m_bodyFormat) m_bodyFormat->Release();
    if (m_badgeFormat) m_badgeFormat->Release();
    if (m_monoFormat) m_monoFormat->Release();
    if (m_lineNumFormat) m_lineNumFormat->Release();
    if (m_dwriteFactory) m_dwriteFactory->Release();
    if (m_d2dFactory) m_d2dFactory->Release();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

bool HistoryWindow::Initialize() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&m_dwriteFactory)
    );
    if (FAILED(hr)) return false;

    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", &m_titleFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"en-us", &m_subtitleFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.5f, L"en-us", &m_cardTitleFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_bodyFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &m_badgeFormat);
    m_dwriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_monoFormat);
    m_dwriteFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"en-us", &m_lineNumFormat);

    ConfigureSingleLineFormat(m_dwriteFactory, m_titleFormat, true);
    ConfigureSingleLineFormat(m_dwriteFactory, m_subtitleFormat, true);
    ConfigureSingleLineFormat(m_dwriteFactory, m_cardTitleFormat, true);
    ConfigureSingleLineFormat(m_dwriteFactory, m_bodyFormat, true);
    ConfigureSingleLineFormat(m_dwriteFactory, m_badgeFormat, true);
    ConfigureSingleLineFormat(m_dwriteFactory, m_monoFormat, false);
    ConfigureSingleLineFormat(m_dwriteFactory, m_lineNumFormat, false);

    if (m_badgeFormat) {
        m_badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_badgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (m_lineNumFormat) {
        m_lineNumFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    m_ocrLangName = OcrService::GetActiveLanguageName();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = HistoryWindow::WndProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = HISTORY_CLASS_NAME;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    int winW = 1080;
    int winH = 710;
    int sx = std::max(20, (GetSystemMetrics(SM_CXSCREEN) - winW) / 2);
    int sy = std::max(20, (GetSystemMetrics(SM_CYSCREEN) - winH) / 2);

    m_hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        HISTORY_CLASS_NAME,
        L"ScreenTextify Studio \x2014 Selectext for Desktop Videos & Images",
        WS_OVERLAPPEDWINDOW,
        sx, sy, winW, winH,
        nullptr, nullptr, m_hInstance, this
    );

    if (!m_hwnd) return false;

    DragAcceptFiles(m_hwnd, TRUE);

    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

    if (!AppStorage::LoadHistory(m_history) || m_history.empty()) {
        CaptureHistoryItem welcome;
        welcome.text =
            L"// Welcome to ScreenTextify Studio!\r\n"
            L"// Alt+Shift+S: Region Selectext | Alt+Shift+A: Stitch scrolled code\r\n"
            L"// Tip: Drag & drop any screenshot here or press Ctrl+V to OCR it!\r\n"
            L"int calculateSum(int n) {\r\n"
            L"    int total = 0;\r\n"
            L"    for (int i = 1; i <= n; ++i) {\r\n"
            L"        total += i;\r\n"
            L"    }\r\n"
            L"    return total;\r\n"
            L"}";
        welcome.timestamp = L"Welcome";
        welcome.wasCodeMode = true;
        welcome.wordCount = 29;
        welcome.ocrTimeMs = 0.0;
        m_history.push_back(std::move(welcome));
    }

    return true;
}

void HistoryWindow::RefreshControls() {
    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void HistoryWindow::ShowAndFocus() {
    if (!m_hwnd) return;
    if (IsIconic(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    } else {
        ShowWindow(m_hwnd, SW_SHOW);
    }
    SetForegroundWindow(m_hwnd);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HistoryWindow::AddHistoryEntry(const std::wstring& text, bool codeMode, size_t wordCount, double ocrTimeMs) {
    std::time_t now = std::time(nullptr);
    std::tm localTm = {};
    localtime_s(&localTm, &now);
    wchar_t timeBuf[32] = {};
    wcsftime(timeBuf, 32, L"%H:%M:%S", &localTm);

    CaptureHistoryItem item;
    item.text = text;
    item.timestamp = timeBuf;
    item.wasCodeMode = codeMode;
    item.wordCount = wordCount;
    item.ocrTimeMs = ocrTimeMs;

    m_history.insert(m_history.begin(), item);
    if (m_history.size() > 30) {
        m_history.pop_back();
    }
    m_selectedHistoryIdx = 0;
    m_codeScrollY = 0.0f;
    AppStorage::SaveHistory(m_history);

    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void HistoryWindow::RunOcrOnBitmap(const RawBitmap& bmp, const std::wstring& sourceLabel) {
    if (!bmp.valid()) {
        m_statusBanner = L"Could not load image from " + sourceLabel + L".";
        m_statusFlashTick = GetTickCount64() + 3000;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    int scale = 1;
    if (!m_settings || m_settings->upscaleForOcr) {
        if (bmp.width < 320 && bmp.height < 120) scale = 3;
        else if (bmp.width < 1500 && bmp.height < 950) scale = 2;
    }

    RawBitmap scaled = ScreenCapture::CropAndScale(
        bmp, { 0.0f, 0.0f, static_cast<float>(bmp.width), static_cast<float>(bmp.height) }, scale
    );
    std::wstring err;
    OcrDocument doc = OcrService::RecognizeBitmap(scaled, &err);
    if (!doc.empty()) {
        size_t wc = 0;
        bool codeOn = m_settings ? m_settings->codeIndentationMode : true;
        std::wstring formatted = TextFormatter::FormatDocument(doc, false, codeOn, &wc);
        OverlayWindow::CopyTextToClipboard(m_hwnd, formatted);
        AddHistoryEntry(formatted, codeOn, wc, doc.recognitionTimeMs);
        m_statusBanner = L"\x2713 OCR from " + sourceLabel + L" (" + std::to_wstring(wc) +
                         L" words in " + std::to_wstring(static_cast<int>(doc.recognitionTimeMs)) + L" ms) \x2014 copied to clipboard!";
    } else {
        m_statusBanner = err.empty() ? (L"No text detected in " + sourceLabel + L".") : err;
    }
    m_statusFlashTick = GetTickCount64() + 3800;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HistoryWindow::RunOcrFromClipboard() {
    RawBitmap clipBmp = ScreenCapture::LoadBitmapFromClipboard(m_hwnd);
    if (!clipBmp.valid()) {
        m_statusBanner = L"Clipboard does not contain an image. Copy a screenshot or image file first, then press Ctrl+V.";
        m_statusFlashTick = GetTickCount64() + 3200;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    RunOcrOnBitmap(clipBmp, L"Clipboard Image");
}

void HistoryWindow::RunBuiltInDemoCapture() {
    RawBitmap frame = ScreenCapture::CreateSyntheticCodeFrame(800, 390);
    RunOcrOnBitmap(frame, L"Live Demo Code");
}

void HistoryWindow::SaveSelectedCaptureToFile() {
    if (m_history.empty() || m_selectedHistoryIdx < 0 || static_cast<size_t>(m_selectedHistoryIdx) >= m_history.size()) {
        return;
    }

    wchar_t filePath[MAX_PATH] = L"captured_snippet.txt";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"Text / Code Files (*.txt;*.cpp;*.py;*.js;*.md)\0*.txt;*.cpp;*.py;*.js;*.md\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"txt";

    if (GetSaveFileNameW(&ofn)) {
        const std::wstring& wtext = m_history[m_selectedHistoryIdx].text;
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), static_cast<int>(wtext.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(utf8Len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), static_cast<int>(wtext.size()), utf8.data(), utf8Len, nullptr, nullptr);

        std::ofstream out(filePath, std::ios::binary);
        if (out.is_open()) {
            out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
            out.close();
            m_statusBanner = L"\x2713 Saved snippet to file!";
            m_statusFlashTick = GetTickCount64() + 3000;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void HistoryWindow::CreateD2DResources() {
    if (m_renderTarget || !m_hwnd) return;

    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    UINT w = std::max<UINT>(1, rc.right - rc.left);
    UINT h = std::max<UINT>(1, rc.bottom - rc.top);

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = 96.0f;
    rtProps.dpiY = 96.0f;

    HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(
        rtProps,
        D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(w, h)),
        &m_renderTarget
    );

    if (SUCCEEDED(hr) && m_renderTarget) {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &m_brush);
    }
}

void HistoryWindow::DiscardD2DResources() {
    if (m_brush) {
        m_brush->Release();
        m_brush = nullptr;
    }
    if (m_renderTarget) {
        m_renderTarget->Release();
        m_renderTarget = nullptr;
    }
}

void HistoryWindow::BuildUiLayout(float width, float height) {
    (void)height;
    m_hotspots.clear();

    float pad = 24.0f;
    float contentW = std::max(640.0f, width - pad * 2.0f);

    // 1. Four Hero Capture Action Cards (y = 78 .. 174, height = 96)
    float cardGap = 12.0f;
    float cardW = (contentW - cardGap * 3.0f) / 4.0f;
    float cardY = 78.0f;
    float cardH = 96.0f;

    m_hotspots.push_back({
        ACT_HERO_REGION,
        { pad, cardY, cardW, cardH },
        L"Region Selectext",
        L"Freeze video & select words",
        L"Alt + Shift + S",
        false
    });

    m_hotspots.push_back({
        ACT_HERO_FULLSCREEN,
        { pad + cardW + cardGap, cardY, cardW, cardH },
        L"Full-Screen Selectext",
        L"Detect all text on monitor",
        L"Alt + Shift + D",
        false
    });

    m_hotspots.push_back({
        ACT_HERO_QUICKSNIP,
        { pad + (cardW + cardGap) * 2.0f, cardY, cardW, cardH },
        L"Quick Snip & Copy",
        L"Drag box to copy in 1 step",
        L"Alt + Shift + C",
        false
    });

    m_hotspots.push_back({
        ACT_HERO_STITCH,
        { pad + (cardW + cardGap) * 3.0f, cardY, cardW, cardH },
        L"Stitch & Append",
        L"Merge scrolled video code",
        L"Alt + Shift + A",
        false
    });

    // 2. Left Column: Six Settings Toggle Switches (x = pad, width = 345)
    float leftColW = 345.0f;
    float toggleY = 220.0f;
    float toggleH = 48.0f;
    float toggleGap = 7.0f;

    struct ToggleDef {
        int id;
        const wchar_t* title;
        const wchar_t* desc;
    };

    ToggleDef toggles[] = {
        { ACT_TOGGLE_CODE,      L"Smart Code Indentation",      L"Preserves 2/4-space tabs & fixes \x00F8 \x2192 0" },
        { ACT_TOGGLE_LINEMODE,  L"Full-Line Selection Mode",    L"Select entire lines instead of single words" },
        { ACT_TOGGLE_AUTOCOPY,  L"Auto-Copy on Mouse Release",  L"Copies immediately when drag selection ends" },
        { ACT_TOGGLE_UPSCALE,   L"2x Super-Res & Dark Boost",   L"Enhances compressed dark-mode video frames" },
        { ACT_TOGGLE_AUTOCLOSE, L"Auto-Close Overlay on Copy",  L"Closes overlay right after first copy" },
        { ACT_TOGGLE_STARTWIN,  L"Start with Windows (Tray)",   L"Auto-launch silently in tray on login" }
    };

    for (int i = 0; i < 6; ++i) {
        m_hotspots.push_back({
            toggles[i].id,
            { pad, toggleY + i * (toggleH + toggleGap), leftColW, toggleH },
            toggles[i].title,
            toggles[i].desc,
            L"",
            false
        });
    }

    float actionY = toggleY + 6 * (toggleH + toggleGap) + 6.0f;
    m_hotspots.push_back({
        ACT_BTN_PASTE_IMG,
        { pad, actionY, 178.0f, 38.0f },
        L"Paste Image (Ctrl+V)",
        L"",
        L"",
        false
    });
    m_hotspots.push_back({
        ACT_BTN_DEMO_OCR,
        { pad + 186.0f, actionY, leftColW - 186.0f, 38.0f },
        L"\x26A1 Live Self-Test",
        L"",
        L"",
        false
    });

    // 3. Right Column: Capture History & Code Inspector
    float rightX = pad + leftColW + 20.0f;
    float rightW = std::max(320.0f, width - rightX - pad);

    float btnY = 186.0f;
    m_hotspots.push_back({
        ACT_BTN_COPY_CODE,
        { rightX + rightW - 266.0f, btnY, 118.0f, 28.0f },
        L"Copy Snippet",
        L"", L"", false
    });
    m_hotspots.push_back({
        ACT_BTN_SAVE_FILE,
        { rightX + rightW - 140.0f, btnY, 72.0f, 28.0f },
        L"Save...",
        L"", L"", false
    });
    m_hotspots.push_back({
        ACT_BTN_CLEAR_HIST,
        { rightX + rightW - 60.0f, btnY, 60.0f, 28.0f },
        L"Clear",
        L"", L"", false
    });

    // Recent capture history pills row
    float pillY = 220.0f;
    int maxPills = std::min<int>(4, static_cast<int>(m_history.size()));
    if (maxPills > 0) {
        float pillGap = 8.0f;
        float pillW = (rightW - pillGap * (maxPills - 1)) / static_cast<float>(maxPills);
        for (int i = 0; i < maxPills; ++i) {
            const auto& h = m_history[i];
            std::wstring label = h.timestamp + L" (" + std::to_wstring(h.wordCount) + L"w)";
            m_hotspots.push_back({
                ACT_HIST_ITEM_BASE + i,
                { rightX + i * (pillW + pillGap), pillY, pillW, 30.0f },
                label,
                h.wasCodeMode ? L"CODE" : L"TEXT",
                L"",
                false
            });
        }
    }

    for (auto& hs : m_hotspots) {
        hs.hovered = hs.bounds.contains(m_mouseX, m_mouseY);
    }
}

void HistoryWindow::OnPaint() {
    CreateD2DResources();
    if (!m_renderTarget || !m_brush) return;

    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    float width = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
    float height = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));

    BuildUiLayout(width, height);

    m_renderTarget->BeginDraw();

    // Deep slate-navy background (#0B0F19)
    m_renderTarget->Clear(D2D1::ColorF(0.043f, 0.059f, 0.098f, 1.0f));

    float pad = 24.0f;

    // 1. Header Bar
    D2D1_RECT_F logoBox = D2D1::RectF(pad, 16.0f, pad + 38.0f, 54.0f);
    m_brush->SetColor(D2D1::ColorF(0.08f, 0.45f, 0.92f, 0.25f));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(logoBox, 8.0f, 8.0f), m_brush);
    m_brush->SetColor(D2D1::ColorF(0.22f, 0.74f, 0.97f, 0.95f));
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(logoBox, 8.0f, 8.0f), m_brush, 1.8f);
    if (m_badgeFormat) {
        m_renderTarget->DrawTextW(L"T", 1, m_badgeFormat, logoBox, m_brush);
    }

    if (m_titleFormat) {
        m_brush->SetColor(D2D1::ColorF(0.96f, 0.98f, 1.0f, 1.0f));
        D2D1_RECT_F titleRect = D2D1::RectF(pad + 50.0f, 12.0f, width - 310.0f, 38.0f);
        m_renderTarget->DrawTextW(L"ScreenTextify Studio", 20, m_titleFormat, titleRect, m_brush);
    }
    if (m_subtitleFormat) {
        m_brush->SetColor(D2D1::ColorF(0.58f, 0.65f, 0.76f, 1.0f));
        std::wstring sub = L"Selectext for Desktop Videos, Live Streams & Images  \x2022  100% Free & Offline";
        D2D1_RECT_F subRect = D2D1::RectF(pad + 50.0f, 38.0f, width - 310.0f, 58.0f);
        m_renderTarget->DrawTextW(sub.c_str(), static_cast<UINT32>(sub.length()), m_subtitleFormat, subRect, m_brush);
    }

    // Engine Status Pill on top right
    std::wstring engText = L"\x25CF Neural OCR Ready (" + m_ocrLangName + L")";
    float pillW = 280.0f;
    D2D1_RECT_F engPill = D2D1::RectF(width - pad - pillW, 20.0f, width - pad, 50.0f);
    m_brush->SetColor(D2D1::ColorF(0.06f, 0.18f, 0.16f, 0.85f));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(engPill, 15.0f, 15.0f), m_brush);
    m_brush->SetColor(D2D1::ColorF(0.16f, 0.82f, 0.58f, 0.75f));
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(engPill, 15.0f, 15.0f), m_brush, 1.0f);
    if (m_badgeFormat) {
        m_brush->SetColor(D2D1::ColorF(0.32f, 0.95f, 0.72f, 1.0f));
        m_renderTarget->DrawTextW(engText.c_str(), static_cast<UINT32>(engText.length()), m_badgeFormat, engPill, m_brush);
    }

    // Section Headers
    if (m_cardTitleFormat) {
        m_brush->SetColor(D2D1::ColorF(0.84f, 0.90f, 0.98f, 1.0f));
        D2D1_RECT_F sLbl = D2D1::RectF(pad, 190.0f, pad + 340.0f, 214.0f);
        m_renderTarget->DrawTextW(L"Selection & OCR Intelligence", 28, m_cardTitleFormat, sLbl, m_brush);
    }

    float rightX = pad + 365.0f;
    float rightW = std::max(320.0f, width - rightX - pad);

    if (m_cardTitleFormat) {
        m_brush->SetColor(D2D1::ColorF(0.84f, 0.90f, 0.98f, 1.0f));
        D2D1_RECT_F rLbl = D2D1::RectF(rightX, 190.0f, rightX + rightW - 275.0f, 214.0f);
        m_renderTarget->DrawTextW(L"Captured Code & Text Inspector", 30, m_cardTitleFormat, rLbl, m_brush);
    }

    // 2. Render Hotspots (Hero Cards, Toggle Switches, Buttons, History Tabs)
    for (const auto& hs : m_hotspots) {
        D2D1_RECT_F r = D2D1::RectF(hs.bounds.x, hs.bounds.y, hs.bounds.right(), hs.bounds.bottom());

        if (hs.id == ACT_HERO_REGION || hs.id == ACT_HERO_FULLSCREEN || hs.id == ACT_HERO_QUICKSNIP || hs.id == ACT_HERO_STITCH) {
            bool isPrimary = (hs.id == ACT_HERO_REGION);
            bool isStitch = (hs.id == ACT_HERO_STITCH);
            if (hs.hovered) {
                m_brush->SetColor(isPrimary ? D2D1::ColorF(0.10f, 0.24f, 0.46f, 0.95f)
                    : (isStitch ? D2D1::ColorF(0.08f, 0.24f, 0.22f, 0.95f) : D2D1::ColorF(0.11f, 0.16f, 0.26f, 0.95f)));
            } else {
                m_brush->SetColor(isPrimary ? D2D1::ColorF(0.08f, 0.18f, 0.36f, 0.90f)
                    : (isStitch ? D2D1::ColorF(0.06f, 0.16f, 0.16f, 0.92f) : D2D1::ColorF(0.075f, 0.105f, 0.175f, 0.92f)));
            }
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 10.0f, 10.0f), m_brush);

            m_brush->SetColor(hs.hovered
                ? (isStitch ? D2D1::ColorF(0.24f, 0.90f, 0.72f, 0.95f) : D2D1::ColorF(0.32f, 0.78f, 1.0f, 0.95f))
                : (isPrimary ? D2D1::ColorF(0.20f, 0.60f, 0.95f, 0.75f)
                    : (isStitch ? D2D1::ColorF(0.16f, 0.68f, 0.54f, 0.70f) : D2D1::ColorF(0.16f, 0.22f, 0.34f, 0.85f))));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 10.0f, 10.0f), m_brush, (isPrimary || isStitch) ? 1.5f : 1.2f);

            // Clip strictly inside the card so nothing can ever overflow
            m_renderTarget->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            // Row 1 (Top): Hotkey badge pill on the left
            D2D1_RECT_F kbRect = D2D1::RectF(hs.bounds.x + 14.0f, hs.bounds.y + 11.0f, hs.bounds.x + 124.0f, hs.bounds.y + 33.0f);
            m_brush->SetColor(D2D1::ColorF(0.045f, 0.075f, 0.135f, 0.95f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(kbRect, 5.0f, 5.0f), m_brush);
            m_brush->SetColor(isStitch ? D2D1::ColorF(0.18f, 0.78f, 0.62f, 0.55f) : D2D1::ColorF(0.22f, 0.62f, 0.95f, 0.50f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(kbRect, 5.0f, 5.0f), m_brush, 1.0f);
            m_brush->SetColor(isStitch ? D2D1::ColorF(0.34f, 0.95f, 0.75f, 1.0f) : D2D1::ColorF(0.42f, 0.85f, 1.0f, 1.0f));
            if (m_badgeFormat) {
                m_renderTarget->DrawTextW(hs.badge.c_str(), static_cast<UINT32>(hs.badge.length()), m_badgeFormat, kbRect, m_brush);
            }

            // Row 2 (Middle): Full-width single-line Title
            m_brush->SetColor(D2D1::ColorF(0.96f, 0.98f, 1.0f, 1.0f));
            if (m_cardTitleFormat) {
                D2D1_RECT_F tr = D2D1::RectF(hs.bounds.x + 14.0f, hs.bounds.y + 40.0f, hs.bounds.right() - 14.0f, hs.bounds.y + 62.0f);
                m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_cardTitleFormat, tr, m_brush);
            }

            // Row 3 (Bottom): Full-width single-line Subtitle
            m_brush->SetColor(D2D1::ColorF(0.64f, 0.72f, 0.84f, 1.0f));
            if (m_bodyFormat) {
                D2D1_RECT_F sr = D2D1::RectF(hs.bounds.x + 14.0f, hs.bounds.y + 66.0f, hs.bounds.right() - 14.0f, hs.bounds.bottom() - 8.0f);
                m_renderTarget->DrawTextW(hs.subtitle.c_str(), static_cast<UINT32>(hs.subtitle.length()), m_bodyFormat, sr, m_brush);
            }

            m_renderTarget->PopAxisAlignedClip();
        } else if (hs.id >= ACT_TOGGLE_CODE && hs.id <= ACT_TOGGLE_STARTWIN) {
            bool isOn = false;
            if (m_settings) {
                if (hs.id == ACT_TOGGLE_CODE) isOn = m_settings->codeIndentationMode;
                else if (hs.id == ACT_TOGGLE_LINEMODE) isOn = m_settings->lineSelectionMode;
                else if (hs.id == ACT_TOGGLE_AUTOCOPY) isOn = m_settings->autoCopyOnRelease;
                else if (hs.id == ACT_TOGGLE_UPSCALE) isOn = m_settings->upscaleForOcr;
                else if (hs.id == ACT_TOGGLE_AUTOCLOSE) isOn = m_settings->autoCloseAfterCopy;
                else if (hs.id == ACT_TOGGLE_STARTWIN) isOn = m_settings->startWithWindows;
            }

            m_brush->SetColor(hs.hovered ? D2D1::ColorF(0.10f, 0.14f, 0.22f, 0.95f) : D2D1::ColorF(0.07f, 0.10f, 0.16f, 0.90f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 8.0f, 8.0f), m_brush);
            m_brush->SetColor(D2D1::ColorF(0.15f, 0.21f, 0.32f, 0.85f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 8.0f, 8.0f), m_brush, 1.0f);

            m_renderTarget->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            m_brush->SetColor(D2D1::ColorF(0.94f, 0.97f, 1.0f, 1.0f));
            if (m_cardTitleFormat) {
                D2D1_RECT_F tr = D2D1::RectF(hs.bounds.x + 14.0f, hs.bounds.y + 6.0f, hs.bounds.right() - 68.0f, hs.bounds.y + 25.0f);
                m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_cardTitleFormat, tr, m_brush);
            }
            m_brush->SetColor(D2D1::ColorF(0.56f, 0.64f, 0.76f, 1.0f));
            if (m_bodyFormat) {
                D2D1_RECT_F dr = D2D1::RectF(hs.bounds.x + 14.0f, hs.bounds.y + 26.0f, hs.bounds.right() - 68.0f, hs.bounds.bottom() - 4.0f);
                m_renderTarget->DrawTextW(hs.subtitle.c_str(), static_cast<UINT32>(hs.subtitle.length()), m_bodyFormat, dr, m_brush);
            }

            float swW = 42.0f;
            float swH = 22.0f;
            float swX = hs.bounds.right() - swW - 14.0f;
            float swY = hs.bounds.centerY() - swH * 0.5f;
            D2D1_RECT_F swRect = D2D1::RectF(swX, swY, swX + swW, swY + swH);
            m_brush->SetColor(isOn ? D2D1::ColorF(0.14f, 0.56f, 0.96f, 1.0f) : D2D1::ColorF(0.18f, 0.24f, 0.34f, 1.0f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(swRect, 11.0f, 11.0f), m_brush);

            float knobX = isOn ? (swX + swW - 11.0f) : (swX + 11.0f);
            float knobY = swY + 11.0f;
            m_brush->SetColor(D2D1::ColorF(0.98f, 0.99f, 1.0f, 1.0f));
            m_renderTarget->FillEllipse(D2D1::Ellipse({ knobX, knobY }, 7.8f, 7.8f), m_brush);

            m_renderTarget->PopAxisAlignedClip();
        } else if (hs.id == ACT_BTN_DEMO_OCR || hs.id == ACT_BTN_PASTE_IMG || hs.id == ACT_BTN_COPY_CODE || hs.id == ACT_BTN_SAVE_FILE || hs.id == ACT_BTN_CLEAR_HIST) {
            bool accent = (hs.id == ACT_BTN_COPY_CODE || hs.id == ACT_BTN_PASTE_IMG);
            if (hs.hovered) {
                m_brush->SetColor(accent ? D2D1::ColorF(0.16f, 0.52f, 0.96f, 0.95f) : D2D1::ColorF(0.18f, 0.24f, 0.35f, 0.95f));
            } else {
                m_brush->SetColor(accent ? D2D1::ColorF(0.11f, 0.40f, 0.82f, 0.90f) : D2D1::ColorF(0.11f, 0.15f, 0.24f, 0.90f));
            }
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), m_brush);
            m_brush->SetColor(accent ? D2D1::ColorF(0.38f, 0.76f, 1.0f, 0.90f) : D2D1::ColorF(0.24f, 0.32f, 0.46f, 0.80f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), m_brush, 1.0f);

            m_brush->SetColor(D2D1::ColorF(0.96f, 0.98f, 1.0f, 1.0f));
            if (m_badgeFormat) {
                m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_badgeFormat, r, m_brush);
            }
        } else if (hs.id >= ACT_HIST_ITEM_BASE) {
            int idx = hs.id - ACT_HIST_ITEM_BASE;
            bool selected = (idx == m_selectedHistoryIdx);
            m_brush->SetColor(selected
                ? D2D1::ColorF(0.12f, 0.34f, 0.68f, 0.92f)
                : (hs.hovered ? D2D1::ColorF(0.12f, 0.17f, 0.27f, 0.95f) : D2D1::ColorF(0.075f, 0.105f, 0.17f, 0.90f)));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), m_brush);
            m_brush->SetColor(selected ? D2D1::ColorF(0.38f, 0.78f, 1.0f, 0.95f) : D2D1::ColorF(0.18f, 0.24f, 0.36f, 0.80f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), m_brush, 1.0f);

            m_brush->SetColor(D2D1::ColorF(0.94f, 0.97f, 1.0f, 1.0f));
            if (m_badgeFormat) {
                m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_badgeFormat, r, m_brush);
            }
        }
    }

    // 3. Draw Dark Code Inspector Panel on the Right (with strict clipping & scrollable line gutter!)
    float codeTop = 258.0f;
    float codeBot = std::max(codeTop + 180.0f, height - 54.0f);
    m_codeViewportRect = { rightX, codeTop, rightW, codeBot - codeTop };

    D2D1_RECT_F codePanel = D2D1::RectF(rightX, codeTop, rightX + rightW, codeBot);
    m_brush->SetColor(D2D1::ColorF(0.055f, 0.075f, 0.125f, 0.98f));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(codePanel, 10.0f, 10.0f), m_brush);

    // Header Bar inside Code Panel
    float headerH = 32.0f;
    D2D1_RECT_F codeHeader = D2D1::RectF(rightX, codeTop, rightX + rightW, codeTop + headerH);
    m_brush->SetColor(D2D1::ColorF(0.08f, 0.11f, 0.18f, 0.98f));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(codeHeader, 10.0f, 10.0f), m_brush);
    // Square off bottom of header
    m_renderTarget->FillRectangle(D2D1::RectF(rightX, codeTop + headerH - 8.0f, rightX + rightW, codeTop + headerH), m_brush);

    // Divider line under code header
    m_brush->SetColor(D2D1::ColorF(0.16f, 0.23f, 0.36f, 0.90f));
    m_renderTarget->DrawLine({ rightX, codeTop + headerH }, { rightX + rightW, codeTop + headerH }, m_brush, 1.0f);

    if (!m_history.empty() && m_selectedHistoryIdx >= 0 && static_cast<size_t>(m_selectedHistoryIdx) < m_history.size()) {
        const auto& item = m_history[m_selectedHistoryIdx];

        // Split lines for rendering
        std::vector<std::wstring> lines;
        {
            std::wstringstream ss(item.text);
            std::wstring l;
            while (std::getline(ss, l)) {
                if (!l.empty() && l.back() == L'\r') l.pop_back();
                lines.push_back(std::move(l));
            }
        }
        if (lines.empty()) lines.push_back(L"");

        std::wstringstream hdr;
        hdr << (item.wasCodeMode ? L"[CODE INDENTATION PRESERVED]" : L"[STANDARD TEXT]")
            << L"   \x2022   " << item.wordCount << L" words (" << lines.size() << L" lines)"
            << L"   \x2022   OCR Speed: " << std::fixed << std::setprecision(1) << item.ocrTimeMs << L" ms";
        std::wstring hdrStr = hdr.str();
        m_brush->SetColor(D2D1::ColorF(0.45f, 0.82f, 0.98f, 1.0f));
        if (m_bodyFormat) {
            D2D1_RECT_F hTextRect = D2D1::RectF(rightX + 14.0f, codeTop + 7.0f, rightX + rightW - 14.0f, codeTop + 27.0f);
            m_renderTarget->DrawTextW(hdrStr.c_str(), static_cast<UINT32>(hdrStr.length()), m_bodyFormat, hTextRect, m_brush);
        }

        // Strict clip for the scrollable code body area
        float bodyTop = codeTop + headerH + 1.0f;
        float bodyBot = codeBot - 2.0f;
        float bodyH = std::max(10.0f, bodyBot - bodyTop);
        D2D1_RECT_F bodyClip = D2D1::RectF(rightX + 2.0f, bodyTop, rightX + rightW - 2.0f, bodyBot);

        m_renderTarget->PushAxisAlignedClip(bodyClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // Gutter background (left 44px)
        float gutterW = 44.0f;
        m_brush->SetColor(D2D1::ColorF(0.045f, 0.062f, 0.105f, 1.0f));
        m_renderTarget->FillRectangle(D2D1::RectF(rightX, bodyTop, rightX + gutterW, bodyBot), m_brush);
        m_brush->SetColor(D2D1::ColorF(0.14f, 0.20f, 0.30f, 0.85f));
        m_renderTarget->DrawLine({ rightX + gutterW, bodyTop }, { rightX + gutterW, bodyBot }, m_brush, 1.0f);

        float lineH = 21.0f;
        float totalContentH = static_cast<float>(lines.size()) * lineH + 16.0f;
        m_maxCodeScrollY = std::max(0.0f, totalContentH - bodyH);
        m_codeScrollY = std::clamp(m_codeScrollY, 0.0f, m_maxCodeScrollY);

        for (size_t i = 0; i < lines.size(); ++i) {
            float ly = bodyTop + 8.0f + static_cast<float>(i) * lineH - m_codeScrollY;
            if (ly + lineH < bodyTop) continue;
            if (ly > bodyBot) break;

            // Draw right-aligned line number in gutter
            std::wstring numStr = std::to_wstring(i + 1);
            if (m_lineNumFormat) {
                m_brush->SetColor(D2D1::ColorF(0.42f, 0.50f, 0.65f, 0.90f));
                D2D1_RECT_F numRect = D2D1::RectF(rightX + 4.0f, ly, rightX + gutterW - 8.0f, ly + lineH);
                m_renderTarget->DrawTextW(numStr.c_str(), static_cast<UINT32>(numStr.length()), m_lineNumFormat, numRect, m_brush);
            }

            // Draw code/text line without wrapping
            if (m_monoFormat && !lines[i].empty()) {
                m_brush->SetColor(D2D1::ColorF(0.90f, 0.94f, 1.0f, 1.0f));
                D2D1_RECT_F lineRect = D2D1::RectF(rightX + gutterW + 12.0f, ly, rightX + rightW - 14.0f, ly + lineH);
                m_renderTarget->DrawTextW(lines[i].c_str(), static_cast<UINT32>(lines[i].length()), m_monoFormat, lineRect, m_brush);
            }
        }

        // Draw sleek vertical scrollbar if content exceeds viewport
        if (m_maxCodeScrollY > 0.5f) {
            float trackTop = bodyTop + 4.0f;
            float trackBot = bodyBot - 4.0f;
            float trackH = std::max(20.0f, trackBot - trackTop);
            float thumbH = std::max(24.0f, trackH * (bodyH / totalContentH));
            float scrollRatio = m_codeScrollY / m_maxCodeScrollY;
            float thumbY = trackTop + scrollRatio * (trackH - thumbH);

            D2D1_RECT_F thumbRect = D2D1::RectF(rightX + rightW - 8.0f, thumbY, rightX + rightW - 3.0f, thumbY + thumbH);
            m_brush->SetColor(D2D1::ColorF(0.28f, 0.38f, 0.54f, 0.80f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(thumbRect, 2.5f, 2.5f), m_brush);
        }

        m_renderTarget->PopAxisAlignedClip();
    } else {
        std::wstring emptyMsg = L"No captures yet. Press Alt+Shift+S or click a capture card above.";
        if (m_bodyFormat) {
            D2D1_RECT_F textArea = D2D1::RectF(rightX + 20.0f, codeTop + 50.0f, rightX + rightW - 20.0f, codeBot - 20.0f);
            m_brush->SetColor(D2D1::ColorF(0.55f, 0.62f, 0.74f, 1.0f));
            m_renderTarget->DrawTextW(emptyMsg.c_str(), static_cast<UINT32>(emptyMsg.length()), m_bodyFormat, textArea, m_brush);
        }
    }

    // Outer border of Code Panel drawn on top for crisp edges
    m_brush->SetColor(D2D1::ColorF(0.16f, 0.23f, 0.36f, 0.90f));
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(codePanel, 10.0f, 10.0f), m_brush, 1.2f);

    // 4. Footer Status Bar inside a clean bottom strip
    D2D1_RECT_F footBar = D2D1::RectF(pad, height - 38.0f, width - pad, height - 10.0f);
    m_brush->SetColor(D2D1::ColorF(0.06f, 0.085f, 0.14f, 0.90f));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(footBar, 6.0f, 6.0f), m_brush);

    std::wstring footer = (m_statusFlashTick > GetTickCount64() && !m_statusBanner.empty())
        ? m_statusBanner
        : L"Tip: Drop any image file here or press Ctrl+V to OCR it. Overlay keys: [A] Stitch Append, [C] Code Indent, [L] Line Mode, [R] Re-Crop.";
    if (m_bodyFormat) {
        m_brush->SetColor((m_statusFlashTick > GetTickCount64())
            ? D2D1::ColorF(0.28f, 0.95f, 0.68f, 1.0f)
            : D2D1::ColorF(0.56f, 0.65f, 0.78f, 1.0f));
        D2D1_RECT_F footTextRect = D2D1::RectF(pad + 12.0f, height - 32.0f, width - pad - 12.0f, height - 12.0f);
        m_renderTarget->DrawTextW(footer.c_str(), static_cast<UINT32>(footer.length()), m_bodyFormat, footTextRect, m_brush);
    }

    HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardD2DResources();
    }
}

LRESULT CALLBACK HistoryWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HistoryWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<HistoryWindow*>(cs->lpCreateParams);
        if (self) {
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    } else {
        self = reinterpret_cast<HistoryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT HistoryWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE: {
        if (m_renderTarget) {
            UINT w = LOWORD(lParam);
            UINT h = HIWORD(lParam);
            m_renderTarget->Resize(D2D1::SizeU(std::max<UINT>(1, w), std::max<UINT>(1, h)));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 920;
        mmi->ptMinTrackSize.y = 656;
        return 0;
    }

    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        if (hDrop) {
            wchar_t pathBuf[MAX_PATH * 2] = {};
            if (DragQueryFileW(hDrop, 0, pathBuf, static_cast<UINT>(std::size(pathBuf))) > 0) {
                std::wstring filePath = pathBuf;
                RawBitmap bmp = ScreenCapture::LoadBitmapFromFile(filePath);
                size_t slash = filePath.find_last_of(L"\\/");
                std::wstring fileName = (slash != std::wstring::npos) ? filePath.substr(slash + 1) : filePath;
                RunOcrOnBitmap(bmp, fileName);
            }
            DragFinish(hDrop);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrlDown && wParam == 'V') {
            RunOcrFromClipboard();
            return 0;
        }
        if (ctrlDown && wParam == 'C') {
            if (!m_history.empty() && m_selectedHistoryIdx >= 0 && static_cast<size_t>(m_selectedHistoryIdx) < m_history.size()) {
                OverlayWindow::CopyTextToClipboard(hwnd, m_history[m_selectedHistoryIdx].text);
                m_statusBanner = L"\x2713 Copied selected snippet to Clipboard!";
                m_statusFlashTick = GetTickCount64() + 2800;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        OnPaint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float step = 42.0f;
        m_codeScrollY = std::clamp(m_codeScrollY - (delta > 0 ? step : -step), 0.0f, m_maxCodeScrollY);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETCURSOR: {
        bool overHotspot = false;
        for (const auto& hs : m_hotspots) {
            if (hs.hovered) {
                overHotspot = true;
                break;
            }
        }
        SetCursor(LoadCursor(nullptr, overHotspot ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    case WM_MOUSEMOVE: {
        m_mouseX = static_cast<float>(GET_X_LPARAM(lParam));
        m_mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
        bool changed = false;
        for (auto& hs : m_hotspots) {
            bool h = hs.bounds.contains(m_mouseX, m_mouseY);
            if (hs.hovered != h) {
                hs.hovered = h;
                changed = true;
            }
        }
        if (changed) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));

        for (const auto& hs : m_hotspots) {
            if (hs.bounds.contains(mx, my)) {
                if (hs.id == ACT_HERO_REGION) {
                    ShowWindow(hwnd, SW_HIDE);
                    Sleep(90);
                    if (m_onStartCapture) m_onStartCapture(CaptureLaunchMode::RegionInteractive);
                } else if (hs.id == ACT_HERO_FULLSCREEN) {
                    ShowWindow(hwnd, SW_HIDE);
                    Sleep(90);
                    if (m_onStartCapture) m_onStartCapture(CaptureLaunchMode::FullScreenSelect);
                } else if (hs.id == ACT_HERO_QUICKSNIP) {
                    ShowWindow(hwnd, SW_HIDE);
                    Sleep(90);
                    if (m_onStartCapture) m_onStartCapture(CaptureLaunchMode::QuickSnipCopy);
                } else if (hs.id == ACT_HERO_STITCH) {
                    ShowWindow(hwnd, SW_HIDE);
                    Sleep(90);
                    if (m_onStartCapture) m_onStartCapture(CaptureLaunchMode::StitchAppend);
                } else if (hs.id == ACT_TOGGLE_CODE && m_settings) {
                    m_settings->codeIndentationMode = !m_settings->codeIndentationMode;
                    AppStorage::SaveSettings(*m_settings);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (hs.id == ACT_TOGGLE_LINEMODE && m_settings) {
                    m_settings->lineSelectionMode = !m_settings->lineSelectionMode;
                    AppStorage::SaveSettings(*m_settings);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (hs.id == ACT_TOGGLE_AUTOCOPY && m_settings) {
                    m_settings->autoCopyOnRelease = !m_settings->autoCopyOnRelease;
                    AppStorage::SaveSettings(*m_settings);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (hs.id == ACT_TOGGLE_UPSCALE && m_settings) {
                    m_settings->upscaleForOcr = !m_settings->upscaleForOcr;
                    AppStorage::SaveSettings(*m_settings);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (hs.id == ACT_TOGGLE_AUTOCLOSE && m_settings) {
                    m_settings->autoCloseAfterCopy = !m_settings->autoCloseAfterCopy;
                    AppStorage::SaveSettings(*m_settings);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (hs.id == ACT_TOGGLE_STARTWIN && m_settings) {
                    bool target = !m_settings->startWithWindows;
                    if (AppStorage::SetStartWithWindows(target)) {
                        m_settings->startWithWindows = target;
                        AppStorage::SaveSettings(*m_settings);
                        m_statusBanner = target
                            ? L"\x2713 Enabled Start with Windows (launches silently in System Tray)."
                            : L"Disabled Start with Windows.";
                    } else {
                        m_statusBanner = L"Could not update Windows startup registry key.";
                    }
                    m_statusFlashTick = GetTickCount64() + 3000;
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (hs.id == ACT_BTN_PASTE_IMG) {
                    RunOcrFromClipboard();
                } else if (hs.id == ACT_BTN_DEMO_OCR) {
                    RunBuiltInDemoCapture();
                } else if (hs.id == ACT_BTN_COPY_CODE) {
                    if (!m_history.empty() && m_selectedHistoryIdx >= 0 && static_cast<size_t>(m_selectedHistoryIdx) < m_history.size()) {
                        OverlayWindow::CopyTextToClipboard(hwnd, m_history[m_selectedHistoryIdx].text);
                        m_statusBanner = L"\x2713 Copied selected snippet to Clipboard!";
                        m_statusFlashTick = GetTickCount64() + 2800;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                } else if (hs.id == ACT_BTN_SAVE_FILE) {
                    SaveSelectedCaptureToFile();
                } else if (hs.id == ACT_BTN_CLEAR_HIST) {
                    m_history.clear();
                    m_selectedHistoryIdx = 0;
                    m_codeScrollY = 0.0f;
                    AppStorage::SaveHistory(m_history);
                    m_statusBanner = L"Cleared capture history.";
                    m_statusFlashTick = GetTickCount64() + 2500;
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (hs.id >= ACT_HIST_ITEM_BASE) {
                    m_selectedHistoryIdx = hs.id - ACT_HIST_ITEM_BASE;
                    m_codeScrollY = 0.0f;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace ScreenTextify
