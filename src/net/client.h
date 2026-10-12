#pragma once
// Vanilla-protocol client: handshake, login (offline mode), configuration and the play phase on
// its own thread. Packets are decoded there into events the main thread drains once per frame;
// keep-alives, pings and chunk batch acknowledgements are answered on the network thread.
#include "core/common.h"
#include "net/connection.h"
#include "net/fabric_sync.h"
#include "net/protocol.h"
#include "world/world.h"

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

class BlockRegistry;
class GameReports;

namespace net {

struct Dimension {
    std::string name;  // e.g. "minecraft:overworld"
    int minY = -64, height = 384;
    bool fastLava = false;  // EnvironmentAttributes.FAST_LAVA (the nether): lava currents push harder
    bool waterEvaporates = false;  // EnvironmentAttributes.WATER_EVAPORATES (the nether): buckets cannot place water
};

// login (respawn = false) or respawn: the world must be cleared on respawn.
struct JoinEvent {
    bool respawn = false;
    int entityId = 0, gameMode = 0, viewDistance = 0;
    Dimension dimension;
    double receivedAt = 0;  // time the login/respawn packet was decoded, for destination readiness timing
};
struct ChunkEvent {
    int cx = 0, cz = 0, firstSy = 0;  // sections[i] is section y firstSy + i (light included)
    std::vector<World::PackedSection> sections;
};
struct ForgetEvent {
    int cx = 0, cz = 0;
};
struct BlockChange {
    int x, y, z;
    u16 id;
};
struct BlocksEvent {
    std::vector<BlockChange> changes;
};
struct LightEvent {
    int cx = 0, cz = 0;
    std::vector<LightSection> sections;
};
struct TeleportEvent {
    int id = 0;
    double pos[3] = {}, delta[3] = {};
    float yaw = 0, pitch = 0;  // degrees
    u32 relative = 0;          // bit i: field i is relative (x y z yaw pitch dx dy dz rotate-delta)
};
struct AckEvent {
    int sequence = 0;
};
struct AbilitiesEvent {
    bool flying = false, mayFly = false, instabuild = false;
    float flyingSpeed = 0.05f, walkingSpeed = 0.1f;
};
struct GameModeEvent {
    int mode = 0;  // 0 survival, 1 creative, 2 adventure, 3 spectator
};
struct HeldSlotEvent {
    int slot = 0;
};
// chunks_biomes: new biomes of loaded columns (sections[i] is section y firstSy + i; lowest quart layer each).
struct BiomesEvent {
    struct Column {
        int cx = 0, cz = 0, firstSy = 0;
        std::vector<std::array<u8, 16>> sections;
    };
    std::vector<Column> columns;
};
struct DisconnectEvent {
    std::string reason;
};
// start_configuration: the server takes the player out of the level (reconfiguration); a new login follows.
struct ReconfigureEvent {};
using Event = std::variant<JoinEvent, ChunkEvent, ForgetEvent, BlocksEvent, LightEvent, TeleportEvent, AckEvent,
                           AbilitiesEvent, GameModeEvent, HeldSlotEvent, DisconnectEvent, BiomesEvent, ReconfigureEvent>;

// Requests of the configuration / play phases that the main thread answers (Client::pollCommon): server resource
// packs, the code of conduct, chat reset, transfers, reconfiguration.
struct PackPushEvent {  // ClientboundResourcePackPushPacket
    std::string id;     // 16 bytes (UUID)
    std::string url, hash;
    bool required = false;
    std::string prompt;  // network NBT component, "" when none
};
struct PackPopEvent {  // ClientboundResourcePackPopPacket
    std::string id;    // 16 bytes, "" = every pack
};
struct CodeOfConductEvent {
    std::string text;
};
struct ResetChatEvent {};
struct TransferEvent {
    std::string host;
    int port = 25565;
};
struct ConfigurationEvent {  // the client entered (reconfiguration) or left the configuration phase
    bool started = true;
};
using CommonEvent = std::variant<PackPushEvent, PackPopEvent, CodeOfConductEvent, ResetChatEvent, TransferEvent,
                                 ConfigurationEvent>;

struct ClientStats {
    double connectMs = -1, joinMs = -1, firstChunkMs = -1;  // since start()
    u64 chunks = 0;
    double decodeMs = 0, maxDecodeMs = 0;  // chunk packet decode time on the network thread
};

class Client {
public:
    struct Config {
        std::string host = "localhost";
        u16 port = 25565;
        std::string name = "Player";
        fs::path jar;  // client jar: protocol version and chat translations
        fs::path serverRegistries;  // optional: a local Fabric server's block table (net/fabric_sync.h)
        bool transfer = false;      // handshake with the transfer intent (ClientboundTransferPacket)
        std::map<std::string, std::string> cookies;  // the previous server's cookies (TransferState)
    };

    // ClientInformation (Options.buildPlayerInformation): sent in the configuration phase, again when it changes.
    struct Information {
        std::string language = "en_us";
        int viewDistance = 12;
        int chatVisibility = 0;  // full
        bool chatColors = true;
        u8 modelParts = 0x7F;    // PlayerModelPart masks (cape 1 .. hat 64)
        int mainHand = 1;        // HumanoidArm id: 0 left, 1 right
        bool textFiltering = false, allowListing = true;
        int particles = 0;  // all
    };

    Client(const GameReports& reports, BlockRegistry& registry);
    ~Client();

    bool start(const Config& config);
    void stop();
    void poll(std::vector<Event>& out);  // appends pending events
    bool alive() const { return alive_; }
    ClientStats stats() const;

    // Play phase (main thread); ignored in other phases.
    void sendAcceptTeleport(int id, const double pos[3], float yaw, float pitch);
    void sendMove(const double pos[3], float yaw, float pitch, bool onGround, bool horizontalCollision);
    void sendTickEnd();
    void sendPlayerLoaded();
    void sendPlayerAction(int action, int x, int y, int z, int face, int sequence);  // 0 start, 3 stop destroy
    // inside: BlockHitResult.isInside; the world border flag is never set by the client (hitBorder is unused).
    void sendUseItemOn(int x, int y, int z, int face, const float cursor[3], int sequence, int hand = 0, bool inside = false);
    void sendCarriedItem(int slot);
    void sendCreativeSlot(int slot, int itemId, int count);  // inventory menu slot; itemId < 0 = empty
    // Any thread; before start() it only sets what the login sends. Options.broadcastOptions in the play phase.
    void setInformation(const Information& info);

    // Extensions (entities, UI, ...): play-phase packets by report name ("add_entity", "container_set_content").
    // A handler runs on the network thread, takes precedence over the built-in handling and must queue
    // whatever it decodes for its own main-thread consumer. Register before start().
    using PacketHandler = std::function<void(Reader& r)>;
    // Several handlers may take the same packet; each reads its own copy of it.
    void onPlay(const std::string& packet, PacketHandler handler) { playHandlers_[packet].push_back(std::move(handler)); }
    // Observers: see a copy of the packet before it is handled as usual (several per packet allowed).
    void onPlayPeek(const std::string& packet, PacketHandler handler) { playPeeks_[packet].push_back(std::move(handler)); }
    bool sendPlay(const char* packet, const Writer& body) { return send(Play, packet, body); }  // any thread
    const Dimension& dimension() const { return dimension_; }  // network thread / after JoinEvent
    // The synced dynamic registries ("minecraft:enchantment", "minecraft:cat_variant", "minecraft:chat_type"...):
    // entries in network id order with their network NBT ("" when the server sent none). Complete once inPlay();
    // registryVersion() changes whenever a configuration phase (re)defined them. Any thread.
    struct RegistryEntry {
        std::string id;   // "minecraft:sharpness"
        std::string nbt;  // network NBT (nameless root)
    };
    std::vector<RegistryEntry> registryEntries(const std::string& registry) const;
    std::string registryId(const std::string& registry, int networkId) const;  // "" when out of range
    int registryVersion() const { return registryVersion_; }
    // update_tags: network ids of a tag's entries ("minecraft:item", "minecraft:swords"; no '#').
    std::vector<int> tagEntries(const std::string& registry, const std::string& tag) const;
    std::vector<std::string> enabledFeatures() const;  // update_enabled_features ("minecraft:vanilla")
    bool inPlay() const { return phase_ == Play; }

    // Configuration / common requests for the main thread (any thread may poll; normally the UI each frame).
    void pollCommon(std::vector<CommonEvent>& out);
    bool hasCommon() const;
    // ServerboundResourcePackPacket in the current phase (Action ordinal: 0 loaded, 1 declined, 2 failed download,
    // 3 accepted, 4 downloaded, 5 invalid URL, 6 failed reload, 7 discarded).
    void sendPackStatus(const std::string& id, int action);
    void acceptCodeOfConduct();
    // Closes the connection on the client's side (Connection.disconnect): the reason shows as a DisconnectEvent.
    void disconnect(const std::string& reason);
    std::map<std::string, std::string> cookies() const;  // store_cookie (for a transfer)
    std::map<std::string, std::string> reportDetails() const;  // custom_report_details (crash reports)
    std::string serverBrand() const;                            // minecraft:brand ("vanilla")
    // The server's chunk cache view (set_chunk_cache_center / _radius, the login's distances) and simulation
    // distance (set_simulation_distance), in chunks; any thread.
    int viewDistance() const { return viewDistance_; }
    int simulationDistance() const { return simulationDistance_; }
    int viewCenterX() const { return viewCenter_[0]; }
    int viewCenterZ() const { return viewCenter_[1]; }

private:
    enum Phase { Login, Configuration, Play, PhaseCount };

    void run();
    bool send(Phase phase, const char* name, const Writer& body);  // only while in `phase`
    bool sendRaw(Phase phase, const char* name, const Writer& body);  // caller holds sendMutex_
    void switchPhase(Phase from, const char* ack, Phase to);           // sends `ack`, then switches
    bool handleLogin(const std::string& name, Reader& r, bool& handled);
    bool handleConfiguration(const std::string& name, Reader& r, bool& handled);
    bool handlePlay(const std::string& name, Reader& r, bool& handled);
    Writer cookieResponse(Reader& r);
    bool handleCommon(Phase phase, const std::string& name, Reader& r, bool& handled);
    void push(Event e);
    void pushCommon(CommonEvent e);
    void chat(const std::string& line);
    // NET-17: packets nothing handled, and readers that left bytes unread or overran, logged once per packet type.
    void diagnose(Phase phase, const std::string& name, bool handled, size_t size, size_t left, bool overran);
    double sinceStart() const { return (nowSeconds() - startTime_) * 1000.0; }

    const GameReports& reports_;
    BlockRegistry& registry_;
    Config config_;
    Information info_;  // guarded by sendMutex_
    int protocol_ = 0;
    std::vector<std::string> clientbound_[PhaseCount];               // packet id -> name
    std::unordered_map<std::string, int> serverbound_[PhaseCount];  // name -> packet id
    std::unordered_map<std::string, std::string> lang_;
    BlockIds blockIds_;
    FabricSync fabric_;  // modded Fabric servers: registry sync handshake, server block table

    Connection connection_;
    std::thread thread_;
    std::atomic<bool> alive_{false}, quit_{false};
    std::atomic<int> phase_{Login};
    std::atomic<int> viewDistance_{0}, simulationDistance_{0}, viewCenter_[2] = {0, 0};
    mutable std::mutex mutex_;  // events_, stats_
    std::mutex sendMutex_;  // phase check + send are atomic w.r.t. phase changes
    std::vector<Event> events_;
    std::unordered_map<std::string, std::vector<PacketHandler>> playHandlers_;
    std::unordered_map<std::string, std::vector<PacketHandler>> playPeeks_;
    ClientStats stats_;
    double startTime_ = 0;

    // Network thread state.
    std::vector<Dimension> dimensionTypes_;
    int biomeBits_ = 7;
    std::vector<u8> biomeIds_;  // the server's biome registry order -> World biome ids
    mutable std::mutex registryMutex_;  // registries_, tags_, features_, cookies_, reportDetails_
    std::unordered_map<std::string, std::vector<RegistryEntry>> registries_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<int>>> tags_;
    std::vector<std::string> features_;
    std::map<std::string, std::string> cookies_, reportDetails_;
    std::string serverBrand_;
    std::atomic<int> registryVersion_{0};
    std::vector<CommonEvent> common_;  // guarded by mutex_
    std::unordered_set<std::string> diagnosed_;
    size_t readLeft_ = 0;       // this packet: the fewest bytes any of its readers left unread
    bool readOverran_ = false;  // ...and whether one of them read past its end
    void noteRead(const Reader& r) {
        if (!r.ok) readOverran_ = true;
        else readLeft_ = std::min(readLeft_, r.left());
    }
    Dimension dimension_;
};

}  // namespace net
