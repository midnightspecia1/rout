#pragma once

#include <windows.h>

#include <optional>
#include <string>

#include "theme.h"

// User settings (M6), stored in %APPDATA%\rout\settings.json. The look is
// a preset plus a few optional overrides, so a preset tweak in a later
// version still reaches users who changed only, say, the font size.
struct Settings {
    std::wstring preset = kThemePresets[0].name;
    std::optional<COLORREF> accent;    // Label outline colour.
    std::optional<int> opacityPercent;  // kMinOpacity..100.
    std::optional<int> fontSize;        // Points.
    std::optional<int> cornerRadius;    // Px at 96 DPI.

    static constexpr int kMinOpacity = 30;
    static constexpr int kMinFontSize = 6;
    static constexpr int kMaxFontSize = 32;
    static constexpr int kMaxCornerRadius = 12;
};

// The preset with the overrides applied. An unknown preset name falls
// back to the first preset.
Theme ResolveTheme(const Settings& settings);

// Defaults when the file is missing or unreadable; values out of range
// are clamped, so a hand-edited file can't break rendering.
Settings LoadSettings();
bool SaveSettings(const Settings& settings);
