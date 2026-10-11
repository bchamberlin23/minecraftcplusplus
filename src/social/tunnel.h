#pragma once
// A peer-to-peer tunnel between two friends for the game's TCP connection, without any server of ours:
//  - ICE (libjuice): each side gathers host, server reflexive (public STUN) and, on a host with UPnP, mapped
//    candidates; the friends system exchanges them over Nostr; connectivity checks punch through the NATs. Every
//    tunnel of a process shares one UDP port (libjuice's mux mode), so one UPnP mapping covers all of them.
//  - Every datagram is ChaCha20-Poly1305 sealed with keys from the two identities' ECDH secret and both sides'
//    session nonces (a 64-bit counter is the nonce; replays are dropped), so only that friend can talk to it.
//  - KCP (reliable, ordered, congestion controlled) carries each TCP connection as its own conversation.
//  - Joiner: a loopback TCP listener the game connects to. Host: each conversation is connected to the local server
//    (its Open to LAN port), after checking that the login name is the one the friend announced.
#include "core/common.h"
#include "social/crypto.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

typedef struct juice_agent juice_agent_t;

namespace social {

class Tunnel {
public:
    enum class Role { Joiner, Host };
    enum class State { Gathering, Connecting, Connected, Failed, Closed };
    struct Config {
        Role role = Role::Joiner;
        u16 muxPort = 0;                       // the process's shared UDP port (0: any)
        std::string stunHost = "stun.l.google.com";
        u16 stunPort = 19302;
        u16 forwardPort = 0;                   // host: the local server's port
        std::string expectedName;              // host: the joiner's login name must be this
        std::string label;                     // logs
    };
    explicit Tunnel(const Config& config);
    ~Tunnel();
    bool start();  // the agent and (joiner) the loopback listener; gathering starts

    // Signaling. The local description holds the candidates gathered so far (all of them once gathered()).
    bool gathered() const { return gathered_; }
    std::string localDescription();
    std::vector<std::string> takeNewCandidates();  // gathered after localDescription(), not in it
    bool setRemoteDescription(const std::string& sdp);
    void addRemoteCandidate(const std::string& candidate);
    void setRemoteGatheringDone();
    // Keys from the identities' shared secret and both nonces (joiner's first).
    void setKeys(const Key32& sharedSecret, const Key32& joinerNonce, const Key32& hostNonce);

    State state() const { return state_; }
    std::string failure() const;           // why it failed
    u16 listenPort() const { return listenPort_; }  // joiner
    std::string route() const;             // the selected candidate pair (logs)
    int streams() const { return streamCount_; }
    double idleSeconds() const;            // since the last datagram from the peer
    void close();

    // Encrypted datagram framing (exposed for the self-test).
    struct Cipher {
        Key32 sendKey{}, recvKey{};
        u64 sendCounter = 0, recvMax = 0, recvWindow = 0;
        bool ready = false, recvAny = false;
        Bytes seal(u8 type, const u8* data, size_t size);
        bool open(const u8* data, size_t size, u8& type, Bytes& out);  // false: forged, damaged or replayed
    };

private:
    struct Stream;
    void pump();
    void sendDatagram(u8 type, const u8* data, size_t size);
    Stream* newStream(u32 conv, uintptr_t socket);
    void handleDatagram(const Bytes& raw);
    void fail(const std::string& why);

    Config config_;
    juice_agent_t* agent_ = nullptr;
    std::atomic<State> state_{State::Gathering};
    std::atomic<bool> gathered_{false}, running_{false};
    mutable std::mutex mutex_;  // candidates, inbound queue, failure, cipher setup
    std::condition_variable wake_;
    std::vector<std::string> candidates_;
    std::string described_;  // the local description handed out
    std::deque<Bytes> inbound_;
    std::string failure_;
    Cipher cipher_;
    std::atomic<bool> keysSet_{false};
    std::atomic<double> lastRecv_{0};
    uintptr_t listen_ = ~uintptr_t(0);
    u16 listenPort_ = 0;
    std::thread pump_;
    // Pump thread only:
    std::map<u32, std::unique_ptr<Stream>> streamsByConv_;
    std::set<u32> closedConvs_;
    u32 nextConv_ = 1;
    double lastPing_ = 0;
    std::atomic<int> streamCount_{0};
};

// The handshake and login start a Minecraft client sends first: intent (1 status, 2 login, 3 transfer) and the
// login name. 1: complete, 0: need more bytes, -1: not a Minecraft connection.
int parseLoginStart(const u8* data, size_t size, int& intent, std::string& name);

}  // namespace social
