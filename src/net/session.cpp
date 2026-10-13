#include "net/session.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "world/terrain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace net {

namespace {
constexpr double kTick = 0.05;
constexpr double kPi = 3.14159265358979;
constexpr int kChunksPerFrame = 32;      // cheap chunks can drain faster even at a low frame rate
constexpr double kChunkBudgetMs = 4.0;   // ...but never more than this per frame
constexpr int kReleaseUseItem = 6;
constexpr int kStab = 8;  // ServerboundPlayerActionPacket.Action.STAB

u64 posKey(int x, int y, int z) { return (u64(x & 0x3FFFFFF) << 38) | (u64(z & 0x3FFFFFF) << 12) | u64(y & 0xFFF); }
void unpackPos(u64 k, int& x, int& y, int& z) {
    x = int(i64(k) >> 38);
    y = int(i64(k << 52) >> 52);
    z = int(i64(k << 26) >> 38);
}
}  // namespace

Session::Session(Client& client, World& world, BlockRegistry& registry, const GameReports& reports)
    : client_(client), world_(world), registry_(registry), lighting_(world, registry), physics_(world, registry) {
    const nlohmann::json& reg = reports.registries();
    if (reg.is_object() && reg.contains("minecraft:item")) {
        const nlohmann::json entries = reg["minecraft:item"].value("entries", nlohmann::json::object());
        for (const auto& item : entries.items()) items_[item.key()] = item.value().value("protocol_id", -1);
    }
    const char* verbose = std::getenv("MCPP_NET_VERBOSE");
    verbose_ = verbose && *verbose == '1';
}

bool Session::join(double timeoutSeconds, PlayerStart& start, Dimension& dimension, bool* deferred, bool patient) {
    double deadline = nowSeconds() + timeoutSeconds;
    bool joined = false;
    while (nowSeconds() < deadline) {
        if (patient && !client_.inPlay()) deadline = nowSeconds() + timeoutSeconds;
        if (deferred && !joined && client_.hasCommon()) {
            *deferred = true;
            return false;
        }
        client_.poll(incoming_);
        for (Event& e : incoming_) {
            if (auto* d = std::get_if<DisconnectEvent>(&e)) {
                LOG("net: disconnected: %s", d->reason.c_str());
                disconnectReason_ = d->reason;
                return false;
            }
            if (auto* j = std::get_if<JoinEvent>(&e); j && !joined) {
                joined = true;
                dimension_ = dimension = j->dimension;
                levelStart_ = j->receivedAt;
                entityId_ = j->entityId;
                gameMode_ = j->gameMode;
                portalCooldown_ = j->portalCooldown;
                instabuild_ = gameMode_ == 1;
                continue;
            }
            if (auto* t = std::get_if<TeleportEvent>(&e); t && joined && !haveServerPos_) {
                std::copy(t->pos, t->pos + 3, serverPos_);  // the first teleport is absolute
                haveServerPos_ = true;
                start.x = t->pos[0], start.y = t->pos[1], start.z = t->pos[2];
                start.yaw = t->yaw, start.pitch = t->pitch;
                start.found = true;
                client_.sendAcceptTeleport(t->id, t->pos, t->yaw, t->pitch);
                continue;
            }
            pending_.push_back(std::move(e));
        }
        incoming_.clear();
        if (haveServerPos_) return true;
        if (!client_.alive()) {
            LOG("net: connection lost before joining");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    LOG("net: timed out waiting for the server to %s", joined ? "position the player" : "accept the login");
    return false;
}

void Session::attach(Game& game, Terrain& terrain) {
    game_ = &game;
    terrain_ = &terrain;
    if (dimension_.name != "minecraft:overworld") clearWorld();  // discard the disk preview on a Nether/End login
    terrain_->setFarActive(dimension_.name == "minecraft:overworld");
    world_.setTrackChanges(false);  // nothing received from a server is saved
    game.onEdit = [this](const Game::EditAction& a) { onEdit(a); };
    game.player().noClip = gameMode_ == 3;
    game.player().mayFly = gameMode_ == 1 || gameMode_ == 3;  // until the server's abilities arrive
    game.player().portalCooldown = portalCooldown_;  // handleLogin: setPortalCooldown
    game.onPunch = [this] {
        if (gameMode_ != 3) client_.sendPlay("punch", Writer{});  // spectators do not attack
    };
    game.onUseItem = [this](int hand) { useItem(hand); };
    game.onStab = [this] {  // ServerboundPlayerActionPacket STAB (piercing weapons)
        syncHotbar();
        client_.sendPlayerAction(kStab, 0, 0, 0, 0, 0);
        if (verbose_) LOG("net: player_action stab");
    };
    game.onReleaseUseItem = [this] {  // ServerPlayerGameMode.releaseUsingItem (bows; stops eating early)
        client_.sendPlayerAction(kReleaseUseItem, 0, 0, 0, 0, 0);
        if (verbose_) LOG("net: release_use_item");
    };
}

void Session::frame(double dt) {
    client_.poll(incoming_);
    const bool changingLevel = std::any_of(incoming_.begin(), incoming_.end(),
                                         [](const Event& e) { return std::holds_alternative<JoinEvent>(e); });
    for (Event& e : incoming_) pending_.push_back(std::move(e));
    incoming_.clear();
    if (changingLevel) {
        // A respawn/portal packet supersedes terrain updates for the previous level. Keep
        // control events ordered (especially teleport acknowledgements), but do not spend
        // frames applying/meshing chunks that clearWorld is about to discard.
        const auto lastJoin = std::prev(std::find_if(pending_.rbegin(), pending_.rend(),
            [](const Event& e) { return std::holds_alternative<JoinEvent>(e); }).base());
        const auto keptEnd = std::remove_if(pending_.begin(), lastJoin, [](const Event& e) {
            return std::holds_alternative<ChunkEvent>(e) || std::holds_alternative<LightEvent>(e) ||
                   std::holds_alternative<BlocksEvent>(e) || std::holds_alternative<BiomesEvent>(e) ||
                   std::holds_alternative<ForgetEvent>(e);
        });
        const size_t discarded = size_t(std::distance(keptEnd, lastJoin));
        pending_.erase(keptEnd, lastJoin);
        if (discarded) LOG("net: skipped %zu obsolete terrain events before dimension change", discarded);
    }
    // Strictly in order; chunks and block updates (relight + remesh each; the server sends bursts of them while
    // it generates around a travelling player) are the costly events and share a per-frame budget.
    Timer budget;
    int chunks = 0;
    while (!pending_.empty()) {
        Event& e = pending_.front();
        if (std::holds_alternative<ChunkEvent>(e)) {
            if (chunks >= kChunksPerFrame || budget.ms() > kChunkBudgetMs) break;
            ++chunks;
        } else if (std::holds_alternative<BlocksEvent>(e) && budget.ms() > kChunkBudgetMs) {
            break;
        }
        handle(e);
        pending_.pop_front();
    }
    world_.clearDirtyChunks();

    // Like the game, the player does not move into chunks that have not arrived: it waits at the last
    // position over a loaded chunk (or where the server put it).
    Player& p = game_->player();
    const bool loaded = world_.hasChunk(int(std::floor(p.pos[0])) >> 4, int(std::floor(p.pos[2])) >> 4);
    if (loaded) {
        std::copy(p.pos, p.pos + 3, serverPos_);
    } else if (haveServerPos_) {
        std::copy(serverPos_, serverPos_ + 3, p.pos);
        std::copy(serverPos_, serverPos_ + 3, p.prev);
        std::fill(p.vel, p.vel + 3, 0.0);
    }
    if (loaded && haveServerPos_ && !loadedSent_) {
        client_.sendPlayerLoaded();
        loadedSent_ = true;
        if (levelStart_ > 0)
            LOG("net: player chunk ready in %.1f ms in %s", (nowSeconds() - levelStart_) * 1000,
                dimension_.name.c_str());
    }
    syncHotbar();
    game_->instabuild = instabuild_;  // MultiPlayerGameMode: creative digging
    game_->gameMode = gameMode_;
    tickClock_ = std::min(tickClock_ + dt, 0.25);
    while (tickClock_ >= kTick) {
        tickClock_ -= kTick;
        tick();
    }
}

void Session::handle(Event& e) {
    if (auto* c = std::get_if<ChunkEvent>(&e)) {
        applyChunk(*c);
    } else if (auto* b = std::get_if<BlocksEvent>(&e)) {
        applyBlocks(b->changes);
    } else if (auto* l = std::get_if<LightEvent>(&e)) {
        applyLight(*l);
    } else if (auto* bi = std::get_if<BiomesEvent>(&e)) {
        applyBiomes(*bi);
    } else if (auto* f = std::get_if<ForgetEvent>(&e)) {
        forget(f->cx, f->cz);
    } else if (auto* a = std::get_if<AckEvent>(&e)) {
        acknowledge(a->sequence);
    } else if (auto* t = std::get_if<TeleportEvent>(&e)) {
        teleport(*t);
    } else if (auto* j = std::get_if<JoinEvent>(&e)) {  // respawn or re-login: a new level
        if (j->dimension.name != dimension_.name || !keepForgotten_)
            clearWorld();  // another dimension: nothing of the old one may stay
        else
            for (u64 k : std::vector<u64>(chunks_.begin(), chunks_.end())) forget(int(i32(k >> 32)), int(i32(k)), true);
        predictions_.clear();
        sentInput_ = 0, sentSprinting_ = false;  // a new server player (respawn): nothing pressed, not sprinting
        dimension_ = j->dimension;
        levelStart_ = j->receivedAt;
        if (!j->respawn) entityId_ = j->entityId;
        gameMode_ = j->gameMode;
        game_->player().noClip = gameMode_ == 3;
        game_->player().portalCooldown = j->portalCooldown;  // handleRespawn: the new player's setPortalCooldown
        instabuild_ = gameMode_ == 1;
        loadedSent_ = false;
        if (dimension_.minY < world_.minSectionY() * 16 || dimension_.minY + dimension_.height > (world_.maxSectionY() + 1) * 16)
            LOG("warning: %s is taller than the world storage; blocks outside y %d..%d are dropped",
                dimension_.name.c_str(), world_.minSectionY() * 16, world_.maxSectionY() * 16 + 15);
    } else if (auto* ab = std::get_if<AbilitiesEvent>(&e)) {
        Player& p = game_->player();
        p.flying = ab->flying || (ab->mayFly && p.flying);
        if (!ab->mayFly) p.flying = false;
        p.mayFly = ab->mayFly;
        p.canSprint = p.foodSprint || p.mayFly;  // hasEnoughFoodToSprint
        p.flyingSpeed = ab->flyingSpeed, p.walkingSpeed = ab->walkingSpeed;
        sentFlying_ = p.flying;  // the server's own state
        instabuild_ = ab->instabuild;
    } else if (auto* g = std::get_if<GameModeEvent>(&e)) {
        gameMode_ = g->mode;
        game_->player().noClip = gameMode_ == 3;
        LOG("net: game mode %d", gameMode_);
        std::fill(std::begin(sentItems_), std::end(sentItems_), -2);  // creative inventory needs a resync
    } else if (std::holds_alternative<HeldSlotEvent>(e)) {
        sentSlot_ = -1;  // the hotbar selection is ours: tell the server again
    } else if (auto* d = std::get_if<DisconnectEvent>(&e)) {
        LOG("net: disconnected: %s", d->reason.c_str());
        disconnectReason_ = d->reason;
    } else if (std::holds_alternative<ReconfigureEvent>(e)) {
        // ClientPacketListener.handleConfigurationStart: the level goes; the login after the configuration brings
        // the next one (and the player's position).
        clearWorld();
        predictions_.clear();
        loadedSent_ = false;
        LOG("net: reconfiguration: the level was cleared");
    }
}

void Session::applyChunk(ChunkEvent& e) {
    Timer t;
    const u64 key = World::chunkKey(e.cx, e.cz);
    bool had;
    {
        std::shared_lock lock(world_.mutex());
        had = world_.hasChunk(e.cx, e.cz);
    }
    if (had) terrain_->removeChunk(e.cx, e.cz);  // resent, kept, saved or generated locally: replace
    {
        std::unique_lock lock(world_.mutex());
        world_.replaceChunk(e.cx, e.cz, e.firstSy, e.sections);
        // Surface biome (for biome colours): the highest section with blocks.
        for (size_t i = e.sections.size(); i-- > 0;)
            if (e.sections[i].hasBiomes && !e.sections[i].palette.empty() &&
                !(e.sections[i].palette.size() == 1 && e.sections[i].palette[0] == 0)) {
                world_.setChunkBiomes(e.cx, e.cz, e.sections[i].biomes);
                break;
            }
    }
    chunks_.insert(key);
    // Neighbours were meshed with air beyond this edge: refresh their resident sections.
    std::vector<u64> dirty;
    static constexpr int kSides[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& s : kSides)
        if (chunks_.count(World::chunkKey(e.cx + s[0], e.cz + s[1])) || world_.hasChunk(e.cx + s[0], e.cz + s[1]))
            for (int sy = world_.minSectionY(); sy <= world_.maxSectionY(); ++sy)
                dirty.push_back(Lighting::sectionKey(e.cx + s[0], sy, e.cz + s[1]));
    terrain_->addChunk(e.cx, e.cz, dirty);
    const double ms = t.ms();
    applyMs_ += ms;
    maxApplyMs_ = std::max(maxApplyMs_, ms);
    if (!applied_++) {
        LOG("net: first chunk applied (%d, %d)", e.cx, e.cz);
    }
}

void Session::clearWorld() {
    Timer timer;
    // Invalidate jobs before clearing storage, so results built in the old dimension never appear
    // in the new one. World keeps region objects alive for jobs already holding their pointers.
    terrain_->clearChunks();
    {
        std::unique_lock lock(world_.mutex());
        world_.clearChunks();
    }
    chunks_.clear();
    LOG("net: dimension cleared in %.2f ms", timer.ms());
}

void Session::forget(int cx, int cz, bool drop) {
    if (!chunks_.erase(World::chunkKey(cx, cz))) return;
    if (keepForgotten_ && !drop) return;
    {
        std::unique_lock lock(world_.mutex());
        world_.removeChunk(cx, cz);
    }
    terrain_->removeChunk(cx, cz);
}

void Session::setBlock(int x, int y, int z, u16 id, std::unordered_set<u64>& dirty) {
    const u16 old = world_.block(x, y, z);
    if (old == id || !world_.hasChunk(x >> 4, z >> 4) || !world_.setBlock(x, y, z, id)) return;
    const int cx = x >> 4, sy = y >> 4, cz = z >> 4;
    dirty.insert(Lighting::sectionKey(cx, sy, cz));
    if ((x & 15) == 0) dirty.insert(Lighting::sectionKey(cx - 1, sy, cz));
    if ((x & 15) == 15) dirty.insert(Lighting::sectionKey(cx + 1, sy, cz));
    if ((y & 15) == 0) dirty.insert(Lighting::sectionKey(cx, sy - 1, cz));
    if ((y & 15) == 15) dirty.insert(Lighting::sectionKey(cx, sy + 1, cz));
    if ((z & 15) == 0) dirty.insert(Lighting::sectionKey(cx, sy, cz - 1));
    if ((z & 15) == 15) dirty.insert(Lighting::sectionKey(cx, sy, cz + 1));
    // Predict the light now; the server's light update follows and wins.
    const BlockInfo* infos = registry_.infos();
    if (infos[old].opacity != infos[id].opacity || infos[old].emission != infos[id].emission)
        lighting_.blockChanged(x, y, z, dirty);
}

void Session::applyBlocks(const std::vector<BlockChange>& changes, bool server) {
    std::unordered_set<u64> dirty;
    {
        std::unique_lock lock(world_.mutex());
        for (const BlockChange& c : changes) {
            auto it = predictions_.find(posKey(c.x, c.y, c.z));
            if (it != predictions_.end()) {  // our own edit is pending: remember, apply on ack
                it->second.state = c.id;
                if (verbose_)
                    LOG("net: [%.3f] server block %d %d %d = %s (our edit pending, sequence %d)", nowSeconds(), c.x, c.y, c.z,
                        registry_.name(c.id).c_str(), it->second.sequence);
                continue;
            }
            setBlock(c.x, c.y, c.z, c.id, dirty);
            serverChanges_ += server;
            if (verbose_)
                LOG("net: server block %d %d %d = %s (world now %s)", c.x, c.y, c.z, registry_.name(c.id).c_str(),
                    registry_.name(world_.block(c.x, c.y, c.z)).c_str());
        }
    }
    if (!dirty.empty()) terrain_->remesh(std::vector<u64>(dirty.begin(), dirty.end()), false);
}

void Session::acknowledge(int sequence) {
    std::unordered_set<u64> dirty;
    if (verbose_) LOG("net: [%.3f] block_changed_ack %d", nowSeconds(), sequence);
    {
        std::unique_lock lock(world_.mutex());
        ++acks_;
        for (auto it = predictions_.begin(); it != predictions_.end();) {
            if (it->second.sequence > sequence) {
                ++it;
                continue;
            }
            int x, y, z;
            unpackPos(it->first, x, y, z);
            if (world_.block(x, y, z) != it->second.state) {
                ++corrections_;
                LOG("net: server corrected %d %d %d to %s", x, y, z, registry_.name(it->second.state).c_str());
            }
            setBlock(x, y, z, it->second.state, dirty);  // no-op when the prediction was right
            it = predictions_.erase(it);
        }
    }
    if (!dirty.empty()) terrain_->remesh(std::vector<u64>(dirty.begin(), dirty.end()));
}

void Session::applyLight(const LightEvent& e) {
    std::unordered_set<u64> changed;
    {
        std::unique_lock lock(world_.mutex());
        for (const LightSection& s : e.sections)
            if (world_.setLight(e.cx, s.sy, e.cz, s.sky, World::kLightUniform | 0, s.data.empty() ? nullptr : s.data.data()))
                for (int d = 0; d < 7; ++d) {  // faces in neighbouring sections sample this light too
                    static constexpr int kOff[7][3] = {{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
                    changed.insert(Lighting::sectionKey(e.cx + kOff[d][0], s.sy + kOff[d][1], e.cz + kOff[d][2]));
                }
    }
    if (!changed.empty()) terrain_->refresh(std::vector<u64>(changed.begin(), changed.end()));
}

// ClientPacketListener.handleChunksBiomes: the columns' new biomes (the surface biome World keeps: the highest section
// with blocks, as in applyChunk), then every section of them and their neighbours is remeshed for the new tints.
void Session::applyBiomes(const BiomesEvent& e) {
    std::unordered_set<u64> dirty;
    {
        std::unique_lock lock(world_.mutex());
        for (const BiomesEvent::Column& c : e.columns) {
            if (!world_.hasChunk(c.cx, c.cz)) continue;
            for (size_t i = c.sections.size(); i-- > 0;)
                if (world_.section(c.cx, c.firstSy + int(i), c.cz, nullptr)) {
                    world_.setChunkBiomes(c.cx, c.cz, c.sections[i].data());
                    break;
                }
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    for (int sy = world_.minSectionY(); sy <= world_.maxSectionY(); ++sy)
                        dirty.insert(Lighting::sectionKey(c.cx + dx, sy, c.cz + dz));
        }
    }
    if (!dirty.empty()) terrain_->remesh(std::vector<u64>(dirty.begin(), dirty.end()), false);
}

void Session::teleport(const TeleportEvent& e) {
    Player& p = game_->player();
    const float yaw = float(p.yaw * 180.0 / kPi), pitch = float(p.pitch * 180.0 / kPi);
    double pos[3];
    for (int a = 0; a < 3; ++a) {
        pos[a] = ((e.relative >> a) & 1 ? p.pos[a] : 0.0) + e.pos[a];
        p.vel[a] = ((e.relative >> (5 + a)) & 1 ? p.vel[a] : 0.0) + e.delta[a];
    }
    const float newYaw = ((e.relative >> 3) & 1 ? yaw : 0.0f) + e.yaw;
    const float newPitch = std::clamp(((e.relative >> 4) & 1 ? pitch : 0.0f) + e.pitch, -90.0f, 90.0f);
    std::copy(pos, pos + 3, p.pos);
    std::copy(pos, pos + 3, p.prev);
    std::copy(pos, pos + 3, serverPos_);
    haveServerPos_ = true;
    p.yaw = float(newYaw * kPi / 180.0);
    p.pitch = float(newPitch * kPi / 180.0);
    client_.sendAcceptTeleport(e.id, pos, newYaw, newPitch);
}

void Session::tick() {
    // LocalPlayer.tick: the movement keys when they change (the server's sneaking state comes from them).
    const Player& p = game_->player();
    const PlayerInput& in = p.lastInput;
    const int flags = (in.forward > 0 ? 1 : 0) | (in.forward < 0 ? 2 : 0) | (in.strafe > 0 ? 4 : 0) | (in.strafe < 0 ? 8 : 0) |
                      (in.jump ? 16 : 0) | (in.sneak ? 32 : 0) | (in.sprint ? 64 : 0);
    if (flags != sentInput_) {
        Writer w;
        w.u8v(u8(flags));
        client_.sendPlay("player_input", w);
        sentInput_ = flags;
    }
    // LocalPlayer.onUpdateAbilities: flight toggled by double-tapping jump, or ended by landing.
    if (p.flying != sentFlying_) {
        Writer w;
        w.u8v(p.flying ? 2 : 0);
        client_.sendPlay("player_abilities", w);
        sentFlying_ = p.flying;
    }
    // LocalPlayer.sendIsSprintingIfNeeded: ServerboundPlayerCommandPacket START / STOP_SPRINTING.
    if (p.sprinting != sentSprinting_) {
        Writer w;
        w.varInt(entityId_);
        w.varInt(p.sprinting ? 1 : 2);
        w.varInt(0);
        client_.sendPlay("player_command", w);
        sentSprinting_ = p.sprinting;
    }
    sendPosition();
    client_.sendTickEnd();
}

// LocalPlayer.sendChanges / sendPosition: a passenger reports its rotation every tick; otherwise (while the camera
// is our own) the position after a move over 2e-4 or every 20 ticks, the rotation when it changed, else the
// on-ground / horizontal collision flags when those changed.
void Session::sendPosition() {
    const Player& p = game_->player();
    const float yaw = float(p.yaw * 180.0 / kPi), pitch = float(p.pitch * 180.0 / kPi);
    const u8 flags = u8((p.onGround ? 1 : 0) | (p.horizontalCollision ? 2 : 0));
    if (game_->riding()) {
        Writer w;
        w.f32(yaw), w.f32(pitch), w.u8v(flags);
        client_.sendPlay("move_player_rot", w);
        sentRot_[0] = yaw, sentRot_[1] = pitch;
        return;
    }
    if (game_->cameraEntity >= 0) return;  // isControlledCamera
    const double dx = p.pos[0] - sentPos_[0], dy = p.pos[1] - sentPos_[1], dz = p.pos[2] - sentPos_[2];
    ++positionReminder_;
    const bool move = dx * dx + dy * dy + dz * dz > 2.0E-4 * 2.0E-4 || positionReminder_ >= 20;
    const bool rot = yaw != sentRot_[0] || pitch != sentRot_[1];
    if (move && rot) {
        client_.sendMove(p.pos, yaw, pitch, p.onGround, p.horizontalCollision);
    } else if (move) {
        Writer w;
        for (int a = 0; a < 3; ++a) w.f64(p.pos[a]);
        w.u8v(flags);
        client_.sendPlay("move_player_pos", w);
    } else if (rot) {
        Writer w;
        w.f32(yaw), w.f32(pitch), w.u8v(flags);
        client_.sendPlay("move_player_rot", w);
    } else if (lastOnGround_ != p.onGround || lastHorizontal_ != p.horizontalCollision) {
        Writer w;
        w.u8v(flags);
        client_.sendPlay("move_player_status_only", w);
    }
    if (move) std::copy(p.pos, p.pos + 3, sentPos_), positionReminder_ = 0;
    if (rot) sentRot_[0] = yaw, sentRot_[1] = pitch;
    lastOnGround_ = p.onGround, lastHorizontal_ = p.horizontalCollision;
}

int Session::itemFor(u16 block) const {
    if (!block) return -1;
    const std::string& state = registry_.name(block);
    std::string name = state.substr(0, state.find('['));
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (auto it = items_.find(name); it != items_.end()) return it->second;
        // Wall-mounted variants share the item of the standing block (wall_torch -> torch, ...).
        if (size_t w = name.find("_wall_"); w != std::string::npos) name.erase(w, 5);
        else if (size_t v = name.find(":wall_"); v != std::string::npos) name.erase(v + 1, 5);
    }
    return -1;
}

void Session::syncHotbar() {
    const int slot = game_->selectedSlot();
    if (slot != sentSlot_) {
        client_.sendCarriedItem(slot);
        sentSlot_ = slot;
    }
    if (gameMode_ != 1 || !creativeHotbarSync) return;  // only creative players may set inventory contents
    for (int i = 0; i < 9; ++i) {
        const int item = itemFor(game_->hotbarBlock(i));
        if (item == sentItems_[i]) continue;
        client_.sendCreativeSlot(36 + i, item, 1);  // hotbar slots of the inventory menu
        sentItems_[i] = item;
    }
}

void Session::onEdit(const Game::EditAction& a) {
    syncHotbar();
    // The server checks reach from where it believes the player is: report any move since the last tick
    // (as a tick of its own; the server accepts one position per client tick).
    const Player& p = game_->player();
    if (!std::equal(p.pos, p.pos + 3, sentPos_) || float(p.yaw * 180.0 / kPi) != sentRot_[0] ||
        float(p.pitch * 180.0 / kPi) != sentRot_[1])
        tick();
    static constexpr const char* kActions[] = {"start_destroy", "change_destroy_direction", "abort_destroy", "stop_destroy"};
    // ABORT / CHANGE_DESTROY_DIRECTION: plain packets (no prediction sequence).
    if (!a.place && (a.action == Game::kAbortDestroy || a.action == Game::kChangeDestroyDirection)) {
        client_.sendPlayerAction(a.action, a.x, a.y, a.z, a.face, 0);
        if (verbose_)
            LOG("net: [%.3f] player_action %s %d %d %d face %d", nowSeconds(), kActions[a.action], a.x, a.y, a.z, a.face);
        return;
    }
    const int sequence = ++sequence_;
    ++edits_;
    if (verbose_)
        LOG("net: [%.3f] %s %d %d %d face %d -> cell %d %d %d %s (sequence %d%s)", nowSeconds(),
            a.place ? "use_item_on" : (std::string("player_action ") + kActions[a.action & 3]).c_str(), a.x, a.y, a.z, a.face,
            a.at[0], a.at[1], a.at[2], a.place ? registry_.name(a.block).c_str() : "", sequence,
            !a.place && a.predicted ? ", predicted break" : "");
    // Remember what the server last said about the changing cell until it acknowledges the edit.
    if (a.place || a.predicted) {
        const u64 key = posKey(a.at[0], a.at[1], a.at[2]);
        auto it = predictions_.find(key);
        if (it != predictions_.end()) it->second.sequence = sequence;
        else predictions_[key] = {sequence, world_.block(a.at[0], a.at[1], a.at[2])};
    }
    if (!a.place) {  // MultiPlayerGameMode.startPrediction: START / STOP_DESTROY_BLOCK with the sequence
        client_.sendPlayerAction(a.action, a.x, a.y, a.z, a.face, sequence);
        return;
    }
    const float cursor[3] = {float(a.hit[0] - a.x), float(a.hit[1] - a.y), float(a.hit[2] - a.z)};
    client_.sendUseItemOn(a.x, a.y, a.z, a.face, cursor, sequence, a.hand, a.inside);
}

void Session::useItem(int hand) {  // ServerboundUseItemPacket
    syncHotbar();
    const Player& p = game_->player();
    Writer w;
    w.varInt(hand);
    w.varInt(++sequence_);
    w.f32(float(p.yaw * 180.0 / kPi));
    w.f32(float(p.pitch * 180.0 / kPi));
    client_.sendPlay("use_item", w);
    if (verbose_) LOG("net: use_item hand %d (sequence %d)", hand, sequence_);
}

void Session::report() const {
    const ClientStats s = client_.stats();
    LOG("net: connect %.1f ms, login %.1f ms, first chunk %.1f ms after start | %llu chunks decoded: avg %.3f ms, "
        "max %.3f ms | %llu applied: avg %.3f ms, max %.3f ms",
        s.connectMs, s.joinMs, s.firstChunkMs, (unsigned long long)s.chunks, s.chunks ? s.decodeMs / double(s.chunks) : 0.0,
        s.maxDecodeMs, (unsigned long long)applied_, applied_ ? applyMs_ / double(applied_) : 0.0, maxApplyMs_);
    LOG("net: %llu edits sent, %llu acks, %llu corrected by the server, %llu server block changes applied",
        (unsigned long long)edits_, (unsigned long long)acks_, (unsigned long long)corrections_,
        (unsigned long long)serverChanges_);
}

}  // namespace net
