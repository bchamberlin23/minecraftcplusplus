#include "net/client.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "assets/asset_source.h"
#include "assets/zip.h"
#include "world/nbt.h"

#include <windows.h>
#include <bcrypt.h>

#include <json/json.hpp>

namespace net {

namespace {
constexpr const char* kPhaseNames[] = {"login", "configuration", "play"};
constexpr float kChunksPerTick = 25.0f;  // asked for in chunk batch acks; main-thread application has a 4 ms budget

// Offline-mode player UUID: name-based (version 3) UUID of "OfflinePlayer:<name>", like the game.
void offlineUuid(const std::string& name, u8 out[16]) {
    const std::string s = "OfflinePlayer:" + name;
    BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, PUCHAR(s.data()), ULONG(s.size()), out, 16);
    out[6] = u8((out[6] & 0x0F) | 0x30);
    out[8] = u8((out[8] & 0x3F) | 0x80);
}

int protocolVersion(const ZipArchive& zip) {
    std::vector<u8> bytes;
    for (const auto& e : zip.entries())
        if (e.name == "version.json" && zip.read(e, bytes)) {
            const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            return j.is_object() ? j.value("protocol_version", 0) : 0;
        }
    return 0;
}

// Dimension type entry of the registry data: min_y and height of the build range.
Dimension parseDimensionType(const std::string& id, std::string_view nbt) {
    Dimension d;
    d.name = id;
    if (id != "minecraft:overworld" && id != "minecraft:overworld_caves") d.minY = 0, d.height = 256;
    if (nbt.size() < 2 || u8(nbt[0]) != TagCompound) return d;
    NbtReader r(reinterpret_cast<const u8*>(nbt.data()) + 1, nbt.size() - 1);
    u8 tag;
    std::string_view name;
    while (r.field(tag, name)) {
        if (tag == TagInt && name == "min_y") d.minY = r.i32v();
        else if (tag == TagInt && name == "height") d.height = r.i32v();
        else if (tag == TagCompound && name == "attributes") {  // environment attributes: gameplay/fast_lava
            u8 t;
            std::string_view n;
            while (r.field(t, n)) {
                if (t == TagByte && n == "minecraft:gameplay/fast_lava") d.fastLava = r.u8v() != 0;
                else if (t == TagByte && n == "minecraft:gameplay/water_evaporates") d.waterEvaporates = r.u8v() != 0;
                else r.skip(t);
            }
        } else r.skip(tag);
    }
    return d;
}
}  // namespace

Client::Client(const GameReports& reports, BlockRegistry& registry) : reports_(reports), registry_(registry) {}

Client::~Client() { stop(); }

bool Client::start(const Config& config) {
    config_ = config;
    startTime_ = nowSeconds();
    const nlohmann::json& packets = reports_.packets();
    for (int phase = 0; phase < PhaseCount; ++phase) {
        if (!packets.is_object() || !packets.contains(kPhaseNames[phase])) {
            LOG("error: the game's packet report (packets.json) is unavailable");
            return false;
        }
        const nlohmann::json& p = packets[kPhaseNames[phase]];
        const nlohmann::json clientbound = p.value("clientbound", nlohmann::json::object());
        const nlohmann::json serverbound = p.value("serverbound", nlohmann::json::object());
        for (const auto& item : clientbound.items()) {
            const int id = item.value().value("protocol_id", -1);
            if (id < 0) continue;
            if (size_t(id) >= clientbound_[phase].size()) clientbound_[phase].resize(size_t(id) + 1);
            clientbound_[phase][size_t(id)] = item.key().substr(item.key().find(':') + 1);
        }
        for (const auto& item : serverbound.items())
            serverbound_[phase][item.key().substr(item.key().find(':') + 1)] = item.value().value("protocol_id", -1);
    }
    ZipArchive zip;
    if (zip.open(config_.jar)) {
        protocol_ = protocolVersion(zip);
        AssetSource::client(config_.jar).readLang("en_us", lang_);  // with resource packs and mods' lang
    }
    if (!protocol_) {
        LOG("error: cannot read the protocol version from %s", config_.jar.string().c_str());
        return false;
    }
    blockIds_.init(reports_, registry_);
    if (!config_.serverRegistries.empty() && fabric_.loadDump(config_.serverRegistries)) {
        blockIds_.initTable(fabric_.stateTable(), registry_);
        LOG("net: block table from the Fabric server: %zu modded blocks", fabric_.moddedBlocks());
    }
    alive_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void Client::stop() {
    quit_ = true;
    connection_.close();
    if (thread_.joinable()) thread_.join();
}

void Client::poll(std::vector<Event>& out) {
    std::lock_guard lock(mutex_);
    for (Event& e : events_) out.push_back(std::move(e));
    events_.clear();
}

ClientStats Client::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void Client::push(Event e) {
    std::lock_guard lock(mutex_);
    events_.push_back(std::move(e));
}

void Client::chat(const std::string& line) { LOG("[chat] %s", line.c_str()); }

bool Client::sendRaw(Phase phase, const char* name, const Writer& body) {
    auto it = serverbound_[phase].find(name);
    if (it == serverbound_[phase].end() || it->second < 0) {
        LOG("net: no packet id for %s %s", kPhaseNames[phase], name);
        return false;
    }
    Writer w;
    w.varInt(it->second);
    w.buf.insert(w.buf.end(), body.buf.begin(), body.buf.end());
    return connection_.send(w.buf);
}

bool Client::send(Phase phase, const char* name, const Writer& body) {
    std::lock_guard lock(sendMutex_);
    return phase_ == phase && sendRaw(phase, name, body);
}

void Client::switchPhase(Phase from, const char* ack, Phase to) {
    std::lock_guard lock(sendMutex_);
    sendRaw(from, ack, Writer{});
    phase_ = to;
}

void Client::run() {
    std::string error;
    if (!connection_.open(config_.host, config_.port, error)) {
        push(DisconnectEvent{error});
        alive_ = false;
        return;
    }
    {
        std::lock_guard lock(mutex_);
        stats_.connectMs = sinceStart();
    }
    // Handshake (intent: login), then login start with the offline-mode profile.
    Writer hs;
    hs.varInt(0);  // the handshake phase's only packet
    hs.varInt(protocol_);
    hs.str(config_.host);
    hs.u16v(config_.port);
    hs.varInt(2);
    connection_.send(hs.buf);
    Writer hello;
    hello.str(config_.name);
    u8 uuid[16];
    offlineUuid(config_.name, uuid);
    hello.bytes(uuid, 16);
    send(Login, "hello", hello);

    std::string reason = "connection closed";
    std::vector<u8> packet;
    while (!quit_ && connection_.read(packet)) {
        Reader r(packet.data(), packet.size());
        const int id = r.varInt();
        const Phase phase = Phase(phase_.load());
        const std::vector<std::string>& names = clientbound_[phase];
        if (!r.ok || id < 0 || size_t(id) >= names.size()) continue;  // unknown: skip by length
        const std::string& name = names[size_t(id)];
        bool keepGoing, handled = false;
        if (phase == Login) {
            keepGoing = handleLogin(name, r);
        } else {
            keepGoing = handleCommon(phase, name, r, handled);
            if (keepGoing && !handled)
                keepGoing = phase == Configuration ? handleConfiguration(name, r) : handlePlay(name, r);
        }
        if (!keepGoing) {
            reason.clear();
            break;
        }
    }
    connection_.close();
    alive_ = false;
    if (!quit_ && !reason.empty()) push(DisconnectEvent{reason});
}

namespace {
// ClientInformation.write: language, view distance, chat visibility / colours, skin parts, main hand, text filtering,
// server listing, particles.
Writer informationPacket(const Client::Information& info) {
    Writer w;
    w.str(info.language);
    w.u8v(u8(info.viewDistance));
    w.varInt(info.chatVisibility);
    w.boolean(info.chatColors);
    w.u8v(info.modelParts);
    w.varInt(info.mainHand);
    w.boolean(info.textFiltering);
    w.boolean(info.allowListing);
    w.varInt(info.particles);
    return w;
}
}  // namespace

bool Client::handleLogin(const std::string& name, Reader& r) {
    if (name == "login_compression") {
        connection_.setCompression(r.varInt());
    } else if (name == "custom_query") {  // no plugin channels: answer "not understood"
        Writer w;
        w.varInt(r.varInt());
        w.boolean(false);
        send(Login, "custom_query_answer", w);
    } else if (name == "cookie_request") {
        Writer w;
        w.str(r.str());
        w.boolean(false);
        send(Login, "cookie_response", w);
    } else if (name == "hello") {
        // Online mode. To add: Microsoft account sign-in (MSA -> Xbox Live -> XSTS -> Minecraft services
        // access token), RSA-encrypt a random shared secret and the challenge with the server's public key,
        // join the session (sessionserver.mojang.com/session/minecraft/join with the server hash), send
        // "key", then connection_.setCipher(AES/CFB8 with the shared secret) for both directions.
        push(DisconnectEvent{"the server is in online mode; Microsoft account authentication is not supported yet"});
        return false;
    } else if (name == "login_disconnect") {
        push(DisconnectEvent{jsonComponentText(r.str(), &lang_)});
        return false;
    } else if (name == "login_finished") {
        r.skip(16);
        const std::string_view player = r.str();
        LOG("net: logged in as %.*s (protocol %d)", int(player.size()), player.data(), protocol_);
        switchPhase(Login, "login_acknowledged", Configuration);
        Writer brand;
        brand.str("minecraft:brand");
        brand.str("minecraftCplusplus");
        send(Configuration, "custom_payload", brand);
        Information information;
        {
            std::lock_guard lock(sendMutex_);
            information = info_;
        }
        send(Configuration, "client_information", informationPacket(information));
    }
    return true;
}

void Client::setInformation(const Information& info) {
    {
        std::lock_guard lock(sendMutex_);
        info_ = info;
    }
    if (send(Play, "client_information", informationPacket(info)))  // ignored outside the play phase
        LOG("net: client information: main hand %s, skin parts 0x%02X, server listing %s", info.mainHand ? "right" : "left",
            info.modelParts, info.allowListing ? "on" : "off");
}

// Packets shared by the configuration and play phases.
bool Client::handleCommon(Phase phase, const std::string& name, Reader& r, bool& handled) {
    handled = true;
    if (name == "keep_alive") {
        Writer w;
        w.u64v(r.u64v());
        send(phase, "keep_alive", w);
    } else if (name == "ping") {
        Writer w;
        w.u32v(r.u32v());
        send(phase, "pong", w);
    } else if (name == "disconnect") {
        push(DisconnectEvent{componentText(r.nbt(), &lang_)});
        return false;
    } else if (name == "cookie_request") {
        Writer w;
        w.str(r.str());
        w.boolean(false);
        send(phase, "cookie_response", w);
    } else if (name == "resource_pack_push") {
        const std::string_view id = r.bytes(16), url = r.str();
        LOG("net: server resource pack %.*s declined (not supported)", int(url.size()), url.data());
        Writer w;
        w.bytes(id.data(), id.size());
        w.varInt(1);  // declined
        send(phase, "resource_pack", w);
    } else {
        handled = false;
    }
    return true;
}

const std::vector<Client::RegistryEntry>& Client::registryEntries(const std::string& registry) const {
    static const std::vector<RegistryEntry> kNone;
    auto it = registries_.find(registry);
    return it == registries_.end() ? kNone : it->second;
}

bool Client::handleConfiguration(const std::string& name, Reader& r) {
    if (name == "select_known_packs") {
        // Claim no packs, so the server sends every registry entry in full (dimension types included).
        Writer w;
        w.varInt(0);
        send(Configuration, "select_known_packs", w);
    } else if (name == "registry_data") {
        const std::string registry(r.str());
        const i32 n = r.varInt();
        if (registry == "minecraft:worldgen/biome") biomeBits_ = std::max(1, ceilLog2(u32(n)));
        static const char* const kKept[] = {"minecraft:enchantment", "minecraft:painting_variant", "minecraft:instrument",
                                            "minecraft:banner_pattern"};
        std::vector<RegistryEntry>* kept = nullptr;
        for (const char* k : kKept)
            if (registry == k) kept = &registries_[registry], kept->clear();
        for (i32 i = 0; i < n && r.ok; ++i) {
            const std::string id(r.str());
            const std::string_view data = r.boolean() ? r.nbt() : std::string_view{};
            if (kept) kept->push_back({id, std::string(data)});
            if (registry == "minecraft:dimension_type") dimensionTypes_.push_back(parseDimensionType(id, data));
            if (registry == "minecraft:worldgen/biome") {
                if (i == 0) biomeIds_.clear();
                biomeIds_.push_back(World::biomeId(id));
            }
        }
    } else if (name == "custom_payload") {  // Fabric servers' handshake (net/fabric_sync.h)
        const std::string channel(r.str());
        Writer w;
        if (channel == "minecraft:register") {
            // The channels we receive: with mods that add blocks / items the server sends their ids (or disconnects).
            w.str("minecraft:register");
            constexpr std::string_view ours = "fabric:registry/sync";
            w.bytes(ours.data(), ours.size());
            send(Configuration, "custom_payload", w);
        } else if (channel == "fabric:registry/sync") {
            if (fabric_.parseSync(r)) {
                const std::vector<std::string> table = fabric_.stateTable();
                if (!table.empty()) blockIds_.initTable(table, registry_);
                LOG("net: Fabric registry sync: %zu modded blocks%s", fabric_.moddedBlocks(),
                    table.empty() ? " (no block table from the server: they show as air)" : "");
            }
            w.str("fabric:registry/sync/complete");
            send(Configuration, "custom_payload", w);
        }
    } else if (name == "code_of_conduct") {
        const std::string_view text = r.str();
        LOG("net: server code of conduct:\n%.*s", int(text.size()), text.data());
        send(Configuration, "accept_code_of_conduct", Writer{});
    } else if (name == "finish_configuration") {
        switchPhase(Configuration, "finish_configuration", Play);
    }
    return true;
}

bool Client::handlePlay(const std::string& name, Reader& r) {
    if (auto p = playPeeks_.find(name); p != playPeeks_.end())
        for (const PacketHandler& peek : p->second) {
            Reader copy = r;
            peek(copy);
        }
    if (auto h = playHandlers_.find(name); h != playHandlers_.end()) {
        for (auto& handler : h->second) {
            Reader copy = r;
            handler(copy);
        }
        return true;
    }
    const int minSy = dimension_.minY >> 4, sections = dimension_.height >> 4;
    if (name == "level_chunk_with_light") {
        Timer t;
        ChunkEvent e;
        e.cx = r.i32v(), e.cz = r.i32v();
        const i32 heightmaps = r.varInt();
        for (i32 i = 0; i < heightmaps && r.ok; ++i) {  // not used: our World derives what it needs
            r.varInt();
            r.skip(size_t(u32(r.varInt())) * 8);
        }
        const std::string_view data = r.byteArray();
        Reader dr(reinterpret_cast<const u8*>(data.data()), data.size());
        if (!decodeSections(dr, sections, blockIds_, biomeBits_, e.sections, biomeIds_.empty() ? nullptr : &biomeIds_)) {
            LOG("net: malformed chunk %d %d skipped", e.cx, e.cz);
            return true;
        }
        const i32 blockEntities = r.varInt();
        for (i32 i = 0; i < blockEntities && r.ok; ++i) {
            r.u8v(), r.i16v(), r.varInt();
            r.nbt();
        }
        std::vector<LightSection> light;
        readLight(r, minSy, sections + 2, light);
        e.firstSy = minSy - 1;
        for (LightSection& l : light) {
            World::PackedSection& s = e.sections[size_t(l.sy - e.firstSy)];
            (l.sky ? s.sky : s.block) = World::kLightUniform | 0;
            (l.sky ? s.skyData : s.blockData) = std::move(l.data);
        }
        const double ms = t.ms();
        {
            std::lock_guard lock(mutex_);
            if (!stats_.chunks) stats_.firstChunkMs = sinceStart();
            ++stats_.chunks;
            stats_.decodeMs += ms;
            stats_.maxDecodeMs = std::max(stats_.maxDecodeMs, ms);
        }
        push(std::move(e));
    } else if (name == "block_update") {
        BlockChange c;
        r.blockPos(c.x, c.y, c.z);
        c.id = blockIds_.local(u32(r.varInt()));
        push(BlocksEvent{{c}});
    } else if (name == "section_blocks_update") {
        const i64 s = r.i64v();
        const int sx = int(s >> 42), sy = int((s << 44) >> 44), sz = int((s << 22) >> 42);
        BlocksEvent e;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            const i64 v = r.varLong();
            const int p = int(v & 0xFFF);
            e.changes.push_back({sx * 16 + ((p >> 8) & 15), sy * 16 + (p & 15), sz * 16 + ((p >> 4) & 15),
                                 blockIds_.local(u32(u64(v) >> 12))});
        }
        push(std::move(e));
    } else if (name == "light_update") {
        LightEvent e;
        e.cx = r.varInt(), e.cz = r.varInt();
        readLight(r, minSy, sections + 2, e.sections);
        push(std::move(e));
    } else if (name == "forget_level_chunk") {
        const i64 v = r.i64v();
        push(ForgetEvent{int(i32(v & 0xFFFFFFFF)), int(i32(v >> 32))});
    } else if (name == "chunk_batch_finished") {
        Writer w;
        w.f32(kChunksPerTick);
        send(Play, "chunk_batch_received", w);
    } else if (name == "block_changed_ack") {
        push(AckEvent{r.varInt()});
    } else if (name == "player_position") {
        TeleportEvent e;
        e.id = r.varInt();
        for (double& v : e.pos) v = r.f64();
        for (double& v : e.delta) v = r.f64();
        e.yaw = r.f32(), e.pitch = r.f32();
        e.relative = r.u32v();
        push(e);
    } else if (name == "login" || name == "respawn") {
        JoinEvent e;
        e.receivedAt = nowSeconds();
        e.respawn = name == "respawn";
        if (!e.respawn) {
            e.entityId = r.i32v();
            r.boolean();  // hardcore
            const i32 levels = r.varInt();
            for (i32 i = 0; i < levels && r.ok; ++i) r.str();
            r.varInt();  // max players
            e.viewDistance = r.varInt();
            viewDistance_ = e.viewDistance;
            simulationDistance_ = r.varInt();
            r.boolean(), r.boolean(), r.boolean();  // reduced debug info, death screen, limited crafting
        }
        const i32 type = r.varInt();
        const std::string level(r.str());
        r.i64v();  // hashed seed
        e.gameMode = r.varInt();
        if (!r.ok || type < 0 || size_t(type) >= dimensionTypes_.size()) {
            LOG("net: unknown dimension type %d", type);
            return true;
        }
        dimension_ = dimensionTypes_[size_t(type)];
        dimension_.name = level;
        e.dimension = dimension_;
        if (!e.respawn) {
            std::lock_guard lock(mutex_);
            stats_.joinMs = sinceStart();
        }
        LOG("net: %s %s (y %d..%d), game mode %d", e.respawn ? "respawned in" : "joined", level.c_str(),
            dimension_.minY, dimension_.minY + dimension_.height - 1, e.gameMode);
        push(e);
    } else if (name == "player_abilities") {  // flags (invulnerable 1, flying 2, may fly 4, instabuild 8), speeds
        const u8 flags = r.u8v();
        const float flyingSpeed = r.f32(), walkingSpeed = r.f32();
        if (r.ok) push(AbilitiesEvent{(flags & 2) != 0, (flags & 4) != 0, (flags & 8) != 0, flyingSpeed, walkingSpeed});
    } else if (name == "game_event") {
        const u8 event = r.u8v();
        const float param = r.f32();
        if (event == 3) push(GameModeEvent{int(param)});
    } else if (name == "set_held_slot") {
        push(HeldSlotEvent{r.varInt()});
    } else if (name == "set_chunk_cache_radius") {
        viewDistance_ = r.varInt();
        LOG("net: server view distance %d chunks", viewDistance_.load());
    } else if (name == "set_chunk_cache_center") {
        const int x = r.varInt(), z = r.varInt();
        if (r.ok) viewCenter_[0] = x, viewCenter_[1] = z;
    } else if (name == "set_simulation_distance") {
        simulationDistance_ = r.varInt();
        LOG("net: server simulation distance %d chunks", simulationDistance_.load());
    } else if (name == "chunks_biomes") {  // ClientboundChunksBiomesPacket: (ChunkPos, biome containers of every section)
        BiomesEvent e;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            const i64 pos = r.i64v();
            BiomesEvent::Column c;
            c.cx = int(i32(pos & 0xFFFFFFFF)), c.cz = int(i32(pos >> 32));
            c.firstSy = minSy;
            const std::string_view data = r.byteArray();
            Reader dr(reinterpret_cast<const u8*>(data.data()), data.size());
            c.sections.resize(size_t(sections));
            for (auto& s : c.sections) decodeBiomes(dr, biomeBits_, biomeIds_.empty() ? nullptr : &biomeIds_, s.data());
            if (dr.ok) e.columns.push_back(std::move(c));
        }
        if (r.ok) push(std::move(e));
    } else if (name == "system_chat") {
        const std::string text = componentText(r.nbt(), &lang_);
        if (!r.boolean()) chat(text);  // overlay = action bar
    } else if (name == "player_chat") {
        r.varInt();
        r.skip(16);  // sender
        r.varInt();
        if (r.boolean()) r.skip(256);  // signature
        std::string text(r.str());
        r.i64v(), r.i64v();  // timestamp, salt
        const i32 seen = r.varInt();
        for (i32 i = 0; i < seen && r.ok; ++i)
            if (r.varInt() == 0) r.skip(256);
        if (r.boolean()) text = componentText(r.nbt(), &lang_);  // unsigned (decorated) content
        if (r.varInt() == 2) r.byteArray();                      // partially filtered mask
        std::string sender = "?";
        if (r.varInt() != 0) sender = componentText(r.nbt(), &lang_);  // registered chat type: then the name
        chat("<" + sender + "> " + text);
    } else if (name == "disguised_chat") {
        const std::string text = componentText(r.nbt(), &lang_);
        const std::string sender = r.varInt() != 0 ? componentText(r.nbt(), &lang_) : "?";
        chat("<" + sender + "> " + text);
    } else if (name == "start_configuration") {
        switchPhase(Play, "configuration_acknowledged", Configuration);
    }
    return true;
}

void Client::sendAcceptTeleport(int id, const double pos[3], float yaw, float pitch) {
    Writer w;
    w.varInt(id);
    for (int a = 0; a < 3; ++a) w.f64(pos[a]);
    w.f32(yaw);
    w.f32(pitch);
    send(Play, "accept_teleportation", w);
}

void Client::sendMove(const double pos[3], float yaw, float pitch, bool onGround, bool horizontalCollision) {
    Writer w;
    for (int a = 0; a < 3; ++a) w.f64(pos[a]);
    w.f32(yaw);
    w.f32(pitch);
    w.u8v(u8((onGround ? 1 : 0) | (horizontalCollision ? 2 : 0)));
    send(Play, "move_player_pos_rot", w);
}

void Client::sendTickEnd() { send(Play, "client_tick_end", Writer{}); }

void Client::sendPlayerLoaded() { send(Play, "player_loaded", Writer{}); }

void Client::sendPlayerAction(int action, int x, int y, int z, int face, int sequence) {
    Writer w;
    w.varInt(action);
    w.blockPos(x, y, z);
    w.u8v(u8(face));
    w.varInt(sequence);
    send(Play, "player_action", w);
}

void Client::sendUseItemOn(int x, int y, int z, int face, const float cursor[3], int sequence, int hand, bool inside) {
    Writer w;
    w.varInt(hand);  // 0 main, 1 off hand
    w.blockPos(x, y, z);
    w.varInt(face);
    for (int a = 0; a < 3; ++a) w.f32(cursor[a]);
    w.boolean(inside);
    w.boolean(false);  // world border hit (BlockHitResult.hitBorder is never used)
    w.varInt(sequence);
    send(Play, "use_item_on", w);
}

void Client::sendCarriedItem(int slot) {
    Writer w;
    w.u16v(u16(slot));
    send(Play, "set_carried_item", w);
}

void Client::sendCreativeSlot(int slot, int itemId, int count) {
    Writer w;
    w.u16v(u16(slot));
    if (itemId < 0 || count <= 0) {
        w.varInt(0);
    } else {
        w.varInt(count);
        w.varInt(itemId);
        w.varInt(0);  // no added components
        w.varInt(0);  // no removed components
    }
    send(Play, "set_creative_mode_slot", w);
}

}  // namespace net
