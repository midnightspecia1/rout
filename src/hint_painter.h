#pragma once

#include <windows.h>

#include <string>

#include "theme.h"

// Draws hint label boxes and the scroll-region frame in a given theme and
// DPI. Shared by the overlay and the settings preview, so the preview
// shows exactly what the overlay will.
class HintPainter {
public:
    HintPainter(const Theme& theme, UINT dpi);
    ~HintPainter();
    HintPainter(const HintPainter&) = delete;
    HintPainter& operator=(const HintPainter&) = delete;

    const Theme& theme() const { return theme_; }

    // Size of the box `text` would get.
    SIZE Measure(HDC hdc, const std::wstring& text) const;
    // One rounded box with centred text; `scroll` picks the scroll colour.
    void DrawBox(HDC hdc, POINT topLeft, const std::wstring& text, bool scroll) const;
    // Outline of the region scroll mode is driving.
    void DrawFrame(HDC hdc, const RECT& r) const;

private:
    int Scale(int px) const { return MulDiv(px, dpi_, 96); }

    Theme theme_;
    UINT dpi_;
    HFONT font_ = nullptr;
    HBRUSH boxBrush_ = nullptr;
    HPEN borderPen_ = nullptr;
    HPEN scrollPen_ = nullptr;
};
