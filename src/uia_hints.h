#pragma once

#include <windows.h>

#include <vector>

// Generic UI Automation hint source (M1): used for any app without a
// live adapter. Walks the target window's control view and returns the
// on-screen rects of elements that look clickable.
//
// Runs on its own MTA thread, because a walk over a Chromium/Electron
// tree can take hundreds of ms and must never block the thread that
// services the low-level keyboard hook. When done, posts `replyMsg` to
// `replyWindow` with wParam = `tag` and lParam = a heap-allocated
// std::vector<RECT>* the receiver must delete.
void StartUiaCollect(HWND target, HWND replyWindow, UINT replyMsg, WPARAM tag);
