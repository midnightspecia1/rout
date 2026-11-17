#pragma once

#include <windows.h>

// Visual parameters for hint rendering. The settings window (M6) picks a
// preset and overrides a few fields; the renderer only ever sees the
// resulting Theme.
struct Theme {
    COLORREF background;
    COLORREF border;        // The accent: label outlines.
    COLORREF scrollBorder;  // Scroll-region labels (M1c) and the active region frame.
    COLORREF text;
    int borderWidth;
    int cornerRadius;  // At 96 DPI; 0 = square corners.
    int fontSize;      // Points; 0 = the system UI font size.
    // Opacity of the whole overlay, 0-255. With the GDI + LWA_COLORKEY
    // renderer this fades box and text together; a translucent box behind
    // opaque text would need per-pixel alpha (UpdateLayeredWindow).
    BYTE opacity;
};

struct ThemePreset {
    const wchar_t* name;  // Also the key stored in settings.json.
    Theme theme;
};

// None of these may use the overlay's transparency key, RGB(1, 1, 1).
inline constexpr ThemePreset kThemePresets[] = {
    {L"Dark", {RGB(30, 30, 30), RGB(90, 180, 220), RGB(235, 160, 60), RGB(255, 255, 255), 1, 6, 0, 255}},
    {L"Light", {RGB(250, 250, 250), RGB(0, 120, 212), RGB(215, 110, 0), RGB(20, 20, 20), 1, 6, 0, 255}},
    {L"Minimal", {RGB(24, 24, 24), RGB(24, 24, 24), RGB(200, 120, 40), RGB(235, 235, 235), 1, 0, 0, 230}},
    {L"High Contrast", {RGB(0, 0, 0), RGB(255, 255, 0), RGB(0, 255, 255), RGB(255, 255, 255), 2, 0, 0, 255}},
    {L"Nyxt", {RGB(255, 214, 51), RGB(130, 95, 0), RGB(200, 60, 0), RGB(0, 0, 0), 1, 3, 0, 255}},
};

inline constexpr Theme kDefaultTheme = kThemePresets[0].theme;
