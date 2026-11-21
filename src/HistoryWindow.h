#pragma once

#include "Types.h"
#include "ScreenCapture.h"
#include "UiTheme.h"
#include "SyntaxHighlighter.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <unordered_map>
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
    enum class SidebarTab {
        History = 0,
        Settings = 1
    };

    struct UiHotspot {
        int id = 0;
        RectF bounds;
        std::wstring title;
        std::wstring subtitle;
        std::wstring badge;
        UiTheme::VectorIcon icon = UiTheme::VectorIcon::RegionCrop;
        D2D1_COLOR_F accent = UiTheme::AccentPrimary;
        bool hovered = false;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateD2DResources();
    void DiscardD2DResources();
    void UpdateDpi(UINT newDpi);
    void StartAnimationTimer();
    void StepAnimations();
    void OnPaint(HDC printHdc = nullptr);
    void BuildUiLayout(float width, float height);
    void RebuildSelectedCodeCache();
    void ShowToast(const std::wstring& message, bool isError = false);

    void RunBuiltInDemoCapture();
    void RunOcrOnBitmap(const RawBitmap& bmp, const std::wstring& sourceLabel);
    void RunOcrFromClipboard();
    void SaveSelectedCaptureToFile();

    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;
    AppSettings* m_settings = nullptr;
    StartCaptureCallback m_onStartCapture;

    // DPI state
    float m_dpi = 96.0f;
    float m_dpiScale = 1.0f;

    // App data & cached parsed code lines for selected history item
    std::vector<CaptureHistoryItem> m_history;
    int m_selectedHistoryIdx = 0;
    std::wstring m_selectedLang = L"Plain Text";
    std::vector<std::wstring> m_cachedLines;
    std::vector<std::vector<SyntaxSpan>> m_cachedSpans;

    // Sidebar tab & scroll state (smooth inertial scrolling)
    SidebarTab m_activeTab = SidebarTab::History;
    float m_tabSlideAnim = 0.0f; // 0.0 = History, 1.0 = Settings
    float m_codeScrollY = 0.0f;
    float m_targetCodeScrollY = 0.0f;
    float m_maxCodeScrollY = 0.0f;
    float m_listScrollY = 0.0f;
    float m_targetListScrollY = 0.0f;
    float m_maxListScrollY = 0.0f;

    RectF m_sidebarListRect;
    RectF m_codeViewportRect;
    RectF m_scrollbarTrackRect;
    bool m_draggingScrollbar = false;
    int m_hoveredCodeLine = -1;

    // Smooth animation state
    bool m_animTimerActive = false;
    ULONGLONG m_lastAnimTick = 0;
    std::unordered_map<int, float> m_hoverAnim;
    float m_toggleAnim[6] = {};

    // Status toast state
    std::wstring m_ocrLangName;
    std::wstring m_statusBanner;
    bool m_statusIsError = false;
    ULONGLONG m_statusExpireTick = 0;
    float m_toastAlpha = 0.0f;

    // Hit testing
    std::vector<UiHotspot> m_hotspots;
    float m_mouseX = -1.0f;
    float m_mouseY = -1.0f;

    // RAII Direct2D / DirectWrite resources
    ComPtr<ID2D1Factory> m_d2dFactory;
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ComPtr<ID2D1LinearGradientBrush> m_logoGradBrush;
    ComPtr<ID2D1SolidColorBrush> m_syntaxBrushes[9];

    ComPtr<IDWriteTextFormat> m_brandFormat;
    ComPtr<IDWriteTextFormat> m_headingFormat;
    ComPtr<IDWriteTextFormat> m_cardTitleFormat;
    ComPtr<IDWriteTextFormat> m_bodyFormat;
    ComPtr<IDWriteTextFormat> m_smallFormat;
    ComPtr<IDWriteTextFormat> m_badgeFormat;
    ComPtr<IDWriteTextFormat> m_monoFormat;
    ComPtr<IDWriteTextFormat> m_monoSmallFormat;
    ComPtr<IDWriteTextFormat> m_lineNumFormat;
};

} // namespace ScreenTextify
