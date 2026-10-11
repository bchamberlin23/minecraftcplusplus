#include "social/nostr.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>

namespace social {

using nlohmann::json;

i64 unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace {
std::string serializeForId(const std::string& pubkey, i64 createdAt, int kind, const json& tags, const std::string& content) {
    // NIP-01: [0, pubkey, created_at, kind, tags, content], no whitespace, UTF-8 (only the JSON escapes required).
    return json::array({0, pubkey, createdAt, kind, tags, content}).dump(-1, ' ', false, json::error_handler_t::replace);
}
}  // namespace

json signEvent(const KeyPair& author, int kind, const json& tags, const std::string& content, i64 createdAt) {
    const std::string pub = toHex(author.pub);
    const Key32 id = sha256(serializeForId(pub, createdAt, kind, tags, content));
    const std::array<u8, 64> sig = schnorrSign(author, id.data(), id.size());
    return json{{"id", toHex(id)}, {"pubkey", pub},   {"created_at", createdAt}, {"kind", kind},
                {"tags", tags},    {"content", content}, {"sig", toHex(sig.data(), sig.size())}};
}

bool verifyEvent(const json& e) {
    try {
        Key32 pub, id;
        Bytes sig;
        if (!fromHex(e.at("pubkey").get<std::string>(), pub) || !fromHex(e.at("id").get<std::string>(), id) ||
            !fromHex(e.at("sig").get<std::string>(), sig) || sig.size() != 64)
            return false;
        const Key32 want = sha256(serializeForId(e.at("pubkey").get<std::string>(), e.at("created_at").get<i64>(),
                                                 e.at("kind").get<int>(), e.at("tags"), e.at("content").get<std::string>()));
        return want == id && schnorrVerify(pub, id.data(), id.size(), sig.data());
    } catch (const std::exception&) {
        return false;
    }
}

struct RelayPool::Relay {
    std::string url;
    std::wstring host, path;
    INTERNET_PORT port = 443;
    bool secure = true;
    HINTERNET ws = nullptr, connecting = nullptr;  // connecting: the session while the WebSocket is being opened
    std::mutex mutex;  // ws, connecting, sending, pending
    std::condition_variable wake;
    struct Outgoing {
        std::string msg;
        double expires;  // ephemeral: dropped when not sent by then; durable: never
    };
    std::deque<Outgoing> queue;  // not sent to this relay yet
    double nextSend = 0;
    int refused = 0;
    double lastRefusedLog = -1e9;
    std::atomic<bool> connected{false};
    u64 subSent = ~0ull;
    std::thread thread;
    double lastNotice = 0;

    bool send(const std::string& text) {  // mutex held
        if (!ws) return false;
        return WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(), DWORD(text.size())) ==
               NO_ERROR;
    }
    void closeLocked() {
        if (ws) {
            WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(ws);
            ws = nullptr;
        }
        connected = false;
    }
};

RelayPool::RelayPool() = default;
RelayPool::~RelayPool() { stop(); }
int RelayPool::total() const { return int(relays_.size()); }

void RelayPool::start(const std::vector<std::string>& urls, EventFn onEvent) {
    stop();
    onEvent_ = std::move(onEvent);
    running_ = true;
    for (const std::string& u : urls) {
        auto r = std::make_unique<Relay>();
        r->url = u;
        std::string rest = u;
        if (rest.starts_with("wss://")) rest.erase(0, 6);
        else if (rest.starts_with("ws://")) rest.erase(0, 5), r->secure = false, r->port = 80;
        const size_t slash = rest.find('/');
        std::string hostPort = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
        if (const size_t colon = hostPort.find(':'); colon != std::string::npos) {
            r->port = INTERNET_PORT(std::atoi(hostPort.c_str() + colon + 1));
            hostPort.resize(colon);
        }
        r->host.assign(hostPort.begin(), hostPort.end());
        r->path.assign(path.begin(), path.end());
        relays_.push_back(std::move(r));
    }
    for (auto& r : relays_) r->thread = std::thread([this, p = r.get()] { run(*p); });
    sender_ = std::thread([this] { sendLoop(); });
}

void RelayPool::sendLoop() {
    constexpr double kInterval = 0.2;  // per relay: at most five events a second
    while (running_) {
        Sleep(25);
        const double now = nowSeconds();
        for (auto& r : relays_) {
            std::lock_guard lock(r->mutex);
            while (!r->queue.empty() && r->queue.front().expires < now) r->queue.pop_front();
            if (!r->connected || r->queue.empty() || now < r->nextSend) continue;
            if (r->send(r->queue.front().msg)) r->queue.pop_front(), r->nextSend = now + kInterval;
        }
    }
}

void RelayPool::stop() {
    if (!running_) return;
    running_ = false;
    for (auto& r : relays_) {
        std::lock_guard lock(r->mutex);
        r->closeLocked();
        if (r->connecting) WinHttpCloseHandle(r->connecting);  // cancels a connection attempt in progress
        r->connecting = nullptr;
        r->wake.notify_all();
    }
    for (auto& r : relays_)
        if (r->thread.joinable()) r->thread.join();
    if (sender_.joinable()) sender_.join();
    relays_.clear();
}

int RelayPool::connected() const {
    int n = 0;
    for (const auto& r : relays_) n += r->connected ? 1 : 0;
    return n;
}

bool RelayPool::deliver(const std::string& id) {
    std::lock_guard lock(mutex_);
    if (!seen_.insert(id).second) return false;
    seenOrder_.push_back(id);
    if (seenOrder_.size() > 4096) {
        seen_.erase(seenOrder_.front());
        seenOrder_.pop_front();
    }
    return true;
}

void RelayPool::publish(const json& event, bool durable, bool urgent) {
    const std::string msg = json::array({"EVENT", event}).dump();
    deliver(event.value("id", std::string()));  // our own events come back from the relays: not news
    const double expires = durable ? 1e300 : nowSeconds() + 15.0;
    for (auto& r : relays_) {
        std::lock_guard lock(r->mutex);
        if (urgent) r->queue.push_front({msg, expires});
        else r->queue.push_back({msg, expires});
        while (r->queue.size() > 256) r->queue.pop_back();
    }
}

void RelayPool::flush(double timeout) {
    for (const double t0 = nowSeconds(); nowSeconds() - t0 < timeout; Sleep(25)) {
        bool empty = true;
        for (auto& r : relays_) {
            std::lock_guard lock(r->mutex);
            empty = empty && (!r->connected || r->queue.empty());
        }
        if (empty) return;
    }
}

void RelayPool::subscribe(const json& filters) {
    std::string msg;
    u64 gen;
    {
        std::lock_guard lock(mutex_);
        filters_ = filters;
        gen = ++subGeneration_;
        json req = json::array({"REQ", "mcpp"});
        for (const json& f : filters) req.push_back(f);
        msg = req.dump();
    }
    for (auto& r : relays_) {
        std::lock_guard lock(r->mutex);
        if (r->connected && r->send(msg)) r->subSent = gen;
    }
}

void RelayPool::run(Relay& r) {
    int backoff = 2;
    const std::string name = r.url;
    while (running_) {
        HINTERNET session = WinHttpOpen(L"minecraftCplusplus", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        {
            std::lock_guard lock(r.mutex);
            if (!running_) {
                if (session) WinHttpCloseHandle(session);
                break;
            }
            r.connecting = session;  // stop() closes it to cancel a slow attempt
        }
        HINTERNET conn = nullptr, req = nullptr, ws = nullptr;
        bool ok = session != nullptr;
        if (ok) WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
        if (ok) ok = (conn = WinHttpConnect(session, r.host.c_str(), r.port, 0)) != nullptr;
        if (ok) ok = (req = WinHttpOpenRequest(conn, L"GET", r.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               r.secure ? WINHTTP_FLAG_SECURE : 0)) != nullptr;
        if (ok) ok = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
        if (ok) ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(req, nullptr);
        DWORD status = 0, size = sizeof(status);
        if (ok) ok = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                         &status, &size, WINHTTP_NO_HEADER_INDEX) &&
                      status == 101;
        if (ok) ok = (ws = WinHttpWebSocketCompleteUpgrade(req, 0)) != nullptr;
        const DWORD err = ok ? 0 : GetLastError();
        if (req) WinHttpCloseHandle(req);
        if (ok) {
            // Keep the connection open between rare events: no receive timeout on the WebSocket itself.
            DWORD zero = 0;
            WinHttpSetOption(ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &zero, sizeof(zero));
            std::lock_guard lock(r.mutex);
            if (!running_) {
                WinHttpCloseHandle(ws);
                ws = nullptr;
                ok = false;
            } else {
                r.ws = ws;
                r.connected = true;
                std::string sub;
                {
                    std::lock_guard l2(mutex_);
                    if (!filters_.is_null()) {
                        json q = json::array({"REQ", "mcpp"});
                        for (const json& f : filters_) q.push_back(f);
                        sub = q.dump();
                        r.subSent = subGeneration_;
                    }
                }
                if (!sub.empty()) r.send(sub);
            }
        }
        if (ok) {
            LOG("friends: relay %s connected", name.c_str());
            backoff = 2;
            std::string message;
            std::vector<char> buf(1 << 16);
            for (;;) {
                DWORD got = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                const DWORD rc = WinHttpWebSocketReceive(ws, buf.data(), DWORD(buf.size()), &got, &type);
                if (rc != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
                message.append(buf.data(), got);
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
                    if (message.size() > (1 << 20)) break;  // nothing we expect is this large
                    continue;
                }
                json m = json::parse(message, nullptr, false);
                message.clear();
                if (!m.is_array() || m.empty() || !m[0].is_string()) continue;
                const std::string verb = m[0].get<std::string>();
                if (verb == "EVENT" && m.size() >= 3 && m[2].is_object()) {
                    const std::string id = m[2].value("id", std::string());
                    if (!id.empty() && deliver(id) && onEvent_) onEvent_(m[2], name);
                } else if (verb == "OK" && m.size() >= 4 && m[2].is_boolean() && !m[2].get<bool>()) {
                    ++r.refused;
                    if (nowSeconds() - r.lastRefusedLog > 60) {
                        r.lastRefusedLog = nowSeconds();
                        LOG("friends: relay %s refused %d event(s): %s", name.c_str(), r.refused, m[3].dump().c_str());
                        r.refused = 0;
                    }
                } else if ((verb == "NOTICE" || verb == "CLOSED") && nowSeconds() - r.lastNotice > 30) {
                    r.lastNotice = nowSeconds();
                    LOG("friends: relay %s: %s", name.c_str(), m.dump().c_str());
                }
            }
            LOG("friends: relay %s disconnected", name.c_str());
        } else if (running_) {
            LOG("friends: relay %s unreachable (status %lu, error %lu); retrying in %d s", name.c_str(), status, err, backoff);
        }
        {
            std::lock_guard lock(r.mutex);
            if (r.ws == ws && ws) r.closeLocked();
            r.connected = false;
        }
        if (conn) WinHttpCloseHandle(conn);
        {
            std::lock_guard lock(r.mutex);
            if (r.connecting == session && session) WinHttpCloseHandle(session);  // else stop() closed it
            r.connecting = nullptr;
        }
        std::unique_lock lock(r.mutex);
        r.wake.wait_for(lock, std::chrono::seconds(backoff), [this] { return !running_.load(); });
        backoff = std::min(backoff * 2, 60);
    }
}

}  // namespace social
