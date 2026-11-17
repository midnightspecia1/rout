#include "settings.h"

#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

fs::path SettingsPath() {
    PWSTR base = nullptr;
    fs::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
        path = fs::path(base) / L"rout" / L"settings.json";
    }
    CoTaskMemFree(base);
    return path;
}

// Preset names are ASCII, which keeps the conversions trivial.
std::string Narrow(const std::wstring& s) { return std::string(s.begin(), s.end()); }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string ColorToHex(COLORREF c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

std::optional<COLORREF> HexToColor(const std::string& s) {
    unsigned r, g, b;
    if (s.size() != 7 || std::sscanf(s.c_str(), "#%2x%2x%2x", &r, &g, &b) != 3) return std::nullopt;
    return RGB(r, g, b);
}

std::optional<int> IntField(const json& j, const char* key, int lo, int hi) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    return std::clamp(it->get<int>(), lo, hi);
}

}  // namespace

Theme ResolveTheme(const Settings& s) {
    Theme theme = kThemePresets[0].theme;
    for (const ThemePreset& p : kThemePresets) {
        if (s.preset == p.name) theme = p.theme;
    }
    if (s.accent) theme.border = *s.accent;
    if (s.opacityPercent) theme.opacity = static_cast<BYTE>(*s.opacityPercent * 255 / 100);
    if (s.fontSize) theme.fontSize = *s.fontSize;
    if (s.cornerRadius) theme.cornerRadius = *s.cornerRadius;
    return theme;
}

Settings LoadSettings() {
    Settings s;
    std::ifstream in(SettingsPath());
    if (!in) return s;
    const json root = json::parse(in, nullptr, false);
    const auto theme = root.is_object() ? root.find("theme") : root.end();
    if (theme == root.end() || !theme->is_object()) return s;

    if (const auto p = theme->find("preset"); p != theme->end() && p->is_string()) s.preset = Widen(p->get<std::string>());
    if (const auto a = theme->find("accent"); a != theme->end() && a->is_string()) s.accent = HexToColor(a->get<std::string>());
    s.opacityPercent = IntField(*theme, "opacity", Settings::kMinOpacity, 100);
    s.fontSize = IntField(*theme, "fontSize", Settings::kMinFontSize, Settings::kMaxFontSize);
    s.cornerRadius = IntField(*theme, "cornerRadius", 0, Settings::kMaxCornerRadius);
    return s;
}

bool SaveSettings(const Settings& s) {
    const fs::path path = SettingsPath();
    if (path.empty()) return false;
    json theme{{"preset", Narrow(s.preset)}};
    if (s.accent) theme["accent"] = ColorToHex(*s.accent);
    if (s.opacityPercent) theme["opacity"] = *s.opacityPercent;
    if (s.fontSize) theme["fontSize"] = *s.fontSize;
    if (s.cornerRadius) theme["cornerRadius"] = *s.cornerRadius;

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Write aside and swap in, so a crash mid-write never leaves a
    // truncated file behind.
    fs::path temp = path;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << json{{"theme", theme}}.dump(2) << '\n';
        if (!out) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
