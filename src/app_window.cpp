#include "app_window.h"

#include <shellapi.h>

namespace {

constexpr wchar_t kClassName[] = L"RoutAppWindow";
constexpr UINT WM_TRAY = WM_APP + 100;
constexpr UINT kTrayId = 1;
enum : UINT { kCmdOpen = 1, kCmdExit = 2 };

// Client size at 96 DPI; scaled for the window's actual DPI.
constexpr int kClientWidth = 380;
constexpr int kClientHeight = 190;
constexpr int kPadding = 16;

constexpr wchar_t kHelpText[] =
    L"rout is running in the background.\r\n"
    L"\r\n"
    L"Ctrl+Shift+Space — show hints\r\n"
    L"Letters — pick a hint, Backspace — erase a letter\r\n"
    L"Esc or Ctrl+Shift+Space — close hints\r\n"
    L"\r\n"
    L"Closing this window does not quit rout;\r\n"
    L"use the tray icon menu to exit.";

HICON AppIcon() { return LoadIconW(nullptr, IDI_APPLICATION); }

}  // namespace

bool AppWindow::Create(HINSTANCE instance) {
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &AppWindow::WndProc;
    wc.hInstance = instance;
    wc.hIcon = AppIcon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;

    // Size is set in ApplyDpi once the window knows its monitor's DPI.
    if (!CreateWindowExW(0, kClassName, L"rout", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this)) {
        return false;
    }
    ApplyDpi(GetDpiForWindow(hwnd_));
    AddTrayIcon();
    return true;
}

void AppWindow::ShowAndActivate() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void AppWindow::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = AppIcon();
    lstrcpynW(nid.szTip, L"rout — Ctrl+Shift+Space", ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Version 4: clean NIN_SELECT / WM_CONTEXTMENU events with the
    // anchor point in wParam, keyboard activation included.
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void AppWindow::RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void AppWindow::ShowTrayMenu(POINT at) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdOpen, L"Open");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");
    SetMenuDefaultItem(menu, kCmdOpen, FALSE);

    // Without this the menu won't close when clicking elsewhere.
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                      at.x, at.y, hwnd_, nullptr);
    DestroyMenu(menu);

    if (cmd == kCmdOpen) ShowAndActivate();
    if (cmd == kCmdExit) DestroyWindow(hwnd_);
}

void AppWindow::ApplyDpi(UINT dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    SendMessageW(label_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (font_) DeleteObject(font_);
    font_ = font;

    RECT r{0, 0, MulDiv(kClientWidth, dpi, 96), MulDiv(kClientHeight, dpi, 96)};
    AdjustWindowRectExForDpi(&r, GetWindowLongW(hwnd_, GWL_STYLE), FALSE, 0, dpi);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    const int pad = MulDiv(kPadding, dpi, 96);
    MoveWindow(label_, pad, pad, MulDiv(kClientWidth, dpi, 96) - 2 * pad,
               MulDiv(kClientHeight, dpi, 96) - 2 * pad, TRUE);
}

LRESULT AppWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == taskbarCreated_ && taskbarCreated_ != 0) {
        AddTrayIcon();
        return 0;
    }
    if (msg >= kAppMessageFirst && msg <= kAppMessageLast) {
        if (onAppMessage_) onAppMessage_(msg, wParam, lParam);
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            label_ = CreateWindowExW(0, L"STATIC", kHelpText, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     0, 0, 0, 0, hwnd_, nullptr, nullptr, nullptr);
            return 0;
        case WM_TRAY:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ShowAndActivate();
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu({static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))});
                    break;
            }
            return 0;
        case WM_DPICHANGED: {
            ApplyDpi(HIWORD(wParam));
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd_, SW_HIDE);  // Keep running in the tray.
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon();
            if (font_) DeleteObject(font_);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

LRESULT CALLBACK AppWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppWindow* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}
