#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct HintBox {
    RECT rect;
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

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Paint();

    HWND hwnd_ = nullptr;
    bool visible_ = false;
    std::vector<HintBox> hints_;
};
