#include "uia_hints.h"

#include <uiautomation.h>

#include <chrono>
#include <thread>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

// Chromium/Electron trees are deep and wide; bound the walk so the
// overlay appears quickly even if the result is incomplete.
constexpr int kMaxDepth = 40;
constexpr size_t kMaxTargets = 676;  // 26 * 26 two-letter labels.
constexpr auto kBudget = std::chrono::milliseconds(600);

// Minimal owning COM pointer (MinGW has no usable WRL ComPtr).
template <class T>
class Com {
public:
    Com() = default;
    ~Com() { Reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com& operator=(Com&& other) noexcept {
        std::swap(p_, other.p_);
        return *this;
    }

    T** Put() { Reset(); return &p_; }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    void Reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T* p_ = nullptr;
};

constexpr PROPERTYID kPatternProps[] = {
    UIA_IsInvokePatternAvailablePropertyId,
    UIA_IsTogglePatternAvailablePropertyId,
    UIA_IsSelectionItemPatternAvailablePropertyId,
    UIA_IsExpandCollapsePatternAvailablePropertyId,
};

bool CachedBool(IUIAutomationElement* el, PROPERTYID id) {
    VARIANT v;
    VariantInit(&v);
    const bool result = SUCCEEDED(el->GetCachedPropertyValue(id, &v)) &&
                        v.vt == VT_BOOL && v.boolVal == VARIANT_TRUE;
    VariantClear(&v);
    return result;
}

bool IsClickable(IUIAutomationElement* el) {
    for (PROPERTYID id : kPatternProps) {
        if (CachedBool(el, id)) return true;
    }
    // Some providers expose no patterns on obviously clickable things
    // (and edits have no "click" pattern at all, but focusing them is
    // exactly what a hint should do).
    CONTROLTYPEID type = 0;
    if (FAILED(el->get_CachedControlType(&type))) return false;
    switch (type) {
        case UIA_ButtonControlTypeId:
        case UIA_HyperlinkControlTypeId:
        case UIA_EditControlTypeId:
        case UIA_MenuItemControlTypeId:
        case UIA_TabItemControlTypeId:
        case UIA_CheckBoxControlTypeId:
            return true;
        default:
            return false;
    }
}

struct Walk {
    IUIAutomationTreeWalker* walker;
    IUIAutomationCacheRequest* cache;
    RECT bounds;  // Target window clipped to the virtual screen.
    Clock::time_point deadline;
    std::vector<RECT>* out;

    bool Exhausted() const {
        return out->size() >= kMaxTargets || Clock::now() > deadline;
    }
};

void Visit(Walk& w, IUIAutomationElement* el, int depth) {
    if (w.Exhausted()) return;

    BOOL offscreen = FALSE;
    RECT r{};
    el->get_CachedIsOffscreen(&offscreen);
    el->get_CachedBoundingRectangle(&r);
    RECT visible;
    if (!offscreen && IsClickable(el) && IntersectRect(&visible, &r, &w.bounds)) {
        // Nested providers often report the same rect twice (e.g. a
        // list item wrapping its own button); one hint is enough.
        bool duplicate = false;
        for (const RECT& existing : *w.out) {
            if (EqualRect(&existing, &visible)) { duplicate = true; break; }
        }
        if (!duplicate) w.out->push_back(visible);
    }

    if (depth >= kMaxDepth) return;

    Com<IUIAutomationElement> child;
    w.walker->GetFirstChildElementBuildCache(el, w.cache, child.Put());
    while (child && !w.Exhausted()) {
        Visit(w, child.Get(), depth + 1);
        Com<IUIAutomationElement> next;
        w.walker->GetNextSiblingElementBuildCache(child.Get(), w.cache, next.Put());
        child = std::move(next);
    }
}

std::vector<RECT> Collect(HWND target) {
    std::vector<RECT> out;

    Com<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void**>(uia.Put())))) {
        return out;
    }

    Com<IUIAutomationCacheRequest> cache;
    if (FAILED(uia->CreateCacheRequest(cache.Put()))) return out;
    cache->AddProperty(UIA_BoundingRectanglePropertyId);
    cache->AddProperty(UIA_IsOffscreenPropertyId);
    cache->AddProperty(UIA_ControlTypePropertyId);
    for (PROPERTYID id : kPatternProps) cache->AddProperty(id);

    Com<IUIAutomationTreeWalker> walker;
    if (FAILED(uia->get_ControlViewWalker(walker.Put()))) return out;

    Com<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandleBuildCache(target, cache.Get(), root.Put())) || !root) {
        return out;
    }

    RECT window{};
    GetWindowRect(target, &window);
    const RECT screen{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
    Walk w{walker.Get(), cache.Get(), {}, Clock::now() + kBudget, &out};
    IntersectRect(&w.bounds, &window, &screen);

    Visit(w, root.Get(), 0);
    return out;
}

}  // namespace

void StartUiaCollect(HWND target, DWORD replyThread, UINT replyMsg, WPARAM tag) {
    std::thread([=] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* rects = new std::vector<RECT>(Collect(target));
        CoUninitialize();
        if (!PostThreadMessageW(replyThread, replyMsg, tag, reinterpret_cast<LPARAM>(rects))) {
            delete rects;
        }
    }).detach();
}
