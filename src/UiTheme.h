#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

namespace ScreenTextify {

using Microsoft::WRL::ComPtr;

namespace UiTheme {

// Deep Obsidian / Tokyo-Night Slate Palette (high contrast, calm visual hierarchy)
inline constexpr D2D1_COLOR_F BgCanvas        = { 0.039f, 0.051f, 0.078f, 1.0f }; // #0A0D14
inline constexpr D2D1_COLOR_F BgSidebar       = { 0.055f, 0.071f, 0.110f, 1.0f }; // #0E121C
inline constexpr D2D1_COLOR_F BgSurface       = { 0.071f, 0.094f, 0.145f, 1.0f }; // #121825
inline constexpr D2D1_COLOR_F BgSurfaceHover  = { 0.094f, 0.125f, 0.192f, 1.0f }; // #182031
inline constexpr D2D1_COLOR_F BgEditor        = { 0.047f, 0.063f, 0.098f, 1.0f }; // #0C1019
inline constexpr D2D1_COLOR_F BgEditorGutter  = { 0.039f, 0.051f, 0.082f, 1.0f }; // #0A0D15
inline constexpr D2D1_COLOR_F BgEditorHeader  = { 0.063f, 0.082f, 0.129f, 1.0f }; // #101521

inline constexpr D2D1_COLOR_F BorderSubtle    = { 0.125f, 0.165f, 0.243f, 0.85f }; // #202A3E
inline constexpr D2D1_COLOR_F BorderFocus     = { 0.220f, 0.741f, 0.973f, 0.90f }; // #38BDF8
inline constexpr D2D1_COLOR_F TopSpecular     = { 1.000f, 1.000f, 1.000f, 0.07f };

inline constexpr D2D1_COLOR_F AccentPrimary   = { 0.220f, 0.741f, 0.973f, 1.0f }; // #38BDF8 Sky
inline constexpr D2D1_COLOR_F AccentBlue      = { 0.145f, 0.388f, 0.922f, 1.0f }; // #2563EB Royal
inline constexpr D2D1_COLOR_F AccentEmerald   = { 0.133f, 0.827f, 0.588f, 1.0f }; // #22D396 Emerald
inline constexpr D2D1_COLOR_F AccentViolet    = { 0.655f, 0.545f, 0.980f, 1.0f }; // #A78BFA Violet
inline constexpr D2D1_COLOR_F AccentAmber     = { 0.984f, 0.749f, 0.141f, 1.0f }; // #FBBF24 Amber

inline constexpr D2D1_COLOR_F TextPrimary     = { 0.945f, 0.961f, 0.988f, 1.0f }; // #F1F5FC
inline constexpr D2D1_COLOR_F TextSecondary   = { 0.580f, 0.647f, 0.745f, 1.0f }; // #94A5BE
inline constexpr D2D1_COLOR_F TextMuted       = { 0.392f, 0.455f, 0.553f, 1.0f }; // #64748D

// Syntax Highlighting Palette (VS Code Dark+ / Tokyo Night inspired)
inline constexpr D2D1_COLOR_F SynPlain        = { 0.878f, 0.910f, 0.965f, 1.0f }; // #E0E8F6
inline constexpr D2D1_COLOR_F SynKeyword      = { 0.776f, 0.525f, 0.855f, 1.0f }; // #C686DA Purple
inline constexpr D2D1_COLOR_F SynType         = { 0.306f, 0.788f, 0.690f, 1.0f }; // #4EC9B0 Teal
inline constexpr D2D1_COLOR_F SynFunction     = { 0.863f, 0.863f, 0.667f, 1.0f }; // #DCDCAA Gold
inline constexpr D2D1_COLOR_F SynString       = { 0.855f, 0.588f, 0.482f, 1.0f }; // #DA967B Peach
inline constexpr D2D1_COLOR_F SynNumber       = { 0.710f, 0.808f, 0.659f, 1.0f }; // #B5CEA8 Sage
inline constexpr D2D1_COLOR_F SynComment      = { 0.416f, 0.600f, 0.333f, 1.0f }; // #6A9955 Green
inline constexpr D2D1_COLOR_F SynOperator     = { 0.537f, 0.824f, 0.996f, 1.0f }; // #89D2FE Cyan
inline constexpr D2D1_COLOR_F SynTag          = { 0.337f, 0.612f, 0.839f, 1.0f }; // #569CD6 Blue

// Smooth color interpolation for 60fps UI transitions
inline D2D1_COLOR_F LerpColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    };
}

inline D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float a) {
    return { c.r, c.g, c.b, a };
}

// Critically damped exponential smoothing (frame-rate independent)
inline bool Approach(float& value, float target, float speed, float dtSec) {
    float diff = target - value;
    if (std::fabs(diff) < 0.002f) {
        value = target;
        return false;
    }
    float factor = 1.0f - std::exp(-speed * dtSec);
    value += diff * factor;
    return true;
}

// Soft layered drop shadow for floating cards, HUD bars, and toast notifications
inline void DrawSoftShadow(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& rect, float radius, float elevation = 1.0f) {
    for (int layer = 3; layer >= 1; --layer) {
        float spread = static_cast<float>(layer) * 2.5f * elevation;
        float offsetY = static_cast<float>(layer) * 2.0f * elevation;
        D2D1_RECT_F sr = D2D1::RectF(rect.left - spread * 0.5f, rect.top + offsetY, rect.right + spread * 0.5f, rect.bottom + offsetY + spread * 0.6f);
        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f / static_cast<float>(layer)));
        rt->FillRoundedRectangle(D2D1::RoundedRect(sr, radius + spread * 0.4f, radius + spread * 0.4f), brush);
    }
}

// Crisp Direct2D Vector Iconography (resolution-independent, zero bitmap blur)
enum class VectorIcon {
    BrandLogo,
    RegionCrop,
    FullScreen,
    QuickSnip,
    StitchLayers,
    Clipboard,
    PasteImage,
    SaveFile,
    Trash,
    SparkBolt,
    HistoryClock,
    Sliders
};

inline void DrawVectorIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, VectorIcon icon, float cx, float cy, float size, const D2D1_COLOR_F& color) {
    brush->SetColor(color);
    const float r = size * 0.5f;
    const float sw = std::max(1.35f, size * 0.105f);

    switch (icon) {
    case VectorIcon::BrandLogo: {
        // Stylized viewfinder corners + crisp 'T' glyph
        float k = r * 0.42f;
        float x0 = cx - r, y0 = cy - r, x1 = cx + r, y1 = cy + r;
        rt->DrawLine({ x0, y0 + k }, { x0, y0 }, brush, sw);
        rt->DrawLine({ x0, y0 }, { x0 + k, y0 }, brush, sw);
        rt->DrawLine({ x1 - k, y0 }, { x1, y0 }, brush, sw);
        rt->DrawLine({ x1, y0 }, { x1, y0 + k }, brush, sw);
        rt->DrawLine({ x0, y1 - k }, { x0, y1 }, brush, sw);
        rt->DrawLine({ x0, y1 }, { x0 + k, y1 }, brush, sw);
        rt->DrawLine({ x1 - k, y1 }, { x1, y1 }, brush, sw);
        rt->DrawLine({ x1, y1 }, { x1, y1 - k }, brush, sw);
        brush->SetColor(TextPrimary);
        rt->DrawLine({ cx - r * 0.46f, cy - r * 0.34f }, { cx + r * 0.46f, cy - r * 0.34f }, brush, sw * 1.25f);
        rt->DrawLine({ cx, cy - r * 0.34f }, { cx, cy + r * 0.48f }, brush, sw * 1.25f);
        break;
    }
    case VectorIcon::RegionCrop: {
        // Dashed-style viewfinder frame + inner text bar
        float x0 = cx - r * 0.85f, y0 = cy - r * 0.85f, x1 = cx + r * 0.85f, y1 = cy + r * 0.85f;
        float k = r * 0.48f;
        rt->DrawLine({ x0, y0 + k }, { x0, y0 }, brush, sw);
        rt->DrawLine({ x0, y0 }, { x0 + k, y0 }, brush, sw);
        rt->DrawLine({ x1 - k, y0 }, { x1, y0 }, brush, sw);
        rt->DrawLine({ x1, y0 }, { x1, y0 + k }, brush, sw);
        rt->DrawLine({ x0, y1 - k }, { x0, y1 }, brush, sw);
        rt->DrawLine({ x0, y1 }, { x0 + k, y1 }, brush, sw);
        rt->DrawLine({ x1 - k, y1 }, { x1, y1 }, brush, sw);
        rt->DrawLine({ x1, y1 }, { x1, y1 - k }, brush, sw);
        rt->DrawLine({ cx - r * 0.38f, cy - r * 0.18f }, { cx + r * 0.38f, cy - r * 0.18f }, brush, sw * 0.9f);
        rt->DrawLine({ cx - r * 0.38f, cy + r * 0.22f }, { cx + r * 0.18f, cy + r * 0.22f }, brush, sw * 0.9f);
        break;
    }
    case VectorIcon::FullScreen: {
        D2D1_RECT_F mon = D2D1::RectF(cx - r * 0.90f, cy - r * 0.72f, cx + r * 0.90f, cy + r * 0.46f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(mon, 2.5f, 2.5f), brush, sw);
        rt->DrawLine({ cx - r * 0.36f, cy + r * 0.80f }, { cx + r * 0.36f, cy + r * 0.80f }, brush, sw);
        rt->DrawLine({ cx, cy + r * 0.46f }, { cx, cy + r * 0.80f }, brush, sw);
        break;
    }
    case VectorIcon::QuickSnip:
    case VectorIcon::SparkBolt: {
        // Sharp lightning bolt
        D2D1_POINT_2F p0 = { cx + r * 0.12f, cy - r * 0.92f };
        D2D1_POINT_2F p1 = { cx - r * 0.56f, cy + r * 0.08f };
        D2D1_POINT_2F p2 = { cx + r * 0.04f, cy + r * 0.08f };
        D2D1_POINT_2F p3 = { cx - r * 0.14f, cy + r * 0.92f };
        D2D1_POINT_2F p4 = { cx + r * 0.58f, cy - r * 0.10f };
        D2D1_POINT_2F p5 = { cx - r * 0.02f, cy - r * 0.10f };
        rt->DrawLine(p0, p1, brush, sw);
        rt->DrawLine(p1, p2, brush, sw);
        rt->DrawLine(p2, p3, brush, sw);
        rt->DrawLine(p3, p4, brush, sw);
        rt->DrawLine(p4, p5, brush, sw);
        rt->DrawLine(p5, p0, brush, sw);
        break;
    }
    case VectorIcon::StitchLayers: {
        // Two overlapping stacked code layers + plus mark
        D2D1_RECT_F top = D2D1::RectF(cx - r * 0.85f, cy - r * 0.75f, cx + r * 0.45f, cy + r * 0.15f);
        D2D1_RECT_F bot = D2D1::RectF(cx - r * 0.45f, cy - r * 0.15f, cx + r * 0.85f, cy + r * 0.75f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(top, 2.2f, 2.2f), brush, sw);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(bot, 2.2f, 2.2f), brush, sw);
        break;
    }
    case VectorIcon::Clipboard: {
        D2D1_RECT_F doc = D2D1::RectF(cx - r * 0.68f, cy - r * 0.65f, cx + r * 0.68f, cy + r * 0.88f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(doc, 2.2f, 2.2f), brush, sw);
        D2D1_RECT_F clip = D2D1::RectF(cx - r * 0.34f, cy - r * 0.88f, cx + r * 0.34f, cy - r * 0.45f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(clip, 1.5f, 1.5f), brush, sw);
        break;
    }
    case VectorIcon::PasteImage: {
        D2D1_RECT_F frame = D2D1::RectF(cx - r * 0.85f, cy - r * 0.72f, cx + r * 0.85f, cy + r * 0.72f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(frame, 2.4f, 2.4f), brush, sw);
        rt->FillEllipse(D2D1::Ellipse({ cx - r * 0.35f, cy - r * 0.22f }, r * 0.16f, r * 0.16f), brush);
        rt->DrawLine({ cx - r * 0.68f, cy + r * 0.52f }, { cx - r * 0.12f, cy + r * 0.04f }, brush, sw);
        rt->DrawLine({ cx - r * 0.12f, cy + r * 0.04f }, { cx + r * 0.22f, cy + r * 0.32f }, brush, sw);
        rt->DrawLine({ cx + r * 0.22f, cy + r * 0.32f }, { cx + r * 0.56f, cy - r * 0.08f }, brush, sw);
        rt->DrawLine({ cx + r * 0.56f, cy - r * 0.08f }, { cx + r * 0.74f, cy + r * 0.52f }, brush, sw);
        break;
    }
    case VectorIcon::SaveFile: {
        D2D1_RECT_F box = D2D1::RectF(cx - r * 0.78f, cy - r * 0.78f, cx + r * 0.78f, cy + r * 0.78f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.2f, 2.2f), brush, sw);
        D2D1_RECT_F slot = D2D1::RectF(cx - r * 0.42f, cy - r * 0.78f, cx + r * 0.34f, cy - r * 0.18f);
        rt->DrawRectangle(slot, brush, sw * 0.9f);
        rt->DrawLine({ cx - r * 0.42f, cy + r * 0.38f }, { cx + r * 0.42f, cy + r * 0.38f }, brush, sw * 0.9f);
        break;
    }
    case VectorIcon::Trash: {
        rt->DrawLine({ cx - r * 0.78f, cy - r * 0.48f }, { cx + r * 0.78f, cy - r * 0.48f }, brush, sw);
        rt->DrawLine({ cx - r * 0.32f, cy - r * 0.78f }, { cx + r * 0.32f, cy - r * 0.78f }, brush, sw);
        D2D1_RECT_F bin = D2D1::RectF(cx - r * 0.56f, cy - r * 0.48f, cx + r * 0.56f, cy + r * 0.80f);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(bin, 1.8f, 1.8f), brush, sw);
        break;
    }
    case VectorIcon::HistoryClock: {
        rt->DrawEllipse(D2D1::Ellipse({ cx, cy }, r * 0.82f, r * 0.82f), brush, sw);
        rt->DrawLine({ cx, cy - r * 0.45f }, { cx, cy + r * 0.04f }, brush, sw);
        rt->DrawLine({ cx, cy + r * 0.04f }, { cx + r * 0.38f, cy + r * 0.24f }, brush, sw);
        break;
    }
    case VectorIcon::Sliders: {
        float y0 = cy - r * 0.48f, y1 = cy, y2 = cy + r * 0.48f;
        rt->DrawLine({ cx - r * 0.82f, y0 }, { cx + r * 0.82f, y0 }, brush, sw);
        rt->DrawLine({ cx - r * 0.82f, y1 }, { cx + r * 0.82f, y1 }, brush, sw);
        rt->DrawLine({ cx - r * 0.82f, y2 }, { cx + r * 0.82f, y2 }, brush, sw);
        rt->FillEllipse(D2D1::Ellipse({ cx - r * 0.28f, y0 }, r * 0.22f, r * 0.22f), brush);
        rt->FillEllipse(D2D1::Ellipse({ cx + r * 0.32f, y1 }, r * 0.22f, r * 0.22f), brush);
        rt->FillEllipse(D2D1::Ellipse({ cx - r * 0.14f, y2 }, r * 0.22f, r * 0.22f), brush);
        break;
    }
    }
}

} // namespace UiTheme

} // namespace ScreenTextify
