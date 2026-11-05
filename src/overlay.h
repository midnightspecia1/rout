#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "theme.h"

struct HintBox {
    RECT rect;  // Screen (virtual-desktop) coordinates, physical pixels.
    std::wstring label;
};

// Fullscreen, click-through, never-activates overlay window that draws
// hint boxes over whatever is currently on screen. Visibility and
// content are the only state it owns; it never receives real input —
// the global keyboard hook in main.cpp decides when to show/hide it.
class Overlay {
public:
    bool Create();
    void Show(const std::vector<HintBox>& hints);
    void Hide();
    bool IsVisible() const { return visible_; }
    void SetTheme(const Theme& theme) { theme_ = theme; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Paint();

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    // Top-left of the virtual screen; negative when a monitor sits left
    // of / above the primary one. Subtracted to map screen -> client.
    POINT origin_{};
    Theme theme_ = kDefaultTheme;
    std::vector<HintBox> hints_;
};
