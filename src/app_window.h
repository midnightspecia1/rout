#pragma once

#include <windows.h>

#include <functional>

#include "settings.h"

// Range of private messages the rest of the app posts to this window
// (WM_APP+1 .. WM_APP+99). Window messages, unlike thread messages, are
// not dropped while a modal loop (tray menu, window drag) is running.
inline constexpr UINT kAppMessageFirst = WM_APP + 1;
inline constexpr UINT kAppMessageLast = WM_APP + 99;

// The program's own window: short usage help and the settings (M6).
// Starts hidden and owns the tray icon: a left click on the icon (or
// "Settings" in its menu) shows the window, "Exit" ends the process.
// Closing the window only hides it; rout keeps running in the
// background.
//
// Settings apply as they change (overlay and preview alike) and are
// saved when a change is complete, so there is no OK/Cancel.
class AppWindow {
public:
    using ThemeHandler = std::function<void(const Theme&)>;
    bool Create(HINSTANCE instance, const Settings& settings, ThemeHandler onTheme);
    void ShowAndActivate();
    HWND Handle() const { return hwnd_; }

    using AppMessageHandler = std::function<void(UINT msg, WPARAM wParam, LPARAM lParam)>;
    void SetAppMessageHandler(AppMessageHandler handler) { onAppMessage_ = std::move(handler); }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT at);
    void ApplyDpi(UINT dpi);

    void CreateControls();
    void SyncControls();  // settings_ -> controls.
    void Changed(bool save);
    void OnCommand(int id, int code);
    void OnSlider(HWND slider, bool done);
    void PickAccent();
    void DrawPreview(const DRAWITEMSTRUCT& di);
    void DrawSwatch(const DRAWITEMSTRUCT& di);
    void DrawLogo(const DRAWITEMSTRUCT& di);

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    // Explorer broadcasts this after it restarts; tray icons must be
    // re-added or they silently disappear.
    UINT taskbarCreated_ = 0;
    AppMessageHandler onAppMessage_;

    Settings settings_;
    ThemeHandler onTheme_;
    HWND logo_ = nullptr;
    HBITMAP logoBitmap_ = nullptr;  // Rescaled from the PNG for each DPI.
    HWND help_ = nullptr;
    HWND preset_ = nullptr;
    HWND accent_ = nullptr;
    HWND swatch_ = nullptr;
    HWND opacity_ = nullptr;
    HWND fontSize_ = nullptr;
    HWND radius_ = nullptr;
    HWND preview_ = nullptr;
    HWND rowLabels_[5]{};
    HWND values_[3]{};  // Opacity, font size, radius readouts.
};
