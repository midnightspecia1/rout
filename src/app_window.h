#pragma once

#include <windows.h>

#include <functional>

// Range of private messages the rest of the app posts to this window
// (WM_APP+1 .. WM_APP+99). Window messages, unlike thread messages, are
// not dropped while a modal loop (tray menu, window drag) is running.
inline constexpr UINT kAppMessageFirst = WM_APP + 1;
inline constexpr UINT kAppMessageLast = WM_APP + 99;

// The program's own window. Starts hidden and owns the tray icon: a
// left click on the icon (or "Open" in its menu) shows the window,
// "Exit" ends the process. Closing the window only hides it — rout
// keeps running in the background.
//
// For now it only shows usage help; the M6 settings UI goes here.
class AppWindow {
public:
    bool Create(HINSTANCE instance);
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

    HWND hwnd_ = nullptr;
    HWND label_ = nullptr;
    HFONT font_ = nullptr;
    // Explorer broadcasts this after it restarts; tray icons must be
    // re-added or they silently disappear.
    UINT taskbarCreated_ = 0;
    AppMessageHandler onAppMessage_;
};
