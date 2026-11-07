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

bool HasActionPattern(IUIAutomationElement* el) {
    for (PROPERTYID id : kPatternProps) {
        if (CachedBool(el, id)) return true;
    }
    return false;
}

enum class Kind {
    None,
    Control,  // Interactive by type alone.
    Wrapper,  // Item that often just wraps a real control (list item
              // around a link); hinted only if nothing inside is.
};

// The control type decides, not the patterns: Firefox reports Invoke on
// nearly everything (windows, groups, separators, toolbars, headings),
// so a pattern alone says nothing about clickability.
Kind Classify(IUIAutomationElement* el) {
    CONTROLTYPEID type = 0;
    if (FAILED(el->get_CachedControlType(&type))) return Kind::None;
    switch (type) {
        case UIA_ButtonControlTypeId:
        case UIA_SplitButtonControlTypeId:
        case UIA_HyperlinkControlTypeId:
        case UIA_EditControlTypeId:  // No click pattern, but focusing is the point.
        case UIA_ComboBoxControlTypeId:
        case UIA_CheckBoxControlTypeId:
        case UIA_RadioButtonControlTypeId:
        case UIA_MenuItemControlTypeId:
        case UIA_TabItemControlTypeId:
            return Kind::Control;
        case UIA_ListItemControlTypeId:
        case UIA_TreeItemControlTypeId:
        case UIA_DataItemControlTypeId:
        case UIA_CustomControlTypeId:
            return HasActionPattern(el) ? Kind::Wrapper : Kind::None;
        default:
            return Kind::None;
    }
}

constexpr LONG kMinSide = 6;

struct Walk {
    HWND target;
    IUIAutomationTreeWalker* walker;
    IUIAutomationCacheRequest* cache;
    RECT bounds;  // Target window clipped to the virtual screen.
    Clock::time_point deadline;
    std::vector<RECT>* out;

    bool Exhausted() const {
        return out->size() >= kMaxTargets || Clock::now() > deadline;
    }

    // Covered by another top-level window (a dialog, an always-on-top
    // tool) at the point we would click.
    bool Occluded(const RECT& r) const {
        const POINT center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        return GetAncestor(WindowFromPoint(center), GA_ROOT) != target;
    }

    bool IsDuplicate(const RECT& r) const {
        for (const RECT& existing : *out) {
            if (EqualRect(&existing, &r)) return true;
        }
        return false;
    }
};

void Visit(Walk& w, IUIAutomationElement* el, int depth) {
    if (w.Exhausted()) return;

    BOOL offscreen = FALSE;
    BOOL enabled = TRUE;
    RECT r{};
    el->get_CachedIsOffscreen(&offscreen);
    el->get_CachedIsEnabled(&enabled);
    el->get_CachedBoundingRectangle(&r);
    RECT visible{};
    const bool inBounds = IntersectRect(&visible, &r, &w.bounds);

    // Scrolled-out page content: thousands of elements in a long web
    // page, none of them hintable. Skipping the subtree is what keeps
    // the walk inside its budget. Only prune when the element has a
    // real rect that lies outside: an empty rect means "no geometry",
    // not "off screen" — Win11 Explorer's title bar reports offscreen
    // with a 0x0 rect while its min/max/close buttons are visible.
    if (offscreen && !inBounds && !IsRectEmpty(&r)) return;

    const size_t self = w.out->size();
    bool added = false;
    const Kind kind = Classify(el);
    if (kind != Kind::None && !offscreen && enabled && inBounds &&
        visible.right - visible.left >= kMinSide && visible.bottom - visible.top >= kMinSide &&
        !w.Occluded(visible) && !w.IsDuplicate(visible)) {
        w.out->push_back(visible);
        added = true;
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

    // A wrapper with a real control inside: hint the inner one only.
    if (added && kind == Kind::Wrapper && w.out->size() > self + 1) {
        w.out->erase(w.out->begin() + self);
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
    cache->AddProperty(UIA_IsEnabledPropertyId);
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
    Walk w{target, walker.Get(), cache.Get(), {}, Clock::now() + kBudget, &out};
    IntersectRect(&w.bounds, &window, &screen);

    Visit(w, root.Get(), 0);
    return out;
}

}  // namespace

void StartUiaCollect(HWND target, HWND replyWindow, UINT replyMsg, WPARAM tag) {
    std::thread([=] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* rects = new std::vector<RECT>(Collect(target));
        CoUninitialize();
        if (!PostMessageW(replyWindow, replyMsg, tag, reinterpret_cast<LPARAM>(rects))) {
            delete rects;
        }
    }).detach();
}
