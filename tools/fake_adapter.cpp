// Minimal adapter for testing the M2 IPC path without a real one.
//
//   fake_adapter <exe-name> [--bad-token] [--silent] [--empty]
//
// Connects to rout, says hello as <exe-name> (e.g. notepad.exe), and
// answers get_hints with four hints at the corners of that app's
// foreground-most window. Prints every message it gets, so pressing a
// hint shows the "activate" Core sends. --bad-token checks rejection,
// --silent never answers (Core must fall back to UIA after 200 ms),
// --empty answers with no items (Core must fall back immediately).

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using nlohmann::json;

namespace {

std::string ReadToken() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\rout\\ipc-token";
    }
    CoTaskMemFree(base);
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

struct Find {
    std::wstring exe;
    HWND found = nullptr;
};

// Top-level windows enumerate in z-order, so the first visible match is
// the one in front.
BOOL CALLBACK FindWindowOf(HWND hwnd, LPARAM param) {
    auto* f = reinterpret_cast<Find*>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(p, 0, path, &size);
    CloseHandle(p);
    if (!ok) return TRUE;
    std::wstring name(path, size);
    name.erase(0, name.find_last_of(L'\\') + 1);
    if (_wcsicmp(name.c_str(), f->exe.c_str()) != 0) return TRUE;
    f->found = hwnd;
    return FALSE;
}

json CornerHints(const std::string& exe) {
    Find f{std::wstring(exe.begin(), exe.end())};
    EnumWindows(FindWindowOf, reinterpret_cast<LPARAM>(&f));
    json items = json::array();
    RECT r{};
    if (!f.found || !GetWindowRect(f.found, &r)) return items;
    const int w = 60, h = 30, m = 40;
    const struct { const char* id; long x, y; } corners[] = {
        {"top-left", r.left + m, r.top + m},
        {"top-right", r.right - m - w, r.top + m},
        {"bottom-left", r.left + m, r.bottom - m - h},
        {"bottom-right", r.right - m - w, r.bottom - m - h},
    };
    for (const auto& c : corners) {
        items.push_back({{"id", c.id}, {"rect", {c.x, c.y, w, h}}, {"name", std::string("Corner ") + c.id}});
    }
    return items;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fake_adapter <exe-name> [--bad-token] [--silent] [--empty]\n");
        return 2;
    }
    const std::string exe = argv[1];
    bool badToken = false, silent = false, empty = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        badToken |= a == "--bad-token";
        silent |= a == "--silent";
        empty |= a == "--empty";
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // Physical px, like Core.
    ix::initNetSystem();
    std::atomic<bool> closed{false};

    ix::WebSocket ws;
    ws.setUrl("ws://127.0.0.1:47613");
    ws.disableAutomaticReconnection();
    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Open: {
                const std::string token = badToken ? std::string(64, '0') : ReadToken();
                ws.sendText(json{{"type", "hello"}, {"app", exe}, {"pid", GetCurrentProcessId()}, {"token", token}}
                                .dump());
                std::printf("connected, hello sent as %s\n", exe.c_str());
                break;
            }
            case ix::WebSocketMessageType::Message: {
                std::printf("<- %s\n", msg->str.c_str());
                const json m = json::parse(msg->str, nullptr, false);
                if (!m.is_object()) break;
                const std::string cmd = m.value("cmd", std::string());
                if (cmd == "get_hints" && !silent) {
                    const json items = empty ? json::array() : CornerHints(exe);
                    ws.sendText(json{{"type", "hints"}, {"req", m.value("req", 0)}, {"items", items}}.dump());
                    std::printf("-> %zu hints\n", items.size());
                } else if (cmd == "activate") {
                    ws.sendText(json{{"type", "ack"}}.dump());
                }
                break;
            }
            case ix::WebSocketMessageType::Close:
                std::printf("closed: %d %s\n", msg->closeInfo.code, msg->closeInfo.reason.c_str());
                closed = true;
                break;
            case ix::WebSocketMessageType::Error:
                std::printf("error: %s\n", msg->errorInfo.reason.c_str());
                closed = true;
                break;
            default:
                break;
        }
        std::fflush(stdout);
    });
    ws.start();
    while (!closed) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ws.stop();
    return 0;
}
