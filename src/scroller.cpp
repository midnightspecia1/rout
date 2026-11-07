#include "scroller.h"

#include <atomic>
#include <cstdlib>
#include <iterator>
#include <future>
#include <utility>
#include <vector>

namespace {

#ifndef UIA_E_ELEMENTNOTAVAILABLE
constexpr HRESULT UIA_E_ELEMENTNOTAVAILABLE = static_cast<HRESULT>(0x80040201);
#endif

// UIA_ScrollPatternNoScroll: "leave this axis alone" for SetScrollPercent.
constexpr double kNoScroll = -1.0;

// Key autorepeat against a stalled app would otherwise pile up a burst
// of scrolls that plays out after the user let go.
constexpr size_t kMaxPendingSteps = 3;

// Top/bottom have no wheel equivalent; this many notches reaches the end
// of any realistic page (~15k px at 3 lines per notch).
constexpr int kEndNotches = 300;

// How many notches make a page: ~80% of the region's height, so a
// little context stays on screen, as with PageDown. One notch scrolls
// SPI_GETWHEELSCROLLLINES lines; a line is taken as ~20 px at 96 DPI,
// which is about what browsers and native lists use.
int PageNotches(const RECT& region) {
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == 0 || lines == WHEEL_PAGESCROLL) lines = 3;
    const int dpi = static_cast<int>(GetDpiForSystem());
    const int notchPx = MulDiv(static_cast<int>(lines) * 20, dpi, 96);
    const int pagePx = (region.bottom - region.top) * 4 / 5;
    return pagePx > notchPx ? pagePx / notchPx : 1;
}

// Signed notch count for the fallback path; positive is up / right.
int WheelNotches(ScrollStep step, const RECT& region) {
    switch (step) {
        case ScrollStep::LineDown: return -1;
        case ScrollStep::LineUp: return 1;
        case ScrollStep::PageDown: return -PageNotches(region);
        case ScrollStep::PageUp: return PageNotches(region);
        case ScrollStep::Bottom: return -kEndNotches;
        case ScrollStep::Top: return kEndNotches;
        case ScrollStep::Right: return 1;
        case ScrollStep::Left: return -1;
    }
    return 0;
}

bool IsHorizontal(ScrollStep step) { return step == ScrollStep::Right || step == ScrollStep::Left; }

HRESULT ScrollWithPattern(IUIAutomationScrollPattern* p, ScrollStep step) {
    switch (step) {
        case ScrollStep::LineDown: return p->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallIncrement);
        case ScrollStep::LineUp: return p->Scroll(ScrollAmount_NoAmount, ScrollAmount_SmallDecrement);
        case ScrollStep::PageDown: return p->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement);
        case ScrollStep::PageUp: return p->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeDecrement);
        case ScrollStep::Top: return p->SetScrollPercent(kNoScroll, 0.0);
        case ScrollStep::Bottom: return p->SetScrollPercent(kNoScroll, 100.0);
        case ScrollStep::Right: return p->Scroll(ScrollAmount_SmallIncrement, ScrollAmount_NoAmount);
        case ScrollStep::Left: return p->Scroll(ScrollAmount_SmallDecrement, ScrollAmount_NoAmount);
    }
    return E_INVALIDARG;
}

// Absolute SendInput coordinates span the virtual desktop as 0..65535.
INPUT MoveTo(POINT p) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = MulDiv(p.x - vx, 65535, vw - 1);
    in.mi.dy = MulDiv(p.y - vy, 65535, vh - 1);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return in;
}

INPUT Key(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    // Right-hand modifiers are extended keys; without the flag a
    // synthetic RCtrl/RAlt would come out as the left one.
    if (vk == VK_RCONTROL || vk == VK_RMENU) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return in;
}

// Unassigned virtual key; tapped before releasing Alt so the release
// doesn't read as a bare Alt tap (which opens the menu bar).
constexpr WORD kMenuMaskKey = 0xE8;

constexpr WORD kModifiers[] = {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT};

// Which modifiers the user physically holds, one bit per kModifiers
// entry. Kept from real (non-injected) key events, because the async key
// state also reflects our own synthetic releases and presses.
std::atomic<unsigned> g_physical{0};

// Tags the last event of a wheel burst, so the mouse hook can tell the
// worker the burst has been processed.
constexpr ULONG_PTR kBurstMarker = 0x726F7574;  // 'rout'
HANDLE g_burstDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);

int ModifierIndex(DWORD vk) {
    for (int i = 0; i < static_cast<int>(std::size(kModifiers)); ++i) {
        if (kModifiers[i] == vk) return i;
    }
    return -1;
}

bool PhysicallyHeld(int i) { return (g_physical.load() >> i) & 1u; }

INPUT MaskTap(bool up) { return Key(kMenuMaskKey, up); }

// The wheel goes to the window under the cursor, so the cursor visits
// the region's centre and comes back. All events are injected, which
// the dismiss-on-mouse hook ignores.
//
// The user is still holding the scroll chord's modifiers, and apps read
// them for wheel events: Ctrl+wheel zooms a browser page, Alt+wheel goes
// back in history, Shift+wheel scrolls sideways. So held modifiers are
// released for the burst and pressed again after it.
//
// A long burst (top/bottom: hundreds of events) takes a noticeable time
// to go through the system's hooks, and the user often lets go of the
// keys meanwhile. Re-pressing blindly would then leave Alt logically
// stuck, and the next keys reach the app as Alt+letter (Firefox beeps).
// So the worker waits until the burst has been processed and re-presses
// only what the user still holds.
void ScrollWithWheel(const RECT& region, ScrollStep step) {
    POINT saved{};
    GetCursorPos(&saved);

    std::vector<int> held;
    for (int i = 0; i < static_cast<int>(std::size(kModifiers)); ++i) {
        if (PhysicallyHeld(i) && (GetAsyncKeyState(kModifiers[i]) & 0x8000)) held.push_back(i);
    }
    auto isAlt = [](int i) { return kModifiers[i] == VK_LMENU || kModifiers[i] == VK_RMENU; };
    bool altHeld = false;
    for (int i : held) altHeld |= isAlt(i);

    std::vector<INPUT> in;
    if (altHeld) {
        in.push_back(MaskTap(false));
        in.push_back(MaskTap(true));
    }
    for (int i : held) in.push_back(Key(kModifiers[i], true));

    in.push_back(MoveTo({(region.left + region.right) / 2, (region.top + region.bottom) / 2}));
    // One event per notch: browsers clamp the delta of a single event,
    // so a page or a jump-to-end sent as one big delta moved no further
    // than a few notches.
    const int notches = WheelNotches(step, region);
    INPUT wheel{};
    wheel.type = INPUT_MOUSE;
    wheel.mi.dwFlags = IsHorizontal(step) ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    wheel.mi.mouseData = static_cast<DWORD>(notches > 0 ? WHEEL_DELTA : -WHEEL_DELTA);
    for (int i = 0; i < std::abs(notches); ++i) in.push_back(wheel);
    in.push_back(MoveTo(saved));
    in.back().mi.dwExtraInfo = kBurstMarker;

    ResetEvent(g_burstDone);
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
    if (held.empty()) return;

    // Real key-ups queued behind the burst have reached the keyboard hook
    // by the time the marker reaches the mouse hook. Without the mouse
    // hook (not expected in scroll mode) the timeout still bounds this.
    WaitForSingleObject(g_burstDone, 1000);

    std::vector<INPUT> restore;
    bool altRestored = false;
    for (int i : held) {
        if (!PhysicallyHeld(i)) continue;
        restore.push_back(Key(kModifiers[i], false));
        altRestored |= isAlt(i);
    }
    // The user will release the restored Alt (or Alt+Shift) with no key
    // in between as far as the system knows: mask it, or that release
    // opens the menu bar or switches the keyboard layout.
    if (altRestored) {
        restore.push_back(MaskTap(false));
        restore.push_back(MaskTap(true));
    }
    if (!restore.empty()) SendInput(static_cast<UINT>(restore.size()), restore.data(), sizeof(INPUT));
}

}  // namespace

void ScrollerNoteKey(const KBDLLHOOKSTRUCT* kb, WPARAM msg) {
    if (kb->flags & LLKHF_INJECTED) return;
    const int i = ModifierIndex(kb->vkCode);
    if (i < 0) return;
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (down) {
        g_physical.fetch_or(1u << i);
    } else {
        g_physical.fetch_and(~(1u << i));
    }
}

void ScrollerNoteMouse(const MSLLHOOKSTRUCT* ms) {
    if (ms->dwExtraInfo == kBurstMarker) SetEvent(g_burstDone);
}

void Scroller::Start() {
    std::promise<void> ready;
    auto started = ready.get_future();
    thread_ = std::thread([this, &ready] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ready.set_value();
        Run();
        DoEnd();
        CoUninitialize();
    });
    started.wait();
}

void Scroller::Stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_one();
    thread_.join();
}

void Scroller::Begin(std::shared_ptr<IUIAutomationElement> region, RECT rect, HWND replyWindow,
                     UINT goneMsg, WPARAM tag) {
    Post([this, region = std::move(region), rect, replyWindow, goneMsg, tag]() mutable {
        DoBegin(std::move(region), rect, replyWindow, goneMsg, tag);
    });
}

void Scroller::Step(ScrollStep step) {
    Post([this, step] { DoStep(step); }, true);
}

void Scroller::End() {
    Post([this] { DoEnd(); });
}

void Scroller::Post(std::function<void()> task, bool droppable) {
    {
        std::lock_guard lock(mutex_);
        if (droppable && tasks_.size() >= kMaxPendingSteps) return;
        tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void Scroller::Run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || !tasks_.empty(); });
            if (quit_) return;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

void Scroller::DoBegin(std::shared_ptr<IUIAutomationElement> region, RECT rect, HWND replyWindow,
                       UINT goneMsg, WPARAM tag) {
    DoEnd();
    region_ = std::move(region);
    rect_ = rect;
    replyWindow_ = replyWindow;
    goneMsg_ = goneMsg;
    tag_ = tag;
    if (!region_) return;
    // No pattern is fine: Steps fall back to the wheel.
    region_->GetCurrentPatternAs(UIA_ScrollPatternId, IID_IUIAutomationScrollPattern,
                                 reinterpret_cast<void**>(&pattern_));
}

void Scroller::DoStep(ScrollStep step) {
    if (!region_) return;
    if (pattern_) {
        const HRESULT hr = ScrollWithPattern(pattern_, step);
        if (SUCCEEDED(hr)) return;
        if (hr == UIA_E_ELEMENTNOTAVAILABLE) {
            PostMessageW(replyWindow_, goneMsg_, tag_, 0);
            DoEnd();
            return;
        }
        // Other failures (an axis the pattern refuses, an app that
        // implements it half-way): the wheel may still work.
    }
    ScrollWithWheel(rect_, step);
}

void Scroller::DoEnd() {
    if (pattern_) pattern_->Release();
    pattern_ = nullptr;
    region_.reset();
}
