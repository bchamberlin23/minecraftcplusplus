#pragma once
// Friends: a friends list with online status, invites and joining each other's singleplayer worlds across networks,
// with no server of our own (the design follows the Essential mod's: relationships, presence, invites, a hosted world
// reached through ICE; none of its code or services).
//
// Identity: a secp256k1 key pair made on first use (the secret key DPAPI-protected in the friends folder). A friend
// code is the public key in base58 with a version byte and a checksum; the display name is the Minecraft name.
//
// Signaling: public Nostr relays (several; everything is sent to all and deduplicated). Each message is NIP-44 v2
// encrypted and published by a throwaway key, tagged ("p") with a value only the two friends can compute
// (SHA-256 of their ECDH secret and the recipient), so relays and onlookers see neither who talks nor what is said:
//  - friend requests go to the recipient's inbox tag (SHA-256 of its key) and carry a Schnorr signature of the sender;
//  - durable messages (request, accept, decline, remove) are a regular event kind with a 7-day NIP-40 expiration, so
//    an offline friend gets them on its next start; presence, invites and connection setup are ephemeral kinds.
//  - Presence: a heartbeat every minute to each friend (online / in a world / joinable), a probe on start that
//    friends answer at once, a goodbye on exit; a friend is offline after 2.5 minutes of silence.
//
// Joining: the joiner sends its ICE description (and a session nonce) to the host, which answers only when the
// joiner is a friend and the world is open to friends or the friend was invited; social/tunnel.h does the rest.
// Hosting forwards to the Open to LAN port, so inviting a friend opens the world to LAN like the game would.
#include "core/common.h"
#include "social/crypto.h"

#include <memory>

namespace social {

enum class Presence { Offline, Online, InWorld, Multiplayer };

struct FriendInfo {
    Key32 pub{};
    std::string code, name;
    Presence presence = Presence::Offline;
    std::string world;        // InWorld: the world's name; Multiplayer: whose world they are in ("" a server)
    bool joinable = false;    // their world takes friends now
    bool invitedMe = false;   // they invited us (the last 10 minutes)
    bool invitedByMe = false; // we invited them to our world
    bool ourGuest = false;    // connected to our world right now
    bool hosting = false;     // we are connected to their world right now
    i64 lastSeen = 0;         // unix seconds of the last presence (0: never)
};

struct RequestInfo {
    Key32 pub{};
    std::string code, name;
    i64 time = 0;
};

struct FriendEvent {
    enum class Type { Online, Request, Accepted, Invited };
    Type type = Type::Online;
    Key32 pub{};
    std::string name, world;
};

struct FriendsSnapshot {
    bool started = false;
    std::string myCode, myName;
    int relaysConnected = 0, relaysTotal = 0;
    std::vector<FriendInfo> friends;  // online first, then by name
    std::vector<RequestInfo> incoming, outgoing;
    u16 hostPort = 0;                 // our world takes friends' connections
    bool openToFriends = false;
    int guests = 0;                   // friends connected to our world now
};

class Friends {
public:
    struct Config {
        fs::path dir;                     // identity and friends list
        std::string name;                 // our Minecraft name
        std::vector<std::string> relays;  // default: the built-in list
    };
    static Friends& get();          // the process's service
    static fs::path defaultDir();   // %MCPP_FRIENDS_DIR% or %LOCALAPPDATA%\minecraftCplusplus\friends
    static std::string codeFor(const Key32& pub);
    static bool parseCode(const std::string& code, Key32& out);  // spaces and dashes are ignored

    Friends();
    ~Friends();
    bool start(const Config& config);  // loads or makes the identity, connects to the relays
    void stop();                       // goodbye to friends, closes tunnels and the UPnP mapping
    bool started() const;

    FriendsSnapshot snapshot() const;
    int pendingRequests() const;  // incoming friend requests (cheap: for button labels)
    std::vector<FriendEvent> takeEvents();

    // The list. add: by friend code (accepts at once when they asked us first).
    enum class AddResult { Sent, Accepted, AlreadyFriends, Invalid, Self, NotStarted };
    AddResult add(const std::string& code);
    void accept(const Key32& pub);
    void decline(const Key32& pub);
    void cancel(const Key32& pub);  // our outgoing request
    void remove(const Key32& pub);

    // Presence: what we are doing (world: its name while in a world).
    void setActivity(Presence presence, const std::string& world);
    // Hosting: friends' connections are forwarded to this local port (0: not hosting). openToFriends: any friend may
    // join, else only invited ones.
    void setHost(u16 port, bool openToFriends);
    bool invite(const Key32& pub);  // also lets that friend in

    // Joining a friend's world (blocking, a worker or startup thread): the loopback port the game connects to, or 0
    // with a reason the player can read.
    u16 join(const Key32& pub, double timeoutSeconds, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --friends-selftest / --friends-node (social/friends_test.cpp). argv after the flag.
int runFriendsSelfTest();
int runFriendsNode(int argc, wchar_t** argv);

}  // namespace social
