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
constexpr UINT_PTR TIMER_ANIM_ID = 2001;

enum ActionId {
    ACT_HERO_REGION     = 101,
    ACT_HERO_FULLSCREEN = 102,
    ACT_HERO_QUICKSNIP  = 103,
    ACT_HERO_STITCH     = 104,

    ACT_TAB_HISTORY     = 150,
    ACT_TAB_SETTINGS    = 151,

    ACT_TOGGLE_CODE      = 201,
    ACT_TOGGLE_LINEMODE  = 202,
    ACT_TOGGLE_AUTOCOPY  = 203,
    ACT_TOGGLE_UPSCALE   = 204,
    ACT_TOGGLE_AUTOCLOSE = 205,
    ACT_TOGGLE_STARTWIN  = 206,
    ACT_TOGGLE_GUTTER    = 207,
    ACT_TOGGLE_HEATMAP   = 208,

    ACT_BTN_COPY_CODE   = 301,
    ACT_BTN_SAVE_FILE   = 302,
    ACT_BTN_DEMO_OCR    = 303,
    ACT_BTN_CLEAR_HIST  = 304,
    ACT_BTN_PASTE_IMG   = 305,

    ACT_HIST_ITEM_BASE  = 1000
};

void ConfigureSingleLine(IDWriteFactory* factory, IDWriteTextFormat* fmt, bool addEllipsis = true) {
    if (!fmt) return;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (addEllipsis && factory) {
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(fmt, &ellipsis)) && ellipsis) {
            DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            fmt->SetTrimming(&trimming, ellipsis.Get());
        }
    }
}

std::wstring ExtractFirstNonEmptyPreview(const std::wstring& text) {
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t s = 0;
        while (s < line.size() && (line[s] == L' ' || line[s] == L'\t')) ++s;
        if (s < line.size()) {
            return line.substr(s);
        }
    }
    return L"(empty snippet)";
}

} // namespace

HistoryWindow::HistoryWindow(HINSTANCE hInstance, AppSettings* settings, StartCaptureCallback onStartCapture)
    : m_hInstance(hInstance), m_settings(settings), m_onStartCapture(std::move(onStartCapture)) {}

HistoryWindow::~HistoryWindow() {
    if (m_hwnd && m_animTimerActive) {
        KillTimer(m_hwnd, TIMER_ANIM_ID);
    }
    DiscardD2DResources();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

bool HistoryWindow::Initialize() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())
    );
    if (FAILED(hr)) return false;

    // Typography scale (Segoe UI Variable / Segoe UI + Cascadia Code / Consolas)
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"en-us", &m_brandFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", &m_headingFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_cardTitleFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &m_bodyFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.5f, L"en-us", &m_smallFormat);
    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &m_badgeFormat);
    m_dwriteFactory->CreateTextFormat(L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_monoFormat);
    m_dwriteFactory->CreateTextFormat(L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &m_monoSmallFormat);
    m_dwriteFactory->CreateTextFormat(L"Cascadia Mono", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_lineNumFormat);

    ConfigureSingleLine(m_dwriteFactory.Get(), m_brandFormat.Get(), true);
    ConfigureSingleLine(m_dwriteFactory.Get(), m_headingFormat.Get(), true);
    ConfigureSingleLine(m_dwriteFactory.Get(), m_cardTitleFormat.Get(), true);
    ConfigureSingleLine(m_dwriteFactory.Get(), m_bodyFormat.Get(), true);
    ConfigureSingleLine(m_dwriteFactory.Get(), m_smallFormat.Get(), true);
    ConfigureSingleLine(m_dwriteFactory.Get(), m_badgeFormat.Get(), true);
    ConfigureSingleLine(m_dwriteFactory.Get(), m_monoFormat.Get(), false);
    ConfigureSingleLine(m_dwriteFactory.Get(), m_monoSmallFormat.Get(), true);
    ConfigureSingleLine(m_dwriteFactory.Get(), m_lineNumFormat.Get(), false);

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

    int winW = 1140;
    int winH = 730;
    int sx = std::max(20, (GetSystemMetrics(SM_CXSCREEN) - winW) / 2);
    int sy = std::max(20, (GetSystemMetrics(SM_CYSCREEN) - winH) / 2);

    m_hwnd = CreateWindowExW(
        WS_EX_ACCEPTFILES,
        HISTORY_CLASS_NAME,
        L"ScreenTextify Studio",
        WS_OVERLAPPEDWINDOW,
        sx, sy, winW, winH,
        nullptr, nullptr, m_hInstance, this
    );
    if (!m_hwnd) return false;

    DragAcceptFiles(m_hwnd, TRUE);
    UpdateDpi(GetDpiForWindow(m_hwnd));

    // Windows 11 seamless dark frame & matched caption color (#0A0D14)
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
    DWORD cornerPref = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
    COLORREF borderCol = RGB(32, 42, 62);
    DwmSetWindowAttribute(m_hwnd, 34 /* DWMWA_BORDER_COLOR */, &borderCol, sizeof(borderCol));
    COLORREF captionCol = RGB(10, 13, 20);
    DwmSetWindowAttribute(m_hwnd, 35 /* DWMWA_CAPTION_COLOR */, &captionCol, sizeof(captionCol));
    COLORREF titleTextCol = RGB(241, 245, 252);
    DwmSetWindowAttribute(m_hwnd, 36 /* DWMWA_TEXT_COLOR */, &titleTextCol, sizeof(titleTextCol));

    // Sync initial toggle animation positions
    if (m_settings) {
        m_toggleAnim[0] = m_settings->codeIndentationMode ? 1.0f : 0.0f;
        m_toggleAnim[1] = m_settings->lineSelectionMode ? 1.0f : 0.0f;
        m_toggleAnim[2] = m_settings->autoCopyOnRelease ? 1.0f : 0.0f;
        m_toggleAnim[3] = m_settings->upscaleForOcr ? 1.0f : 0.0f;
        m_toggleAnim[4] = m_settings->autoCloseAfterCopy ? 1.0f : 0.0f;
        m_toggleAnim[5] = m_settings->startWithWindows ? 1.0f : 0.0f;
        m_toggleAnim[6] = m_settings->stripLineNumbers ? 1.0f : 0.0f;
        m_toggleAnim[7] = m_settings->showConfidenceHeatmap ? 1.0f : 0.0f;
    }

    if (!AppStorage::LoadHistory(m_history) || m_history.empty()) {
        CaptureHistoryItem welcome;
        welcome.text =
            L"// Welcome to ScreenTextify Studio v3.0\r\n"
            L"// Alt+Shift+S : Region Selectext   | Alt+Shift+A : Stitch & Append\r\n"
            L"// Ctrl+V      : Paste Image OCR    | Drag & Drop : Any PNG / JPG\r\n"
            L"\r\n"
            L"async function fetchUserProfile(userId) {\r\n"
            L"  const response = await fetch(`/api/users/${userId}`);\r\n"
            L"  if (!response.ok) {\r\n"
            L"    throw new Error(\"Request failed: \" + response.status);\r\n"
            L"  }\r\n"
            L"  return await response.json();\r\n"
            L"}";
        welcome.timestamp = L"Welcome";
        welcome.wasCodeMode = true;
        welcome.wordCount = 41;
        welcome.ocrTimeMs = 0.0;
        m_history.push_back(std::move(welcome));
    }

    RebuildSelectedCodeCache();
    return true;
}

void HistoryWindow::UpdateDpi(UINT newDpi) {
    m_dpi = (newDpi >= 96) ? static_cast<float>(newDpi) : 96.0f;
    m_dpiScale = m_dpi / 96.0f;
    if (m_renderTarget) {
        m_renderTarget->SetDpi(m_dpi, m_dpi);
    }
}

void HistoryWindow::StartAnimationTimer() {
    if (m_hwnd && !m_animTimerActive) {
        m_lastAnimTick = GetTickCount64();
        SetTimer(m_hwnd, TIMER_ANIM_ID, 16, nullptr);
        m_animTimerActive = true;
    }
}

void HistoryWindow::StepAnimations() {
    ULONGLONG now = GetTickCount64();
    float dt = std::clamp(static_cast<float>(now - m_lastAnimTick) / 1000.0f, 0.004f, 0.05f);
    m_lastAnimTick = now;

    bool animating = false;

    // 1. Sidebar tab slide
    float targetTab = (m_activeTab == SidebarTab::Settings) ? 1.0f : 0.0f;
    if (UiTheme::Approach(m_tabSlideAnim, targetTab, 18.0f, dt)) animating = true;

    // 2. Smooth inertial scrolling
    if (UiTheme::Approach(m_codeScrollY, m_targetCodeScrollY, 20.0f, dt)) animating = true;
    if (UiTheme::Approach(m_listScrollY, m_targetListScrollY, 20.0f, dt)) animating = true;

    // 3. Toggle switch springs
    if (m_settings) {
        const bool vals[8] = {
            m_settings->codeIndentationMode,
            m_settings->lineSelectionMode,
            m_settings->autoCopyOnRelease,
            m_settings->upscaleForOcr,
            m_settings->autoCloseAfterCopy,
            m_settings->startWithWindows,
            m_settings->stripLineNumbers,
            m_settings->showConfidenceHeatmap
        };
        for (int i = 0; i < 8; ++i) {
            if (UiTheme::Approach(m_toggleAnim[i], vals[i] ? 1.0f : 0.0f, 20.0f, dt)) animating = true;
        }
    }

    // 4. Hotspot hover glow fades
    for (const auto& hs : m_hotspots) {
        float& hVal = m_hoverAnim[hs.id];
        if (UiTheme::Approach(hVal, hs.hovered ? 1.0f : 0.0f, 16.0f, dt)) animating = true;
    }

    // 5. Status toast fade-in / fade-out
    bool toastActive = (!m_statusBanner.empty() && now < m_statusExpireTick);
    if (UiTheme::Approach(m_toastAlpha, toastActive ? 1.0f : 0.0f, toastActive ? 16.0f : 8.0f, dt)) {
        animating = true;
    } else if (toastActive) {
        animating = true; // Keep timer alive until toast expires so it fades out cleanly
    }

    if (!animating && m_animTimerActive && m_hwnd) {
        KillTimer(m_hwnd, TIMER_ANIM_ID);
        m_animTimerActive = false;
    }

    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void HistoryWindow::ShowToast(const std::wstring& message, bool isError) {
    m_statusBanner = message;
    m_statusIsError = isError;
    m_statusExpireTick = GetTickCount64() + 3400;
    StartAnimationTimer();
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void HistoryWindow::RebuildSelectedCodeCache() {
    m_cachedLines.clear();
    m_cachedSpans.clear();
    m_selectedLang = L"Plain Text";

    if (m_history.empty() || m_selectedHistoryIdx < 0 || static_cast<size_t>(m_selectedHistoryIdx) >= m_history.size()) {
        return;
    }

    const auto& item = m_history[m_selectedHistoryIdx];
    m_selectedLang = item.wasCodeMode ? SyntaxHighlighter::DetectLanguage(item.text) : L"Plain Text";

    std::wstringstream ss(item.text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        m_cachedSpans.push_back(item.wasCodeMode ? SyntaxHighlighter::TokenizeLine(line) : std::vector<SyntaxSpan>{});
        m_cachedLines.push_back(std::move(line));
    }
    if (m_cachedLines.empty()) {
        m_cachedLines.push_back(L"");
        m_cachedSpans.push_back({});
    }
}

void HistoryWindow::RefreshControls() {
    StartAnimationTimer();
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

void HistoryWindow::AddHistoryEntry(
    const std::wstring& text,
    bool codeMode,
    size_t wordCount,
    double ocrTimeMs,
    const std::vector<std::wstring>& uncertainWords
) {
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
    item.uncertainWords = uncertainWords;

    m_history.insert(m_history.begin(), item);
    if (m_history.size() > 30) {
        m_history.pop_back();
    }
    m_selectedHistoryIdx = 0;
    m_codeScrollY = m_targetCodeScrollY = 0.0f;
    m_listScrollY = m_targetListScrollY = 0.0f;
    m_activeTab = SidebarTab::History;
    RebuildSelectedCodeCache();
    AppStorage::SaveHistory(m_history);
    StartAnimationTimer();

    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void HistoryWindow::RunOcrOnBitmap(const RawBitmap& bmp, const std::wstring& sourceLabel) {
    if (!bmp.valid()) {
        ShowToast(L"Could not load image from " + sourceLabel + L".", true);
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
        bool codeOn = m_settings ? m_settings->codeIndentationMode : true;
        bool stripGutter = !m_settings || m_settings->stripLineNumbers;
        size_t gutterStripped = TextFormatter::MarkLineNumberGutter(doc);

        std::vector<std::wstring> uncertainWords;
        for (const auto& line : doc.lines) {
            for (const auto& w : line.words) {
                if (w.isUncertain() && !w.text.empty()) {
                    if (std::find(uncertainWords.begin(), uncertainWords.end(), w.text) == uncertainWords.end()) {
                        uncertainWords.push_back(w.text);
                    }
                }
            }
        }

        size_t wc = 0;
        std::wstring formatted = TextFormatter::FormatDocument(doc, false, codeOn, &wc, stripGutter);
        OverlayWindow::CopyTextToClipboard(m_hwnd, formatted);
        AddHistoryEntry(formatted, codeOn, wc, doc.recognitionTimeMs, uncertainWords);

        std::wstring msg = L"Copied " + std::to_wstring(wc) + L" words from " + sourceLabel + L" (" +
                           std::to_wstring(static_cast<int>(doc.recognitionTimeMs)) + L" ms)";
        if (stripGutter && gutterStripped > 0) {
            msg += L" \x2022 -" + std::to_wstring(gutterStripped) + L" line #s";
        }
        ShowToast(msg);
    } else {
        ShowToast(err.empty() ? (L"No text detected in " + sourceLabel + L".") : err, true);
    }
}

void HistoryWindow::RunOcrFromClipboard() {
    RawBitmap clipBmp = ScreenCapture::LoadBitmapFromClipboard(m_hwnd);
    if (!clipBmp.valid()) {
        ShowToast(L"Clipboard has no image. Copy a screenshot or image file, then press Ctrl+V.", true);
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
    ofn.lpstrFilter = L"Code / Text Files (*.txt;*.cpp;*.js;*.ts;*.jsx;*.py;*.rs)\0*.txt;*.cpp;*.js;*.ts;*.jsx;*.py;*.rs\0All Files (*.*)\0*.*\0";
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
            ShowToast(L"Saved snippet to file.");
        }
    }
}

void HistoryWindow::CreateD2DResources() {
    if (m_renderTarget || !m_hwnd) return;

    RECT rc = {};
    GetClientRect(m_hwnd, &rc);
    UINT w = std::max<UINT>(1, rc.right - rc.left);
    UINT h = std::max<UINT>(1, rc.bottom - rc.top);

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        m_dpi,
        m_dpi,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE
    );

    HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(
        rtProps,
        D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(w, h)),
        &m_renderTarget
    );
    if (FAILED(hr)) {
        D2D1_RENDER_TARGET_PROPERTIES fallbackProps = D2D1::RenderTargetProperties();
        fallbackProps.dpiX = m_dpi;
        fallbackProps.dpiY = m_dpi;
        hr = m_d2dFactory->CreateHwndRenderTarget(
            fallbackProps,
            D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(w, h)),
            &m_renderTarget
        );
    }

    if (SUCCEEDED(hr) && m_renderTarget) {
        m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        m_renderTarget->CreateSolidColorBrush(UiTheme::TextPrimary, &m_brush);

        // Cached syntax highlighting brushes matching SyntaxKind enum order
        const D2D1_COLOR_F synCols[9] = {
            UiTheme::SynPlain,
            UiTheme::SynKeyword,
            UiTheme::SynType,
            UiTheme::SynFunction,
            UiTheme::SynString,
            UiTheme::SynNumber,
            UiTheme::SynComment,
            UiTheme::SynOperator,
            UiTheme::SynTag
        };
        for (int i = 0; i < 9; ++i) {
            m_renderTarget->CreateSolidColorBrush(synCols[i], &m_syntaxBrushes[i]);
        }

        // Brand emblem gradient brush
        D2D1_GRADIENT_STOP stops[2] = {
            { 0.0f, D2D1::ColorF(0.11f, 0.36f, 0.88f, 0.42f) },
            { 1.0f, D2D1::ColorF(0.14f, 0.72f, 0.96f, 0.22f) }
        };
        ComPtr<ID2D1GradientStopCollection> stopCol;
        if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(stops, 2, &stopCol))) {
            m_renderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties({ 18.0f, 16.0f }, { 56.0f, 54.0f }),
                stopCol.Get(),
                &m_logoGradBrush
            );
        }
    }
}

void HistoryWindow::DiscardD2DResources() {
    for (auto& b : m_syntaxBrushes) b.Reset();
    m_logoGradBrush.Reset();
    m_brush.Reset();
    m_renderTarget.Reset();
}

void HistoryWindow::BuildUiLayout(float width, float height) {
    m_hotspots.clear();

    const float sidebarW = 304.0f;
    const float sidePad = 16.0f;

    // 1. Left Sidebar Segmented Tabs (y = 76 .. 110)
    float tabW = (sidebarW - sidePad * 2.0f - 6.0f) * 0.5f;
    m_hotspots.push_back({
        ACT_TAB_HISTORY,
        { sidePad + 3.0f, 79.0f, tabW, 28.0f },
        L"History (" + std::to_wstring(m_history.size()) + L")",
        L"", L"", UiTheme::VectorIcon::HistoryClock, UiTheme::AccentPrimary, false
    });
    m_hotspots.push_back({
        ACT_TAB_SETTINGS,
        { sidePad + 3.0f + tabW, 79.0f, tabW, 28.0f },
        L"Settings",
        L"", L"", UiTheme::VectorIcon::Sliders, UiTheme::AccentPrimary, false
    });

    // Sidebar scrollable content viewport (y = 122 .. height - 66)
    float listTop = 122.0f;
    float listBot = std::max(listTop + 120.0f, height - 66.0f);
    m_sidebarListRect = { sidePad, listTop, sidebarW - sidePad * 2.0f, listBot - listTop };

    if (m_activeTab == SidebarTab::History) {
        float itemH = 62.0f;
        float itemGap = 7.0f;
        float totalListH = static_cast<float>(m_history.size()) * (itemH + itemGap);
        m_maxListScrollY = std::max(0.0f, totalListH - m_sidebarListRect.height);
        m_targetListScrollY = std::clamp(m_targetListScrollY, 0.0f, m_maxListScrollY);
        m_listScrollY = std::clamp(m_listScrollY, 0.0f, m_maxListScrollY);

        for (size_t i = 0; i < m_history.size(); ++i) {
            float iy = listTop + static_cast<float>(i) * (itemH + itemGap) - m_listScrollY;
            if (iy + itemH < listTop - 10.0f || iy > listBot + 10.0f) continue;
            const auto& h = m_history[i];
            std::wstring lang = h.wasCodeMode ? SyntaxHighlighter::DetectLanguage(h.text) : L"Text";
            std::wstring meta = h.timestamp + L" \x2022 " + std::to_wstring(h.wordCount) + L"w";
            m_hotspots.push_back({
                ACT_HIST_ITEM_BASE + static_cast<int>(i),
                { sidePad, iy, sidebarW - sidePad * 2.0f, itemH },
                ExtractFirstNonEmptyPreview(h.text),
                meta,
                lang,
                UiTheme::VectorIcon::HistoryClock,
                UiTheme::AccentPrimary,
                false
            });
        }
    } else {
        struct ToggleDef { int id; const wchar_t* title; const wchar_t* desc; };
        ToggleDef toggles[8] = {
            { ACT_TOGGLE_CODE,      L"Code Indentation",      L"Preserve tabs & fix \x00F8 \x2192 0" },
            { ACT_TOGGLE_GUTTER,    L"Strip Line Numbers",    L"Auto-remove editor gutter (1, 2, 3)" },
            { ACT_TOGGLE_HEATMAP,   L"Confidence Heatmap",    L"Amber warning on uncertain words" },
            { ACT_TOGGLE_LINEMODE,  L"Full-Line Selection",   L"Select whole lines on drag" },
            { ACT_TOGGLE_AUTOCOPY,  L"Auto-Copy on Release",  L"Copy immediately on mouse up" },
            { ACT_TOGGLE_UPSCALE,   L"2x Super-Res Boost",    L"Sharpen dark-mode video frames" },
            { ACT_TOGGLE_AUTOCLOSE, L"Auto-Close Overlay",    L"Dismiss right after copying" },
            { ACT_TOGGLE_STARTWIN,  L"Start with Windows",    L"Launch silently in System Tray" }
        };
        float tH = 50.0f;
        float tGap = 7.0f;
        float totalSettingsH = 8.0f * (tH + tGap);
        m_maxListScrollY = std::max(0.0f, totalSettingsH - m_sidebarListRect.height);
        m_targetListScrollY = std::clamp(m_targetListScrollY, 0.0f, m_maxListScrollY);
        m_listScrollY = std::clamp(m_listScrollY, 0.0f, m_maxListScrollY);

        for (int i = 0; i < 8; ++i) {
            float ty = listTop + static_cast<float>(i) * (tH + tGap) - m_listScrollY;
            if (ty + tH < listTop - 10.0f || ty > listBot + 10.0f) continue;
            m_hotspots.push_back({
                toggles[i].id,
                { sidePad, ty, sidebarW - sidePad * 2.0f, tH },
                toggles[i].title,
                toggles[i].desc,
                L"",
                UiTheme::VectorIcon::Sliders,
                UiTheme::AccentPrimary,
                false
            });
        }
    }

    // Sidebar Bottom Utility Buttons (y = height - 52 .. height - 16)
    float footY = height - 52.0f;
    float footW = (sidebarW - sidePad * 2.0f - 8.0f) * 0.5f;
    m_hotspots.push_back({
        ACT_BTN_PASTE_IMG,
        { sidePad, footY, footW + 10.0f, 36.0f },
        L"Paste (Ctrl+V)",
        L"", L"", UiTheme::VectorIcon::PasteImage, UiTheme::AccentPrimary, false
    });
    m_hotspots.push_back({
        ACT_BTN_DEMO_OCR,
        { sidePad + footW + 18.0f, footY, footW - 10.0f, 36.0f },
        L"Live Test",
        L"", L"", UiTheme::VectorIcon::SparkBolt, UiTheme::AccentAmber, false
    });

    // 2. Right Main Workspace
    float mainX = sidebarW + 20.0f;
    float mainW = std::max(460.0f, width - mainX - 20.0f);

    // Top 4 Capture Action Cards (y = 18 .. 104, height = 86)
    float cardGap = 12.0f;
    float cardW = (mainW - cardGap * 3.0f) / 4.0f;
    float cardY = 18.0f;
    float cardH = 86.0f;

    m_hotspots.push_back({
        ACT_HERO_REGION,
        { mainX, cardY, cardW, cardH },
        L"Region Selectext",
        L"Freeze & pick words",
        L"Alt+Shift+S",
        UiTheme::VectorIcon::RegionCrop,
        UiTheme::AccentPrimary,
        false
    });
    m_hotspots.push_back({
        ACT_HERO_FULLSCREEN,
        { mainX + cardW + cardGap, cardY, cardW, cardH },
        L"Full Screen",
        L"Scan active monitor",
        L"Alt+Shift+D",
        UiTheme::VectorIcon::FullScreen,
        UiTheme::AccentBlue,
        false
    });
    m_hotspots.push_back({
        ACT_HERO_QUICKSNIP,
        { mainX + (cardW + cardGap) * 2.0f, cardY, cardW, cardH },
        L"Quick Snip",
        L"1-step box copy",
        L"Alt+Shift+C",
        UiTheme::VectorIcon::QuickSnip,
        UiTheme::AccentViolet,
        false
    });
    m_hotspots.push_back({
        ACT_HERO_STITCH,
        { mainX + (cardW + cardGap) * 3.0f, cardY, cardW, cardH },
        L"Stitch & Append",
        L"Merge scrolled code",
        L"Alt+Shift+A",
        UiTheme::VectorIcon::StitchLayers,
        UiTheme::AccentEmerald,
        false
    });

    // Code Inspector Header Toolbar Buttons (y = 125 .. 153)
    float edHeaderBtnY = 126.0f;
    float rightEdge = mainX + mainW - 12.0f;
    m_hotspots.push_back({
        ACT_BTN_CLEAR_HIST,
        { rightEdge - 72.0f, edHeaderBtnY, 72.0f, 28.0f },
        L"Clear",
        L"", L"", UiTheme::VectorIcon::Trash, UiTheme::TextSecondary, false
    });
    m_hotspots.push_back({
        ACT_BTN_SAVE_FILE,
        { rightEdge - 152.0f, edHeaderBtnY, 74.0f, 28.0f },
        L"Save",
        L"", L"", UiTheme::VectorIcon::SaveFile, UiTheme::TextSecondary, false
    });
    m_hotspots.push_back({
        ACT_BTN_COPY_CODE,
        { rightEdge - 266.0f, edHeaderBtnY, 108.0f, 28.0f },
        L"Copy Code",
        L"", L"", UiTheme::VectorIcon::Clipboard, UiTheme::AccentPrimary, false
    });

    for (auto& hs : m_hotspots) {
        if (hs.id >= ACT_HIST_ITEM_BASE) {
            hs.hovered = m_sidebarListRect.contains(m_mouseX, m_mouseY) && hs.bounds.contains(m_mouseX, m_mouseY);
        } else {
            hs.hovered = hs.bounds.contains(m_mouseX, m_mouseY);
        }
    }
}

void HistoryWindow::OnPaint(HDC printHdc) {
    CreateD2DResources();
    if (!m_renderTarget || !m_brush) return;

    D2D1_SIZE_F rtSize = m_renderTarget->GetSize();
    float width = std::max(1.0f, rtSize.width);
    float height = std::max(1.0f, rtSize.height);

    BuildUiLayout(width, height);

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(UiTheme::BgCanvas);

    const float sidebarW = 304.0f;
    const float sidePad = 16.0f;

    // 1. Left Navigation Rail Surface
    m_brush->SetColor(UiTheme::BgSidebar);
    m_renderTarget->FillRectangle(D2D1::RectF(0.0f, 0.0f, sidebarW, height), m_brush.Get());
    m_brush->SetColor(UiTheme::BorderSubtle);
    m_renderTarget->DrawLine({ sidebarW, 0.0f }, { sidebarW, height }, m_brush.Get(), 1.0f);

    // Brand Header inside Sidebar
    D2D1_RECT_F logoRect = D2D1::RectF(sidePad, 16.0f, sidePad + 38.0f, 54.0f);
    if (m_logoGradBrush) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(logoRect, 9.0f, 9.0f), m_logoGradBrush.Get());
    }
    m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.72f));
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(logoRect, 9.0f, 9.0f), m_brush.Get(), 1.25f);
    UiTheme::DrawVectorIcon(m_renderTarget.Get(), m_brush.Get(), UiTheme::VectorIcon::BrandLogo, sidePad + 19.0f, 35.0f, 20.0f, UiTheme::AccentPrimary);

    if (m_brandFormat) {
        m_brush->SetColor(UiTheme::TextPrimary);
        m_renderTarget->DrawTextW(L"ScreenTextify", 13, m_brandFormat.Get(), D2D1::RectF(sidePad + 48.0f, 15.0f, sidebarW - 60.0f, 38.0f), m_brush.Get());
    }
    // Version badge
    D2D1_RECT_F verPill = D2D1::RectF(sidebarW - sidePad - 38.0f, 18.0f, sidebarW - sidePad, 35.0f);
    m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.14f));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(verPill, 4.0f, 4.0f), m_brush.Get());
    m_brush->SetColor(UiTheme::AccentPrimary);
    if (m_badgeFormat) {
        m_renderTarget->DrawTextW(L"v3.0", 4, m_badgeFormat.Get(), verPill, m_brush.Get());
    }

    // Active Engine indicator row under Brand Title
    m_brush->SetColor(UiTheme::AccentEmerald);
    m_renderTarget->FillEllipse(D2D1::Ellipse({ sidePad + 53.0f, 46.5f }, 3.5f, 3.5f), m_brush.Get());
    if (m_smallFormat) {
        m_brush->SetColor(UiTheme::TextSecondary);
        m_renderTarget->DrawTextW(m_ocrLangName.c_str(), static_cast<UINT32>(m_ocrLangName.length()),
                                  m_smallFormat.Get(), D2D1::RectF(sidePad + 62.0f, 39.0f, sidebarW - sidePad, 55.0f), m_brush.Get());
    }

    // Segmented Tab Control Background (y = 76 .. 110)
    D2D1_RECT_F segBg = D2D1::RectF(sidePad, 76.0f, sidebarW - sidePad, 110.0f);
    m_brush->SetColor(UiTheme::BgCanvas);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(segBg, 8.0f, 8.0f), m_brush.Get());
    m_brush->SetColor(UiTheme::BorderSubtle);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(segBg, 8.0f, 8.0f), m_brush.Get(), 1.0f);

    // Smooth sliding active tab pill
    float tabW = (sidebarW - sidePad * 2.0f - 6.0f) * 0.5f;
    float pillX = sidePad + 3.0f + m_tabSlideAnim * tabW;
    D2D1_RECT_F activeTabPill = D2D1::RectF(pillX, 79.0f, pillX + tabW, 107.0f);
    m_brush->SetColor(UiTheme::BgSurfaceHover);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(activeTabPill, 6.0f, 6.0f), m_brush.Get());
    m_brush->SetColor(UiTheme::TopSpecular);
    m_renderTarget->DrawLine({ activeTabPill.left + 6.0f, activeTabPill.top + 0.5f }, { activeTabPill.right - 6.0f, activeTabPill.top + 0.5f }, m_brush.Get(), 1.0f);

    // Draw Tab Labels + Icons (optically centered icon + label pair)
    for (const auto& hs : m_hotspots) {
        if (hs.id != ACT_TAB_HISTORY && hs.id != ACT_TAB_SETTINGS) continue;
        bool isActive = (hs.id == ACT_TAB_HISTORY && m_activeTab == SidebarTab::History) ||
                        (hs.id == ACT_TAB_SETTINGS && m_activeTab == SidebarTab::Settings);
        D2D1_COLOR_F col = isActive ? UiTheme::TextPrimary : UiTheme::TextSecondary;
        float estTextW = static_cast<float>(hs.title.size()) * 6.2f;
        float groupW = 18.0f + estTextW;
        float startX = hs.bounds.centerX() - groupW * 0.5f;
        UiTheme::DrawVectorIcon(m_renderTarget.Get(), m_brush.Get(), hs.icon, startX + 6.5f, hs.bounds.centerY(), 12.5f, isActive ? UiTheme::AccentPrimary : UiTheme::TextMuted);
        if (m_badgeFormat) {
            m_brush->SetColor(col);
            D2D1_RECT_F tr = D2D1::RectF(startX + 16.0f, hs.bounds.y, startX + 16.0f + estTextW + 10.0f, hs.bounds.bottom());
            m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_badgeFormat.Get(), tr, m_brush.Get());
        }
    }

    // 2. Render Clipped Sidebar Content (History List OR Settings Switches)
    D2D1_RECT_F listClip = D2D1::RectF(m_sidebarListRect.x - 2.0f, m_sidebarListRect.y, m_sidebarListRect.right() + 2.0f, m_sidebarListRect.bottom());
    m_renderTarget->PushAxisAlignedClip(listClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (m_activeTab == SidebarTab::History && m_history.empty()) {
        if (m_bodyFormat) {
            m_brush->SetColor(UiTheme::TextMuted);
            D2D1_RECT_F emptyR = D2D1::RectF(sidePad + 8.0f, listClip.top + 24.0f, sidebarW - sidePad - 8.0f, listClip.top + 80.0f);
            m_renderTarget->DrawTextW(L"No captures yet. Press Alt+Shift+S!", 35, m_bodyFormat.Get(), emptyR, m_brush.Get());
        }
    }

    for (const auto& hs : m_hotspots) {
        float hAnim = m_hoverAnim[hs.id];
        D2D1_RECT_F r = D2D1::RectF(hs.bounds.x, hs.bounds.y, hs.bounds.right(), hs.bounds.bottom());

        if (hs.id >= ACT_HIST_ITEM_BASE) {
            int idx = hs.id - ACT_HIST_ITEM_BASE;
            bool selected = (idx == m_selectedHistoryIdx);

            D2D1_COLOR_F bg = selected
                ? D2D1::ColorF(0.095f, 0.145f, 0.235f, 1.0f)
                : UiTheme::LerpColor(UiTheme::BgSurface, UiTheme::BgSurfaceHover, hAnim);
            m_brush->SetColor(bg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 8.0f, 8.0f), m_brush.Get());

            D2D1_COLOR_F border = selected
                ? UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.55f)
                : UiTheme::LerpColor(UiTheme::BorderSubtle, UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.35f), hAnim);
            m_brush->SetColor(border);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 8.0f, 8.0f), m_brush.Get(), 1.0f);

            if (selected) {
                // Crisp vertical selection bar on the left
                m_brush->SetColor(UiTheme::AccentPrimary);
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(r.left + 1.0f, r.top + 10.0f, r.left + 4.0f, r.bottom - 10.0f), 1.5f, 1.5f),
                    m_brush.Get()
                );
            }

            // Row 1: Language tag pill + right-aligned timestamp/word count
            float tagW = std::clamp(static_cast<float>(hs.badge.size()) * 6.8f + 14.0f, 44.0f, 88.0f);
            D2D1_RECT_F langPill = D2D1::RectF(r.left + 12.0f, r.top + 9.0f, r.left + 12.0f + tagW, r.top + 26.0f);
            m_brush->SetColor(selected ? UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.20f) : UiTheme::WithAlpha(UiTheme::TextSecondary, 0.12f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(langPill, 4.0f, 4.0f), m_brush.Get());
            if (m_badgeFormat) {
                m_brush->SetColor(selected ? UiTheme::AccentPrimary : UiTheme::TextSecondary);
                m_renderTarget->DrawTextW(hs.badge.c_str(), static_cast<UINT32>(hs.badge.length()), m_badgeFormat.Get(), langPill, m_brush.Get());
            }
            if (m_smallFormat) {
                m_brush->SetColor(UiTheme::TextMuted);
                D2D1_RECT_F metaR = D2D1::RectF(langPill.right + 8.0f, r.top + 10.0f, r.right - 10.0f, r.top + 26.0f);
                m_renderTarget->DrawTextW(hs.subtitle.c_str(), static_cast<UINT32>(hs.subtitle.length()), m_smallFormat.Get(), metaR, m_brush.Get());
            }

            // Row 2: Monospace snippet preview line
            if (m_monoSmallFormat) {
                m_brush->SetColor(selected ? UiTheme::TextPrimary : UiTheme::TextSecondary);
                D2D1_RECT_F prevR = D2D1::RectF(r.left + 12.0f, r.top + 33.0f, r.right - 12.0f, r.bottom - 8.0f);
                m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_monoSmallFormat.Get(), prevR, m_brush.Get());
            }
        } else if (hs.id >= ACT_TOGGLE_CODE && hs.id <= ACT_TOGGLE_HEATMAP) {
            int tIdx = hs.id - ACT_TOGGLE_CODE;
            float anim = (tIdx >= 0 && tIdx < 8) ? m_toggleAnim[tIdx] : 0.0f;

            m_brush->SetColor(UiTheme::LerpColor(UiTheme::BgSurface, UiTheme::BgSurfaceHover, hAnim));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 8.0f, 8.0f), m_brush.Get());
            m_brush->SetColor(UiTheme::LerpColor(UiTheme::BorderSubtle, UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.45f), hAnim));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 8.0f, 8.0f), m_brush.Get(), 1.0f);

            if (m_cardTitleFormat) {
                m_brush->SetColor(UiTheme::TextPrimary);
                m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_cardTitleFormat.Get(),
                                          D2D1::RectF(r.left + 12.0f, r.top + 8.0f, r.right - 60.0f, r.top + 27.0f), m_brush.Get());
            }
            if (m_smallFormat) {
                m_brush->SetColor(UiTheme::TextSecondary);
                m_renderTarget->DrawTextW(hs.subtitle.c_str(), static_cast<UINT32>(hs.subtitle.length()), m_smallFormat.Get(),
                                          D2D1::RectF(r.left + 12.0f, r.top + 29.0f, r.right - 60.0f, r.bottom - 6.0f), m_brush.Get());
            }

            // Smooth animated toggle switch
            float swW = 40.0f, swH = 22.0f;
            float swX = r.right - swW - 12.0f;
            float swY = hs.bounds.centerY() - swH * 0.5f;
            D2D1_COLOR_F trackOff = D2D1::ColorF(0.16f, 0.21f, 0.30f, 1.0f);
            D2D1_COLOR_F trackOn  = UiTheme::AccentBlue;
            m_brush->SetColor(UiTheme::LerpColor(trackOff, trackOn, anim));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(swX, swY, swX + swW, swY + swH), 11.0f, 11.0f), m_brush.Get());

            float knobX = (swX + 11.0f) + anim * (swW - 22.0f);
            float knobY = swY + 11.0f;
            m_brush->SetColor(UiTheme::TextPrimary);
            m_renderTarget->FillEllipse(D2D1::Ellipse({ knobX, knobY }, 7.5f, 7.5f), m_brush.Get());
        }
    }

    m_renderTarget->PopAxisAlignedClip();

    // Subtle sidebar list scrollbar indicator when scrollable (in right margin of rail)
    if (m_maxListScrollY > 0.5f) {
        float trackH = std::max(24.0f, m_sidebarListRect.height - 8.0f);
        float totalListH = m_sidebarListRect.height + m_maxListScrollY;
        float thumbH = std::max(24.0f, trackH * (m_sidebarListRect.height / totalListH));
        float thumbY = m_sidebarListRect.y + 4.0f + (m_listScrollY / m_maxListScrollY) * (trackH - thumbH);
        D2D1_RECT_F sbR = D2D1::RectF(sidebarW - 8.0f, thumbY, sidebarW - 4.5f, thumbY + thumbH);
        m_brush->SetColor(UiTheme::WithAlpha(UiTheme::TextMuted, 0.50f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sbR, 1.75f, 1.75f), m_brush.Get());
    }

    // Subtle divider above sidebar footer buttons
    m_brush->SetColor(UiTheme::BorderSubtle);
    m_renderTarget->DrawLine({ sidePad, height - 60.0f }, { sidebarW - sidePad, height - 60.0f }, m_brush.Get(), 1.0f);

    // 3. Render Top 4 Hero Capture Action Cards & All Buttons
    for (const auto& hs : m_hotspots) {
        float hAnim = m_hoverAnim[hs.id];
        D2D1_RECT_F r = D2D1::RectF(hs.bounds.x, hs.bounds.y, hs.bounds.right(), hs.bounds.bottom());

        if (hs.id >= ACT_HERO_REGION && hs.id <= ACT_HERO_STITCH) {
            if (hAnim > 0.05f) {
                UiTheme::DrawSoftShadow(m_renderTarget.Get(), m_brush.Get(), r, 10.0f, 0.7f * hAnim);
            }

            D2D1_COLOR_F cardBg = UiTheme::LerpColor(UiTheme::BgSurface, UiTheme::BgSurfaceHover, hAnim);
            m_brush->SetColor(cardBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 10.0f, 10.0f), m_brush.Get());

            // Subtle top specular highlight
            m_brush->SetColor(UiTheme::TopSpecular);
            m_renderTarget->DrawLine({ r.left + 10.0f, r.top + 0.75f }, { r.right - 10.0f, r.top + 0.75f }, m_brush.Get(), 1.0f);

            D2D1_COLOR_F borderCol = UiTheme::LerpColor(UiTheme::BorderSubtle, UiTheme::WithAlpha(hs.accent, 0.85f), hAnim);
            m_brush->SetColor(borderCol);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 10.0f, 10.0f), m_brush.Get(), 1.15f + 0.45f * hAnim);

            m_renderTarget->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            // Top row: Tinted Vector Icon box (left) + Hotkey badge (right)
            D2D1_RECT_F iconBox = D2D1::RectF(r.left + 12.0f, r.top + 11.0f, r.left + 38.0f, r.top + 37.0f);
            m_brush->SetColor(UiTheme::WithAlpha(hs.accent, 0.14f + 0.10f * hAnim));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(iconBox, 7.0f, 7.0f), m_brush.Get());
            UiTheme::DrawVectorIcon(m_renderTarget.Get(), m_brush.Get(), hs.icon, r.left + 25.0f, r.top + 24.0f, 15.0f, hs.accent);

            D2D1_RECT_F kbRect = D2D1::RectF(r.right - 88.0f, r.top + 13.0f, r.right - 12.0f, r.top + 34.0f);
            m_brush->SetColor(UiTheme::BgCanvas);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(kbRect, 5.0f, 5.0f), m_brush.Get());
            m_brush->SetColor(UiTheme::BorderSubtle);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(kbRect, 5.0f, 5.0f), m_brush.Get(), 1.0f);
            if (m_badgeFormat) {
                m_brush->SetColor(UiTheme::LerpColor(UiTheme::TextSecondary, hs.accent, hAnim));
                m_renderTarget->DrawTextW(hs.badge.c_str(), static_cast<UINT32>(hs.badge.length()), m_badgeFormat.Get(), kbRect, m_brush.Get());
            }

            // Middle row: Title
            if (m_cardTitleFormat) {
                m_brush->SetColor(UiTheme::TextPrimary);
                m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_cardTitleFormat.Get(),
                                          D2D1::RectF(r.left + 12.0f, r.top + 44.0f, r.right - 12.0f, r.top + 63.0f), m_brush.Get());
            }
            // Bottom row: Subtitle
            if (m_smallFormat) {
                m_brush->SetColor(UiTheme::TextSecondary);
                m_renderTarget->DrawTextW(hs.subtitle.c_str(), static_cast<UINT32>(hs.subtitle.length()), m_smallFormat.Get(),
                                          D2D1::RectF(r.left + 12.0f, r.top + 64.0f, r.right - 12.0f, r.bottom - 6.0f), m_brush.Get());
            }

            m_renderTarget->PopAxisAlignedClip();
        } else if (hs.id == ACT_BTN_PASTE_IMG || hs.id == ACT_BTN_DEMO_OCR) {
            bool isPrimary = (hs.id == ACT_BTN_PASTE_IMG);
            D2D1_COLOR_F baseBg = isPrimary ? D2D1::ColorF(0.11f, 0.35f, 0.78f, 0.92f) : UiTheme::BgSurface;
            D2D1_COLOR_F hovBg  = isPrimary ? D2D1::ColorF(0.15f, 0.46f, 0.94f, 1.0f)  : UiTheme::BgSurfaceHover;
            m_brush->SetColor(UiTheme::LerpColor(baseBg, hovBg, hAnim));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 6.5f, 6.5f), m_brush.Get());

            D2D1_COLOR_F baseBd = isPrimary ? UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.65f) : UiTheme::BorderSubtle;
            D2D1_COLOR_F hovBd  = isPrimary ? UiTheme::AccentPrimary : UiTheme::WithAlpha(hs.accent, 0.65f);
            m_brush->SetColor(UiTheme::LerpColor(baseBd, hovBd, hAnim));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 6.5f, 6.5f), m_brush.Get(), 1.0f);

            UiTheme::DrawVectorIcon(m_renderTarget.Get(), m_brush.Get(), hs.icon, r.left + 16.0f, hs.bounds.centerY(), 13.0f,
                                    isPrimary ? UiTheme::TextPrimary : hs.accent);
            if (m_badgeFormat) {
                m_brush->SetColor(UiTheme::TextPrimary);
                D2D1_RECT_F tr = D2D1::RectF(r.left + 26.0f, r.top, r.right - 6.0f, r.bottom);
                m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_badgeFormat.Get(), tr, m_brush.Get());
            }
        }
    }

    // 4. Main Syntax-Highlighted Code Inspector Card (y = 118 .. height - 18)
    float mainX = sidebarW + 20.0f;
    float mainW = std::max(460.0f, width - mainX - 20.0f);
    float edTop = 118.0f;
    float edBot = std::max(edTop + 200.0f, height - 18.0f);
    float edHeaderH = 44.0f;
    m_codeViewportRect = { mainX, edTop + edHeaderH, mainW, std::max(10.0f, edBot - (edTop + edHeaderH)) };

    D2D1_RECT_F edPanel = D2D1::RectF(mainX, edTop, mainX + mainW, edBot);
    UiTheme::DrawSoftShadow(m_renderTarget.Get(), m_brush.Get(), edPanel, 12.0f, 0.6f);
    m_brush->SetColor(UiTheme::BgEditor);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(edPanel, 12.0f, 12.0f), m_brush.Get());

    // Header strip of Code Inspector
    D2D1_RECT_F edHeader = D2D1::RectF(mainX, edTop, mainX + mainW, edTop + edHeaderH);
    m_brush->SetColor(UiTheme::BgEditorHeader);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(edHeader, 12.0f, 12.0f), m_brush.Get());
    m_renderTarget->FillRectangle(D2D1::RectF(mainX, edTop + edHeaderH - 10.0f, mainX + mainW, edTop + edHeaderH), m_brush.Get());
    m_brush->SetColor(UiTheme::TopSpecular);
    m_renderTarget->DrawLine({ mainX + 12.0f, edTop + 0.75f }, { mainX + mainW - 12.0f, edTop + 0.75f }, m_brush.Get(), 1.0f);
    m_brush->SetColor(UiTheme::BorderSubtle);
    m_renderTarget->DrawLine({ mainX, edTop + edHeaderH }, { mainX + mainW, edTop + edHeaderH }, m_brush.Get(), 1.0f);

    // Re-draw header toolbar buttons on top of the editor header strip
    for (const auto& hs : m_hotspots) {
        if (hs.id != ACT_BTN_COPY_CODE && hs.id != ACT_BTN_SAVE_FILE && hs.id != ACT_BTN_CLEAR_HIST) continue;
        float hAnim = m_hoverAnim[hs.id];
        D2D1_RECT_F r = D2D1::RectF(hs.bounds.x, hs.bounds.y, hs.bounds.right(), hs.bounds.bottom());
        bool isPrimary = (hs.id == ACT_BTN_COPY_CODE);
        D2D1_COLOR_F baseBg = isPrimary ? D2D1::ColorF(0.11f, 0.35f, 0.78f, 0.92f) : UiTheme::BgSurface;
        D2D1_COLOR_F hovBg  = isPrimary ? D2D1::ColorF(0.15f, 0.46f, 0.94f, 1.0f)  : UiTheme::BgSurfaceHover;
        m_brush->SetColor(UiTheme::LerpColor(baseBg, hovBg, hAnim));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), m_brush.Get());
        m_brush->SetColor(isPrimary ? UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.75f) : UiTheme::BorderSubtle);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 6.0f, 6.0f), m_brush.Get(), 1.0f);
        UiTheme::DrawVectorIcon(m_renderTarget.Get(), m_brush.Get(), hs.icon, r.left + 15.0f, hs.bounds.centerY(), 12.5f,
                                isPrimary ? UiTheme::TextPrimary : UiTheme::TextSecondary);
        if (m_badgeFormat) {
            m_brush->SetColor(UiTheme::TextPrimary);
            m_renderTarget->DrawTextW(hs.title.c_str(), static_cast<UINT32>(hs.title.length()), m_badgeFormat.Get(),
                                      D2D1::RectF(r.left + 24.0f, r.top, r.right - 6.0f, r.bottom), m_brush.Get());
        }
    }

    if (!m_history.empty() && m_selectedHistoryIdx >= 0 && static_cast<size_t>(m_selectedHistoryIdx) < m_history.size()) {
        const auto& item = m_history[m_selectedHistoryIdx];

        // Language Pill + Metadata in Inspector Header
        float langW = std::clamp(static_cast<float>(m_selectedLang.size()) * 7.2f + 22.0f, 64.0f, 120.0f);
        D2D1_RECT_F langPill = D2D1::RectF(mainX + 14.0f, edTop + 10.0f, mainX + 14.0f + langW, edTop + 34.0f);
        m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.16f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(langPill, 6.0f, 6.0f), m_brush.Get());
        m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.45f));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(langPill, 6.0f, 6.0f), m_brush.Get(), 1.0f);
        if (m_badgeFormat) {
            m_brush->SetColor(UiTheme::AccentPrimary);
            m_renderTarget->DrawTextW(m_selectedLang.c_str(), static_cast<UINT32>(m_selectedLang.length()), m_badgeFormat.Get(), langPill, m_brush.Get());
        }

        float metaStartX = langPill.right + 12.0f;
        bool showHeatmap = !m_settings || m_settings->showConfidenceHeatmap;
        if (showHeatmap && !item.uncertainWords.empty()) {
            std::wstring warnText = L"\x26A0 " + std::to_wstring(item.uncertainWords.size()) + L" uncertain";
            float warnW = std::clamp(static_cast<float>(warnText.size()) * 7.0f + 18.0f, 88.0f, 134.0f);
            D2D1_RECT_F warnPill = D2D1::RectF(metaStartX, edTop + 10.0f, metaStartX + warnW, edTop + 34.0f);
            m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentAmber, 0.16f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(warnPill, 6.0f, 6.0f), m_brush.Get());
            m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentAmber, 0.55f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(warnPill, 6.0f, 6.0f), m_brush.Get(), 1.0f);
            if (m_badgeFormat) {
                m_brush->SetColor(UiTheme::AccentAmber);
                m_renderTarget->DrawTextW(warnText.c_str(), static_cast<UINT32>(warnText.length()), m_badgeFormat.Get(), warnPill, m_brush.Get());
            }
            metaStartX = warnPill.right + 12.0f;
        }

        std::wstringstream meta;
        meta << m_cachedLines.size() << L" lines  \x2022  " << item.wordCount << L" words";
        if (item.ocrTimeMs > 0.1) {
            meta << L"  \x2022  " << std::fixed << std::setprecision(0) << item.ocrTimeMs << L" ms";
        }
        std::wstring metaStr = meta.str();
        if (m_bodyFormat && metaStartX < mainX + mainW - 296.0f) {
            m_brush->SetColor(UiTheme::TextSecondary);
            m_renderTarget->DrawTextW(metaStr.c_str(), static_cast<UINT32>(metaStr.length()), m_bodyFormat.Get(),
                                      D2D1::RectF(metaStartX, edTop + 14.0f, mainX + mainW - 286.0f, edTop + 34.0f), m_brush.Get());
        }

        // Scrollable Syntax-Highlighted Code Viewport
        float bodyTop = edTop + edHeaderH + 1.0f;
        float bodyBot = edBot - 2.0f;
        float bodyH = std::max(10.0f, bodyBot - bodyTop);
        D2D1_RECT_F bodyClip = D2D1::RectF(mainX + 1.0f, bodyTop, mainX + mainW - 1.0f, bodyBot);
        m_renderTarget->PushAxisAlignedClip(bodyClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        float gutterW = 48.0f;
        m_brush->SetColor(UiTheme::BgEditorGutter);
        m_renderTarget->FillRectangle(D2D1::RectF(mainX, bodyTop, mainX + gutterW, bodyBot), m_brush.Get());
        m_brush->SetColor(UiTheme::WithAlpha(UiTheme::BorderSubtle, 0.65f));
        m_renderTarget->DrawLine({ mainX + gutterW, bodyTop }, { mainX + gutterW, bodyBot }, m_brush.Get(), 1.0f);

        const float lineH = 22.0f;
        float totalContentH = static_cast<float>(m_cachedLines.size()) * lineH + 22.0f;
        m_maxCodeScrollY = std::max(0.0f, totalContentH - bodyH);
        m_targetCodeScrollY = std::clamp(m_targetCodeScrollY, 0.0f, m_maxCodeScrollY);
        m_codeScrollY = std::clamp(m_codeScrollY, 0.0f, m_maxCodeScrollY);

        float codeTextW = std::max(100.0f, mainW - gutterW - 30.0f);

        for (size_t i = 0; i < m_cachedLines.size(); ++i) {
            float ly = bodyTop + 10.0f + static_cast<float>(i) * lineH - m_codeScrollY;
            if (ly + lineH < bodyTop) continue;
            if (ly > bodyBot) break;

            // Subtle active-line hover highlight
            if (static_cast<int>(i) == m_hoveredCodeLine) {
                m_brush->SetColor(D2D1::ColorF(0.22f, 0.74f, 0.97f, 0.06f));
                m_renderTarget->FillRectangle(D2D1::RectF(mainX + gutterW + 1.0f, ly - 1.0f, mainX + mainW - 12.0f, ly + lineH - 1.0f), m_brush.Get());
            }

            // Line number in gutter
            if (m_lineNumFormat) {
                std::wstring numStr = std::to_wstring(i + 1);
                m_brush->SetColor(static_cast<int>(i) == m_hoveredCodeLine ? UiTheme::AccentPrimary : UiTheme::TextMuted);
                m_renderTarget->DrawTextW(numStr.c_str(), static_cast<UINT32>(numStr.length()), m_lineNumFormat.Get(),
                                          D2D1::RectF(mainX + 4.0f, ly + 1.0f, mainX + gutterW - 10.0f, ly + lineH), m_brush.Get());
            }

            // Syntax-highlighted code line via IDWriteTextLayout + SetDrawingEffect
            const std::wstring& lineStr = m_cachedLines[i];
            if (!lineStr.empty() && m_monoFormat && m_dwriteFactory) {
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                        lineStr.c_str(), static_cast<UINT32>(lineStr.size()),
                        m_monoFormat.Get(), codeTextW, lineH, &layout)) && layout) {
                    if (i < m_cachedSpans.size()) {
                        for (const auto& sp : m_cachedSpans[i]) {
                            int bIdx = static_cast<int>(sp.kind);
                            if (bIdx >= 0 && bIdx < 9 && m_syntaxBrushes[bIdx]) {
                                DWRITE_TEXT_RANGE range = { sp.start, sp.length };
                                layout->SetDrawingEffect(m_syntaxBrushes[bIdx].Get(), range);
                            }
                        }
                    }

                    // Confidence Heatmap: highlight uncertain OCR words with warm Amber tint & underline
                    float textOriginX = mainX + gutterW + 14.0f;
                    if (showHeatmap && !item.uncertainWords.empty()) {
                        for (const auto& uw : item.uncertainWords) {
                            if (uw.size() < 2) continue;
                            size_t pos = 0;
                            while ((pos = lineStr.find(uw, pos)) != std::wstring::npos) {
                                UINT32 hitCount = 0;
                                DWRITE_HIT_TEST_METRICS metrics[4] = {};
                                HRESULT hrHit = layout->HitTestTextRange(
                                    static_cast<UINT32>(pos), static_cast<UINT32>(uw.size()),
                                    textOriginX, ly, metrics, 4, &hitCount
                                );
                                if (SUCCEEDED(hrHit) && hitCount > 0) {
                                    const auto& m = metrics[0];
                                    D2D1_RECT_F ur = D2D1::RectF(m.left - 2.0f, ly, m.left + m.width + 2.0f, ly + lineH - 2.0f);
                                    m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentAmber, 0.16f));
                                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(ur, 3.0f, 3.0f), m_brush.Get());
                                    m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentAmber, 0.90f));
                                    m_renderTarget->DrawLine({ ur.left + 1.0f, ur.bottom - 0.5f }, { ur.right - 1.0f, ur.bottom - 0.5f }, m_brush.Get(), 1.6f);
                                }
                                pos += uw.size();
                            }
                        }
                    }

                    m_renderTarget->DrawTextLayout({ textOriginX, ly }, layout.Get(), m_syntaxBrushes[0].Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }

        // Sleek interactive scrollbar
        m_scrollbarTrackRect = {};
        if (m_maxCodeScrollY > 0.5f) {
            float trackTop = bodyTop + 6.0f;
            float trackBot = bodyBot - 6.0f;
            float trackH = std::max(24.0f, trackBot - trackTop);
            float thumbH = std::max(28.0f, trackH * (bodyH / totalContentH));
            float thumbY = trackTop + (m_codeScrollY / m_maxCodeScrollY) * (trackH - thumbH);

            m_scrollbarTrackRect = { mainX + mainW - 14.0f, trackTop, 14.0f, trackH };
            bool hovSb = m_draggingScrollbar || m_scrollbarTrackRect.contains(m_mouseX, m_mouseY);
            float sbW = hovSb ? 7.0f : 4.5f;
            D2D1_RECT_F thumbRect = D2D1::RectF(mainX + mainW - 4.0f - sbW, thumbY, mainX + mainW - 4.0f, thumbY + thumbH);
            m_brush->SetColor(hovSb ? UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.72f) : UiTheme::WithAlpha(UiTheme::TextMuted, 0.55f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(thumbRect, sbW * 0.5f, sbW * 0.5f), m_brush.Get());
        }

        m_renderTarget->PopAxisAlignedClip();
    } else {
        if (m_bodyFormat) {
            m_brush->SetColor(UiTheme::TextMuted);
            D2D1_RECT_F emptyR = D2D1::RectF(mainX + 24.0f, edTop + 72.0f, mainX + mainW - 24.0f, edTop + 120.0f);
            m_renderTarget->DrawTextW(L"No snippets captured yet. Press Alt+Shift+S, paste an image (Ctrl+V), or drop a screenshot here.", 95, m_bodyFormat.Get(), emptyR, m_brush.Get());
        }
    }

    // Crisp outer border of Code Inspector
    m_brush->SetColor(UiTheme::BorderSubtle);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(edPanel, 12.0f, 12.0f), m_brush.Get(), 1.15f);

    // 5. Floating Animated Toast Notification Pill (bottom-right of workspace)
    if (m_toastAlpha > 0.01f && !m_statusBanner.empty()) {
        float toastW = std::clamp(static_cast<float>(m_statusBanner.size()) * 6.8f + 44.0f, 220.0f, mainW - 32.0f);
        float toastH = 36.0f;
        float slideY = (1.0f - m_toastAlpha) * 10.0f;
        D2D1_RECT_F toastR = D2D1::RectF(mainX + mainW - toastW - 16.0f, edBot - toastH - 16.0f + slideY,
                                         mainX + mainW - 16.0f, edBot - 16.0f + slideY);

        UiTheme::DrawSoftShadow(m_renderTarget.Get(), m_brush.Get(), toastR, 8.0f, 0.9f * m_toastAlpha);
        D2D1_COLOR_F accent = m_statusIsError ? UiTheme::AccentAmber : UiTheme::AccentEmerald;
        m_brush->SetColor(D2D1::ColorF(0.055f, 0.085f, 0.125f, 0.96f * m_toastAlpha));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(toastR, 8.0f, 8.0f), m_brush.Get());
        m_brush->SetColor(UiTheme::WithAlpha(accent, 0.85f * m_toastAlpha));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(toastR, 8.0f, 8.0f), m_brush.Get(), 1.25f);

        m_brush->SetColor(UiTheme::WithAlpha(accent, m_toastAlpha));
        m_renderTarget->FillEllipse(D2D1::Ellipse({ toastR.left + 16.0f, (toastR.top + toastR.bottom) * 0.5f }, 4.0f, 4.0f), m_brush.Get());

        if (m_badgeFormat) {
            m_brush->SetColor(UiTheme::WithAlpha(UiTheme::TextPrimary, m_toastAlpha));
            m_renderTarget->DrawTextW(m_statusBanner.c_str(), static_cast<UINT32>(m_statusBanner.length()),
                                      m_badgeFormat.Get(), D2D1::RectF(toastR.left + 26.0f, toastR.top, toastR.right - 12.0f, toastR.bottom), m_brush.Get());
        }
    }

    if (printHdc) {
        ComPtr<ID2D1GdiInteropRenderTarget> gdiInterop;
        if (SUCCEEDED(m_renderTarget.As(&gdiInterop))) {
            RECT rc = {};
            GetClientRect(m_hwnd, &rc);
            HDC d2dHdc = nullptr;
            if (SUCCEEDED(gdiInterop->GetDC(D2D1_DC_INITIALIZE_MODE_COPY, &d2dHdc)) && d2dHdc) {
                BitBlt(printHdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, d2dHdc, 0, 0, SRCCOPY);
                RECT emptyRect = {};
                gdiInterop->ReleaseDC(&emptyRect);
            }
        }
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
    case WM_DPICHANGED: {
        UpdateDpi(HIWORD(wParam));
        auto* prc = reinterpret_cast<RECT*>(lParam);
        if (prc) {
            SetWindowPos(hwnd, nullptr, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

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
        mmi->ptMinTrackSize.x = static_cast<LONG>(960.0f * m_dpiScale);
        mmi->ptMinTrackSize.y = static_cast<LONG>(620.0f * m_dpiScale);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == TIMER_ANIM_ID) {
            StepAnimations();
            return 0;
        }
        break;
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
                ShowToast(L"Copied snippet to Clipboard!");
            }
            return 0;
        }
        break;
    }

    case WM_PAINT: {
        if (wParam != 0) {
            OnPaint(reinterpret_cast<HDC>(wParam));
            return 0;
        }
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        OnPaint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_PRINTCLIENT: {
        HDC targetHdc = reinterpret_cast<HDC>(wParam);
        if (targetHdc) {
            OnPaint(targetHdc);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        float step = 66.0f;
        if (m_sidebarListRect.contains(m_mouseX, m_mouseY)) {
            m_targetListScrollY = std::clamp(m_targetListScrollY - (delta > 0 ? step : -step), 0.0f, m_maxListScrollY);
        } else {
            m_targetCodeScrollY = std::clamp(m_targetCodeScrollY - (delta > 0 ? step : -step), 0.0f, m_maxCodeScrollY);
        }
        StartAnimationTimer();
        return 0;
    }

    case WM_SETCURSOR: {
        bool overInteractive = m_draggingScrollbar || m_scrollbarTrackRect.contains(m_mouseX, m_mouseY);
        if (!overInteractive) {
            for (const auto& hs : m_hotspots) {
                if (hs.hovered) {
                    overInteractive = true;
                    break;
                }
            }
        }
        SetCursor(LoadCursor(nullptr, overInteractive ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    case WM_MOUSEMOVE: {
        m_mouseX = static_cast<float>(GET_X_LPARAM(lParam)) / m_dpiScale;
        m_mouseY = static_cast<float>(GET_Y_LPARAM(lParam)) / m_dpiScale;

        if (m_draggingScrollbar && m_maxCodeScrollY > 0.5f && m_scrollbarTrackRect.height > 10.0f) {
            float ratio = std::clamp((m_mouseY - m_scrollbarTrackRect.y) / m_scrollbarTrackRect.height, 0.0f, 1.0f);
            m_codeScrollY = m_targetCodeScrollY = ratio * m_maxCodeScrollY;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        bool changed = false;
        for (auto& hs : m_hotspots) {
            bool h = (hs.id >= ACT_HIST_ITEM_BASE)
                ? (m_sidebarListRect.contains(m_mouseX, m_mouseY) && hs.bounds.contains(m_mouseX, m_mouseY))
                : hs.bounds.contains(m_mouseX, m_mouseY);
            if (hs.hovered != h) {
                hs.hovered = h;
                changed = true;
            }
        }

        int newHoveredLine = -1;
        if (m_codeViewportRect.contains(m_mouseX, m_mouseY) && !m_cachedLines.empty()) {
            float relY = (m_mouseY - (m_codeViewportRect.y + 11.0f)) + m_codeScrollY;
            int idx = static_cast<int>(std::floor(relY / 22.0f));
            if (idx >= 0 && idx < static_cast<int>(m_cachedLines.size())) {
                newHoveredLine = idx;
            }
        }
        if (newHoveredLine != m_hoveredCodeLine) {
            m_hoveredCodeLine = newHoveredLine;
            changed = true;
        }

        if (changed) {
            StartAnimationTimer();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (m_draggingScrollbar) {
            m_draggingScrollbar = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        float mx = static_cast<float>(GET_X_LPARAM(lParam)) / m_dpiScale;
        float my = static_cast<float>(GET_Y_LPARAM(lParam)) / m_dpiScale;
        m_mouseX = mx;
        m_mouseY = my;

        if (m_maxCodeScrollY > 0.5f && m_scrollbarTrackRect.contains(mx, my)) {
            m_draggingScrollbar = true;
            SetCapture(hwnd);
            float ratio = std::clamp((my - m_scrollbarTrackRect.y) / m_scrollbarTrackRect.height, 0.0f, 1.0f);
            m_codeScrollY = m_targetCodeScrollY = ratio * m_maxCodeScrollY;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        for (const auto& hs : m_hotspots) {
            bool inSidebarScroll = (hs.id >= ACT_HIST_ITEM_BASE) || (hs.id >= ACT_TOGGLE_CODE && hs.id <= ACT_TOGGLE_HEATMAP);
            bool hit = inSidebarScroll
                ? (m_sidebarListRect.contains(mx, my) && hs.bounds.contains(mx, my))
                : hs.bounds.contains(mx, my);
            if (!hit) continue;

            if (hs.id == ACT_TAB_HISTORY) {
                m_activeTab = SidebarTab::History;
                m_listScrollY = m_targetListScrollY = 0.0f;
                StartAnimationTimer();
            } else if (hs.id == ACT_TAB_SETTINGS) {
                m_activeTab = SidebarTab::Settings;
                m_listScrollY = m_targetListScrollY = 0.0f;
                StartAnimationTimer();
            } else if (hs.id == ACT_HERO_REGION) {
                ShowWindow(hwnd, SW_HIDE);
                Sleep(80);
                if (m_onStartCapture) m_onStartCapture(CaptureLaunchMode::RegionInteractive);
            } else if (hs.id == ACT_HERO_FULLSCREEN) {
                ShowWindow(hwnd, SW_HIDE);
                Sleep(80);
                if (m_onStartCapture) m_onStartCapture(CaptureLaunchMode::FullScreenSelect);
            } else if (hs.id == ACT_HERO_QUICKSNIP) {
                ShowWindow(hwnd, SW_HIDE);
                Sleep(80);
                if (m_onStartCapture) m_onStartCapture(CaptureLaunchMode::QuickSnipCopy);
            } else if (hs.id == ACT_HERO_STITCH) {
                ShowWindow(hwnd, SW_HIDE);
                Sleep(80);
                if (m_onStartCapture) m_onStartCapture(CaptureLaunchMode::StitchAppend);
            } else if (hs.id == ACT_TOGGLE_CODE && m_settings) {
                m_settings->codeIndentationMode = !m_settings->codeIndentationMode;
                AppStorage::SaveSettings(*m_settings);
                StartAnimationTimer();
            } else if (hs.id == ACT_TOGGLE_LINEMODE && m_settings) {
                m_settings->lineSelectionMode = !m_settings->lineSelectionMode;
                AppStorage::SaveSettings(*m_settings);
                StartAnimationTimer();
            } else if (hs.id == ACT_TOGGLE_AUTOCOPY && m_settings) {
                m_settings->autoCopyOnRelease = !m_settings->autoCopyOnRelease;
                AppStorage::SaveSettings(*m_settings);
                StartAnimationTimer();
            } else if (hs.id == ACT_TOGGLE_UPSCALE && m_settings) {
                m_settings->upscaleForOcr = !m_settings->upscaleForOcr;
                AppStorage::SaveSettings(*m_settings);
                StartAnimationTimer();
            } else if (hs.id == ACT_TOGGLE_AUTOCLOSE && m_settings) {
                m_settings->autoCloseAfterCopy = !m_settings->autoCloseAfterCopy;
                AppStorage::SaveSettings(*m_settings);
                StartAnimationTimer();
            } else if (hs.id == ACT_TOGGLE_STARTWIN && m_settings) {
                bool target = !m_settings->startWithWindows;
                if (AppStorage::SetStartWithWindows(target)) {
                    m_settings->startWithWindows = target;
                    AppStorage::SaveSettings(*m_settings);
                    ShowToast(target ? L"Enabled Start with Windows (Tray)." : L"Disabled Start with Windows.");
                } else {
                    ShowToast(L"Could not update Windows startup registry key.", true);
                }
                StartAnimationTimer();
            } else if (hs.id == ACT_TOGGLE_GUTTER && m_settings) {
                m_settings->stripLineNumbers = !m_settings->stripLineNumbers;
                AppStorage::SaveSettings(*m_settings);
                ShowToast(m_settings->stripLineNumbers ? L"Auto Line-Number Gutter Stripping enabled." : L"Line-Number Gutter Stripping disabled.");
                StartAnimationTimer();
            } else if (hs.id == ACT_TOGGLE_HEATMAP && m_settings) {
                m_settings->showConfidenceHeatmap = !m_settings->showConfidenceHeatmap;
                AppStorage::SaveSettings(*m_settings);
                ShowToast(m_settings->showConfidenceHeatmap ? L"Confidence Heatmap enabled." : L"Confidence Heatmap hidden.");
                StartAnimationTimer();
            } else if (hs.id == ACT_BTN_PASTE_IMG) {
                RunOcrFromClipboard();
            } else if (hs.id == ACT_BTN_DEMO_OCR) {
                RunBuiltInDemoCapture();
            } else if (hs.id == ACT_BTN_COPY_CODE) {
                if (!m_history.empty() && m_selectedHistoryIdx >= 0 && static_cast<size_t>(m_selectedHistoryIdx) < m_history.size()) {
                    OverlayWindow::CopyTextToClipboard(hwnd, m_history[m_selectedHistoryIdx].text);
                    ShowToast(L"Copied snippet to Clipboard!");
                }
            } else if (hs.id == ACT_BTN_SAVE_FILE) {
                SaveSelectedCaptureToFile();
            } else if (hs.id == ACT_BTN_CLEAR_HIST) {
                m_history.clear();
                m_selectedHistoryIdx = 0;
                m_codeScrollY = m_targetCodeScrollY = 0.0f;
                RebuildSelectedCodeCache();
                AppStorage::SaveHistory(m_history);
                ShowToast(L"Cleared capture history.");
            } else if (hs.id >= ACT_HIST_ITEM_BASE) {
                m_selectedHistoryIdx = hs.id - ACT_HIST_ITEM_BASE;
                m_codeScrollY = m_targetCodeScrollY = 0.0f;
                RebuildSelectedCodeCache();
                StartAnimationTimer();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
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
