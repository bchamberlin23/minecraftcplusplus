#include "social/friends.h"

#include "social/nostr.h"
#include "social/tunnel.h"
#include "social/upnp.h"

#include <json/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <set>

namespace social {

using nlohmann::json;

namespace {

// Event kinds: ephemeral (20000-29999: relays pass them on without storing) for presence, invites and connection
// setup; a regular kind for what an offline friend must still get (with a NIP-40 expiration).
constexpr int kEphemeralKind = 21421;
constexpr int kDurableKind = 4421;
constexpr i64 kDurableLifetime = 7 * 86400;
const char* const kDefaultRelays[] = {"wss://relay.damus.io", "wss://nos.lol", "wss://relay.primal.net", "wss://nostr.mom"};
constexpr double kHeartbeat = 60, kOfflineAfter = 150, kInviteValid = 600, kQuietStart = 15;
constexpr i64 kMaxSkew = 300;  // ephemeral messages older (or newer) than this are replays or broken clocks
constexpr u8 kCodeVersion = 1;

const char* kBase58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

std::string base58(const Bytes& in) {
    std::vector<u8> digits;  // base 58, least significant first
    for (u8 byte : in) {
        u32 carry = byte;
        for (u8& d : digits) {
            carry += u32(d) << 8;
            d = u8(carry % 58);
            carry /= 58;
        }
        while (carry) digits.push_back(u8(carry % 58)), carry /= 58;
    }
    std::string s;
    for (u8 b : in) {
        if (b) break;
        s += '1';
    }
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) s += kBase58[*it];
    return s;
}

bool unbase58(std::string_view text, Bytes& out) {
    std::vector<u8> bytes;  // base 256, least significant first
    for (char c : text) {
        const char* p = strchr(kBase58, c);
        if (!p || !c) return false;
        u32 carry = u32(p - kBase58);
        for (u8& b : bytes) {
            carry += u32(b) * 58;
            b = u8(carry & 0xff);
            carry >>= 8;
        }
        while (carry) bytes.push_back(u8(carry & 0xff)), carry >>= 8;
    }
    out.clear();
    for (char c : text) {
        if (c != '1') break;
        out.push_back(0);
    }
    out.insert(out.end(), bytes.rbegin(), bytes.rend());
    return true;
}

std::array<u8, 4> codeChecksum(const u8* versionAndKey) {
    Bytes b(reinterpret_cast<const u8*>("mcpp-friend-code"), reinterpret_cast<const u8*>("mcpp-friend-code") + 16);
    b.insert(b.end(), versionAndKey, versionAndKey + 33);
    const Key32 h = sha256(b.data(), b.size());
    return {h[0], h[1], h[2], h[3]};
}

std::string tagHash(const char* label, std::initializer_list<const Key32*> parts) {
    Bytes b(reinterpret_cast<const u8*>(label), reinterpret_cast<const u8*>(label) + strlen(label));
    for (const Key32* k : parts) b.insert(b.end(), k->begin(), k->end());
    return toHex(sha256(b.data(), b.size()));
}

bool validName(const std::string& n) {
    if (n.empty() || n.size() > 16) return false;
    for (char c : n)
        if (!(isalnum(u8(c)) || c == '_')) return false;
    return true;
}

bool writeAtomic(const fs::path& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), std::streamsize(data.size()));
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::string readText(const fs::path& path) {
    std::vector<u8> b;
    if (!readFile(path, b)) return {};
    return std::string(b.begin(), b.end());
}

bool protect(const Bytes& in, Bytes& out, bool encrypt) {
    DATA_BLOB a{DWORD(in.size()), const_cast<BYTE*>(in.data())}, b{};
    const BOOL ok = encrypt ? CryptProtectData(&a, L"minecraftCplusplus friends", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &b)
                            : CryptUnprotectData(&a, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &b);
    if (!ok) return false;
    out.assign(b.pbData, b.pbData + b.cbData);
    SecureZeroMemory(b.pbData, b.cbData);
    LocalFree(b.pbData);
    return true;
}

u16 pickUdpPort() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    for (int attempt = 0; attempt < 20; ++attempt) {
        u16 r;
        randomBytes(reinterpret_cast<u8*>(&r), sizeof(r));
        const u16 port = u16(40000 + r % 20000);
        SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        const bool ok = s != INVALID_SOCKET && bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
        if (s != INVALID_SOCKET) closesocket(s);
        if (ok) return port;
    }
    return 0;
}

}  // namespace

// --- codes --------------------------------------------------------------------------------------------------------

std::string Friends::codeFor(const Key32& pub) {
    Bytes b{kCodeVersion};
    b.insert(b.end(), pub.begin(), pub.end());
    const auto sum = codeChecksum(b.data());
    b.insert(b.end(), sum.begin(), sum.end());
    return base58(b);
}

bool Friends::parseCode(const std::string& code, Key32& out) {
    std::string clean;
    for (char c : code)
        if (!isspace(u8(c)) && c != '-') clean += c;
    Bytes b;
    if (clean.empty() || !unbase58(clean, b) || b.size() != 37 || b[0] != kCodeVersion) return false;
    const auto sum = codeChecksum(b.data());
    if (!std::equal(sum.begin(), sum.end(), b.begin() + 33)) return false;
    std::copy(b.begin() + 1, b.begin() + 33, out.begin());
    return validPublicKey(out);
}

fs::path Friends::defaultDir() {
    if (const wchar_t* d = _wgetenv(L"MCPP_FRIENDS_DIR"); d && *d) return fs::path(d);
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return fs::path(local ? local : L".") / "minecraftCplusplus" / "friends";
}

// --- state --------------------------------------------------------------------------------------------------------

namespace {

struct Peer {
    enum class Rel { Friend, Incoming, Outgoing };
    Key32 pub{}, shared{}, conv{};
    std::string hex, name, tagToMe, tagToThem;
    Rel rel = Rel::Friend;
    i64 since = 0, lastSeen = 0, requestTime = 0;
    // Presence (friends).
    double heard = -1e9, invitedAt = -1e9, lastReply = -1e9;
    Presence presence = Presence::Offline;
    std::string world;
    bool joinable = false, invitedByMe = false;
};

struct Session {
    std::string sid, peerHex, name;
    bool host = false;
    std::unique_ptr<Tunnel> tunnel;
    Key32 nonce{}, peerNonce{};
    bool descSent = false, answered = false, doneSent = false, upnpSent = false;
    double created = 0, lastCand = 0, lastActive = 0, lastAsk = 0;
    int asks = 0;
    json ask;  // joiner: the join message (sent again until answered)
    std::string error;
};

const char* relName(Peer::Rel r) { return r == Peer::Rel::Friend ? "friend" : r == Peer::Rel::Incoming ? "incoming" : "outgoing"; }

}  // namespace

struct Friends::Impl {
    Config cfg;
    KeyPair id;
    std::string myHex, inboxTag;
    mutable std::mutex mutex;
    std::map<std::string, Peer> peers;  // by public key hex
    std::map<std::string, i64> declined;
    std::deque<std::string> seenDurable;
    std::set<std::string> seenDurableSet;
    i64 lastDurable = 0;
    bool dirty = false, subDirty = true, presenceDirty = true, probed = false;
    RelayPool pool;
    std::deque<json> inbound;
    std::condition_variable wake;
    std::thread worker;
    std::atomic<bool> running{false};
    std::vector<FriendEvent> events;
    Presence activity = Presence::Online;
    std::string world;
    u16 hostPort = 0;
    bool openToFriends = false;
    double startedAt = 0, lastHeartbeat = -1e9, firstConnected = -1;
    u16 muxPort = 0;
    std::string stunHost = "stun.l.google.com";
    u16 stunPort = 19302;
    UpnpMapping upnp;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::vector<std::string> answeredSids;  // join requests seen (repeats are ignored)

    // --- persistence (mutex held) ---
    fs::path listPath() const { return cfg.dir / "friends.json"; }
    bool loadIdentity();
    void load();
    void save();

    Peer& addPeer(const Key32& pub, Peer::Rel rel) {
        const std::string hex = toHex(pub);
        Peer& p = peers[hex];
        p.pub = pub, p.hex = hex, p.rel = rel;
        sharedX(id.secret, pub, p.shared);
        nip44ConversationKey(id.secret, pub, p.conv);
        p.tagToMe = tagHash("mcpp-pair-v1", {&p.shared, &id.pub});
        p.tagToThem = tagHash("mcpp-pair-v1", {&p.shared, &p.pub});
        subDirty = true;
        dirty = true;
        return p;
    }

    // --- sending (mutex held) ---
    void sendTo(const Peer& p, json m, bool durable, bool urgent = false) {
        m["ts"] = unixNow();
        const std::string content = nip44Encrypt(p.conv, m.dump());
        publish(p.tagToThem, content, durable, urgent);
    }
    void publish(const std::string& tag, const std::string& content, bool durable, bool urgent = false) {
        const i64 now = unixNow();
        json tags = json::array({json::array({"p", tag})});
        if (durable) tags.push_back(json::array({"expiration", std::to_string(now + kDurableLifetime)}));
        const KeyPair author = generateKeyPair();  // a fresh key per message: nothing links them
        pool.publish(signEvent(author, durable ? kDurableKind : kEphemeralKind, tags, content, now), durable, urgent);
    }
    void sendRequest(const Peer& p) {
        const i64 ts = unixNow();
        const std::string signedText = "mcpp-req-v1\n" + p.hex + "\n" + myHex + "\n" + cfg.name + "\n" + std::to_string(ts);
        const Key32 h = sha256(signedText);
        const auto sig = schnorrSign(id, h.data(), h.size());
        const json m{{"t", "req"}, {"from", myHex}, {"name", cfg.name}, {"ts", ts}, {"sig", toHex(sig.data(), sig.size())}};
        // To the inbox: encrypted from the throwaway author key (the recipient does not know us yet).
        const KeyPair author = generateKeyPair();
        Key32 conv;
        if (!nip44ConversationKey(author.secret, p.pub, conv)) return;
        const std::string content = nip44Encrypt(conv, m.dump());
        const i64 now = unixNow();
        const json tags = json::array({json::array({"p", tagHash("mcpp-inbox-v1", {&p.pub})}),
                                       json::array({"expiration", std::to_string(now + kDurableLifetime)})});
        pool.publish(signEvent(author, kDurableKind, tags, content, now), true);
    }
    json presenceMessage(bool want) const {
        json m{{"t", "pres"}, {"name", cfg.name}, {"want", want}};
        m["st"] = activity == Presence::InWorld ? "world" : activity == Presence::Multiplayer ? "mp" : "online";
        if (activity != Presence::Online && !world.empty()) m["world"] = world;
        m["join"] = hostPort != 0 && openToFriends;
        return m;
    }
    void broadcastPresence(bool want) {
        for (auto& [hex, p] : peers)
            if (p.rel == Peer::Rel::Friend) sendTo(p, presenceMessage(want), false);
    }
    json filters() const {
        json tags = json::array({inboxTag});
        for (const auto& [hex, p] : peers)
            if (p.rel != Peer::Rel::Incoming) tags.push_back(p.tagToMe);
        const i64 now = unixNow();
        const i64 since = std::max(lastDurable - 3600, now - kDurableLifetime);
        return json::array({json{{"kinds", {kEphemeralKind}}, {"#p", tags}, {"since", now - 60}},
                            json{{"kinds", {kDurableKind}}, {"#p", tags}, {"since", since}}});
    }
    void event(FriendEvent::Type t, const Peer& p, std::string world = {}) {
        FriendEvent e;
        e.type = t, e.pub = p.pub, e.name = p.name, e.world = std::move(world);
        events.push_back(std::move(e));
        if (events.size() > 64) events.erase(events.begin());
    }
    void removePeer(const std::string& hex) {
        for (auto it = sessions.begin(); it != sessions.end();)
            if (it->second->peerHex == hex && it->second->host) it = sessions.erase(it);
            else ++it;
        peers.erase(hex);
        subDirty = dirty = true;
    }

    // --- receiving (worker thread, mutex held) ---
    void handle(const json& ev);
    void handleRequest(const json& m);
    void handleMessage(Peer& p, const json& m, bool durable);
    void handleJoin(Peer& p, const json& m);
    void tick();
    void run();
};

Friends& Friends::get() {
    static Friends f;
    return f;
}

Friends::Friends() : impl_(std::make_unique<Impl>()) {}
Friends::~Friends() { stop(); }

bool Friends::started() const { return impl_->running; }

bool Friends::Impl::loadIdentity() {
    const fs::path path = cfg.dir / "identity.dat";
    std::vector<u8> blob;
    if (readFile(path, blob) && !blob.empty()) {
        Bytes secret;
        Key32 sk;
        if (!protect(blob, secret, false) || secret.size() != 32) {
            LOG("error: friends: %s cannot be read by this Windows account", path.string().c_str());
            return false;
        }
        std::copy(secret.begin(), secret.end(), sk.begin());
        SecureZeroMemory(secret.data(), secret.size());
        if (!keyPairFromSecret(sk, id)) return false;
        return true;
    }
    id = generateKeyPair();
    Bytes secret(id.secret.begin(), id.secret.end()), sealed;
    if (!protect(secret, sealed, true)) return false;
    SecureZeroMemory(secret.data(), secret.size());
    if (!writeAtomic(path, std::string(sealed.begin(), sealed.end()))) {
        LOG("error: friends: cannot write %s", path.string().c_str());
        return false;
    }
    LOG("friends: new identity made");
    return true;
}

void Friends::Impl::load() {
    const json j = json::parse(readText(listPath()), nullptr, false);
    if (!j.is_object()) return;
    try {
        for (const json& e : j.value("peers", json::array())) {
            Key32 pub;
            if (!fromHex(e.value("pub", std::string()), pub) || !validPublicKey(pub)) continue;
            const std::string rel = e.value("rel", std::string("friend"));
            Peer& p = addPeer(pub, rel == "incoming" ? Peer::Rel::Incoming : rel == "outgoing" ? Peer::Rel::Outgoing : Peer::Rel::Friend);
            p.name = e.value("name", std::string());
            p.since = e.value("since", i64(0));
            p.lastSeen = e.value("lastSeen", i64(0));
            p.requestTime = e.value("requestTime", i64(0));
        }
        for (auto& [k, v] : j.value("declined", json::object()).items()) declined[k] = v.get<i64>();
        lastDurable = j.value("lastDurable", i64(0));
        for (const json& s : j.value("seen", json::array())) {
            seenDurable.push_back(s.get<std::string>());
            seenDurableSet.insert(seenDurable.back());
        }
    } catch (const std::exception& e) {
        LOG("friends: friends.json: %s", e.what());
    }
    dirty = false;
}

void Friends::Impl::save() {
    json peersJ = json::array();
    for (const auto& [hex, p] : peers)
        peersJ.push_back({{"pub", hex}, {"name", p.name}, {"rel", relName(p.rel)}, {"since", p.since}, {"lastSeen", p.lastSeen},
                          {"requestTime", p.requestTime}});
    json j{{"version", 1}, {"peers", peersJ}, {"declined", declined}, {"lastDurable", lastDurable},
           {"seen", json(std::vector<std::string>(seenDurable.begin(), seenDurable.end()))}};
    if (!writeAtomic(listPath(), j.dump(1))) LOG("friends: could not save %s", listPath().string().c_str());
    dirty = false;
}

bool Friends::start(const Config& config) {
    stop();
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    m.cfg = config;
    if (m.cfg.dir.empty()) m.cfg.dir = defaultDir();
    if (m.cfg.relays.empty()) m.cfg.relays.assign(std::begin(kDefaultRelays), std::end(kDefaultRelays));
    std::error_code ec;
    fs::create_directories(m.cfg.dir, ec);
    m.peers.clear(), m.declined.clear(), m.seenDurable.clear(), m.seenDurableSet.clear(), m.sessions.clear();
    if (!m.loadIdentity()) return false;
    m.myHex = toHex(m.id.pub);
    m.inboxTag = tagHash("mcpp-inbox-v1", {&m.id.pub});
    m.load();
    m.muxPort = pickUdpPort();
    m.startedAt = nowSeconds();
    m.probed = false, m.subDirty = m.presenceDirty = true, m.firstConnected = -1;
    m.running = true;
    m.pool.start(m.cfg.relays, [this](const json& ev, const std::string&) {
        Impl& im = *impl_;
        {
            std::lock_guard l(im.mutex);
            if (im.inbound.size() < 1024) im.inbound.push_back(ev);
        }
        im.wake.notify_one();
    });
    m.worker = std::thread([this] { impl_->run(); });
    LOG("friends: started as %s (code %s), %zu friends, UDP port %u", m.cfg.name.c_str(), codeFor(m.id.pub).c_str(),
        m.peers.size(), m.muxPort);
    return true;
}

void Friends::stop() {
    Impl& m = *impl_;
    if (!m.running) return;
    {
        std::lock_guard lock(m.mutex);
        for (auto& [hex, p] : m.peers)
            if (p.rel == Peer::Rel::Friend) m.sendTo(p, json{{"t", "bye"}}, false);
        m.running = false;
    }
    m.wake.notify_all();
    if (m.worker.joinable()) m.worker.join();
    std::map<std::string, std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(m.mutex);
        sessions.swap(m.sessions);
        if (m.dirty) m.save();
    }
    sessions.clear();  // closes the tunnels
    m.pool.flush(1.5);  // the goodbyes
    m.pool.stop();
    m.upnp.stop();
    LOG("friends: stopped");
}

// --- worker -------------------------------------------------------------------------------------------------------

void Friends::Impl::run() {
    {  // libjuice takes one STUN server: Google's, or Cloudflare's when that name does not resolve
        addrinfo* res = nullptr;
        if (getaddrinfo("stun.l.google.com", "19302", nullptr, &res) != 0) {
            std::lock_guard lock(mutex);
            stunHost = "stun.cloudflare.com", stunPort = 3478;
        }
        if (res) freeaddrinfo(res);
    }
    while (running) {
        std::deque<json> in;
        {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(100), [this] { return !inbound.empty() || !running; });
            in.swap(inbound);
            for (const json& ev : in) handle(ev);
            tick();
        }
    }
}

void Friends::Impl::tick() {
    const double now = nowSeconds();
    if (subDirty) {
        pool.subscribe(filters());
        subDirty = false;
    }
    const int connected = pool.connected();
    if (connected > 0 && firstConnected < 0) firstConnected = now;
    // Presence: a probe once the relays are up (friends answer at once), then a heartbeat.
    if (connected > 0 && !probed && now - firstConnected > 1.5) {
        broadcastPresence(true);
        probed = true, presenceDirty = false, lastHeartbeat = now;
    } else if (probed && (presenceDirty || now - lastHeartbeat > kHeartbeat)) {
        broadcastPresence(false);
        presenceDirty = false, lastHeartbeat = now;
    }
    for (auto& [hex, p] : peers)
        if (p.presence != Presence::Offline && now - p.heard > kOfflineAfter) p.presence = Presence::Offline, p.joinable = false;
    // Connection setup.
    for (auto it = sessions.begin(); it != sessions.end();) {
        Session& s = *it->second;
        auto pit = peers.find(s.peerHex);
        Tunnel& t = *s.tunnel;
        if (pit == peers.end()) {
            it = sessions.erase(it);
            continue;
        }
        Peer& p = pit->second;
        if (!s.descSent && (t.gathered() || now - s.created > 3.0) && connected > 0) {
            std::string sdp = t.localDescription();
            if (s.host && !upnp.candidate().empty()) sdp = withCandidate(sdp, upnp.candidate()), s.upnpSent = true;
            json m{{"sid", s.sid}, {"nonce", toHex(s.nonce)}, {"sdp", sdp}};
            m["t"] = s.host ? "ans" : "join";
            if (!s.host) m["name"] = cfg.name, s.ask = m;
            sendTo(p, m, false, true);
            s.descSent = true, s.lastCand = s.lastAsk = now, s.doneSent = t.gathered(), s.asks = 1;
            LOG("friends: %s %s (%zu bytes of candidates)", s.host ? "answered" : "asked to join", p.name.c_str(), sdp.size());
        } else if (!s.host && s.descSent && !s.answered && s.asks < 4 && now - s.lastAsk > 4.0) {
            // No answer yet: a relay may have dropped it (the host ignores repeats of the same session).
            sendTo(p, s.ask, false, true);
            s.lastAsk = now, ++s.asks;
        } else if (s.descSent && now - s.lastCand > 0.5) {
            s.lastCand = now;
            json cands = json::array();
            for (std::string& c : t.takeNewCandidates()) cands.push_back(std::move(c));
            if (s.host && !s.upnpSent && !upnp.candidate().empty()) cands.push_back(upnp.candidate()), s.upnpSent = true;
            const bool done = !s.doneSent && t.gathered();
            if (!cands.empty() || done) {
                sendTo(p, json{{"t", "cand"}, {"sid", s.sid}, {"c", cands}, {"done", t.gathered()}}, false, true);
                s.doneSent = s.doneSent || done;
            }
        }
        if (s.host) {
            const Tunnel::State st = t.state();
            if (t.streams() > 0) s.lastActive = now;
            const bool over = st == Tunnel::State::Failed || st == Tunnel::State::Closed ||
                              (st != Tunnel::State::Connected && now - s.created > 60) ||
                              (st == Tunnel::State::Connected && now - std::max(s.lastActive, s.created) > 120);
            if (over) {
                LOG("friends: %s's connection ended (%s)", p.name.c_str(), st == Tunnel::State::Failed ? t.failure().c_str() : "idle");
                it = sessions.erase(it);
                continue;
            }
        }
        ++it;
    }
    if (dirty) save();
}

void Friends::Impl::handle(const json& ev) {
    if (!ev.is_object()) return;
    const int kind = ev.value("kind", 0);
    if (kind != kEphemeralKind && kind != kDurableKind) return;
    const bool durable = kind == kDurableKind;
    std::string tag;
    if (ev.contains("tags") && ev["tags"].is_array())
        for (const json& t : ev["tags"])
            if (t.is_array() && t.size() >= 2 && t[0] == "p" && t[1].is_string()) tag = t[1].get<std::string>();
    const std::string content = ev.value("content", std::string());
    if (tag.empty() || content.empty()) return;
    if (durable) {
        const std::string eid = ev.value("id", std::string());
        if (eid.empty() || seenDurableSet.count(eid)) return;
        seenDurable.push_back(eid);
        seenDurableSet.insert(eid);
        while (seenDurable.size() > 512) seenDurableSet.erase(seenDurable.front()), seenDurable.pop_front();
        lastDurable = std::max(lastDurable, ev.value("created_at", i64(0)));
        dirty = true;
    }
    if (tag == inboxTag) {
        Key32 author, conv;
        if (!fromHex(ev.value("pubkey", std::string()), author) || !nip44ConversationKey(id.secret, author, conv)) return;
        const auto text = nip44Decrypt(conv, content);
        if (!text) return;
        const json m = json::parse(*text, nullptr, false);
        if (m.is_object() && m.value("t", std::string()) == "req") handleRequest(m);
        return;
    }
    for (auto& [hex, p] : peers) {
        if (p.tagToMe != tag) continue;
        const auto text = nip44Decrypt(p.conv, content);
        if (!text) return;
        const json m = json::parse(*text, nullptr, false);
        if (!m.is_object()) return;
        const i64 ts = m.value("ts", i64(0));
        if (!durable && std::abs(unixNow() - ts) > kMaxSkew) return;  // a replay, or a clock far off
        handleMessage(p, m, durable);
        return;
    }
}

void Friends::Impl::handleRequest(const json& m) {
    Key32 from;
    Bytes sig;
    const std::string fromHex_ = m.value("from", std::string()), name = m.value("name", std::string());
    const i64 ts = m.value("ts", i64(0));
    if (!fromHex(fromHex_, from) || !fromHex(m.value("sig", std::string()), sig) || sig.size() != 64 || !validName(name)) return;
    const Key32 h = sha256("mcpp-req-v1\n" + myHex + "\n" + fromHex_ + "\n" + name + "\n" + std::to_string(ts));
    if (!schnorrVerify(from, h.data(), h.size(), sig.data()) || from == id.pub) return;
    if (ts > unixNow() + 86400) return;
    if (auto d = declined.find(fromHex_); d != declined.end() && d->second >= ts) return;
    auto it = peers.find(fromHex_);
    if (it != peers.end()) {
        Peer& p = it->second;
        p.name = name, dirty = true;
        if (p.rel == Peer::Rel::Outgoing) {  // we asked each other: friends
            p.rel = Peer::Rel::Friend, p.since = unixNow();
            sendTo(p, json{{"t", "acc"}, {"name", cfg.name}}, true);
            event(FriendEvent::Type::Accepted, p);
            sendTo(p, presenceMessage(true), false);
            LOG("friends: %s and we asked each other: now friends", name.c_str());
        } else if (p.rel == Peer::Rel::Friend && ts > p.since) {  // they lost their list: confirm again
            sendTo(p, json{{"t", "acc"}, {"name", cfg.name}}, true);
        } else if (p.rel == Peer::Rel::Incoming) {
            p.requestTime = ts;
        }
        return;
    }
    Peer& p = addPeer(from, Peer::Rel::Incoming);
    p.name = name, p.requestTime = ts, p.since = unixNow();
    event(FriendEvent::Type::Request, p);
    LOG("friends: friend request from %s", name.c_str());
}

void Friends::Impl::handleMessage(Peer& p, const json& m, bool durable) {
    const std::string t = m.value("t", std::string());
    const i64 ts = m.value("ts", i64(0));
    if (durable) {
        if (t == "acc" && p.rel == Peer::Rel::Outgoing) {
            const std::string name = m.value("name", std::string());
            if (validName(name)) p.name = name;
            p.rel = Peer::Rel::Friend, p.since = unixNow(), dirty = true;
            event(FriendEvent::Type::Accepted, p);
            sendTo(p, presenceMessage(true), false);
            LOG("friends: %s accepted our friend request", p.name.c_str());
        } else if (t == "dec" && p.rel == Peer::Rel::Outgoing && ts >= p.since) {
            LOG("friends: %s declined our friend request", p.name.c_str());
            removePeer(p.hex);
        } else if (t == "rem" && p.rel != Peer::Rel::Incoming && ts >= p.since) {
            LOG("friends: %s removed us", p.name.c_str());
            removePeer(p.hex);
        }
        return;
    }
    if (p.rel != Peer::Rel::Friend) return;
    const double now = nowSeconds();
    if (t == "pres") {
        const std::string name = m.value("name", std::string());
        if (validName(name) && name != p.name) p.name = name, dirty = true;
        const bool wasOffline = p.presence == Presence::Offline;
        const std::string st = m.value("st", std::string());
        p.presence = st == "world" ? Presence::InWorld : st == "mp" ? Presence::Multiplayer : Presence::Online;
        p.world = m.value("world", std::string());
        p.joinable = m.value("join", false);
        p.heard = now;
        if (unixNow() - p.lastSeen > 60) dirty = true;
        p.lastSeen = unixNow();
        if (wasOffline && now - startedAt > kQuietStart) event(FriendEvent::Type::Online, p);
        if (m.value("want", false) && now - p.lastReply > 5) {
            p.lastReply = now;
            sendTo(p, presenceMessage(false), false);
        }
    } else if (t == "bye") {
        // Offline in a few seconds unless they come back: switching between the title screen and a world is a new
        // process that says hello again right away.
        p.heard = std::min(p.heard, now - kOfflineAfter + 12);
    } else if (t == "inv") {
        p.invitedAt = now;
        p.heard = now;
        if (p.presence == Presence::Offline) p.presence = Presence::InWorld;
        p.world = m.value("world", p.world);
        event(FriendEvent::Type::Invited, p, p.world);
        LOG("friends: %s invited us to \"%s\"", p.name.c_str(), p.world.c_str());
    } else if (t == "join") {
        handleJoin(p, m);
    } else if (t == "ans" || t == "cand") {
        auto it = sessions.find(m.value("sid", std::string()));
        if (it == sessions.end() || it->second->peerHex != p.hex) return;
        Session& s = *it->second;
        if (t == "ans") {
            if (s.host || s.answered) return;
            s.answered = true;
            if (m.contains("err")) {
                s.error = m.value("err", std::string("closed"));
                return;
            }
            Key32 nonce;
            if (!fromHex(m.value("nonce", std::string()), nonce)) return;
            s.peerNonce = nonce;
            s.tunnel->setKeys(p.shared, s.nonce, nonce);
            s.tunnel->setRemoteDescription(m.value("sdp", std::string()));
        } else {
            if (m.contains("c") && m["c"].is_array())
                for (const json& c : m["c"])
                    if (c.is_string()) s.tunnel->addRemoteCandidate(c.get<std::string>());
            if (m.value("done", false)) s.tunnel->setRemoteGatheringDone();
        }
    }
}

void Friends::Impl::handleJoin(Peer& p, const json& m) {
    const std::string sid = m.value("sid", std::string()), name = m.value("name", std::string());
    Key32 nonce;
    if (sid.empty() || sid.size() > 64 || !fromHex(m.value("nonce", std::string()), nonce) || sessions.count(sid)) return;
    if (std::find(answeredSids.begin(), answeredSids.end(), sid) != answeredSids.end()) return;  // a repeat
    answeredSids.push_back(sid);
    if (answeredSids.size() > 64) answeredSids.erase(answeredSids.begin());
    const bool allowed = hostPort != 0 && (openToFriends || p.invitedByMe);
    if (!allowed || !validName(name) || name == cfg.name) {
        LOG("friends: %s asked to join, refused (%s)", p.name.c_str(), !allowed ? "the world is not open to them" : "bad name");
        sendTo(p, json{{"t", "ans"}, {"sid", sid}, {"err", !allowed ? "closed" : "name"}}, false, true);
        return;
    }
    int hosted = 0;
    for (auto& [k, s] : sessions) hosted += s->host ? 1 : 0;
    if (hosted >= 8) return;
    auto s = std::make_shared<Session>();
    s->sid = sid, s->peerHex = p.hex, s->name = name, s->host = true, s->created = nowSeconds();
    s->nonce = randomArray<32>(), s->peerNonce = nonce;
    Tunnel::Config tc;
    tc.role = Tunnel::Role::Host, tc.muxPort = muxPort, tc.stunHost = stunHost, tc.stunPort = stunPort;
    tc.forwardPort = hostPort, tc.expectedName = name, tc.label = name;
    s->tunnel = std::make_unique<Tunnel>(tc);
    s->tunnel->setKeys(p.shared, nonce, s->nonce);
    if (!s->tunnel->start() || !s->tunnel->setRemoteDescription(m.value("sdp", std::string()))) {
        sendTo(p, json{{"t", "ans"}, {"sid", sid}, {"err", "failed"}}, false, true);
        return;
    }
    sessions[sid] = s;
    LOG("friends: %s (%s) is connecting to our world", p.name.c_str(), name.c_str());
}

// --- public API ---------------------------------------------------------------------------------------------------

FriendsSnapshot Friends::snapshot() const {
    const Impl& m = *impl_;
    FriendsSnapshot s;
    std::lock_guard lock(m.mutex);
    s.started = m.running;
    if (!m.running) return s;
    s.myCode = codeFor(m.id.pub);
    s.myName = m.cfg.name;
    s.relaysConnected = m.pool.connected(), s.relaysTotal = m.pool.total();
    s.hostPort = m.hostPort, s.openToFriends = m.openToFriends;
    const double now = nowSeconds();
    for (const auto& [hex, p] : m.peers) {
        if (p.rel == Peer::Rel::Friend) {
            FriendInfo f;
            f.pub = p.pub, f.code = codeFor(p.pub), f.name = p.name, f.presence = p.presence, f.world = p.world;
            f.joinable = p.joinable && p.presence == Presence::InWorld;
            f.invitedMe = now - p.invitedAt < kInviteValid && p.presence != Presence::Offline;
            f.invitedByMe = p.invitedByMe, f.lastSeen = p.lastSeen;
            s.friends.push_back(std::move(f));
        } else {
            RequestInfo r;
            r.pub = p.pub, r.code = codeFor(p.pub), r.name = p.name, r.time = p.requestTime;
            (p.rel == Peer::Rel::Incoming ? s.incoming : s.outgoing).push_back(std::move(r));
        }
    }
    for (const auto& [sid, sess] : m.sessions) {
        const bool live = sess->tunnel->state() == Tunnel::State::Connected;
        if (sess->host && live && sess->tunnel->streams() > 0) ++s.guests;
        for (FriendInfo& f : s.friends) {
            if (!live || toHex(f.pub) != sess->peerHex) continue;
            if (sess->host) f.ourGuest = f.ourGuest || sess->tunnel->streams() > 0;
            else f.hosting = true;
        }
    }
    // Worlds we can join first, then whoever is online, then the rest; by name within each.
    auto rank = [](const FriendInfo& f) { return f.presence == Presence::Offline ? 2 : f.joinable || f.invitedMe ? 0 : 1; };
    std::sort(s.friends.begin(), s.friends.end(), [&](const FriendInfo& a, const FriendInfo& b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return s;
}

int Friends::pendingRequests() const {
    std::lock_guard lock(impl_->mutex);
    int n = 0;
    for (const auto& [hex, p] : impl_->peers) n += p.rel == Peer::Rel::Incoming;
    return n;
}

std::vector<FriendEvent> Friends::takeEvents() {
    std::lock_guard lock(impl_->mutex);
    std::vector<FriendEvent> e;
    e.swap(impl_->events);
    return e;
}

Friends::AddResult Friends::add(const std::string& code) {
    Impl& m = *impl_;
    Key32 pub;
    if (!parseCode(code, pub)) return AddResult::Invalid;
    std::lock_guard lock(m.mutex);
    if (!m.running) return AddResult::NotStarted;
    if (pub == m.id.pub) return AddResult::Self;
    const std::string hex = toHex(pub);
    auto it = m.peers.find(hex);
    if (it != m.peers.end() && it->second.rel == Peer::Rel::Friend) return AddResult::AlreadyFriends;
    if (it != m.peers.end() && it->second.rel == Peer::Rel::Incoming) {
        Peer& p = it->second;
        p.rel = Peer::Rel::Friend, p.since = unixNow(), m.dirty = m.subDirty = true;
        m.sendTo(p, json{{"t", "acc"}, {"name", m.cfg.name}}, true);
        m.sendTo(p, m.presenceMessage(true), false);
        return AddResult::Accepted;
    }
    Peer& p = it != m.peers.end() ? it->second : m.addPeer(pub, Peer::Rel::Outgoing);
    p.requestTime = unixNow(), p.since = unixNow();
    m.declined.erase(hex);
    m.sendRequest(p);
    m.dirty = true;
    LOG("friends: friend request sent");
    m.wake.notify_one();
    return AddResult::Sent;
}

void Friends::accept(const Key32& pub) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    auto it = m.peers.find(toHex(pub));
    if (it == m.peers.end() || it->second.rel != Peer::Rel::Incoming || !m.running) return;
    Peer& p = it->second;
    p.rel = Peer::Rel::Friend, p.since = unixNow(), m.dirty = m.subDirty = true;
    m.sendTo(p, json{{"t", "acc"}, {"name", m.cfg.name}}, true);
    m.sendTo(p, m.presenceMessage(true), false);
    LOG("friends: accepted %s", p.name.c_str());
    m.wake.notify_one();
}

void Friends::decline(const Key32& pub) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    auto it = m.peers.find(toHex(pub));
    if (it == m.peers.end() || it->second.rel != Peer::Rel::Incoming) return;
    m.declined[it->first] = unixNow();
    if (m.running) m.sendTo(it->second, json{{"t", "dec"}}, true);
    m.removePeer(it->first);
}

void Friends::cancel(const Key32& pub) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    auto it = m.peers.find(toHex(pub));
    if (it == m.peers.end() || it->second.rel != Peer::Rel::Outgoing) return;
    if (m.running) m.sendTo(it->second, json{{"t", "rem"}}, true);
    m.removePeer(it->first);
}

void Friends::remove(const Key32& pub) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    auto it = m.peers.find(toHex(pub));
    if (it == m.peers.end()) return;
    if (m.running) m.sendTo(it->second, json{{"t", "rem"}}, true);
    m.declined[it->first] = unixNow();  // their old request must not come back
    m.removePeer(it->first);
}

void Friends::setActivity(Presence presence, const std::string& world) {
    std::lock_guard lock(impl_->mutex);
    impl_->activity = presence, impl_->world = world, impl_->presenceDirty = true;
}

void Friends::setHost(u16 port, bool openToFriends) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    if (m.hostPort == port && m.openToFriends == openToFriends) return;
    m.hostPort = port, m.openToFriends = openToFriends, m.presenceDirty = true;
    if (port && m.running) m.upnp.start(m.muxPort);
    if (!port) {
        m.upnp.stop();
        for (auto& [hex, p] : m.peers) p.invitedByMe = false;
    }
}

bool Friends::invite(const Key32& pub) {
    Impl& m = *impl_;
    std::lock_guard lock(m.mutex);
    auto it = m.peers.find(toHex(pub));
    if (!m.running || it == m.peers.end() || it->second.rel != Peer::Rel::Friend || !m.hostPort) return false;
    it->second.invitedByMe = true;
    m.sendTo(it->second, json{{"t", "inv"}, {"world", m.world}}, false);
    LOG("friends: invited %s", it->second.name.c_str());
    return true;
}

u16 Friends::join(const Key32& pub, double timeout, std::string& error) {
    Impl& m = *impl_;
    std::shared_ptr<Session> s;
    std::string friendName;
    {
        std::lock_guard lock(m.mutex);
        auto it = m.peers.find(toHex(pub));
        if (!m.running || it == m.peers.end() || it->second.rel != Peer::Rel::Friend) {
            error = "That player is not on your friends list.";
            return 0;
        }
        friendName = it->second.name;
        s = std::make_shared<Session>();
        const auto sidBytes = randomArray<8>();
        s->sid = toHex(sidBytes.data(), sidBytes.size());
        s->peerHex = it->first, s->host = false, s->created = nowSeconds(), s->nonce = randomArray<32>();
        Tunnel::Config tc;
        tc.role = Tunnel::Role::Joiner, tc.muxPort = m.muxPort, tc.stunHost = m.stunHost, tc.stunPort = m.stunPort;
        tc.label = friendName;
        s->tunnel = std::make_unique<Tunnel>(tc);
        if (!s->tunnel->start()) {
            error = s->tunnel->failure();
            return 0;
        }
        m.sessions[s->sid] = s;
    }
    LOG("friends: connecting to %s's world...", friendName.c_str());
    // The request goes out once the relays are up (a fresh start connects in about a second).
    for (double waitStart = nowSeconds(); nowSeconds() - waitStart < 10;) {
        {
            std::lock_guard lock(m.mutex);
            if (m.firstConnected >= 0 && (m.pool.connected() == m.pool.total() || nowSeconds() - m.firstConnected > 2.5)) break;
        }
        Sleep(50);
    }
    const double t0 = nowSeconds();
    std::string why;
    for (;;) {
        Sleep(50);
        const double waited = nowSeconds() - t0;
        std::lock_guard lock(m.mutex);
        const Tunnel::State st = s->tunnel->state();
        if (!s->error.empty()) {
            why = s->error == "closed" ? friendName + "'s world is not open to friends right now."
                                       : friendName + " could not take the connection.";
            break;
        }
        if (st == Tunnel::State::Connected) {
            LOG("friends: connected to %s's world in %.1f s via %s", friendName.c_str(), waited, s->tunnel->route().c_str());
            return s->tunnel->listenPort();
        }
        if (st == Tunnel::State::Failed || waited > timeout || (!s->answered && waited > 15)) {
            const bool noAnswer = !s->answered;
            why = noAnswer ? friendName + " did not answer. Are they still in their world?"
                           : "Couldn't connect directly - your or your friend's network blocks peer-to-peer connections.";
            LOG("friends: connecting to %s failed: %s", friendName.c_str(),
                noAnswer ? "no answer" : st == Tunnel::State::Failed ? s->tunnel->failure().c_str() : "timed out");
            break;
        }
    }
    {
        std::lock_guard lock(m.mutex);
        m.sessions.erase(s->sid);
    }
    error = why;
    return 0;
}

}  // namespace social
