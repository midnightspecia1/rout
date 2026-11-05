#include <windows.h>

#include <string>
#include <vector>

#include "app_window.h"
#include "overlay.h"
#include "uia_hints.h"

namespace {

// Thread messages to the main loop. Anything slow or input-injecting
// happens there, never inside the low-level hook callback: Windows
// silently unhooks a hook that exceeds LowLevelHooksTimeout.
constexpr UINT WM_APP_HINTS = WM_APP + 1;     // wParam=generation, lParam=std::vector<RECT>*
constexpr UINT WM_APP_ACTIVATE = WM_APP + 2;  // wParam=index into g_targets

// Home row first, so the most common hints are the easiest to type.
constexpr wchar_t kAlphabet[] = L"ASDFGHJKLQWERTYUIOPZXCVBNM";
constexpr size_t kAlphabetSize = sizeof(kAlphabet) / sizeof(kAlphabet[0]) - 1;

enum class Mode { Idle, Collecting, Showing };

struct Target {
    RECT rect;
    std::wstring label;
};

Overlay* g_overlay = nullptr;
HHOOK g_hook = nullptr;
DWORD g_mainThread = 0;
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
    const LONG w = 8 + 9 * static_cast<LONG>(t.label.size());
    return {{t.rect.left, t.rect.top, t.rect.left + w, t.rect.top + 18}, t.label};
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

void Cancel() {
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
    StartUiaCollect(target, g_mainThread, WM_APP_HINTS, ++g_generation);
}

void OnHints(WPARAM generation, std::vector<RECT>* rects) {
    if (generation != g_generation || g_mode != Mode::Collecting) return;
    if (rects->empty()) {
        Log(L"no clickable elements found");
        g_mode = Mode::Idle;
        return;
    }
    const auto labels = MakeLabels(rects->size());
    g_targets.clear();
    for (size_t i = 0; i < rects->size(); ++i) {
        g_targets.push_back({(*rects)[i], labels[i]});
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
            PostThreadMessageW(g_mainThread, WM_APP_ACTIVATE, i, 0);
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
    g_mainThread = GetCurrentThreadId();

    // Hidden at start; reachable from the tray icon. Exit from the tray
    // menu destroys it, which posts WM_QUIT and ends the loop below.
    AppWindow appWindow;
    if (!appWindow.Create(hInstance)) return 1;

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    if (!g_hook) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.hwnd == nullptr && msg.message == WM_APP_HINTS) {
            auto* rects = reinterpret_cast<std::vector<RECT>*>(msg.lParam);
            OnHints(msg.wParam, rects);
            delete rects;
            continue;
        }
        if (msg.hwnd == nullptr && msg.message == WM_APP_ACTIVATE) {
            OnActivate(msg.wParam);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    CloseHandle(instanceMutex);
    return 0;
}
