// The library headers come first: they include winsock2.h, which must
// precede windows.h (pulled in by ipc_server.h).
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "ipc_server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

#include "ipc_token.h"

namespace {

using nlohmann::json;

// Same cap as the UIA collector: 26 * 26 two-letter labels.
constexpr size_t kMaxItems = 676;

// Application-defined close codes (4000-4999 per RFC 6455).
constexpr uint16_t kCloseUnauthorized = 4001;

std::string AsciiLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(c < 0x80 ? std::tolower(c) : c); });
    return s;
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// One "hints" item; false if it is malformed and must be skipped.
bool ParseItem(const json& j, AdapterHint* out) {
    if (!j.is_object()) return false;
    const auto id = j.find("id");
    const auto rect = j.find("rect");
    if (id == j.end() || !id->is_string() || rect == j.end() || !rect->is_array() || rect->size() != 4) {
        return false;
    }
    long v[4];
    for (int i = 0; i < 4; ++i) {
        if (!(*rect)[i].is_number()) return false;
        v[i] = static_cast<long>((*rect)[i].get<double>());
    }
    if (v[2] <= 0 || v[3] <= 0) return false;
    out->id = id->get<std::string>();
    out->rect = {v[0], v[1], v[0] + v[2], v[1] + v[3]};
    const auto name = j.find("name");
    out->name = (name != j.end() && name->is_string()) ? Widen(name->get<std::string>()) : std::wstring();
    const auto click = j.find("click");
    out->click = click != j.end() && click->is_boolean() && click->get<bool>();
    return true;
}

}  // namespace

IpcServer::IpcServer() = default;

IpcServer::~IpcServer() { Stop(); }

bool IpcServer::Start(std::string token, HWND replyWindow, UINT hintsMsg) {
    if (token.empty()) return false;
    token_ = std::move(token);
    replyWindow_ = replyWindow;
    hintsMsg_ = hintsMsg;

    ix::initNetSystem();
    // Loopback only: adapters run on this machine, nothing else may reach
    // the port.
    server_ = std::make_unique<ix::WebSocketServer>(kIpcPort, "127.0.0.1");
    server_->disablePerMessageDeflate();
    server_->setOnClientMessageCallback(
        [this](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
            switch (msg->type) {
                case ix::WebSocketMessageType::Open: {
                    std::lock_guard lock(mutex_);
                    clients_[&ws] = {};
                    break;
                }
                case ix::WebSocketMessageType::Message:
                    if (msg->binary) break;
                    // A field of the wrong type makes json throw; on a
                    // library thread that would end the process. A bad
                    // message is just dropped.
                    try {
                        OnText(ws, msg->str);
                    } catch (const nlohmann::json::exception&) {
                    }
                    break;
                case ix::WebSocketMessageType::Close:
                    OnClose(ws);
                    break;
                default:
                    break;
            }
        });

    // Fails if the port is taken (another rout build, another app);
    // rout then runs on UIA alone.
    if (!server_->listen().first) {
        server_.reset();
        return false;
    }
    server_->start();
    return true;
}

void IpcServer::Stop() {
    if (!server_) return;
    server_->stop();  // Joins connection threads; close callbacks run.
    server_.reset();
    std::lock_guard lock(mutex_);
    clients_.clear();
    byApp_.clear();
    pendingReq_.clear();
}

void IpcServer::OnText(ix::WebSocket& ws, const std::string& text) {
    const json msg = json::parse(text, nullptr, false);
    const std::string type = (msg.is_object() && msg.contains("type") && msg["type"].is_string())
                                 ? msg["type"].get<std::string>()
                                 : std::string();

    bool reject = false;
    {
        std::lock_guard lock(mutex_);
        auto client = clients_.find(&ws);
        if (client == clients_.end()) return;

        if (client->second.app.empty()) {
            // The first message must be a valid hello; anything else is
            // an unauthenticated client.
            const std::string token = msg.is_object() ? msg.value("token", std::string()) : std::string();
            const std::string app = msg.is_object() ? AsciiLower(msg.value("app", std::string())) : std::string();
            if (type != "hello" || app.empty() || !TokenEquals(token, token_)) {
                reject = true;
            } else {
                // Read everything that can throw before registering, so
                // a malformed hello leaves the client unauthenticated.
                const unsigned long pid = msg.value("pid", 0ul);
                client->second.app = app;
                client->second.pid = pid;
                byApp_[app] = &ws;  // The newest connection for an app wins.
            }
        } else if (type == "hints") {
            const std::string& app = client->second.app;
            const auto pending = pendingReq_.find(app);
            const WPARAM req = msg.value("req", static_cast<WPARAM>(0));
            if (pending == pendingReq_.end() || pending->second != req) return;  // Stale or unasked.
            pendingReq_.erase(pending);

            auto hints = std::make_unique<AdapterHints>();
            if (const auto items = msg.find("items"); items != msg.end() && items->is_array()) {
                for (const json& item : *items) {
                    AdapterHint h;
                    if (ParseItem(item, &h)) hints->items.push_back(std::move(h));
                    if (hints->items.size() >= kMaxItems) break;
                }
            }
            if (PostMessageW(replyWindow_, hintsMsg_, req, reinterpret_cast<LPARAM>(hints.get()))) {
                hints.release();  // The receiver owns it now.
            }
        }
        // "ack" and unknown types: nothing to do.
    }
    // Outside the lock: close() can run the close callback, which locks.
    if (reject) ws.close(kCloseUnauthorized, "unauthorized");
}

void IpcServer::OnClose(ix::WebSocket& ws) {
    std::lock_guard lock(mutex_);
    auto client = clients_.find(&ws);
    if (client == clients_.end()) return;
    const std::string app = client->second.app;
    clients_.erase(client);
    if (auto it = byApp_.find(app); it != byApp_.end() && it->second == &ws) {
        byApp_.erase(it);
        pendingReq_.erase(app);
    }
}

bool IpcServer::HasAdapter(const std::string& app) {
    std::lock_guard lock(mutex_);
    return byApp_.count(AsciiLower(app)) != 0;
}

bool IpcServer::SendTo(const std::string& app, const std::string& text) {
    std::lock_guard lock(mutex_);
    const auto it = byApp_.find(app);
    if (it == byApp_.end()) return false;
    // Loopback send: fast, so holding the lock (which keeps the
    // connection alive) is fine.
    return it->second->sendText(text).success;
}

bool IpcServer::RequestHints(const std::string& app, WPARAM tag) {
    const std::string key = AsciiLower(app);
    {
        std::lock_guard lock(mutex_);
        if (!byApp_.count(key)) return false;
        pendingReq_[key] = tag;
    }
    return SendTo(key, json{{"cmd", "get_hints"}, {"req", tag}}.dump());
}

bool IpcServer::Activate(const std::string& app, const std::string& id) {
    return SendTo(AsciiLower(app), json{{"cmd", "activate"}, {"id", id}}.dump());
}
