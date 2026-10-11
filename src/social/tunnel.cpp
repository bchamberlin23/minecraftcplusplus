#include "social/tunnel.h"

#include <ikcp.h>
#include <juice/juice.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>

namespace social {

namespace {

constexpr u8 kMagic = 0xA7;          // first byte of every tunnel datagram
constexpr int kKcpMtu = 1150;        // + 1 type + 9 header + 16 tag stays under the usual 1200-byte safe size
constexpr double kIdleTimeout = 30;  // seconds without a datagram from the peer: the path is gone
constexpr size_t kMaxToTcp = 1 << 20;

u32 load32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }
u32 nowMs() { return u32(u64(nowSeconds() * 1000.0)); }

void winsockInit() {
    static const bool done = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)done;
}

bool readVarInt(const u8* d, size_t size, size_t& off, int& out) {
    u32 v = 0;
    for (int i = 0; i < 5; ++i) {
        if (off >= size) return false;
        const u8 b = d[off++];
        v |= u32(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) {
            out = int(v);
            return true;
        }
    }
    out = -1;  // too long: treated as malformed by the caller
    return true;
}

}  // namespace

int parseLoginStart(const u8* d, size_t size, int& intent, std::string& name) {
    size_t off = 0;
    int len = 0;
    if (size > 0 && d[0] == 0xFE) return -1;  // legacy ping
    if (!readVarInt(d, size, off, len)) return 0;
    if (len <= 0 || len > 1100) return -1;
    if (off + size_t(len) > size) return 0;
    const size_t end = off + size_t(len);
    int id = 0, protocol = 0, hostLen = 0;
    if (!readVarInt(d, end, off, id) || id != 0) return -1;
    if (!readVarInt(d, end, off, protocol)) return -1;
    if (!readVarInt(d, end, off, hostLen) || hostLen < 0 || off + size_t(hostLen) + 2 > end) return -1;
    off += size_t(hostLen) + 2;  // address, port
    if (!readVarInt(d, end, off, intent) || off != end) return -1;
    if (intent == 1) return 1;  // status: no login follows
    if (intent != 2 && intent != 3) return -1;
    if (!readVarInt(d, size, off, len)) return 0;
    if (len <= 0 || len > 64) return -1;
    if (off + size_t(len) > size) return 0;
    const size_t end2 = off + size_t(len);
    int nameLen = 0;
    if (!readVarInt(d, end2, off, id) || id != 0) return -1;
    if (!readVarInt(d, end2, off, nameLen) || nameLen <= 0 || nameLen > 64 || off + size_t(nameLen) > end2) return -1;
    name.assign(reinterpret_cast<const char*>(d + off), size_t(nameLen));
    return 1;
}

// --- Cipher -------------------------------------------------------------------------------------------------------

Bytes Tunnel::Cipher::seal(u8 type, const u8* data, size_t size) {
    Bytes out(9 + 1 + size + 16);
    const u64 n = sendCounter++;
    out[0] = kMagic;
    for (int i = 0; i < 8; ++i) out[size_t(1 + i)] = u8(n >> (8 * i));
    out[9] = type;
    if (size) memcpy(out.data() + 10, data, size);
    u8 nonce[12] = {};
    memcpy(nonce + 4, out.data() + 1, 8);
    aeadSeal(sendKey.data(), nonce, out.data(), 9, out.data() + 9, 1 + size, out.data() + 9);
    return out;
}

bool Tunnel::Cipher::open(const u8* data, size_t size, u8& type, Bytes& out) {
    if (size < 9 + 1 + 16 || data[0] != kMagic) return false;
    u64 n = 0;
    for (int i = 0; i < 8; ++i) n |= u64(data[1 + i]) << (8 * i);
    // Sliding window of the last 64 counters (like WireGuard / IPsec): replays and very old datagrams are dropped.
    if (recvAny && (n + 64 <= recvMax || (n <= recvMax && (recvWindow >> (recvMax - n)) & 1))) return false;
    u8 nonce[12] = {};
    memcpy(nonce + 4, data + 1, 8);
    Bytes plain(size - 9 - 16);
    if (!aeadOpen(recvKey.data(), nonce, data, 9, data + 9, size - 9, plain.data())) return false;
    if (!recvAny) {
        recvAny = true, recvMax = n, recvWindow = 1;
    } else if (n > recvMax) {
        const u64 shift = n - recvMax;
        recvWindow = shift >= 64 ? 1 : (recvWindow << shift) | 1;
        recvMax = n;
    } else {
        recvWindow |= 1ull << (recvMax - n);
    }
    type = plain[0];
    out.assign(plain.begin() + 1, plain.end());
    return true;
}

// --- streams ------------------------------------------------------------------------------------------------------

struct Tunnel::Stream {
    Tunnel* tunnel = nullptr;
    u32 conv = 0;
    ikcpcb* kcp = nullptr;
    SOCKET sock = INVALID_SOCKET;
    Bytes toTcp;  // from the peer, not written to the socket yet
    size_t toTcpOff = 0;
    bool tcpEof = false;   // the local side is done: FIN once the peer has everything
    bool peerFin = false;  // the peer is done: close once everything is delivered
    bool authed = true;    // host: the login name was checked
    bool kill = false;     // refused or the socket failed: close now (FIN to the peer)
    Bytes authBuf;
    ~Stream() {
        if (kcp) ikcp_release(kcp);
        if (sock != INVALID_SOCKET) closesocket(sock);
    }
};

Tunnel::Stream* Tunnel::newStream(u32 conv, uintptr_t socket) {
    auto st = std::make_unique<Stream>();
    st->tunnel = this;
    st->conv = conv;
    st->sock = SOCKET(socket);
    st->kcp = ikcp_create(conv, st.get());
    st->kcp->stream = 1;
    ikcp_setmtu(st->kcp, kKcpMtu);
    ikcp_nodelay(st->kcp, 1, 10, 2, 1);
    ikcp_wndsize(st->kcp, 1024, 1024);
    ikcp_setoutput(st->kcp, [](const char* buf, int len, ikcpcb*, void* user) {
        Stream* s = static_cast<Stream*>(user);
        s->tunnel->sendDatagram('D', reinterpret_cast<const u8*>(buf), size_t(len));
        return 0;
    });
    st->authed = config_.role == Role::Joiner;
    Stream* raw = st.get();
    streamsByConv_[conv] = std::move(st);
    streamCount_ = int(streamsByConv_.size());
    return raw;
}

// --- Tunnel -------------------------------------------------------------------------------------------------------

Tunnel::Tunnel(const Config& config) : config_(config) {}

Tunnel::~Tunnel() { close(); }

bool Tunnel::start() {
    winsockInit();
    if (config_.role == Role::Joiner) {
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof(a);
        if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 4) != 0 ||
            getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
            if (s != INVALID_SOCKET) closesocket(s);
            fail("could not open a local port for the game");
            return false;
        }
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        listen_ = uintptr_t(s);
        listenPort_ = ntohs(a.sin_port);
    }
    juice_config_t cfg{};
    cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_MUX;
    cfg.stun_server_host = config_.stunHost.empty() ? nullptr : config_.stunHost.c_str();
    cfg.stun_server_port = config_.stunPort;
    cfg.local_port_range_begin = cfg.local_port_range_end = config_.muxPort;
    cfg.user_ptr = this;
    // Captureless lambdas in a member function: they may use the private members through the user pointer.
    cfg.cb_state_changed = [](juice_agent_t*, juice_state_t st, void* user) {
        Tunnel* t = static_cast<Tunnel*>(user);
        LOG("friends: tunnel %s: ICE %s", t->config_.label.c_str(), juice_state_to_string(st));
        if (st == JUICE_STATE_CONNECTED || st == JUICE_STATE_COMPLETED) {
            if (t->state_ != State::Failed && t->state_ != State::Closed) t->state_ = State::Connected;
            t->lastRecv_ = nowSeconds();
        } else if (st == JUICE_STATE_FAILED) {
            t->fail(t->state_ == State::Connected ? "the connection was lost" : "no direct path between the two networks");
        } else if (st == JUICE_STATE_CONNECTING && t->state_ == State::Gathering) {
            t->state_ = State::Connecting;
        }
        t->wake_.notify_all();
    };
    cfg.cb_candidate = [](juice_agent_t*, const char* sdp, void* user) {
        Tunnel* t = static_cast<Tunnel*>(user);
        std::lock_guard lock(t->mutex_);
        t->candidates_.push_back(sdp);
    };
    cfg.cb_gathering_done = [](juice_agent_t*, void* user) { static_cast<Tunnel*>(user)->gathered_ = true; };
    cfg.cb_recv = [](juice_agent_t*, const char* data, size_t size, void* user) {
        Tunnel* t = static_cast<Tunnel*>(user);
        {
            std::lock_guard lock(t->mutex_);
            if (t->inbound_.size() > 8192) return;  // the pump is far behind: let KCP resend
            t->inbound_.emplace_back(reinterpret_cast<const u8*>(data), reinterpret_cast<const u8*>(data) + size);
        }
        t->wake_.notify_one();
    };
    agent_ = juice_create(&cfg);
    if (!agent_) {
        fail("could not open the UDP port for peer-to-peer connections");
        return false;
    }
    running_ = true;
    pump_ = std::thread([this] { pump(); });
    if (juice_gather_candidates(agent_) != JUICE_ERR_SUCCESS) {
        fail("could not gather network candidates");
        return false;
    }
    return true;
}

std::string Tunnel::localDescription() {
    char buf[JUICE_MAX_SDP_STRING_LEN];
    if (!agent_ || juice_get_local_description(agent_, buf, sizeof(buf)) != JUICE_ERR_SUCCESS) return {};
    std::lock_guard lock(mutex_);
    described_ = buf;
    return described_;
}

std::vector<std::string> Tunnel::takeNewCandidates() {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (std::string& c : candidates_) {
        std::string line = c.starts_with("a=") ? c : "a=" + c;
        if (described_.find(line.substr(2)) == std::string::npos) out.push_back(std::move(line));
    }
    candidates_.clear();
    return out;
}

bool Tunnel::setRemoteDescription(const std::string& sdp) {
    if (!agent_) return false;
    if (juice_set_remote_description(agent_, sdp.c_str()) != JUICE_ERR_SUCCESS) {
        fail("the friend's connection details were not readable");
        return false;
    }
    if (state_ == State::Gathering) state_ = State::Connecting;
    return true;
}

void Tunnel::addRemoteCandidate(const std::string& candidate) {
    if (agent_) juice_add_remote_candidate(agent_, candidate.c_str());
}

void Tunnel::setRemoteGatheringDone() {
    if (agent_) juice_set_remote_gathering_done(agent_);
}

void Tunnel::setKeys(const Key32& shared, const Key32& joinerNonce, const Key32& hostNonce) {
    u8 salt[64];
    memcpy(salt, joinerNonce.data(), 32);
    memcpy(salt + 32, hostNonce.data(), 32);
    const Key32 prk = hkdfExtract(salt, sizeof(salt), shared.data(), shared.size());
    static const char info[] = "mcpp-tunnel-v1";
    const Bytes keys = hkdfExpand(prk, reinterpret_cast<const u8*>(info), sizeof(info) - 1, 64);
    std::lock_guard lock(mutex_);
    Key32 j2h, h2j;
    std::copy(keys.begin(), keys.begin() + 32, j2h.begin());
    std::copy(keys.begin() + 32, keys.end(), h2j.begin());
    cipher_.sendKey = config_.role == Role::Joiner ? j2h : h2j;
    cipher_.recvKey = config_.role == Role::Joiner ? h2j : j2h;
    cipher_.ready = true;
    keysSet_ = true;
}

std::string Tunnel::failure() const {
    std::lock_guard lock(mutex_);
    return failure_;
}

std::string Tunnel::route() const {
    char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {}, remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
    if (!agent_ || juice_get_selected_candidates(agent_, local, sizeof(local), remote, sizeof(remote)) != JUICE_ERR_SUCCESS)
        return "?";
    return std::string(local) + " <-> " + remote;
}

double Tunnel::idleSeconds() const { return nowSeconds() - lastRecv_.load(); }

void Tunnel::fail(const std::string& why) {
    {
        std::lock_guard lock(mutex_);
        if (failure_.empty()) failure_ = why;
    }
    if (state_ != State::Closed) state_ = State::Failed;
    wake_.notify_all();
}

void Tunnel::close() {
    if (state_ != State::Failed) state_ = State::Closed;
    running_ = false;
    wake_.notify_all();
    if (pump_.joinable()) pump_.join();
    if (agent_) juice_destroy(agent_);
    agent_ = nullptr;
    streamsByConv_.clear();
    streamCount_ = 0;
    if (listen_ != ~uintptr_t(0)) closesocket(SOCKET(listen_));
    listen_ = ~uintptr_t(0);
}

void Tunnel::sendDatagram(u8 type, const u8* data, size_t size) {
    if (!agent_ || !cipher_.ready || state_ != State::Connected) return;
    const Bytes d = cipher_.seal(type, data, size);
    juice_send(agent_, reinterpret_cast<const char*>(d.data()), d.size());
}

void Tunnel::handleDatagram(const Bytes& raw) {
    u8 type = 0;
    Bytes p;
    {
        std::lock_guard lock(mutex_);
        if (!cipher_.ready || !cipher_.open(raw.data(), raw.size(), type, p)) return;
    }
    lastRecv_ = nowSeconds();
    if (type == 'P') {
        sendDatagram('Q', nullptr, 0);
    } else if (type == 'F' && p.size() >= 4) {
        auto it = streamsByConv_.find(load32(p.data()));
        if (it != streamsByConv_.end()) it->second->peerFin = true;
    } else if (type == 'D' && p.size() >= 24) {
        const u32 conv = load32(p.data());
        auto it = streamsByConv_.find(conv);
        Stream* st = it != streamsByConv_.end() ? it->second.get() : nullptr;
        if (!st) {
            // The joiner opens conversations (odd counter); the host follows them.
            if (config_.role != Role::Host || closedConvs_.count(conv) || streamsByConv_.size() >= 16) return;
            st = newStream(conv, uintptr_t(INVALID_SOCKET));
            LOG("friends: tunnel %s: new connection %u", config_.label.c_str(), conv);
        }
        ikcp_input(st->kcp, reinterpret_cast<const char*>(p.data()), long(p.size()));
    }
}

void Tunnel::pump() {
    std::vector<char> buf(1 << 16);
    struct FinResend {
        u32 conv;
        int left;
        double next;
    };
    std::vector<FinResend> fins;
    auto sendFin = [&](u32 conv) {
        u8 c[4] = {u8(conv), u8(conv >> 8), u8(conv >> 16), u8(conv >> 24)};
        sendDatagram('F', c, 4);
        fins.push_back({conv, 3, nowSeconds() + 0.3});
    };
    while (running_) {
        std::deque<Bytes> in;
        {
            std::lock_guard lock(mutex_);
            in.swap(inbound_);
        }
        for (const Bytes& d : in) handleDatagram(d);
        const double now = nowSeconds();
        const bool live = state_ == State::Connected && keysSet_;
        if (live) {
            // Joiner: the game connects to the loopback listener.
            if (config_.role == Role::Joiner && listen_ != ~uintptr_t(0)) {
                for (;;) {
                    const SOCKET c = ::accept(SOCKET(listen_), nullptr, nullptr);
                    if (c == INVALID_SOCKET) break;
                    u_long nb = 1;
                    ioctlsocket(c, FIONBIO, &nb);
                    BOOL nd = TRUE;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
                    newStream(nextConv_, uintptr_t(c));
                    LOG("friends: tunnel %s: game connected (stream %u)", config_.label.c_str(), nextConv_);
                    nextConv_ += 2;
                }
            }
            const u32 ms = nowMs();
            std::vector<u32> dead;
            for (auto& [conv, stp] : streamsByConv_) {
                Stream& st = *stp;
                // TCP -> KCP (while the peer keeps up).
                if (st.sock != INVALID_SOCKET && !st.tcpEof && ikcp_waitsnd(st.kcp) < 2048) {
                    const int n = recv(st.sock, buf.data(), int(buf.size()), 0);
                    if (n > 0) ikcp_send(st.kcp, buf.data(), n);
                    else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) st.tcpEof = true;
                }
                // KCP -> TCP.
                while (st.toTcp.size() - st.toTcpOff < kMaxToTcp) {
                    const int size = ikcp_peeksize(st.kcp);
                    if (size <= 0) break;
                    if (size_t(size) > buf.size()) buf.resize(size_t(size));
                    const int got = ikcp_recv(st.kcp, buf.data(), size);
                    if (got <= 0) break;
                    if (st.authed) {
                        st.toTcp.insert(st.toTcp.end(), buf.begin(), buf.begin() + got);
                        continue;
                    }
                    // Host: the joiner's first bytes must be a Minecraft login as the announced player.
                    st.authBuf.insert(st.authBuf.end(), buf.begin(), buf.begin() + got);
                    int intent = 0;
                    std::string name;
                    const int r = parseLoginStart(st.authBuf.data(), st.authBuf.size(), intent, name);
                    if (r == 0 && st.authBuf.size() < 4096) continue;
                    bool ok = r == 1 && (intent == 1 || config_.expectedName.empty() || name == config_.expectedName);
                    if (!ok)
                        LOG("friends: tunnel %s: refused connection %u (%s)", config_.label.c_str(), conv,
                            r == 1 ? ("logs in as \"" + name + "\", not \"" + config_.expectedName + "\"").c_str()
                                   : "not a Minecraft login");
                    if (ok) {
                        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                        sockaddr_in a{};
                        a.sin_family = AF_INET;
                        a.sin_port = htons(config_.forwardPort);
                        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                        if (s == INVALID_SOCKET || ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
                            LOG("friends: tunnel %s: the world is not reachable on port %u", config_.label.c_str(),
                                config_.forwardPort);
                            if (s != INVALID_SOCKET) closesocket(s);
                            ok = false;
                        } else {
                            u_long nb = 1;
                            ioctlsocket(s, FIONBIO, &nb);
                            BOOL nd = TRUE;
                            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
                            st.sock = s;
                            st.authed = true;
                            st.toTcp = std::move(st.authBuf);
                            st.authBuf.clear();
                            if (intent != 1) LOG("friends: tunnel %s: %s is joining", config_.label.c_str(), name.c_str());
                        }
                    }
                    if (!ok) {
                        st.kill = true;
                        break;
                    }
                }
                // Write what the peer sent.
                if (st.sock != INVALID_SOCKET && st.toTcpOff < st.toTcp.size()) {
                    const int n = send(st.sock, reinterpret_cast<const char*>(st.toTcp.data() + st.toTcpOff),
                                       int(std::min<size_t>(st.toTcp.size() - st.toTcpOff, 1 << 20)), 0);
                    if (n > 0) st.toTcpOff += size_t(n);
                    else if (WSAGetLastError() != WSAEWOULDBLOCK) st.kill = true;
                    if (st.toTcpOff == st.toTcp.size()) st.toTcp.clear(), st.toTcpOff = 0;
                    else if (st.toTcpOff > (1 << 20)) st.toTcp.erase(st.toTcp.begin(), st.toTcp.begin() + std::ptrdiff_t(st.toTcpOff)), st.toTcpOff = 0;
                }
                ikcp_update(st.kcp, ms);
                if (st.kcp->state == IUINT32(-1)) {
                    fail("the connection to your friend was lost");
                    break;
                }
                const bool delivered = st.toTcp.empty() && ikcp_peeksize(st.kcp) < 0;
                if (st.kill || (st.tcpEof && ikcp_waitsnd(st.kcp) == 0) || (st.peerFin && delivered && st.authed))
                    dead.push_back(conv);
            }
            for (u32 conv : dead) {
                auto it = streamsByConv_.find(conv);
                if (it == streamsByConv_.end()) continue;
                if (!it->second->peerFin) sendFin(conv);
                LOG("friends: tunnel %s: connection %u closed", config_.label.c_str(), conv);
                closedConvs_.insert(conv);
                streamsByConv_.erase(it);
            }
            streamCount_ = int(streamsByConv_.size());
            for (FinResend& f : fins)
                if (f.left > 0 && now >= f.next) {
                    u8 c[4] = {u8(f.conv), u8(f.conv >> 8), u8(f.conv >> 16), u8(f.conv >> 24)};
                    sendDatagram('F', c, 4);
                    --f.left, f.next = now + 0.3;
                }
            fins.erase(std::remove_if(fins.begin(), fins.end(), [](const FinResend& f) { return f.left <= 0; }), fins.end());
            if (now - lastPing_ > 2.0) {
                lastPing_ = now;
                sendDatagram('P', nullptr, 0);
            }
            if (idleSeconds() > kIdleTimeout) fail("the connection to your friend timed out");
        }
        // Wait for sockets or datagrams (1 ms while streams exist: KCP's 10 ms clock needs a fine tick).
        fd_set r;
        FD_ZERO(&r);
        if (live && listen_ != ~uintptr_t(0)) FD_SET(SOCKET(listen_), &r);
        if (live)
            for (auto& [conv, st] : streamsByConv_)
                if (st->sock != INVALID_SOCKET && r.fd_count < FD_SETSIZE) FD_SET(st->sock, &r);
        bool pending;
        {
            std::lock_guard lock(mutex_);
            pending = !inbound_.empty();
        }
        if (pending) continue;
        if (r.fd_count > 0) {
            timeval tv{0, 1000};
            select(0, &r, nullptr, nullptr, &tv);
        } else {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(live ? 2 : 20), [this] { return !inbound_.empty() || !running_; });
        }
        if (state_ == State::Failed) {
            streamsByConv_.clear();  // closes the game's connection: it shows the disconnect
            streamCount_ = 0;
        }
    }
}

}  // namespace social
