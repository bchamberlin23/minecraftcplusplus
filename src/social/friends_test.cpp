// --friends-selftest: the friends system's building blocks against published test vectors, plus an in-process tunnel.
// --friends-node: a headless friends client (no game) for end-to-end tests through the real relays and ICE.
#include "social/friends.h"
#include "social/nostr.h"
#include "social/tunnel.h"
#include "social/upnp.h"

#include <ikcp.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <random>
#include <thread>

namespace social {

namespace {

#include "social/test_vectors.inc"

int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) ++g_failures;
    LOG("selftest %-60s %s", what, ok ? "ok" : "FAILED");
}

Bytes hexBytes(const char* h) {
    Bytes b;
    fromHex(h, b);
    return b;
}
Key32 hexKey(const char* h) {
    Key32 k{};
    fromHex(h, k);
    return k;
}

// A loopback TCP echo server (stands in for the game's server in the tunnel tests).
struct EchoServer {
    SOCKET listen = INVALID_SOCKET;
    u16 port = 0;
    std::atomic<bool> running{true};
    std::thread thread;
    std::vector<std::thread> clients;
    bool start() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        listen = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof(a);
        if (bind(listen, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(listen, 8) != 0 ||
            getsockname(listen, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return false;
        port = ntohs(a.sin_port);
        thread = std::thread([this] {
            while (running) {
                const SOCKET c = ::accept(listen, nullptr, nullptr);
                if (c == INVALID_SOCKET) break;
                clients.emplace_back([c] {
                    std::vector<char> buf(1 << 16);
                    for (;;) {
                        const int n = recv(c, buf.data(), int(buf.size()), 0);
                        if (n <= 0) break;
                        for (int done = 0; done < n;) {
                            const int s = send(c, buf.data() + done, n - done, 0);
                            if (s <= 0) break;
                            done += s;
                        }
                    }
                    closesocket(c);
                });
            }
        });
        return true;
    }
    ~EchoServer() {
        running = false;
        closesocket(listen);
        if (thread.joinable()) thread.join();
        for (std::thread& t : clients)
            if (t.joinable()) t.join();
    }
};

void writeVarInt(Bytes& b, int v) {
    u32 u = u32(v);
    do {
        u8 x = u & 0x7f;
        u >>= 7;
        if (u) x |= 0x80;
        b.push_back(x);
    } while (u);
}

// The handshake (login intent) and login start a game client sends.
Bytes loginBytes(const std::string& name) {
    Bytes hs{0x00};
    writeVarInt(hs, 775);
    const std::string host = "127.0.0.1";
    writeVarInt(hs, int(host.size()));
    hs.insert(hs.end(), host.begin(), host.end());
    hs.push_back(0x63), hs.push_back(0xDD);
    writeVarInt(hs, 2);
    Bytes ls{0x00};
    writeVarInt(ls, int(name.size()));
    ls.insert(ls.end(), name.begin(), name.end());
    for (int i = 0; i < 16; ++i) ls.push_back(u8(i));
    Bytes out;
    writeVarInt(out, int(hs.size()));
    out.insert(out.end(), hs.begin(), hs.end());
    writeVarInt(out, int(ls.size()));
    out.insert(out.end(), ls.begin(), ls.end());
    return out;
}

u16 freeUdpPort() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const u16 port = u16(40000 + std::random_device{}() % 20000);
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        const bool ok = bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
        closesocket(s);
        if (ok) return port;
    }
    return 0;
}

SOCKET connectLoopback(u16 port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD timeout = 20000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return s;
}

// Sends prefix + `megabytes` of random data through `port` and checks that the echo matches. Returns MB/s or -1.
double echoTransfer(u16 port, const Bytes& prefix, double megabytes, std::string& error) {
    const SOCKET s = connectLoopback(port);
    if (s == INVALID_SOCKET) {
        error = "connect failed";
        return -1;
    }
    Bytes data = prefix;
    std::mt19937_64 rng(12345);
    const size_t n = size_t(megabytes * 1048576.0);
    data.reserve(prefix.size() + n);
    for (size_t i = 0; i < n; i += 8) {
        const u64 v = rng();
        for (size_t k = 0; k < 8 && i + k < n; ++k) data.push_back(u8(v >> (8 * k)));
    }
    const Key32 want = sha256(data.data(), data.size());
    const Timer t;
    std::thread sender([&] {
        for (size_t off = 0; off < data.size();) {
            const int sent = send(s, reinterpret_cast<const char*>(data.data() + off), int(std::min<size_t>(data.size() - off, 1 << 16)), 0);
            if (sent <= 0) break;
            off += size_t(sent);
        }
    });
    Bytes got;
    got.reserve(data.size());
    std::vector<char> buf(1 << 16);
    while (got.size() < data.size()) {
        const int r = recv(s, buf.data(), int(buf.size()), 0);
        if (r <= 0) break;
        got.insert(got.end(), buf.begin(), buf.begin() + r);
    }
    const double secs = t.ms() / 1000.0;
    shutdown(s, SD_BOTH);
    sender.join();
    closesocket(s);
    if (got.size() != data.size()) {
        error = "echo stopped after " + std::to_string(got.size()) + " of " + std::to_string(data.size()) + " bytes";
        return -1;
    }
    if (sha256(got.data(), got.size()) != want) {
        error = "echoed data differs";
        return -1;
    }
    return double(n) / 1048576.0 / std::max(secs, 1e-3);
}

// True when the tunnel closes a connection that logs in as someone else.
bool refusedAs(u16 port, const std::string& name) {
    const SOCKET s = connectLoopback(port);
    if (s == INVALID_SOCKET) return false;
    const Bytes hs = loginBytes(name);
    send(s, reinterpret_cast<const char*>(hs.data()), int(hs.size()), 0);
    char c;
    const int r = recv(s, &c, 1, 0);  // closed (0), never echoed
    closesocket(s);
    return r == 0;
}

void selfTestCrypto() {
    {  // SHA-256 / HMAC (FIPS 180-2, RFC 4231 test case 2)
        check(toHex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(\"abc\")");
        const std::string key = "Jefe", msg = "what do ya want for nothing?";
        check(toHex(hmacSha256(reinterpret_cast<const u8*>(key.data()), key.size(), reinterpret_cast<const u8*>(msg.data()), msg.size())) ==
                  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
              "hmac-sha256 (RFC 4231 #2)");
    }
    {  // Poly1305 (RFC 8439 2.5.2), ChaCha20-Poly1305 AEAD (RFC 8439 2.8.2)
        const Bytes key = hexBytes("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
        const std::string msg = "Cryptographic Forum Research Group";
        u8 tag[16];
        poly1305(key.data(), reinterpret_cast<const u8*>(msg.data()), msg.size(), tag);
        check(toHex(tag, 16) == "a8061dc1305136c6c22b8baf0c0127a9", "poly1305 (RFC 8439 2.5.2)");
        const std::string plain =
            "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        const Bytes k = hexBytes("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        const Bytes nonce = hexBytes("070000004041424344454647"), aad = hexBytes("50515253c0c1c2c3c4c5c6c7");
        Bytes out(plain.size() + 16);
        aeadSeal(k.data(), nonce.data(), aad.data(), aad.size(), reinterpret_cast<const u8*>(plain.data()), plain.size(), out.data());
        check(toHex(out.data(), 16) == "d31a8d34648e60db7b86afbc53ef7ec2", "chacha20-poly1305 ciphertext (RFC 8439 2.8.2)");
        check(toHex(out.data() + plain.size(), 16) == "1ae10b594f09e26a7e902ecbd0600691", "chacha20-poly1305 tag (RFC 8439 2.8.2)");
        Bytes back(plain.size());
        check(aeadOpen(k.data(), nonce.data(), aad.data(), aad.size(), out.data(), out.size(), back.data()) &&
                  std::string(back.begin(), back.end()) == plain,
              "chacha20-poly1305 open");
        out[5] ^= 1;
        check(!aeadOpen(k.data(), nonce.data(), aad.data(), aad.size(), out.data(), out.size(), back.data()), "chacha20-poly1305 tamper rejected");
    }
    {  // BIP340
        int signedOk = 0, signedAll = 0, verifyOk = 0;
        for (const Bip340Vector& v : kBip340) {
            const Bytes msg = hexBytes(v.msg), sig = hexBytes(v.sig);
            if (*v.sk) {
                ++signedAll;
                KeyPair kp;
                const Key32 aux = hexKey(v.aux);
                if (keyPairFromSecret(hexKey(v.sk), kp) && _stricmp(toHex(kp.pub).c_str(), v.pk) == 0) {
                    const auto s = schnorrSign(kp, msg.data(), msg.size(), &aux);
                    signedOk += _stricmp(toHex(s.data(), 64).c_str(), v.sig) == 0;
                }
            }
            Key32 pk;
            const bool parsed = fromHex(v.pk, pk);
            const bool verified = parsed && sig.size() == 64 && schnorrVerify(pk, msg.data(), msg.size(), sig.data());
            verifyOk += verified == v.ok;
        }
        check(signedOk == signedAll, ("BIP340 signing (" + std::to_string(signedOk) + "/" + std::to_string(signedAll) + " vectors)").c_str());
        check(verifyOk == int(std::size(kBip340)),
              ("BIP340 verification (" + std::to_string(verifyOk) + "/" + std::to_string(std::size(kBip340)) + " vectors)").c_str());
    }
    {  // NIP-44 v2
        int ok = 0;
        for (const Nip44ConvVector& v : kNip44Conv) {
            Key32 k;
            ok += nip44ConversationKey(hexKey(v.sec1), hexKey(v.pub2), k) && toHex(k) == v.key;
        }
        check(ok == int(std::size(kNip44Conv)), "NIP-44 conversation keys");
        ok = 0;
        for (const auto& v : kNip44Padded) ok += int(nip44PaddedLength(size_t(v[0]))) == v[1];
        check(ok == int(std::size(kNip44Padded)), "NIP-44 padded lengths");
        int enc = 0, dec = 0;
        for (const Nip44Vector& v : kNip44) {
            KeyPair b;
            Key32 k1, k2;
            keyPairFromSecret(hexKey(v.sec2), b);
            nip44ConversationKey(hexKey(v.sec1), b.pub, k1);
            const Key32 nonce = hexKey(v.nonce);
            enc += toHex(k1) == v.key && nip44Encrypt(k1, v.plaintext, &nonce) == v.payload;
            KeyPair a;
            keyPairFromSecret(hexKey(v.sec1), a);
            nip44ConversationKey(hexKey(v.sec2), a.pub, k2);  // the other side's key is the same
            const auto plain = nip44Decrypt(k2, v.payload);
            dec += plain && *plain == v.plaintext;
        }
        check(enc == int(std::size(kNip44)), "NIP-44 encryption (published payloads)");
        check(dec == int(std::size(kNip44)), "NIP-44 decryption (both directions)");
        ok = 0;
        for (const Nip44Bad& v : kNip44Bad) ok += !nip44Decrypt(hexKey(v.key), v.payload).has_value();
        check(ok == int(std::size(kNip44Bad)), "NIP-44 invalid payloads rejected");
    }
    {  // Friend codes
        const KeyPair kp = generateKeyPair();
        const std::string code = Friends::codeFor(kp.pub);
        Key32 back{};
        check(Friends::parseCode(code, back) && back == kp.pub, ("friend code round trip (" + std::to_string(code.size()) + " chars)").c_str());
        std::string spaced = code.substr(0, 10) + " - " + code.substr(10);
        check(Friends::parseCode(spaced, back) && back == kp.pub, "friend code with spaces / dashes");
        int caught = 0, tried = 0;
        for (size_t i = 0; i < code.size(); ++i) {
            std::string bad = code;
            bad[i] = bad[i] == 'z' ? 'y' : 'z';
            ++tried;
            caught += !Friends::parseCode(bad, back);
        }
        check(caught == tried, "friend code typos rejected (checksum)");
        check(!Friends::parseCode("", back) && !Friends::parseCode("hello", back), "friend code garbage rejected");
    }
    {  // Nostr events
        const KeyPair kp = generateKeyPair();
        nlohmann::json ev = signEvent(kp, 21421, nlohmann::json::array({nlohmann::json::array({"p", "ab"})}), "hi \"there\"\n", 1700000000);
        check(verifyEvent(ev), "nostr event id + signature");
        ev["content"] = "changed";
        check(!verifyEvent(ev), "nostr event tamper rejected");
    }
    {  // Tunnel datagram cipher: round trip, replays, forgeries, reordering
        Tunnel::Cipher a, b;
        a.sendKey = b.recvKey = randomArray<32>();
        a.ready = b.ready = true;
        const std::string msg = "hello";
        std::vector<Bytes> d;
        for (int i = 0; i < 5; ++i) d.push_back(a.seal('D', reinterpret_cast<const u8*>(msg.data()), msg.size()));
        u8 type;
        Bytes out;
        bool ok = b.open(d[0].data(), d[0].size(), type, out) && type == 'D' && std::string(out.begin(), out.end()) == msg;
        ok = ok && b.open(d[3].data(), d[3].size(), type, out) && b.open(d[1].data(), d[1].size(), type, out);  // reordered
        check(ok, "tunnel cipher round trip / reordering");
        check(!b.open(d[3].data(), d[3].size(), type, out) && !b.open(d[0].data(), d[0].size(), type, out), "tunnel cipher replays dropped");
        d[2][12] ^= 0x40;
        check(!b.open(d[2].data(), d[2].size(), type, out), "tunnel cipher forgery rejected");
    }
    {  // Login check of the host side
        const Bytes login = loginBytes("Alex_123");
        int intent = 0;
        std::string name;
        check(parseLoginStart(login.data(), login.size(), intent, name) == 1 && intent == 2 && name == "Alex_123", "login start parsed");
        check(parseLoginStart(login.data(), 10, intent, name) == 0, "partial login waits for more");
        const char junk[] = "\x10\x07 not a handshake";  // a 16-byte packet with id 7
        check(parseLoginStart(reinterpret_cast<const u8*>(junk), sizeof(junk) - 1, intent, name) == -1, "non-Minecraft connection refused");
    }
}

void selfTestKcp() {
    // Two KCP ends over a simulated path that drops 10% and reorders: 4 MB must arrive intact and in order.
    struct Wire {
        std::vector<std::pair<u32, Bytes>> inFlight;  // (deliver at ms, datagram)
        std::mt19937 rng{7};
    };
    Wire ab, ba;
    struct Ctx {
        Wire* out;
        u32* clock;
    };
    u32 clock = 0;
    Ctx ca{&ab, &clock}, cb{&ba, &clock};
    ikcpcb* a = ikcp_create(1, &ca);
    ikcpcb* b = ikcp_create(1, &cb);
    for (ikcpcb* k : {a, b}) {
        k->stream = 1;
        ikcp_setmtu(k, 1150);
        ikcp_nodelay(k, 1, 10, 2, 1);
        ikcp_wndsize(k, 1024, 1024);
        ikcp_setoutput(k, [](const char* buf, int len, ikcpcb*, void* user) {
            Ctx* c = static_cast<Ctx*>(user);
            if (c->out->rng() % 10 == 0) return 0;  // lost
            c->out->inFlight.push_back({*c->clock + 20 + c->out->rng() % 30, Bytes(buf, buf + len)});
            return 0;
        });
    }
    Bytes data(4 << 20);
    std::mt19937 rng(99);
    for (u8& x : data) x = u8(rng());
    Bytes got;
    size_t sent = 0;
    for (clock = 0; clock < 600000 && got.size() < data.size(); clock += 5) {
        while (sent < data.size() && ikcp_waitsnd(a) < 2048) {
            const size_t n = std::min<size_t>(32768, data.size() - sent);
            ikcp_send(a, reinterpret_cast<const char*>(data.data() + sent), int(n));
            sent += n;
        }
        for (auto [wire, to] : {std::pair{&ab, b}, std::pair{&ba, a}}) {
            auto& f = wire->inFlight;
            for (size_t i = 0; i < f.size();)
                if (f[i].first <= clock) {
                    ikcp_input(to, reinterpret_cast<const char*>(f[i].second.data()), long(f[i].second.size()));
                    f[i] = std::move(f.back());
                    f.pop_back();
                } else {
                    ++i;
                }
        }
        char buf[65536];
        for (;;) {
            const int n = ikcp_recv(b, buf, sizeof(buf));
            if (n <= 0) break;
            got.insert(got.end(), buf, buf + n);
        }
        ikcp_update(a, clock);
        ikcp_update(b, clock);
    }
    ikcp_release(a);
    ikcp_release(b);
    check(got == data, ("KCP stream integrity, 4 MB over 10% loss + reordering (" + std::to_string(clock) + " ms simulated)").c_str());
}

void selfTestTunnel() {
    // Two tunnels in this process over loopback ICE (host candidates only, no STUN): the joiner's TCP stream reaches
    // an echo server through the host, sealed, with the login name checked.
    EchoServer echo;
    if (!echo.start()) return check(false, "tunnel: echo server");
    const KeyPair j = generateKeyPair(), h = generateKeyPair();
    Key32 sharedJ, sharedH;
    sharedX(j.secret, h.pub, sharedJ);
    sharedX(h.secret, j.pub, sharedH);
    check(sharedJ == sharedH, "ECDH shared secret agrees");
    const Key32 nj = randomArray<32>(), nh = randomArray<32>();
    Tunnel::Config jc, hc;
    jc.role = Tunnel::Role::Joiner, jc.stunHost.clear(), jc.label = "selftest-joiner", jc.muxPort = freeUdpPort();
    hc.role = Tunnel::Role::Host, hc.stunHost.clear(), hc.label = "selftest-host", hc.forwardPort = echo.port, hc.expectedName = "Alex";
    do hc.muxPort = freeUdpPort();
    while (hc.muxPort == jc.muxPort);
    Tunnel joiner(jc), host(hc);
    joiner.setKeys(sharedJ, nj, nh);
    host.setKeys(sharedH, nj, nh);
    if (!joiner.start() || !host.start()) return check(false, "tunnel: start");
    for (int i = 0; i < 100 && !(joiner.gathered() && host.gathered()); ++i) Sleep(20);
    host.setRemoteDescription(joiner.localDescription());
    // The host's description as sent with a UPnP mapping (an unreachable documentation address here): still accepted.
    const std::string hostSdp = withCandidate(host.localDescription(), UpnpMapping::candidateFor("192.0.2.1", 9));
    check(hostSdp.find("typ srflx") != std::string::npos && joiner.setRemoteDescription(hostSdp),
          "tunnel: a description with a UPnP candidate is accepted");
    for (int i = 0; i < 250 && joiner.state() != Tunnel::State::Connected; ++i) Sleep(20);
    check(joiner.state() == Tunnel::State::Connected, "tunnel: ICE connected over loopback");
    if (joiner.state() != Tunnel::State::Connected) return;
    check(refusedAs(joiner.listenPort(), "Steve"), "tunnel: a login under another name is refused");
    std::string error;
    const double mbps = echoTransfer(joiner.listenPort(), loginBytes("Alex"), 8.0, error);
    check(mbps > 0, ("tunnel: 8 MB echoed intact through the tunnel" + (mbps > 0 ? "" : " (" + error + ")")).c_str());
    // The joiner leaves without a word: the host's side must notice and close cleanly.
    const Timer tc;
    joiner.close();
    LOG("selftest: joiner closed in %.0f ms", tc.ms());
    const Timer tf;
    while (host.state() == Tunnel::State::Connected && tf.ms() < 60000) Sleep(100);
    LOG("selftest: host state %d after %.1f s", int(host.state()), tf.ms() / 1000.0);
    const Timer th;
    host.close();
    check(th.ms() < 5000 && tf.ms() < 60000, "tunnel: a peer leaving is noticed and both sides close");
}

}  // namespace

int runFriendsSelfTest() {
    selfTestCrypto();
    selfTestKcp();
    selfTestTunnel();
    LOG("selftest friends: %s (%d failure%s)", g_failures ? "FAILED" : "all passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

// --- headless node ------------------------------------------------------------------------------------------------

int runFriendsNode(int argc, wchar_t** argv) {
    std::string name = "Node", addCode, joinCode, inviteCode;
    fs::path dir;
    bool printCode = false, autoAccept = false, hostEcho = false, checkName = false, joinNow = false;
    double seconds = 120, transferMb = 4;
    std::vector<std::string> relays;
    auto narrow = [](const wchar_t* w) { return fs::path(w).string(); };
    for (int i = 0; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? narrow(argv[++i]) : std::string(); };
        if (a == L"--dir") dir = fs::path(next());
        else if (a == L"--name") name = next();
        else if (a == L"--print-code") printCode = true;
        else if (a == L"--add") addCode = next();
        else if (a == L"--auto-accept") autoAccept = true;
        else if (a == L"--host-echo") hostEcho = true;
        else if (a == L"--invite") inviteCode = next();
        else if (a == L"--join") joinCode = next();
        else if (a == L"--join-now") joinNow = true;
        else if (a == L"--transfer-mb") transferMb = std::atof(next().c_str());
        else if (a == L"--check-name-gate") checkName = true;
        else if (a == L"--seconds") seconds = std::atof(next().c_str());
        else if (a == L"--relays") {
            std::string list = next();
            for (size_t p = 0; p < list.size();) {
                const size_t c = list.find(',', p);
                relays.push_back(list.substr(p, c == std::string::npos ? std::string::npos : c - p));
                p = c == std::string::npos ? list.size() : c + 1;
            }
        } else {
            LOG("usage: --friends-node [--dir <dir>] [--name <name>] [--print-code] [--add <code>] [--auto-accept]\n"
                "                      [--host-echo] [--invite <code>] [--join <code> [--join-now] [--transfer-mb <n>] [--check-name-gate]]\n"
                "                      [--relays wss://a,wss://b] [--seconds <n>]");
            return 2;
        }
    }
    Friends& f = Friends::get();
    Friends::Config cfg;
    cfg.dir = dir.empty() ? Friends::defaultDir() : dir;
    cfg.name = name;
    cfg.relays = relays;
    if (!f.start(cfg)) return 1;
    const FriendsSnapshot first = f.snapshot();
    LOG("node: code %s", first.myCode.c_str());
    if (printCode) {
        f.stop();
        return 0;
    }
    std::unique_ptr<EchoServer> echo;
    if (hostEcho) {
        echo = std::make_unique<EchoServer>();
        if (!echo->start()) return 1;
        f.setActivity(Presence::InWorld, "Echo World");
        f.setHost(echo->port, inviteCode.empty());
        LOG("node: hosting an echo server on port %u", echo->port);
    }
    Key32 joinKey{}, inviteKey{};
    if (!joinCode.empty() && !Friends::parseCode(joinCode, joinKey)) return LOG("node: bad --join code"), 2;
    if (!inviteCode.empty() && !Friends::parseCode(inviteCode, inviteKey)) return LOG("node: bad --invite code"), 2;
    bool added = addCode.empty(), invited = inviteCode.empty(), joined = joinCode.empty();
    int result = 0;
    const double t0 = nowSeconds();
    while (nowSeconds() - t0 < seconds) {
        Sleep(200);
        const FriendsSnapshot s = f.snapshot();
        if (!added && s.relaysConnected > 0) {
            const Friends::AddResult r = f.add(addCode);
            LOG("node: add -> %d", int(r));
            added = true;
        }
        for (const FriendEvent& e : f.takeEvents())
            LOG("node: event %d from %s %s", int(e.type), e.name.c_str(), e.world.c_str());
        if (autoAccept)
            for (const RequestInfo& r : s.incoming) {
                LOG("node: accepting %s", r.name.c_str());
                f.accept(r.pub);
            }
        if (!invited)
            for (const FriendInfo& fr : s.friends)
                if (fr.pub == inviteKey && fr.presence != Presence::Offline) {
                    invited = f.invite(fr.pub);
                    LOG("node: invited %s: %d", fr.name.c_str(), int(invited));
                }
        if (!joined)
            for (const FriendInfo& fr : s.friends)
                if (fr.pub == joinKey && (fr.joinable || fr.invitedMe || (joinNow && fr.presence != Presence::Offline))) {
                    joined = true;
                    LOG("node: %s is %s; joining", fr.name.c_str(), fr.joinable ? "joinable" : "inviting us");
                    std::string error;
                    const Timer jt;
                    const u16 port = f.join(fr.pub, 30, error);
                    if (!port) {
                        LOG("node: JOIN FAILED: %s", error.c_str());
                        result = 1;
                        break;
                    }
                    LOG("node: tunnel up in %.1f s, local port %u", jt.ms() / 1000.0, port);
                    if (checkName) {
                        const bool refused = refusedAs(port, "Impostor");
                        LOG("node: name gate %s", refused ? "PASSED (other name refused)" : "FAILED");
                        if (!refused) result = 1;
                    }
                    const double mbps = echoTransfer(port, loginBytes(name), transferMb, error);
                    if (mbps > 0) {
                        LOG("node: TRANSFER OK: %.1f MB echoed intact (%.2f MB/s)", transferMb, mbps);
                    } else {
                        LOG("node: TRANSFER FAILED: %s", error.c_str());
                        result = 1;
                    }
                    seconds = std::min(seconds, nowSeconds() - t0 + 3);  // done: leave soon
                }
        static double lastStatus = 0;
        if (nowSeconds() - lastStatus > 10) {
            lastStatus = nowSeconds();
            std::string list;
            for (const FriendInfo& fr : s.friends) list += " " + fr.name + "(" + std::to_string(int(fr.presence)) + (fr.joinable ? ",joinable" : "") + ")";
            LOG("node: relays %d/%d, friends:%s, incoming %zu, outgoing %zu, guests %d", s.relaysConnected, s.relaysTotal,
                list.c_str(), s.incoming.size(), s.outgoing.size(), s.guests);
        }
    }
    if (!joined) {
        LOG("node: JOIN FAILED: the friend never became joinable");
        result = 1;
    }
    f.stop();
    return result;
}

}  // namespace social
