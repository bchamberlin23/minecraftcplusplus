#include "net/client.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "assets/asset_source.h"
#include "assets/zip.h"
#include "platform/crash.h"
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
    {
        std::lock_guard lock(registryMutex_);
        cookies_ = config_.cookies;
    }
    // Handshake (intent: login, or transfer after a ClientboundTransferPacket), then login start with the
    // offline-mode profile.
    Writer hs;
    hs.varInt(0);  // the handshake phase's only packet
    hs.varInt(protocol_);
    hs.str(config_.host);
    hs.u16v(config_.port);
    hs.varInt(config_.transfer ? 3 : 2);
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
        if (!r.ok || id < 0 || size_t(id) >= names.size()) {  // unknown: skip by length
            diagnose(phase, "#" + std::to_string(id), false, r.left(), r.left(), false);
            continue;
        }
        const std::string& name = names[size_t(id)];
        const size_t size = r.left();
        readLeft_ = size, readOverran_ = false;
        bool keepGoing, handled = false;
        if (phase == Login) {
            keepGoing = handleLogin(name, r, handled);
        } else {
            keepGoing = handleCommon(phase, name, r, handled);
            if (keepGoing && !handled)
                keepGoing = phase == Configuration ? handleConfiguration(name, r, handled) : handlePlay(name, r, handled);
        }
        if (!keepGoing) {
            reason.clear();
            break;
        }
        diagnose(phase, name, handled, size, readLeft_, readOverran_);
    }
    connection_.close();
    alive_ = false;
    if (!quit_ && !reason.empty()) push(DisconnectEvent{reason});
}

void Client::diagnose(Phase phase, const std::string& name, bool handled, size_t size, size_t left, bool overran) {
    // Packets one of the readers takes whole are understood; anything else is logged once per packet type.
    const char* problem = !handled ? "unhandled" : overran ? "overran" : left ? "partial" : nullptr;
    if (!problem || !diagnosed_.insert(std::string(kPhaseNames[phase]) + "/" + name + "/" + problem).second) return;
    if (!handled)
        LOG("net: [diag] %s packet %s ignored: nothing handles it (%zu bytes)", kPhaseNames[phase], name.c_str(), size);
    else if (overran)
        LOG("net: [diag] %s packet %s: a reader read past its end (%zu bytes): malformed or misread", kPhaseNames[phase],
            name.c_str(), size);
    else
        LOG("net: [diag] %s packet %s read partially: %zu of %zu bytes left unread", kPhaseNames[phase], name.c_str(), left,
            size);
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

bool Client::handleLogin(const std::string& name, Reader& r, bool& handled) {
    handled = true;
    if (name == "login_compression") {
        connection_.setCompression(r.varInt());
    } else if (name == "custom_query") {  // no plugin channels: answer "not understood"
        Writer w;
        w.varInt(r.varInt());
        w.boolean(false);
        send(Login, "custom_query_answer", w);
    } else if (name == "cookie_request") {
        send(Login, "cookie_response", cookieResponse(r));
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
        const i32 properties = r.varInt();  // the profile's properties (skin textures): name, value, signature
        for (i32 i = 0; i < properties && r.ok; ++i) {
            r.str(), r.str();
            if (r.boolean()) r.str();
        }
        r.skip(16);  // session id
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
    } else {
        handled = false;
    }
    noteRead(r);
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

Writer Client::cookieResponse(Reader& r) {  // ClientboundCookieRequestPacket: the stored payload, if any
    const std::string key(r.str());
    Writer w;
    w.str(key);
    std::lock_guard lock(registryMutex_);
    auto it = cookies_.find(key);
    w.boolean(it != cookies_.end());
    if (it != cookies_.end()) {
        w.varInt(i32(it->second.size()));
        w.bytes(it->second.data(), it->second.size());
    }
    return w;
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
        send(phase, "cookie_response", cookieResponse(r));
    } else if (name == "store_cookie") {  // ClientboundStoreCookiePacket: kept for this connection and transfers
        const std::string key(r.str()), payload(r.byteArray());
        if (r.ok && payload.size() <= 5120) {
            std::lock_guard lock(registryMutex_);
            cookies_[key] = payload;
        }
    } else if (name == "resource_pack_push") {  // answered by the main thread (ui/server_config.cpp)
        PackPushEvent e;
        e.id = std::string(r.bytes(16));
        e.url = std::string(r.str());
        e.hash = std::string(r.str());
        e.required = r.boolean();
        if (r.boolean()) e.prompt = std::string(r.nbt());
        if (r.ok) pushCommon(std::move(e));
    } else if (name == "resource_pack_pop") {
        PackPopEvent e;
        if (r.boolean()) e.id = std::string(r.bytes(16));
        if (r.ok) pushCommon(std::move(e));
    } else if (name == "transfer") {  // ClientCommonPacketListenerImpl.handleTransfer: this connection ends here
        TransferEvent e;
        e.host = std::string(r.str());
        e.port = r.varInt();
        if (!r.ok) return true;
        LOG("net: transfer to %s:%d", e.host.c_str(), e.port);
        pushCommon(std::move(e));
        auto it = lang_.find("disconnect.transfer");
        push(DisconnectEvent{it != lang_.end() ? it->second : "Transferred to another server"});
        return false;
    } else if (name == "server_links") {  // ClientCommonPacketListenerImpl.handleServerLinks
        std::vector<ServerLink> links;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            ServerLink l;
            if (r.boolean()) l.known = r.varInt();  // Either<KnownLinkType, Component>
            else l.label = std::string(r.nbt());
            l.url = std::string(r.str());
            // Util.parseAndValidateUntrustedUri: http(s) only; others are dropped.
            if (l.url.starts_with("http://") || l.url.starts_with("https://")) links.push_back(std::move(l));
        }
        if (r.ok) {
            std::lock_guard lock(registryMutex_);
            serverLinks_ = std::move(links);
        }
    } else if (name == "custom_report_details") {  // shown in crash reports
        std::map<std::string, std::string> details;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            std::string key(r.str());
            details[key] = std::string(r.str());
        }
        if (r.ok) {
            std::string text;
            for (const auto& [key, value] : details) text += "\t" + key + ": " + value + "\n";
            setCrashDetails(text);
            std::lock_guard lock(registryMutex_);
            reportDetails_ = std::move(details);
        }
    } else if (name == "update_tags") {  // both phases: replaces the tags of each registry listed
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<int>>> tags;
        const i32 registries = r.varInt();
        for (i32 i = 0; i < registries && r.ok; ++i) {
            auto& reg = tags[std::string(r.str())];
            const i32 n = r.varInt();
            for (i32 t = 0; t < n && r.ok; ++t) {
                std::vector<int>& ids = reg[std::string(r.str())];
                const i32 count = r.varInt();
                for (i32 k = 0; k < count && r.ok; ++k) ids.push_back(r.varInt());
            }
        }
        if (r.ok) {
            std::lock_guard lock(registryMutex_);
            for (auto& [registry, t] : tags) tags_[registry] = std::move(t);
            if (phase == Play) ++registryVersion_;
        }
    } else {
        handled = false;
    }
    if (handled) noteRead(r);
    return true;
}

void Client::pushCommon(CommonEvent e) {
    std::lock_guard lock(mutex_);
    common_.push_back(std::move(e));
}

void Client::pollCommon(std::vector<CommonEvent>& out) {
    std::lock_guard lock(mutex_);
    for (CommonEvent& e : common_) out.push_back(std::move(e));
    common_.clear();
}

bool Client::hasCommon() const {
    std::lock_guard lock(mutex_);
    return !common_.empty();
}

void Client::sendPackStatus(const std::string& id, int action) {
    Writer w;
    w.bytes(id.data(), id.size());
    w.varInt(action);
    std::lock_guard lock(sendMutex_);
    if (phase_ != Login) sendRaw(Phase(phase_.load()), "resource_pack", w);
}

void Client::acceptCodeOfConduct() { send(Configuration, "accept_code_of_conduct", Writer{}); }

void Client::disconnect(const std::string& reason) {
    push(DisconnectEvent{reason});
    quit_ = true;
    connection_.close();
}

std::map<std::string, std::string> Client::cookies() const {
    std::lock_guard lock(registryMutex_);
    return cookies_;
}

std::string Client::serverBrand() const {
    std::lock_guard lock(registryMutex_);
    return serverBrand_;
}

std::map<std::string, std::string> Client::reportDetails() const {
    std::lock_guard lock(registryMutex_);
    return reportDetails_;
}

std::vector<Client::ServerLink> Client::serverLinks() const {
    std::lock_guard lock(registryMutex_);
    return serverLinks_;
}

std::vector<Client::RegistryEntry> Client::registryEntries(const std::string& registry) const {
    std::lock_guard lock(registryMutex_);
    auto it = registries_.find(registry);
    return it == registries_.end() ? std::vector<RegistryEntry>{} : it->second;
}

std::string Client::registryId(const std::string& registry, int networkId) const {
    std::lock_guard lock(registryMutex_);
    auto it = registries_.find(registry);
    return it == registries_.end() || networkId < 0 || size_t(networkId) >= it->second.size()
               ? std::string()
               : it->second[size_t(networkId)].id;
}

bool Client::readTags(Reader& r, std::unordered_map<std::string, TagMap>& out) {
    const i32 registries = r.varInt();  // registry -> TagNetworkSerialization.NetworkPayload (tag -> ids)
    for (i32 i = 0; i < registries && r.ok; ++i) {
        TagMap& tags = out[std::string(r.str())];
        tags.clear();
        const i32 n = r.varInt();
        for (i32 k = 0; k < n && r.ok; ++k) {
            std::vector<int>& ids = tags[std::string(r.str())];
            const i32 c = r.varInt();
            for (i32 j = 0; j < c && r.ok; ++j) ids.push_back(r.varInt());
        }
    }
    return r.ok;
}

std::unordered_map<std::string, Client::TagMap> Client::tags() const {
    std::lock_guard lock(registryMutex_);
    return tags_;
}

std::vector<int> Client::tagEntries(const std::string& registry, const std::string& tag) const {
    std::lock_guard lock(registryMutex_);
    auto it = tags_.find(registry);
    if (it == tags_.end()) return {};
    auto t = it->second.find(tag.find(':') == std::string::npos ? "minecraft:" + tag : tag);
    return t == it->second.end() ? std::vector<int>{} : t->second;
}

std::vector<std::string> Client::enabledFeatures() const {
    std::lock_guard lock(registryMutex_);
    return features_;
}

bool Client::handleConfiguration(const std::string& name, Reader& r, bool& handled) {
    handled = true;
    if (name == "select_known_packs") {
        // Claim no packs, so the server sends every registry entry in full (dimension types included).
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) r.str(), r.str(), r.str();  // namespace, id, version
        Writer w;
        w.varInt(0);
        send(Configuration, "select_known_packs", w);
    } else if (name == "registry_data") {
        // RegistryDataCollector: a registry sent again (reconfiguration) replaces its previous entries.
        const std::string registry(r.str());
        const i32 n = r.varInt();
        if (registry == "minecraft:worldgen/biome") biomeBits_ = std::max(1, ceilLog2(u32(n))), biomeIds_.clear();
        if (registry == "minecraft:dimension_type") dimensionTypes_.clear();
        std::vector<RegistryEntry> entries;
        for (i32 i = 0; i < n && r.ok; ++i) {
            const std::string id(r.str());
            const std::string_view data = r.boolean() ? r.nbt() : std::string_view{};
            entries.push_back({id, std::string(data)});
            if (registry == "minecraft:dimension_type") dimensionTypes_.push_back(parseDimensionType(id, data));
            if (registry == "minecraft:worldgen/biome") biomeIds_.push_back(World::biomeId(id));
        }
        std::lock_guard lock(registryMutex_);
        registries_[registry] = std::move(entries);
    } else if (name == "update_enabled_features") {  // the world's feature flags ("minecraft:vanilla", experiments)
        std::vector<std::string> features;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) features.emplace_back(r.str());
        std::lock_guard lock(registryMutex_);
        features_ = std::move(features);
    } else if (name == "reset_chat") {
        pushCommon(ResetChatEvent{});
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
        } else if (channel == "minecraft:brand") {  // BrandPayload
            const std::string brand(r.str());
            std::lock_guard lock(registryMutex_);
            serverBrand_ = brand;
        } else if (diagnosed_.insert("configuration/custom_payload/" + channel).second) {
            LOG("net: [diag] configuration custom_payload %s ignored (%zu bytes)", channel.c_str(), r.left());
        }
        r.skip(r.left());  // payloads of channels we do not speak are discarded (the game's DiscardedPayload)
    } else if (name == "code_of_conduct") {  // answered by the main thread (ui/server_config.cpp)
        pushCommon(CodeOfConductEvent{std::string(r.str())});
    } else if (name == "finish_configuration") {
        {
            std::lock_guard lock(registryMutex_);
            size_t tags = 0;
            for (const auto& [registry, t] : tags_) tags += t.size();
            LOG("net: configuration finished: %zu registries, %zu tags in %zu registries, features %zu", registries_.size(),
                tags, tags_.size(), features_.size());
        }
        ++registryVersion_;
        switchPhase(Configuration, "finish_configuration", Play);
        pushCommon(ConfigurationEvent{false});
    } else {
        handled = false;
    }
    if (handled) noteRead(r);
    return true;
}

bool Client::handlePlay(const std::string& name, Reader& r, bool& handled) {
    handled = true;
    bool peeked = false;
    if (auto p = playPeeks_.find(name); p != playPeeks_.end())
        for (const PacketHandler& peek : p->second) {
            Reader copy = r;
            peek(copy);
            noteRead(copy);
            peeked = true;
        }
    if (auto h = playHandlers_.find(name); h != playHandlers_.end()) {
        for (auto& handler : h->second) {
            Reader copy = r;
            handler(copy);
            noteRead(copy);
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
    } else if (name == "chunk_batch_start" || name == "bundle_delimiter") {
        // Nothing to do: batches are acknowledged when they finish; bundled packets are applied in order anyway.
    } else if (name == "chunk_batch_finished") {
        r.varInt();  // batch size
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
        const i32 type = r.varInt();  // CommonPlayerSpawnInfo
        const std::string level(r.str());
        r.i64v();  // hashed seed
        e.gameMode = r.varInt();
        r.u8v();                 // previous game mode
        r.boolean(), r.boolean();  // debug, flat
        if (r.boolean()) r.str(), r.i64v();  // last death location
        r.varInt(), r.varInt();  // portal cooldown, sea level
        if (e.respawn) r.u8v();  // data to keep
        else r.boolean(), r.boolean();  // online mode, enforces secure chat
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
    } else if (name == "start_configuration") {  // reconfiguration: the level goes until the next login
        switchPhase(Play, "configuration_acknowledged", Configuration);
        push(ReconfigureEvent{});
        pushCommon(ConfigurationEvent{true});
    } else {
        handled = peeked;
        return true;
    }
    noteRead(r);
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
