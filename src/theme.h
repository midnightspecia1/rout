#pragma once

#include <windows.h>

// Visual parameters for hint rendering. Kept as a plain struct from the
// start so the M6 settings window (presets + override sliders) only has
// to produce a different Theme, not touch the renderer.
struct Theme {
    COLORREF background;
    COLORREF border;
    COLORREF scrollBorder;  // Scroll-region labels (M1c) and the active region frame.
    COLORREF text;
    int borderWidth;
    int cornerRadius;  // 0 = square corners.
    // Opacity of the whole overlay, 0-255. With the current GDI +
    // LWA_COLORKEY renderer this fades box and text together; a
    // translucent box behind opaque text needs per-pixel alpha
    // (UpdateLayeredWindow), planned with the M6 theming work.
    BYTE opacity;
};

inline constexpr Theme kDefaultTheme{
    RGB(30, 30, 30),
    RGB(90, 180, 220),
    RGB(235, 160, 60),
    RGB(255, 255, 255),
    1,
    6,
    255,
};
