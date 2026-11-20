#pragma once

#include "Types.h"
#include "ScreenCapture.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <vector>
#include <string>

namespace ScreenTextify {

class HistoryWindow {
public:
    using StartCaptureCallback = std::function<void(CaptureLaunchMode mode)>;

    HistoryWindow(HINSTANCE hInstance, AppSettings* settings, StartCaptureCallback onStartCapture);
    ~HistoryWindow();

    bool Initialize();
    void ShowAndFocus();
    void AddHistoryEntry(const std::wstring& text, bool codeMode, size_t wordCount, double ocrTimeMs);
    void RefreshControls();
    HWND GetHwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateD2DResources();
    void DiscardD2DResources();
    void OnPaint();
    void BuildUiLayout(float width, float height);
    void RunBuiltInDemoCapture();
    void RunOcrOnBitmap(const RawBitmap& bmp, const std::wstring& sourceLabel);
    void RunOcrFromClipboard();
    void SaveSelectedCaptureToFile();

    struct UiHotspot {
        int id = 0;
        RectF bounds;
        std::wstring title;
        std::wstring subtitle;
        std::wstring badge;
        bool hovered = false;
    };

    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;
    AppSettings* m_settings = nullptr;
    StartCaptureCallback m_onStartCapture;

    std::vector<CaptureHistoryItem> m_history;
    int m_selectedHistoryIdx = 0;
    float m_codeScrollY = 0.0f;
    float m_maxCodeScrollY = 0.0f;
    RectF m_codeViewportRect;

    std::wstring m_ocrLangName;
    std::wstring m_statusBanner;
    ULONGLONG m_statusFlashTick = 0;

    std::vector<UiHotspot> m_hotspots;
    float m_mouseX = 0.0f;
    float m_mouseY = 0.0f;

    // Direct2D / DirectWrite resources
    ID2D1Factory* m_d2dFactory = nullptr;
    IDWriteFactory* m_dwriteFactory = nullptr;
    ID2D1HwndRenderTarget* m_renderTarget = nullptr;
    ID2D1SolidColorBrush* m_brush = nullptr;

    IDWriteTextFormat* m_titleFormat = nullptr;
    IDWriteTextFormat* m_subtitleFormat = nullptr;
    IDWriteTextFormat* m_cardTitleFormat = nullptr;
    IDWriteTextFormat* m_bodyFormat = nullptr;
    IDWriteTextFormat* m_badgeFormat = nullptr;
    IDWriteTextFormat* m_monoFormat = nullptr;
    IDWriteTextFormat* m_lineNumFormat = nullptr;
};

} // namespace ScreenTextify
