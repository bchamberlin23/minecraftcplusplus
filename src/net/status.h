#pragma once
// The multiplayer menu's network side, like the game's:
//  - server list pings (ServerStatusPinger): handshake with the status intent, status request (MOTD, players,
//    version, favicon) and a ping for the latency;
//  - LAN worlds: the announcements open singleplayer worlds multicast to 224.0.2.60:4445
//    ("[MOTD]<motd>[/MOTD][AD]<port>[/AD]", LanServerDetection / LanServerPinger);
//  - Open to LAN for the local server: those announcements, plus a port on all interfaces forwarded to the server
//    (which itself only listens on loopback).
#include "core/common.h"

#include <json/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace net {

struct ServerAddress {
    std::string host;
    u16 port = 25565;
    bool portGiven = false;
};
// "host", "host:port", "[ipv6]:port" (ServerAddress.parseString).
ServerAddress parseAddress(std::string_view text);
// The game's SRV lookup (_minecraft._tcp.<host>) when no port was given; the address itself otherwise.
ServerAddress resolveAddress(const ServerAddress& a);

struct ServerStatus {
    bool ok = false;
    std::string error;           // why not
    nlohmann::json description;  // MOTD: a text component (JSON)
    std::string versionName;
    bool hasVersion = false, hasPlayers = false;  // ServerStatus.version() / players() present
    int protocol = -1, online = -1, max = -1;
    std::vector<std::string> sample;  // player names from the status
    std::vector<bool> sampleAnonymous;  // MinecraftServer.ANONYMOUS_PLAYER_PROFILE (nil id, "Anonymous Player")
    bool connected = false;  // not ok: the connection opened but no status came (Java keeps pinging, "can't connect")
    bool legacy = false;     // ok: answered only the pre-1.7 ping (LegacyServerPinger: shown incompatible)
    std::vector<u8> favicon;          // PNG
    std::string faviconBase64;        // as sent (servers.dat keeps it this way)
    int pingMs = -1;
};
// Blocking (call from a worker thread). protocol: ours, sent in the handshake.
ServerStatus pingServer(const std::string& address, int protocol, double timeoutSeconds = 6.0);

// The installed game's protocol version and name ("26.3") from the client jar's version.json.
int gameProtocol(const fs::path& jar, std::string* versionName = nullptr);

class LanListener {
public:
    struct Entry {
        std::string motd, address;  // address: "ip:port"
        double lastSeen = 0;
    };
    ~LanListener();
    bool start();
    void stop();
    std::vector<Entry> entries() const;
    bool running() const { return running_; }

private:
    void run();
    uintptr_t socket_ = ~uintptr_t(0);
    std::thread thread_;
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

class LanHost {
public:
    ~LanHost();
    // Announces `motd` with `publicPort`; forward: listen on publicPort (all interfaces) and relay each connection
    // to 127.0.0.1:serverPort. error: why it could not open the port.
    bool start(const std::string& motd, u16 publicPort, u16 serverPort, bool forward, std::string& error);
    void stop();
    bool active() const { return active_; }
    u16 port() const { return publicPort_; }

private:
    void announce();
    void accept();
    void relay(uintptr_t client);
    std::string motd_;
    u16 publicPort_ = 0, serverPort_ = 0;
    std::atomic<bool> active_{false};
    uintptr_t listen_ = ~uintptr_t(0);
    std::thread announcer_, acceptor_;
    std::mutex mutex_;
    std::vector<uintptr_t> sockets_;  // open relay sockets (closed on stop)
    std::vector<std::thread> relays_;
};

// The port Open to LAN offers: a free one from the game's range (HttpUtil.getAvailablePort tries 25564 first).
u16 suggestLanPort();
bool lanPortAvailable(u16 port);

}  // namespace net
