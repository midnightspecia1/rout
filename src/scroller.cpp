#include "scroller.h"

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

// Wheel notches per step for the fallback path. Top/bottom have no
// wheel equivalent; a big burst gets there on any realistic page.
int WheelNotches(ScrollStep step) {
    switch (step) {
        case ScrollStep::LineDown: return -1;
        case ScrollStep::LineUp: return 1;
        case ScrollStep::PageDown: return -5;
        case ScrollStep::PageUp: return 5;
        case ScrollStep::Bottom: return -200;
        case ScrollStep::Top: return 200;
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

// The wheel goes to the window under the cursor, so the cursor visits
// the region's centre and comes back. All events are injected, which
// the dismiss-on-mouse hook ignores.
//
// The user is still holding the scroll chord's modifiers, and apps read
// them for wheel events: Ctrl+wheel zooms a browser page, Shift+wheel
// scrolls sideways. So held modifiers are released around the wheel and
// pressed again after it, in one SendInput call so no real input can
// interleave.
void ScrollWithWheel(const RECT& region, ScrollStep step) {
    POINT saved{};
    GetCursorPos(&saved);

    std::vector<WORD> held;
    for (WORD vk : kModifiers) {
        if (GetAsyncKeyState(vk) & 0x8000) held.push_back(vk);
    }
    const bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

    std::vector<INPUT> in;
    if (altHeld) {
        in.push_back(Key(kMenuMaskKey, false));
        in.push_back(Key(kMenuMaskKey, true));
    }
    for (WORD vk : held) in.push_back(Key(vk, true));

    in.push_back(MoveTo({(region.left + region.right) / 2, (region.top + region.bottom) / 2}));
    INPUT wheel{};
    wheel.type = INPUT_MOUSE;
    wheel.mi.dwFlags = IsHorizontal(step) ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    wheel.mi.mouseData = static_cast<DWORD>(WheelNotches(step) * WHEEL_DELTA);
    in.push_back(wheel);
    in.push_back(MoveTo(saved));

    for (WORD vk : held) in.push_back(Key(vk, false));
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

}  // namespace

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
