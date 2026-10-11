#include "game/gameplay.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "entity/entities.h"
#include "game/game.h"
#include "game/riding.h"
#include "net/client.h"
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
            if (game_) game_->border = WorldBorder{};  // a new ClientLevel
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
    client.onPlayPeek("damage_event", [this](net::Reader& r) {  // ClientboundDamageEventPacket: entity id first
        const int id = r.varInt();
        if (r.ok) post([this, id] {
            if (game_ && id == entityId_) game_->hurt();
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
    game.predictUse = [this](const RayHit* target, bool afterEntity) { predictUse(target, afterEntity); };
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
    game.canDestroyInCreative = [this] {  // Item.canDestroyBlock: the tool component's can_destroy_blocks_in_creative
        const ItemRules::Tool* t = heldTool(mainHand());
        return !t || t->canDestroyInCreative;
    };
    game.adventureMayBreak = [this](int x, int y, int z) { return adventureMayBreak(x, y, z); };
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
    a.jumpBoost = effect("jump_boost"), a.slowFalling = effect("slow_falling"), a.levitation = effect("levitation");
    a.dolphinsGrace = effect("dolphins_grace");
    const ui::ItemStack* chest = ui_ ? ui_->playerSlot(6) : nullptr;
    a.canGlide = chest && rules_.glider(itemName(chest->item)) && (chest->maxDamage <= 0 || chest->damage < chest->maxDamage - 1);
    const ui::ItemStack* feet = ui_ ? ui_->playerSlot(8) : nullptr;
    a.powderSnowWalker = feet && itemName(feet->item) == "minecraft:leather_boots";
    game_->attackSpeed = float(attribute("attack_speed", 4.0));
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
    game_->crosshairOnLiving = false;
    if (entities_) {  // Minecraft.crosshairPickEntity: a living entity in front of the targeted block
        const CameraState c = game_->camera();
        const double eye[3] = {c.x, c.y, c.z}, cp = std::cos(c.pitch);
        const double dir[3] = {-std::sin(c.yaw) * cp, -std::sin(c.pitch), std::cos(c.yaw) * cp};
        const double block = game_->pickBlockDistance();
        const double limit = block >= 0 ? std::min(block, game_->entityReach) : game_->entityReach;
        const ent::PickResult pick = entities_->pick(eye, dir, limit);
        if (pick.id >= 0)
            if (const ent::Entity* e = entities_->entity(pick.id))
                game_->crosshairOnLiving = (e->category == ent::Category::Living || e->category == ent::Category::Player) &&
                                           !e->dying && e->health > 0;
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
    const ItemRules::Use u = rules_.use(itemName(s->item));
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

// Minecraft.startUseItem -> MultiPlayerGameMode.useItem -> ItemStack.use: the main hand, then the off hand when the
// main hand's use passes. Uses that start over time are predicted; the server's living flags confirm or end them.
void Gameplay::predictUse(const RayHit* target, bool) {
    if (!game_ || game_->use.active || game_->gameMode == 3) return;
    for (int hand = 0; hand < 2; ++hand) {
        const ui::ItemStack* s = hand == 0 ? mainHand() : offHand();
        if (!s) continue;
        const ItemRules::Use u = rules_.use(itemName(s->item));
        if (u.anim == ItemRules::Anim::None && u.duration <= 0) {
            if (hand == 0) continue;  // nothing to use: the off hand's turn
            return;
        }
        bool can = u.duration > 0;
        switch (u.anim) {
            case ItemRules::Anim::Eat:
            case ItemRules::Anim::Drink:  // Consumable.canConsume -> Player.canEat
                can = can && (!u.food || u.alwaysEat || food_ < 20 || game_->gameMode == 1);
                break;
            case ItemRules::Anim::Bow: can = can && hasProjectile(); break;
            case ItemRules::Anim::Crossbow: can = can && chargeType(s) == 0 && hasProjectile(); break;  // charged: fires
            case ItemRules::Anim::Brush: can = can && target != nullptr; break;
            case ItemRules::Anim::Bundle: can = false; break;  // depends on the contents: the server decides
            default: break;
        }
        if (can) startUsing(hand);
        return;  // this hand's use was handled (consumed or failed): Minecraft.startUseItem stops
    }
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

std::string Gameplay::itemName(int item) const {
    return item >= 0 && size_t(item) < itemNames_.size() ? itemNames_[size_t(item)] : std::string();
}

bool Gameplay::eyeInWater() const {
    if (!game_) return false;
    const Player& p = game_->player();
    const double eyeY = p.pos[1] + p.eyeHeight() - 0.11111111;
    const int x = int(std::floor(p.pos[0])), y = int(std::floor(eyeY)), z = int(std::floor(p.pos[2]));
    const u16 id = world_.block(x, y, z);
    const BlockInfo& b = registry_.infos()[id];
    if (!b.fluid && !b.waterlogged) return false;
    const std::string& name = registry_.name(id);
    if (!b.waterlogged && !name.starts_with("minecraft:water")) return false;
    // FluidState.getHeight: own amount / 9 (8 / 9 for sources and falling water; full under the same fluid).
    double height = 8.0 / 9.0;
    if (const std::string_view level = registry_.property(id, "level"); !b.waterlogged && !level.empty()) {
        const int l = std::atoi(std::string(level).c_str());
        if (l > 0 && l < 8) height = double(8 - l) / 9.0;
    }
    const u16 above = world_.block(x, y + 1, z);
    if (registry_.infos()[above].waterlogged || registry_.name(above).starts_with("minecraft:water")) height = 1.0;
    return y + height > eyeY;
}

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
