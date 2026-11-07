#include <windows.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app_window.h"
#include "overlay.h"
#include "scroller.h"
#include "uia_hints.h"

namespace {

// Messages posted to the app window and handled on the main thread.
// Anything slow or input-injecting happens there, never inside the
// low-level hook callback: Windows silently unhooks a hook that exceeds
// LowLevelHooksTimeout.
constexpr UINT WM_APP_HINTS = kAppMessageFirst;         // wParam=generation, lParam=std::vector<UiaElement>*
constexpr UINT WM_APP_ACTIVATE = kAppMessageFirst + 1;  // wParam=index into g_targets
constexpr UINT WM_APP_CANCEL = kAppMessageFirst + 2;    // wParam=generation
constexpr UINT WM_APP_ALT_MASK = kAppMessageFirst + 3;  // Scroll key typed with Alt held.

// Unassigned virtual key, injected after a swallowed Alt+key so that
// releasing Alt doesn't read as a bare Alt tap, which opens the menu bar
// in Notepad, Explorer and most classic apps.
constexpr WORD kMenuMaskKey = 0xE8;

// M1c: scroll mode ends after this long without a scroll key.
constexpr UINT kScrollIdleTimeoutMs = 10000;

// How far the mouse may drift (at 96 DPI) before hints are dismissed;
// small enough to react to a deliberate move, large enough to ignore a
// bumped desk.
constexpr int kMouseCancelDistance = 16;

// Home row first, so the most common hints are the easiest to type.
constexpr wchar_t kAlphabet[] = L"ASDFGHJKLQWERTYUIOPZXCVBNM";
constexpr size_t kAlphabetSize = sizeof(kAlphabet) / sizeof(kAlphabet[0]) - 1;

enum class Mode { Idle, Collecting, Showing, Scrolling };

// While showing: do letters pick a label, or type the name query?
enum class Phase { Labels, Search };

struct Target {
    RECT rect;
    std::wstring nameLower;  // Lowercased UIA name, for M1b search.
    std::wstring label;      // Empty when filtered out by the query.
    bool scroll = false;     // A scroll region (M1c): picking it scrolls, not clicks.
    std::shared_ptr<IUIAutomationElement> element;  // Scroll regions only.
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
Phase g_phase = Phase::Labels;
// Bumped on every start/cancel so a late UIA result for an abandoned
// request is dropped instead of popping up an overlay.
WPARAM g_generation = 0;
HWND g_targetWindow = nullptr;
std::vector<Target> g_targets;
std::vector<size_t> g_active;  // Targets matching the query, in order.
std::wstring g_query;          // M1b name search, as typed.
std::wstring g_typed;          // Label prefix typed so far.
// Ends hints / scroll mode when another window comes to the front
// (Alt+Tab, a notification, a dialog of another app).
HWINEVENTHOOK g_foregroundHook = nullptr;
Scroller g_scroller;
RECT g_scrollRect{};
UINT_PTR g_idleTimer = 0;

void Log(const std::wstring& text) {
    OutputDebugStringW((L"rout: " + text + L"\n").c_str());
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) {
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), static_cast<int>(s.size()),
                      s.data(), static_cast<int>(s.size()), nullptr, nullptr, 0);
    }
    return s;
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

// Recomputes the active set from the query and hands out labels over
// it, so a narrow search gets short (single-letter) labels again.
void Refilter() {
    const std::wstring q = Lower(g_query);
    g_active.clear();
    for (size_t i = 0; i < g_targets.size(); ++i) {
        // An empty query keeps everything; a non-empty one drops
        // unnamed elements, since find() on "" never matches q.
        if (q.empty() || g_targets[i].nameLower.find(q) != std::wstring::npos) g_active.push_back(i);
    }
    const auto labels = MakeLabels(g_active.size());
    for (auto& t : g_targets) t.label.clear();
    for (size_t k = 0; k < g_active.size(); ++k) g_targets[g_active[k]].label = labels[k];
    g_typed.clear();
}

size_t CountMatches() {
    size_t n = 0;
    for (size_t i : g_active) {
        if (g_targets[i].label.starts_with(g_typed)) ++n;
    }
    return n;
}

Banner SearchBanner() {
    if (g_phase != Phase::Search && g_query.empty()) return {};
    RECT window{};
    GetWindowRect(g_targetWindow, &window);
    const int pad = MulDiv(12, GetDpiForWindow(g_targetWindow), 96);
    std::wstring text = L"/" + g_query;
    if (g_phase == Phase::Search) text += L"│";  // Caret while typing.
    text += L"   " + std::to_wstring(g_active.size());
    return {{window.left + pad, window.bottom - pad}, text};
}

void Redraw() {
    std::vector<HintBox> boxes;
    for (size_t i : g_active) {
        const Target& t = g_targets[i];
        // Label box anchored at the element's top-left corner.
        if (!t.label.starts_with(g_typed)) continue;
        if (t.scroll) {
            boxes.push_back({{t.rect.right, t.rect.top}, t.label, true});  // Top-right corner.
        } else {
            boxes.push_back({{t.rect.left, t.rect.top}, t.label});  // Top-left corner.
        }
    }
    g_overlay->Show(boxes, SearchBanner());
}

// Taking the mouse means the user gave up on keyboard hints: any
// button press or wheel, or moving past the threshold, dismisses them.
// The event itself is never swallowed, so that click still lands.
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        ScrollerNoteMouse(ms);
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

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (hwnd && GetAncestor(hwnd, GA_ROOT) != g_targetWindow) {
        PostMessageW(g_appWindow, WM_APP_CANCEL, g_generation, 0);
    }
}

void StartForegroundWatch() {
    if (g_foregroundHook) return;
    // Out of context: the callback runs on this thread from the message
    // loop, no DLL injection.
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                       OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
}

void StopForegroundWatch() {
    if (g_foregroundHook) UnhookWinEvent(g_foregroundHook);
    g_foregroundHook = nullptr;
}

void StopIdleTimer() {
    if (g_idleTimer) KillTimer(nullptr, g_idleTimer);
    g_idleTimer = 0;
}

void Cancel() {
    if (g_mode == Mode::Scrolling) g_scroller.End();
    StopIdleTimer();
    StopForegroundWatch();
    StopMouseWatch();
    ++g_generation;
    g_mode = Mode::Idle;
    g_phase = Phase::Labels;
    g_targets.clear();
    g_active.clear();
    g_query.clear();
    g_typed.clear();
    g_overlay->Hide();
}

void StartCollect() {
    HWND target = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!target) return;
    g_targetWindow = target;
    g_mode = Mode::Collecting;
    StartMouseWatch();
    StartForegroundWatch();
    StartUiaCollect(target, g_appWindow, WM_APP_HINTS, ++g_generation);
}

void OnHints(WPARAM generation, std::vector<UiaElement>* elements) {
    if (generation != g_generation || g_mode != Mode::Collecting) return;
    if (elements->empty()) {
        Log(L"no clickable elements found");
        Cancel();
        return;
    }
    g_targets.clear();
    for (const UiaElement& e : *elements) {
        g_targets.push_back({e.rect, Lower(e.name), {}, e.scroll, e.element});
    }
    g_mode = Mode::Showing;
    g_phase = Phase::Labels;
    Refilter();
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

void CALLBACK OnIdleTimeout(HWND, UINT, UINT_PTR, DWORD) {
    if (g_mode == Mode::Scrolling) Cancel();
}

void RestartIdleTimer() {
    StopIdleTimer();
    g_idleTimer = SetTimer(nullptr, 0, kScrollIdleTimeoutMs, OnIdleTimeout);
}

// The keyboard stays captured in scroll mode, so it must be visible:
// a frame around the region and a badge in its corner.
void ShowScrollIndicator() {
    const int pad = MulDiv(8, GetDpiForWindow(g_targetWindow), 96);
    g_overlay->Show({}, {{g_scrollRect.left + pad, g_scrollRect.bottom - pad}, L"SCROLL"}, g_scrollRect);
}

// Hint mode hands over to scroll mode; the mouse and foreground watches
// keep running, with the same generation, and end it the same way.
void EnterScroll(const Target& target) {
    g_scrollRect = target.rect;
    g_scroller.Begin(target.element, target.rect, g_appWindow, WM_APP_CANCEL, g_generation);
    g_mode = Mode::Scrolling;
    g_phase = Phase::Labels;
    g_targets.clear();
    g_active.clear();
    g_query.clear();
    g_typed.clear();
    ShowScrollIndicator();
    RestartIdleTimer();
}

void OnActivate(size_t index) {
    if (g_mode != Mode::Showing || index >= g_targets.size()) return;
    if (g_targets[index].scroll) {
        EnterScroll(g_targets[index]);
        return;
    }
    const RECT r = g_targets[index].rect;
    Cancel();
    ClickAt({(r.left + r.right) / 2, (r.top + r.bottom) / 2});
}

// Emacs-style keys; anything else is not a scroll key.
std::optional<ScrollStep> ScrollStepForKey(DWORD vk, bool ctrl, bool alt) {
    if (ctrl && !alt) {
        switch (vk) {
            case 'N': return ScrollStep::LineDown;
            case 'P': return ScrollStep::LineUp;
            case 'V': return ScrollStep::PageDown;
            case 'F': return ScrollStep::Right;
            case 'B': return ScrollStep::Left;
        }
    } else if (alt && !ctrl) {
        switch (vk) {
            case 'V': return ScrollStep::PageUp;
            case VK_OEM_COMMA: return ScrollStep::Top;      // M-< (Shift optional).
            case VK_OEM_PERIOD: return ScrollStep::Bottom;  // M->
        }
    }
    return std::nullopt;
}

bool IsModifierKey(DWORD vk) {
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
        case kMenuMaskKey:
            return true;
    }
    return false;
}

// Returns true to swallow the key. Like isearch in Emacs, a key that is
// not a scroll key ends the mode and goes through to the app, so the
// user can just start typing.
bool OnScrollKey(const KBDLLHOOKSTRUCT* kb, bool ctrl, bool alt) {
    // Bare modifiers pass through untouched: they are half of every
    // scroll key.
    if (IsModifierKey(kb->vkCode)) return false;
    if (kb->vkCode == VK_ESCAPE || (ctrl && !alt && kb->vkCode == 'G')) {
        Cancel();
        return true;
    }
    const auto step = ScrollStepForKey(kb->vkCode, ctrl, alt);
    if (!step) {
        Cancel();
        return false;
    }
    g_scroller.Step(*step);
    if (alt) PostMessageW(g_appWindow, WM_APP_ALT_MASK, 0, 0);
    RestartIdleTimer();
    return true;
}

void SendMenuMaskKey() {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = kMenuMaskKey;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// The character a key would type in the target app's keyboard layout,
// or 0 for non-printing keys. Flag 0x4 keeps ToUnicodeEx from touching
// dead-key state, so the app's own typing isn't disturbed.
wchar_t CharFromKey(const KBDLLHOOKSTRUCT* kb) {
    BYTE state[256]{};
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
    const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(g_targetWindow, nullptr));
    wchar_t buf[4]{};
    const int n = ToUnicodeEx(kb->vkCode, kb->scanCode, state, buf, 4, 0x4, layout);
    return (n == 1 && buf[0] >= L' ') ? buf[0] : 0;
}

void OnSearchKey(const KBDLLHOOKSTRUCT* kb) {
    switch (kb->vkCode) {
        case VK_RETURN:
            // One match: go. Several: freeze the query and pick by label.
            if (g_active.size() == 1) {
                PostMessageW(g_appWindow, WM_APP_ACTIVATE, g_active[0], 0);
            } else if (!g_active.empty()) {
                g_phase = Phase::Labels;
                Redraw();
            }
            return;
        case VK_BACK:
            if (g_query.empty()) {
                g_phase = Phase::Labels;  // Back to plain hints.
            } else {
                g_query.pop_back();
            }
            Refilter();
            Redraw();
            return;
    }
    if (const wchar_t c = CharFromKey(kb)) {
        g_query.push_back(c);
        Refilter();
        Redraw();
    }
}

void OnLabelKey(DWORD vk) {
    if (vk == VK_OEM_2 && g_typed.empty()) {  // '/' starts a name search.
        g_phase = Phase::Search;
        Redraw();
        return;
    }
    if (vk == VK_BACK) {
        if (!g_typed.empty()) {
            g_typed.pop_back();
        } else if (!g_query.empty()) {
            g_phase = Phase::Search;  // Reopen the frozen query for editing.
        }
        Redraw();
        return;
    }
    if (vk < 'A' || vk > 'Z') return;

    g_typed.push_back(static_cast<wchar_t>(vk));
    if (CountMatches() == 0) {
        g_typed.pop_back();  // Typo: ignore the key rather than cancel.
        return;
    }
    for (size_t i : g_active) {
        if (g_targets[i].label == g_typed) {
            PostMessageW(g_appWindow, WM_APP_ACTIVATE, i, 0);
            return;
        }
    }
    Redraw();
}

// Handles one key while the overlay owns the keyboard.
void OnHintKey(const KBDLLHOOKSTRUCT* kb) {
    if (kb->vkCode == VK_ESCAPE) {
        Cancel();
        return;
    }
    if (g_mode != Mode::Showing) return;  // Still collecting: swallow.
    if (g_phase == Phase::Search) {
        OnSearchKey(kb);
    } else {
        OnLabelKey(kb->vkCode);
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) ScrollerNoteKey(reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam), wParam);
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

        if (g_mode == Mode::Scrolling) {
            if (OnScrollKey(kb, PhysicalCtrlHeld(), PhysicalAltHeld())) return 1;
            return CallNextHookEx(g_hook, nCode, wParam, lParam);
        }

        if (g_mode != Mode::Idle) {
            OnHintKey(kb);
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

    // First: its MTA thread keeps collected UIA elements alive.
    g_scroller.Start();

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
        } else if (msg == WM_APP_ALT_MASK) {
            SendMenuMaskKey();
        }
    });

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    if (!g_hook) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Cancel();
    UnhookWindowsHookEx(g_hook);
    g_scroller.Stop();
    CloseHandle(instanceMutex);
    return 0;
}
