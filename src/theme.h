#pragma once

#include <windows.h>

// Visual parameters for hint rendering. Kept as a plain struct from the
// start so the M6 settings window (presets + override sliders) only has
// to produce a different Theme, not touch the renderer.
struct Theme {
    COLORREF background;
    COLORREF border;
    COLORREF text;
    int borderWidth;
};

inline constexpr Theme kDefaultTheme{
    RGB(30, 30, 30),
    RGB(90, 180, 220),
    RGB(255, 255, 255),
    1,
};
