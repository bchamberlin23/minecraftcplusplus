#include "net/connection.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <libdeflate.h>

#include <algorithm>

namespace net {

namespace {
constexpr size_t kMaxPacket = size_t(8) << 20;  // the game caps packets at 8 MB

struct WinsockInit {
    bool ok = false;
    WinsockInit() {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};

void putVarInt(std::vector<u8>& out, u32 v) {
    while (v >= 0x80) {
        out.push_back(u8(v | 0x80));
        v >>= 7;
    }
    out.push_back(u8(v));
}

// Parses a VarInt at p (up to `avail` bytes). Returns bytes used, 0 if incomplete, -1 if invalid.
int getVarInt(const u8* p, size_t avail, u32& v) {
    v = 0;
    for (int i = 0; i < 5; ++i) {
        if (size_t(i) >= avail) return 0;
        v |= u32(p[i] & 0x7F) << (7 * i);
        if (!(p[i] & 0x80)) return i + 1;
    }
    return -1;
}
}  // namespace

Connection::Connection() : socket_(INVALID_SOCKET) {
    static WinsockInit winsock;
    compressor_ = libdeflate_alloc_compressor(4);
    decompressor_ = libdeflate_alloc_decompressor();
    rbuf_.resize(size_t(1) << 18);
}

Connection::~Connection() {
    close();
    libdeflate_free_compressor(compressor_);
    libdeflate_free_decompressor(decompressor_);
}

bool Connection::open(const std::string& host, u16 port, std::string& error) {
    addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0) {
        error = "cannot resolve " + host;
        return false;
    }
    // IPv4 first, like Java: a server bound to 127.0.0.1 would otherwise cost a refused ::1 attempt (~2 s).
    std::vector<addrinfo*> candidates;
    for (addrinfo* a = result; a; a = a->ai_next) candidates.push_back(a);
    std::stable_partition(candidates.begin(), candidates.end(), [](addrinfo* a) { return a->ai_family == AF_INET; });
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a : candidates) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (::connect(s, a->ai_addr, int(a->ai_addrlen)) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if (s == INVALID_SOCKET) {
        error = "cannot connect to " + host + ":" + std::to_string(port);
        return false;
    }
    BOOL noDelay = TRUE;  // small movement packets every tick
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    DWORD timeoutMs = 30000;  // Connection's ReadTimeoutHandler(30): nothing for 30 s is a timeout
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    socket_ = s;
    return true;
}

void Connection::close() {
    const SOCKET s = SOCKET(socket_);
    if (s == INVALID_SOCKET) return;
    shutdown(s, SD_BOTH);
    closesocket(s);
    socket_ = INVALID_SOCKET;
}

void Connection::setCipher(std::unique_ptr<StreamCipher> cipher) {
    std::lock_guard lock(sendMutex_);
    if (cipher && rend_ > rpos_) cipher->decrypt(&rbuf_[rpos_], rend_ - rpos_);  // already buffered bytes
    cipher_ = std::move(cipher);
}

bool Connection::fill() {
    if (rpos_ > 0) {  // keep the unread tail at the front
        std::memmove(rbuf_.data(), rbuf_.data() + rpos_, rend_ - rpos_);
        rend_ -= rpos_;
        rpos_ = 0;
    }
    if (rend_ == rbuf_.size()) rbuf_.resize(rbuf_.size() * 2);
    const int n = recv(SOCKET(socket_), reinterpret_cast<char*>(rbuf_.data() + rend_), int(rbuf_.size() - rend_), 0);
    if (n <= 0) {
        if (n < 0 && WSAGetLastError() == WSAETIMEDOUT) timedOut_ = true;
        return false;
    }
    if (cipher_) cipher_->decrypt(rbuf_.data() + rend_, size_t(n));
    rend_ += size_t(n);
    received_ += u64(n);
    return true;
}

bool Connection::read(std::vector<u8>& packet) {
    u32 length = 0;
    int used;
    while ((used = getVarInt(rbuf_.data() + rpos_, rend_ - rpos_, length)) == 0)
        if (!fill()) return false;
    if (used < 0 || length == 0 || length > kMaxPacket) return false;
    while (rend_ - rpos_ < size_t(used) + length)
        if (!fill()) return false;
    const u8* body = rbuf_.data() + rpos_ + size_t(used);
    rpos_ += size_t(used) + length;
    if (threshold_ < 0) {
        packet.assign(body, body + length);
        return true;
    }
    u32 rawSize = 0;
    const int h = getVarInt(body, length, rawSize);
    if (h <= 0 || rawSize > kMaxPacket) return false;
    if (rawSize == 0) {  // below the threshold: sent uncompressed
        packet.assign(body + h, body + length);
        return true;
    }
    packet.resize(rawSize);
    size_t actual = 0;
    return libdeflate_zlib_decompress(decompressor_, body + h, length - size_t(h), packet.data(), rawSize, &actual) ==
               LIBDEFLATE_SUCCESS &&
           actual == rawSize;
}

bool Connection::send(const std::vector<u8>& packet) {
    std::lock_guard lock(sendMutex_);
    if (SOCKET(socket_) == INVALID_SOCKET) return false;
    frame_.clear();
    const int threshold = threshold_;
    if (threshold < 0) {
        putVarInt(frame_, u32(packet.size()));
        frame_.insert(frame_.end(), packet.begin(), packet.end());
    } else if (packet.size() < size_t(threshold)) {
        putVarInt(frame_, u32(packet.size() + 1));
        frame_.push_back(0);
        frame_.insert(frame_.end(), packet.begin(), packet.end());
    } else {
        std::vector<u8> body;
        putVarInt(body, u32(packet.size()));
        const size_t header = body.size();
        body.resize(header + libdeflate_zlib_compress_bound(compressor_, packet.size()));
        const size_t n = libdeflate_zlib_compress(compressor_, packet.data(), packet.size(), body.data() + header,
                                                  body.size() - header);
        if (n == 0) return false;
        body.resize(header + n);
        putVarInt(frame_, u32(body.size()));
        frame_.insert(frame_.end(), body.begin(), body.end());
    }
    if (cipher_) cipher_->encrypt(frame_.data(), frame_.size());
    size_t sent = 0;
    while (sent < frame_.size()) {
        const int n = ::send(SOCKET(socket_), reinterpret_cast<const char*>(frame_.data() + sent), int(frame_.size() - sent), 0);
        if (n <= 0) return false;
        sent += size_t(n);
    }
    return true;
}

}  // namespace net
