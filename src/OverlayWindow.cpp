#include "OverlayWindow.h"
#include "AppStorage.h"
#include "OcrService.h"
#include "TextFormatter.h"
#include "UiTheme.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace ScreenTextify {

namespace {
constexpr wchar_t OVERLAY_CLASS_NAME[] = L"ScreenTextifyOverlayWnd";
constexpr UINT_PTR TIMER_TOAST_ID = 1001;
constexpr UINT_PTR TIMER_AUTOCLOSE_ID = 1002;

enum HudButtonId {
    BTN_CODE_MODE     = 1,
    BTN_LINE_MODE     = 2,
    BTN_STRIP_GUTTER  = 3,
    BTN_COPY_ALL      = 4,
    BTN_STITCH_APPEND = 5,
    BTN_NEW_CROP      = 6,
    BTN_CLOSE         = 7
};
} // namespace

OverlayWindow::OverlayWindow(HINSTANCE hInstance, AppSettings* settings, OnTextCopiedCallback onCopied)
    : m_hInstance(hInstance), m_settings(settings), m_onCopied(std::move(onCopied)) {}

OverlayWindow::~OverlayWindow() {
    DiscardD2DResources();
    if (m_hudTextFormat) m_hudTextFormat->Release();
    if (m_bannerTextFormat) m_bannerTextFormat->Release();
    if (m_monoPreviewFormat) m_monoPreviewFormat->Release();
    if (m_dwriteFactory) m_dwriteFactory->Release();
    if (m_d2dFactory) m_d2dFactory->Release();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

bool OverlayWindow::Initialize() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_d2dFactory);
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&m_dwriteFactory)
    );
    if (FAILED(hr)) return false;

    m_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"en-us", &m_hudTextFormat
    );
    if (m_hudTextFormat) {
        m_hudTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_hudTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    m_dwriteFactory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.5f, L"en-us", &m_bannerTextFormat
    );
    if (m_bannerTextFormat) {
        m_bannerTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_bannerTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    m_dwriteFactory->CreateTextFormat(
        L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.5f, L"en-us", &m_monoPreviewFormat
    );
    if (m_monoPreviewFormat) {
        m_monoPreviewFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = OverlayWindow::WndProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = OVERLAY_CLASS_NAME;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS_NAME,
        L"ScreenTextify Overlay",
        WS_POPUP,
        0, 0, 100, 100,
        nullptr, nullptr, m_hInstance, this
    );

    return m_hwnd != nullptr;
}

void OverlayWindow::StartCaptureSession(CaptureLaunchMode mode) {
    if (!m_hwnd) return;
    m_launchMode = mode;

    if (m_phase != OverlayPhase::Hidden) {
        ShowWindow(m_hwnd, SW_HIDE);
        Sleep(35);
    }

    m_frozenScreen = ScreenCapture::CaptureVirtualScreen();
    if (!m_frozenScreen.valid()) return;

    DiscardD2DResources();

    m_ocrDoc = OcrDocument{};
    m_ocrError.clear();
    m_isDragging = false;
    m_toastExpireTick = 0;
    m_toastTitle.clear();
    m_toastPreview.clear();

    SetWindowPos(
        m_hwnd, HWND_TOPMOST,
        m_frozenScreen.screenOriginX,
        m_frozenScreen.screenOriginY,
        m_frozenScreen.width,
        m_frozenScreen.height,
        SWP_SHOWWINDOW
    );

    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);

    if (mode == CaptureLaunchMode::FullScreenSelect) {
        ScanActiveMonitorFull();
    } else {
        m_phase = OverlayPhase::SelectingRegion;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void OverlayWindow::CloseOverlay() {
    if (!m_hwnd) return;
    KillTimer(m_hwnd, TIMER_TOAST_ID);
    KillTimer(m_hwnd, TIMER_AUTOCLOSE_ID);
    ReleaseCapture();
    m_isDragging = false;
    m_phase = OverlayPhase::Hidden;
    ShowWindow(m_hwnd, SW_HIDE);
}

void OverlayWindow::ScanActiveMonitorFull() {
    POINT pt = {};
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    RectF region = {
        0.0f, 0.0f,
        static_cast<float>(m_frozenScreen.width),
        static_cast<float>(m_frozenScreen.height)
    };
    if (GetMonitorInfoW(hMon, &mi)) {
        region.x = static_cast<float>(mi.rcMonitor.left - m_frozenScreen.screenOriginX);
        region.y = static_cast<float>(mi.rcMonitor.top - m_frozenScreen.screenOriginY);
        region.width = static_cast<float>(mi.rcMonitor.right - mi.rcMonitor.left);
        region.height = static_cast<float>(mi.rcMonitor.bottom - mi.rcMonitor.top);
    }
    RunOcrOnRegion(region);
}

void OverlayWindow::RunOcrOnRegion(const RectF& localRegion) {
    RectF norm = localRegion.normalized();
    if (norm.width < 12.0f || norm.height < 10.0f) {
        return;
    }

    SetCursor(LoadCursor(nullptr, IDC_WAIT));

    // Smart Edge Auto-Padding: if the user's crop box grazed ink on any edge, expand slightly
    // (up to 10px) so clipped edge punctuation like ';', '{', '}' is never sliced off.
    RectF smartRegion = ScreenCapture::SmartExpandCropRegion(m_frozenScreen, norm, 10);
    m_activeScanRegion = smartRegion;
    m_ocrError.clear();

    int scale = 1;
    if (m_settings && m_settings->upscaleForOcr) {
        if (smartRegion.width < 320.0f && smartRegion.height < 120.0f) {
            scale = 3;
        } else if (smartRegion.width < 1500.0f && smartRegion.height < 950.0f) {
            scale = 2;
        }
    }

    RawBitmap cropped = ScreenCapture::CropAndScale(m_frozenScreen, smartRegion, scale);
    m_ocrDoc = OcrService::RecognizeBitmap(cropped, &m_ocrError);
    m_gutterWordCount = TextFormatter::MarkLineNumberGutter(m_ocrDoc);
    m_uncertainWordCount = m_ocrDoc.uncertainWordCount();

    size_t codeWords = m_ocrDoc.totalWords() - ((m_settings && m_settings->stripLineNumbers) ? m_gutterWordCount : 0);
    std::wstringstream msStream;
    msStream << codeWords << L"w \x2022 "
             << std::fixed << std::setprecision(0) << m_ocrDoc.recognitionTimeMs << L"ms";
    if (m_uncertainWordCount > 0 && (!m_settings || m_settings->showConfidenceHeatmap)) {
        msStream << L" \x2022 \x26A0 " << m_uncertainWordCount;
    }
    m_hudMetricsText = msStream.str();

    m_phase = OverlayPhase::InteractiveText;
    UpdateHudLayout();

    // If launched via QuickSnipCopy (Alt+Shift+C) or StitchAppend (Alt+Shift+A), act immediately!
    if ((m_launchMode == CaptureLaunchMode::QuickSnipCopy || m_launchMode == CaptureLaunchMode::StitchAppend) && !m_ocrDoc.empty()) {
        bool stitch = (m_launchMode == CaptureLaunchMode::StitchAppend);
        CopyCurrentSelection(true, stitch);
        SetTimer(m_hwnd, TIMER_AUTOCLOSE_ID, stitch ? 700 : 520, nullptr);
    }

    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void OverlayWindow::UpdateHudLayout() {
    m_hudButtons.clear();

    float barWidth = 918.0f;
    float barHeight = 44.0f;

    float barX = m_activeScanRegion.centerX() - barWidth * 0.5f;
    barX = std::clamp(barX, 16.0f, std::max(16.0f, static_cast<float>(m_frozenScreen.width) - barWidth - 16.0f));

    float barY = m_activeScanRegion.y - barHeight - 12.0f;
    if (barY < 14.0f) {
        barY = m_activeScanRegion.bottom() + 12.0f;
        if (barY + barHeight > m_frozenScreen.height - 14.0f) {
            barY = m_activeScanRegion.y + 12.0f;
        }
    }

    m_hudBarRect = { barX, barY, barWidth, barHeight };

    bool codeOn = m_settings ? m_settings->codeIndentationMode : true;
    bool lineOn = m_settings ? m_settings->lineSelectionMode : false;
    bool gutterOn = m_settings ? m_settings->stripLineNumbers : true;

    struct BtnSpec {
        int id;
        std::wstring text;
        float width;
        bool active;
    };

    std::vector<BtnSpec> specs = {
        { BTN_CODE_MODE,     codeOn ? L"Code: ON (C)" : L"Code: OFF (C)", 114.0f, codeOn },
        { BTN_LINE_MODE,     lineOn ? L"Lines (L)" : L"Words (L)", 88.0f, lineOn },
        { BTN_STRIP_GUTTER,  gutterOn ? L"Gutter: Strip (G)" : L"Gutter: Keep (G)", 128.0f, gutterOn },
        { BTN_COPY_ALL,      L"Copy All (Ctrl+A)", 124.0f, false },
        { BTN_STITCH_APPEND, L"+ Stitch (A)", 98.0f, m_launchMode == CaptureLaunchMode::StitchAppend },
        { BTN_NEW_CROP,      L"Re-Crop (R)", 92.0f, false },
        { BTN_CLOSE,         L"Esc", 54.0f, false }
    };

    float curX = barX + 174.0f; // Leave 164px on the left for the live OCR metrics & warning pill
    for (const auto& s : specs) {
        HudButton btn;
        btn.id = s.id;
        btn.label = s.text;
        btn.bounds = { curX, barY + 6.0f, s.width, barHeight - 12.0f };
        btn.active = s.active;
        m_hudButtons.push_back(btn);
        curX += s.width + 6.0f;
    }
}

void OverlayWindow::UpdateSelectionFromDrag(const RectF& dragRect, bool additive) {
    RectF norm = dragRect.normalized();
    bool isClick = (norm.width < 4.0f && norm.height < 4.0f);
    bool lineMode = m_settings && m_settings->lineSelectionMode;
    bool stripGutter = !m_settings || m_settings->stripLineNumbers;

    for (auto& line : m_ocrDoc.lines) {
        bool lineHit = isClick ? line.bounds.contains(norm.x, norm.y) : line.bounds.intersects(norm);
        for (auto& word : line.words) {
            if (stripGutter && word.isLineNumber) {
                if (!additive) word.selected = false;
                continue;
            }
            bool hit = lineMode ? lineHit
                                : (isClick ? word.bounds.contains(norm.x, norm.y) : word.bounds.intersects(norm));
            if (hit) {
                word.selected = true;
            } else if (!additive) {
                word.selected = false;
            }
        }
    }
}

void OverlayWindow::CopyCurrentSelection(bool copyAllIfNoneSelected, bool stitchAppend) {
    if (m_ocrDoc.empty()) return;

    bool stripGutter = !m_settings || m_settings->stripLineNumbers;
    bool anySelected = false;
    for (const auto& line : m_ocrDoc.lines) {
        for (const auto& word : line.words) {
            if (word.selected && !(stripGutter && word.isLineNumber)) {
                anySelected = true;
                break;
            }
        }
        if (anySelected) break;
    }

    if (!anySelected && !copyAllIfNoneSelected) {
        return;
    }

    if (!anySelected && copyAllIfNoneSelected) {
        for (auto& line : m_ocrDoc.lines) {
            for (auto& word : line.words) {
                word.selected = !(stripGutter && word.isLineNumber);
            }
        }
    }

    bool codeMode = m_settings ? m_settings->codeIndentationMode : true;
    size_t wordCount = 0;
    std::wstring formatted = TextFormatter::FormatDocument(m_ocrDoc, true, codeMode, &wordCount, stripGutter);
    if (formatted.empty()) return;

    // Collect unique uncertain words from the copied selection for Studio Dashboard highlighting
    std::vector<std::wstring> uncertainWords;
    for (const auto& line : m_ocrDoc.lines) {
        for (const auto& word : line.words) {
            if (word.selected && word.isUncertain() && !word.text.empty()) {
                if (std::find(uncertainWords.begin(), uncertainWords.end(), word.text) == uncertainWords.end()) {
                    uncertainWords.push_back(word.text);
                }
            }
        }
    }

    std::wstring finalOutput = formatted;
    std::wstring title;

    if (stitchAppend) {
        std::wstring existing = ReadTextFromClipboard(m_hwnd);
        int overlapLines = 0;
        int appendedLines = 0;
        finalOutput = TextFormatter::StitchCodeBlocks(existing, formatted, &overlapLines, &appendedLines);

        std::wstringstream ws(finalOutput);
        std::wstring tok;
        size_t totalWords = 0;
        while (ws >> tok) ++totalWords;
        wordCount = totalWords;

        title = L"\x2713 Stitched +" + std::to_wstring(appendedLines) + L" lines";
        if (overlapLines > 0) {
            title += L" (" + std::to_wstring(overlapLines) + L" overlap merged)";
        }
        title += L" \x2022 Clipboard Updated";
    } else {
        title = L"\x2713 Copied " + std::to_wstring(wordCount) +
                (wordCount == 1 ? L" word" : L" words");
        if (stripGutter && m_gutterWordCount > 0) {
            title += L" \x2022 Stripped " + std::to_wstring(m_gutterWordCount) + L" line #s";
        }
        if (!uncertainWords.empty()) {
            title += L" \x2022 \x26A0 " + std::to_wstring(uncertainWords.size()) + L" uncertain";
        } else {
            title += codeMode ? L" \x2022 Code Mode" : L" \x2022 Standard Text";
        }
    }

    if (CopyTextToClipboard(m_hwnd, finalOutput)) {
        if (m_onCopied) {
            m_onCopied(finalOutput, codeMode, wordCount, m_ocrDoc.recognitionTimeMs, uncertainWords);
        }

        ShowToast(title, stitchAppend ? formatted : finalOutput);

        if (m_settings && m_settings->autoCloseAfterCopy && !stitchAppend) {
            SetTimer(m_hwnd, TIMER_AUTOCLOSE_ID, 480, nullptr);
        }
    }
}

std::wstring OverlayWindow::ReadTextFromClipboard(HWND hwndOwner) {
    std::wstring text;
    if (!OpenClipboard(hwndOwner)) return text;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HGLOBAL hMem = GetClipboardData(CF_UNICODETEXT);
        if (hMem) {
            const wchar_t* pText = static_cast<const wchar_t*>(GlobalLock(hMem));
            if (pText) {
                text = pText;
                GlobalUnlock(hMem);
            }
        }
    }
    CloseClipboard();
    return text;
}

void OverlayWindow::ShowToast(const std::wstring& message, const std::wstring& preview) {
    m_toastTitle = message;
    std::wstringstream ss(preview);
    std::wstring line;
    std::wstring clipped;
    int lineCount = 0;
    while (std::getline(ss, line) && lineCount < 6) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.length() > 78) line = line.substr(0, 75) + L"...";
        if (lineCount > 0) clipped += L"\n";
        clipped += line;
        ++lineCount;
    }
    m_toastPreview = clipped;
    m_toastExpireTick = GetTickCount64() + 2800;
    SetTimer(m_hwnd, TIMER_TOAST_ID, 2850, nullptr);
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool OverlayWindow::CopyTextToClipboard(HWND hwndOwner, const std::wstring& text) {
    if (!OpenClipboard(hwndOwner)) return false;
    EmptyClipboard();

    size_t byteCount = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!hMem) {
        CloseClipboard();
        return false;
    }

    void* pMem = GlobalLock(hMem);
    if (pMem) {
        memcpy(pMem, text.c_str(), byteCount);
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
    } else {
        GlobalFree(hMem);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

void OverlayWindow::CreateD2DResources() {
    if (m_renderTarget || !m_hwnd || !m_frozenScreen.valid()) return;

    D2D1_SIZE_U size = D2D1::SizeU(m_frozenScreen.width, m_frozenScreen.height);
    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = 96.0f;
    rtProps.dpiY = 96.0f;

    HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(
        rtProps,
        D2D1::HwndRenderTargetProperties(m_hwnd, size),
        &m_renderTarget
    );

    if (SUCCEEDED(hr) && m_renderTarget) {
        D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f
        );
        m_renderTarget->CreateBitmap(
            size,
            m_frozenScreen.pixels.data(),
            m_frozenScreen.stride,
            &bmpProps,
            &m_screenBitmap
        );
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &m_brush);
    }
}

void OverlayWindow::DiscardD2DResources() {
    if (m_brush) {
        m_brush->Release();
        m_brush = nullptr;
    }
    if (m_screenBitmap) {
        m_screenBitmap->Release();
        m_screenBitmap = nullptr;
    }
    if (m_renderTarget) {
        m_renderTarget->Release();
        m_renderTarget = nullptr;
    }
}

void OverlayWindow::OnPaint() {
    CreateD2DResources();
    if (!m_renderTarget || !m_screenBitmap || !m_brush) return;

    m_renderTarget->BeginDraw();

    float fullW = static_cast<float>(m_frozenScreen.width);
    float fullH = static_cast<float>(m_frozenScreen.height);
    D2D1_RECT_F fullRect = D2D1::RectF(0.0f, 0.0f, fullW, fullH);

    m_renderTarget->DrawBitmap(m_screenBitmap, fullRect);

    if (m_phase == OverlayPhase::SelectingRegion) {
        m_brush->SetColor(D2D1::ColorF(0.03f, 0.05f, 0.10f, 0.56f));
        m_renderTarget->FillRectangle(fullRect, m_brush);

        if (m_isDragging) {
            RectF sel = RectF{ m_dragStartX, m_dragStartY, m_mouseX - m_dragStartX, m_mouseY - m_dragStartY }.normalized();
            D2D1_RECT_F d2dSel = D2D1::RectF(sel.x, sel.y, sel.right(), sel.bottom());

            m_renderTarget->DrawBitmap(m_screenBitmap, d2dSel, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, d2dSel);

            m_brush->SetColor(D2D1::ColorF(0.22f, 0.74f, 0.97f, 0.98f));
            m_renderTarget->DrawRectangle(d2dSel, m_brush, 2.0f);

            // Corner handles
            float ch = 12.0f;
            m_brush->SetColor(D2D1::ColorF(0.38f, 0.85f, 1.0f, 1.0f));
            m_renderTarget->DrawLine({ sel.x, sel.y }, { sel.x + ch, sel.y }, m_brush, 3.5f);
            m_renderTarget->DrawLine({ sel.x, sel.y }, { sel.x, sel.y + ch }, m_brush, 3.5f);
            m_renderTarget->DrawLine({ sel.right() - ch, sel.y }, { sel.right(), sel.y }, m_brush, 3.5f);
            m_renderTarget->DrawLine({ sel.right(), sel.y }, { sel.right(), sel.y + ch }, m_brush, 3.5f);

            std::wstring dimText = std::to_wstring(static_cast<int>(sel.width)) + L" \x00D7 " + std::to_wstring(static_cast<int>(sel.height)) + L" px";
            D2D1_RECT_F badgeRect = D2D1::RectF(sel.x, std::max(4.0f, sel.y - 28.0f), sel.x + 120.0f, std::max(28.0f, sel.y - 4.0f));
            m_brush->SetColor(D2D1::ColorF(0.05f, 0.08f, 0.15f, 0.92f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, 6.0f, 6.0f), m_brush);
            m_brush->SetColor(D2D1::ColorF(0.45f, 0.85f, 1.0f, 1.0f));
            if (m_hudTextFormat) {
                m_renderTarget->DrawTextW(dimText.c_str(), static_cast<UINT32>(dimText.length()), m_hudTextFormat, badgeRect, m_brush);
            }
        }

        float bannerW = 720.0f;
        float bannerH = 46.0f;
        float bx = (fullW - bannerW) * 0.5f;
        D2D1_RECT_F bannerRect = D2D1::RectF(bx, 24.0f, bx + bannerW, 24.0f + bannerH);
        m_brush->SetColor(D2D1::ColorF(0.05f, 0.08f, 0.15f, 0.94f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(bannerRect, 12.0f, 12.0f), m_brush);
        m_brush->SetColor(D2D1::ColorF(0.22f, 0.74f, 0.97f, 0.85f));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(bannerRect, 12.0f, 12.0f), m_brush, 1.5f);

        std::wstring prompt;
        if (m_launchMode == CaptureLaunchMode::QuickSnipCopy) {
            prompt = L"Quick Snip Mode: Drag over any video/image to immediately copy all text   \x2022   [Esc] Cancel";
        } else if (m_launchMode == CaptureLaunchMode::StitchAppend) {
            prompt = L"Stitch & Append Mode: Drag over scrolled code to append & merge overlaps   \x2022   [Esc] Cancel";
        } else {
            prompt = L"Selectext Mode: Drag over video/image to detect words   \x2022   [Space / F] Full Screen   \x2022   [Esc] Cancel";
        }
        m_brush->SetColor(D2D1::ColorF(0.95f, 0.98f, 1.0f, 1.0f));
        if (m_bannerTextFormat) {
            m_renderTarget->DrawTextW(prompt.c_str(), static_cast<UINT32>(prompt.length()), m_bannerTextFormat, bannerRect, m_brush);
        }
    } else if (m_phase == OverlayPhase::InteractiveText) {
        RectF sr = m_activeScanRegion;
        m_brush->SetColor(D2D1::ColorF(0.03f, 0.05f, 0.10f, 0.60f));
        if (sr.y > 0) m_renderTarget->FillRectangle(D2D1::RectF(0, 0, fullW, sr.y), m_brush);
        if (sr.bottom() < fullH) m_renderTarget->FillRectangle(D2D1::RectF(0, sr.bottom(), fullW, fullH), m_brush);
        if (sr.x > 0) m_renderTarget->FillRectangle(D2D1::RectF(0, sr.y, sr.x, sr.bottom()), m_brush);
        if (sr.right() < fullW) m_renderTarget->FillRectangle(D2D1::RectF(sr.right(), sr.y, fullW, sr.bottom()), m_brush);

        D2D1_RECT_F srRect = D2D1::RectF(sr.x, sr.y, sr.right(), sr.bottom());
        m_brush->SetColor(D2D1::ColorF(0.22f, 0.74f, 0.97f, 0.75f));
        m_renderTarget->DrawRectangle(srRect, m_brush, 1.6f);

        // Draw Selectext interactive word boxes with Confidence Heatmap & Auto-Stripped Gutter styling
        bool stripGutter = !m_settings || m_settings->stripLineNumbers;
        bool showHeatmap = !m_settings || m_settings->showConfidenceHeatmap;
        const OcrWordBox* hoveredWord = nullptr;

        for (const auto& line : m_ocrDoc.lines) {
            for (const auto& word : line.words) {
                if (word.hovered) hoveredWord = &word;

                D2D1_RECT_F wr = D2D1::RectF(
                    word.bounds.x - 2.0f,
                    word.bounds.y - 1.5f,
                    word.bounds.right() + 2.0f,
                    word.bounds.bottom() + 1.5f
                );
                D2D1_ROUNDED_RECT rrect = D2D1::RoundedRect(wr, 4.0f, 4.0f);

                if (stripGutter && word.isLineNumber) {
                    // Subtle muted badge showing the editor line-number gutter is auto-stripped
                    m_brush->SetColor(D2D1::ColorF(0.06f, 0.08f, 0.13f, 0.28f));
                    m_renderTarget->FillRoundedRectangle(rrect, m_brush);
                    m_brush->SetColor(D2D1::ColorF(0.45f, 0.53f, 0.65f, 0.48f));
                    m_renderTarget->DrawRoundedRectangle(rrect, m_brush, 1.0f);
                    continue;
                }

                bool uncertain = showHeatmap && word.isUncertain();
                bool veryLow = showHeatmap && word.isVeryLowConfidence();
                D2D1_COLOR_F warnCol = veryLow
                    ? D2D1::ColorF(0.976f, 0.451f, 0.086f, 1.0f)   // #F97316 Orange-Amber (high risk)
                    : UiTheme::AccentAmber;                        // #FBBF24 Warm Amber (uncertain)

                if (word.selected) {
                    m_brush->SetColor(uncertain
                        ? D2D1::ColorF(0.16f, 0.46f, 0.92f, 0.42f)
                        : D2D1::ColorF(0.14f, 0.48f, 0.98f, 0.48f));
                    m_renderTarget->FillRoundedRectangle(rrect, m_brush);
                    m_brush->SetColor(uncertain ? warnCol : D2D1::ColorF(0.42f, 0.78f, 1.0f, 1.0f));
                    m_renderTarget->DrawRoundedRectangle(rrect, m_brush, uncertain ? 1.9f : 1.7f);
                } else if (word.hovered) {
                    m_brush->SetColor(uncertain
                        ? UiTheme::WithAlpha(warnCol, 0.34f)
                        : D2D1::ColorF(0.08f, 0.65f, 0.92f, 0.36f));
                    m_renderTarget->FillRoundedRectangle(rrect, m_brush);
                    m_brush->SetColor(uncertain ? warnCol : D2D1::ColorF(0.28f, 0.82f, 1.0f, 0.95f));
                    m_renderTarget->DrawRoundedRectangle(rrect, m_brush, 1.65f);
                } else if (uncertain) {
                    // Amber Confidence Heatmap warning box + crisp bottom warning accent bar
                    m_brush->SetColor(UiTheme::WithAlpha(warnCol, veryLow ? 0.28f : 0.20f));
                    m_renderTarget->FillRoundedRectangle(rrect, m_brush);
                    m_brush->SetColor(UiTheme::WithAlpha(warnCol, 0.92f));
                    m_renderTarget->DrawRoundedRectangle(rrect, m_brush, 1.4f);
                    m_renderTarget->DrawLine({ wr.left + 3.0f, wr.bottom - 0.5f }, { wr.right - 3.0f, wr.bottom - 0.5f }, m_brush, 2.0f);
                } else {
                    m_brush->SetColor(D2D1::ColorF(0.06f, 0.12f, 0.25f, 0.28f));
                    m_renderTarget->FillRoundedRectangle(rrect, m_brush);
                    m_brush->SetColor(D2D1::ColorF(0.22f, 0.74f, 0.97f, 0.52f));
                    m_renderTarget->DrawRoundedRectangle(rrect, m_brush, 1.0f);
                }
            }
        }

        // Floating Hover Confidence Tooltip Pill directly above hovered word (shows recognized text + %)
        if (hoveredWord && !m_isDragging && m_hudTextFormat) {
            int confPct = std::clamp(static_cast<int>(std::round(hoveredWord->minCharConf * 100.0f)), 0, 100);
            std::wstring tipText;
            D2D1_COLOR_F tipAccent = UiTheme::AccentEmerald;
            if (stripGutter && hoveredWord->isLineNumber) {
                tipText = L"Line #" + hoveredWord->text + L" (auto-stripped)";
                tipAccent = UiTheme::TextSecondary;
            } else if (hoveredWord->isUncertain()) {
                tipText = L"\x26A0 \"" + hoveredWord->text + L"\"  \x2022  " + std::to_wstring(confPct) + L"%";
                tipAccent = hoveredWord->isVeryLowConfidence()
                    ? D2D1::ColorF(0.976f, 0.451f, 0.086f, 1.0f)
                    : UiTheme::AccentAmber;
            } else {
                tipText = L"\"" + hoveredWord->text + L"\"  \x2022  " + std::to_wstring(confPct) + L"%";
                tipAccent = UiTheme::AccentEmerald;
            }

            float tipW = std::clamp(static_cast<float>(tipText.size()) * 7.6f + 26.0f, 110.0f, 380.0f);
            float tipH = 26.0f;
            float tipX = std::clamp(hoveredWord->bounds.centerX() - tipW * 0.5f, 8.0f, std::max(8.0f, fullW - tipW - 8.0f));
            float tipY = hoveredWord->bounds.y - tipH - 7.0f;
            if (tipY < 8.0f) tipY = hoveredWord->bounds.bottom() + 7.0f;

            D2D1_RECT_F tipRect = D2D1::RectF(tipX, tipY, tipX + tipW, tipY + tipH);
            UiTheme::DrawSoftShadow(m_renderTarget, m_brush, tipRect, 6.0f, 0.8f);
            m_brush->SetColor(D2D1::ColorF(0.043f, 0.063f, 0.105f, 0.96f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tipRect, 6.0f, 6.0f), m_brush);
            m_brush->SetColor(UiTheme::WithAlpha(tipAccent, 0.90f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(tipRect, 6.0f, 6.0f), m_brush, 1.25f);
            m_brush->SetColor(tipAccent);
            m_renderTarget->DrawTextW(tipText.c_str(), static_cast<UINT32>(tipText.length()), m_hudTextFormat, tipRect, m_brush);
        }

        if (m_isDragging) {
            RectF lasso = RectF{ m_dragStartX, m_dragStartY, m_mouseX - m_dragStartX, m_mouseY - m_dragStartY }.normalized();
            D2D1_RECT_F lr = D2D1::RectF(lasso.x, lasso.y, lasso.right(), lasso.bottom());
            m_brush->SetColor(D2D1::ColorF(0.22f, 0.65f, 0.98f, 0.18f));
            m_renderTarget->FillRectangle(lr, m_brush);
            m_brush->SetColor(D2D1::ColorF(0.38f, 0.78f, 1.0f, 0.92f));
            m_renderTarget->DrawRectangle(lr, m_brush, 1.5f);
        }

        if (m_ocrDoc.empty()) {
            std::wstring msg = m_ocrError.empty()
                ? L"No text detected in this area. Press [R] to crop another region or [F] for Full Screen."
                : m_ocrError;
            D2D1_RECT_F emptyBox = D2D1::RectF(sr.centerX() - 280.0f, sr.centerY() - 24.0f, sr.centerX() + 280.0f, sr.centerY() + 24.0f);
            m_brush->SetColor(D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.94f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(emptyBox, 8.0f, 8.0f), m_brush);
            m_brush->SetColor(D2D1::ColorF(0.98f, 0.75f, 0.25f, 1.0f));
            if (m_hudTextFormat) {
                m_renderTarget->DrawTextW(msg.c_str(), static_cast<UINT32>(msg.length()), m_hudTextFormat, emptyBox, m_brush);
            }
        }

        // Draw Floating HUD Toolbar with soft drop shadow & specular top edge
        D2D1_RECT_F hudRect = D2D1::RectF(m_hudBarRect.x, m_hudBarRect.y, m_hudBarRect.right(), m_hudBarRect.bottom());
        UiTheme::DrawSoftShadow(m_renderTarget, m_brush, hudRect, 11.0f, 1.0f);
        m_brush->SetColor(D2D1::ColorF(0.047f, 0.067f, 0.114f, 0.96f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(hudRect, 11.0f, 11.0f), m_brush);
        m_brush->SetColor(UiTheme::TopSpecular);
        m_renderTarget->DrawLine({ hudRect.left + 12.0f, hudRect.top + 0.75f }, { hudRect.right - 12.0f, hudRect.top + 0.75f }, m_brush, 1.0f);
        m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.48f));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(hudRect, 11.0f, 11.0f), m_brush, 1.25f);

        // Metrics pill on left of HUD (turns warm Amber if uncertain words are flagged)
        D2D1_RECT_F metricsRect = D2D1::RectF(m_hudBarRect.x + 8.0f, m_hudBarRect.y + 6.0f, m_hudBarRect.x + 168.0f, m_hudBarRect.bottom() - 6.0f);
        bool hasUncertain = (m_uncertainWordCount > 0 && (!m_settings || m_settings->showConfidenceHeatmap));
        m_brush->SetColor(hasUncertain ? D2D1::ColorF(0.14f, 0.10f, 0.04f, 0.95f) : UiTheme::BgCanvas);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(metricsRect, 7.0f, 7.0f), m_brush);
        m_brush->SetColor(hasUncertain ? UiTheme::WithAlpha(UiTheme::AccentAmber, 0.70f) : UiTheme::BorderSubtle);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(metricsRect, 7.0f, 7.0f), m_brush, 1.0f);
        m_brush->SetColor(hasUncertain ? UiTheme::AccentAmber : UiTheme::AccentPrimary);
        if (m_hudTextFormat) {
            m_renderTarget->DrawTextW(m_hudMetricsText.c_str(), static_cast<UINT32>(m_hudMetricsText.length()), m_hudTextFormat, metricsRect, m_brush);
        }

        for (const auto& btn : m_hudButtons) {
            D2D1_RECT_F br = D2D1::RectF(btn.bounds.x, btn.bounds.y, btn.bounds.right(), btn.bounds.bottom());
            bool isStitchBtn = (btn.id == BTN_STITCH_APPEND);
            if (btn.active) {
                m_brush->SetColor(isStitchBtn ? D2D1::ColorF(0.08f, 0.56f, 0.42f, 0.94f) : D2D1::ColorF(0.12f, 0.42f, 0.90f, 0.94f));
            } else if (btn.hovered) {
                m_brush->SetColor(UiTheme::BgSurfaceHover);
            } else {
                m_brush->SetColor(UiTheme::BgSurface);
            }
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(br, 7.0f, 7.0f), m_brush);

            m_brush->SetColor(btn.active
                ? (isStitchBtn ? UiTheme::AccentEmerald : UiTheme::AccentPrimary)
                : (btn.hovered ? UiTheme::WithAlpha(UiTheme::AccentPrimary, 0.55f) : UiTheme::BorderSubtle));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(br, 7.0f, 7.0f), m_brush, 1.0f);

            m_brush->SetColor(UiTheme::TextPrimary);
            if (m_hudTextFormat) {
                m_renderTarget->DrawTextW(btn.label.c_str(), static_cast<UINT32>(btn.label.length()), m_hudTextFormat, br, m_brush);
            }
        }

        // Draw Copied Toast & Live Code Preview Card with drop shadow
        if (m_toastExpireTick > GetTickCount64() && !m_toastTitle.empty()) {
            float toastW = 580.0f;
            float toastH = m_toastPreview.empty() ? 48.0f : 154.0f;
            float tx = std::clamp(m_activeScanRegion.centerX() - toastW * 0.5f, 20.0f, std::max(20.0f, fullW - toastW - 20.0f));
            float ty = std::min(fullH - toastH - 24.0f, m_activeScanRegion.bottom() + 62.0f);
            if (ty < 64.0f) ty = 64.0f;

            D2D1_RECT_F toastRect = D2D1::RectF(tx, ty, tx + toastW, ty + toastH);
            UiTheme::DrawSoftShadow(m_renderTarget, m_brush, toastRect, 12.0f, 1.2f);
            m_brush->SetColor(D2D1::ColorF(0.043f, 0.067f, 0.110f, 0.97f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(toastRect, 12.0f, 12.0f), m_brush);
            m_brush->SetColor(UiTheme::WithAlpha(UiTheme::AccentEmerald, 0.90f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(toastRect, 12.0f, 12.0f), m_brush, 1.6f);

            D2D1_RECT_F titleRect = D2D1::RectF(tx + 14.0f, ty + 8.0f, tx + toastW - 14.0f, ty + 34.0f);
            m_brush->SetColor(D2D1::ColorF(0.32f, 0.96f, 0.70f, 1.0f));
            if (m_hudTextFormat) {
                m_renderTarget->DrawTextW(m_toastTitle.c_str(), static_cast<UINT32>(m_toastTitle.length()), m_hudTextFormat, titleRect, m_brush);
            }

            if (!m_toastPreview.empty() && m_monoPreviewFormat) {
                D2D1_RECT_F codeRect = D2D1::RectF(tx + 16.0f, ty + 38.0f, tx + toastW - 16.0f, ty + toastH - 10.0f);
                m_renderTarget->PushAxisAlignedClip(codeRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                m_brush->SetColor(D2D1::ColorF(0.86f, 0.92f, 0.99f, 0.96f));
                m_renderTarget->DrawTextW(m_toastPreview.c_str(), static_cast<UINT32>(m_toastPreview.length()), m_monoPreviewFormat, codeRect, m_brush);
                m_renderTarget->PopAxisAlignedClip();
            }
        }
    }

    HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardD2DResources();
    }
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(cs->lpCreateParams);
        if (self) {
            self->m_hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    } else {
        self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT OverlayWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        OnPaint();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR: {
        if (m_phase == OverlayPhase::SelectingRegion) {
            SetCursor(LoadCursor(nullptr, IDC_CROSS));
            return TRUE;
        } else if (m_phase == OverlayPhase::InteractiveText) {
            bool overButton = false;
            for (const auto& b : m_hudButtons) {
                if (b.hovered) {
                    overButton = true;
                    break;
                }
            }
            SetCursor(LoadCursor(nullptr, overButton ? IDC_HAND : IDC_IBEAM));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));
        m_mouseX = mx;
        m_mouseY = my;

        if (m_phase == OverlayPhase::InteractiveText) {
            for (const auto& btn : m_hudButtons) {
                if (btn.bounds.contains(mx, my)) {
                    switch (btn.id) {
                    case BTN_CODE_MODE:
                        if (m_settings) {
                            m_settings->codeIndentationMode = !m_settings->codeIndentationMode;
                            AppStorage::SaveSettings(*m_settings);
                        }
                        UpdateHudLayout();
                        CopyCurrentSelection(false);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    case BTN_LINE_MODE:
                        if (m_settings) {
                            m_settings->lineSelectionMode = !m_settings->lineSelectionMode;
                            AppStorage::SaveSettings(*m_settings);
                        }
                        UpdateHudLayout();
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    case BTN_STRIP_GUTTER:
                        if (m_settings) {
                            m_settings->stripLineNumbers = !m_settings->stripLineNumbers;
                            AppStorage::SaveSettings(*m_settings);
                        }
                        UpdateHudLayout();
                        CopyCurrentSelection(false);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    case BTN_COPY_ALL:
                        CopyCurrentSelection(true, false);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    case BTN_STITCH_APPEND:
                        CopyCurrentSelection(true, true);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    case BTN_NEW_CROP:
                        m_phase = OverlayPhase::SelectingRegion;
                        m_isDragging = false;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        break;
                    case BTN_CLOSE:
                        CloseOverlay();
                        break;
                    }
                    return 0;
                }
            }
        }

        m_isDragging = true;
        m_dragStartX = mx;
        m_dragStartY = my;
        m_dragAdditive = (GetKeyState(VK_SHIFT) & 0x8000) != 0 || (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        SetCapture(hwnd);

        if (m_phase == OverlayPhase::InteractiveText) {
            UpdateSelectionFromDrag({ mx, my, 1.0f, 1.0f }, m_dragAdditive);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));
        m_mouseX = mx;
        m_mouseY = my;

        if (m_phase == OverlayPhase::SelectingRegion) {
            if (m_isDragging) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (m_phase == OverlayPhase::InteractiveText) {
            bool changed = false;
            for (auto& btn : m_hudButtons) {
                bool h = btn.bounds.contains(mx, my);
                if (btn.hovered != h) {
                    btn.hovered = h;
                    changed = true;
                }
            }

            if (m_isDragging) {
                RectF dragRect = { m_dragStartX, m_dragStartY, mx - m_dragStartX, my - m_dragStartY };
                UpdateSelectionFromDrag(dragRect, m_dragAdditive);
                changed = true;
            } else {
                bool lineMode = m_settings && m_settings->lineSelectionMode;
                for (auto& line : m_ocrDoc.lines) {
                    bool lineHov = line.bounds.contains(mx, my);
                    for (auto& word : line.words) {
                        bool hov = lineMode ? lineHov : word.bounds.contains(mx, my);
                        if (word.hovered != hov) {
                            word.hovered = hov;
                            changed = true;
                        }
                    }
                }
            }

            if (changed) {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!m_isDragging) return 0;
        m_isDragging = false;
        ReleaseCapture();

        float mx = static_cast<float>(GET_X_LPARAM(lParam));
        float my = static_cast<float>(GET_Y_LPARAM(lParam));
        m_mouseX = mx;
        m_mouseY = my;

        if (m_phase == OverlayPhase::SelectingRegion) {
            RectF crop = RectF{ m_dragStartX, m_dragStartY, mx - m_dragStartX, my - m_dragStartY }.normalized();
            if (crop.width >= 14.0f && crop.height >= 12.0f) {
                RunOcrOnRegion(crop);
            } else {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (m_phase == OverlayPhase::InteractiveText) {
            RectF dragRect = { m_dragStartX, m_dragStartY, mx - m_dragStartX, my - m_dragStartY };
            UpdateSelectionFromDrag(dragRect, m_dragAdditive);
            if (!m_settings || m_settings->autoCopyOnRelease) {
                CopyCurrentSelection(false);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        CloseOverlay();
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wParam == VK_ESCAPE) {
            CloseOverlay();
            return 0;
        }
        if (m_phase == OverlayPhase::SelectingRegion) {
            if (wParam == VK_SPACE || wParam == 'F') {
                ScanActiveMonitorFull();
                return 0;
            }
        } else if (m_phase == OverlayPhase::InteractiveText) {
            bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrlDown && wParam == 'A') {
                CopyCurrentSelection(true, false);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if ((ctrlDown && shiftDown && wParam == 'C') || (!ctrlDown && wParam == 'A')) {
                CopyCurrentSelection(true, true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if ((ctrlDown && wParam == 'C') || wParam == VK_RETURN) {
                CopyCurrentSelection(false, false);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!ctrlDown && wParam == 'C') {
                if (m_settings) {
                    m_settings->codeIndentationMode = !m_settings->codeIndentationMode;
                    AppStorage::SaveSettings(*m_settings);
                }
                UpdateHudLayout();
                CopyCurrentSelection(false, false);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!ctrlDown && wParam == 'L') {
                if (m_settings) {
                    m_settings->lineSelectionMode = !m_settings->lineSelectionMode;
                    AppStorage::SaveSettings(*m_settings);
                }
                UpdateHudLayout();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!ctrlDown && wParam == 'G') {
                if (m_settings) {
                    m_settings->stripLineNumbers = !m_settings->stripLineNumbers;
                    AppStorage::SaveSettings(*m_settings);
                }
                UpdateHudLayout();
                CopyCurrentSelection(false, false);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!ctrlDown && wParam == 'H') {
                if (m_settings) {
                    m_settings->showConfidenceHeatmap = !m_settings->showConfidenceHeatmap;
                    AppStorage::SaveSettings(*m_settings);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!ctrlDown && wParam == 'R') {
                m_phase = OverlayPhase::SelectingRegion;
                m_isDragging = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!ctrlDown && wParam == 'F') {
                ScanActiveMonitorFull();
                return 0;
            }
        }
        break;
    }

    case WM_TIMER: {
        if (wParam == TIMER_TOAST_ID) {
            KillTimer(hwnd, TIMER_TOAST_ID);
            m_toastExpireTick = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == TIMER_AUTOCLOSE_ID) {
            KillTimer(hwnd, TIMER_AUTOCLOSE_ID);
            CloseOverlay();
            return 0;
        }
        break;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace ScreenTextify
