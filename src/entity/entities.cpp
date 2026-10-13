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
bool readItemStack(net::Reader& r, i64& item, int& count, i32* dye = nullptr, bool* foil = nullptr, i32* mapId = nullptr,
                   std::string* fireworks = nullptr, std::shared_ptr<const ui::ItemStack>* full = nullptr) {
    if (dye) *dye = -1;
    if (foil) *foil = false;
    if (mapId) *mapId = -1;
    if (gItems) {
        auto stack = std::make_shared<ui::ItemStack>();
        ui::ItemStack& st = *stack;
        const bool ok = gItems->read(r, st);
        if (full && !st.empty()) *full = std::move(stack);
        item = st.empty() ? -1 : st.item;
        count = st.count;
        if (dye) *dye = st.dyedColor >= 0 ? i32(st.dyedColor & 0xFFFFFF) : -1;
        if (foil) *foil = gItems->foil(st);
        static const int mapIdType = gItems->componentType("map_id");
        if (mapId)
            for (const auto& c : st.added)
                if (c.type == mapIdType) {
                    net::Reader v(reinterpret_cast<const u8*>(c.data.data()), c.data.size());
                    const i32 id = v.varInt();
                    if (v.ok) *mapId = id;
                }
        if (fireworks) {
            static const int type = gItems->componentType("fireworks");
            for (const auto& c : st.added)
                if (c.type == type) *fireworks = c.data;
        }
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
        d.dangerous = id({"DATA_DANGEROUS"});
        d.cloudRadius = id({"DATA_RADIUS"});
        d.cloudWaiting = id({"DATA_WAITING"});
        d.cloudParticle = id({"DATA_PARTICLE"});
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
        d.color = id({"DATA_COLOR", "DATA_COLOR_ID"});
        d.attachFace = id({"DATA_ATTACH_FACE_ID"});
        d.peek = id({"DATA_PEEK_ID"});
        if (t.name == "minecraft:block_display" || t.name == "minecraft:item_display" || t.name == "minecraft:text_display") {
            d.dStart = id({"DATA_TRANSFORMATION_INTERPOLATION_START_DELTA_TICKS_ID"});
            d.dDuration = id({"DATA_TRANSFORMATION_INTERPOLATION_DURATION_ID"});
            d.dPosRot = id({"DATA_POS_ROT_INTERPOLATION_DURATION_ID"});
            d.dTranslation = id({"DATA_TRANSLATION_ID"}), d.dScale = id({"DATA_SCALE_ID"});
            d.dLeft = id({"DATA_LEFT_ROTATION_ID"}), d.dRight = id({"DATA_RIGHT_ROTATION_ID"});
            d.dBillboard = id({"DATA_BILLBOARD_RENDER_CONSTRAINTS_ID"}), d.dBrightness = id({"DATA_BRIGHTNESS_OVERRIDE_ID"});
            d.dViewRange = id({"DATA_VIEW_RANGE_ID"}), d.dWidth = id({"DATA_WIDTH_ID"}), d.dHeight = id({"DATA_HEIGHT_ID"});
            d.dBlock = id({"DATA_BLOCK_STATE_ID"}), d.dItem = id({"DATA_ITEM_STACK_ID"}), d.dItemContext = id({"DATA_ITEM_DISPLAY_ID"});
            d.dText = id({"DATA_TEXT_ID"}), d.dLineWidth = id({"DATA_LINE_WIDTH_ID"});
            d.dBackground = id({"DATA_BACKGROUND_COLOR_ID"}), d.dOpacity = id({"DATA_TEXT_OPACITY_ID"});
            d.dStyle = id({"DATA_STYLE_FLAGS_ID"});
        }
        d.fireworksItem = id({"DATA_ID_FIREWORKS_ITEM"});
        d.staysStill = id({"STAYS_STILL"});
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
                                   "leash_knot", "evoker_fangs", "end_crystal", "ominous_item_spawner", "cushion"};
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
void readEntityData(net::Reader& r, EventQueue& q, int entityEffect,
                    const std::function<bool(net::Reader& r, std::string& raw)>& readParticle) {
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
            case 5:  // component: kept as NBT (text displays lay out the styled text)
                q.strings.emplace_back(r.nbt()), v.str = i32(q.strings.size() - 1);
                break;
            case 6:  // optional component
                if (r.boolean()) q.strings.push_back(net::componentText(r.nbt(), nullptr));
                else q.strings.emplace_back();
                v.str = i32(q.strings.size() - 1);
                break;
            case 7: {  // item stack
                int count = 0;
                i32 dye = -1, mapId = -1;
                bool foil = false;
                std::string fireworks;
                std::shared_ptr<const ui::ItemStack> full;
                const bool complete = readItemStack(r, v.i, count, &dye, &foil, &mapId, &fireworks, &full);
                v.f[0] = float(count), v.f[1] = float(dye), v.f[2] = foil ? 1.0f : 0.0f, v.f[3] = float(mapId);
                if (full) q.stacks.push_back(std::move(full)), v.stack = i32(q.stacks.size() - 1);
                if (!fireworks.empty()) q.strings.push_back(std::move(fireworks)), v.str = i32(q.strings.size() - 1);
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
            case 16:  // particle (the area effect cloud's): kept encoded when the particles can read it
                if (readParticle) {
                    std::string raw;
                    if (readParticle(r, raw)) {
                        q.strings.push_back(std::move(raw));
                        v.str = i32(q.strings.size() - 1);
                    } else {
                        stop = true;
                    }
                    break;
                }
                [[fallthrough]];
            case 17: {  // particles: only argument-less and colour particles can be skipped
                const i32 n = v.serializer == 16 ? 1 : r.varInt();
                for (i32 i = 0; i < n && r.ok && !stop; ++i) {
                    const i32 type = r.varInt();
                    if (type == entityEffect) r.skip(4);
                    else stop = true;
                }
                keep = false;
                break;
            }
            case 18: v.i = r.varInt(), v.f[0] = float(r.varInt()), v.f[1] = float(r.varInt()); break;  // villager data
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
    on("set_entity_data", [this, effect](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Data;
        e.id = r.varInt();
        e.first = u32(q.meta.size());
        readEntityData(r, q, effect, readParticle);
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
            std::shared_ptr<const ui::ItemStack> full;
            const bool complete = readItemStack(r, item, count, &dye, &foil, nullptr, nullptr, &full);
            if (!r.ok) break;
            q.ints.push_back(slot & 127), q.ints.push_back(i32(item)), q.ints.push_back(dye), q.ints.push_back(foil);
            q.ints.push_back(full ? i32(q.stacks.size()) : -1);
            if (full) q.stacks.push_back(std::move(full));
            if (!complete || !(slot & 0x80)) break;
        }
        e.count = (u32(q.ints.size()) - e.first) / 5;
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
        e.a = r.varInt();      // hand
        e.b = r.varInt();      // SwingAnimation: type (0 none, 1 whack, 2 stab), duration
        e.count = u32(r.varInt());
        if (r.ok) q.events.push_back(e);
    });
    on("animate", [](net::Reader& r, EventQueue& q) {  // wake up / critical hit particles
        Event e;
        e.type = EventType::Animate;
        e.id = r.varInt();
        e.a = r.u8v();
        if (r.ok) q.events.push_back(e);
    });
    on("hurt_animation", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Hurt;
        e.id = r.varInt();
        e.f[0] = r.f32();
        if (r.ok) q.events.push_back(e);
    });
    on("projectile_power", [](net::Reader& r, EventQueue& q) {  // ClientboundProjectilePowerPacket
        Event e;
        e.type = EventType::ProjectilePower;
        e.id = r.varInt();
        e.d[0] = r.f64();
        if (r.ok) q.events.push_back(e);
    });
    on("add_transient_block", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::TransientBlock;
        const i64 p = i64(r.u64v());  // BlockPos.asLong: x 26 bits, z 26, y 12
        e.a = r.varInt();
        e.d[0] = double(p >> 38), e.d[1] = double((p << 52) >> 52), e.d[2] = double((p << 26) >> 38);
        if (r.ok) q.events.push_back(e);
    });
    on("set_entity_link", [](net::Reader& r, EventQueue& q) {  // ClientboundSetEntityLinkPacket: source, holder (ints)
        Event e;
        e.type = EventType::EntityLink;
        e.id = r.i32v();
        e.a = r.i32v();
        if (r.ok) q.events.push_back(e);
    });
    on("damage_event", [](net::Reader& r, EventQueue& q) {
        Event e;
        e.type = EventType::Damage;
        e.id = r.varInt();
        e.a = r.varInt();      // damage_type network id (Client::registryId("minecraft:damage_type", ...))
        r.varInt(), r.varInt();  // cause / direct entity id + 1
        if (r.boolean()) r.f64(), r.f64(), r.f64();  // source position
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
    client.onPlayPeek("set_time", [this](net::Reader& r) {  // the game time (species animations)
        const i64 t = r.i64v();
        if (r.ok) netGameTime_ = t;
    });
    client.onPlayPeek("game_event", [this](net::Reader& r) {  // rain / thunder levels (scared pandas)
        const int type = r.u8v();
        const float param = r.f32();
        if (!r.ok) return;
        if (type == 1) rainLevel_ = 0;  // START_RAINING (the level rises from 0)
        else if (type == 2) rainLevel_ = 1;  // STOP_RAINING
        else if (type == 7) rainLevel_ = param;
        else if (type == 8) thunderLevel_ = param;
        setSpeciesThundering(thunderLevel_.load() * rainLevel_.load() > 0.9f);
    });
    on("level_event", [](net::Reader& r, EventQueue& q) {  // jukebox songs: parrots dance, allays follow
        const i32 type = r.i32v();
        int x, y, z;
        r.blockPos(x, y, z);
        if (!r.ok || (type != 1010 && type != 1011)) return;
        Event e;
        e.type = EventType::Jukebox;
        e.d[0] = x, e.d[1] = y, e.d[2] = z;
        e.flag = type == 1010;
        q.events.push_back(e);
    });
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
    // Displays: LinearInterpolationHandler over teleport_duration steps (0: placed at once).
    const bool linear = e.category == Category::Vehicle || (e.info && e.info->name == "minecraft:experience_orb") ||
                        (e.display.is && e.display.posRotDuration > 0);
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
        e.linearRemaining = e.display.is ? e.display.posRotDuration : 3;
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
    if (ev.type == EventType::TransientBlock) {  // ClientPacketListener.handleAddTransientBlockPacket
        std::erase_if(transientBlocks, [&](const TransientBlock& t) { return t.x == int(ev.d[0]) && t.y == int(ev.d[1]) && t.z == int(ev.d[2]); });
        transientBlocks.push_back({int(ev.d[0]), int(ev.d[1]), int(ev.d[2]), ev.a, nowSeconds() + 1.0, meshSerial ? meshSerial() : 0});
        return;
    }
    if (ev.type == EventType::PlayerInfo) {
        PlayerInfo& p = players_[uuidKey(ev.uuid)];
        if (ev.a >= 0 && size_t(ev.a) < q.strings.size()) p.name = q.strings[size_t(ev.a)];
        const int skin = defaultSkinIndex(ev.uuid);
        static const char* kSkins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};
        p.slim = skin < 9;
        p.skin = assets_.texture(std::string("entity/player/") + (p.slim ? "slim/" : "wide/") + kSkins[skin % 9]);
        return;
    }
    if (ev.type == EventType::Jukebox) {  // LevelRenderer.notifyNearbyEntities: living entities within 3 blocks
        const int pos[3] = {int(ev.d[0]), int(ev.d[1]), int(ev.d[2])};
        for (auto& [id, other] : entities_)
            if (other.category == Category::Living && other.pos[0] > pos[0] - 3 && other.pos[0] < pos[0] + 4 &&
                other.pos[1] > pos[1] - 3 && other.pos[1] < pos[1] + 4 && other.pos[2] > pos[2] - 3 && other.pos[2] < pos[2] + 4)
                speciesJukebox(other, pos, ev.flag);
        return;
    }
    if (ev.type == EventType::PlayerRemove) {
        players_.erase(uuidKey(ev.uuid));
        return;
    }
    if (ev.type == EventType::Remove) {
        for (u32 i = 0; i < ev.count; ++i) {
            auto r = entities_.find(q.ints[ev.first + i]);
            if (r != entities_.end() && r->second.pickupBy == INT_MIN) entities_.erase(r);  // picked up: goes by itself
        }
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
        if (e.info && (e.info->name == "minecraft:item_frame" || e.info->name == "minecraft:glow_item_frame")) {
            // ItemFrame.recreateFromPacket: the packet holds the block position; the frame sits at the block's centre
            // moved 0.46875 back against the wall it hangs on (createBoundingBox).
            static constexpr int kDir[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
            const int d = std::clamp(ev.b, 0, 5);
            for (int a = 0; a < 3; ++a)
                e.pos[a] = e.prevPos[a] = std::floor(ev.d[a]) + 0.5 - kDir[d][a] * 0.46875;
        }
        e.display.is = size_t(ev.a) < dataIds_.size() && dataIds_[size_t(ev.a)].dTranslation >= 0;
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
        // ClientPacketListener.handleTakeItemEntity: a fully collected item flies to the collector (ItemPickupParticle)
        // and goes after 3 ticks; a partly collected stack shrinks.
        if (it != entities_.end() && it->second.category == Category::Item && it->second.pickupBy == INT_MIN) {
            Entity& item = it->second;
            if (ev.b > 0 && item.itemCount > ev.b) item.itemCount -= ev.b;
            else item.pickupBy = ev.a, item.pickupLife = 0, item.stepCount = 0, item.linearRemaining = 0;
        }
        return;
    }
    if (it == entities_.end()) {
        if (ev.type == EventType::EntityEvent && onUntrackedEntityEvent) onUntrackedEntityEvent(ev.id, ev.a);
        return;
    }
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
                const int slot = q.ints[ev.first + i * 5];
                if (slot < 0 || slot >= 8) continue;
                e.equipment[slot] = q.ints[ev.first + i * 5 + 1];
                e.equipDye[slot] = q.ints[ev.first + i * 5 + 2];
                e.equipFoil = u8((e.equipFoil & ~(1 << slot)) | (q.ints[ev.first + i * 5 + 3] ? 1 << slot : 0));
                const i32 stack = q.ints[ev.first + i * 5 + 4];
                e.equipStack[slot] = stack >= 0 ? q.stacks[size_t(stack)] : nullptr;
            }
            break;
        case EventType::Swing:
            e.swingTime = 0, e.swingHand = ev.a == 1 ? 1 : 0, e.swingType = int(ev.b);
            e.swingDuration = std::max<int>(int(ev.count), 1);
            break;
        case EventType::Hurt:
            e.hurtTime = 10;
            if (verbose_) LOG("entities: %d hurt animation", e.id);
            break;
        case EventType::ProjectilePower: e.accelerationPower = ev.d[0]; break;  // handleProjectilePowerPacket
        case EventType::EntityLink: e.leashHolder = ev.a; break;               // Leashable.setDelayedLeashHolderId
        case EventType::Damage:  // ClientPacketListener.handleDamageEvent -> animateHurt
            e.hurtTime = 10;
            if (verbose_) LOG("entities: %d damaged", e.id);
            break;
        case EventType::EntityEvent:
            if (ev.a == 3 && e.category == Category::Living) e.dying = true;  // death
            if (ev.a == 2 && (e.kineticHitTicks < 0 || e.kineticHitTicks > 10)) e.kineticHitTicks = 0;  // KINETIC_HIT
            speciesEvent(e, ev.a);
            if (onEntityEvent) onEntityEvent(e, ev.a);
            break;
        case EventType::Animate:
            if (onAnimate) onAnimate(e, ev.a);
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
        if (e.display.is && applyDisplayData(e, d, v, q)) continue;
        if (idx < SpeciesState::kData) e.sp.data[idx] = v.i, e.sp.dataF[idx] = v.f[0], e.sp.seen |= 1u << idx;
        if (idx == d.flags) e.sharedFlags = u8(v.i);
        else if (idx == d.silent && v.serializer == 8) e.silent = v.i != 0;
        else if (idx == d.staysStill && v.serializer == 8) e.staysStill = v.i != 0;
        else if (idx == d.fireworksItem && v.serializer == 7) e.fireworks = v.str >= 0 ? q.strings[size_t(v.str)] : std::string();
        else if (idx == d.mobFlags) e.mobFlags = u8(v.i);
        else if (idx == d.noGravity) e.noGravity = v.i != 0;
        else if (idx == d.dangerous) e.dangerous = v.i != 0;
        else if (idx == d.cloudRadius) e.cloudRadius = v.f[0];
        else if (idx == d.cloudWaiting) e.cloudWaiting = v.i != 0;
        else if (idx == d.cloudParticle && v.str >= 0 && size_t(v.str) < q.strings.size()) e.cloudParticle = q.strings[size_t(v.str)];
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
            e.itemId = int(v.i), e.itemCount = std::max(1, int(v.f[0])), e.itemDye = i32(v.f[1]), e.itemFoil = v.f[2] > 0.5f,
            e.mapId = int(v.f[3]), e.itemStack = v.stack >= 0 ? q.stacks[size_t(v.stack)] : nullptr;
        else if (idx == d.rotation && v.serializer == 1) e.rotation = int(v.i);
        else if (idx == d.painting && v.serializer == 34) e.painting = int(v.i);
        else if (idx == d.nameVisible && v.serializer == 8) e.nameVisible = v.i != 0;
        else if (idx == d.size) e.size = std::max(1, int(v.i));
        else if (idx == d.swellDir) e.swellDir = int(v.i);
        else if (idx == d.powered) e.powered = v.i != 0;
        else if (idx == d.mainHand) e.mainArm = int(v.i);
        else if (idx == d.skinParts) e.skinParts = int(v.i);
        else if (idx == d.villagerData) e.villagerType = int(v.i), e.variant2 = int(v.f[0]), e.villagerLevel = int(v.f[1]);
        else if (idx == d.tameFlags && (name == "minecraft:wolf" || name == "minecraft:cat" || name == "minecraft:nautilus" ||
                                        name == "minecraft:zombie_nautilus"))
            e.tame = (v.i & 4) != 0;  // TamableAnimal.DATA_FLAGS_ID
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
        else if (idx == d.color) e.dyeColor = int(v.i);
        else if (idx == d.attachFace) e.attachFace = int(v.i);
        else if (idx == d.peek) e.peekRaw = int(v.i);
        else if (idx == d.mainGene) e.variant = int(v.i);
        else if (idx == d.hiddenGene) e.variant2 = int(v.i);
        speciesDataChanged(e, idx);
    }
}

// Display.onSyncedDataUpdated (and the BlockDisplay / ItemDisplay / TextDisplay overrides).
bool EntityWorld::applyDisplayData(Entity& e, const DataIds& d, const MetaValue& v, const EventQueue& q) {
    DisplayState& s = e.display;
    const int idx = v.index;
    auto copy = [&](float* out, int n) { std::copy(v.f, v.f + n, out); };
    if (idx == d.dStart) s.startDelta = int(v.i), s.updateStart = true;
    else if (idx == d.dDuration) s.duration = int(v.i), s.updateDuration = true;
    else if (idx == d.dPosRot) s.posRotDuration = int(v.i);
    else if (idx == d.dTranslation) copy(s.synched.t, 3), s.updateRender = true;
    else if (idx == d.dScale) copy(s.synched.s, 3), s.updateRender = true;
    else if (idx == d.dLeft) copy(s.synched.l, 4), s.updateRender = true;
    else if (idx == d.dRight) copy(s.synched.r, 4), s.updateRender = true;
    else if (idx == d.dBillboard) s.billboard = u8(v.i), s.updateRender = true;
    else if (idx == d.dBrightness) s.brightness = int(v.i), s.updateRender = true;
    else if (idx == d.dViewRange) s.viewRange = v.f[0];
    else if (idx == d.dWidth) s.width = v.f[0];
    else if (idx == d.dHeight) s.height = v.f[0];
    else if (idx == d.dBlock && v.serializer == 14) e.blockState = int(v.i), s.updateRender = true;
    else if (idx == d.dItem && v.serializer == 7) {
        e.itemId = int(v.i), e.itemCount = std::max(1, int(v.f[0])), e.itemDye = i32(v.f[1]), e.itemFoil = v.f[2] > 0.5f;
        e.itemStack = v.stack >= 0 ? q.stacks[size_t(v.stack)] : nullptr;
        s.updateRender = true;
    } else if (idx == d.dItemContext) s.itemContext = int(v.i), s.updateRender = true;
    else if (idx == d.dText && v.str >= 0) s.text = q.strings[size_t(v.str)], ++s.textVersion, s.updateRender = true;
    else if (idx == d.dLineWidth) s.lineWidth = int(v.i), ++s.textVersion, s.updateRender = true;
    else if (idx == d.dBackground) s.background = int(v.i), s.updateRender = true;
    else if (idx == d.dOpacity) s.opacity = int(i8(u8(v.i))), s.updateRender = true;
    else if (idx == d.dStyle) s.style = u8(v.i), ++s.textVersion, s.updateRender = true;
    else return false;
    return true;
}

namespace {

// JOML Quaternionf.slerp (Transformation.slerp: translation / scale lerp, rotations slerp).
void slerp(const float a[4], const float b[4], float alpha, float out[4]) {
    const float cosom = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float absCosom = std::abs(cosom);
    float scale0, scale1;
    if (1.0f - absCosom > 1e-6f) {
        const float sinSqr = 1.0f - absCosom * absCosom, sinom = 1.0f / std::sqrt(sinSqr);
        const float omega = std::atan2(sinSqr * sinom, absCosom);
        scale0 = std::sin((1.0f - alpha) * omega) * sinom, scale1 = std::sin(alpha * omega) * sinom;
    } else {
        scale0 = 1.0f - alpha, scale1 = alpha;
    }
    if (cosom < 0) scale1 = -scale1;
    for (int k = 0; k < 4; ++k) out[k] = scale0 * a[k] + scale1 * b[k];
}
DisplayState::Xform xformAt(const DisplayState& s, float progress) {
    if (progress >= 1.0f) return s.to;
    DisplayState::Xform x;
    for (int k = 0; k < 3; ++k)
        x.t[k] = s.from.t[k] + (s.to.t[k] - s.from.t[k]) * progress, x.s[k] = s.from.s[k] + (s.to.s[k] - s.from.s[k]) * progress;
    slerp(s.from.l, s.to.l, progress, x.l);
    slerp(s.from.r, s.to.r, progress, x.r);
    return x;
}
// Row-major 3x3 of a quaternion like JOML Matrix4f.rotation (not normalised).
void quatMatrix(const float q[4], float m[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float w2 = w * w, x2 = x * x, y2 = y * y, z2 = z * z;
    const float dzw = 2 * z * w, dxy = 2 * x * y, dxz = 2 * x * z, dyw = 2 * y * w, dyz = 2 * y * z, dxw = 2 * x * w;
    m[0] = w2 + x2 - z2 - y2, m[1] = dxy - dzw, m[2] = dyw + dxz;
    m[3] = dxy + dzw, m[4] = y2 - z2 + w2 - x2, m[5] = dyz - dxw;
    m[6] = dxz - dyw, m[7] = dyz + dxw, m[8] = z2 - y2 - x2 + w2;
}
void mul33(const float a[9], const float b[9], float out[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}
int lerpByte(float a, int from, int to) { return from + int(std::floor(a * float(to - from))); }  // Mth.lerpInt
int lerpArgb(float a, int from, int to) {  // ARGB.srgbLerp
    u32 c = 0;
    for (int k = 0; k < 32; k += 8) c |= u32(lerpByte(a, (from >> k) & 255, (to >> k) & 255) & 255) << k;
    return int(c);
}

}  // namespace

void displayTextColors(const Entity& e, float pt, u32& background, int& opacity) {
    const DisplayState& s = e.display;
    const float p = displayProgress(e, pt);
    background = u32(lerpArgb(p, s.background0, s.background1));
    opacity = lerpByte(p, s.opacity0, s.opacity1);
}

float displayProgress(const Entity& e, float pt) {
    const DisplayState& s = e.display;
    if (s.activeDuration <= 0) return 1.0f;
    const float t = float(e.age) - float(s.startTick) + pt;
    return std::clamp(t / float(s.activeDuration), 0.0f, 1.0f);
}

bool displayMatrix(const Entity& e, float pt, float camYaw, float camPitch, float out[12]) {
    const DisplayState& s = e.display;
    if (!s.hasRender) return false;
    const DisplayState::Xform x = xformAt(s, displayProgress(e, pt));
    // DisplayRenderer.calculateOrientation: rotationYXZ(-yRot, xRot, 0) of the entity or the camera.
    float yaw = e.prevYaw + wrapDegrees(e.yaw - e.prevYaw) * pt, pitch = e.prevPitch + (e.pitch - e.prevPitch) * pt;
    if (s.billboard == 1 || s.billboard == 3) yaw = camYaw - 180.0f;  // vertical, center
    if (s.billboard == 2 || s.billboard == 3) pitch = -camPitch;       // horizontal, center
    const float ya = -yaw * kPi / 180.0f, xa = pitch * kPi / 180.0f;
    const float cy = std::cos(ya), sy = std::sin(ya), cx = std::cos(xa), sx = std::sin(xa);
    const float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy}, rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
    float orient[9], left[9], right[9], tmp[9], m[9];
    mul33(ry, rx, orient);
    // Transformation.getMatrix: translate(t) * rotate(left) * scale(s) * rotate(right).
    quatMatrix(x.l, left);
    quatMatrix(x.r, right);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) left[i * 3 + j] *= x.s[j];
    mul33(left, right, tmp);
    mul33(orient, tmp, m);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i * 4 + j] = m[i * 3 + j];
        out[i * 4 + 3] = orient[i * 3] * x.t[0] + orient[i * 3 + 1] * x.t[1] + orient[i * 3 + 2] * x.t[2];
    }
    return true;
}

u32 displayLight(const Entity& e, u32 worldLight) {
    const int b = e.display.brightness;
    if (b == -1) return worldLight;
    return u32(((b >> 20) & 15) << 4 | ((b >> 4) & 15));
}

void EntityWorld::tick() {
    if (const i64 t = netGameTime_.exchange(INT64_MIN); t != INT64_MIN) gameTime_ = t;
    else ++gameTime_;
    for (auto& [id, e] : entities_)  // TickRateManager.isEntityFrozen: all but players and what they drive
        if (tickRate.runs || e.category == Category::Player || e.id == localControl_) tickEntity(e);
    for (auto it = entities_.begin(); it != entities_.end();)  // ItemPickupParticle.LIFE_TIME
        it = it->second.pickupBy != INT_MIN && it->second.pickupLife >= 3 ? entities_.erase(it) : std::next(it);
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
    if (e.pickupBy != INT_MIN) {  // flying to its collector: the visuals move it, nothing else changes
        ++e.pickupLife;
        return;
    }
    ++e.age;
    if (e.display.is) {  // Display.tick (client side)
        DisplayState& s = e.display;
        const float last = displayProgress(e, 0.0f);  // lastProgress: the progress the last frame drew
        if (s.updateStart) s.updateStart = false, s.startTick = e.age + s.startDelta;
        if (s.updateDuration) s.updateDuration = false, s.activeDuration = s.duration;
        if (s.updateRender) {
            s.updateRender = false;
            if (s.activeDuration != 0 && s.hasRender) {  // createInterpolatedRenderState from the current state
                s.from = xformAt(s, last);
                s.background0 = lerpArgb(last, s.background0, s.background1), s.opacity0 = lerpByte(last, s.opacity0, s.opacity1);
            } else {
                s.from = s.synched, s.background0 = s.background, s.opacity0 = s.opacity;
            }
            s.to = s.synched, s.background1 = s.background, s.opacity1 = s.opacity;
            s.hasRender = true;
        }
    }
    if (e.hurtTime > 0) --e.hurtTime;
    if (e.vehicleHurt > 0) --e.vehicleHurt;
    if (e.vehicleDamage > 0) e.vehicleDamage = std::max(0.0f, e.vehicleDamage - 1.0f);
    if (e.dying && e.deathTime < 20) ++e.deathTime;
    if (e.swingTime >= 0 && ++e.swingTime >= e.swingDuration) e.swingTime = -1;
    if (e.kineticHitTicks >= 0) ++e.kineticHitTicks;
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
        const std::string_view name = e.info ? std::string_view(e.info->name) : std::string_view();
        const bool windCharge = name == "minecraft:wind_charge" || name == "minecraft:breeze_wind_charge";
        if (windCharge || name == "minecraft:fireball" || name == "minecraft:small_fireball" || name == "minecraft:dragon_fireball" ||
            name == "minecraft:wither_skull") {
            // AbstractHurtingProjectile.tick: applyInertia (the velocity grows by accelerationPower along itself, then
            // the inertia: 0.95, 0.8 in water, a dangerous wither skull 0.73, wind charges 1), then the move.
            bool water = false;
            {
                std::shared_lock lock(world_.mutex());
                const u16 at = world_.block(int(std::floor(e.pos[0])), int(std::floor(e.pos[1])), int(std::floor(e.pos[2])));
                const BlockInfo& b = registry_.infos()[at];
                water = b.waterlogged || (b.fluid && registry_.name(at).find("water") != std::string::npos);
            }
            const double inertia = windCharge ? 1.0 : water ? 0.8 : name == "minecraft:wither_skull" && e.dangerous ? 0.73 : 0.95;
            const double len = std::sqrt(e.vel[0] * e.vel[0] + e.vel[1] * e.vel[1] + e.vel[2] * e.vel[2]);
            const double power = windCharge ? 0.0 : e.accelerationPower;
            for (double& v : e.vel) v = (v + (len < 1e-4 ? 0.0 : v / len * power)) * inertia;
            for (int a = 0; a < 3; ++a) e.pos[a] += e.vel[a];
            const double h = std::sqrt(e.vel[0] * e.vel[0] + e.vel[2] * e.vel[2]);
            if (h > 1e-4 || std::abs(e.vel[1]) > 1e-4) {  // ProjectileUtil.rotateTowardsMovement(0.2)
                e.yaw = rotLerp(0.2f, e.yaw, float(std::atan2(e.vel[0], e.vel[2]) * 180.0 / kPi));
                e.pitch = lerpf(0.2f, e.pitch, float(std::atan2(e.vel[1], h) * 180.0 / kPi));
            }
            return tickSpecies(e, gameTime_, entities_);
        }
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

    if (e.info && e.info->name == "minecraft:area_effect_cloud" && spawnParticle) {  // AreaEffectCloud.clientTick
        static std::mt19937 rng(0xC10D);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        if (!e.cloudWaiting || unit(rng) < 0.5f) {
            std::string particle = e.cloudParticle;
            std::string white;  // DEFAULT_PARTICLE: entity_effect, colour -1
            {
                net::Writer w;
                w.varInt(entityEffectParticle_);
                w.u32v(0xFFFFFFFFu);
                white.assign(w.buf.begin(), w.buf.end());
            }
            if (particle.empty()) particle = white;
            net::Reader pr(reinterpret_cast<const u8*>(particle.data()), particle.size());
            const bool effect = pr.varInt() == entityEffectParticle_;
            const int count = e.cloudWaiting ? 2 : int(std::ceil(kPi * e.cloudRadius * e.cloudRadius));
            const float radius = e.cloudWaiting ? 0.2f : e.cloudRadius;
            for (int i = 0; i < count; ++i) {
                const float angle = unit(rng) * 2.0f * kPi, dist = std::sqrt(unit(rng)) * radius;
                const double x = e.pos[0] + std::cos(angle) * dist, y = e.pos[1], z = e.pos[2] + std::sin(angle) * dist;
                if (effect) spawnParticle(e.cloudWaiting && unit(rng) < 0.5f ? white : particle, x, y, z, 0, 0, 0);
                else if (e.cloudWaiting) spawnParticle(particle, x, y, z, 0, 0, 0);
                else spawnParticle(particle, x, y, z, (0.5 - unit(rng)) * 0.15, 0.01f, (0.5 - unit(rng)) * 0.15);
            }
        }
    }
    // AbstractBoat.tick: the paddles turn pi/8 per tick while rowing (getRowingTime lerps from the previous step).
    for (int i = 0; i < 2; ++i) e.paddlePos[i] = e.paddle[i] ? e.paddlePos[i] + kPi / 8 : 0.0f;
    if (e.category != Category::Living && e.category != Category::Player) return tickSpecies(e, gameTime_, entities_);
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
    // calculateEntityAnimation(omnidirectionalAirMover): only bees and parrots count vertical movement.
    const bool flier = n == "minecraft:bee" || n == "minecraft:parrot";
    const double dy = flier ? e.pos[1] - e.prevPos[1] : 0.0;
    if (e.vehicle < 0 && !e.dying) {
        const float distance = float(std::sqrt(dx * dx + dy * dy + dz * dz));
        // updateWalkAnimation and its overrides (Camel, Frog, Creaking).
        float target = std::min(distance * 4.0f, 1.0f), rate = 0.4f;
        if (n == "minecraft:camel" || n == "minecraft:camel_husk")
            target = e.pose == 0 && e.sp.anim[kCamelDash] == INT_MIN ? std::min(distance * 6.0f, 1.0f) : 0.0f, rate = 0.2f;
        else if (n == "minecraft:frog") target = e.sp.anim[kFrogJump] != INT_MIN ? 0.0f : std::min(distance * 25.0f, 1.0f);
        else if (n == "minecraft:creaking") target = std::min(distance * 25.0f, 3.0f);
        e.walkSpeedOld = e.walkSpeed;
        e.walkSpeed += (target - e.walkSpeed) * rate;
        e.walkPos += e.walkSpeed;
        e.walkScale = e.baby && n != "minecraft:creaking" ? 3.0f : 1.0f;
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
    // Shulker.updatePeekAmount: the lid follows the synched peek by 0.05 per tick.
    if (n == "minecraft:shulker") {
        e.bodyYaw = e.prevBodyYaw = 0;  // the body never turns (Shulker.recreateFromPacket, ShulkerBodyRotationControl)
        e.peekO = e.peek;
        const float target = float(e.peekRaw) * 0.01f;
        if (e.peek > target) e.peek = std::clamp(e.peek - 0.05f, target, 1.0f);
        else if (e.peek < target) e.peek = std::clamp(e.peek + 0.05f, 0.0f, target);
    }
    // Creeper swelling (Creeper.tick).
    if (n == "minecraft:creeper") {
        e.oldSwell = e.swell;
        e.swell = std::clamp(e.swell + (e.swellDir > 0 ? 1 : e.swellDir < 0 ? -1 : 0), 0, 30);
    }
    tickSpecies(e, gameTime_, entities_);
}

// ---------------------------------------------------------------------------------------------------------
// Picking and interaction.

void entitySize(const Entity& e, double& w, double& h) {
    w = e.info ? e.info->width : 0.6, h = e.info ? e.info->height : 1.8;
    if (!e.info) return;
    if (e.baby && e.category == Category::Living) w *= 0.5, h *= 0.5;
    if (e.info->dataId("ID_SIZE") >= 0) w *= e.size, h *= e.size;
    if (e.info->name == "minecraft:armor_stand" && (e.standFlags & 1)) w *= 0.5, h *= 0.5;
    if (e.category == Category::Player) {
        if (e.pose == 5) h = 1.5;
        else if (e.pose == 1 || e.pose == 3 || e.pose == 4) h = 0.6;
        else if (e.pose == 2) w = h = 0.2;
    }
}

PickResult EntityWorld::pick(const double o[3], const double dir[3], double maxDistance) const {
    PickResult best;
    best.distance = maxDistance;
    const float pt = partialTick();
    for (const auto& [id, e] : entities_) {
        if (!e.info) continue;
        bool pickable = e.category == Category::Living || e.category == Category::Player ||
                        e.category == Category::Vehicle || e.category == Category::Block;
        const std::string& n = e.info->name;
        if (n == "minecraft:item_frame" || n == "minecraft:glow_item_frame" || n == "minecraft:painting" || n == "minecraft:cushion" ||
            n == "minecraft:leash_knot" || n == "minecraft:end_crystal" || n == "minecraft:interaction" ||
            n == "minecraft:shulker_bullet" || n == "minecraft:fireball" || n == "minecraft:small_fireball" ||
            n == "minecraft:wind_charge" || n == "minecraft:breeze_wind_charge")
            pickable = true;
        if (n == "minecraft:ender_dragon" || (n == "minecraft:armor_stand" && (e.standFlags & 16))) pickable = false;
        if (!pickable || e.deathTime > 0) continue;
        double w, h;
        entitySize(e, w, h);
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

bool EntityWorld::attackInRange(const double eye[3], const double dir[3], double minReach, double maxReach, double margin,
                                double forward, const BlockRay& blockRay) {
    const PickResult p = pickAttackRange(eye, dir, minReach, maxReach, margin, forward, blockRay);
    if (verbose_) LOG("entities: attack range %.2f..%.2f margin %.2f -> %d", minReach, maxReach, margin, p.id);
    if (p.id < 0) {  // a miss: only the swing goes out
        if (client_ && client_->alive()) client_->sendPlay("punch", net::Writer{});
        return true;
    }
    attack(p.id);
    return true;
}

PickResult EntityWorld::pickAttackRange(const double eye[3], const double dir[3], double minReach, double maxReach,
                                        double margin, double forward, const BlockRay& blockRay) const {
    PickResult best;
    double from[3], to[3];
    double toDist = maxReach + std::max(0.0, forward);
    const double blockT = blockRay(eye, dir, toDist);
    if (blockT < minReach) return best;  // a block before the reach starts
    toDist = std::min(toDist, blockT);
    for (int a = 0; a < 3; ++a) from[a] = eye[a] + dir[a] * minReach, to[a] = eye[a] + dir[a] * toDist;
    // Segment entry into a box (AABB.clip): the parameter along from..to, false if it misses.
    auto clip = [](const double lo[3], const double hi[3], const double a[3], const double b[3], double& t) {
        double t0 = 0, t1 = 1;
        for (int k = 0; k < 3; ++k) {
            const double d = b[k] - a[k];
            if (std::abs(d) < 1e-12) {
                if (a[k] < lo[k] || a[k] > hi[k]) return false;
                continue;
            }
            double ta = (lo[k] - a[k]) / d, tb = (hi[k] - a[k]) / d;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta), t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        t = t0;
        return true;
    };
    const float pt = partialTick();
    int bestId = -1;
    double bestDist = 1e30;
    for (const auto& [id, e] : entities_) {
        if (!e.info || e.deathTime > 0) continue;
        const std::string& n = e.info->name;
        // EntitySelector.CAN_BE_PICKED: the entities the crosshair can target (EntityWorld::pick).
        bool pickable = e.category == Category::Living || e.category == Category::Player || e.category == Category::Vehicle ||
                        e.category == Category::Block || n == "minecraft:item_frame" || n == "minecraft:glow_item_frame" ||
                        n == "minecraft:painting" || n == "minecraft:end_crystal" || n == "minecraft:interaction" ||
                        n == "minecraft:shulker_bullet" || n == "minecraft:fireball" || n == "minecraft:small_fireball" ||
                        n == "minecraft:wind_charge" || n == "minecraft:breeze_wind_charge";
        if (n == "minecraft:ender_dragon" || (n == "minecraft:armor_stand" && (e.standFlags & 16))) pickable = false;
        if (!pickable) continue;
        double w, h, c[3];
        entitySize(e, w, h);
        for (int a = 0; a < 3; ++a) c[a] = e.prevPos[a] + (e.pos[a] - e.prevPos[a]) * pt;
        const double lo[3] = {c[0] - w / 2, c[1], c[2] - w / 2}, hi[3] = {c[0] + w / 2, c[1] + h, c[2] + w / 2};
        double hit[3];
        double t;
        const bool inside = from[0] >= lo[0] && from[0] <= hi[0] && from[1] >= lo[1] && from[1] <= hi[1] && from[2] >= lo[2] &&
                            from[2] <= hi[2];
        if (inside) {
            std::copy(from, from + 3, hit);
        } else if (clip(lo, hi, from, to, t)) {
            for (int a = 0; a < 3; ++a) hit[a] = from[a] + (to[a] - from[a]) * t;
        } else if (margin > 0) {
            const double ilo[3] = {lo[0] - margin, lo[1] - margin, lo[2] - margin}, ihi[3] = {hi[0] + margin, hi[1] + margin, hi[2] + margin};
            if (!clip(ilo, ihi, from, to, t)) continue;
            double outside[3], toward[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
            for (int a = 0; a < 3; ++a) outside[a] = from[a] + (to[a] - from[a]) * t;
            double d[3] = {toward[0] - outside[0], toward[1] - outside[1], toward[2] - outside[2]};
            const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-9) {  // a block between the margin and the box stops it
                for (double& k : d) k /= len;
                const double bt = blockRay(outside, d, len);
                if (bt < len)
                    for (int a = 0; a < 3; ++a) toward[a] = outside[a] + d[a] * bt;
            }
            if (!clip(lo, hi, outside, toward, t)) continue;
            for (int a = 0; a < 3; ++a) hit[a] = outside[a] + (toward[a] - outside[a]) * t;  // the surface hit
        } else {
            continue;
        }
        const double dx = hit[0] - eye[0], dy = hit[1] - eye[1], dz = hit[2] - eye[2];
        const double dist = dx * dx + dy * dy + dz * dz;
        if (dist < bestDist) {
            bestDist = dist, bestId = id;
            best.id = id, best.distance = std::sqrt(dist);
            for (int a = 0; a < 3; ++a) best.hit[a] = hit[a] - c[a];  // relative to the entity
        }
    }
    (void)bestId;
    return best;
}

bool EntityWorld::click(const double eye[3], const double dir[3], bool attack, bool use, double blockDistance,
                        double reach, bool sneaking, double minReach) {
    const double limit = blockDistance >= 0 ? std::min(blockDistance, reach) : reach;
    const PickResult p = pick(eye, dir, limit);
    if (verbose_)
        LOG("entities: click attack=%d use=%d block=%.2f -> entity %d at %.2f", attack, use, blockDistance, p.id, p.distance);
    if (p.id < 0) return false;
    if (attack && p.distance < minReach) {  // AttackRange.isInRange: too close for the weapon, only the swing goes out
        if (client_ && client_->alive()) client_->sendPlay("punch", net::Writer{});
        return true;
    }
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
