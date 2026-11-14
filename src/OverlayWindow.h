#pragma once

#include "Types.h"
#include "ScreenCapture.h"
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <string>
#include <vector>

namespace ScreenTextify {

enum class OverlayPhase {
    Hidden,
    SelectingRegion,
    InteractiveText
};

struct HudButton {
    int id = 0;
    std::wstring label;
    RectF bounds;
    bool hovered = false;
    bool active = false;
};

class OverlayWindow {
public:
    using OnTextCopiedCallback = std::function<void(const std::wstring& text, bool codeMode, size_t wordCount, double ocrTimeMs)>;

    OverlayWindow(HINSTANCE hInstance, AppSettings* settings, OnTextCopiedCallback onCopied);
    ~OverlayWindow();

    bool Initialize();

    // Starts the capture session in RegionInteractive, FullScreenSelect, or QuickSnipCopy mode
    void StartCaptureSession(CaptureLaunchMode mode);

    void CloseOverlay();
    bool IsVisible() const { return m_phase != OverlayPhase::Hidden; }
    HWND GetHwnd() const { return m_hwnd; }

    static bool CopyTextToClipboard(HWND hwndOwner, const std::wstring& text);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateD2DResources();
    void DiscardD2DResources();
    void OnPaint();

    void RunOcrOnRegion(const RectF& localRegion);
    void ScanActiveMonitorFull();
    void UpdateHudLayout();
    void UpdateSelectionFromDrag(const RectF& dragRect, bool additive);
    void CopyCurrentSelection(bool copyAllIfNoneSelected = false);
    void ShowToast(const std::wstring& message, const std::wstring& preview);

    HINSTANCE m_hInstance = nullptr;
    HWND m_hwnd = nullptr;
    AppSettings* m_settings = nullptr;
    OnTextCopiedCallback m_onCopied;

    CaptureLaunchMode m_launchMode = CaptureLaunchMode::RegionInteractive;
    OverlayPhase m_phase = OverlayPhase::Hidden;
    RawBitmap m_frozenScreen;
    OcrDocument m_ocrDoc;
    std::wstring m_ocrError;

    // Region drag or text lasso drag state
    bool m_isDragging = false;
    bool m_dragAdditive = false;
    float m_dragStartX = 0.0f;
    float m_dragStartY = 0.0f;
    float m_mouseX = 0.0f;
    float m_mouseY = 0.0f;
    RectF m_activeScanRegion;

    // HUD buttons & status
    std::vector<HudButton> m_hudButtons;
    RectF m_hudBarRect;
    std::wstring m_hudMetricsText;

    // Toast notification state
    std::wstring m_toastTitle;
    std::wstring m_toastPreview;
    ULONGLONG m_toastExpireTick = 0;

    // Direct2D / DirectWrite resources
    ID2D1Factory* m_d2dFactory = nullptr;
    IDWriteFactory* m_dwriteFactory = nullptr;
    ID2D1HwndRenderTarget* m_renderTarget = nullptr;
    ID2D1Bitmap* m_screenBitmap = nullptr;
    ID2D1SolidColorBrush* m_brush = nullptr;
    IDWriteTextFormat* m_hudTextFormat = nullptr;
    IDWriteTextFormat* m_bannerTextFormat = nullptr;
    IDWriteTextFormat* m_monoPreviewFormat = nullptr;
};

} // namespace ScreenTextify
