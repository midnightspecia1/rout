#pragma once

#include <windows.h>
#include <uiautomation.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

enum class ScrollStep { LineDown, LineUp, PageDown, PageUp, Top, Bottom, Right, Left };

// M1c scroll engine: drives one scroll region at a time.
//
// Scrolling is a cross-process UIA call that a busy app can stall, so it
// runs on a worker thread fed from a queue; the keyboard hook thread only
// enqueues. The worker is an MTA thread that lives as long as the app,
// which also keeps the MTA alive between collections: the elements the
// collector hands out stay valid while scroll mode holds on to them.
class Scroller {
public:
    // Starts the worker; returns once its apartment exists.
    void Start();
    void Stop();

    // Takes over `region` (UIA element, may lack a ScrollPattern) whose
    // visible part is `rect`. If the element disappears, posts `goneMsg`
    // with wParam = `tag` to `replyWindow`.
    void Begin(std::shared_ptr<IUIAutomationElement> region, RECT rect, HWND replyWindow, UINT goneMsg,
               WPARAM tag);
    void Step(ScrollStep step);
    void End();

private:
    void Post(std::function<void()> task, bool droppable = false);
    void Run();
    // Worker-thread side.
    void DoBegin(std::shared_ptr<IUIAutomationElement> region, RECT rect, HWND replyWindow, UINT goneMsg,
                 WPARAM tag);
    void DoStep(ScrollStep step);
    void DoEnd();

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
    bool quit_ = false;

    // Owned by the worker thread.
    std::shared_ptr<IUIAutomationElement> region_;
    IUIAutomationScrollPattern* pattern_ = nullptr;
    RECT rect_{};
    HWND replyWindow_ = nullptr;
    UINT goneMsg_ = 0;
    WPARAM tag_ = 0;
};
