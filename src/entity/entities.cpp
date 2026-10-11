#include "entity/entities.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "net/client.h"
#include "net/protocol.h"
#include "ui/items.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <shared_mutex>

namespace ent {
namespace {

constexpr float kPi = 3.14159265f;

float wrapDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d >= 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}
float rotLerp(float a, float from, float to) { return from + a * wrapDegrees(to - from); }
float lerpf(float a, float from, float to) { return from + a * (to - from); }
float unpackDegrees(i8 b) { return float(b) * 360.0f / 256.0f; }

void readUuid(net::Reader& r, u8 out[16]) {
    if (!r.need(16)) return;
    std::memcpy(out, r.p, 16);
    r.p += 16;
}
std::string uuidKey(const u8 u[16]) { return std::string(reinterpret_cast<const char*>(u), 16); }

// Vec3.LP_STREAM_CODEC (LpVec3).
void readLpVec3(net::Reader& r, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    const u32 lowest = r.u8v();
    if (lowest == 0) return;
    const u32 middle = r.u8v();
    const u64 highest = r.u32v();
    const u64 buffer = (highest << 16) | (u64(middle) << 8) | lowest;
    u64 scale = lowest & 3;
    if (lowest & 4) scale |= (u64(u32(r.varInt())) & 0xFFFFFFFFull) << 2;
    auto unpack = [](u64 v) { return std::min(double(v & 32767), 32766.0) * 2.0 / 32766.0 - 1.0; };
    out[0] = unpack(buffer >> 3) * double(scale);
    out[1] = unpack(buffer >> 18) * double(scale);
    out[2] = unpack(buffer >> 33) * double(scale);
}
void writeLpVec3(net::Writer& w, const double v[3]) {
    const double m = std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])});
    if (m < 3.051944088384301E-5) {
        w.u8v(0);
        return;
    }
    const i64 scale = i64(std::ceil(m));
    const bool partial = (scale & 3) != scale;
    const i64 markers = partial ? ((scale & 3) | 4) : scale;
    auto pack = [&](double x) { return i64(std::llround((x / double(scale) * 0.5 + 0.5) * 32766.0)); };
    const u64 buffer = u64(markers) | (u64(pack(v[0])) << 3) | (u64(pack(v[1])) << 18) | (u64(pack(v[2])) << 33);
    w.u8v(u8(buffer));
    w.u8v(u8(buffer >> 8));
    w.u32v(u32(buffer >> 16));
    if (partial) w.varInt(i32(scale >> 2));
}

const ui::Items* gItems = nullptr;  // EntityWorld::setItemDecoder

// ItemStack.OPTIONAL_STREAM_CODEC. Component patches are not length-prefixed: without the item registry a stack
// with added components cannot be skipped and returns false (after reading the item id); the caller stops there.
bool readItemStack(net::Reader& r, i64& item, int& count, i32* dye = nullptr, bool* foil = nullptr) {
    if (dye) *dye = -1;
    if (foil) *foil = false;
    if (gItems) {
        ui::ItemStack st;
        const bool ok = gItems->read(r, st);
        item = st.empty() ? -1 : st.item;
        count = st.count;
        if (dye) *dye = st.dyedColor >= 0 ? i32(st.dyedColor & 0xFFFFFF) : -1;
        if (foil) *foil = gItems->foil(st);
        return ok && r.ok;
    }
    count = r.varInt();
    item = -1;
    if (count <= 0) return r.ok;
    item = r.varInt();
    const i32 added = r.varInt(), removed = r.varInt();
    if (added != 0) return false;
    for (i32 i = 0; i < removed && r.ok; ++i) r.varInt();
    return r.ok;
}

double decodeDelta(double base, i16 delta) {
    if (delta == 0) return base;
    return double(std::llround(base * 4096.0) + delta) / 4096.0;
}

}  // namespace

void EntityWorld::setItemDecoder(const ui::Items* items) { gItems = items; }

void tickPoseAnimation(Entity& e, const double move[3], bool fallFlying, bool crouching) {
    // LivingEntity.updateSwimAmount: isVisuallySwimming = pose SWIMMING, or FALL_FLYING without the flag.
    e.swimAmountO = e.swimAmount;
    const bool visuallySwimming = e.pose == 3 || (e.pose == 1 && !fallFlying);
    e.swimAmount = visuallySwimming ? std::min(1.0f, e.swimAmount + 0.09f) : std::max(0.0f, e.swimAmount - 0.09f);
    e.fallFlyTicks = fallFlying ? e.fallFlyTicks + 1 : 0;
    std::copy(e.move, e.move + 3, e.moveO);
    for (int k = 0; k < 3; ++k) e.move[k] = float(move[k]);
    // ElytraAnimationState.tick
    std::copy(e.elytraRot, e.elytraRot + 3, e.elytraRotO);
    float tx = kPi / 12, ty = 0, tz = -kPi / 12;
    if (fallFlying) {
        float ratio = 1.0f;
        const double len = std::sqrt(move[0] * move[0] + move[1] * move[1] + move[2] * move[2]);
        if (move[1] < 0 && len > 1e-4) ratio = 1.0f - float(std::pow(-move[1] / len, 1.5));
        tx = kPi / 12 + (kPi / 9 - kPi / 12) * ratio;
        tz = -kPi / 12 + (-kPi / 2 + kPi / 12) * ratio;
    } else if (crouching) {
        tx = kPi * 2.0f / 9.0f, tz = -kPi / 4, ty = 0.08726646f;
    }
    e.elytraRot[0] += (tx - e.elytraRot[0]) * 0.3f;
    e.elytraRot[1] += (ty - e.elytraRot[1]) * 0.3f;
    e.elytraRot[2] += (tz - e.elytraRot[2]) * 0.3f;
}

int bedFacingAt(const World& world, const BlockRegistry& registry, int x, int y, int z) {
    const std::string& state = registry.name(world.block(x, y, z));
    if (state.find("_bed") == std::string::npos) return -1;
    if (state.find("facing=south") != std::string::npos) return 0;
    if (state.find("facing=west") != std::string::npos) return 1;
    if (state.find("facing=north") != std::string::npos) return 2;
    if (state.find("facing=east") != std::string::npos) return 3;
    return -1;
}

int defaultSkinIndex(const u8 u[16]) {
    u64 msb = 0, lsb = 0;
    for (int i = 0; i < 8; ++i) msb = (msb << 8) | u[i], lsb = (lsb << 8) | u[8 + i];
    const u64 hilo = msb ^ lsb;
    const i32 h = i32(u32(hilo >> 32)) ^ i32(u32(hilo));
    return ((h % 18) + 18) % 18;
}

EntityWorld::EntityWorld(EntityAssets& assets, World& world, BlockRegistry& registry, const GameReports& reports)
    : assets_(assets), world_(world), registry_(registry), reports_(reports) {
    const char* v = std::getenv("MCPP_ENTITY_VERBOSE");
    verbose_ = v && *v == '1';
    const auto& types = assets_.types();
    dataIds_.resize(types.size());
    categories_.resize(types.size(), Category::Other);
    for (size_t i = 0; i < types.size(); ++i) {
        const TypeInfo& t = types[i];
        DataIds& d = dataIds_[i];
        auto id = [&](std::initializer_list<const char*> names) -> i8 {
            for (const char* n : names)
                if (int v = t.dataId(n); v >= 0) return i8(v);
            return -1;
        };
        d.flags = id({"DATA_SHARED_FLAGS_ID"});
        d.pose = id({"DATA_POSE"});
        d.customName = id({"DATA_CUSTOM_NAME"});
        d.health = id({"DATA_HEALTH_ID"});
        d.baby = id({"DATA_BABY_ID"});
        d.wool = id({"DATA_WOOL_ID"});
        d.variant = id({"DATA_VARIANT_ID", "DATA_VARIANT", "DATA_TYPE_ID", "DATA_TYPE"});
        d.item = id({"DATA_ITEM"});
        d.size = id({"ID_SIZE"});
        d.swellDir = id({"DATA_SWELL_DIR"});
        d.powered = id({"DATA_IS_POWERED"});
        d.mainHand = id({"DATA_PLAYER_MAIN_HAND"});
        d.skinParts = id({"DATA_PLAYER_MODE_CUSTOMISATION"});
        d.horseVariant = id({"DATA_ID_TYPE_VARIANT"});
        d.villagerData = id({"DATA_VILLAGER_DATA"});
        d.tameFlags = id({"DATA_FLAGS_ID"});
        d.collar = id({"DATA_COLLAR_COLOR"});
        d.anger = id({"DATA_ANGER_END_TIME"});
        d.standFlags = id({"DATA_CLIENT_FLAGS"});
        d.standPose = id({"DATA_HEAD_POSE"});
        d.hurt = id({"DATA_ID_HURT"});
        d.hurtDir = id({"DATA_ID_HURTDIR"});
        d.damage = id({"DATA_ID_DAMAGE"});
        d.mainGene = id({"MAIN_GENE_ID", "DATA_MAIN_GENE_ID"});
        d.mobFlags = id({"DATA_MOB_FLAGS_ID"});
        d.noGravity = id({"DATA_NO_GRAVITY"});
        d.inGround = id({"IN_GROUND"});
        d.hiddenGene = id({"HIDDEN_GENE_ID", "DATA_HIDDEN_GENE_ID"});
        d.rotation = id({"DATA_ROTATION"});
        d.painting = id({"DATA_PAINTING_VARIANT_ID"});
        d.nameVisible = id({"DATA_CUSTOM_NAME_VISIBLE"});
        d.livingFlags = id({"DATA_LIVING_ENTITY_FLAGS"});
        d.attachedTo = id({"DATA_ATTACHED_TO_TARGET"});
        d.sleepPos = id({"SLEEPING_POS_ID"});
        d.ticksFrozen = id({"DATA_TICKS_FROZEN"});
        d.paddleLeft = id({"DATA_ID_PADDLE_LEFT"});
        d.paddleRight = id({"DATA_ID_PADDLE_RIGHT"});
        d.chest = id({"DATA_ID_CHEST"});
        categories_[i] = categoryFor(t.name);
        // Synched data only carries values that differ from the default: start registry variants at the
        // default each entity defines (VariantUtils.getDefaultOrAny(..., DEFAULT)).
        static const std::pair<const char*, std::pair<const char*, const char*>> kDefaults[] = {
            {"minecraft:cow", {"cow_variant", "temperate"}},       {"minecraft:pig", {"pig_variant", "temperate"}},
            {"minecraft:chicken", {"chicken_variant", "temperate"}}, {"minecraft:frog", {"frog_variant", "temperate"}},
            {"minecraft:cat", {"cat_variant", "black"}},            {"minecraft:wolf", {"wolf_variant", "pale"}},
            {"minecraft:zombie_nautilus", {"zombie_nautilus_variant", "temperate"}}};
        defaultVariant_.push_back(0);
        for (const auto& [type, def] : kDefaults) {
            if (t.name != type) continue;
            const auto& list = assets_.variants(def.first);
            for (size_t v = 0; v < list.size(); ++v)
                if (list[v].name == def.second) defaultVariant_.back() = int(v);
        }
    }
    for (const auto& [key, id] : reports.stateIds()) {
        if (id < 0) continue;
        if (size_t(id) >= blockStates_.size()) blockStates_.resize(size_t(id) + 1);
        blockStates_[size_t(id)] = key;
    }
    const auto& reg = reports.registries();
    auto order = [&](const char* name, std::vector<std::string>& out) {
        if (!reg.is_object() || !reg.contains(name)) return;
        const nlohmann::json entries = reg[name].value("entries", nlohmann::json::object());  // keep alive
        for (const auto& [k, e] : entries.items()) {
            const int pid = e.value("protocol_id", -1);
            if (pid < 0) continue;
            if (size_t(pid) >= out.size()) out.resize(size_t(pid) + 1);
            out[size_t(pid)] = k.substr(k.find(':') + 1);
        }
    };
    order("minecraft:villager_type", villagerTypes_);
    order("minecraft:villager_profession", professions_);
    std::vector<std::string> particles;
    order("minecraft:particle_type", particles);
    for (size_t i = 0; i < particles.size(); ++i)
        if (particles[i] == "entity_effect") entityEffectParticle_ = int(i);
}

Category EntityWorld::categoryFor(const std::string& n) const {
    if (n == "minecraft:player" || n == "minecraft:mannequin") return Category::Player;
    if (n == "minecraft:item") return Category::Item;
    if (n == "minecraft:falling_block" || n == "minecraft:tnt") return Category::Block;
    if (n.ends_with("boat") || n.ends_with("raft") || n.ends_with("minecart")) return Category::Vehicle;
    static const char* kProjectiles[] = {"arrow", "spectral_arrow", "trident", "snowball", "egg", "ender_pearl",
                                         "experience_bottle", "splash_potion", "lingering_potion", "fireball",
                                         "small_fireball", "dragon_fireball", "wither_skull", "wind_charge",
                                         "breeze_wind_charge", "llama_spit", "shulker_bullet", "eye_of_ender",
                                         "firework_rocket", "fishing_bobber", "experience_orb"};
    for (const char* p : kProjectiles)
        if (n == std::string("minecraft:") + p) return Category::Projectile;
    static const char* kOther[] = {"area_effect_cloud", "marker", "interaction", "block_display", "item_display",
                                   "text_display", "lightning_bolt", "item_frame", "glow_item_frame", "painting",
                                   "leash_knot", "evoker_fangs", "end_crystal", "ominous_item_spawner"};
    for (const char* p : kOther)
        if (n == std::string("minecraft:") + p) return Category::Other;
    return Category::Living;
}

u16 EntityWorld::localBlock(int state) {
    if (state <= 0 || size_t(state) >= blockStates_.size() || blockStates_[size_t(state)].empty()) return 0;
    auto it = blockCache_.find(state);
    if (it != blockCache_.end()) return it->second;
    const u16 id = registry_.idFor(blockStates_[size_t(state)]);
    blockCache_[state] = id;
    return id;
}

// ---------------------------------------------------------------------------------------------------------
// Network thread: decode into the incoming queue.

namespace {

// Parses set_entity_data values until the terminator, or until a value that cannot be skipped safely.
void readEntityData(net::Reader& r, EventQueue& q, int entityEffect) {
    for (;;) {
        const u8 index = r.u8v();
        if (!r.ok || index == 0xFF) return;
        MetaValue v;
        v.index = index;
        v.serializer = u8(r.varInt());
        bool keep = true, stop = false;
        switch (v.serializer) {
            case 0: v.i = r.u8v(); break;                  // byte
            case 1: v.i = r.varInt(); break;               // int
            case 2: v.i = r.varLong(); break;              // long
            case 3: v.f[0] = r.f32(); break;               // float
            case 4: q.strings.emplace_back(r.str()), v.str = i32(q.strings.size() - 1); break;  // string
            case 5:  // component
                q.strings.push_back(net::componentText(r.nbt(), nullptr)), v.str = i32(q.strings.size() - 1);
                break;
            case 6:  // optional component
                if (r.boolean()) q.strings.push_back(net::componentText(r.nbt(), nullptr));
                else q.strings.emplace_back();
                v.str = i32(q.strings.size() - 1);
                break;
            case 7: {  // item stack
                int count = 0;
                i32 dye = -1;
                bool foil = false;
                const bool complete = readItemStack(r, v.i, count, &dye, &foil);
                v.f[0] = float(count), v.f[1] = float(dye), v.f[2] = foil ? 1.0f : 0.0f;
                stop = !complete;
                break;
            }
            case 8: v.i = r.boolean(); break;                                       // boolean
            case 9: v.f[0] = r.f32(), v.f[1] = r.f32(), v.f[2] = r.f32(); break;  // rotations
            case 10: v.i = r.i64v(); break;                                         // block pos
            case 11: v.f[0] = r.boolean() ? 1.0f : 0.0f, v.i = v.f[0] > 0 ? r.i64v() : 0; break;  // optional block pos
            case 12: v.i = r.varInt(); break;                                       // direction
            case 13:                                                                // optional entity reference
                if (r.boolean()) r.skip(16);
                keep = false;
                break;
            case 14: case 15: v.i = r.varInt(); break;  // (optional) block state
            case 16: case 17: {  // particle(s): only argument-less and colour particles can be skipped
                const i32 n = v.serializer == 16 ? 1 : r.varInt();
                for (i32 i = 0; i < n && r.ok && !stop; ++i) {
                    const i32 type = r.varInt();
                    if (type == entityEffect) r.skip(4);
                    else stop = true;
                }
                keep = false;
                break;
            }
            case 18: v.i = r.varInt(), v.f[0] = float(r.varInt()), r.varInt(); break;  // villager data
            case 19: case 20: v.i = r.varInt(); break;  // optional unsigned int, pose
            case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32:
                v.i = r.varInt();  // registry holders (variants)
                break;
            case 33:  // optional global pos
                if (r.boolean()) r.str(), r.i64v();
                keep = false;
                break;
            case 34: {  // painting variant: registry id + 1, or 0 + inline definition
                const i32 h = r.varInt();
                if (h == 0) stop = true;
                v.i = h - 1;
                break;
            }
            case 35: case 36: case 37: case 38: case 42: case 43: v.i = r.varInt(); break;  // enums
            case 39: v.f[0] = r.f32(), v.f[1] = r.f32(), v.f[2] = r.f32(); break;
            case 40: v.f[0] = r.f32(), v.f[1] = r.f32(), v.f[2] = r.f32(), v.f[3] = r.f32(); break;
            default: stop = true, keep = false; break;  // resolvable profile and anything newer
        }
        if (!r.ok) return;
        if (keep) q.meta.push_back(v);
        if (stop) return;
    }
}

}  // namespace

void EntityWorld::attach(net::Client& client) {
    client_ = &client;
    auto on = [&](const char* name, auto&& fn) {
        client.onPlay(name, [this, fn](net::Reader& r) {
            std::lock_guard lock(mutex_);
            fn(r, incoming_);
        });
    };
    const int effect = entityEffectParticle_;
    on("add_entity", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Add;
        e.id = r.varInt();
        readUuid(r, e.uuid);
        e.a = r.varInt();
        e.d[0] = r.f64(), e.d[1] = r.f64(), e.d[2] = r.f64();
        readLpVec3(r, e.d + 3);
        e.f[1] = unpackDegrees(i8(r.u8v()));  // xRot
        e.f[0] = unpackDegrees(i8(r.u8v()));  // yRot
        e.f[2] = unpackDegrees(i8(r.u8v()));  // yHeadRot
        e.b = r.varInt();
        if (r.ok) q.events.push_back(e);
    });
    on("remove_entities", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Remove;
        e.first = u32(q.ints.size());
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) q.ints.push_back(r.varInt());
        e.count = u32(q.ints.size()) - e.first;
        q.events.push_back(e);
    });
    auto move = [](net::Reader& r, EventQueue& q, bool pos, bool rot) {
        Event e;
        e.type = EventType::Move;
        e.id = r.varInt();
        e.flag = pos;
        e.hasRot = rot;
        e.first = u32(q.steps.size());
        if (pos) {
            const i32 props = r.varInt();
            e.b = props & 1;  // on ground
            const i32 stepCount = props >> 1;
            if (stepCount <= 0) {
                Step s{};
                s.ticks = -1;
                s.delta[0] = r.i16v(), s.delta[1] = r.i16v(), s.delta[2] = r.i16v();
                q.steps.push_back(s);
            } else {
                for (i32 i = 0; i < stepCount && r.ok; ++i) {
                    Step s{};
                    s.ticks = r.varInt();
                    s.delta[0] = r.i16v(), s.delta[1] = r.i16v(), s.delta[2] = r.i16v();
                    q.steps.push_back(s);
                }
            }
        } else {
            e.b = r.boolean();
        }
        if (rot) e.f[0] = unpackDegrees(i8(r.u8v())), e.f[1] = unpackDegrees(i8(r.u8v()));
        e.count = u32(q.steps.size()) - e.first;
        if (r.ok) q.events.push_back(e);
        else q.steps.resize(e.first);
    };
    on("move_entity_pos", [move](net::Reader& r, EventQueue& q) { move(r, q, true, false); });
    on("move_entity_pos_rot", [move](net::Reader& r, EventQueue& q) { move(r, q, true, true); });
    on("move_entity_rot", [move](net::Reader& r, EventQueue& q) { move(r, q, false, true); });
    on("entity_position_sync", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::PositionSync;
        e.id = r.varInt();
        e.first = u32(q.steps.size());
        if (r.varInt() == 0) {  // PositionPath.Linear
            Step s{};
            s.ticks = -1;
            s.pos[0] = r.f64(), s.pos[1] = r.f64(), s.pos[2] = r.f64();
            q.steps.push_back(s);
        } else {
            const i32 n = r.varInt();
            for (i32 i = 0; i < n && r.ok; ++i) {
                Step s{};
                s.pos[0] = r.f64(), s.pos[1] = r.f64(), s.pos[2] = r.f64();
                s.ticks = r.varInt();
                q.steps.push_back(s);
            }
        }
        e.count = u32(q.steps.size()) - e.first;
        e.f[0] = r.f32(), e.f[1] = r.f32();
        e.flag = r.boolean();
        if (r.ok && e.count) q.events.push_back(e);
        else q.steps.resize(e.first);
    });
    on("teleport_entity", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Teleport;
        e.id = r.varInt();
        for (double& v : e.d) v = r.f64();
        e.f[0] = r.f32(), e.f[1] = r.f32();
        e.a = i32(r.u32v());
        e.flag = r.boolean();
        if (r.ok) q.events.push_back(e);
    });
    on("set_entity_motion", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Motion;
        e.id = r.varInt();
        readLpVec3(r, e.d);
        if (r.ok) q.events.push_back(e);
    });
    on("rotate_head", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::HeadRot;
        e.id = r.varInt();
        e.f[0] = unpackDegrees(i8(r.u8v()));
        if (r.ok) q.events.push_back(e);
    });
    on("set_entity_data", [effect](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Data;
        e.id = r.varInt();
        e.first = u32(q.meta.size());
        readEntityData(r, q, effect);
        e.count = u32(q.meta.size()) - e.first;
        if (e.count) q.events.push_back(e);
    });
    on("set_equipment", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Equipment;
        e.id = r.varInt();
        e.first = u32(q.ints.size());
        for (;;) {
            const u8 slot = r.u8v();
            i64 item;
            int count;
            i32 dye;
            bool foil;
            const bool complete = readItemStack(r, item, count, &dye, &foil);
            if (!r.ok) break;
            q.ints.push_back(slot & 127), q.ints.push_back(i32(item)), q.ints.push_back(dye), q.ints.push_back(foil);
            if (!complete || !(slot & 0x80)) break;
        }
        e.count = (u32(q.ints.size()) - e.first) / 4;
        if (e.count) q.events.push_back(e);
    });
    on("player_info_update", [](net::Reader& r, EventQueue& q) {
        const u8 actions = r.u8v();
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            Event e;
            e.type = EventType::PlayerInfo;
            readUuid(r, e.uuid);
            if (actions & 1) {  // add player: name + properties
                q.strings.emplace_back(r.str());
                e.a = i32(q.strings.size() - 1);
                const i32 props = r.varInt();
                for (i32 p = 0; p < props && r.ok; ++p) {
                    r.str(), r.str();
                    if (r.boolean()) r.str();
                }
            } else {
                e.a = -1;
            }
            if (actions & 2)  // chat session
                if (r.boolean()) r.skip(16), r.i64v(), r.byteArray(), r.byteArray();
            if (actions & 4) r.varInt();   // game mode
            if (actions & 8) r.boolean();  // listed
            if (actions & 16) r.varInt();  // latency
            if (actions & 32)              // display name
                if (r.boolean()) r.nbt();
            if (actions & 64) r.varInt();   // list order
            if (actions & 128) r.boolean();  // hat
            if (r.ok && e.a >= 0) q.events.push_back(e);
        }
    });
    on("player_info_remove", [](net::Reader& r, EventQueue& q) {
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            Event e;
            e.type = EventType::PlayerRemove;
            readUuid(r, e.uuid);
            if (r.ok) q.events.push_back(e);
        }
    });
    on("swing_animation", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Swing;
        e.id = r.varInt();
        e.a = r.varInt();  // hand
        if (r.ok) q.events.push_back(e);
    });
    on("animate", [](net::Reader&, EventQueue&) {});  // wake up / critical hit particles: nothing to draw
    on("hurt_animation", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Hurt;
        e.id = r.varInt();
        e.f[0] = r.f32();
        if (r.ok) q.events.push_back(e);
    });
    on("damage_event", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Damage;
        e.id = r.varInt();
        if (r.ok) q.events.push_back(e);
    });
    on("entity_event", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::EntityEvent;
        e.id = r.i32v();
        e.a = i8(r.u8v());
        if (r.ok) q.events.push_back(e);
    });
    on("take_item_entity", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::TakeItem;
        e.id = r.varInt();
        e.a = r.varInt();
        e.b = r.varInt();
        if (r.ok) q.events.push_back(e);
    });
    on("set_passengers", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Passengers;
        e.id = r.varInt();
        e.first = u32(q.ints.size());
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) q.ints.push_back(r.varInt());
        e.count = u32(q.ints.size()) - e.first;
        if (r.ok) q.events.push_back(e);
    });
    on("update_attributes", [](net::Reader&, EventQueue&) {});  // not needed for drawing
}

// ---------------------------------------------------------------------------------------------------------
// Main thread.

void EntityWorld::clear() {
    entities_.clear();
    std::lock_guard lock(mutex_);
    incoming_.clear();
}

void EntityWorld::frame(double dt) {
    Timer t;
    {
        std::lock_guard lock(mutex_);
        std::swap(incoming_, processing_);
    }
    for (const Event& e : processing_.events) apply(e, processing_);
    stats_.events = processing_.events.size();
    processing_.clear();
    stats_.applyMs = t.ms();
    Timer tt;
    tickSeconds_ = tickRate.tickSeconds();
    tickAccumulator_ += std::min(dt, 0.25);
    while (tickAccumulator_ >= tickSeconds_) {
        tickRate.tick();
        tick();
        tickAccumulator_ -= tickSeconds_;
    }
    stats_.tickMs = tt.ms();
}

void EntityWorld::startInterpolation(Entity& e, const Step* steps, u32 count, float yaw, float pitch, bool hasRot) {
    // AbstractInterpolationHandler.interpolateTo with the handler the entity type uses.
    const double* end = count ? steps[count - 1].pos : nullptr;
    const bool stepped = e.category == Category::Living || e.category == Category::Player;
    const bool linear = e.category == Category::Vehicle || (e.info && e.info->name == "minecraft:experience_orb");
    if (!hasRot) {
        if (stepped && e.stepCount) yaw = e.steps[(e.stepHead + e.stepCount - 1) % Entity::kMaxSteps].yaw;
        else yaw = e.yaw;
        if (stepped && e.stepCount) pitch = e.steps[(e.stepHead + e.stepCount - 1) % Entity::kMaxSteps].pitch;
        else pitch = e.pitch;
    }
    double target[3];
    if (end) std::copy(end, end + 3, target);
    else if (stepped && e.stepCount) std::copy(e.lastStep.pos, e.lastStep.pos + 3, target);  // replaced below
    else std::copy(e.pos, e.pos + 3, target);
    if (!stepped && !linear) {  // no interpolation: snap
        std::copy(target, target + 3, e.pos);
        e.yaw = yaw, e.pitch = pitch;
        return;
    }
    if (linear) {
        std::copy(target, target + 3, e.linearTarget);
        e.linearYaw = yaw, e.linearPitch = pitch;
        e.linearRemaining = 3;
        return;
    }
    const int interval = std::max(1, e.info ? e.info->updateInterval : 3);
    // Current target (InterpolationData position/rotation).
    Entity::IStep* tail = e.stepCount ? &e.steps[(e.stepHead + e.stepCount - 1) % Entity::kMaxSteps] : nullptr;
    if (!end) {
        if (tail) std::copy(tail->pos, tail->pos + 3, target);
        else std::copy(e.pos, e.pos + 3, target);
    }
    if (tail && tail->pos[0] == target[0] && tail->pos[1] == target[1] && tail->pos[2] == target[2] &&
        tail->yaw == yaw && tail->pitch == pitch)
        return;  // already heading there
    if (!e.stepCount) {
        std::copy(e.pos, e.pos + 3, e.lastStep.pos);
        e.lastStep.yaw = e.yaw, e.lastStep.pitch = e.pitch;
        e.currentStepTicks = 1;
    }
    auto add = [&](const double* p, float y, float x, int ticks) {
        if (e.stepCount == Entity::kMaxSteps) {  // overflow: drop the oldest step
            e.remainingTicks -= float(e.steps[e.stepHead].ticks);
            e.lastStep = e.steps[e.stepHead];
            e.stepHead = (e.stepHead + 1) % Entity::kMaxSteps;
            --e.stepCount;
        }
        Entity::IStep& s = e.steps[(e.stepHead + e.stepCount) % Entity::kMaxSteps];
        std::copy(p, p + 3, s.pos);
        s.yaw = y, s.pitch = x, s.ticks = std::max(1, ticks);
        ++e.stepCount;
        e.remainingTicks += float(s.ticks);
    };
    const float prevYaw = tail ? tail->yaw : e.yaw, prevPitch = tail ? tail->pitch : e.pitch;
    if (count <= 1 || steps[0].ticks < 0) {
        add(target, yaw, pitch, interval);
    } else if (yaw == prevYaw && pitch == prevPitch) {
        for (u32 i = 0; i < count; ++i) add(steps[i].pos, yaw, pitch, steps[i].ticks);
    } else {
        int total = 0;
        for (u32 i = 0; i < count; ++i) total += steps[i].ticks;
        int offset = 0;
        for (u32 i = 0; i < count; ++i) {
            offset += steps[i].ticks;
            const float a = total > 0 ? float(offset) / float(total) : 1.0f;
            add(steps[i].pos, rotLerp(a, prevYaw, yaw), lerpf(a, prevPitch, pitch), steps[i].ticks);
        }
    }
}

void EntityWorld::apply(const Event& ev, const EventQueue& q) {
    if (ev.type == EventType::PlayerInfo) {
        PlayerInfo& p = players_[uuidKey(ev.uuid)];
        if (ev.a >= 0 && size_t(ev.a) < q.strings.size()) p.name = q.strings[size_t(ev.a)];
        const int skin = defaultSkinIndex(ev.uuid);
        static const char* kSkins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};
        p.slim = skin < 9;
        p.skin = assets_.texture(std::string("entity/player/") + (p.slim ? "slim/" : "wide/") + kSkins[skin % 9]);
        return;
    }
    if (ev.type == EventType::PlayerRemove) {
        players_.erase(uuidKey(ev.uuid));
        return;
    }
    if (ev.type == EventType::Remove) {
        for (u32 i = 0; i < ev.count; ++i) entities_.erase(q.ints[ev.first + i]);
        return;
    }
    if (ev.type == EventType::Add) {
        Entity e;
        e.id = ev.id;
        e.type = ev.a;
        std::memcpy(e.uuid, ev.uuid, 16);
        e.info = assets_.type(ev.a);
        e.variant = size_t(ev.a) < defaultVariant_.size() ? defaultVariant_[size_t(ev.a)] : 0;
        e.category = size_t(ev.a) < categories_.size() ? categories_[size_t(ev.a)] : Category::Other;
        for (int a = 0; a < 3; ++a) e.pos[a] = e.prevPos[a] = e.codecBase[a] = ev.d[a], e.vel[a] = ev.d[3 + a];
        e.yaw = e.prevYaw = ev.f[0];
        e.pitch = e.prevPitch = ev.f[1];
        e.headYaw = e.prevHeadYaw = e.bodyYaw = e.prevBodyYaw = ev.f[2];
        e.lerpHeadYaw = ev.f[2];
        if (e.category == Category::Block) e.blockState = ev.b;
        e.direction = ev.b;
        if (e.category == Category::Player) {
            auto it = players_.find(uuidKey(ev.uuid));
            const int skin = defaultSkinIndex(ev.uuid);
            static const char* kSkins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};
            e.slim = skin < 9;
            e.skin = assets_.texture(std::string("entity/player/") + (e.slim ? "slim/" : "wide/") + kSkins[skin % 9]);
            if (it != players_.end()) e.name = it->second.name, e.skin = it->second.skin, e.slim = it->second.slim;
        }
        static std::minstd_rand rng(1234);
        e.bobOffset = float(rng() % 10000) / 10000.0f * kPi * 2.0f;
        if (verbose_)
            LOG("entities: add %d %s at %.2f %.2f %.2f yaw %.0f head %.0f data %d", ev.id, e.info ? e.info->name.c_str() : "?",
                e.pos[0], e.pos[1], e.pos[2], e.yaw, e.headYaw, ev.b);
        entities_[ev.id] = std::move(e);
        return;
    }
    auto it = entities_.find(ev.id);
    if (ev.type == EventType::TakeItem) {
        // The collected item flies to the collector in the game; here it simply disappears.
        if (it != entities_.end() && it->second.category == Category::Item) {
            if (ev.b > 0 && it->second.itemCount > ev.b) it->second.itemCount -= ev.b;
            else entities_.erase(it);
        }
        return;
    }
    if (it == entities_.end()) return;
    Entity& e = it->second;
    switch (ev.type) {
        case EventType::Move: {
            Step steps[Entity::kMaxSteps];
            u32 n = 0;
            if (ev.flag) {
                double base[3] = {e.codecBase[0], e.codecBase[1], e.codecBase[2]};
                for (u32 i = 0; i < ev.count && n < Entity::kMaxSteps; ++i) {
                    const Step& s = q.steps[ev.first + i];
                    Step& o = steps[n++];
                    for (int a = 0; a < 3; ++a) o.pos[a] = decodeDelta(base[a], s.delta[a]);
                    o.ticks = s.ticks;
                    std::copy(o.pos, o.pos + 3, base);  // stepped deltas chain from step to step
                }
                if (n) std::copy(steps[n - 1].pos, steps[n - 1].pos + 3, e.codecBase);
            }
            if (e.id == localControl_) break;  // our own vehicle: the client moves it
            e.onGround = ev.b != 0;
            if (n || ev.hasRot) startInterpolation(e, steps, n, ev.f[0], ev.f[1], ev.hasRot);
            break;
        }
        case EventType::PositionSync: {
            const Step& last = q.steps[ev.first + ev.count - 1];
            std::copy(last.pos, last.pos + 3, e.codecBase);
            if (e.id == localControl_) break;
            const double dx = last.pos[0] - e.pos[0], dy = last.pos[1] - e.pos[1], dz = last.pos[2] - e.pos[2];
            if (dx * dx + dy * dy + dz * dz > 4096.0) {
                std::copy(last.pos, last.pos + 3, e.pos);
                std::copy(last.pos, last.pos + 3, e.prevPos);
                e.yaw = e.prevYaw = ev.f[0], e.pitch = e.prevPitch = ev.f[1];
                e.stepCount = 0, e.remainingTicks = 0, e.linearRemaining = 0;
            } else {
                startInterpolation(e, &q.steps[ev.first], std::min<u32>(ev.count, Entity::kMaxSteps), ev.f[0], ev.f[1],
                                   true);
            }
            e.onGround = ev.flag;
            break;
        }
        case EventType::Teleport: {
            // Relative flags: X Y Z Y_ROT X_ROT DELTA_X DELTA_Y DELTA_Z ROTATE_DELTA.
            Step s{};
            s.ticks = -1;
            for (int a = 0; a < 3; ++a) s.pos[a] = (ev.a & (1 << a)) ? e.pos[a] + ev.d[a] : ev.d[a];
            const float yaw = (ev.a & 8) ? e.yaw + ev.f[0] : ev.f[0];
            const float pitch = (ev.a & 16) ? e.pitch + ev.f[1] : ev.f[1];
            std::copy(s.pos, s.pos + 3, e.codecBase);
            if (e.id == localControl_) break;
            for (int a = 0; a < 3; ++a) e.vel[a] = (ev.a & (32 << a)) ? e.vel[a] + ev.d[3 + a] : ev.d[3 + a];
            startInterpolation(e, &s, 1, yaw, pitch, true);
            e.onGround = ev.flag;
            break;
        }
        case EventType::Motion:
            if (e.id != localControl_) std::copy(ev.d, ev.d + 3, e.vel);
            break;
        case EventType::HeadRot:
            e.lerpHeadYaw = ev.f[0];
            e.lerpHeadSteps = 3;
            break;
        case EventType::Data: applyData(e, &q.meta[ev.first], ev.count, q); break;
        case EventType::Equipment:
            for (u32 i = 0; i < ev.count; ++i) {
                const int slot = q.ints[ev.first + i * 4];
                if (slot < 0 || slot >= 8) continue;
                e.equipment[slot] = q.ints[ev.first + i * 4 + 1];
                e.equipDye[slot] = q.ints[ev.first + i * 4 + 2];
                e.equipFoil = u8((e.equipFoil & ~(1 << slot)) | (q.ints[ev.first + i * 4 + 3] ? 1 << slot : 0));
            }
            break;
        case EventType::Swing: e.swingTime = 0; break;
        case EventType::Hurt:
            e.hurtTime = 10;
            if (verbose_) LOG("entities: %d hurt animation", e.id);
            break;
        case EventType::Damage:  // ClientPacketListener.handleDamageEvent -> animateHurt
            e.hurtTime = 10;
            if (verbose_) LOG("entities: %d damaged", e.id);
            break;
        case EventType::EntityEvent:
            if (ev.a == 3 && e.category == Category::Living) e.dying = true;  // death
            if (ev.a == 2) e.hurtTime = 10;                                     // legacy hurt
            break;
        case EventType::Passengers:
            for (auto& [id, other] : entities_)
                if (other.vehicle == e.id) other.vehicle = -1;
            for (u32 i = 0; i < ev.count; ++i)
                if (auto p = entities_.find(q.ints[ev.first + i]); p != entities_.end()) p->second.vehicle = e.id;
            if (verbose_) LOG("entities: %d now carries %u passenger(s)", e.id, ev.count);
            break;
        default: break;
    }
}

void EntityWorld::applyData(Entity& e, const MetaValue* values, u32 count, const EventQueue& q) {
    if (e.type < 0 || size_t(e.type) >= dataIds_.size()) return;
    const DataIds& d = dataIds_[size_t(e.type)];
    const std::string& name = e.info ? e.info->name : std::string();
    for (u32 i = 0; i < count; ++i) {
        const MetaValue& v = values[i];
        const int idx = v.index;
        if (idx == d.flags) e.sharedFlags = u8(v.i);
        else if (idx == d.mobFlags) e.mobFlags = u8(v.i);
        else if (idx == d.noGravity) e.noGravity = v.i != 0;
        else if (idx == d.inGround) e.inGround = v.i != 0;
        else if (idx == d.pose) e.pose = int(v.i);
        else if (idx == d.customName && v.str >= 0 && e.category != Category::Player) e.name = q.strings[size_t(v.str)];
        else if (idx == d.health) {
            e.health = v.f[0];
            if (verbose_) LOG("entities: %d %s health %.1f", e.id, name.c_str(), e.health);
            if (e.health <= 0 && e.category == Category::Living) e.dying = true;
        } else if (idx == d.baby) e.baby = v.i != 0;
        else if (idx == d.wool) e.wool = int(v.i);
        else if (idx == d.variant) {
            e.variant = int(v.i);
            if (verbose_) LOG("entities: %d %s variant %d", e.id, name.c_str(), e.variant);
        }
        else if (idx == d.horseVariant) e.variant = int(v.i & 0xFF), e.variant2 = int((v.i >> 8) & 0xFF);
        else if (idx == d.item && v.serializer == 7)
            e.itemId = int(v.i), e.itemCount = std::max(1, int(v.f[0])), e.itemDye = i32(v.f[1]), e.itemFoil = v.f[2] > 0.5f;
        else if (idx == d.rotation && v.serializer == 1) e.rotation = int(v.i);
        else if (idx == d.painting && v.serializer == 34) e.painting = int(v.i);
        else if (idx == d.nameVisible && v.serializer == 8) e.nameVisible = v.i != 0;
        else if (idx == d.size) e.size = std::max(1, int(v.i));
        else if (idx == d.swellDir) e.swellDir = int(v.i);
        else if (idx == d.powered) e.powered = v.i != 0;
        else if (idx == d.mainHand) e.mainArm = int(v.i);
        else if (idx == d.skinParts) e.skinParts = int(v.i);
        else if (idx == d.villagerData) e.villagerType = int(v.i), e.variant2 = int(v.f[0]);
        else if (idx == d.tameFlags && (name == "minecraft:wolf" || name == "minecraft:cat")) e.tame = (v.i & 4) != 0;
        else if (idx == d.collar) e.collar = int(v.i);
        else if (idx == d.anger) e.angry = v.i > 0;
        else if (idx == d.standFlags) e.standFlags = int(v.i);
        else if (d.standPose >= 0 && idx >= d.standPose && idx < d.standPose + 6) {
            float* p = e.standPose[idx - d.standPose];
            p[0] = v.f[0], p[1] = v.f[1], p[2] = v.f[2];
        } else if (idx == d.hurt) e.vehicleHurt = int(v.i);
        else if (idx == d.hurtDir) e.vehicleHurtDir = int(v.i);
        else if (idx == d.damage) e.vehicleDamage = v.f[0];
        else if (idx == d.attachedTo && v.serializer == 19) e.attachedTo = v.i > 0 ? int(v.i - 1) : -1;
        else if (idx == d.livingFlags && v.serializer == 0) {
            if (verbose_) LOG("entities: %d %s living flags %d", e.id, name.c_str(), int(v.i));
            if (!(v.i & 1)) e.useTicks = 0;
            e.livingFlags = u8(v.i);
        } else if (idx == d.sleepPos && v.serializer == 11) {
            e.hasSleepPos = v.f[0] > 0;
            if (e.hasSleepPos)
                e.sleepPos[0] = int(v.i >> 38), e.sleepPos[1] = int((v.i << 52) >> 52), e.sleepPos[2] = int((v.i << 26) >> 38);
        } else if (idx == d.ticksFrozen && v.serializer == 1) e.ticksFrozen = int(v.i);
        else if (idx == d.paddleLeft && v.serializer == 8) { if (e.id != localControl_) e.paddle[0] = v.i != 0; }
        else if (idx == d.paddleRight && v.serializer == 8) { if (e.id != localControl_) e.paddle[1] = v.i != 0; }
        else if (idx == d.chest && v.serializer == 8) e.hasChest = v.i != 0;
        else if (idx == d.mainGene) e.variant = int(v.i);
        else if (idx == d.hiddenGene) e.variant2 = int(v.i);
    }
}

void EntityWorld::tick() {
    for (auto& [id, e] : entities_)  // TickRateManager.isEntityFrozen: all but players and what they drive
        if (tickRate.runs || e.category == Category::Player || e.id == localControl_) tickEntity(e);
    // The server stops tracking entities in chunks the client forgets, but a respawn or dimension change
    // replaces the whole level without removing them: drop entities whose chunk stays unloaded for 5 s.
    if (++ticks_ % 20 == 0) {
        std::shared_lock lock(world_.mutex());
        for (auto it = entities_.begin(); it != entities_.end();) {
            Entity& e = it->second;
            e.unloadedTicks = world_.loaded(int(std::floor(e.pos[0])), int(std::floor(e.pos[2]))) ? 0 : e.unloadedTicks + 20;
            if (e.unloadedTicks >= 100) it = entities_.erase(it);
            else ++it;
        }
    }
}

void EntityWorld::tickEntity(Entity& e) {
    std::copy(e.pos, e.pos + 3, e.prevPos);
    e.prevYaw = e.yaw, e.prevPitch = e.pitch;
    e.prevHeadYaw = e.headYaw, e.prevBodyYaw = e.bodyYaw;
    ++e.age;
    if (e.hurtTime > 0) --e.hurtTime;
    if (e.vehicleHurt > 0) --e.vehicleHurt;
    if (e.vehicleDamage > 0) e.vehicleDamage = std::max(0.0f, e.vehicleDamage - 1.0f);
    if (e.dying && e.deathTime < 20) ++e.deathTime;
    if (e.swingTime >= 0 && ++e.swingTime >= e.swingDuration) e.swingTime = -1;
    if (e.livingFlags & 1) ++e.useTicks;

    // Interpolation (a vehicle the local player drives moves itself).
    if (e.id == localControl_ && localTick_) {
        localTick_(e);
    } else if (e.stepCount) {
        const int interval = std::max(1, e.info ? e.info->updateInterval : 3);
        // getNewPositionAndRotation
        bool placed = false;
        while (e.stepCount) {
            const Entity::IStep& s = e.steps[e.stepHead];
            if (e.currentStepTicks < float(s.ticks)) {
                const float a = e.currentStepTicks / float(s.ticks);
                for (int k = 0; k < 3; ++k) e.pos[k] = e.lastStep.pos[k] + (s.pos[k] - e.lastStep.pos[k]) * a;
                e.yaw = rotLerp(a, e.lastStep.yaw, s.yaw);
                e.pitch = lerpf(a, e.lastStep.pitch, s.pitch);
                placed = true;
                break;
            }
            e.currentStepTicks -= float(s.ticks);
            e.lastStep = s;
            e.stepHead = (e.stepHead + 1) % Entity::kMaxSteps;
            --e.stepCount;
        }
        if (!placed) {
            std::copy(e.lastStep.pos, e.lastStep.pos + 3, e.pos);
            e.yaw = e.lastStep.yaw, e.pitch = e.lastStep.pitch;
            e.remainingTicks = 0, e.interpSpeed = 1;
        } else {
            // advance(1 tick)
            float ticks = 1.0f;
            const float targetSpeed = std::max(e.remainingTicks / float(interval), 1.0f);
            e.interpSpeed = lerpf(1.0f / float(interval), e.interpSpeed, targetSpeed);
            if (ticks * e.interpSpeed < e.remainingTicks) ticks *= e.interpSpeed;
            else ticks = e.remainingTicks, e.interpSpeed = 1.0f;
            e.currentStepTicks += ticks;
            e.remainingTicks -= ticks;
        }
    } else if (e.linearRemaining > 0) {
        const double alpha = 1.0 / e.linearRemaining;
        for (int k = 0; k < 3; ++k) e.pos[k] += (e.linearTarget[k] - e.pos[k]) * alpha;
        e.yaw = rotLerp(float(alpha), e.yaw, e.linearYaw);
        e.pitch = lerpf(float(alpha), e.pitch, e.linearPitch);
        --e.linearRemaining;
    } else if ((e.category == Category::Item || e.category == Category::Block || e.category == Category::Projectile) &&
               !e.inGround) {
        // Client-side motion between server updates (items, falling blocks and projectiles simulate locally).
        const bool arrowLike = e.info && (e.info->name.ends_with("arrow") || e.info->name == "minecraft:trident");
        const double gravity = e.category == Category::Projectile ? (arrowLike ? 0.05 : 0.03) : 0.04;
        const double drag = e.category == Category::Projectile ? 0.99 : 0.98;
        if (!e.onGround || e.vel[1] > 0) {
            if (!e.noGravity) e.vel[1] -= gravity;
            double next[3] = {e.pos[0] + e.vel[0], e.pos[1] + e.vel[1], e.pos[2] + e.vel[2]};
            // Stop on the block below (full collision shapes only; the server corrects the rest).
            {
                std::shared_lock lock(world_.mutex());
                const int bx = int(std::floor(next[0])), by = int(std::floor(next[1])), bz = int(std::floor(next[2]));
                const BlockInfo& b = registry_.infos()[world_.block(bx, by, bz)];
                if (b.collisionCount && e.vel[1] < 0) {
                    float top = 0;
                    for (u32 i = 0; i < b.collisionCount; ++i)
                        top = std::max(top, registry_.box(b.collisionFirst + i).max[1]);
                    if (next[1] < by + top) {
                        next[1] = by + top;
                        e.vel[1] = 0;
                        if (arrowLike) e.vel[0] = e.vel[2] = 0;
                        e.onGround = true;
                    }
                }
            }
            std::copy(next, next + 3, e.pos);
        } else {
            e.pos[0] += e.vel[0], e.pos[2] += e.vel[2];
            e.vel[0] *= 0.6, e.vel[2] *= 0.6;  // ground friction (block friction 0.6)
        }
        for (double& v : e.vel) v *= drag;
        if (e.category == Category::Projectile && !e.onGround) {  // face the direction of flight
            const double h = std::sqrt(e.vel[0] * e.vel[0] + e.vel[2] * e.vel[2]);
            if (h > 1e-4 || std::abs(e.vel[1]) > 1e-4) {
                e.yaw = float(std::atan2(e.vel[0], e.vel[2]) * 180.0 / kPi);
                e.pitch = float(std::atan2(e.vel[1], h) * 180.0 / kPi);
            }
        }
    }

    // AbstractBoat.tick: the paddles turn pi/8 per tick while rowing (getRowingTime lerps from the previous step).
    for (int i = 0; i < 2; ++i) e.paddlePos[i] = e.paddle[i] ? e.paddlePos[i] + kPi / 8 : 0.0f;
    if (e.category != Category::Living && e.category != Category::Player) return;
    {  // pose animation state (swimming / gliding / elytra wings), the bed of a sleeping entity
        const double move[3] = {e.pos[0] - e.prevPos[0], e.pos[1] - e.prevPos[1], e.pos[2] - e.prevPos[2]};
        std::shared_lock lock(world_.mutex());
        const u16 at = world_.block(int(std::floor(e.pos[0])), int(std::floor(e.pos[1] + 0.3)), int(std::floor(e.pos[2])));
        const BlockInfo& b = registry_.infos()[at];
        e.inWater = b.waterlogged || (b.fluid && registry_.name(at).find("water") != std::string::npos);
        e.bedFacing = e.hasSleepPos ? bedFacingAt(world_, registry_, e.sleepPos[0], e.sleepPos[1], e.sleepPos[2]) : -1;
        tickPoseAnimation(e, move, (e.sharedFlags & 0x80) != 0, e.pose == 5);
    }
    // LivingEntity.aiStep (client): head lerp, walk animation.
    if (e.lerpHeadSteps > 0) {
        e.headYaw = rotLerp(1.0f / float(e.lerpHeadSteps), e.headYaw, e.lerpHeadYaw);
        --e.lerpHeadSteps;
    }
    const double dx = e.pos[0] - e.prevPos[0], dz = e.pos[2] - e.prevPos[2];
    const std::string& n = e.info ? e.info->name : std::string();
    const bool flier = n == "minecraft:bat" || n == "minecraft:bee" || n == "minecraft:allay" || n == "minecraft:vex" ||
                       n == "minecraft:parrot" || n == "minecraft:ghast" || n == "minecraft:blaze" ||
                       n == "minecraft:phantom" || n == "minecraft:happy_ghast";
    const double dy = flier ? e.pos[1] - e.prevPos[1] : 0.0;
    if (e.vehicle < 0 && !e.dying) {
        const float distance = float(std::sqrt(dx * dx + dy * dy + dz * dz));
        const float target = std::min(distance * 4.0f, 1.0f);
        e.walkSpeedOld = e.walkSpeed;
        e.walkSpeed += (target - e.walkSpeed) * 0.4f;
        e.walkPos += e.walkSpeed;
        e.walkScale = e.baby ? 3.0f : 1.0f;
    } else {
        e.walkSpeedOld = e.walkSpeed = e.walkPos = 0;
    }
    // LivingEntity.tick: body rotation follows the walking direction, limited relative to the head.
    float bodyTarget = e.bodyYaw;
    if (float(dx * dx + dz * dz) > 0.0025000002f) {
        const float walkDirection = float(std::atan2(dz, dx)) * (180.0f / kPi) - 90.0f;
        const float diff = std::abs(wrapDegrees(wrapDegrees(e.yaw) - walkDirection));
        bodyTarget = (95.0f < diff && diff < 265.0f) ? walkDirection - 180.0f : walkDirection;
    }
    if (e.swingTime >= 0) bodyTarget = e.yaw;
    const float d = wrapDegrees(bodyTarget - e.bodyYaw);
    e.bodyYaw += d * 0.3f;
    // tickHeadTurn: the body stays within 50 degrees of the entity's yRot.
    const float headDiff = wrapDegrees(e.yaw - e.bodyYaw);
    constexpr float kMaxHead = 50.0f;
    if (std::abs(headDiff) > kMaxHead) e.bodyYaw += headDiff - (headDiff > 0 ? kMaxHead : -kMaxHead);
    // Keep the previous values within 180 degrees for interpolation.
    while (e.bodyYaw - e.prevBodyYaw < -180.0f) e.prevBodyYaw -= 360.0f;
    while (e.bodyYaw - e.prevBodyYaw >= 180.0f) e.prevBodyYaw += 360.0f;
    while (e.headYaw - e.prevHeadYaw < -180.0f) e.prevHeadYaw -= 360.0f;
    while (e.headYaw - e.prevHeadYaw >= 180.0f) e.prevHeadYaw += 360.0f;
    while (e.yaw - e.prevYaw < -180.0f) e.prevYaw -= 360.0f;
    while (e.yaw - e.prevYaw >= 180.0f) e.prevYaw += 360.0f;

    // Chicken wing flapping (Chicken.aiStep). A resting mob gets no movement packets: check the ground here.
    if (n == "minecraft:chicken") {
        if (!e.stepCount) {
            std::shared_lock lock(world_.mutex());
            const int by = int(std::floor(e.pos[1] - 0.01));
            const BlockInfo& b = registry_.infos()[world_.block(int(std::floor(e.pos[0])), by, int(std::floor(e.pos[2])))];
            e.onGround = b.collisionCount > 0;
        }
        e.oFlap = e.flap, e.oFlapSpeed = e.flapSpeed;
        e.flapSpeed += (e.onGround ? -1.0f : 4.0f) * 0.3f;
        e.flapSpeed = std::clamp(e.flapSpeed, 0.0f, 1.0f);
        if (!e.onGround && e.flapping < 1.0f) e.flapping = 1.0f;
        e.flapping *= 0.9f;
        e.flap += e.flapping * 2.0f;
    }
    // Creeper swelling (Creeper.tick).
    if (n == "minecraft:creeper") {
        e.oldSwell = e.swell;
        e.swell = std::clamp(e.swell + (e.swellDir > 0 ? 1 : e.swellDir < 0 ? -1 : 0), 0, 30);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Picking and interaction.

PickResult EntityWorld::pick(const double o[3], const double dir[3], double maxDistance) const {
    PickResult best;
    best.distance = maxDistance;
    const float pt = partialTick();
    for (const auto& [id, e] : entities_) {
        if (!e.info) continue;
        bool pickable = e.category == Category::Living || e.category == Category::Player ||
                        e.category == Category::Vehicle || e.category == Category::Block;
        const std::string& n = e.info->name;
        if (n == "minecraft:item_frame" || n == "minecraft:glow_item_frame" || n == "minecraft:painting" ||
            n == "minecraft:leash_knot" || n == "minecraft:end_crystal" || n == "minecraft:interaction" ||
            n == "minecraft:shulker_bullet" || n == "minecraft:fireball" || n == "minecraft:small_fireball" ||
            n == "minecraft:wind_charge" || n == "minecraft:breeze_wind_charge")
            pickable = true;
        if (n == "minecraft:ender_dragon" || (n == "minecraft:armor_stand" && (e.standFlags & 16))) pickable = false;
        if (!pickable || e.deathTime > 0) continue;
        double w = e.info->width, h = e.info->height;
        if (e.baby && e.category == Category::Living) w *= 0.5, h *= 0.5;
        if (e.info->dataId("ID_SIZE") >= 0) w *= e.size, h *= e.size;
        if (n == "minecraft:armor_stand" && (e.standFlags & 1)) w *= 0.5, h *= 0.5;
        if (e.category == Category::Player) {
            if (e.pose == 5) h = 1.5;
            else if (e.pose == 1 || e.pose == 3 || e.pose == 4) h = 0.6;
            else if (e.pose == 2) w = h = 0.2;
        }
        double c[3];
        for (int a = 0; a < 3; ++a) c[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        const double lo[3] = {c[0] - w / 2, c[1], c[2] - w / 2}, hi[3] = {c[0] + w / 2, c[1] + h, c[2] + w / 2};
        double t0 = 0, t1 = best.distance;
        bool hit = true;
        for (int a = 0; a < 3 && hit; ++a) {
            if (std::abs(dir[a]) < 1e-12) {
                if (o[a] < lo[a] || o[a] > hi[a]) hit = false;
                continue;
            }
            double ta = (lo[a] - o[a]) / dir[a], tb = (hi[a] - o[a]) / dir[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta), t1 = std::min(t1, tb);
            if (t0 > t1) hit = false;
        }
        if (hit && t0 < best.distance) {
            best.id = id;
            best.distance = t0;
            for (int a = 0; a < 3; ++a) best.hit[a] = o[a] + dir[a] * t0 - c[a];  // relative to the entity
        }
    }
    return best;
}

bool EntityWorld::click(const double eye[3], const double dir[3], bool attack, bool use, double blockDistance,
                        double reach, bool sneaking) {
    const double limit = blockDistance >= 0 ? std::min(blockDistance, reach) : reach;
    const PickResult p = pick(eye, dir, limit);
    if (verbose_)
        LOG("entities: click attack=%d use=%d block=%.2f -> entity %d at %.2f", attack, use, blockDistance, p.id, p.distance);
    if (p.id < 0) return false;
    if (attack) this->attack(p.id);
    else if (use) this->use(p.id, p.hit, sneaking);
    return true;
}

bool EntityWorld::attack(int id) {
    if (!client_ || !client_->alive()) return false;
    auto it = entities_.find(id);
    // The server disconnects clients that attack items, experience orbs or arrows.
    if (it == entities_.end() || it->second.category == Category::Item || it->second.category == Category::Projectile)
        return false;
    net::Writer w;
    w.varInt(id);
    const bool ok = client_->sendPlay("attack", w);
    LOG("entities: attack %d (%s)", id, it->second.info ? it->second.info->name.c_str() : "?");
    client_->sendPlay("punch", net::Writer{});
    return ok;
}

bool EntityWorld::use(int id, const double hit[3], bool sneaking) {
    if (!client_ || !client_->alive()) return false;
    net::Writer w;
    w.varInt(id);
    w.varInt(0);  // main hand
    writeLpVec3(w, hit);
    w.boolean(sneaking);
    return client_->sendPlay("interact", w);
}

}  // namespace ent
