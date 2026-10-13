#include "game/gameplay.h"
#include "ui/mp_hud.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "entity/entities.h"
#include "game/game.h"
#include "game/riding.h"
#include "net/client.h"
#include "ui/item_props.h"
#include "ui/items.h"
#include "ui/ui.h"
#include "world/world.h"

#include <cmath>
#include <cstdlib>
#include <optional>

namespace {

std::vector<std::string> registryNames(const GameReports& reports, const char* key) {
    std::vector<std::string> names;
    const nlohmann::json& reg = reports.registries();
    if (!reg.is_object() || !reg.contains(key)) return names;
    for (const auto& e : reg[key]["entries"].items()) {
        const int id = e.value().value("protocol_id", -1);
        if (id < 0) continue;
        if (size_t(id) >= names.size()) names.resize(size_t(id) + 1);
        names[size_t(id)] = e.key();
    }
    return names;
}

std::string_view shortName(std::string_view n) { return n.starts_with("minecraft:") ? n.substr(10) : n; }

}  // namespace

Gameplay::Gameplay(World& world, BlockRegistry& registry, const GameReports& reports)
    : world_(world), registry_(registry), reports_(reports) {
    itemNames_ = registryNames(reports, "minecraft:item");
    attributeNames_ = registryNames(reports, "minecraft:attribute");
    for (size_t i = 0; i < attributeNames_.size(); ++i)
        if (attributeNames_[i] == "minecraft:scale") scaleAttribute_ = int(i);
    effectNames_ = registryNames(reports, "minecraft:mob_effect");
    const char* v = std::getenv("MCPP_GAMEPLAY_VERBOSE");
    verbose_ = v && *v == '1';
    const std::vector<std::string> components = registryNames(reports, "minecraft:data_component_type");
    for (size_t i = 0; i < components.size(); ++i) {
        if (components[i] == "minecraft:charged_projectiles") chargedProjectilesType_ = int(i);
        if (components[i] == "minecraft:tool") toolType_ = int(i);
        if (components[i] == "minecraft:can_break") canBreakType_ = int(i);
    }
    blockNames_ = registryNames(reports, "minecraft:block");
    soundNames_ = registryNames(reports, "minecraft:sound_event");
    const std::vector<std::string> entityTypes = registryNames(reports, "minecraft:entity_type");
    for (size_t i = 0; i < entityTypes.size(); ++i)
        if (entityTypes[i] == "minecraft:player") playerEntityType_ = int(i);
}

namespace {
// HolderSet<Block> from the network: a tag ("#minecraft:mineable/pickaxe") or block names.
std::vector<std::string> readBlockSet(net::Reader& r, const std::vector<std::string>& blockNames) {
    std::vector<std::string> out;
    const i32 c = r.varInt();
    if (c == 0) {
        out.push_back("#" + std::string(r.str()));
        return out;
    }
    for (i32 i = 0; i < c - 1 && r.ok; ++i) {
        const i32 id = r.varInt();
        if (id >= 0 && size_t(id) < blockNames.size()) out.push_back(blockNames[size_t(id)]);
    }
    return out;
}
}  // namespace

const ItemRules::Tool* Gameplay::heldTool(const ui::ItemStack* s) {
    if (!s) return nullptr;
    if (toolType_ >= 0) {
        if (std::find(s->removed.begin(), s->removed.end(), toolType_) != s->removed.end()) return nullptr;
        for (const ui::ItemStack::Component& c : s->added) {
            if (c.type != toolType_) continue;
            auto it = toolCache_.find(c.data);
            if (it == toolCache_.end()) {  // Tool.STREAM_CODEC
                ItemRules::Tool t;
                net::Reader r(reinterpret_cast<const u8*>(c.data.data()), c.data.size());
                const i32 n = r.varInt();
                for (i32 i = 0; i < n && r.ok; ++i) {
                    ItemRules::Rule rule;
                    rule.blocks = rules_.blocks(readBlockSet(r, blockNames_));
                    if (r.boolean()) rule.speed = r.f32();
                    if (r.boolean()) rule.correct = r.boolean() ? 1 : 0;
                    t.rules.push_back(std::move(rule));
                }
                t.defaultSpeed = r.f32();
                t.damagePerBlock = r.varInt();
                t.canDestroyInCreative = r.boolean();
                if (!r.ok) return rules_.tool(itemName(s->item));
                if (verbose_) LOG("gameplay: held tool component, %zu rules, default speed %.2f", t.rules.size(), t.defaultSpeed);
                it = toolCache_.emplace(c.data, std::move(t)).first;
            }
            return &it->second;
        }
    }
    return rules_.tool(itemName(s->item));
}

bool Gameplay::adventureMayBreak(int x, int y, int z) {
    const ui::ItemStack* s = mainHand();
    if (!s || canBreakType_ < 0) return false;
    const std::string& state = registry_.name(world_.block(x, y, z));
    const std::string block = state.substr(0, state.find('['));
    auto property = [&](const std::string& name) -> std::optional<std::string> {  // the state's property value
        const size_t open = state.find('[');
        if (open == std::string::npos) return std::nullopt;
        const std::string key = name + "=";
        for (size_t p = open + 1; p < state.size();) {
            const size_t end = std::min(state.find(',', p), state.find(']', p));
            if (state.compare(p, key.size(), key) == 0) return state.substr(p + key.size(), end - p - key.size());
            if (end == std::string::npos) break;
            p = end + 1;
        }
        return std::nullopt;
    };
    auto less = [](const std::string& a, const std::string& b) {  // Comparable property values (ints numerically)
        char* ea = nullptr;
        char* eb = nullptr;
        const long ia = std::strtol(a.c_str(), &ea, 10), ib = std::strtol(b.c_str(), &eb, 10);
        if (!a.empty() && !b.empty() && *ea == 0 && *eb == 0) return ia < ib;
        return a < b;
    };
    for (const ui::ItemStack::Component& c : s->added) {
        if (c.type != canBreakType_) continue;
        net::Reader r(reinterpret_cast<const u8*>(c.data.data()), c.data.size());
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {  // AdventureModePredicate.test: any BlockPredicate matches
            bool match = true;
            if (r.boolean() && !rules_.blockIn(rules_.blocks(readBlockSet(r, blockNames_)), block)) match = false;
            if (r.boolean()) {  // StatePropertiesPredicate
                const i32 props = r.varInt();
                for (i32 k = 0; k < props && r.ok; ++k) {
                    const std::string name(r.str());
                    const std::optional<std::string> value = property(name);
                    if (r.boolean()) {
                        const std::string exact(r.str());
                        if (!value || *value != exact) match = false;
                    } else {
                        std::optional<std::string> lo, hi;
                        if (r.boolean()) lo = std::string(r.str());
                        if (r.boolean()) hi = std::string(r.str());
                        if (!value || (lo && less(*value, *lo)) || (hi && less(*hi, *value))) match = false;
                    }
                }
            }
            if (r.boolean()) r.nbt();  // block entity NBT: not known here; the server decides
            if (r.varInt() != 0 || r.varInt() != 0) return true;  // component matchers: not decoded, let the server decide
            if (match && r.ok) return true;
        }
    }
    return false;
}

Gameplay::~Gameplay() = default;

void Gameplay::setEntities(ent::EntityWorld* entities) {
    entities_ = entities;
    if (entities_) entities_->scaleAttribute = scaleAttribute_;
}

double Gameplay::entityAttribute(int entity, std::string_view name, double fallback) const {
    auto it = entityAttributes_.find(entity);
    if (it == entityAttributes_.end()) return fallback;
    auto a = it->second.find(name);
    return a == it->second.end() ? fallback : a->second;
}

bool Gameplay::load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir,
                    const fs::path& componentReports) {
    rulesOk_ = rules_.load(minecraftDir, version, cacheDir, componentReports);
    return rulesOk_;
}

void Gameplay::post(std::function<void()> fn) {
    std::lock_guard lock(mutex_);
    pending_.push_back(std::move(fn));
}

void Gameplay::attach(net::Client& client) {
    client_ = &client;
    if (riding_) riding_->setClient(&client);
    client.onPlayPeek("login", [this](net::Reader& r) {  // ClientboundLoginPacket: our entity id first
        const int id = r.i32v();
        netEntityId_ = id;
        post([this, id] {
            entityId_ = id;
            effects_.clear(), attributes_.clear(), entityAttributes_.clear();
            if (riding_) riding_->dismounted();
            if (game_) {  // a new ClientLevel and player
                game_->border = WorldBorder{}, game_->tickRate = TickRate{};
                game_->clearCooldowns(), game_->cameraEntity = -1;
            }
            if (entities_) entities_->tickRate = TickRate{};
        });
    });
    // ClientboundCooldownPacket: cooldown group, duration in ticks (0 removes it).
    client.onPlayPeek("cooldown", [this](net::Reader& r) {
        std::string group(r.str());
        const int duration = r.varInt();
        if (r.ok) post([this, group = std::move(group), duration] {
            if (verbose_) LOG("gameplay: cooldown %s %d ticks", group.c_str(), duration);
            if (!game_) return;
            if (duration == 0) game_->removeCooldown(group);
            else game_->addCooldown(group, duration);
        });
    });
    // ClientboundGameEventPacket rain events (ClientPacketListener.handleGameEvent): the rain level (isRaining > 0.2).
    client.onPlayPeek("game_event", [this](net::Reader& r) {
        const int type = r.u8v();
        const float param = r.f32();
        if (r.ok && (type == 1 || type == 2 || type == 7))
            post([this, type, param] { rainLevel_ = type == 1 ? 0.0f : type == 2 ? 1.0f : std::clamp(param, 0.0f, 1.0f); });
        // PLAY_ARROW_HIT_SOUND (6), PUFFER_FISH_STING (9), GUARDIAN_ELDER_EFFECT (10, param 1: the curse sound)
        if (r.ok && (type == 6 || type == 9 || (type == 10 && param == 1.0f)))
            post([this, type] {
                audio::AudioSystem* a = audio::instance();
                if (!a || !game_) return;
                const Player& p = game_->player();
                if (type == 6) a->play("minecraft:entity.arrow.hit_player", audio::Category::Player, p.pos[0], p.pos[1] + 1.62, p.pos[2], 0.18f, 0.45f);
                else if (type == 9) a->play("minecraft:entity.puffer_fish.sting", audio::Category::Neutral, p.pos[0], p.pos[1], p.pos[2], 1.0f, 1.0f);
                else a->play("minecraft:entity.elder_guardian.curse", audio::Category::Hostile, p.pos[0], p.pos[1], p.pos[2], 1.0f, 1.0f);
            });
    });
    // ClientboundSetCameraPacket: the entity the view follows (handleSetCamera: only an entity we know).
    client.onPlayPeek("set_camera", [this](net::Reader& r) {
        const int id = r.varInt();
        if (r.ok) post([this, id] {
            if (!game_) return;
            if (id == entityId_) game_->cameraEntity = -1;
            else if (entities_ && entities_->entity(id)) game_->cameraEntity = id;
        });
    });
    // ClientboundPlayerRotationPacket (handleRotatePlayer): yaw / pitch, each absolute or relative; the client answers
    // with its new rotation (ServerboundMovePlayerPacket.Rot, not on the ground, no collision).
    client.onPlayPeek("player_rotation", [this](net::Reader& r) {
        const float yRot = r.f32();
        const bool relativeY = r.boolean();
        const float xRot = r.f32();
        const bool relativeX = r.boolean();
        if (r.ok) post([this, yRot, relativeY, xRot, relativeX] {
            if (!game_) return;
            Player& p = game_->player();
            constexpr float kDeg = 180.0f / 3.14159265f;
            const float yaw = (relativeY ? p.yaw * kDeg : 0.0f) + yRot;
            const float pitch = std::clamp((relativeX ? p.pitch * kDeg : 0.0f) + xRot, -90.0f, 90.0f);
            p.yaw = yaw / kDeg, p.pitch = pitch / kDeg;
            if (!client_) return;
            net::Writer w;
            w.f32(yaw), w.f32(pitch);
            w.u8v(0);
            client_->sendPlay("move_player_rot", w);
        });
    });
    // ClientboundPlayerLookAtPacket (handleLookAt -> Entity.lookAt): turn from our feet or eyes towards a point, or
    // towards an entity's feet or eyes when we know it.
    client.onPlayPeek("player_look_at", [this](net::Reader& r) {
        const int fromEyes = r.varInt();
        double to[3];
        for (double& v : to) v = r.f64();
        int entity = -1, toEyes = 0;
        if (r.boolean()) entity = r.varInt(), toEyes = r.varInt();
        if (r.ok) post([this, fromEyes, x = to[0], y = to[1], z = to[2], entity, toEyes] {
            if (!game_) return;
            double target[3] = {x, y, z};
            if (const ent::Entity* e = entity >= 0 && entities_ ? entities_->entity(entity) : nullptr) {
                std::copy(e->pos, e->pos + 3, target);
                if (toEyes == 1 && e->info) target[1] += e->info->eye;
            }
            Player& p = game_->player();
            const double dx = target[0] - p.pos[0], dy = target[1] - (p.pos[1] + (fromEyes == 1 ? p.eyeHeight() : 0.0));
            const double dz = target[2] - p.pos[2];
            auto wrap = [](float deg) { return deg - 360.0f * std::floor((deg + 180.0f) / 360.0f); };  // Mth.wrapDegrees
            constexpr double kDeg = 180.0 / 3.14159265358979;
            const float pitch = wrap(float(-(std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * kDeg)));
            const float yaw = wrap(float(std::atan2(dz, dx) * kDeg) - 90.0f);
            p.yaw = float(yaw / kDeg), p.pitch = float(pitch / kDeg);
        });
    });
    // ClientboundTickingStatePacket / ClientboundTickingStepPacket (handleTickingState / handleTickingStep): the
    // tick rate manager of the client level, here one copy for the player's ticks and one for the entities'.
    client.onPlayPeek("ticking_state", [this](net::Reader& r) {
        const float rate = r.f32();
        const bool frozen = r.boolean();
        if (r.ok) post([this, rate, frozen] {
            if (verbose_) LOG("gameplay: tick rate %.1f%s", rate, frozen ? ", frozen" : "");
            for (TickRate* t : {game_ ? &game_->tickRate : nullptr, entities_ ? &entities_->tickRate : nullptr})
                if (t) t->rate = rate, t->frozen = frozen;
        });
    });
    client.onPlayPeek("ticking_step", [this](net::Reader& r) {
        const int steps = r.varInt();
        if (r.ok) post([this, steps] {
            for (TickRate* t : {game_ ? &game_->tickRate : nullptr, entities_ ? &entities_->tickRate : nullptr})
                if (t) t->stepTicks = steps;
        });
    });
    // World border (ClientPacketListener.handleInitializeBorder / handleSetBorder*).
    auto border = [this](std::function<void(WorldBorder&)> fn) {
        post([this, fn = std::move(fn)] {
            if (game_) fn(game_->border);
        });
    };
    client.onPlayPeek("initialize_border", [border](net::Reader& r) {
        const double x = r.f64(), z = r.f64(), oldSize = r.f64(), newSize = r.f64();
        const i64 lerpTime = r.varLong();
        const int maxSize = r.varInt(), warningBlocks = r.varInt(), warningTime = r.varInt();
        if (r.ok) border([=](WorldBorder& b) {
            b.setCenter(x, z);
            if (lerpTime > 0) b.lerpSizeBetween(oldSize, newSize, lerpTime);
            else b.setSize(newSize);
            b.absoluteMaxSize = maxSize;
            b.warningBlocks = warningBlocks, b.warningTime = warningTime;
        });
    });
    client.onPlayPeek("set_border_center", [border](net::Reader& r) {
        const double x = r.f64(), z = r.f64();
        if (r.ok) border([=](WorldBorder& b) { b.setCenter(x, z); });
    });
    client.onPlayPeek("set_border_lerp_size", [border](net::Reader& r) {
        const double oldSize = r.f64(), newSize = r.f64();
        const i64 lerpTime = r.varLong();
        if (r.ok) border([=](WorldBorder& b) { b.lerpSizeBetween(oldSize, newSize, lerpTime); });
    });
    client.onPlayPeek("set_border_size", [border](net::Reader& r) {
        const double size = r.f64();
        if (r.ok) border([=](WorldBorder& b) { b.setSize(size); });
    });
    client.onPlayPeek("set_border_warning_delay", [border](net::Reader& r) {
        const int time = r.varInt();
        if (r.ok) border([=](WorldBorder& b) { b.warningTime = time; });
    });
    client.onPlayPeek("set_border_warning_distance", [border](net::Reader& r) {
        const int blocks = r.varInt();
        if (r.ok) border([=](WorldBorder& b) { b.warningBlocks = blocks; });
    });
    client.onPlayPeek("respawn", [this](net::Reader&) {  // a new player entity: no effects, default attributes
        post([this] {
            effects_.clear(), attributes_.clear();
            if (game_) game_->clearCooldowns(), game_->cameraEntity = -1;
            if (riding_) riding_->dismounted();
        });
    });
    // ClientboundSetPassengersPacket: VarInt vehicle, VarInt array of passengers.
    client.onPlayPeek("set_passengers", [this](net::Reader& r) {
        const int vehicle = r.varInt();
        const i32 n = r.varInt();
        std::vector<int> ids;
        for (i32 i = 0; i < n && r.ok && i < 64; ++i) ids.push_back(r.varInt());
        if (!r.ok) return;
        post([this, vehicle, ids = std::move(ids)] {
            if (riding_) riding_->passengers(vehicle, ids, entityId_);
        });
    });
    client.onPlayPeek("move_vehicle", [this](net::Reader& r) {  // ClientboundMoveVehiclePacket: the server's correction
        double pos[3];
        for (double& c : pos) c = r.f64();
        const float yRot = r.f32(), xRot = r.f32();
        if (!r.ok) return;
        post([this, p0 = pos[0], p1 = pos[1], p2 = pos[2], yRot, xRot] {
            const double pos[3] = {p0, p1, p2};
            if (riding_) riding_->moveVehicle(pos, yRot, xRot);
        });
    });
    client.onPlayPeek("remove_entities", [this](net::Reader& r) {
        const i32 n = r.varInt();
        std::vector<int> ids;
        for (i32 i = 0; i < n && r.ok && i < 4096; ++i) ids.push_back(r.varInt());
        post([this, ids = std::move(ids)] {
            for (int id : ids) {
                entityAttributes_.erase(id);
                if (riding_ && id == riding_->vehicle()) riding_->dismounted();
            }
        });
    });
    client.onPlayPeek("update_mob_effect", [this](net::Reader& r) {
        const int entity = r.varInt(), id = r.varInt(), amplifier = r.varInt();
        if (!r.ok || id < 0 || size_t(id) >= effectNames_.size()) return;
        post([this, entity, name = std::string(shortName(effectNames_[size_t(id)])), amplifier] {
            if (entity == entityId_) effects_[name] = amplifier;
        });
    });
    client.onPlayPeek("remove_mob_effect", [this](net::Reader& r) {
        const int entity = r.varInt(), id = r.varInt();
        if (!r.ok || id < 0 || size_t(id) >= effectNames_.size()) return;
        post([this, entity, name = std::string(shortName(effectNames_[size_t(id)]))] {
            if (entity == entityId_) effects_.erase(name);
        });
    });
    client.onPlayPeek("update_attributes", [this](net::Reader& r) {
        const int entity = r.varInt();
        const i32 n = r.varInt();
        std::vector<std::pair<std::string, double>> values;
        for (i32 i = 0; i < n && r.ok; ++i) {
            const int id = r.varInt();
            const double base = r.f64();
            double add = 0, mulBase = 0, mulTotal = 1;
            const i32 mods = r.varInt();
            for (i32 m = 0; m < mods && r.ok; ++m) {  // AttributeModifier: id, amount, operation
                // The sprint boost is the client's own (LocalPlayer.setSprinting): the movement adds it locally.
                const bool sprint = r.str() == "minecraft:sprinting";
                const double amount = r.f64();
                const int op = r.varInt();
                if (sprint) continue;
                if (op == 0) add += amount;
                else if (op == 1) mulBase += amount;
                else mulTotal *= 1 + amount;
            }
            const double v = base + add;
            if (id >= 0 && size_t(id) < attributeNames_.size())
                values.push_back({std::string(shortName(attributeNames_[size_t(id)])), (v + v * mulBase) * mulTotal});
        }
        if (!r.ok) return;
        post([this, entity, values = std::move(values)] {
            auto& target = entity == entityId_ ? attributes_ : entityAttributes_[entity];
            for (const auto& [k, v] : values) target[k] = v;
        });
    });
    client.onPlayPeek("set_health", [this](net::Reader& r) {  // ClientboundSetHealthPacket: health, food, saturation
        const float health = r.f32();
        const int food = r.varInt();
        if (r.ok) post([this, food, health] {
            food_ = food;
            if (!game_) return;
            if (health < health_ && health > 0) game_->hurt();  // LocalPlayer.hurtTo: a drop starts the hurt tilt
            game_->dead = health <= 0;
            health_ = health;
        });
    });
    // ClientboundHurtAnimationPacket: entity id, yaw (LivingEntity.animateHurt; Player keeps the direction).
    client.onPlayPeek("hurt_animation", [this](net::Reader& r) {
        const int id = r.varInt();
        const float yaw = r.f32();
        if (r.ok) post([this, id, yaw] {
            if (game_ && id == entityId_) game_->hurt(yaw);
        });
    });
    client.onPlayPeek("damage_event", [this](net::Reader& r) {  // ClientboundDamageEventPacket: entity id, damage type
        const int id = r.varInt();
        const int type = r.varInt();
        if (r.ok) post([this, id, type] {
            if (!game_ || id != entityId_) return;
            game_->hurt();
            // LivingEntity.handleDamageEvent -> Player.getHurtSound: the damage type's effects (DamageEffects), played
            // here as the server leaves the player out (Player.playSound excepts itself).
            std::string effects = "hurt";
            if (client_) {
                const std::vector<net::Client::RegistryEntry> types = client_->registryEntries("minecraft:damage_type");
                NbtValue nbt;
                if (type >= 0 && size_t(type) < types.size() &&
                    nbtReadNetwork(reinterpret_cast<const u8*>(types[size_t(type)].nbt.data()), types[size_t(type)].nbt.size(), nbt))
                    if (NbtValue* e = nbt.find("effects"); e && e->type == TagString) effects = e->s;
            }
            const char* sound = effects == "drowning"  ? "minecraft:entity.player.hurt_drown"
                                : effects == "burning" ? "minecraft:entity.player.hurt_on_fire"
                                : effects == "poking"  ? "minecraft:entity.player.hurt_sweet_berry_bush"
                                : effects == "freezing" ? "minecraft:entity.player.hurt_freeze"
                                                        : "minecraft:entity.player.hurt";
            playOwnVoice(sound);
        });
    });
    client.onPlayPeek("entity_event", [this](net::Reader& r) {  // ours: 3 death (LivingEntity.handleEntityEvent)
        const int id = r.i32v(), event = r.u8v();
        if (r.ok && event == 3) post([this, id] {
            if (game_ && id == entityId_) playOwnVoice("minecraft:entity.player.death");  // Player.getDeathSound
        });
    });
    // Our own synched data: DATA_LIVING_ENTITY_FLAGS (index 8; bit 1 using an item, bit 2 the off hand), in front of
    // which only Entity's values (0..7) can come. Serializers: 0 byte, 1 int, 3 float, 5 / 6 component (NBT),
    // 8 boolean, 20 pose (VarInt).
    client.onPlayPeek("set_entity_data", [this](net::Reader& r) {
        if (r.varInt() != netEntityId_.load()) return;
        for (int guard = 0; guard < 16 && r.ok; ++guard) {
            const int index = r.u8v();
            if (index == 0xFF) break;
            const int type = r.varInt();
            if (index == 0 && type == 0) {  // Entity shared flags: bit 7 gliding (the server's word on it)
                const int flags = r.u8v();
                if (r.ok) post([this, flags] {
                    if (game_) game_->player().fallFlying = (flags & 0x80) != 0;
                });
                continue;
            }
            if (index == 8 && type == 0) {
                const int flags = r.u8v();
                if (r.ok) post([this, flags] {
                    if (!game_) return;
                    game_->player().spinFlag = (flags & 4) != 0;  // LivingEntity.isAutoSpinAttack (the SPIN_ATTACK pose)
                    const bool serverUsing = (flags & 1) != 0;
                    const int hand = (flags & 2) ? 1 : 0;
                    if (serverUsing && !game_->use.active) startUsing(hand);
                    else if (!serverUsing && game_->use.active) game_->use.active = false;
                });
                break;
            }
            if (index > 8) break;
            switch (type) {
                case 0: r.u8v(); break;
                case 1: case 20: r.varInt(); break;
                case 3: r.f32(); break;
                case 5: r.nbt(); break;
                case 6: if (r.boolean()) r.nbt(); break;
                case 8: r.boolean(); break;
                default: return;  // a value this reader does not know: stop
            }
        }
    });
    // ClientboundBlockDestructionPacket: another player's crack (ClientLevel.destroyBlockProgress).
    client.onPlayPeek("block_destruction", [this](net::Reader& r) {
        const int id = r.varInt();
        int x, y, z;
        r.blockPos(x, y, z);
        const int progress = r.u8v();
        if (!r.ok) return;
        post([this, id, x, y, z, progress] {
            if (verbose_) LOG("gameplay: block_destruction by %d at %d %d %d stage %d", id, x, y, z, progress);
            if (game_ && id != entityId_) game_->destroyBlockProgress(id, x, y, z, progress);
        });
    });
}

void Gameplay::setPlayer(Game& game, ui::Ui& ui) {
    game_ = &game;
    ui_ = &ui;
    if (rulesOk_) game.destroyProgress = [this](int x, int y, int z, u16 state) { return destroyProgress(x, y, z, state); };
    game.heldIdentity = [this]() -> u64 {  // ItemStack.isSameItemSameComponents
        const ui::ItemStack* s = mainHand();
        if (!s) return 0;
        u64 h = 1469598103934665603ull ^ u64(u32(s->item));
        auto mix = [&](const void* p, size_t n) {
            for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const u8*>(p)[i]) * 1099511628211ull;
        };
        for (const auto& c : s->added) mix(&c.type, sizeof(c.type)), mix(c.data.data(), c.data.size());
        for (int t : s->removed) mix(&t, sizeof(t));
        return h;
    };
    game.predictItemUse = [this](int hand, const RayHit* target) { return predictItemUse(hand, target); };
    game.predictUseOn = [this](int hand, const RayHit& hit) { return predictUseOn(hand, hit); };
    game.handHasItem = [this](int hand) { return (hand == 0 ? mainHand() : offHand()) != nullptr; };
    game.handInTag = [this](int hand, const char* tag) {
        const ui::ItemStack* s = hand == 0 ? mainHand() : offHand();
        const std::vector<int>* t = s && ui_ ? ui_->items().tag("minecraft:item", tag) : nullptr;
        return t && std::find(t->begin(), t->end(), s->item) != t->end();
    };
    game.handOnCooldown = [this](int hand) { return onCooldown(hand == 0 ? mainHand() : offHand()); };
    game.predictRelease = [this] { releaseUse(); };
    game.onStabEffects = [this] {  // PiercingWeapon.makeSound
        const ui::ItemStack* s = mainHand();
        const std::string sound = s ? weaponSound(*s, "piercing_weapon") : std::string();
        if (audio::AudioSystem* a = audio::instance(); a && !sound.empty())
            a->play(sound, audio::Category::Player, game_->player().pos[0], game_->player().pos[1], game_->player().pos[2], 1.0f, 1.0f);
    };
    riding_ = std::make_unique<Riding>(*this, game, entities_, rules_);
    riding_->setClient(client_);
    game.player().onStartFallFlying = [this] {  // ServerboundPlayerCommandPacket START_FALL_FLYING
        if (!client_) return;
        net::Writer w;
        w.varInt(entityId_);
        w.varInt(6);
        w.varInt(0);
        client_->sendPlay("player_command", w);
    };
    game.onUseTick = [this] { tickUse(); };
    game.physics().rules = &rules_;
    // Level.getEntityCollisions for the player (Entity.canCollideWith): boats, living shulkers, adult happy ghasts
    // from above (or holding still); not the vehicle it rides.
    game.physics().entityColliders = [this](const AABB& area, std::vector<AABB>& out) {
        if (!entities_ || !game_) return;
        const Player& p = game_->player();
        for (const auto& [id, e] : entities_->entities()) {
            if (!e.info || e.dying || (riding_ && id == riding_->vehicle())) continue;
            const std::string& n = e.info->name;
            double w, h;
            ent::entitySize(e, w, h);
            const AABB b{{e.pos[0] - w / 2, e.pos[1], e.pos[2] - w / 2}, {e.pos[0] + w / 2, e.pos[1] + h, e.pos[2] + w / 2}};
            const bool collides = n.ends_with("_boat") || n.ends_with("_raft") || (n == "minecraft:shulker" && e.health > 0) ||
                                  (n == "minecraft:happy_ghast" && !e.baby && (p.pos[1] >= b.max[1] || e.staysStill));
            AABB grown = area;
            for (int a = 0; a < 3; ++a) grown.min[a] -= 1e-7, grown.max[a] += 1e-7;
            if (collides && b.intersects(grown)) out.push_back(b);
        }
    };
    // Entity.push(Entity) from the entities that push the local player on the client: living entities touching it
    // (LivingEntity.pushEntities; not bats, parrots or armor stands), boats within 0.2 blocks (AbstractBoat.tick).
    game.entityPush = [this](Player& p) {
        if (!entities_ || p.noClip || game_->dead || p.onClimbable(game_->physics())) return;  // LivingEntity.isPushable
        // EntitySelector.pushableBy on the client: only our own team's rule counts, NEVER pushes nothing.
        if (ui::teamCollisionRule(game_->playerName) == 1) return;
        const AABB body = p.box();
        for (const auto& [id, e] : entities_->entities()) {
            if (!e.info || e.dying || id == entityId_) continue;
            const std::string& n = e.info->name;
            const bool boat = n.ends_with("_boat") || n.ends_with("_raft");
            const bool living = e.category == ent::Category::Living || e.category == ent::Category::Player;
            if ((!living && !boat) || n == "minecraft:bat" || n == "minecraft:parrot" || n == "minecraft:armor_stand") continue;
            if (riding_ && id == riding_->vehicle()) continue;
            double w, h;
            ent::entitySize(e, w, h);
            AABB b{{e.pos[0] - w / 2, e.pos[1], e.pos[2] - w / 2}, {e.pos[0] + w / 2, e.pos[1] + h, e.pos[2] + w / 2}};
            if (boat) b.min[0] -= 0.2, b.max[0] += 0.2, b.min[1] += 0.01, b.max[1] -= 0.01, b.min[2] -= 0.2, b.max[2] += 0.2;
            if (!b.intersects(body)) continue;
            if (boat && !(body.min[1] <= e.pos[1])) continue;  // AbstractBoat.push: not one standing higher than the boat
            double xa = p.pos[0] - e.pos[0], za = p.pos[2] - e.pos[2];
            double dd = std::max(std::abs(xa), std::abs(za));  // Mth.absMax
            if (dd < double(0.01f)) continue;
            dd = std::sqrt(dd);
            xa /= dd, za /= dd;
            const double pow = std::min(1.0, 1.0 / dd);
            p.vel[0] += xa * pow * double(0.05f), p.vel[2] += za * pow * double(0.05f);
        }
    };
    // LivingEntity.checkAutoSpinAttack: a living entity in the swept box.
    game.player().spinAttackHits = [this](const AABB& swept) {
        if (!entities_) return false;
        for (const auto& [id, e] : entities_->entities()) {
            if (!e.info || e.dying || id == entityId_ ||
                (e.category != ent::Category::Living && e.category != ent::Category::Player))
                continue;
            double w, h;
            ent::entitySize(e, w, h);
            const AABB b{{e.pos[0] - w / 2, e.pos[1], e.pos[2] - w / 2}, {e.pos[0] + w / 2, e.pos[1] + h, e.pos[2] + w / 2}};
            if (b.intersects(swept)) return true;
        }
        return false;
    };
    game.canDestroyInCreative = [this] {  // Item.canDestroyBlock: the tool component's can_destroy_blocks_in_creative
        const ItemRules::Tool* t = heldTool(mainHand());
        return !t || t->canDestroyInCreative;
    };
    game.adventureMayBreak = [this](int x, int y, int z) { return adventureMayBreak(x, y, z); };
    game.entityView = [this](int id, CameraState& c) {  // Camera.setup on an entity: its eyes, its view rotation
        const ent::Entity* e = entities_ ? entities_->entity(id) : nullptr;
        if (!e || !e->info) return false;
        const float a = entities_->partialTick();
        c.x = e->prevPos[0] + (e->pos[0] - e->prevPos[0]) * a;
        c.y = e->prevPos[1] + (e->pos[1] - e->prevPos[1]) * a + e->info->eye;
        c.z = e->prevPos[2] + (e->pos[2] - e->prevPos[2]) * a;
        // LivingEntity.getViewYRot: the head's rotation; other entities their own.
        const bool living = e->category == ent::Category::Living || e->category == ent::Category::Player;
        const float y0 = living ? e->prevHeadYaw : e->prevYaw, y1 = living ? e->headYaw : e->yaw;
        const float yaw = y0 + (y1 - y0 - 360.0f * std::floor((y1 - y0 + 180.0f) / 360.0f)) * a;  // Mth.rotLerp
        const float pitch = e->prevPitch + (e->pitch - e->prevPitch) * a;
        c.yaw = yaw * 3.14159265f / 180.0f, c.pitch = pitch * 3.14159265f / 180.0f;
        return true;
    };
}

bool Gameplay::onCooldown(const ui::ItemStack* s) const {  // ItemCooldowns.isOnCooldown
    return s && game_ && ui_ && game_->cooldownPercent(ui_->items().cooldownGroup(*s), 0.0f) > 0.0f;
}

void Gameplay::frame() {
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(pending_);
    }
    for (auto& fn : pending) fn();
    if (riding_) riding_->frame();
    if (!game_) return;
    // The movement's server-driven values: attributes (effects and equipment change them on the server), effects
    // the movement reads itself, and equipment (elytra, leather boots, an attached firework rocket).
    Player::Attributes& a = game_->player().attr;
    a.moveSpeed = attribute("movement_speed", 0.1);
    a.jumpStrength = attribute("jump_strength", 0.42);
    a.gravity = attribute("gravity", 0.08);
    a.stepHeight = attribute("step_height", 0.6);
    a.sneakSpeed = attribute("sneaking_speed", 0.3);
    a.waterEfficiency = attribute("water_movement_efficiency", 0.0);
    a.movementEfficiency = attribute("movement_efficiency", 0.0);
    a.frictionModifier = attribute("friction_modifier", 1.0), a.airDragModifier = attribute("air_drag_modifier", 1.0);
    a.bounciness = attribute("bounciness", 0.0);
    a.safeFallDistance = attribute("safe_fall_distance", 3.0);
    a.fallDamageMultiplier = attribute("fall_damage_multiplier", 1.0);
    game_->player().scale = std::clamp(attribute("scale", 1.0), 0.0625, 16.0);  // LivingEntity.getScale
    {  // Camera.setup's third-person distance: camera_distance x getScale, the player's or its mount's if larger
        double dist = attribute("camera_distance", 4.0) * attribute("scale", 1.0);
        if (auto it = entityAttributes_.find(game_->cameraEntity); game_->cameraEntity >= 0 && it != entityAttributes_.end()) {
            // spectating a living entity: its own (with the default 4 for what the server did not send)
            auto value = [&](const char* k, double d) { auto f = it->second.find(k); return f != it->second.end() ? f->second : d; };
            dist = value("camera_distance", 4.0) * value("scale", 1.0);
        } else if (game_->cameraEntity >= 0) {
            dist = 4.0;
        }
        if (auto it = entityAttributes_.find(game_->vehicleId); game_->vehicleId >= 0 && it != entityAttributes_.end()) {
            auto value = [&](const char* k, double d) { auto f = it->second.find(k); return f != it->second.end() ? f->second : d; };
            dist = std::max(dist, value("camera_distance", 4.0) * value("scale", 1.0));
        }
        game_->cameraDistance = dist;
    }
    a.jumpBoost = effect("jump_boost"), a.slowFalling = effect("slow_falling"), a.levitation = effect("levitation");
    a.dolphinsGrace = effect("dolphins_grace");
    a.blind = effect("blindness") >= 0;
    a.weaving = effect("weaving") >= 0;
    const ui::ItemStack* chest = ui_ ? ui_->playerSlot(6) : nullptr;
    a.canGlide = chest && rules_.glider(itemName(chest->item)) && (chest->maxDamage <= 0 || chest->damage < chest->maxDamage - 1);
    const ui::ItemStack* feet = ui_ ? ui_->playerSlot(8) : nullptr;
    a.powderSnowWalker = feet && itemName(feet->item) == "minecraft:leather_boots";
    game_->attackSpeed = float(attribute("attack_speed", 4.0));
    heldWeapon();
    // Player.blockInteractionRange / entityInteractionRange (creative adds 0.5 / 2 as attribute modifiers).
    const bool creative = game_->gameMode == 1;
    game_->blockReach = attribute("block_interaction_range", creative ? 5.0 : 4.5);
    game_->entityReach = attribute("entity_interaction_range", creative ? 5.0 : 3.0);
    {  // Player.tick: another item in the main hand resets the attack strength
        const ui::ItemStack* held = mainHand();
        const int item = held ? held->item : -1;
        if (item != lastMainItem_) game_->attackStrengthTicker = 0;
        lastMainItem_ = item;
    }
    game_->crosshairOnLiving = game_->crosshairOnMenuEntity = false, game_->crosshairEntity = -1;
    if (entities_) {  // Minecraft.crosshairPickEntity: a living entity in front of the targeted block
        const CameraState c = game_->camera();
        const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
        const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
        // LocalPlayer.pick: entities up to the block hit (or the longer reach), out of the entity reach a miss
        const double block = game_->pickBlockDistance();
        const double limit = block >= 0 ? block : std::max(game_->blockReach, game_->entityReach);
        ent::PickResult pick;
        if (game_->attackMaxReach >= 0) {  // LocalPlayer.raycastHitResult: the held item's attack_range picks
            const double* v = game_->player().vel;
            pick = entities_->pickAttackRange(eye, dir, game_->attackMinReach, game_->attackMaxReach, game_->attackMargin,
                                              v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2],
                                              [this](const double o[3], const double d[3], double max) {
                                                  const RayHit h = game_->physics().raycast(o, d, max);
                                                  return h.hit ? h.distance : max;
                                              });
        }
        game_->entityBlocksTarget = false;
        if (pick.id < 0) {
            pick = entities_->pick(eye, dir, limit);
            if (pick.id >= 0 && !(pick.distance < game_->entityReach)) game_->entityBlocksTarget = true, pick.id = -1;
        }
        game_->crosshairEntity = pick.id;
        if (pick.id >= 0)
            if (const ent::Entity* e = entities_->entity(pick.id); e && e->info) {
                const std::string& n = e->info->name;
                game_->crosshairOnMenuEntity = n.ends_with("chest_minecart") || n == "minecraft:hopper_minecart" ||
                                               n.ends_with("_chest_boat") || n.ends_with("_chest_raft");
            }
        if (pick.id >= 0)
            if (const ent::Entity* e = entities_->entity(pick.id))
                game_->crosshairOnLiving = (e->category == ent::Category::Living || e->category == ent::Category::Player) &&
                                           !e->dying && e->health > 0 &&
                                           // Hud: AttackRange.isInRange of the hit (reaches widened by the margin)
                                           (game_->attackMaxReach < 0 ||
                                            (pick.distance >= game_->attackMinReach - game_->attackMargin &&
                                             pick.distance <= game_->attackMaxReach + game_->attackMargin));
    }
    if (verbose_) {  // MCPP_GAMEPLAY_VERBOSE=1: movement state changes
        const Player& p = game_->player();
        const int state = int(p.pose) | (p.swimming ? 8 : 0) | (p.fallFlying ? 16 : 0) | (game_->use.active ? 32 : 0);
        if (state != loggedState_)
            LOG("gameplay: [%.3f] pose %d swimming %d gliding %d using %d at %.2f %.2f %.2f", nowSeconds(), int(p.pose),
                int(p.swimming), int(p.fallFlying), int(game_->use.active), p.pos[0], p.pos[1], p.pos[2]);
        loggedState_ = state;
    }
    a.rocketBoost = false;
    if (entities_ && game_->player().fallFlying)
        for (const auto& [id, e] : entities_->entities())
            if (e.attachedTo == entityId_ && e.info && e.info->name == "minecraft:firework_rocket") a.rocketBoost = true;
}

int Gameplay::effect(std::string_view name) const {
    auto it = effects_.find(name);
    return it == effects_.end() ? -1 : it->second;
}

double Gameplay::attribute(std::string_view name, double fallback) const {
    auto it = attributes_.find(name);
    return it == attributes_.end() ? fallback : it->second;
}

const ui::ItemStack* Gameplay::mainHand() const {
    return ui_ && game_ ? ui_->playerSlot(36 + game_->selectedSlot()) : nullptr;
}

const ui::ItemStack* Gameplay::offHand() const { return ui_ ? ui_->playerSlot(45) : nullptr; }

int Gameplay::chargeType(const ui::ItemStack* s) const {
    if (!s || chargedProjectilesType_ < 0) return 0;
    for (const auto& c : s->added) {
        if (c.type != chargedProjectilesType_) continue;
        // ChargedProjectiles: a list of item stacks (VarInt size, then count, item id, patch...).
        net::Reader r(reinterpret_cast<const u8*>(c.data.data()), c.data.size());
        if (r.varInt() <= 0) return 0;
        r.varInt();  // count
        const int item = r.varInt();
        return r.ok && itemName(item) == "minecraft:firework_rocket" ? 2 : 1;
    }
    return 0;
}

bool Gameplay::hasProjectile() const {
    if (game_ && game_->gameMode == 1) return true;  // Player.getProjectile: infinite materials
    if (!ui_) return false;
    for (int slot = 9; slot <= 45; ++slot)
        if (const ui::ItemStack* s = ui_->playerSlot(slot)) {
            const std::string n = itemName(s->item);
            if (n == "minecraft:arrow" || n == "minecraft:spectral_arrow" || n == "minecraft:tipped_arrow") return true;
        }
    return false;
}

bool Gameplay::startUsing(int hand) {
    const ui::ItemStack* s = hand == 0 ? mainHand() : offHand();
    if (!s || !game_) return false;
    const ItemRules::Use u = stackUse(*s);
    Game::ItemUse& use = game_->use;
    use.active = true;
    use.hand = hand;
    use.item = s->item;
    use.anim = int(u.anim);
    use.duration = use.remaining = u.duration;
    use.speed = u.speed;
    use.canSprint = u.canSprint;
    return true;
}

namespace {
const std::string* patchOf(const ui::ItemStack& s, int type) {
    if (type < 0) return nullptr;
    for (const ui::ItemStack::Component& c : s.added)
        if (c.type == type) return &c.data;
    return nullptr;
}
bool removedFrom(const ui::ItemStack& s, int type) {
    return type >= 0 && std::find(s.removed.begin(), s.removed.end(), type) != s.removed.end();
}
}  // namespace

int Gameplay::componentType(const char* name) {
    auto it = componentTypes_.find(name);
    if (it != componentTypes_.end()) return it->second;
    const int id = ui_ ? ui_->items().componentType(name) : -1;
    if (ui_) componentTypes_.emplace(name, id);
    return id;
}

// The stack's use as Item.getUseAnimation / getUseDuration, Consumable and UseEffects see it: the item's defaults
// (component reports and the item classes) with the stack's own patch of consumable, food, blocks_attacks,
// kinetic_weapon and use_effects.
// Player.interactOn as the client predicts it: Entity.interact (riding, trading, feeding, milking, shearing, leashing,
// equipping...) then ItemStack.interactLivingEntity (name tags, dyes, saddles...). 1 consumes, 0 passes, -1 an empty
// hand (passes). An approximation by entity type and held item; spectators pass.
int Gameplay::entityInteraction(const ent::Entity& e, int hand) {
    if (!e.info || !game_) return 1;
    if (game_->gameMode == 3) return 0;
    const ui::ItemStack* held = hand == 0 ? mainHand() : offHand();
    const std::string item = held && !held->empty() ? std::string(shortName(itemName(held->item))) : std::string();
    const std::string n(shortName(e.info->name));
    const bool sneaking = game_->player().sneaking, empty = item.empty();
    auto pass = [&] { return empty ? -1 : 0; };
    auto inTag = [&](const char* tag) {
        if (empty || !ui_) return false;
        const std::vector<int>* t = ui_->items().tag("minecraft:item", tag);
        return t && std::find(t->begin(), t->end(), held->item) != t->end();
    };
    // Vehicles and decorations (Entity.interact overrides).
    if (n.ends_with("_boat") || n.ends_with("_raft") || n == "boat") return n.find("chest") != std::string::npos || !sneaking ? 1 : pass();
    if (n.ends_with("minecart")) {
        if (n == "chest_minecart" || n == "hopper_minecart" || n == "command_block_minecart") return 1;
        if (n == "furnace_minecart") return inTag("minecraft:furnace_minecart_fuel") ? 1 : pass();
        if (n == "tnt_minecart") return item == "flint_and_steel" || item == "fire_charge" ? 1 : pass();
        return !sneaking ? 1 : pass();
    }
    if (n == "item_frame" || n == "glow_item_frame") return e.itemId >= 0 || !empty ? 1 : pass();
    if (n == "interaction" || n == "leash_knot") return 1;
    if (n == "armor_stand") return !empty || e.equipment[0] >= 0 || e.equipment[1] >= 0 || e.equipment[2] >= 0 || e.equipment[3] >= 0 ||
                                           e.equipment[4] >= 0 || e.equipment[5] >= 0 ? 1 : pass();
    if (e.category != ent::Category::Living) return pass();
    // Mob.checkAndHandleImportantInteractions / Leashable: leads, spawn eggs of the same type, name tags.
    if (item == "lead" || item == n + "_spawn_egg") return 1;
    if (item == "name_tag" && held->hasCustomName) return 1;
    // Mob.mobInteract by type.
    if (n == "villager" || n == "wandering_trader") return item == "villager_spawn_egg" ? pass() : 1;
    const bool horse = n == "horse" || n == "donkey" || n == "mule" || n == "skeleton_horse" || n == "zombie_horse" || n == "camel" ||
                       n == "camel_husk" || n == "llama" || n == "trader_llama";
    if (horse && !e.baby) return 1;  // riding, the inventory, feeding, equipping
    if ((n == "pig" || n == "strider") && !e.baby && e.equipment[7] >= 0 && !sneaking) return 1;  // ride the saddled pig
    if (n == "happy_ghast" && !e.baby && e.equipment[6] >= 0 && !sneaking) return 1;  // ride the harnessed ghast
    if (n == "allay") return 1;
    if ((n == "wolf" || n == "cat" || n == "parrot" || n == "nautilus") && e.tame) return 1;  // sitting, healing, dyeing
    if ((n == "cow" || n == "goat" || n == "mooshroom") && item == "bucket" && !e.baby) return 1;
    if (n == "mooshroom" && (item == "bowl" || item == "shears") && !e.baby) return 1;
    if (n == "sheep" && item == "shears" && !e.baby && !(e.wool & 0x10)) return 1;
    if (n == "sheep" && item.ends_with("_dye") && !(e.wool & 0x10)) return 1;
    if ((n == "snow_golem" || n == "bogged") && item == "shears") return 1;
    if (n == "creeper" && (item == "flint_and_steel" || item == "fire_charge")) return 1;
    if (n == "iron_golem" && item == "iron_ingot") return 1;
    if (n == "armadillo" && item == "brush") return 1;
    if (item == "water_bucket" && (n == "axolotl" || n == "tadpole" || n == "cod" || n == "salmon" || n == "pufferfish" || n == "tropical_fish"))
        return 1;
    // Animal.mobInteract: its food (the <type>_food item tag) feeds, breeds or grows it; saddles / body armour equip.
    if (inTag(("minecraft:" + n + "_food").c_str())) return 1;
    if (!empty && held && (item == "saddle" || item.ends_with("_horse_armor") || item.ends_with("_carpet") || item == "wolf_armor" ||
                           item.ends_with("_harness")))
        return 1;
    return pass();
}

// Player.playSound for our own hurt / death: at the player, PLAYERS, volume 1, pitch 1 +- 0.2 (the server excepts us).
void Gameplay::playOwnVoice(const char* sound) {
    audio::AudioSystem* a = audio::instance();
    if (!a || !game_) return;
    static std::mt19937 rng(0x5EED);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const Player& p = game_->player();
    a->play(sound, audio::Category::Player, p.pos[0], p.pos[1], p.pos[2], 1.0f, (u(rng) - u(rng)) * 0.2f + 1.0f);
}

ItemRules::Use Gameplay::stackUse(const ui::ItemStack& s) {
    const std::string name = itemName(s.item);
    ItemRules::Use u = rules_.use(name);
    const ItemRules::Components& d = rules_.components(name);
    auto present = [&](const char* type, bool fallback) {
        const int t = componentType(type);
        return patchOf(s, t) ? true : removedFrom(s, t) ? false : fallback;
    };
    if (!d.classUse) {
        if (const std::string* c = patchOf(s, componentType("consumable"))) {  // Consumable.STREAM_CODEC
            net::Reader r(reinterpret_cast<const u8*>(c->data()), c->size());
            const float seconds = r.f32();
            const int anim = r.varInt();
            const int holder = r.varInt();  // Holder<SoundEvent>: 0 inline (identifier, optional range), else id + 1
            std::string sound;
            if (holder == 0) {
                sound = std::string(r.str());
                if (r.boolean()) r.f32();
            } else if (size_t(holder - 1) < soundNames_.size()) {
                sound = soundNames_[size_t(holder - 1)];
            }
            const bool particles = r.boolean();
            if (r.ok) {
                u.anim = ItemRules::Anim(std::clamp(anim, 0, int(ItemRules::Anim::Spear)));
                u.duration = int(seconds * 20.0f);
                u.consumable = true, u.particles = particles;
                if (!sound.empty()) u.sound = sound;
            }
        } else if (!present("consumable", d.consumable)) {  // Item.getUseAnimation without a consumable
            const bool blocks = present("blocks_attacks", d.blocksAttacks), kinetic = present("kinetic_weapon", d.kinetic);
            u.consumable = false;
            u.anim = blocks ? ItemRules::Anim::Block : kinetic ? ItemRules::Anim::Spear : ItemRules::Anim::None;
            u.duration = blocks || kinetic ? 72000 : 0;
        }
    }
    if (patchOf(s, componentType("food"))) u.food = true, u.alwaysEat = s.foodAlwaysEdible;
    else if (removedFrom(s, componentType("food"))) u.food = false;
    if (const std::string* c = patchOf(s, componentType("use_effects"))) {  // can_sprint, interact_vibrations, speed
        net::Reader r(reinterpret_cast<const u8*>(c->data()), c->size());
        const bool sprint = r.boolean();
        r.boolean();
        const float speed = r.f32();
        if (r.ok) u.canSprint = sprint, u.speed = speed;
    } else if (removedFrom(s, componentType("use_effects"))) {
        u.canSprint = false, u.speed = 0.2f;  // UseEffects.DEFAULT
    }
    return u;
}

// LivingEntity.getEquipmentSlotForItem for the player: the stack's equippable (patched, else the item's default)
// slot, else the main hand. allowed: isEquippableInSlot (the equippable's allowed entities include players);
// swappable: Equippable.swappable.
int Gameplay::equipSlot(const ui::ItemStack& s, bool& allowed, bool* swappable) {
    const ItemRules::Components& d = rules_.components(itemName(s.item));
    allowed = true;
    if (swappable) *swappable = true;
    const int type = componentType("equippable");
    if (const std::string* c = patchOf(s, type)) {  // Equippable.STREAM_CODEC
        net::Reader r(reinterpret_cast<const u8*>(c->data()), c->size());
        const int slot = r.varInt();
        if (r.varInt() == 0) {  // equip sound: inline
            r.str();
            if (r.boolean()) r.f32();
        }
        if (r.boolean()) r.str();  // asset id
        if (r.boolean()) r.str();  // camera overlay
        if (r.boolean()) {         // allowed entities: a tag (not resolved: no players) or entity type ids
            const i32 n = r.varInt();
            if (n == 0) {
                r.str();
                allowed = false;
            } else {
                allowed = false;
                for (i32 i = 0; i < n - 1 && r.ok; ++i)
                    if (r.varInt() == playerEntityType_) allowed = true;
            }
        }
        r.boolean();  // dispensable
        const bool swap = r.boolean();
        if (!r.ok) return 0;
        if (swappable) *swappable = swap;
        return slot;
    }
    if (removedFrom(s, type) || d.equipSlot < 0) return 0;
    allowed = d.equipForPlayer;
    if (swappable) *swappable = d.swappable;
    return d.equipSlot;
}

// Level.isRainingAt for the player's position or the top of its box (Entity.isInRain): raining, open sky, a biome
// that rains rather than snows.
bool Gameplay::rainingAt(int x, int y, int z) const {
    if (rainLevel_ <= 0.2f) return false;
    if (motionBlocking) {  // the MOTION_BLOCKING heightmap: nothing above
        if (motionBlocking(x, z) > y) return false;
    } else {
        for (int yy = y; yy < y + 384; ++yy) {
            const BlockInfo& b = registry_.infos()[world_.block(x, yy, z)];
            if (b.collisionCount || b.fluid || b.waterlogged) return false;
            if (yy > y + 2 && world_.block(x, yy, z) == 0 && yy > 320) break;
        }
    }
    // Biome.getPrecipitationAt: the biome's has_precipitation, its height-adjusted temperature and the frozen noise
    return precipitationAt ? precipitationAt(x, y, z) == 1 : true;
}

bool Gameplay::inWaterOrRain() const {  // Entity.isInWaterOrRain
    const Player& p = game_->player();
    const int x = int(std::floor(p.pos[0])), y = int(std::floor(p.pos[1])), z = int(std::floor(p.pos[2]));
    return p.inWater || rainingAt(x, y, z) || rainingAt(x, int(std::floor(p.box().max[1])), z);
}

// EnchantmentHelper.getTridentSpinAttackStrength: the stack's enchantments with a trident_spin_attack_strength effect
// (riptide: 1.5 + 0.75 per level above the first).
float Gameplay::riptideStrength(const ui::ItemStack& s) const {
    // EnchantmentHelper.getTridentSpinAttackStrength: the trident_spin_attack_strength effects (riptide)
    return ui_ ? ui::enchantmentValue(ui_->items(), s, "minecraft:trident_spin_attack_strength", 0.0f) : 0.0f;
}

// Minecraft.startUseItem -> MultiPlayerGameMode.useItem -> ItemStack.use for one hand: kUseConsume when the use was
// consumed (started using, instant success), kUseFail when it failed (both end startUseItem; a failed use still lets
// the other hand try), kUsePass when nothing happened. Uses that start over time are predicted (the server's living
// flags confirm or end them); instant uses apply their use_cooldown; buckets place / pick up fluids.
int Gameplay::predictItemUse(int hand, const RayHit* target) {
    const ui::ItemStack* s = hand == 0 ? mainHand() : offHand();
    if (!s || !game_ || game_->gameMode == 3 || onCooldown(s)) return Game::kUsePass;
    const std::string name(shortName(itemName(s->item)));
    const ItemRules::Use u = stackUse(*s);
    const bool creative = game_->gameMode == 1;
    auto start = [&] {
        startUsing(hand);
        return Game::kUseConsume;
    };
    auto instant = [&] {  // ItemStack.applyAfterUseComponentSideEffects: use_cooldown of an instantly used item
        const ItemRules::Components& d = rules_.components(itemName(s->item));
        float seconds = d.cooldownSeconds;
        if (const std::string* c = patchOf(*s, componentType("use_cooldown"))) {
            net::Reader r(reinterpret_cast<const u8*>(c->data()), c->size());
            seconds = r.f32();
        } else if (removedFrom(*s, componentType("use_cooldown"))) {
            seconds = -1;
        }
        if (seconds > 0 && ui_) game_->addCooldown(ui_->items().cooldownGroup(*s), int(seconds * 20.0f));
        return Game::kUseConsume;
    };
    // Item classes with their own use.
    if (name == "bow") return creative || hasProjectile() ? start() : Game::kUseFail;
    if (name == "crossbow") return chargeType(s) != 0 ? Game::kUseConsume : hasProjectile() ? start() : Game::kUseFail;
    if (name == "trident") {
        if (s->maxDamage > 0 && s->damage >= s->maxDamage - 1) return Game::kUseFail;  // nextDamageWillBreak
        if (riptideStrength(*s) > 0 && !inWaterOrRain()) return Game::kUseFail;
        return start();
    }
    const Player& self = game_->player();
    auto localSound = [&](std::string_view sound, audio::Category c, float volume, float pitch) {  // heard by the user only
        if (audio::AudioSystem* a = audio::instance()) a->play(sound, c, self.pos[0], self.pos[1], self.pos[2], volume, pitch);
    };
    if (name == "spyglass") {  // SpyglassItem.use: player.playSound(SPYGLASS_USE)
        localSound("minecraft:item.spyglass.use", audio::Category::Player, 1.0f, 1.0f);
        return start();
    }
    if (name == "goat_horn") {  // InstrumentItem.use: the instrument's sound (RECORDS, range / 16) and its cooldown
        if (!ui_) return Game::kUsePass;
        const auto& list = ui_->items().instruments();
        int id = s->instrument;
        if (id < 0)  // the item's default component
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].id == "minecraft:ponder_goat_horn") id = int(i);
        if (id < 0 || size_t(id) >= list.size()) return Game::kUseFail;
        const ui::Items::RegistryInfo& ins = list[size_t(id)];
        const int result = start();
        game_->use.duration = game_->use.remaining = int(std::floor(ins.useDuration * 20.0f));  // InstrumentItem.getUseDuration
        if (!ins.soundEvent.empty()) localSound(ins.soundEvent, audio::Category::Record, ins.range / 16.0f, 1.0f);
        game_->addCooldown(ui_->items().cooldownGroup(*s), int(std::floor(ins.useDuration * 20.0f)));
        return result;
    }
    if (name == "bundle" || name.ends_with("_bundle")) return start();
    if (name == "lily_pad" || name == "frogspawn") {  // PlaceOnWaterBlockItem.use: the cell above a SOURCE_ONLY hit
        const CameraState c = game_->camera();
        const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
        const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
        RayHit hit = game_->physics().raycast(eye, dir, game_->blockReach);
        const double limit = hit.hit ? hit.distance : game_->blockReach;
        for (double t = 0; t < limit; t += 0.01) {  // a fluid source before the block
            const double py = eye[1] + dir[1] * t;
            const int x = int(std::floor(eye[0] + dir[0] * t)), y = int(std::floor(py)), z = int(std::floor(eye[2] + dir[2] * t));
            const Physics::Fluid f = game_->physics().fluid(x, y, z);
            if (!f.type || !f.source || py > y + game_->physics().fluidHeight(x, y, z, f)) continue;
            hit = RayHit{};
            hit.hit = true, hit.x = x, hit.y = y, hit.z = z, hit.distance = t;
            hit.face = dir[1] < 0 && py >= y + game_->physics().fluidHeight(x, y, z, f) - 0.02 ? 1 : 0;
            hit.point[0] = eye[0] + dir[0] * t, hit.point[1] = py, hit.point[2] = eye[2] + dir[2] * t;
            break;
        }
        if (!hit.hit) return Game::kUsePass;
        hit.y += 1;  // withPosition(getBlockPos().above())
        return game_->placeFromUse(hand, hit);
    }
    if (name == "glass_bottle") {  // BottleItem.use: a water source in reach fills it (BOTTLE_FILL, the user's own)
        const CameraState c = game_->camera();
        const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
        const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
        const RayHit blockHit = game_->physics().raycast(eye, dir, game_->blockReach);
        const double limit = blockHit.hit ? blockHit.distance : game_->blockReach;
        for (double t = 0; t < limit; t += 0.01) {  // ClipContext.Fluid.SOURCE_ONLY
            const int x = int(std::floor(eye[0] + dir[0] * t)), y = int(std::floor(eye[1] + dir[1] * t)), z = int(std::floor(eye[2] + dir[2] * t));
            const Physics::Fluid f = game_->physics().fluid(x, y, z);
            if (!f.type || !f.source || eye[1] + dir[1] * t > y + game_->physics().fluidHeight(x, y, z, f)) continue;
            if (registry_.name(world_.block(x, y, z)).starts_with("minecraft:lava")) return Game::kUsePass;
            localSound("minecraft:item.bottle.fill", audio::Category::Neutral, 1.0f, 1.0f);
            return Game::kUseConsume;
        }
        return Game::kUsePass;
    }
    if (name == "bucket" || name == "water_bucket" || name == "lava_bucket" || name.ends_with("_bucket") && name != "milk_bucket" &&
        name != "powder_snow_bucket")
        return predictBucket(hand, name);
    if (name == "ender_pearl" || name == "snowball" || name == "egg" || name == "blue_egg" || name == "brown_egg" ||
        name == "wind_charge" || name == "experience_bottle" || name == "splash_potion" || name == "lingering_potion" ||
        name == "fishing_rod" || name == "writable_book" || name == "written_book" || name == "knowledge_book" || name == "map" ||
        name == "ender_eye")
        return instant();
    if (name == "firework_rocket") return game_->player().fallFlying ? instant() : Game::kUsePass;
    if (name == "carrot_on_a_stick" || name == "warped_fungus_on_a_stick") return Game::kUsePass;  // steering: riding.cpp
    // Item.use: consumables, swappable equipment, shields, kinetic weapons.
    if (u.consumable) {
        // Consumable.canConsume -> Player.canEat: invulnerable (creative), can_always_eat, or hungry.
        if (u.food && !(creative || u.alwaysEat || food_ < 20)) return Game::kUseFail;
        return u.duration > 0 ? start() : instant();
    }
    bool allowed, swappable;
    const int slot = equipSlot(*s, allowed, &swappable);
    if (slot != 0 && swappable) {  // Equippable.swapWithEquipmentSlot
        if (!allowed || slot == 6 || slot == 7) return Game::kUsePass;
        static constexpr int kMenuSlot[6] = {-1, 8, 7, 6, 5, 45};  // feet .. head, off hand
        const ui::ItemStack* worn = ui_ ? ui_->playerSlot(kMenuSlot[slot]) : nullptr;
        if (worn && worn->sameItemSameComponents(*s)) return Game::kUseFail;
        return instant();
    }
    if (u.anim == ItemRules::Anim::Spear) {  // KineticWeapon: the use sound, heard by the user only here
        const std::string sound = weaponSound(*s, "kinetic_weapon");
        if (audio::AudioSystem* a = audio::instance(); a && !sound.empty())
            a->play(sound, audio::Category::Player, game_->player().pos[0], game_->player().pos[1], game_->player().pos[2], 1.0f, 1.0f);
        return start();
    }
    if (u.anim == ItemRules::Anim::Block) return start();
    (void)target;
    return Game::kUsePass;
}

// BucketItem.use on the client: an empty bucket picks up the fluid source it looks at (or empties a waterlogged
// block), a filled one places its fluid in front of the targeted block (or waterlogs a block that can hold water).
int Gameplay::predictBucket(int hand, const std::string& name) {
    (void)hand;
    Player& p = game_->player();
    const CameraState c = game_->camera();
    const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
    const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
    const double reach = game_->blockReach;
    const Physics& physics = game_->physics();
    const RayHit block = physics.raycast(eye, dir, reach);
    const double blockT = block.hit ? block.distance : reach;
    // The user's bucket sounds (the server plays them with the player as the exception).
    auto sound = [&](const char* id, audio::Category cat, double x, double y, double z) {
        if (audio::AudioSystem* a = audio::instance()) a->play(id, cat, x, y, z, 1.0f, 1.0f);
    };
    if (name == "bucket") {  // ClipContext.Fluid.SOURCE_ONLY: the first source (or block) along the ray
        for (double t = 0; t < blockT; t += 0.01) {
            const int x = int(std::floor(eye[0] + dir[0] * t)), y = int(std::floor(eye[1] + dir[1] * t)), z = int(std::floor(eye[2] + dir[2] * t));
            const Physics::Fluid f = physics.fluid(x, y, z);
            if (!f.type || !f.source || eye[1] + dir[1] * t > y + physics.fluidHeight(x, y, z, f)) continue;
            const u16 state = world_.block(x, y, z);
            // BucketItem.use: player.playSound(the pickup's getPickupSound) at the player
            const bool lava = registry_.name(state).starts_with("minecraft:lava");
            if (!registry_.property(state, "waterlogged").empty()) {  // SimpleWaterloggedBlock.pickupBlock
                game_->predictBlock(x, y, z, registry_.withProperty(state, "waterlogged", "false"));
                sound("minecraft:item.bucket.fill", audio::Category::Player, p.pos[0], p.pos[1], p.pos[2]);
                return Game::kUseConsume;
            }
            if (!registry_.infos()[state].fluid) return Game::kUseFail;  // kelp, seagrass: not a BucketPickup
            game_->predictBlock(x, y, z, 0);  // LiquidBlock.pickupBlock
            sound(lava ? "minecraft:item.bucket.fill_lava" : "minecraft:item.bucket.fill", audio::Category::Player, p.pos[0], p.pos[1],
                  p.pos[2]);
            return Game::kUseConsume;
        }
        return block.hit ? Game::kUseFail : Game::kUsePass;
    }
    if (!block.hit) return Game::kUsePass;
    const bool water = name != "lava_bucket";
    const u16 fluidState = registry_.idFor(water ? "minecraft:water[level=0]" : "minecraft:lava[level=0]");
    auto emptySound = [&](int x, int y, int z) {  // BucketItem / MobBucketItem.playEmptySound at the placed block
        const char* id = !water ? "minecraft:item.bucket.empty_lava" : "minecraft:item.bucket.empty";
        audio::Category cat = audio::Category::Block;
        if (name == "cod_bucket" || name == "salmon_bucket" || name == "pufferfish_bucket" || name == "tropical_fish_bucket")
            id = "minecraft:item.bucket.empty_fish", cat = audio::Category::Neutral;
        else if (name == "axolotl_bucket") id = "minecraft:item.bucket.empty_axolotl", cat = audio::Category::Neutral;
        else if (name == "tadpole_bucket") id = "minecraft:item.bucket.empty_tadpole", cat = audio::Category::Neutral;
        sound(id, cat, x + 0.5, y + 0.5, z + 0.5);
    };
    auto tryPlace = [&](int x, int y, int z) {  // BucketItem.emptyContents
        const u16 state = world_.block(x, y, z);
        const BlockInfo& target = registry_.infos()[state];
        if (water && p.waterEvaporates && (target.replaceable || state == 0)) {
            // WATER_EVAPORATES: a fizz and eight puffs of large smoke instead of water.
            if (audio::AudioSystem* a = audio::instance()) {
                const double pos[3] = {x + 0.5, y + 0.5, z + 0.5};
                a->play("minecraft:block.fire.extinguish", audio::Category::Block, pos[0], pos[1], pos[2], 0.5f,
                        2.6f + (randf() - randf()) * 0.8f);
            }
            for (int i = 0; i < 8 && game_->onParticle; ++i) {
                const double pos[3] = {x + randf(), y + randf(), z + randf()}, vel[3] = {0, 0, 0};
                game_->onParticle("minecraft:large_smoke", pos, vel);
            }
            return true;
        }
        const std::string_view logged = registry_.property(state, "waterlogged");
        if (water && logged == "false") {  // LiquidBlockContainer.placeLiquid
            game_->predictBlock(x, y, z, registry_.withProperty(state, "waterlogged", "true"));
            emptySound(x, y, z);
            return true;
        }
        const BlockInfo& b = registry_.infos()[state];
        if (!b.replaceable && state != 0) return false;
        game_->predictBlock(x, y, z, fluidState);
        emptySound(x, y, z);
        return true;
    };
    const int fx = block.x + (block.face == 4 ? -1 : block.face == 5 ? 1 : 0), fy = block.y + (block.face == 0 ? -1 : block.face == 1 ? 1 : 0),
              fz = block.z + (block.face == 2 ? -1 : block.face == 3 ? 1 : 0);
    const bool container = water && registry_.property(world_.block(block.x, block.y, block.z), "waterlogged") == "false";
    const bool ok = container ? tryPlace(block.x, block.y, block.z) : (tryPlace(fx, fy, fz));
    (void)p;
    return ok ? Game::kUseConsume : Game::kUseFail;
}

// ItemStack.useOn for items that are not blocks: the uses that act on the clicked block on the client side consume
// the click (the other hand gets no turn); the rest pass to the item's own use.
int Gameplay::predictUseOn(int hand, const RayHit& hit) {
    const ui::ItemStack* s = hand == 0 ? mainHand() : offHand();
    if (!s || !game_) return Game::kUsePass;
    const std::string name(shortName(itemName(s->item)));
    const std::string& state = registry_.name(world_.block(hit.x, hit.y, hit.z));
    const std::string block(shortName(state.substr(0, state.find('['))));
    const bool airAbove = world_.block(hit.x, hit.y + 1, hit.z) == 0;
    // Uses that take one of the stack (the others damage the tool).
    game_->useOnTakesItem = name == "fire_charge" || name.ends_with("_spawn_egg") || name == "armor_stand" || name == "item_frame" ||
                            name == "glow_item_frame" || name == "painting" || name == "end_crystal" ||
                            name.ends_with("minecart") || name == "honeycomb" || name == "ender_eye";
    if (name == "flint_and_steel") {  // FlintAndSteelItem.useOn: only the user plays the strike (the server skips them)
        const bool lightable = (block == "campfire" || block == "soul_campfire" || block.ends_with("candle") || block.ends_with("candle_cake")) &&
                               state.find("lit=false") != std::string::npos;
        static constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
        const int fx = hit.x + (lightable ? 0 : kDir[hit.face][0]), fy = hit.y + (lightable ? 0 : kDir[hit.face][1]),
                  fz = hit.z + (lightable ? 0 : kDir[hit.face][2]);
        if (lightable || world_.block(fx, fy, fz) == 0)  // BaseFireBlock.canBePlacedAt: an empty cell
            if (audio::AudioSystem* a = audio::instance())
                a->play("minecraft:item.flintandsteel.use", audio::Category::Block, fx + 0.5, fy + 0.5, fz + 0.5, 1.0f, randf() * 0.4f + 0.8f);
        return Game::kUseConsume;
    }
    if (name == "shears" && (block == "kelp" || block == "weeping_vines" || block == "twisting_vines" || block == "cave_vines") &&
        registry_.property(world_.block(hit.x, hit.y, hit.z), "age") != "25") {  // ShearsItem.useOn: a growing plant's head
        if (audio::AudioSystem* a = audio::instance())
            a->play("minecraft:block.growing_plant.crop", audio::Category::Block, hit.x + 0.5, hit.y + 0.5, hit.z + 0.5, 1.0f, 1.0f);
        return Game::kUseConsume;
    }
    if (name == "fire_charge" || name.ends_with("_spawn_egg") || name == "armor_stand" ||
        name == "item_frame" || name == "glow_item_frame" || name == "painting")
        return Game::kUseConsume;
    if (name == "brush") {  // BrushItem.useOn: starts brushing whatever it hits
        startUsing(hand);
        return Game::kUseConsume;
    }
    if (name == "end_crystal") return block == "obsidian" || block == "bedrock" ? Game::kUseConsume : Game::kUseFail;
    if (name.ends_with("minecart")) return block.find("rail") != std::string::npos ? Game::kUseConsume : Game::kUseFail;
    // BlockTransformer.transformBlock: the user hears the transform's sound and sees its particle level event (both
    // played with the player as the exception, so only here).
    auto transformed = [&](const char* sound, int levelEvent) {
        if (audio::AudioSystem* a = audio::instance())
            a->play(sound, audio::Category::Block, hit.x + 0.5, hit.y + 0.5, hit.z + 0.5, 1.0f, 1.0f);
        if (levelEvent && game_->onLevelEvent) game_->onLevelEvent(levelEvent, hit.x, hit.y, hit.z, 0);
        return Game::kUseConsume;
    };
    if (name.ends_with("_hoe") && hit.face != 0 && airAbove &&  // block_transformer/hoe: tilling
        (block == "grass_block" || block == "dirt_path" || block == "dirt" || block == "coarse_dirt" || block == "rooted_dirt"))
        return transformed("minecraft:item.hoe.till", 0);
    if (name.ends_with("_shovel") && hit.face != 0 && airAbove &&  // block_transformer/shovel: paths
        (block == "grass_block" || block == "dirt" || block == "podzol" || block == "coarse_dirt" || block == "mycelium" ||
         block == "rooted_dirt"))
        return transformed("minecraft:item.shovel.flatten", 0);
    if (name.ends_with("_axe") && !block.starts_with("stripped_") &&  // block_transformer/axe: stripping, scraping, unwaxing
        (block.ends_with("_log") || block.ends_with("_wood") || block.ends_with("_stem") || block.ends_with("_hyphae") ||
         block == "bamboo_block" || block.find("copper") != std::string::npos)) {
        if (block.find("copper") == std::string::npos) return transformed("minecraft:item.axe.strip", 0);
        if (block.starts_with("waxed_")) return transformed("minecraft:item.axe.wax_off", 3004);
        if (block.starts_with("exposed_") || block.starts_with("weathered_") || block.starts_with("oxidized_"))
            return transformed("minecraft:item.axe.scrape", 3005);
        return Game::kUseConsume;
    }
    if (name == "honeycomb" && block.find("copper") != std::string::npos && !block.starts_with("waxed_"))  // HoneycombItem
        return transformed("minecraft:item.honeycomb.wax_on", 3003);
    if (name == "ender_eye" && block == "end_portal_frame" && registry_.property(world_.block(hit.x, hit.y, hit.z), "eye") == "false")
        return Game::kUseConsume;
    return Game::kUsePass;
}

// A weapon component's "sound" (kinetic_weapon / piercing_weapon): the stack's patch, else the item's default.
std::string Gameplay::weaponSound(const ui::ItemStack& s, const char* type) {
    const ItemRules::Components& d = rules_.components(itemName(s.item));
    std::string sound = std::string(type) == "kinetic_weapon" ? d.kineticSound : d.piercingSound;
    const int t = componentType(type);
    if (removedFrom(s, t)) return {};
    const std::string* c = patchOf(s, t);
    if (!c) return sound;
    net::Reader r(reinterpret_cast<const u8*>(c->data()), c->size());
    auto holder = [&]() -> std::string {  // Holder<SoundEvent>: 0 inline (identifier, optional range), else id + 1
        const int h = r.varInt();
        if (h == 0) {
            std::string n(r.str());
            if (r.boolean()) r.f32();
            return n;
        }
        return size_t(h - 1) < soundNames_.size() ? soundNames_[size_t(h - 1)] : std::string();
    };
    if (std::string(type) == "piercing_weapon") {  // dealsKnockback, dismounts, optional sound, optional hit sound
        r.boolean(), r.boolean();
        sound = r.boolean() ? holder() : std::string();
    } else {  // contact cooldown, delay, three optional conditions, forward movement, multiplier, optional sound
        r.varInt(), r.varInt();
        for (int i = 0; i < 3; ++i)
            if (r.boolean()) r.varInt(), r.f32(), r.f32();
        r.f32(), r.f32();
        sound = r.boolean() ? holder() : std::string();
    }
    return r.ok ? sound : std::string();
}

// The main hand's weapon components for Minecraft.startAttack (defaults with the stack's patch): piercing_weapon,
// minimum_attack_charge, attack_range (the player's: creative or survival reaches, plus the hitbox margin) and the
// attack_animation duration with Haste / Conduit Power / Mining Fatigue (LivingEntity.getModifiedSwingDuration).
ItemRules::Components Gameplay::handComponents(const ui::ItemStack* s) {
    ItemRules::Components d = s ? rules_.components(itemName(s->item)) : ItemRules::Components{};
    if (s) {
        auto read = [&](const char* type, auto fn) {
            const int t = componentType(type);
            if (const std::string* c = patchOf(*s, t)) {
                net::Reader r(reinterpret_cast<const u8*>(c->data()), c->size());
                fn(r, true);
            } else if (removedFrom(*s, t)) {
                net::Reader r(nullptr, 0);
                fn(r, false);
            }
        };
        read("piercing_weapon", [&](net::Reader&, bool on) { d.piercing = on; });
        read("minimum_attack_charge", [&](net::Reader& r, bool on) { d.minimumAttackCharge = on ? r.f32() : 0.0f; });
        read("attack_range", [&](net::Reader& r, bool on) {
            d.hasAttackRange = on;
            for (int i = 0; i < 6 && on; ++i) d.attackRange[i] = r.f32();
        });
        read("attack_animation", [&](net::Reader& r, bool on) {  // SwingAnimation: type, duration (default whack, 6)
            d.attackSwingType = on ? r.varInt() : 1;
            d.attackSwingTicks = on ? r.varInt() : 6;
        });
        read("interact_animation", [&](net::Reader& r, bool on) {
            d.interactSwingType = on ? r.varInt() : 1;
            d.interactSwingTicks = on ? r.varInt() : 6;
        });
        read("kinetic_weapon", [&](net::Reader& r, bool on) {  // contact cooldown, delay, three conditions, movement...
            d.kineticValid = on;
            if (!on) return;
            r.varInt();
            d.kineticDelay = r.varInt();
            int* durations[3] = {&d.kineticDismount, &d.kineticKnockback, &d.kineticDamage};
            for (int* t : durations) {
                *t = 0;
                if (r.boolean()) *t = r.varInt(), r.f32(), r.f32();
            }
            d.kineticForward = r.f32();
        });
    }
    return d;
}

void Gameplay::heldWeapon() {
    const ItemRules::Components d = handComponents(mainHand());
    const ItemRules::Components off = handComponents(offHand());
    game_->heldPiercing = d.piercing;
    game_->heldMinimumCharge = d.minimumAttackCharge;
    const bool creative = game_->gameMode == 1;
    game_->attackMinReach = d.hasAttackRange ? double(d.attackRange[creative ? 2 : 0]) : 0.0;
    game_->attackMaxReach = d.hasAttackRange ? double(d.attackRange[creative ? 3 : 1]) : -1.0;
    game_->attackMargin = d.hasAttackRange ? double(d.attackRange[4]) : 0.0;
    const int haste = effect("haste"), conduit = effect("conduit_power"), fatigue = effect("mining_fatigue");
    auto modified = [&](int swing) {  // MobEffectUtil.getDigSpeedAmplification / Mining Fatigue
        if (haste >= 0 || conduit >= 0) return swing - (1 + std::max(haste, conduit));
        return fatigue >= 0 ? swing + (1 + fatigue) * 2 : swing;
    };
    game_->swingTicks = modified(d.attackSwingTicks);
    game_->useSwingTicks = modified(d.interactSwingTicks);
    game_->swingType = d.attackSwingType;
    game_->useSwingTicksOf[0] = game_->useSwingTicks, game_->useSwingTicksOf[1] = modified(off.interactSwingTicks);
    game_->useSwingTypeOf[0] = d.interactSwingType, game_->useSwingTypeOf[1] = off.interactSwingType;
    const ItemRules::Components* hands[2] = {&d, &off};
    for (int h = 0; h < 2; ++h) {
        const ItemRules::Components& k = *hands[h];
        game_->kinetic[h] = {k.kineticValid, k.kineticDelay, k.kineticDismount, k.kineticKnockback, k.kineticDamage, k.kineticForward};
    }
}

void Gameplay::kineticHit() {
    if (!game_ || (game_->kineticHitTicks >= 0 && game_->kineticHitTicks <= 10)) return;
    game_->kineticHitTicks = 0;
    // KineticWeapon.makeLocalHitSound: the used item's hit sound.
    const ui::ItemStack* s = game_->use.active ? (game_->use.hand == 0 ? mainHand() : offHand()) : nullptr;
    if (!s) return;
    const std::string& sound = rules_.components(itemName(s->item)).kineticHitSound;
    if (audio::AudioSystem* a = audio::instance(); a && !sound.empty())
        a->play(sound, audio::Category::Player, game_->player().pos[0], game_->player().pos[1], game_->player().pos[2], 1.0f, 1.0f);
}

// Item.releaseUsing on the client: a riptide trident held for 10 ticks in water or rain launches the player.
void Gameplay::releaseUse() {
    Game::ItemUse& use = game_->use;
    const ui::ItemStack* s = use.hand == 0 ? mainHand() : offHand();
    if (s && s->item == use.item && shortName(itemName(s->item)) == "spyglass")  // SpyglassItem.stopUsing
        if (audio::AudioSystem* a = audio::instance())
            a->play("minecraft:item.spyglass.stop_using", audio::Category::Player, game_->player().pos[0], game_->player().pos[1],
                    game_->player().pos[2], 1.0f, 1.0f);
    if (!s || s->item != use.item || shortName(itemName(s->item)) != "trident" || use.ticksUsing() < 10) return;
    if (s->maxDamage > 0 && s->damage >= s->maxDamage - 1) return;  // nextDamageWillBreak
    const float strength = riptideStrength(*s);
    if (strength <= 0 || !inWaterOrRain() || (riding_ && riding_->vehicle() >= 0)) return;
    if (verbose_) LOG("gameplay: riptide %.2f", strength);
    game_->player().riptide(game_->physics(), strength);
}

// LivingEntity.updatingUsingItem / updateUsingItem (before the count down) and ItemStack.onUseTick.
void Gameplay::tickUse() {
    Game::ItemUse& use = game_->use;
    const ui::ItemStack* s = use.hand == 0 ? mainHand() : offHand();
    if (!s || s->item != use.item) {  // ItemStack.isSameItem: another item in that hand ends the use
        use.active = false;
        return;
    }
    const ItemRules::Use u = rules_.use(itemName(s->item));
    if (shortName(itemName(s->item)) == "brush") return tickBrush();
    if (!u.consumable) return;
    // Consumable.shouldEmitParticlesAndSounds: after 21.875 % of the use, every 4 ticks.
    const int used = use.duration - use.remaining;
    if (!(used > int(float(use.duration) * 0.21875f) && use.remaining % 4 == 0)) return;
    const Player& p = game_->player();
    const bool drink = u.anim == ItemRules::Anim::Drink;
    if (u.particles && game_->onItemParticle) {  // LivingEntity.spawnItemParticles(stack, 5)
        const double yaw = p.yaw, pitch = p.pitch;  // radians, like the entity's yRot / xRot
        auto rot = [&](double v[3]) {  // Vec3.xRot(-xRot) then yRot(-yRot)
            const double cx = std::cos(-pitch), sx = std::sin(-pitch);
            const double y = v[1] * cx + v[2] * sx, z = v[2] * cx - v[1] * sx;
            v[1] = y, v[2] = z;
            const double cy = std::cos(-yaw), sy = std::sin(-yaw);
            const double x = v[0] * cy + v[2] * sy, z2 = v[2] * cy - v[0] * sy;
            v[0] = x, v[2] = z2;
        };
        for (int i = 0; i < 5; ++i) {
            double d[3] = {(randf() - 0.5) * 0.1, randf() * 0.1 + 0.1, 0.0};
            rot(d);
            double q[3] = {(randf() - 0.5) * 0.3, -randf() * 0.6 - 0.3, 0.6};
            rot(q);
            const double pos[3] = {q[0] + p.pos[0], q[1] + p.pos[1] + p.eyeHeight(), q[2] + p.pos[2]};
            const double vel[3] = {d[0], d[1] + 0.05, d[2]};
            game_->onItemParticle(s->item, pos, vel);
        }
    }
    if (audio::AudioSystem* a = audio::instance()) {  // the player's own consume sound (the server skips the user)
        const float volume = drink ? 0.5f : (randf() < 0.5f ? 0.5f : 1.0f);
        const float pitch = drink ? 0.9f + randf() * 0.1f : 1.0f + 0.2f * (randf() - randf());
        a->play(u.sound, audio::Category::Player, p.pos[0], p.pos[1], p.pos[2], volume, pitch);
    }
}

// BrushItem.onUseTick on the client: 5 ticks into each 10-tick stroke, dust off the brushed face and the brush
// sound (left to the user by the server); off any block the use ends.
void Gameplay::tickBrush() {
    Game::ItemUse& use = game_->use;
    const RayHit& hit = game_->target();
    if (!hit.hit) {  // LivingEntity.releaseUsingItem on the client (no packet: the server stops its own brushing)
        use.active = false;
        return;
    }
    if ((use.duration - use.remaining + 1) % 10 != 5) return;
    const u16 state = world_.block(hit.x, hit.y, hit.z);
    const std::string& full = registry_.name(state);
    const std::string block(shortName(full.substr(0, full.find('['))));
    const Player& p = game_->player();
    if (game_->onBlockParticle && registry_.infos()[state].kind != KindAir) {  // spawnDustParticles
        const bool rightArm = (use.hand == 0) == (!ui_ || ui_->settings().mainHand == 1);
        const double flip = rightArm ? 1.0 : -1.0;
        const double vx = -std::sin(p.yaw) * std::cos(p.pitch), vz = std::cos(p.yaw) * std::cos(p.pitch);  // view vector
        double dx, dz;
        switch (hit.face) {  // DustParticlesDelta.fromDirection
            case 0: case 1: dx = vz, dz = -vx; break;
            case 2: dx = 1.0, dz = -0.1; break;   // north
            case 3: dx = -1.0, dz = 0.1; break;   // south
            case 4: dx = -0.1, dz = -1.0; break;  // west
            default: dx = 0.1, dz = 1.0; break;   // east
        }
        const int count = 7 + int(randf() * 5.0f);
        for (int i = 0; i < count; ++i) {
            const double pos[3] = {hit.point[0] - (hit.face == 4 ? 1e-6 : 0.0), hit.point[1], hit.point[2] - (hit.face == 2 ? 1e-6 : 0.0)};
            const double vel[3] = {dx * flip * 3.0 * randf(), 0.0, dz * flip * 3.0 * randf()};
            game_->onBlockParticle(state, pos, vel);
        }
    }
    const char* sound = block == "suspicious_sand"     ? "minecraft:item.brush.brushing.sand"
                        : block == "suspicious_gravel" ? "minecraft:item.brush.brushing.gravel"
                                                       : "minecraft:item.brush.brushing.generic";
    if (audio::AudioSystem* a = audio::instance()) a->play(sound, audio::Category::Block, hit.x + 0.5, hit.y + 0.5, hit.z + 0.5, 1.0f, 1.0f);
}

std::string Gameplay::itemName(int item) const {
    return item >= 0 && size_t(item) < itemNames_.size() ? itemNames_[size_t(item)] : std::string();
}

bool Gameplay::eyeInWater() const { return game_ && game_->player().eyeInWater; }  // the player's fluid tracker

// BlockBehaviour.getDestroyProgress with Player.getDestroySpeed / hasCorrectToolForDrops.
float Gameplay::destroyProgress(int, int, int, u16 state) {
    const std::string& key = registry_.name(state);
    const ItemRules::Hardness h = rules_.hardness(key);
    if (h.destroyTime == -1.0f) return 0.0f;
    const std::string_view block = std::string_view(key).substr(0, key.find('['));
    const ItemRules::Tool* tool = heldTool(mainHand());
    float speed = tool ? rules_.miningSpeed(*tool, block) : 1.0f;
    const bool correct = !h.requiresTool || (tool && rules_.correctForDrops(*tool, block));
    if (speed > 1.0f) speed += float(attribute("mining_efficiency", 0.0));
    const int haste = effect("haste"), conduit = effect("conduit_power");
    if (haste >= 0 || conduit >= 0) speed *= 1.0f + float(std::max(haste, conduit) + 1) * 0.2f;  // MobEffectUtil
    if (const int fatigue = effect("mining_fatigue"); fatigue >= 0) speed *= float(std::pow(0.3, fatigue + 1));
    speed *= float(attribute("block_break_speed", 1.0));
    if (eyeInWater()) speed *= float(attribute("submerged_mining_speed", 0.2));
    if (!game_->player().onGround) speed /= 5.0f;
    return speed / h.destroyTime / float(correct ? 30 : 100);
}
