#pragma once

#include <windows.h>

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
};
