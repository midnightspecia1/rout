#include <windows.h>

#include <string>

#include "overlay.h"

namespace {

Overlay* g_overlay = nullptr;
HHOOK g_hook = nullptr;

// Hardcoded placeholder targets until the adapters/UIA path (M1-M4)
// exist to supply real ones.
std::vector<HintBox> DemoHints() {
    return {
        {{200, 200, 260, 230}, L"A"},
        {{400, 300, 460, 330}, L"B"},
        {{600, 150, 660, 180}, L"C"},
        {{300, 500, 360, 530}, L"D"},
        {{800, 400, 860, 430}, L"E"},
    };
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (kb->vkCode == VK_SPACE && ctrl && shift) {
            if (g_overlay->IsVisible()) {
                g_overlay->Hide();
            } else {
                g_overlay->Show(DemoHints());
            }
            return 1;
        }

        if (g_overlay->IsVisible()) {
            if (kb->vkCode == VK_ESCAPE) {
                g_overlay->Hide();
                return 1;
            }
            for (wchar_t c = L'A'; c <= L'E'; ++c) {
                if (kb->vkCode == static_cast<DWORD>(c)) {
                    std::wstring msg = L"rout: hint activated (placeholder): ";
                    msg += c;
                    msg += L'\n';
                    OutputDebugStringW(msg.c_str());
                    g_overlay->Hide();
                    return 1;
                }
            }
            // Swallow everything else so typing doesn't leak into the
            // focused app while the overlay is up.
            return 1;
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    Overlay overlay;
    if (!overlay.Create()) return 1;
    g_overlay = &overlay;

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    if (!g_hook) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    return 0;
}
