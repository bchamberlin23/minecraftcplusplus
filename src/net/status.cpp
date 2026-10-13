#include "net/status.h"

#include "assets/zip.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windns.h>

#include <algorithm>
#include <array>

#pragma comment(lib, "dnsapi.lib")

namespace net {

namespace {

constexpr char kLanGroup[] = "224.0.2.60";
constexpr u16 kLanPort = 4445;

struct Winsock {
    Winsock() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~Winsock() { WSACleanup(); }
};
void winsock() { static Winsock w; }

void putVarInt(std::vector<u8>& out, u32 v) {
    while (v >= 0x80) out.push_back(u8(v | 0x80)), v >>= 7;
    out.push_back(u8(v));
}
void putString(std::vector<u8>& out, std::string_view s) {
    putVarInt(out, u32(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}
std::vector<u8> frame(const std::vector<u8>& packet) {
    std::vector<u8> out;
    putVarInt(out, u32(packet.size()));
    out.insert(out.end(), packet.begin(), packet.end());
    return out;
}

// Blocking socket with timeouts.
class Socket {
public:
    ~Socket() {
        if (s_ != INVALID_SOCKET) closesocket(s_);
    }
    bool connect(const std::string& host, u16 port, double timeout, std::string& error) {
        addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
            error = "unknown host";
            return false;
        }
        std::vector<addrinfo*> candidates;
        for (addrinfo* a = result; a; a = a->ai_next) candidates.push_back(a);
        std::stable_partition(candidates.begin(), candidates.end(), [](addrinfo* a) { return a->ai_family == AF_INET; });
        error = "can't connect to server";
        for (addrinfo* a : candidates) {
            SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            ::connect(s, a->ai_addr, int(a->ai_addrlen));
            fd_set w, e;
            FD_ZERO(&w);
            FD_ZERO(&e);
            FD_SET(s, &w);
            FD_SET(s, &e);
            timeval tv{long(timeout), long((timeout - double(long(timeout))) * 1e6)};
            const int r = select(0, nullptr, &w, &e, &tv);
            if (r > 0 && FD_ISSET(s, &w)) {
                nb = 0;
                ioctlsocket(s, FIONBIO, &nb);
                const DWORD ms = DWORD(timeout * 1000);
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
                setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
                BOOL nd = TRUE;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
                s_ = s;
                break;
            }
            if (r == 0) error = "timed out";
            closesocket(s);
        }
        freeaddrinfo(result);
        return s_ != INVALID_SOCKET;
    }
    bool send(const std::vector<u8>& bytes) {
        size_t done = 0;
        while (done < bytes.size()) {
            const int n = ::send(s_, reinterpret_cast<const char*>(bytes.data() + done), int(bytes.size() - done), 0);
            if (n <= 0) return false;
            done += size_t(n);
        }
        return true;
    }
    bool readByte(u8& b) {
        if (pos_ == end_) {
            const int n = ::recv(s_, reinterpret_cast<char*>(buf_), sizeof(buf_), 0);
            if (n <= 0) return false;
            pos_ = 0, end_ = size_t(n);
        }
        b = buf_[pos_++];
        return true;
    }
    bool readVarInt(u32& v) {
        v = 0;
        for (int i = 0; i < 5; ++i) {
            u8 b;
            if (!readByte(b)) return false;
            v |= u32(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool readPacket(std::vector<u8>& out) {
        u32 len;
        if (!readVarInt(len) || len > (u32(4) << 20)) return false;
        out.resize(len);
        for (u32 i = 0; i < len; ++i)
            if (!readByte(out[i])) return false;
        return true;
    }

private:
    SOCKET s_ = INVALID_SOCKET;
    u8 buf_[8192];
    size_t pos_ = 0, end_ = 0;
};

bool getVarInt(const std::vector<u8>& p, size_t& i, u32& v) {
    v = 0;
    for (int k = 0; k < 5; ++k) {
        if (i >= p.size()) return false;
        const u8 b = p[i++];
        v |= u32(b & 0x7F) << (7 * k);
        if (!(b & 0x80)) return true;
    }
    return false;
}

std::vector<u8> base64(std::string_view s) {
    static const auto table = [] {
        std::array<int, 256> t;
        t.fill(-1);
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[u8(chars[i])] = i;
        return t;
    }();
    std::vector<u8> out;
    u32 acc = 0;
    int bits = 0;
    for (char c : s) {
        const int v = table[u8(c)];
        if (v < 0) continue;  // padding, line breaks
        acc = (acc << 6) | u32(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(u8(acc >> bits));
        }
    }
    return out;
}

}  // namespace

ServerAddress parseAddress(std::string_view text) {
    ServerAddress a;
    std::string s(text);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.starts_with("[")) {  // [ipv6]:port
        const size_t close = s.find(']');
        if (close != std::string::npos) {
            a.host = s.substr(1, close - 1);
            if (close + 1 < s.size() && s[close + 1] == ':')
                a.port = u16(std::clamp(std::atoi(s.c_str() + close + 2), 1, 65535)), a.portGiven = true;
            return a;
        }
    }
    const size_t c = s.rfind(':');
    if (c != std::string::npos && s.find(':') == c) {
        a.host = s.substr(0, c);
        const int p = std::atoi(s.c_str() + c + 1);
        if (p > 0 && p < 65536) a.port = u16(p), a.portGiven = true;
    } else {
        a.host = s;
    }
    return a;
}

ServerAddress resolveAddress(const ServerAddress& a) {
    if (a.portGiven || a.host.empty()) return a;
    in_addr v4;
    if (inet_pton(AF_INET, a.host.c_str(), &v4) == 1) return a;  // an IP: no SRV record
    const std::wstring name = L"_minecraft._tcp." + std::wstring(a.host.begin(), a.host.end());
    PDNS_RECORDW records = nullptr;
    ServerAddress out = a;
    if (DnsQuery_W(name.c_str(), DNS_TYPE_SRV, DNS_QUERY_STANDARD, nullptr, &records, nullptr) == 0 && records) {
        for (PDNS_RECORDW r = records; r; r = r->pNext)
            if (r->wType == DNS_TYPE_SRV) {
                const std::wstring target = r->Data.SRV.pNameTarget;
                out.host = fs::path(target).string();
                if (!out.host.empty() && out.host.back() == '.') out.host.pop_back();
                out.port = r->Data.SRV.wPort;
                out.portGiven = true;
                break;
            }
        DnsRecordListFree(records, DnsFreeRecordList);
    }
    return out;
}

ServerStatus pingServer(const std::string& address, int protocol, double timeout) {
    winsock();
    ServerStatus st;
    const ServerAddress a = resolveAddress(parseAddress(address));
    if (a.host.empty()) {
        st.error = "unknown host";
        return st;
    }
    Socket s;
    if (!s.connect(a.host, a.port, timeout, st.error)) return st;
    std::vector<u8> hs;
    putVarInt(hs, 0);  // intention
    putVarInt(hs, u32(protocol));
    putString(hs, a.host);
    hs.push_back(u8(a.port >> 8)), hs.push_back(u8(a.port));
    putVarInt(hs, 1);  // status
    std::vector<u8> request{0};
    if (!s.send(frame(hs)) || !s.send(frame(request))) {
        st.error = "connection lost";
        return st;
    }
    std::vector<u8> p;
    size_t i = 0;
    u32 id = 0, len = 0;
    if (!s.readPacket(p) || !getVarInt(p, i, id) || id != 0 || !getVarInt(p, i, len) || i + len > p.size()) {
        st.error = "no response";
        return st;
    }
    const nlohmann::json j = nlohmann::json::parse(p.begin() + std::ptrdiff_t(i), p.begin() + std::ptrdiff_t(i + len), nullptr, false);
    if (!j.is_object()) {
        st.error = "invalid response";
        return st;
    }
    st.ok = true;
    if (j.contains("description")) st.description = j["description"];
    if (j.contains("version") && j["version"].is_object()) {
        st.versionName = j["version"].value("name", "");
        st.protocol = j["version"].value("protocol", -1);
    }
    if (j.contains("players") && j["players"].is_object()) {
        const auto& pl = j["players"];
        st.online = pl.value("online", -1), st.max = pl.value("max", -1);
        if (pl.contains("sample") && pl["sample"].is_array())
            for (const auto& e : pl["sample"])
                if (e.is_object()) st.sample.push_back(e.value("name", ""));
    }
    if (j.contains("favicon") && j["favicon"].is_string()) {
        const std::string f = j["favicon"].get<std::string>();
        const std::string prefix = "data:image/png;base64,";
        if (f.starts_with(prefix)) {
            st.faviconBase64 = f.substr(prefix.size());
            st.favicon = base64(st.faviconBase64);
        }
    }
    // Ping: the time from the request to the answer.
    std::vector<u8> ping{1};
    const u64 stamp = u64(GetTickCount64());
    for (int k = 7; k >= 0; --k) ping.push_back(u8(stamp >> (k * 8)));
    const double t0 = nowSeconds();
    if (s.send(frame(ping)) && s.readPacket(p)) st.pingMs = int((nowSeconds() - t0) * 1000.0 + 0.5);
    return st;
}

int gameProtocol(const fs::path& jar, std::string* versionName) {
    ZipArchive zip;
    if (!zip.open(jar)) return 0;
    for (const auto& e : zip.entries())
        if (e.name == "version.json") {
            std::vector<u8> bytes;
            if (!zip.read(e, bytes)) return 0;
            const auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            if (!j.is_object()) return 0;
            if (versionName) *versionName = j.value("name", "");
            return j.value("protocol_version", 0);
        }
    return 0;
}

// --- LAN worlds --------------------------------------------------------------------------------------------------

LanListener::~LanListener() { stop(); }

bool LanListener::start() {
    if (running_) return true;
    winsock();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kLanPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        LOG("lan: cannot listen on port %u (%d)", kLanPort, WSAGetLastError());
        closesocket(s);
        return false;
    }
    ip_mreq m{};
    inet_pton(AF_INET, kLanGroup, &m.imr_multiaddr);
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m));
    const DWORD ms = 250;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    socket_ = s;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void LanListener::stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    closesocket(SOCKET(socket_));
    socket_ = ~uintptr_t(0);
}

void LanListener::run() {
    char buf[1024];
    while (running_) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(SOCKET(socket_), buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) continue;
        const std::string msg(buf, size_t(n));
        auto between = [&](const char* open, const char* close) -> std::string {
            const size_t a = msg.find(open);
            if (a == std::string::npos) return {};
            const size_t b = msg.find(close, a + std::strlen(open));
            return b == std::string::npos ? std::string() : msg.substr(a + std::strlen(open), b - a - std::strlen(open));
        };
        std::string motd = between("[MOTD]", "[/MOTD]"), ad = between("[AD]", "[/AD]");
        if (motd.empty()) motd = "missing no";  // LanServerPinger.parseMotd
        if (ad.empty()) continue;
        char ip[64] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        // The port only (current versions) or host:port.
        const std::string address = ad.find(':') == std::string::npos ? std::string(ip) + ":" + ad : ad;
        std::lock_guard lock(mutex_);
        bool found = false;
        for (Entry& e : entries_)
            if (e.address == address) e.motd = motd, e.lastSeen = nowSeconds(), found = true;
        if (!found) {
            entries_.push_back({motd, address, nowSeconds()});
            LOG("lan: found \"%s\" at %s", motd.c_str(), address.c_str());
        }
    }
}

std::vector<LanListener::Entry> LanListener::entries() const {
    std::lock_guard lock(mutex_);
    return entries_;
}

// --- Open to LAN -------------------------------------------------------------------------------------------------

u16 suggestLanPort() {
    winsock();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    int len = sizeof(a);
    u16 port = 25564;
    if (s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    if (s != INVALID_SOCKET) closesocket(s);
    return port;
}

bool lanPortAvailable(u16 port) {  // HttpUtil.isPortAvailable: a server socket can open on it
    winsock();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    const bool ok = bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

LanHost::~LanHost() { stop(); }

bool LanHost::start(const std::string& motd, u16 publicPort, u16 serverPort, bool forward, std::string& error) {
    stop();
    winsock();
    motd_ = motd, publicPort_ = publicPort, serverPort_ = serverPort;
    if (forward) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(publicPort);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 16) != 0) {
            error = "port " + std::to_string(publicPort) + " is not available";
            if (s != INVALID_SOCKET) closesocket(s);
            return false;
        }
        listen_ = s;
    }
    active_ = true;
    announcer_ = std::thread([this] { announce(); });
    if (forward) acceptor_ = std::thread([this] { accept(); });
    return true;
}

void LanHost::stop() {
    if (!active_) return;
    active_ = false;
    if (listen_ != ~uintptr_t(0)) closesocket(SOCKET(listen_));
    listen_ = ~uintptr_t(0);
    if (announcer_.joinable()) announcer_.join();
    if (acceptor_.joinable()) acceptor_.join();
    {
        std::lock_guard lock(mutex_);
        for (uintptr_t s : sockets_) shutdown(SOCKET(s), SD_BOTH);
    }
    for (std::thread& t : relays_)
        if (t.joinable()) t.join();
    relays_.clear();
}

void LanHost::announce() {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kLanPort);
    inet_pton(AF_INET, kLanGroup, &to.sin_addr);
    const std::string msg = "[MOTD]" + motd_ + "[/MOTD][AD]" + std::to_string(publicPort_) + "[/AD]";
    while (active_) {
        sendto(s, msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        for (int i = 0; i < 15 && active_; ++i) Sleep(100);  // every 1.5 s, like the game
    }
    closesocket(s);
}

void LanHost::accept() {
    while (active_) {
        const SOCKET c = ::accept(SOCKET(listen_), nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        std::lock_guard lock(mutex_);
        sockets_.push_back(c);
        relays_.emplace_back([this, c] { relay(c); });
    }
}

void LanHost::relay(uintptr_t client) {
    const SOCKET c = SOCKET(client);
    SOCKET server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(serverPort_);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (server != INVALID_SOCKET && ::connect(server, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        {
            std::lock_guard lock(mutex_);
            sockets_.push_back(server);
        }
        BOOL nd = TRUE;
        setsockopt(server, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
        std::vector<char> buf(1 << 16);
        while (active_) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(c, &r);
            FD_SET(server, &r);
            timeval tv{0, 200000};
            const int n = select(0, &r, nullptr, nullptr, &tv);
            if (n < 0) break;
            bool closed = false;
            for (auto [from, to] : {std::pair{c, server}, std::pair{server, c}}) {
                if (!FD_ISSET(from, &r)) continue;
                const int got = recv(from, buf.data(), int(buf.size()), 0);
                if (got <= 0) {
                    closed = true;
                    break;
                }
                for (int done = 0; done < got;) {
                    const int sent = send(to, buf.data() + done, got - done, 0);
                    if (sent <= 0) {
                        closed = true;
                        break;
                    }
                    done += sent;
                }
            }
            if (closed) break;
        }
    }
    std::lock_guard lock(mutex_);
    for (SOCKET s : {c, server}) {
        if (s == INVALID_SOCKET) continue;
        closesocket(s);
        sockets_.erase(std::remove(sockets_.begin(), sockets_.end(), uintptr_t(s)), sockets_.end());
    }
}

}  // namespace net
