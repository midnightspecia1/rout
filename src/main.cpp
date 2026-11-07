#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "app_window.h"
#include "overlay.h"
#include "uia_hints.h"

namespace {

// Messages posted to the app window and handled on the main thread.
// Anything slow or input-injecting happens there, never inside the
// low-level hook callback: Windows silently unhooks a hook that exceeds
// LowLevelHooksTimeout.
constexpr UINT WM_APP_HINTS = kAppMessageFirst;         // wParam=generation, lParam=std::vector<UiaElement>*
constexpr UINT WM_APP_ACTIVATE = kAppMessageFirst + 1;  // wParam=index into g_targets
constexpr UINT WM_APP_CANCEL = kAppMessageFirst + 2;    // wParam=generation

// How far the mouse may drift (at 96 DPI) before hints are dismissed;
// small enough to react to a deliberate move, large enough to ignore a
// bumped desk.
constexpr int kMouseCancelDistance = 16;

// Home row first, so the most common hints are the easiest to type.
constexpr wchar_t kAlphabet[] = L"ASDFGHJKLQWERTYUIOPZXCVBNM";
constexpr size_t kAlphabetSize = sizeof(kAlphabet) / sizeof(kAlphabet[0]) - 1;

enum class Mode { Idle, Collecting, Showing };

struct Target {
    RECT rect;
    std::wstring name;  // For M1b name search; may be empty.
    std::wstring label;
};

Overlay* g_overlay = nullptr;
HHOOK g_hook = nullptr;
// Installed only while hints are collecting/showing, so idle rout
// costs nothing on every mouse move.
HHOOK g_mouseHook = nullptr;
POINT g_mouseStart{};
LONG g_mouseThreshold = kMouseCancelDistance;
HWND g_appWindow = nullptr;  // Receives WM_APP_* messages.
Mode g_mode = Mode::Idle;
// Bumped on every start/cancel so a late UIA result for an abandoned
// request is dropped instead of popping up an overlay.
WPARAM g_generation = 0;
std::vector<Target> g_targets;
std::wstring g_typed;

void Log(const std::wstring& text) {
    OutputDebugStringW((L"rout: " + text + L"\n").c_str());
}

// Fixed-length labels: with every label the same length no label is a
// prefix of another, so a full match is always unambiguous.
std::vector<std::wstring> MakeLabels(size_t count) {
    const size_t length = count <= kAlphabetSize ? 1 : 2;
    std::vector<std::wstring> labels;
    for (size_t i = 0; i < count; ++i) {
        if (length == 1) {
            labels.push_back(std::wstring(1, kAlphabet[i]));
        } else {
            labels.push_back({kAlphabet[i / kAlphabetSize], kAlphabet[i % kAlphabetSize]});
        }
    }
    return labels;
}

// Label box anchored at the element's top-left corner.
HintBox BoxFor(const Target& t) {
    return {{t.rect.left, t.rect.top}, t.label};
}

size_t CountMatches() {
    size_t n = 0;
    for (const auto& t : g_targets) {
        if (t.label.starts_with(g_typed)) ++n;
    }
    return n;
}

void Redraw() {
    std::vector<HintBox> boxes;
    for (const auto& t : g_targets) {
        if (t.label.starts_with(g_typed)) boxes.push_back(BoxFor(t));
    }
    g_overlay->Show(boxes);
}

// Taking the mouse means the user gave up on keyboard hints: any
// button press or wheel, or moving past the threshold, dismisses them.
// The event itself is never swallowed, so that click still lands.
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        bool dismiss = false;
        switch (wParam) {
            case WM_MOUSEMOVE:
                dismiss = std::labs(ms->pt.x - g_mouseStart.x) > g_mouseThreshold ||
                          std::labs(ms->pt.y - g_mouseStart.y) > g_mouseThreshold;
                break;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                dismiss = true;
                break;
        }
        // Ignore synthetic input (other tools, remote desktop helpers).
        if (dismiss && !(ms->flags & LLMHF_INJECTED)) {
            PostMessageW(g_appWindow, WM_APP_CANCEL, g_generation, 0);
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

void StartMouseWatch() {
    if (g_mouseHook) return;
    GetCursorPos(&g_mouseStart);
    g_mouseThreshold = MulDiv(kMouseCancelDistance, GetDpiForSystem(), 96);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(nullptr), 0);
}

void StopMouseWatch() {
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
}

void Cancel() {
    StopMouseWatch();
    ++g_generation;
    g_mode = Mode::Idle;
    g_targets.clear();
    g_typed.clear();
    g_overlay->Hide();
}

void StartCollect() {
    HWND target = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!target) return;
    g_mode = Mode::Collecting;
    StartMouseWatch();
    StartUiaCollect(target, g_appWindow, WM_APP_HINTS, ++g_generation);
}

void OnHints(WPARAM generation, std::vector<UiaElement>* elements) {
    if (generation != g_generation || g_mode != Mode::Collecting) return;
    if (elements->empty()) {
        Log(L"no clickable elements found");
        Cancel();
        return;
    }
    const auto labels = MakeLabels(elements->size());
    g_targets.clear();
    for (size_t i = 0; i < elements->size(); ++i) {
        g_targets.push_back({(*elements)[i].rect, std::move((*elements)[i].name), labels[i]});
    }
    g_mode = Mode::Showing;
    Redraw();
}

void ClickAt(POINT p) {
    // We are per-monitor DPI aware, so UIA rects and SetCursorPos both
    // speak physical pixels; no scaling needed.
    SetCursorPos(p.x, p.y);
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

void OnActivate(size_t index) {
    if (index >= g_targets.size()) return;
    const RECT r = g_targets[index].rect;
    Cancel();
    ClickAt({(r.left + r.right) / 2, (r.top + r.bottom) / 2});
}

// Handles one key while the overlay owns the keyboard.
void OnHintKey(DWORD vk) {
    if (vk == VK_ESCAPE) {
        Cancel();
        return;
    }
    if (g_mode != Mode::Showing) return;  // Still collecting: swallow.

    if (vk == VK_BACK) {
        if (!g_typed.empty()) g_typed.pop_back();
        Redraw();
        return;
    }
    if (vk < 'A' || vk > 'Z') return;

    g_typed.push_back(static_cast<wchar_t>(vk));
    if (CountMatches() == 0) {
        g_typed.pop_back();  // Typo: ignore the key rather than cancel.
        return;
    }
    for (size_t i = 0; i < g_targets.size(); ++i) {
        if (g_targets[i].label == g_typed) {
            PostMessageW(g_appWindow, WM_APP_ACTIVATE, i, 0);
            return;
        }
    }
    Redraw();
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        // GetKeyState reflects only this thread's input queue, which a
        // low-level hook never syncs for keys going to other apps.
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

        if (kb->vkCode == VK_SPACE && ctrl && shift) {
            if (g_mode == Mode::Idle) {
                StartCollect();
            } else {
                Cancel();
            }
            return 1;
        }

        if (g_mode != Mode::Idle) {
            OnHintKey(kb->vkCode);
            // Swallow everything so typing doesn't leak into the
            // focused app while hints are up.
            return 1;
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // One instance per session: a second copy would install a second
    // hook and both would react to the hotkey. Released by the OS on
    // exit, so a crashed instance never blocks the next start.
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"Local\\rout-single-instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"already running, exiting");
        return 0;
    }

    // Must precede any window/metrics call: otherwise Windows scales us
    // and virtual-screen size and UIA rects (physical px) disagree.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Overlay overlay;
    if (!overlay.Create()) return 1;
    g_overlay = &overlay;

    // Hidden at start; reachable from the tray icon. Exit from the tray
    // menu destroys it, which posts WM_QUIT and ends the loop below.
    AppWindow appWindow;
    if (!appWindow.Create(hInstance)) return 1;
    g_appWindow = appWindow.Handle();
    appWindow.SetAppMessageHandler([](UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_APP_HINTS) {
            auto* elements = reinterpret_cast<std::vector<UiaElement>*>(lParam);
            OnHints(wParam, elements);
            delete elements;
        } else if (msg == WM_APP_ACTIVATE) {
            OnActivate(wParam);
        } else if (msg == WM_APP_CANCEL) {
            if (wParam == g_generation && g_mode != Mode::Idle) Cancel();
        }
    });

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    if (!g_hook) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopMouseWatch();
    UnhookWindowsHookEx(g_hook);
    CloseHandle(instanceMutex);
    return 0;
}
