#pragma once

#include <windows.h>
#include <uiautomation.h>

#include <memory>
#include <string>
#include <vector>

struct UiaElement {
    RECT rect;          // Visible part, screen coordinates, physical px.
    std::wstring name;  // UIA Name ("Reload", "Address Bar"); may be empty.
    // A scrollable region (M1c) rather than something to click. The
    // element is kept so scroll mode can drive its ScrollPattern; it is
    // an MTA object, usable from any thread while the Scroller's MTA
    // thread keeps the apartment alive.
    bool scroll = false;
    std::shared_ptr<IUIAutomationElement> element;
};

// Generic UI Automation hint source (M1): used for any app without a
// live adapter. Walks the target window's control view and returns the
// on-screen elements that look clickable, plus scrollable regions.
//
// Runs on its own MTA thread, because a walk over a Chromium/Electron
// tree can take hundreds of ms and must never block the thread that
// services the low-level keyboard hook. When done, posts `replyMsg` to
// `replyWindow` with wParam = `tag` and lParam = a heap-allocated
// std::vector<UiaElement>* the receiver must delete.
//
// With `region` (a scroll region from an earlier result), only that
// region's subtree is walked: hints inside it, the region itself left out.
void StartUiaCollect(HWND target, HWND replyWindow, UINT replyMsg, WPARAM tag,
                     std::shared_ptr<IUIAutomationElement> region = {});

// For the direct scroll hotkey; replies like StartUiaCollect. One element:
// the scroll region around the focused element. Otherwise all scroll
// regions in the window (possibly none), for the user to pick from.
void StartScrollTargetCollect(HWND target, HWND replyWindow, UINT replyMsg, WPARAM tag);
