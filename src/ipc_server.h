#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ix {
class WebSocket;
class WebSocketServer;
}  // namespace ix

// Fixed loopback port adapters connect to (ws://127.0.0.1:47613).
constexpr int kIpcPort = 47613;

struct AdapterHint {
    std::string id;     // Opaque to Core; sent back in "activate".
    RECT rect;          // Screen coordinates, physical px.
    std::wstring name;  // Optional, for M1b search.
    // Core clicks the hint itself instead of sending "activate": a real
    // click is the most faithful way to press an ordinary widget.
    bool click = false;
};

// Posted to the reply window when an adapter answers "get_hints".
struct AdapterHints {
    std::vector<AdapterHint> items;
};

// M2: the WebSocket server adapters talk to (Protocol v1, see PLAN.md).
//
// Each connection must open with a "hello" carrying the session token
// (ipc_token.h); anything else closes it. Authenticated adapters are
// registered by their "app" (lowercased exe name, e.g. "rider64.exe");
// a newer connection for the same app replaces the older one.
//
// The library runs one thread per connection; this class only locks
// its tables and posts results to the UI thread, the same way the UIA
// collector does. All public methods are thread-safe.
class IpcServer {
public:
    IpcServer();
    ~IpcServer();

    // Listens on 127.0.0.1:kIpcPort. Hints replies are posted as
    // `hintsMsg` with wParam = the request's tag and lParam = a
    // heap-allocated AdapterHints* the receiver must delete.
    bool Start(std::string token, HWND replyWindow, UINT hintsMsg);
    void Stop();

    bool HasAdapter(const std::string& app);
    // Sends {"cmd":"get_hints","req":tag}; replies with another req
    // (from an abandoned request) are dropped here.
    bool RequestHints(const std::string& app, WPARAM tag);
    bool Activate(const std::string& app, const std::string& id);

private:
    struct Client {
        std::string app;  // Empty until "hello" is accepted.
        unsigned long pid = 0;
    };

    void OnText(ix::WebSocket& ws, const std::string& text);
    void OnClose(ix::WebSocket& ws);
    bool SendTo(const std::string& app, const std::string& text);

    std::unique_ptr<ix::WebSocketServer> server_;
    std::string token_;
    HWND replyWindow_ = nullptr;
    UINT hintsMsg_ = 0;

    std::mutex mutex_;
    // Connection objects live until their close callback has run, and
    // are removed from these tables in it, so a pointer found here
    // under the lock is valid.
    std::map<ix::WebSocket*, Client> clients_;
    std::map<std::string, ix::WebSocket*> byApp_;
    std::map<std::string, WPARAM> pendingReq_;  // app -> outstanding get_hints tag
};
