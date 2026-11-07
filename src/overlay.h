#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "theme.h"

struct HintBox {
    // Top-left corner of the label box in screen (virtual-desktop)
    // coordinates, physical pixels. The overlay sizes the box from the
    // label text, so callers never deal with font metrics or DPI.
    POINT anchor;
    std::wstring label;
    // A scroll-region label (M1c): drawn in the scroll colour, and the
    // anchor is the box's top-right corner, so it sits inside the
    // region's corner next to the scrollbar instead of on top of
    // whatever clickable element starts there.
    bool scroll = false;
};

// A one-line status box (e.g. the M1b search query), drawn in the hint
// style. Empty text means no banner.
struct Banner {
    POINT bottomLeft;  // Screen coordinates, physical pixels.
    std::wstring text;
};

// Fullscreen, click-through, never-activates overlay window that draws
// hint boxes over whatever is currently on screen. Visibility and
// content are the only state it owns; it never receives real input —
// the global keyboard hook in main.cpp decides when to show/hide it.
class Overlay {
public:
    bool Create();
    // `frame`, when not empty, is outlined: the region scroll mode is
    // driving.
    void Show(const std::vector<HintBox>& hints, const Banner& banner = {}, const RECT& frame = {});
    void Hide();
    bool IsVisible() const { return visible_; }
    void SetTheme(const Theme& theme);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ApplyLayeredAttributes();
    void Paint();
    // Draws one rounded box with centred text; returns its size.
    SIZE DrawBox(HDC hdc, POINT topLeftClient, const std::wstring& text, bool measureOnly);
    void DrawFrame(HDC hdc);
    int Scale(int px) const { return MulDiv(px, dpi_, 96); }

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    // Top-left of the virtual screen; negative when a monitor sits left
    // of / above the primary one. Subtracted to map screen -> client.
    POINT origin_{};
    Theme theme_ = kDefaultTheme;
    // The overlay spans every monitor, so it uses the system DPI; on
    // mixed-DPI setups labels on other monitors are sized for the
    // primary one. Per-monitor sizing comes with the M6 renderer.
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    std::vector<HintBox> hints_;
    Banner banner_;
    RECT frame_{};
};
