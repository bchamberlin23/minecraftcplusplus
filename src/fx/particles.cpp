#include "fx/particles.h"

#include "audio/audio.h"

#include "assets/assets.h"
#include "assets/game_reports.h"
#include "entity/item_models.h"
#include "net/client.h"
#include "net/protocol.h"
#include "ui/resources.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <shared_mutex>

namespace fx {

namespace {
constexpr float kPi = 3.14159265f;

enum class Kind : u8 {
    None, Generic, Terrain, Crumble, DustPillar, FallingDust, Rain, Splash, Poof, Explosion, ExplosionSeed, Smoke,
    LargeSmoke, WhiteSmoke, Flame, SmallFlame, Crit, DamageIndicator, EnchantedHit, Heart, AngryVillager, Bubble,
    BubblePop, CherryLeaves, PaleLeaves, TintedLeaves, Portal, Enchant, CampfireCosy, CampfireSignal, Spell, Witch,
    Suspended, HappyVillager, Lava, Dust, DustTransition, Note, Cloud, Snowflake, Item, Vibration, Drip, Spark, Flash,
    Totem, EndRod, Gust, GustSeed, Rising, SculkCharge, SculkChargePop, Shriek, Firefly, Glow
};
enum class Opt : u8 { None, Block, Dust, DustTransition, Spell, Color, Float, VarInt, Int, IntFloat, Trail, Item, Vibration };
enum SizeMode : u8 { SizeConst, SizeGrow, SizeFlame, SizePortal, SizeLava };
enum Flags : u8 { Physics = 1, OnGround = 2, SpeedUp = 4, Stopped = 8, Removed = 16, Translucent = 32, NoRender = 64, AgeSprite = 128 };

const std::pair<const char*, Kind> kKinds[] = {
    {"block", Kind::Terrain}, {"block_crumble", Kind::Crumble}, {"dust_pillar", Kind::DustPillar},
    {"falling_dust", Kind::FallingDust}, {"rain", Kind::Rain}, {"splash", Kind::Splash}, {"poof", Kind::Poof},
    {"explosion", Kind::Explosion}, {"explosion_emitter", Kind::ExplosionSeed}, {"smoke", Kind::Smoke},
    {"large_smoke", Kind::LargeSmoke}, {"white_smoke", Kind::WhiteSmoke}, {"flame", Kind::Flame},
    {"soul_fire_flame", Kind::Flame}, {"copper_fire_flame", Kind::Flame}, {"small_flame", Kind::SmallFlame},
    {"crit", Kind::Crit}, {"damage_indicator", Kind::DamageIndicator}, {"enchanted_hit", Kind::EnchantedHit},
    {"heart", Kind::Heart}, {"angry_villager", Kind::AngryVillager}, {"bubble", Kind::Bubble},
    {"bubble_pop", Kind::BubblePop}, {"cherry_leaves", Kind::CherryLeaves}, {"pale_oak_leaves", Kind::PaleLeaves},
    {"red_poplar_leaves", Kind::PaleLeaves}, {"orange_poplar_leaves", Kind::PaleLeaves},
    {"yellow_poplar_leaves", Kind::PaleLeaves}, {"tinted_leaves", Kind::TintedLeaves}, {"portal", Kind::Portal},
    {"enchant", Kind::Enchant}, {"nautilus", Kind::Enchant}, {"campfire_cosy_smoke", Kind::CampfireCosy},
    {"campfire_signal_smoke", Kind::CampfireSignal}, {"effect", Kind::Spell}, {"instant_effect", Kind::Spell},
    {"entity_effect", Kind::Spell}, {"infested", Kind::Spell}, {"raid_omen", Kind::Spell}, {"trial_omen", Kind::Spell},
    {"witch", Kind::Witch}, {"mycelium", Kind::Suspended}, {"happy_villager", Kind::HappyVillager},
    {"composter", Kind::HappyVillager}, {"egg_crack", Kind::HappyVillager}, {"dolphin", Kind::Suspended},
    {"lava", Kind::Lava}, {"dust", Kind::Dust}, {"dust_color_transition", Kind::DustTransition}, {"note", Kind::Note},
    {"cloud", Kind::Cloud}, {"sneeze", Kind::Cloud}, {"snowflake", Kind::Snowflake}, {"item", Kind::Item},
    {"item_slime", Kind::Item}, {"item_snowball", Kind::Item}, {"item_cobweb", Kind::Item}, {"vibration", Kind::Vibration},
    {"firework", Kind::Spark}, {"flash", Kind::Flash}, {"totem_of_undying", Kind::Totem}, {"end_rod", Kind::EndRod},
    {"gust", Kind::Gust}, {"small_gust", Kind::Gust}, {"gust_emitter_large", Kind::GustSeed},
    {"gust_emitter_small", Kind::GustSeed}, {"sonic_boom", Kind::Explosion}, {"soul", Kind::Rising},
    {"sculk_soul", Kind::Rising}, {"sculk_charge", Kind::SculkCharge}, {"sculk_charge_pop", Kind::SculkChargePop},
    {"shriek", Kind::Shriek}, {"firefly", Kind::Firefly}, {"glow", Kind::Glow}, {"wax_on", Kind::Glow},
    {"wax_off", Kind::Glow}, {"electric_spark", Kind::Glow}, {"scrape", Kind::Glow}};

// DripParticle and its providers: the hanging drop falls into the next type, which lands as another one.
enum DripStage : u8 { DripHang, DripCoolingHang, DripFall, DripFallLand, DripDripstoneFallLand, DripHoneyFallLand, DripLand };
struct DripDef {
    const char* name;
    DripStage stage;
    u8 fluid;  // 0 none, 1 water, 2 lava: the drop ends inside that fluid
    float r, g, b, gravity;
    float life;  // > 0: lifetime; < 0: (int)(-life / (random * 0.8 + 0.2))
    bool glow;
    const char* next;  // the falling / landing particle
};
constexpr float kTear[3] = {0.51171875f, 0.03125f, 0.890625f};
const DripDef kDrips[] = {
    {"dripping_water", DripHang, 1, 0.2f, 0.3f, 1.0f, 0.06f * 0.02f, 40, false, "falling_water"},
    {"falling_water", DripFallLand, 1, 0.2f, 0.3f, 1.0f, 0.06f, -64, false, "splash"},
    {"dripping_lava", DripCoolingHang, 2, 1, 1, 1, 0.06f * 0.02f, 40, false, "falling_lava"},
    {"falling_lava", DripFallLand, 2, 1.0f, 0.2857143f, 0.083333336f, 0.06f, -64, false, "landing_lava"},
    {"landing_lava", DripLand, 2, 1.0f, 0.2857143f, 0.083333336f, 0.06f, -16, false, nullptr},
    {"dripping_honey", DripHang, 0, 0.622f, 0.508f, 0.082f, 0.06f * 0.02f * 0.01f, 100, false, "falling_honey"},
    {"falling_honey", DripHoneyFallLand, 0, 0.582f, 0.448f, 0.082f, 0.01f, -64, false, "landing_honey"},
    {"landing_honey", DripLand, 0, 0.522f, 0.408f, 0.082f, 0.06f, -128, false, nullptr},
    {"falling_nectar", DripFall, 0, 0.92f, 0.782f, 0.72f, 0.007f, -16, false, nullptr},
    {"dripping_obsidian_tear", DripHang, 0, kTear[0], kTear[1], kTear[2], 0.06f * 0.02f * 0.01f, 100, true, "falling_obsidian_tear"},
    {"falling_obsidian_tear", DripFallLand, 0, kTear[0], kTear[1], kTear[2], 0.01f, -64, true, "landing_obsidian_tear"},
    {"landing_obsidian_tear", DripLand, 0, kTear[0], kTear[1], kTear[2], 0.06f, -28, true, nullptr},
    {"falling_spore_blossom", DripFall, 0, 0.32f, 0.5f, 0.22f, 0.005f, -64, false, nullptr},
    {"dripping_dripstone_water", DripHang, 1, 0.2f, 0.3f, 1.0f, 0.06f * 0.02f, 40, false, "falling_dripstone_water"},
    {"falling_dripstone_water", DripDripstoneFallLand, 1, 0.2f, 0.3f, 1.0f, 0.06f, -64, false, "splash"},
    {"dripping_dripstone_lava", DripCoolingHang, 2, 1, 1, 1, 0.06f * 0.02f, 40, false, "falling_dripstone_lava"},
    {"falling_dripstone_lava", DripDripstoneFallLand, 2, 1.0f, 0.2857143f, 0.083333336f, 0.06f, -64, false, "landing_lava"},
};

// FireflyParticle.getFadeAmount.
float fadeAmount(float progress, float fadeIn, float fadeOut) {
    if (progress >= 1.0f - fadeIn) return (1.0f - progress) / fadeIn;
    return progress <= fadeOut ? progress / fadeOut : 1.0f;
}

const std::pair<const char*, Opt> kOptions[] = {
    {"block", Opt::Block}, {"block_marker", Opt::Block}, {"falling_dust", Opt::Block}, {"dust_pillar", Opt::Block},
    {"block_crumble", Opt::Block}, {"dust", Opt::Dust}, {"dust_color_transition", Opt::DustTransition},
    {"effect", Opt::Spell}, {"instant_effect", Opt::Spell}, {"entity_effect", Opt::Color}, {"tinted_leaves", Opt::Color},
    {"flash", Opt::Color}, {"dragon_breath", Opt::Float}, {"sculk_charge", Opt::Float}, {"shriek", Opt::VarInt},
    {"geyser", Opt::Int}, {"geyser_plume", Opt::Int}, {"geyser_base", Opt::IntFloat}, {"geyser_poof", Opt::IntFloat},
    {"trail", Opt::Trail}, {"item", Opt::Item}, {"vibration", Opt::Vibration}};

constexpr u32 kTints[8] = {0xFFFFFF, 0x91BD59, 0x77AB2F, 0x80A755, 0x619961, 0x3F76E4, 0x208030, 0x8B1A00};

u32 abgrF(float r, float g, float b, float a) {
    auto c = [](float v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}

// GpuParticle::orient for a quad turned by rotationY(a).rotateX(b).rotateY(c).
u32 orientedQuad(float a, float b, float c) {
    auto q = [](float v) {
        const float t = v / (2.0f * kPi);
        return u32(int(std::floor((t - std::floor(t)) * 1024.0f + 0.5f)) & 1023);
    };
    return 0x80000000u | q(a) | q(b) << 10 | q(c) << 20;
}
}  // namespace

struct Particles::Type {
    std::string name;
    Kind kind = Kind::None;
    Opt opt = Opt::None;
    std::vector<int> sprites;  // item atlas rects
    int drip = -1;             // kDrips index
    int next = -2;             // drips: the particle type they turn into (-2 unresolved)
};

struct Particles::Particle {
    double x, y, z, xo, yo, zo, xd, yd, zd;
    double sx, sy, sz;  // start (portal / enchant)
    float quadSize, r, g, b, a, roll, oRoll, gravity, friction;
    float e0, e1, e2, e3;  // per-kind state
    i32 age, lifetime;
    i32 sprite;            // item atlas rect
    u16 type;
    u8 flags, light, sizeMode;
    u8 emissive;           // minimum block light
    float uo, vo;          // terrain shard sub-rectangle (-1: whole sprite)
    i32 target;            // vibration: destination entity (-1: the block centre in sx, sy, sz)
};

Particles::Particles(World& world, BlockRegistry& registry, const GameReports& reports, ent::ItemModels& items)
    : world_(world), registry_(registry), reports_(reports), items_(items) {}
Particles::~Particles() = default;
u32 Particles::count() const { return u32(live_.size()); }

bool Particles::init(const ui::Resources& res) {
    res_ = &res;
    const auto& reg = reports_.registries();
    const nlohmann::json entries = reg.is_object() && reg.contains("minecraft:particle_type")
                                       ? reg["minecraft:particle_type"].value("entries", nlohmann::json::object())
                                       : nlohmann::json::object();  // keep alive for the loop
    if (entries.is_object())
        for (const auto& [k, e] : entries.items()) {
            const int pid = e.value("protocol_id", -1);
            if (pid < 0) continue;
            if (size_t(pid) >= types_.size()) types_.resize(size_t(pid) + 1);
            types_[size_t(pid)].name = k.substr(k.find(':') + 1);
        }
    for (size_t i = 0; i < types_.size(); ++i) {
        Type& t = types_[i];
        if (t.name.empty()) continue;
        typeIds_[t.name] = int(i);
        t.kind = Kind::Generic;
        for (auto [n, k] : kKinds)
            if (t.name == n) t.kind = k;
        for (auto [n, o] : kOptions)
            if (t.name == n) t.opt = o;
        for (size_t d = 0; d < std::size(kDrips); ++d)
            if (t.name == kDrips[d].name) t.kind = Kind::Drip, t.drip = int(d);
    }
    for (const auto& [key, id] : reports_.stateIds()) {
        if (id < 0) continue;
        if (size_t(id) >= blockStates_.size()) blockStates_.resize(size_t(id) + 1);
        blockStates_[size_t(id)] = key;
    }
    live_.reserve(4096);
    spawnQueue_.reserve(512);
    LOG("particles: %zu particle types", typeIds_.size());
    return !types_.empty();
}

int Particles::typeId(std::string_view name) const {
    auto it = typeIds_.find(std::string(name));
    return it == typeIds_.end() ? -1 : it->second;
}

int Particles::dripNext(Type& t) {
    if (t.next == -2) t.next = t.drip >= 0 && kDrips[size_t(t.drip)].next ? typeId(kDrips[size_t(t.drip)].next) : -1;
    return t.next;
}

u16 Particles::localBlock(int state) {
    if (state <= 0 || size_t(state) >= blockStates_.size() || blockStates_[size_t(state)].empty()) return 0;
    auto it = blockCache_.find(state);
    if (it != blockCache_.end()) return it->second;
    const u16 id = registry_.idFor(blockStates_[size_t(state)]);
    blockCache_[state] = id;
    return id;
}

// ---------------------------------------------------------------------------------------------------------
// Network thread

void Particles::attach(net::Client& client) {
    client.onPlay("level_particles", [this](net::Reader& r) {
        NetSpawn s{};
        s.type = r.varInt();
        if (!r.ok || s.type < 0 || size_t(s.type) >= types_.size()) return;
        const Type& t = types_[size_t(s.type)];
        if (!readOptions(&r, s.type, s.opt)) return;  // options that cannot be decoded: the rest is unreadable
        r.boolean();  // override limiter
        s.alwaysShow = r.boolean();
        s.x = r.f64(), s.y = r.f64(), s.z = r.f64();
        s.dx = r.f32(), s.dy = r.f32(), s.dz = r.f32();
        s.sx = r.f32(), s.sy = r.f32(), s.sz = r.f32();
        s.count = r.varInt();
        s.mode = r.varInt();
        if (!r.ok || t.kind == Kind::None) return;
        std::lock_guard lock(netMutex_);
        if (netSpawns_.size() < 4096) netSpawns_.push_back(s);
    });
    client.onPlay("level_event", [this](net::Reader& r) {
        const int type = r.i32v();
        int x, y, z;
        r.blockPos(x, y, z);
        const int data = r.i32v();
        if (!r.ok || (type != 2001 && type != 2019 && type != 2020)) return;
        std::lock_guard lock(netMutex_);
        if (type == 2001) netBreaks_.push_back({x, y, z, data});
        else netBreaks_.push_back({x, y, z, 0, std::clamp(data, 0, 5)});  // LevelEventHandler: Direction by ordinal
    });
    // ClientboundExplodePacket: the explosion particle and sound (ClientPacketListener.handleExplosion).
    client.onPlay("explode", [this](net::Reader& r) {
        NetExplosion e{};
        for (double& c : e.pos) c = r.f64();
        e.radius = r.f32();
        e.blockCount = r.i32v();
        if (r.boolean()) r.f64(), r.f64(), r.f64();  // knockback (handled by the UI)
        e.particle = r.varInt();
        if (!r.ok || !skipOptions(&r, e.particle)) return;
        const int holder = r.varInt();  // Holder<SoundEvent>: 0 inline (name + optional range), else id + 1
        e.soundId = holder - 1;
        if (holder == 0) {
            e.soundName = std::string(r.str());
            if (r.boolean()) r.f32();
        }
        const i32 n = r.varInt();  // block particles: WeightedList<ExplosionParticleInfo>
        for (i32 i = 0; i < n && r.ok && i < 64; ++i) {
            BlockParticleInfo b{};
            b.type = r.varInt();
            if (!readOptions(&r, b.type, b.opt)) return;
            b.scaling = r.f32(), b.speed = r.f32(), b.weight = r.varInt();
            if (b.weight > 0) e.blockParticles.push_back(b);
        }
        e.playSound = r.boolean();
        if (!r.ok) return;
        std::lock_guard lock(netMutex_);
        netExplosions_.push_back(std::move(e));
    });
    client.onPlayPeek("game_event", [this](net::Reader& r) {
        const int type = r.u8v();
        const float param = r.f32();
        if (!r.ok || (type != 1 && type != 2 && type != 7)) return;
        std::lock_guard lock(netMutex_);
        netWeather_.push_back({type, param});
    });
}

bool Particles::skipOptions(void* reader, int type) const {
    Options unused;
    return readOptions(reader, type, unused);
}

bool Particles::readOptions(void* reader, int type, Options& o) const {
    net::Reader& r = *static_cast<net::Reader*>(reader);
    if (type < 0 || size_t(type) >= types_.size()) return false;
    switch (types_[size_t(type)].opt) {
        case Opt::None: break;
        case Opt::Block: o.block = r.varInt(); break;
        case Opt::Dust: o.color = 0xFF000000u | u32(r.i32v()), o.scale = r.f32(), o.hasColor = true; break;
        case Opt::DustTransition:
            o.color = 0xFF000000u | u32(r.i32v()), o.color2 = 0xFF000000u | u32(r.i32v()), o.scale = r.f32(), o.hasColor = true;
            break;
        case Opt::Spell: o.color = u32(r.i32v()), o.power = r.f32(), o.hasColor = true; break;
        case Opt::Color: o.color = u32(r.i32v()), o.hasColor = true; break;
        case Opt::Float: o.scale = r.f32(); break;  // sculk_charge: the roll
        case Opt::VarInt: o.delay = r.varInt(); break;  // shriek: the delay
        case Opt::Int: r.i32v(); break;
        case Opt::IntFloat: r.i32v(), r.f32(); break;
        case Opt::Trail: r.f64(), r.f64(), r.f64(), r.i32v(), r.varInt(); break;
        case Opt::Item:
            if (!readItem || !readItem(r, o.item)) return false;
            break;
        case Opt::Vibration: {  // PositionSource (position_source_type registry: 0 block, 1 entity), arrival ticks
            const int source = r.varInt();
            if (source == 0) {
                int bx, by, bz;
                r.blockPos(bx, by, bz);
                o.target[0] = bx + 0.5, o.target[1] = by + 0.5, o.target[2] = bz + 0.5;  // Vec3.atCenterOf
            } else if (source == 1) {
                o.entity = r.varInt(), o.yOffset = r.f32();
            } else {
                return false;
            }
            o.arrival = r.varInt();
            break;
        }
    }
    return r.ok;
}

// ---------------------------------------------------------------------------------------------------------
// World queries (caller holds the world lock shared)

u16 Particles::blockAt(int x, int y, int z) {
    CachedBlock& c = blockCache2_[(u32(x) * 73856093u ^ u32(y) * 19349663u ^ u32(z) * 83492791u) & 1023];
    if (c.y != y || c.x != x || c.z != z) c.x = x, c.y = y, c.z = z, c.id = world_.block(x, y, z);
    return c.id;
}

u8 Particles::lightAt(double x, double y, double z) const {
    const int bx = int(std::floor(x)), by = int(std::floor(y)), bz = int(std::floor(z));
    if (by < world_.minSectionY() * 16 || by >= (world_.maxSectionY() + 1) * 16) return 0xF0;
    return world_.light(bx >> 4, by >> 4, bz >> 4).at(bx & 15, by & 15, bz & 15);
}

int Particles::blockSprite(u16 block, u32& tint) {
    if (blockSprites_.size() <= block) blockSprites_.resize(size_t(block) + 1, 0), blockTints_.resize(size_t(block) + 1, 0xFFFFFF);
    if (blockSprites_[block] == 0) {
        std::string name = registry_.name(block);
        if (const size_t b = name.find('['); b != std::string::npos) name.resize(b);
        const std::string tex = items_.blockParticleTexture(name);
        const int rect = tex.empty() ? -1 : items_.texture(tex);
        blockSprites_[block] = rect >= 0 ? rect + 1 : -1;
        const BlockInfo& bi = registry_.infos()[block];
        u8 t = bi.kind == KindCube ? bi.faces[1].tint : 0;
        if (bi.kind == KindModel && bi.quadCount) t = registry_.modelQuad(bi.quadFirst).tint;
        blockTints_[block] = kTints[t & 7];
        if (name == "minecraft:grass_block") blockTints_[block] = 0xFFFFFF;  // its particle texture is dirt
    }
    tint = blockTints_[block];
    return blockSprites_[block] > 0 ? blockSprites_[block] - 1 : -1;
}

// Average colour of a block's particle texture, times its tint (RGB).
u32 Particles::dustColor(u16 block) {
    u32 tint;
    const int rect = blockSprite(block, tint);
    if (rect < 0) return 0xFFFFFF;
    const ent::ItemModels::Rect& r = items_.rect(rect);
    ui::Atlas& a = items_.atlas();
    const int x0 = int(r.u0 * float(a.width()) + 0.5f), y0 = int(r.v0 * float(a.height()) + 0.5f);
    double sum[3] = {}, n = 0;
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            const u8* px = a.pixel(x0 + x, y0 + y);
            if (px[3] < 128) continue;
            for (int c = 0; c < 3; ++c) sum[c] += px[c];
            n += 1;
        }
    if (n <= 0) return 0xFFFFFF;
    const u32 rr = u32(sum[0] / n * float((tint >> 16) & 255) / 255.0), gg = u32(sum[1] / n * float((tint >> 8) & 255) / 255.0);
    const u32 bb = u32(sum[2] / n * float(tint & 255) / 255.0);
    return (std::min(rr, 255u) << 16) | (std::min(gg, 255u) << 8) | std::min(bb, 255u);
}

// ---------------------------------------------------------------------------------------------------------
// Spawning (ports of the particle constructors and providers)

Particles::Particle* Particles::add(int typeIndex, double x, double y, double z, double xa, double ya, double za,
                                    const Options* opt) {
    if (typeIndex < 0 || size_t(typeIndex) >= types_.size()) return nullptr;
    Type& t = types_[size_t(typeIndex)];
    if (t.kind == Kind::None) return nullptr;
    if (live_.size() + spawnQueue_.size() >= kMaxParticles) return nullptr;
    if (t.kind == Kind::Item) {  // BreakingItemParticle: the item's particle texture, no sprite set
        if (t.name == "item") {
            if (!opt || opt->item < 0) return nullptr;
            int& sprite = itemSprites_[opt->item];  // 0 unresolved, -1 none, else rect + 1
            if (sprite == 0) {
                const std::string tex = items_.itemParticleTexture(opt->item);
                const int rect = tex.empty() ? -1 : items_.texture(tex);
                sprite = rect >= 0 ? rect + 1 : -1;
            }
            if (sprite > 0) shard(sprite - 1, x, y, z, xa, ya, za, true);
        } else {  // SlimeProvider / SnowballProvider / CobwebProvider: a fixed item, the plain random push
            const char* tex = t.name == "item_slime" ? "item/slime_ball" : t.name == "item_snowball" ? "item/snowball" : "block/cobweb";
            const int rect = items_.texture(tex);
            if (rect >= 0) shard(rect, x, y, z, 0, 0, 0, false);
        }
        return nullptr;
    }
    if (t.sprites.empty() && t.kind != Kind::Terrain && t.kind != Kind::Crumble && t.kind != Kind::DustPillar &&
        t.kind != Kind::ExplosionSeed && t.kind != Kind::GustSeed) {
        // Sprite set from assets/minecraft/particles/<type>.json (loaded on first use).
        const nlohmann::json j = res_ ? res_->json("assets/minecraft/particles/" + t.name + ".json") : nlohmann::json();
        if (j.is_object() && j.contains("textures") && j["textures"].is_array())
            for (const auto& n : j["textures"]) {
                if (!n.is_string()) continue;
                const std::string name = n.get<std::string>();
                const int rect = items_.texture("particle/" + name.substr(name.find(':') + 1));
                if (rect >= 0) t.sprites.push_back(rect);
            }
        if (t.sprites.empty()) {
            t.kind = Kind::None;
            return nullptr;
        }
    }
    Particle p{};
    p.type = u16(typeIndex);
    p.r = p.g = p.b = p.a = 1;
    p.friction = 0.98f;
    p.flags = Physics;
    p.uo = -1;
    p.sprite = -1;
    p.target = -1;
    p.light = lightAt(x, y, z);
    auto base = [&](double px, double py, double pz) {  // Particle(level, x, y, z) + SingleQuadParticle
        p.x = p.xo = px, p.y = p.yo = py, p.z = p.zo = pz;
        p.lifetime = int(4.0f / (randf() * 0.9f + 0.1f));
        p.quadSize = 0.1f * (randf() * 0.5f + 0.5f) * 2.0f;
    };
    auto baseVel = [&](double px, double py, double pz, double vx, double vy, double vz) {
        p.x = p.xo = px, p.y = p.yo = py, p.z = p.zo = pz;
        p.lifetime = int(4.0f / (randf() * 0.9f + 0.1f));
        p.xd = vx + (randf() * 2.0f - 1.0f) * 0.4f;
        p.yd = vy + (randf() * 2.0f - 1.0f) * 0.4f;
        p.zd = vz + (randf() * 2.0f - 1.0f) * 0.4f;
        const double speed = (randf() + randf() + 1.0f) * 0.15f;
        const double dd = std::max(std::sqrt(p.xd * p.xd + p.yd * p.yd + p.zd * p.zd), 1e-9);
        p.xd = p.xd / dd * speed * 0.4f;
        p.yd = p.yd / dd * speed * 0.4f + 0.1f;
        p.zd = p.zd / dd * speed * 0.4f;
        p.quadSize = 0.1f * (randf() * 0.5f + 0.5f) * 2.0f;
    };
    auto randomSprite = [&] { p.sprite = t.sprites[std::uniform_int_distribution<size_t>(0, t.sprites.size() - 1)(rng_)]; };
    auto ageSprite = [&] { p.flags |= AgeSprite; };
    auto color = [&](u32 argb) {
        p.r = float((argb >> 16) & 255) / 255.0f, p.g = float((argb >> 8) & 255) / 255.0f, p.b = float(argb & 255) / 255.0f;
    };
    switch (t.kind) {
        case Kind::Terrain:
        case Kind::Crumble:
        case Kind::DustPillar: {
            const u16 block = opt ? localBlock(opt->block) : 0;
            if (!block) return nullptr;
            u32 tint;
            p.sprite = blockSprite(block, tint);
            if (p.sprite < 0) return nullptr;
            baseVel(x, y, z, xa, ya, za);
            p.gravity = 1.0f;
            p.r = 0.6f * float((tint >> 16) & 255) / 255.0f, p.g = 0.6f * float((tint >> 8) & 255) / 255.0f;
            p.b = 0.6f * float(tint & 255) / 255.0f;
            p.quadSize /= 2.0f;
            p.uo = randf() * 3.0f, p.vo = randf() * 3.0f;
            if (items_.rect(p.sprite).translucent) p.flags |= Translucent;
            if (t.kind == Kind::Crumble) p.xd = p.yd = p.zd = 0, p.lifetime = int(rng_() % 10) + 1;
            if (t.kind == Kind::DustPillar)
                p.xd = gauss() / 30.0, p.yd = ya + gauss() / 2.0, p.zd = gauss() / 30.0, p.lifetime = int(rng_() % 20) + 20;
            break;
        }
        case Kind::FallingDust: {
            const u16 block = opt ? localBlock(opt->block) : 0;
            if (!block) return nullptr;
            base(x, y, z);
            color(dustColor(block));  // FallingBlock dust colour / map colour: the texture's average here
            p.quadSize *= 0.675f;
            p.lifetime = std::max(int(float(int(32.0 / (randf() * 0.8 + 0.2))) * 0.9f), 1);
            p.e0 = (randf() - 0.5f) * 0.1f;
            p.roll = p.oRoll = randf() * kPi * 2;
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::Rain:
        case Kind::Splash:
            baseVel(x, y, z, 0, 0, 0);
            p.xd *= 0.3f, p.yd = randf() * 0.2f + 0.1f, p.zd *= 0.3f;
            p.gravity = t.kind == Kind::Splash ? 0.04f : 0.06f;
            p.lifetime = int(8.0 / (randf() * 0.8 + 0.2));
            if (t.kind == Kind::Splash && ya == 0.0 && (xa != 0.0 || za != 0.0)) p.xd = xa, p.yd = 0.1, p.zd = za;
            randomSprite();
            break;
        case Kind::Poof: {
            base(x, y, z);
            p.gravity = -0.1f, p.friction = 0.9f;
            p.xd = xa + (randf() * 2.0f - 1.0f) * 0.05f, p.yd = ya + (randf() * 2.0f - 1.0f) * 0.05f;
            p.zd = za + (randf() * 2.0f - 1.0f) * 0.05f;
            const float c = randf() * 0.3f + 0.7f;
            p.r = p.g = p.b = c;
            p.quadSize = 0.1f * (randf() * randf() * 6.0f + 1.0f);
            p.lifetime = int(16.0 / (randf() * 0.8 + 0.2)) + 2;
            ageSprite();
            break;
        }
        case Kind::Explosion: {
            baseVel(x, y, z, 0, 0, 0);
            p.lifetime = 6 + int(rng_() % 4);
            const float c = randf() * 0.6f + 0.4f;
            p.r = p.g = p.b = c;
            p.quadSize = 2.0f * (1.0f - float(xa) * 0.5f);
            p.emissive = 255;
            if (t.name == "sonic_boom") p.lifetime = 16, p.quadSize = 1.5f;  // SonicBoomParticle
            ageSprite();
            break;
        }
        case Kind::ExplosionSeed:
            base(x, y, z);
            p.lifetime = 8;
            p.flags |= NoRender;
            break;
        case Kind::Smoke:
        case Kind::LargeSmoke:
        case Kind::WhiteSmoke: {  // BaseAshSmokeParticle(dir 0.1, colour random 0.3, lifetime 8, gravity -0.1)
            const float scale = t.kind == Kind::LargeSmoke ? 2.5f : 1.0f;
            baseVel(x, y, z, 0, 0, 0);
            p.friction = 0.96f, p.gravity = -0.1f;
            p.flags |= SpeedUp;
            p.xd = p.xd * 0.1f + xa, p.yd = p.yd * 0.1f + ya, p.zd = p.zd * 0.1f + za;
            const float c = t.kind == Kind::WhiteSmoke ? 0.7f + randf() * 0.3f : randf() * 0.3f;
            p.r = p.g = p.b = c;
            p.quadSize *= 0.75f * scale;
            p.lifetime = std::max(int(8.0 / (randf() * 0.8 + 0.2) * scale), 1);
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::Flame:
        case Kind::SmallFlame:
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.xd = p.xd * 0.01f + xa, p.yd = p.yd * 0.01f + ya, p.zd = p.zd * 0.01f + za;
            p.x += (randf() - randf()) * 0.05f, p.y += (randf() - randf()) * 0.05f, p.z += (randf() - randf()) * 0.05f;
            p.xo = p.x, p.yo = p.y, p.zo = p.z;
            p.lifetime = int(8.0 / (randf() * 0.8 + 0.2)) + 4;
            p.flags &= u8(~Physics);
            p.sizeMode = SizeFlame;
            p.emissive = 15;
            if (t.kind == Kind::SmallFlame) p.quadSize *= 0.5f;
            randomSprite();
            break;
        case Kind::Crit:
        case Kind::DamageIndicator:
        case Kind::EnchantedHit: {
            baseVel(x, y, z, 0, 0, 0);
            p.friction = 0.7f, p.gravity = 0.5f;
            const double yy = t.kind == Kind::DamageIndicator ? ya + 1.0 : ya;
            p.xd = p.xd * 0.1f + xa * 0.4, p.yd = p.yd * 0.1f + yy * 0.4, p.zd = p.zd * 0.1f + za * 0.4;
            const float c = randf() * 0.3f + 0.6f;
            p.r = p.g = p.b = c;
            if (t.kind == Kind::EnchantedHit) p.r *= 0.3f, p.g *= 0.8f;
            p.quadSize *= 0.75f;
            p.lifetime = t.kind == Kind::DamageIndicator ? 20 : std::max(int(6.0 / (randf() * 0.8 + 0.6)), 1);
            p.flags &= u8(~Physics);
            p.sizeMode = SizeGrow;
            randomSprite();
            break;
        }
        case Kind::Heart:
        case Kind::AngryVillager:
            baseVel(x, t.kind == Kind::AngryVillager ? y + 0.5 : y, z, 0, 0, 0);
            p.flags |= SpeedUp;
            p.flags &= u8(~Physics);
            p.friction = 0.86f;
            p.xd *= 0.01f, p.yd = p.yd * 0.01f + 0.1, p.zd *= 0.01f;
            p.quadSize *= 1.5f;
            p.lifetime = 16;
            p.sizeMode = SizeGrow;
            randomSprite();
            break;
        case Kind::Bubble:
            base(x, y, z);
            p.quadSize *= randf() * 0.6f + 0.2f;
            p.xd = xa * 0.2f + (randf() * 2.0f - 1.0f) * 0.02f, p.yd = ya * 0.2f + (randf() * 2.0f - 1.0f) * 0.02f;
            p.zd = za * 0.2f + (randf() * 2.0f - 1.0f) * 0.02f;
            p.lifetime = int(8.0 / (randf() * 0.8 + 0.2));
            randomSprite();
            break;
        case Kind::BubblePop:
            base(x, y, z);
            p.lifetime = 4, p.gravity = 0.008f;
            p.xd = xa, p.yd = ya, p.zd = za;
            ageSprite();
            break;
        case Kind::CherryLeaves:
        case Kind::PaleLeaves:
        case Kind::TintedLeaves: {  // FallingParticle
            base(x, y, z);
            const bool cherry = t.kind == Kind::CherryLeaves;
            const float fall = cherry ? 0.25f : 0.07f, side = cherry ? 2.0f : 10.0f, scale = cherry ? 1.0f : 2.0f;
            p.e0 = side;                                   // windBig
            p.e3 = cherry ? 1.0f : 0.0f;                   // flowAway (swirl otherwise)
            p.lifetime = 300;
            p.gravity = fall * 1.2f * 0.0025f;
            p.quadSize = scale * (rng_() & 1 ? 0.05f : 0.075f);
            p.friction = 1.0f;
            p.yd = cherry ? 0.0 : -0.021;
            const float pr = randf();
            p.sx = std::cos(pr * 60.0f * kPi / 180.0f) * side;  // xaFlowScale
            p.sz = std::sin(pr * 60.0f * kPi / 180.0f) * side;  // zaFlowScale
            p.sy = (1000.0f + pr * 3000.0f) * kPi / 180.0f;      // swirlPeriod
            p.e1 = (rng_() & 1 ? -30.0f : 30.0f) * kPi / 180.0f;  // rotSpeed
            p.e2 = (rng_() & 1 ? -5.0f : 5.0f) * kPi / 180.0f;    // spinAcceleration
            if (t.kind == Kind::TintedLeaves && opt && opt->hasColor) color(opt->color);
            randomSprite();
            break;
        }
        case Kind::Portal: {
            base(x, y, z);
            p.xd = xa, p.yd = ya, p.zd = za;
            p.sx = x, p.sy = y, p.sz = z;
            p.quadSize = 0.1f * (randf() * 0.2f + 0.5f);
            const float br = randf() * 0.6f + 0.4f;
            p.r = br * 0.9f, p.g = br * 0.3f, p.b = br;
            p.lifetime = int(randf() * 10.0f) + 40;
            p.flags &= u8(~Physics);
            p.sizeMode = SizePortal;
            randomSprite();
            break;
        }
        case Kind::Enchant: {
            base(x, y, z);
            p.xd = xa, p.yd = ya, p.zd = za;
            p.sx = x, p.sy = y, p.sz = z;
            p.x = p.xo = x + xa, p.y = p.yo = y + ya, p.z = p.zo = z + za;
            p.quadSize = 0.1f * (randf() * 0.5f + 0.2f);
            const float br = randf() * 0.6f + 0.4f;
            p.r = 0.9f * br, p.g = 0.9f * br, p.b = br;
            p.flags &= u8(~Physics);
            p.lifetime = int(randf() * 10.0f) + 30;
            randomSprite();
            break;
        }
        case Kind::CampfireCosy:
        case Kind::CampfireSignal:
            base(x, y, z);
            p.quadSize *= 3.0f;
            p.lifetime = int(rng_() % 50) + (t.kind == Kind::CampfireSignal ? 280 : 80);
            p.gravity = 3.0e-6f;
            p.xd = xa, p.yd = ya + randf() / 500.0f, p.zd = za;
            p.a = t.kind == Kind::CampfireSignal ? 0.95f : 0.9f;
            p.flags |= Translucent;
            randomSprite();
            break;
        case Kind::Spell:
        case Kind::Witch: {
            baseVel(x, y, z, 0.5 - randf(), ya, 0.5 - randf());
            p.friction = 0.96f, p.gravity = -0.1f;
            p.flags |= SpeedUp | Translucent;
            p.flags &= u8(~Physics);
            p.yd *= 0.2f;
            if (xa == 0.0 && za == 0.0) p.xd *= 0.1f, p.zd *= 0.1f;
            p.quadSize *= 0.75f;
            p.lifetime = int(8.0 / (randf() * 0.8 + 0.2));
            if (t.kind == Kind::Witch) {
                const float br = randf() * 0.5f + 0.35f;
                p.r = br, p.g = 0, p.b = br;
            } else if (opt && opt->hasColor) {
                color(opt->color);
                if (t.opt == Opt::Color) p.a = float(opt->color >> 24) / 255.0f;
                if (t.opt == Opt::Spell) p.xd *= opt->power, p.yd = (p.yd - 0.1f) * opt->power + 0.1f, p.zd *= opt->power;
            }
            p.e0 = p.a;  // original alpha
            ageSprite();
            break;
        }
        case Kind::Suspended:
        case Kind::HappyVillager: {
            baseVel(x, y, z, xa, ya, za);
            const float br = randf() * 0.1f + 0.2f;
            p.r = p.g = p.b = t.kind == Kind::HappyVillager ? 1.0f : br;
            p.quadSize *= randf() * 0.6f + 0.5f;
            p.xd *= 0.02f, p.yd *= 0.02f, p.zd *= 0.02f;
            p.lifetime = int(20.0 / (randf() * 0.8 + 0.2));
            p.flags &= u8(~Physics);
            randomSprite();
            break;
        }
        case Kind::Lava:
            baseVel(x, y, z, 0, 0, 0);
            p.gravity = 0.75f, p.friction = 0.999f;
            p.xd *= 0.8f, p.zd *= 0.8f, p.yd = randf() * 0.4f + 0.05f;
            p.quadSize *= randf() * 2.0f + 0.2f;
            p.lifetime = int(16.0 / (randf() * 0.8 + 0.2));
            p.emissive = 15;
            p.sizeMode = SizeLava;
            randomSprite();
            break;
        case Kind::Dust:
        case Kind::DustTransition: {  // DustParticleBase
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.flags |= SpeedUp;
            p.xd *= 0.1f, p.yd *= 0.1f, p.zd *= 0.1f;
            const float scale = opt ? std::clamp(opt->scale, 0.01f, 4.0f) : 1.0f;
            const float br = randf() * 0.4f + 0.6f;
            if (opt && opt->hasColor) color(opt->color);
            p.r *= br, p.g *= br, p.b *= br;
            if (t.kind == Kind::DustTransition) {  // DustColorTransitionParticle: from (r, g, b) to (e1, e2, e3)
                const u32 to = opt ? opt->color2 : 0xFFFFFFFF;
                p.e1 = float((to >> 16) & 255) / 255.0f * br, p.e2 = float((to >> 8) & 255) / 255.0f * br;
                p.e3 = float(to & 255) / 255.0f * br;
                p.sx = p.r, p.sy = p.g, p.sz = p.b;
            }
            p.quadSize *= 0.75f * scale;
            p.lifetime = std::max(int(float(int(8.0 / (randf() * 0.8 + 0.2))) * scale), 1);
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::Note: {
            baseVel(x, y, z, 0, 0, 0);
            p.friction = 0.66f;
            p.flags |= SpeedUp;
            p.flags &= u8(~Physics);
            p.xd *= 0.01f, p.yd = p.yd * 0.01f + 0.2, p.zd *= 0.01f;
            const float note = float(xa);  // NoteParticle.getColor: rainbow over 24 notes
            p.r = std::max(0.0f, std::sin((note + 0.0f) * kPi * 2) * 0.65f + 0.35f);
            p.g = std::max(0.0f, std::sin((note + 1.0f / 3) * kPi * 2) * 0.65f + 0.35f);
            p.b = std::max(0.0f, std::sin((note + 2.0f / 3) * kPi * 2) * 0.65f + 0.35f);
            p.quadSize *= 1.5f;
            p.lifetime = 6;
            p.sizeMode = SizeGrow;
            randomSprite();
            break;
        }
        case Kind::Cloud:  // PlayerCloudParticle
            baseVel(x, y, z, 0, 0, 0);
            p.friction = 0.96f;
            p.xd = p.xd * 0.1f + xa, p.yd = p.yd * 0.1f + ya, p.zd = p.zd * 0.1f + za;
            p.r = p.g = p.b = 1.0f - randf() * 0.3f;
            p.quadSize *= 1.875f;
            p.lifetime = std::max(int(8.0 / (randf() * 0.8 + 0.3) * 2.5), 1);
            p.sizeMode = SizeGrow;
            if (t.name == "sneeze") p.r = 0.22f, p.g = 1.0f, p.b = 0.53f, p.a = 0.4f, p.flags |= Translucent;  // SneezeProvider
            ageSprite();
            break;
        case Kind::Vibration: {  // VibrationSignalParticle: flies to its destination, arriving after `arrival` ticks
            if (!opt) return nullptr;
            base(x, y, z);
            p.quadSize = 0.3f;
            p.lifetime = opt->arrival;
            p.target = opt->entity;
            p.sx = opt->target[0], p.sy = opt->target[1], p.sz = opt->target[2];
            p.e0 = opt->yOffset;
            p.flags &= u8(~Physics);
            p.flags |= Translucent;
            p.emissive = 15;
            double dest[3];
            if (vibrationTarget(p, dest)) {
                const double dx = x - dest[0], dy = y - dest[1], dz = z - dest[2];
                p.roll = p.oRoll = float(std::atan2(dx, dz));                          // rot
                p.e1 = p.e2 = float(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));   // pitch, pitchO
            }
            randomSprite();
            break;
        }
        case Kind::Drip: {  // DripParticle: position constructor, then the provider's colour / gravity / lifetime
            const DripDef& d = kDrips[size_t(t.drip)];
            base(x, y, z);
            p.r = d.r, p.g = d.g, p.b = d.b;
            p.gravity = d.gravity;
            if (d.life > 0) p.lifetime = int(d.life);
            else if (t.name == "falling_spore_blossom") p.lifetime = int(64.0f / (0.1f + randf() * 0.8f));
            else p.lifetime = int(-d.life / (randf() * 0.8f + 0.2f));
            if (d.glow) p.emissive = 15;
            randomSprite();
            break;
        }
        case Kind::Spark:
        case Kind::Totem:
        case Kind::EndRod: {  // SimpleAnimatedParticle: friction 0.91, fading out over the second half, full bright
            base(x, y, z);
            p.friction = 0.91f;
            p.xd = xa, p.yd = ya, p.zd = za;
            p.quadSize *= 0.75f;
            p.flags |= Translucent;
            p.emissive = 255;
            if (t.kind == Kind::Spark) {  // FireworkParticles.SparkParticle
                p.gravity = 0.1f;
                p.lifetime = 48 + int(rng_() % 12);
                p.a = 0.99f;
                if (opt && opt->spark) {  // the firework starter's colours and flags
                    color(opt->color);
                    p.e0 = float(opt->spark);
                    if (opt->spark & kSparkFade)
                        p.e1 = float((opt->color2 >> 16) & 255) / 255.0f, p.e2 = float((opt->color2 >> 8) & 255) / 255.0f,
                        p.e3 = float(opt->color2 & 255) / 255.0f;
                }
            } else if (t.kind == Kind::Totem) {  // TotemParticle
                p.gravity = 1.25f, p.friction = 0.6f;
                p.lifetime = 60 + int(rng_() % 12);
                if (rng_() % 4 == 0) p.r = 0.6f + randf() * 0.2f, p.g = 0.6f + randf() * 0.3f, p.b = randf() * 0.2f;
                else p.r = 0.1f + randf() * 0.2f, p.g = 0.4f + randf() * 0.3f, p.b = randf() * 0.2f;
            } else {  // EndRodParticle: no collisions, fades to #F2DEC9
                p.gravity = 0.0125f;
                p.lifetime = 60 + int(rng_() % 12);
                p.flags &= u8(~Physics);
                p.e0 = float(kSparkFade);
                p.e1 = 0xF2 / 255.0f, p.e2 = 0xDE / 255.0f, p.e3 = 0xC9 / 255.0f;
            }
            ageSprite();
            break;
        }
        case Kind::Flash:  // FireworkParticles.OverlayParticle: a big flash for four ticks (size / alpha in build)
            base(x, y, z);
            p.lifetime = 4;
            p.flags |= Translucent;
            if (opt && opt->hasColor) color(opt->color), p.a = float(opt->color >> 24) / 255.0f;
            randomSprite();
            break;
        case Kind::Gust:  // GustParticle: still, full bright, animated over 12-15 ticks
            base(x, y, z);
            p.lifetime = 12 + int(rng_() % 4);
            p.quadSize = t.name == "small_gust" ? 0.15f : 1.0f;
            p.emissive = 255;
            p.flags &= u8(~Physics);
            ageSprite();
            break;
        case Kind::GustSeed: {  // GustSeedParticle: emits gusts (scale, lifetime, ticks in between)
            base(x, y, z);
            const bool large = t.name == "gust_emitter_large";
            p.e0 = large ? 3.0f : 1.0f;
            p.lifetime = large ? 7 : 3;
            p.e1 = large ? 0.0f : 2.0f;
            p.flags |= NoRender;
            break;
        }
        case Kind::Rising:  // EmissiveRisingParticle (RisingParticle): soul / sculk_soul
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.xd = p.xd * 0.01f + xa, p.yd = p.yd * 0.01f + ya, p.zd = p.zd * 0.01f + za;
            p.x += (randf() - randf()) * 0.05f, p.y += (randf() - randf()) * 0.05f, p.z += (randf() - randf()) * 0.05f;
            p.xo = p.x, p.yo = p.y, p.zo = p.z;
            p.lifetime = int(8.0 / (randf() * 0.8 + 0.2)) + 4;
            p.quadSize *= 1.5f;
            p.flags |= Translucent;
            if (t.name == "sculk_soul") p.emissive = 15;
            ageSprite();
            break;
        case Kind::SculkCharge:
        case Kind::SculkChargePop:  // no collisions, glowing, flying at the given speed
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.xd = xa, p.yd = ya, p.zd = za;
            p.flags &= u8(~Physics);
            p.flags |= Translucent;
            p.emissive = 15;
            if (t.kind == Kind::SculkCharge) {
                p.quadSize *= 1.5f;
                p.roll = p.oRoll = opt ? opt->scale : 0.0f;
                p.lifetime = int(rng_() % 12) + 8;
            } else {
                p.lifetime = int(rng_() % 4) + 6;
            }
            ageSprite();
            break;
        case Kind::Shriek:  // ShriekParticle: two tilted rings rising after a delay
            base(x, y, z);
            p.quadSize = 0.85f;
            p.e0 = float(opt ? opt->delay : 0);
            p.lifetime = 30;
            p.xd = 0, p.yd = 0.1, p.zd = 0;
            p.flags |= Translucent;
            p.emissive = 15;
            randomSprite();
            break;
        case Kind::Firefly:  // FireflyParticle: wanders, fading in and out, glowing with its own light
            baseVel(x, y, z, 0.5 - randf(), rng_() & 1 ? ya : -ya, 0.5 - randf());
            p.flags |= SpeedUp | Translucent;
            p.friction = 0.96f;
            p.quadSize *= 0.75f * 1.5f;
            p.xd *= 0.8f, p.yd *= 0.8f, p.zd *= 0.8f;
            p.lifetime = 200 + int(rng_() % 101);
            p.a = 0;
            randomSprite();
            break;
        case Kind::Glow: {  // GlowParticle providers: glow squid, wax on / off, electric spark, scrape
            const bool squid = t.name == "glow";
            baseVel(x, y, z, squid ? 0.5 - randf() : 0.0, squid ? ya : 0.0, squid ? 0.5 - randf() : 0.0);
            p.friction = 0.96f;
            p.flags |= SpeedUp;
            p.flags &= u8(~Physics);
            p.quadSize *= 0.75f;
            if (squid) {
                if (rng_() & 1) p.r = 0.6f, p.g = 1.0f, p.b = 0.8f;
                else p.r = 0.08f, p.g = 0.4f, p.b = 0.4f;
                p.yd *= 0.2f;
                if (xa == 0.0 && za == 0.0) p.xd *= 0.1f, p.zd *= 0.1f;
                p.lifetime = int(8.0 / (randf() * 0.8 + 0.2));
            } else if (t.name == "electric_spark") {
                p.r = 1.0f, p.g = 0.9f, p.b = 1.0f;
                p.xd = xa * 0.25, p.yd = ya * 0.25, p.zd = za * 0.25;
                p.lifetime = int(rng_() % 2) + 2;
            } else if (t.name == "scrape") {
                if (rng_() & 1) p.r = 0.29f, p.g = 0.58f, p.b = 0.51f;
                else p.r = 0.43f, p.g = 0.77f, p.b = 0.62f;
                p.xd = xa * 0.01, p.yd = ya * 0.01, p.zd = za * 0.01;
                p.lifetime = int(rng_() % 30) + 10;
            } else {  // wax_on / wax_off
                if (t.name == "wax_on") p.r = 0.91f, p.g = 0.55f, p.b = 0.08f;
                else p.r = 1.0f, p.g = 0.9f, p.b = 1.0f;
                p.xd = xa * 0.01 / 2.0, p.yd = ya * 0.01, p.zd = za * 0.01 / 2.0;
                p.lifetime = int(rng_() % 30) + 10;
            }
            ageSprite();
            break;
        }
        case Kind::Snowflake:
            baseVel(x, y, z, xa, ya, za);
            p.gravity = 0.225f, p.friction = 1.0f;
            p.xd = xa + (randf() * 2.0f - 1.0f) * 0.05f, p.yd = ya + (randf() * 2.0f - 1.0f) * 0.05f;
            p.zd = za + (randf() * 2.0f - 1.0f) * 0.05f;
            p.quadSize = 0.1f * (randf() * randf() + 1.0f);
            p.lifetime = int(16.0 / (randf() * 0.8 + 0.2)) + 2;
            ageSprite();
            break;
        default:  // Generic: the base particle with the type's sprites
            baseVel(x, y, z, xa, ya, za);
            if (t.sprites.size() > 1) ageSprite();
            else randomSprite();
            if (opt && opt->hasColor) color(opt->color);
            break;
    }
    if ((p.flags & AgeSprite) && !t.sprites.empty()) p.sprite = t.sprites[0];
    if (p.sprite < 0 && !(p.flags & NoRender)) return nullptr;
    if (t.kind == Kind::Crit || t.kind == Kind::DamageIndicator || t.kind == Kind::EnchantedHit) tickOne(p);  // ticks once
    spawnQueue_.push_back(p);
    ++stats_.spawned;
    return &spawnQueue_.back();
}

void Particles::spawn(std::string_view type, double x, double y, double z, double vx, double vy, double vz) {
    add(typeId(type), x, y, z, vx, vy, vz);
}

void Particles::fireworks(const double pos[3], const double vel[3], const std::string& component, bool sound) {
    // Fireworks.STREAM_CODEC: flight duration, then the FireworkExplosion list.
    Starter s{{pos[0], pos[1], pos[2]}, {vel[0], vel[1], vel[2]}};
    s.sound = sound;
    net::Reader r(reinterpret_cast<const u8*>(component.data()), component.size());
    if (!component.empty()) {
        r.varInt();
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && i < 256 && r.ok; ++i) {
            Explosion e;
            e.shape = std::clamp(r.varInt(), 0, 4);
            for (std::vector<u32>* list : {&e.colors, &e.fades}) {
                const i32 c = r.varInt();
                for (i32 k = 0; k < c && k < 256 && r.ok; ++k) list->push_back(u32(r.i32v()) & 0xFFFFFF);
            }
            e.trail = r.boolean(), e.twinkle = r.boolean();
            if (r.ok) s.explosions.push_back(std::move(e));
        }
    }
    if (s.explosions.empty()) {  // no explosions: a few poofs
        static int poof = -2;
        if (poof == -2) poof = typeId("poof");
        for (int i = 0, n = int(rng_() % 3) + 2; i < n; ++i) add(poof, pos[0], pos[1], pos[2], gauss() * 0.05, 0.005, gauss() * 0.05);
        return;
    }
    s.lifetime = int(s.explosions.size()) * 2 - 1;
    for (const Explosion& e : s.explosions)
        if (e.twinkle) {
            s.twinkleDelay = true;
            s.lifetime += 15;
            break;
        }
    starters_.push_back(std::move(s));
}

void Particles::tickStarter(Starter& s) {
    static int spark = -2, flash = -2;
    if (spark == -2) spark = typeId("firework"), flash = typeId("flash");
    const double dx = s.pos[0] - cam_[0], dy = s.pos[1] - cam_[1], dz = s.pos[2] - cam_[2];
    const bool distant = dx * dx + dy * dy + dz * dz >= 256.0;
    auto play = [&](const char* name, float pitch) {
        if (audio::AudioSystem* a = audio::instance()) a->playNetwork(-1, name, audio::Category::Ambient, s.pos, 20.0f, pitch);
    };
    if (s.life == 0 && s.sound) {
        bool large = s.explosions.size() >= 3;
        for (const Explosion& e : s.explosions) large |= e.shape == 1;
        play(large ? (distant ? "minecraft:entity.firework_rocket.large_blast_far" : "minecraft:entity.firework_rocket.large_blast")
                   : (distant ? "minecraft:entity.firework_rocket.blast_far" : "minecraft:entity.firework_rocket.blast"),
             0.95f + randf() * 0.1f);
    }
    if (s.life % 2 == 0 && size_t(s.life / 2) < s.explosions.size()) {
        const Explosion& e = s.explosions[size_t(s.life / 2)];
        const std::vector<u32> black = {0x1E1B1B};  // DyeColor.BLACK firework colour
        const std::vector<u32>& colors = e.colors.empty() ? black : e.colors;
        auto one = [&](double xa, double ya, double za) {  // Starter.createParticle
            Options o;
            o.spark = kSparkSet | (e.trail ? kSparkTrail : 0) | (e.twinkle ? kSparkTwinkle : 0) | (e.fades.empty() ? 0 : kSparkFade);
            o.color = colors[rng_() % colors.size()];
            if (!e.fades.empty()) o.color2 = e.fades[rng_() % e.fades.size()];
            add(spark, s.pos[0], s.pos[1], s.pos[2], xa, ya, za, &o);
        };
        auto ball = [&](double speed, int steps) {
            for (int y = -steps; y <= steps; ++y)
                for (int x = -steps; x <= steps; ++x)
                    for (int z = -steps; z <= steps; ++z) {
                        const double xa = x + (randf() - randf()) * 0.5, ya = y + (randf() - randf()) * 0.5, za = z + (randf() - randf()) * 0.5;
                        const double len = std::sqrt(xa * xa + ya * ya + za * za) / speed + gauss() * 0.05;
                        one(xa / len, ya / len, za / len);
                        if (y != -steps && y != steps && x != -steps && x != steps) z += steps * 2 - 1;
                    }
        };
        auto shape = [&](double speed, const double (*coords)[2], int count, bool flat) {
            one(coords[0][0] * speed, coords[0][1] * speed, 0);
            const float baseAngle = randf() * kPi;
            const double mod = flat ? 0.034 : 0.34;
            for (int step = 0; step < 3; ++step) {
                const double angle = baseAngle + step * kPi * mod;
                double ox = coords[0][0], oy = coords[0][1];
                for (int c = 1; c < count; ++c) {
                    const double tx = coords[c][0], ty = coords[c][1];
                    for (double sub = 0.25; sub <= 1.0; sub += 0.25) {
                        double xa = (ox + (tx - ox) * sub) * speed;
                        const double ya = (oy + (ty - oy) * sub) * speed;
                        const double za = xa * std::sin(angle);
                        xa *= std::cos(angle);
                        for (double flip = -1.0; flip <= 1.0; flip += 2.0) one(xa * flip, ya, za * flip);
                    }
                    ox = tx, oy = ty;
                }
            }
        };
        static const double kCreeper[][2] = {{0.0, 0.2}, {0.2, 0.2}, {0.2, 0.6}, {0.6, 0.6}, {0.6, 0.2}, {0.2, 0.2},
                                             {0.2, 0.0}, {0.4, 0.0}, {0.4, -0.6}, {0.2, -0.6}, {0.2, -0.4}, {0.0, -0.4}};
        static const double kStar[][2] = {{0.0, 1.0}, {0.3455, 0.309}, {0.9511, 0.309}, {0.3795918367346939, -0.12653061224489795},
                                          {0.6122448979591837, -0.8040816326530612}, {0.0, -0.35918367346938773}};
        switch (e.shape) {
            case 0: ball(0.25, 2); break;
            case 1: ball(0.5, 4); break;
            case 2: shape(0.5, kStar, 6, false); break;
            case 3: shape(0.5, kCreeper, 12, true); break;
            default: {  // burst
                const double offX = gauss() * 0.05, offZ = gauss() * 0.05;
                for (int i = 0; i < 70; ++i)
                    one(s.vel[0] * 0.5 + gauss() * 0.15 + offX, s.vel[1] * 0.5 + randf() * 0.5, s.vel[2] * 0.5 + gauss() * 0.15 + offZ);
            }
        }
        Options o;  // ColorParticleOption.create(FLASH, colour): the flash takes the first colour
        o.hasColor = true;
        o.color = colors[0];
        add(flash, s.pos[0], s.pos[1], s.pos[2], 0, 0, 0, &o);
    }
    if (++s.life > s.lifetime) {
        if (s.twinkleDelay && s.sound)
            play(distant ? "minecraft:entity.firework_rocket.twinkle_far" : "minecraft:entity.firework_rocket.twinkle", 0.9f + randf() * 0.15f);
        s.life = -1;  // done
    }
}

// ClientLevel.addDestroyBlockEffect: a grid of shards over the block's shape boxes.
void Particles::blockDestroyed(int x, int y, int z, u16 block) {
    if (!block) return;
    static int terrain = -2;
    if (terrain == -2) terrain = typeId("block");
    const BlockInfo& b = registry_.infos()[block];
    if (b.kind == KindAir || (b.fluid && !b.waterlogged)) return;
    // Network state for the block: find it through the reports' key (the shards only need the sprite).
    Options opt;
    opt.block = -1;
    u32 tint;
    const int sprite = blockSprite(block, tint);
    if (sprite < 0) return;
    Box full{{0, 0, 0}, {1, 1, 1}};
    const u32 boxes = b.selectionCount ? b.selectionCount : 1;
    for (u32 bi = 0; bi < boxes; ++bi) {
        const Box& s = b.selectionCount ? registry_.box(b.selectionFirst + bi) : full;
        const double wx = std::min(1.0, double(s.max[0] - s.min[0])), wy = std::min(1.0, double(s.max[1] - s.min[1]));
        const double wz = std::min(1.0, double(s.max[2] - s.min[2]));
        const int cx = std::max(2, int(std::ceil(wx / 0.25))), cy = std::max(2, int(std::ceil(wy / 0.25)));
        const int cz = std::max(2, int(std::ceil(wz / 0.25)));
        for (int xx = 0; xx < cx; ++xx)
            for (int yy = 0; yy < cy; ++yy)
                for (int zz = 0; zz < cz; ++zz) {
                    const double rx = (xx + 0.5) / cx, ry = (yy + 0.5) / cy, rz = (zz + 0.5) / cz;
                    if (live_.size() + spawnQueue_.size() >= kMaxParticles) return;
                    Particle p{};
                    p.type = u16(std::max(terrain, 0));
                    p.a = 1;
                    p.flags = Physics;
                    const double px = x + rx * wx + s.min[0], py = y + ry * wy + s.min[1], pz = z + rz * wz + s.min[2];
                    // TerrainParticle(level, x, y, z, relX - 0.5, relY - 0.5, relZ - 0.5, state)
                    p.x = p.xo = px, p.y = p.yo = py, p.z = p.zo = pz;
                    p.lifetime = int(4.0f / (randf() * 0.9f + 0.1f));
                    const double vx = rx - 0.5, vy = ry - 0.5, vz = rz - 0.5;
                    p.xd = vx + (randf() * 2.0f - 1.0f) * 0.4f, p.yd = vy + (randf() * 2.0f - 1.0f) * 0.4f;
                    p.zd = vz + (randf() * 2.0f - 1.0f) * 0.4f;
                    const double speed = (randf() + randf() + 1.0f) * 0.15f;
                    const double dd = std::max(std::sqrt(p.xd * p.xd + p.yd * p.yd + p.zd * p.zd), 1e-9);
                    p.xd = p.xd / dd * speed * 0.4f, p.yd = p.yd / dd * speed * 0.4f + 0.1f, p.zd = p.zd / dd * speed * 0.4f;
                    p.quadSize = 0.1f * (randf() * 0.5f + 0.5f) * 2.0f / 2.0f;
                    p.gravity = 1.0f, p.friction = 0.98f;
                    p.r = 0.6f * float((tint >> 16) & 255) / 255.0f, p.g = 0.6f * float((tint >> 8) & 255) / 255.0f;
                    p.b = 0.6f * float(tint & 255) / 255.0f;
                    p.sprite = sprite;
                    p.uo = randf() * 3.0f, p.vo = randf() * 3.0f;
                    if (items_.rect(sprite).translucent) p.flags |= Translucent;
                    p.light = lightAt(px, py, pz);
                    spawnQueue_.push_back(p);
                    ++stats_.spawned;
                }
    }
}

void Particles::blockCrack(int x, int y, int z, int face) {
    const u16 block = world_.block(x, y, z);
    if (!block) return;
    static int terrain = -2;
    if (terrain == -2) terrain = typeId("block");
    const BlockInfo& b = registry_.infos()[block];
    if (b.kind == KindAir || (b.fluid && !b.waterlogged)) return;
    u32 tint;
    const int sprite = blockSprite(block, tint);
    if (sprite < 0 || live_.size() + spawnQueue_.size() >= kMaxParticles) return;
    // The outline shape's bounds; a point 0.1 inside them, pushed 0.1 out of the hit face.
    Box s{{1, 1, 1}, {0, 0, 0}};
    for (u32 i = 0; i < b.selectionCount; ++i) {
        const Box& o = registry_.box(b.selectionFirst + i);
        for (int a = 0; a < 3; ++a) s.min[a] = std::min(s.min[a], o.min[a]), s.max[a] = std::max(s.max[a], o.max[a]);
    }
    if (!b.selectionCount) s = {{0, 0, 0}, {1, 1, 1}};
    auto coord = [&](int a) { return double(randf()) * (s.max[a] - s.min[a] - 0.2f) + 0.1f + s.min[a]; };
    double px = x + coord(0), py = y + coord(1), pz = z + coord(2);
    switch (face) {
        case 0: py = y + s.min[1] - 0.1f; break;
        case 1: py = y + s.max[1] + 0.1f; break;
        case 2: pz = z + s.min[2] - 0.1f; break;
        case 3: pz = z + s.max[2] + 0.1f; break;
        case 4: px = x + s.min[0] - 0.1f; break;
        case 5: px = x + s.max[0] + 0.1f; break;
    }
    // new TerrainParticle(level, x, y, z, 0, 0, 0, state, pos).setPower(0.2F).scale(0.6F)
    Particle p{};
    p.type = u16(std::max(terrain, 0));
    p.a = 1;
    p.flags = Physics;
    p.x = p.xo = px, p.y = p.yo = py, p.z = p.zo = pz;
    p.lifetime = int(4.0f / (randf() * 0.9f + 0.1f));
    p.xd = (randf() * 2.0f - 1.0f) * 0.4f, p.yd = (randf() * 2.0f - 1.0f) * 0.4f, p.zd = (randf() * 2.0f - 1.0f) * 0.4f;
    const double speed = (randf() + randf() + 1.0f) * 0.15f;
    const double dd = std::max(std::sqrt(p.xd * p.xd + p.yd * p.yd + p.zd * p.zd), 1e-9);
    p.xd = p.xd / dd * speed * 0.4f, p.yd = p.yd / dd * speed * 0.4f + 0.1f, p.zd = p.zd / dd * speed * 0.4f;
    p.xd *= 0.2f, p.yd = (p.yd - 0.1f) * 0.2f + 0.1f, p.zd *= 0.2f;  // setPower(0.2)
    p.quadSize = 0.1f * (randf() * 0.5f + 0.5f) * 2.0f / 2.0f * 0.6f;  // scale(0.6)
    p.gravity = 1.0f, p.friction = 0.98f;
    p.r = 0.6f * float((tint >> 16) & 255) / 255.0f, p.g = 0.6f * float((tint >> 8) & 255) / 255.0f;
    p.b = 0.6f * float(tint & 255) / 255.0f;
    p.sprite = sprite;
    p.uo = randf() * 3.0f, p.vo = randf() * 3.0f;
    if (items_.rect(sprite).translucent) p.flags |= Translucent;
    p.light = lightAt(px, py, pz);
    spawnQueue_.push_back(p);
    ++stats_.spawned;
}

void Particles::itemShard(int item, double x, double y, double z, double xa, double ya, double za) {
    int& sprite = itemSprites_[item];  // 0 unresolved, -1 none, else rect + 1
    if (sprite == 0) {
        const std::string tex = items_.itemParticleTexture(item);
        const int rect = tex.empty() ? -1 : items_.texture(tex);
        sprite = rect >= 0 ? rect + 1 : -1;
    }
    if (sprite > 0) shard(sprite - 1, x, y, z, xa, ya, za, true);
}

void Particles::shard(int rect, double x, double y, double z, double xa, double ya, double za, bool scaled) {
    static int itemType = -2;  // ticked like terrain shards (same motion as BreakingItemParticle)
    if (itemType == -2) itemType = typeId("block");
    if (live_.size() + spawnQueue_.size() >= kMaxParticles) return;
    Particle p{};
    p.type = u16(std::max(itemType, 0));
    p.a = 1, p.r = p.g = p.b = 1;
    p.flags = Physics;
    p.target = -1;
    p.x = p.xo = x, p.y = p.yo = y, p.z = p.zo = z;
    p.lifetime = int(4.0f / (randf() * 0.9f + 0.1f));
    // Particle(level, x, y, z, 0, 0, 0): a random push; with a velocity BreakingItemParticle keeps a tenth of it plus
    // (xa, ya, za).
    p.xd = (randf() * 2.0f - 1.0f) * 0.4f, p.yd = (randf() * 2.0f - 1.0f) * 0.4f, p.zd = (randf() * 2.0f - 1.0f) * 0.4f;
    const double speed = (randf() + randf() + 1.0f) * 0.15f;
    const double dd = std::max(std::sqrt(p.xd * p.xd + p.yd * p.yd + p.zd * p.zd), 1e-9);
    p.xd = p.xd / dd * speed * 0.4f, p.yd = p.yd / dd * speed * 0.4f + 0.1f, p.zd = p.zd / dd * speed * 0.4f;
    if (scaled) p.xd = p.xd * 0.1f + xa, p.yd = p.yd * 0.1f + ya, p.zd = p.zd * 0.1f + za;
    p.quadSize = 0.1f * (randf() * 0.5f + 0.5f) * 2.0f / 2.0f;
    p.gravity = 1.0f, p.friction = 0.98f;
    p.sprite = rect;
    p.uo = randf() * 3.0f, p.vo = randf() * 3.0f;
    if (items_.rect(p.sprite).translucent) p.flags |= Translucent;
    p.light = lightAt(x, y, z);
    spawnQueue_.push_back(p);
    ++stats_.spawned;
}

// PositionSource.getPosition: the block centre, or the entity's position raised by its y offset.
bool Particles::vibrationTarget(const Particle& p, double out[3]) {
    if (p.target < 0) {
        out[0] = p.sx, out[1] = p.sy, out[2] = p.sz;
        return true;
    }
    if (!entityPosition || !entityPosition(p.target, out)) return false;
    out[1] += p.e0;
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Simulation

void Particles::move(Particle& p, double xa, double ya, double za) {
    if (p.flags & Stopped) return;
    const double ox = xa, oy = ya, oz = za;
    if ((p.flags & Physics) && (xa != 0 || ya != 0 || za != 0) && xa * xa + ya * ya + za * za < 10000.0) {
        // Point collision against block collision boxes: vertical first, then the horizontal axes.
        auto boxesAt = [&](double px, double py, double pz, Box& hit) {
            const int bx = int(std::floor(px)), by = int(std::floor(py)), bz = int(std::floor(pz));
            const BlockInfo& b = registry_.infos()[blockAt(bx, by, bz)];
            for (u32 i = 0; i < b.collisionCount; ++i) {
                const Box& s = registry_.box(b.collisionFirst + i);
                const double lx = px - bx, ly = py - by, lz = pz - bz;
                if (lx >= s.min[0] && lx <= s.max[0] && ly >= s.min[1] && ly < s.max[1] && lz >= s.min[2] && lz <= s.max[2]) {
                    hit = {{s.min[0] + bx, s.min[1] + by, s.min[2] + bz}, {s.max[0] + bx, s.max[1] + by, s.max[2] + bz}};
                    return true;
                }
            }
            return false;
        };
        Box hit;
        if (ya != 0 && boxesAt(p.x, p.y + ya, p.z, hit)) ya = ya < 0 ? std::min(0.0, hit.max[1] - p.y) : 0.0;
        if (xa != 0 && boxesAt(p.x + xa, p.y + ya + 0.01, p.z, hit)) xa = 0;
        if (za != 0 && boxesAt(p.x + xa, p.y + ya + 0.01, p.z + za, hit)) za = 0;
    }
    p.x += xa, p.y += ya, p.z += za;
    if (std::abs(oy) >= 1.0e-5 && std::abs(ya) < 1.0e-5) p.flags |= Stopped;
    if (oy != ya && oy < 0.0) p.flags |= OnGround;
    else p.flags &= u8(~OnGround);
    if (ox != xa) p.xd = 0;
    if (oz != za) p.zd = 0;
}

void Particles::tickOne(Particle& p) {
    const Type& t = types_[p.type];
    p.xo = p.x, p.yo = p.y, p.zo = p.z;
    auto remove = [&] { p.flags |= Removed; };
    auto ageSprite = [&] {
        if ((p.flags & AgeSprite) && !t.sprites.empty() && p.lifetime > 0)
            p.sprite = t.sprites[std::min(size_t(p.age) * (t.sprites.size() - 1) / size_t(p.lifetime), t.sprites.size() - 1)];
    };
    auto defaultTick = [&] {
        if (p.age++ >= p.lifetime) return remove();
        p.yd -= 0.04 * p.gravity;
        move(p, p.xd, p.yd, p.zd);
        if ((p.flags & SpeedUp) && p.y == p.yo) p.xd *= 1.1, p.zd *= 1.1;
        p.xd *= p.friction, p.yd *= p.friction, p.zd *= p.friction;
        if (p.flags & OnGround) p.xd *= 0.7f, p.zd *= 0.7f;
    };
    switch (t.kind) {
        case Kind::Rain:
        case Kind::Splash: {
            if (p.lifetime-- <= 0) return remove();
            p.yd -= p.gravity;
            move(p, p.xd, p.yd, p.zd);
            p.xd *= 0.98f, p.yd *= 0.98f, p.zd *= 0.98f;
            if (p.flags & OnGround) {
                if (randf() < 0.5f) return remove();
                p.xd *= 0.7f, p.zd *= 0.7f;
            }
            const int bx = int(std::floor(p.x)), by = int(std::floor(p.y)), bz = int(std::floor(p.z));
            const BlockInfo& b = registry_.infos()[blockAt(bx, by, bz)];
            double top = (b.fluid || b.waterlogged) ? 0.875 : 0.0;
            for (u32 i = 0; i < b.collisionCount; ++i) top = std::max(top, double(registry_.box(b.collisionFirst + i).max[1]));
            if (top > 0 && p.y < by + top) remove();
            break;
        }
        case Kind::Explosion:
            if (p.age++ >= p.lifetime) return remove();
            ageSprite();
            break;
        case Kind::ExplosionSeed: {
            static int explosion = -2;
            if (explosion == -2) explosion = typeId("explosion");
            for (int i = 0; i < 6; ++i)
                add(explosion, p.x + (randf() - randf()) * 4.0, p.y + (randf() - randf()) * 4.0, p.z + (randf() - randf()) * 4.0,
                    float(p.age) / float(p.lifetime), 0, 0);
            if (++p.age == p.lifetime) remove();
            break;
        }
        case Kind::Bubble: {
            if (p.lifetime-- <= 0) return remove();
            p.yd += 0.002;
            move(p, p.xd, p.yd, p.zd);
            p.xd *= 0.85f, p.yd *= 0.85f, p.zd *= 0.85f;
            const BlockInfo& b = registry_.infos()[blockAt(int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z)))];
            if (!(b.fluid || b.waterlogged)) remove();
            break;
        }
        case Kind::BubblePop:
            if (p.age++ >= p.lifetime) return remove();
            p.yd -= p.gravity;
            move(p, p.xd, p.yd, p.zd);
            ageSprite();
            break;
        case Kind::FallingDust:
            if (p.age++ >= p.lifetime) return remove();
            p.oRoll = p.roll;
            p.roll += kPi * p.e0 * 2.0f;
            if (p.flags & OnGround) p.oRoll = p.roll = 0;
            move(p, p.xd, p.yd, p.zd);
            p.yd -= 0.003f;
            p.yd = std::max(p.yd, -0.14);
            break;
        case Kind::CherryLeaves:
        case Kind::PaleLeaves:
        case Kind::TintedLeaves: {
            if (p.lifetime-- <= 0) return remove();
            const float alive = float(300 - p.lifetime), rel = std::min(alive / 300.0f, 1.0f);
            double xa = 0, za = 0;
            if (p.e3 > 0.5f) xa += p.sx * std::pow(rel, 1.25), za += p.sz * std::pow(rel, 1.25);
            else xa += rel * std::cos(rel * p.sy) * p.e0, za += rel * std::sin(rel * p.sy) * p.e0;
            p.xd += xa * 0.0025f, p.zd += za * 0.0025f;
            p.yd -= p.gravity;
            p.e1 += p.e2 / 20.0f;
            p.oRoll = p.roll;
            p.roll += p.e1 / 20.0f;
            move(p, p.xd, p.yd, p.zd);
            if ((p.flags & OnGround) || (p.lifetime < 299 && (p.xd == 0.0 || p.zd == 0.0))) remove();
            break;
        }
        case Kind::Portal: {
            if (p.age++ >= p.lifetime) return remove();
            const float pos = float(p.age) / float(p.lifetime);
            const float v3 = -pos + pos * pos * 2.0f, v4 = 1.0f - v3;
            p.x = p.sx + p.xd * v4, p.y = p.sy + p.yd * v4 + (1.0f - pos), p.z = p.sz + p.zd * v4;
            break;
        }
        case Kind::Enchant: {
            if (p.age++ >= p.lifetime) return remove();
            float pos = 1.0f - float(p.age) / float(p.lifetime);
            float pp = 1.0f - pos;
            pp *= pp, pp *= pp;
            p.x = p.sx + p.xd * pos, p.y = p.sy + p.yd * pos - pp * 1.2f, p.z = p.sz + p.zd * pos;
            break;
        }
        case Kind::CampfireCosy:
        case Kind::CampfireSignal:
            if (p.age++ < p.lifetime && p.a > 0.0f) {
                p.xd += randf() / 5000.0f * (rng_() & 1 ? 1 : -1);
                p.zd += randf() / 5000.0f * (rng_() & 1 ? 1 : -1);
                p.yd -= p.gravity;
                move(p, p.xd, p.yd, p.zd);
                if (p.age >= p.lifetime - 60 && p.a > 0.01f) p.a -= 0.015f;
            } else {
                remove();
            }
            break;
        case Kind::Suspended:
        case Kind::HappyVillager:
            if (p.lifetime-- <= 0) return remove();
            move(p, p.xd, p.yd, p.zd);
            p.xd *= 0.99, p.yd *= 0.99, p.zd *= 0.99;
            break;
        case Kind::Spell:
        case Kind::Witch:
            defaultTick();
            ageSprite();
            p.a += (p.e0 - p.a) * 0.05f;
            break;
        case Kind::Crit:
        case Kind::DamageIndicator:
        case Kind::EnchantedHit:
            defaultTick();
            p.g *= 0.96f, p.b *= 0.9f;
            break;
        case Kind::Vibration: {
            if (p.age++ >= p.lifetime) return remove();
            double dest[3];
            if (!vibrationTarget(p, dest)) return remove();
            const int remaining = p.lifetime - p.age;  // the rest of the way over the ticks remaining
            const double alpha = remaining > 0 ? 1.0 / double(remaining) : 1.0;
            p.x += (dest[0] - p.x) * alpha, p.y += (dest[1] - p.y) * alpha, p.z += (dest[2] - p.z) * alpha;
            const double dx = p.x - dest[0], dy = p.y - dest[1], dz = p.z - dest[2];
            p.oRoll = p.roll, p.roll = float(std::atan2(dx, dz));
            p.e2 = p.e1, p.e1 = float(std::atan2(dy, std::sqrt(dx * dx + dz * dz)));
            break;
        }
        case Kind::Drip: {
            const DripDef& d = kDrips[size_t(t.drip)];
            if (d.stage == DripCoolingHang)  // CoolingDripHangParticle: lava drops cool from white to orange
                p.r = 1.0f, p.g = 16.0f / float(40 - p.lifetime + 16), p.b = 4.0f / float(40 - p.lifetime + 8);
            if (p.lifetime-- <= 0) {  // preMoveUpdate: a hanging drop lets go
                remove();
                if (d.stage == DripHang || d.stage == DripCoolingHang) add(dripNext(types_[p.type]), p.x, p.y, p.z, p.xd, p.yd, p.zd);
                return;
            }
            p.yd -= p.gravity;
            move(p, p.xd, p.yd, p.zd);
            if (d.stage == DripHang || d.stage == DripCoolingHang) {
                p.xd *= 0.02, p.yd *= 0.02, p.zd *= 0.02;
            } else if (d.stage != DripLand && (p.flags & OnGround)) {  // postMoveUpdate on the ground
                remove();
                if (d.stage != DripFall) add(dripNext(types_[p.type]), p.x, p.y, p.z, 0, 0, 0);
                const char* sound = d.stage == DripHoneyFallLand ? "minecraft:block.beehive.drip"
                                    : d.stage != DripDripstoneFallLand ? nullptr
                                    : d.fluid == 2 ? "minecraft:block.pointed_dripstone.drip_lava"
                                                   : "minecraft:block.pointed_dripstone.drip_water";
                if (sound)
                    if (audio::AudioSystem* a = audio::instance()) {
                        const double pos[3] = {p.x, p.y, p.z};
                        a->playNetwork(-1, sound, audio::Category::Block, pos, 0.3f + randf() * 0.7f, 1.0f);
                    }
                return;
            }
            p.xd *= 0.98f, p.yd *= 0.98f, p.zd *= 0.98f;
            if (d.fluid) {  // into its own fluid: gone
                const int bx = int(std::floor(p.x)), by = int(std::floor(p.y)), bz = int(std::floor(p.z));
                const u16 id = blockAt(bx, by, bz);
                const BlockInfo& b = registry_.infos()[id];
                const bool lava = registry_.name(id).starts_with("minecraft:lava");
                if ((d.fluid == 2 ? lava : (b.fluid || b.waterlogged) && !lava) && p.y < by + 0.875) remove();
            }
            break;
        }
        case Kind::Spark:
        case Kind::Totem:
        case Kind::EndRod: {
            defaultTick();
            ageSprite();
            if (p.age > p.lifetime / 2) {  // SimpleAnimatedParticle.tick: fade out, towards the fade colour
                p.a = 1.0f - (float(p.age) - float(p.lifetime / 2)) / float(p.lifetime);
                if (int(p.e0) & kSparkFade)
                    p.r += (p.e1 - p.r) * 0.2f, p.g += (p.e2 - p.g) * 0.2f, p.b += (p.e3 - p.b) * 0.2f;
            }
            // SparkParticle.tick: a trail of sparks over the first half
            if (t.kind == Kind::Spark && (int(p.e0) & kSparkTrail) && p.age < p.lifetime / 2 && (p.age + p.lifetime) % 2 == 0) {
                Options o;
                o.spark = (int(p.e0) & ~kSparkTrail) | kSparkSet;
                o.color = (u32(p.r * 255.0f) << 16) | (u32(p.g * 255.0f) << 8) | u32(p.b * 255.0f);
                o.color2 = (u32(p.e1 * 255.0f) << 16) | (u32(p.e2 * 255.0f) << 8) | u32(p.e3 * 255.0f);
                if (Particle* q = add(p.type, p.x, p.y, p.z, 0, 0, 0, &o)) q->age = q->lifetime / 2, q->r = p.r, q->g = p.g, q->b = p.b;
            }
            break;
        }
        case Kind::Flash:
            defaultTick();
            break;
        case Kind::Gust:
            if (p.age++ >= p.lifetime) return remove();
            ageSprite();
            break;
        case Kind::GustSeed: {
            static int gust = -2;
            if (gust == -2) gust = typeId("gust");
            const float scale = p.e0;
            if (p.age % (int(p.e1) + 1) == 0)
                for (int i = 0; i < 3; ++i)
                    add(gust, p.x + (randf() - randf()) * scale, p.y + (randf() - randf()) * scale, p.z + (randf() - randf()) * scale,
                        float(p.age) / float(p.lifetime), 0, 0);
            if (p.age++ == p.lifetime) remove();
            break;
        }
        case Kind::Rising:
        case Kind::SculkCharge:
        case Kind::SculkChargePop:
        case Kind::Glow:
            defaultTick();
            ageSprite();
            break;
        case Kind::Shriek:
            if (p.e0 > 0) {
                p.e0 -= 1;
                p.xo = p.x, p.yo = p.y, p.zo = p.z;
            } else {
                defaultTick();
            }
            break;
        case Kind::Firefly: {
            defaultTick();
            if (p.flags & Removed) break;
            if (registry_.infos()[blockAt(int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z)))].kind != KindAir) return remove();
            p.a = fadeAmount(std::clamp(float(p.age) / float(p.lifetime), 0.0f, 1.0f), 0.3f, 0.5f);
            if (randf() > 0.95f || p.age == 1)
                p.xd = -0.05f + 0.1f * randf(), p.yd = -0.05f + 0.1f * randf(), p.zd = -0.05f + 0.1f * randf();
            break;
        }
        case Kind::DustTransition: {
            defaultTick();
            ageSprite();
            const float f = float(p.age) / (float(p.lifetime) + 1.0f);  // lerpColors (per tick)
            p.r = float(p.sx) + (p.e1 - float(p.sx)) * f, p.g = float(p.sy) + (p.e2 - float(p.sy)) * f;
            p.b = float(p.sz) + (p.e3 - float(p.sz)) * f;
            break;
        }
        case Kind::Lava:
            defaultTick();
            if (!(p.flags & Removed) && randf() > float(p.age) / float(p.lifetime)) {
                static int smoke = -2;
                if (smoke == -2) smoke = typeId("smoke");
                add(smoke, p.x, p.y, p.z, p.xd, p.yd, p.zd);
            }
            break;
        default:
            defaultTick();
            ageSprite();
            break;
    }
    if (p.lifetime <= 0 && p.age > 1000) remove();
}

void Particles::ambient(const double cam[3]) {
    // ClientLevel.animateTick: random blocks around the camera run their animateTick.
    const int cx = int(std::floor(cam[0])), cy = int(std::floor(cam[1])), cz = int(std::floor(cam[2]));
    static int ids[10] = {-2};
    if (ids[0] == -2) {
        const char* names[10] = {"flame", "smoke", "soul_fire_flame", "tinted_leaves", "cherry_leaves", "pale_oak_leaves",
                                 "lava", "campfire_cosy_smoke", "dripping_water", "dripping_lava"};
        for (int i = 0; i < 10; ++i) ids[i] = typeId(names[i]);
    }
    enum Amb : u8 { AmbNone, AmbTorch, AmbWallTorch, AmbSoulTorch, AmbSoulWallTorch, AmbTinted, AmbCherry, AmbPale, AmbLava,
                    AmbCampfire, AmbFurnace, AmbWater };
    auto classify = [&](u16 id) -> u8 {
        if (blockKinds_.size() <= id) blockKinds_.resize(size_t(id) + 1, 0);
        if (!blockKinds_[id]) {
            const std::string& n = registry_.name(id);
            u8 k = AmbNone;
            if (n.starts_with("minecraft:torch")) k = AmbTorch;
            else if (n.starts_with("minecraft:wall_torch")) k = AmbWallTorch;
            else if (n.starts_with("minecraft:soul_torch")) k = AmbSoulTorch;
            else if (n.starts_with("minecraft:soul_wall_torch")) k = AmbSoulWallTorch;
            else if (n.starts_with("minecraft:cherry_leaves")) k = AmbCherry;
            else if (n.starts_with("minecraft:pale_oak_leaves")) k = AmbPale;
            else if (n.starts_with("minecraft:oak_leaves") || n.starts_with("minecraft:birch_leaves") ||
                     n.starts_with("minecraft:jungle_leaves") || n.starts_with("minecraft:acacia_leaves") ||
                     n.starts_with("minecraft:dark_oak_leaves") || n.starts_with("minecraft:mangrove_leaves"))
                k = AmbTinted;
            else if (n.starts_with("minecraft:lava")) k = AmbLava;
            else if (n.starts_with("minecraft:water") || n.starts_with("minecraft:bubble_column") || registry_.infos()[id].waterlogged)
                k = AmbWater;
            else if ((n.starts_with("minecraft:campfire") || n.starts_with("minecraft:soul_campfire")) && n.find("lit=true") != std::string::npos)
                k = AmbCampfire;
            else if ((n.starts_with("minecraft:furnace") || n.starts_with("minecraft:smoker") || n.starts_with("minecraft:blast_furnace")) &&
                     n.find("lit=true") != std::string::npos)
                k = AmbFurnace;
            blockKinds_[id] = u8(k + 1);
        }
        return u8(blockKinds_[id] - 1);
    };
    auto facingStep = [&](u16 id, int& sx, int& sz) {
        const std::string_view f = registry_.property(id, "facing");
        sx = f == "east" ? 1 : f == "west" ? -1 : 0;
        sz = f == "south" ? 1 : f == "north" ? -1 : 0;
    };
    for (int i = 0; i < 667; ++i)
        for (int radius : {16, 32}) {
            const int x = cx + int(rng_() % u32(radius)) - int(rng_() % u32(radius));
            const int y = cy + int(rng_() % u32(radius)) - int(rng_() % u32(radius));
            const int z = cz + int(rng_() % u32(radius)) - int(rng_() % u32(radius));
            const u16 id = blockAt(x, y, z);
            if (!id) continue;
            const u8 k = classify(id);
            switch (k) {
                case AmbTorch:
                case AmbSoulTorch:
                    add(ids[1], x + 0.5, y + 0.7, z + 0.5, 0, 0, 0);
                    add(k == AmbTorch ? ids[0] : ids[2], x + 0.5, y + 0.7, z + 0.5, 0, 0, 0);
                    break;
                case AmbWallTorch:
                case AmbSoulWallTorch: {
                    int sx, sz;
                    facingStep(id, sx, sz);
                    const double px = x + 0.5 - 0.27 * sx, py = y + 0.7 + 0.22, pz = z + 0.5 - 0.27 * sz;
                    add(ids[1], px, py, pz, 0, 0, 0);
                    add(k == AmbWallTorch ? ids[0] : ids[2], px, py, pz, 0, 0, 0);
                    break;
                }
                case AmbTinted:
                case AmbCherry:
                case AmbPale: {
                    const float chance = k == AmbCherry ? 0.1f : k == AmbPale ? 0.02f : 0.01f;
                    if (randf() >= chance) break;
                    const BlockInfo& below = registry_.infos()[blockAt(x, y - 1, z)];
                    if (below.occludes) break;
                    Options o;
                    o.hasColor = true;
                    o.color = 0xFF48B518;  // FoliageColor default (no biome blending here)
                    add(k == AmbTinted ? ids[3] : k == AmbCherry ? ids[4] : ids[5], x + double(randf()), y - 0.05,
                        z + double(randf()), 0, 0, 0, &o);
                    break;
                }
                case AmbLava:
                    if (rng_() % 100 == 0 && blockAt(x, y + 1, z) == 0)
                        add(ids[6], x + double(randf()), y + 1.0, z + double(randf()), 0, 0, 0);
                    if (rng_() % 10 == 0) fluidDrip(id, x, y, z, ids[9]);
                    break;
                case AmbWater:
                    if (rng_() % 10 == 0) fluidDrip(id, x, y, z, ids[8]);
                    break;
                case AmbCampfire:  // BlockPos.asLong
                    campfires_.insert((u64(x & 0x3FFFFFF) << 38) | (u64(z & 0x3FFFFFF) << 12) | u64(y & 0xFFF));
                    break;
                case AmbFurnace: {
                    int sx, sz;
                    facingStep(id, sx, sz);
                    const double px = x + 0.5 + sx * 0.52 + (sx ? 0 : randf() * 0.6 - 0.3);
                    const double pz = z + 0.5 + sz * 0.52 + (sz ? 0 : randf() * 0.6 - 0.3);
                    const double py = y + randf() * 6.0 / 16.0;
                    add(ids[1], px, py, pz, 0, 0, 0);
                    add(ids[0], px, py, pz, 0, 0, 0);
                    break;
                }
                default: break;
            }
        }
}

// ClientLevel.trySpawnDripParticles: a fluid drips through the ceiling block below it into the air under that.
void Particles::fluidDrip(u16 fluidBlock, int x, int y, int z, int drip) {
    auto shape = [&](u16 id, Box& out) {  // the collision shape's bounds; false: empty
        const BlockInfo& b = registry_.infos()[id];
        if (!b.collisionCount) return false;
        out = {{1, 1, 1}, {0, 0, 0}};
        for (u32 i = 0; i < b.collisionCount; ++i) {
            const Box& c = registry_.box(b.collisionFirst + i);
            for (int a = 0; a < 3; ++a) out.min[a] = std::min(out.min[a], c.min[a]), out.max[a] = std::max(out.max[a], c.max[a]);
        }
        return true;
    };
    auto hasFluid = [&](u16 id) {
        const BlockInfo& b = registry_.infos()[id];
        return b.fluid || b.waterlogged || registry_.name(id).starts_with("minecraft:bubble_column");
    };
    auto spawn = [&](double x0, double x1, double z0, double z1, double py) {
        add(drip, x0 + (x1 - x0) * double(randf()), py, z0 + (z1 - z0) * double(randf()), 0, 0, 0);
    };
    const u16 below = blockAt(x, y - 1, z);
    if (hasFluid(below)) return;
    Box s;
    if (!shape(below, s) || s.max[1] < 1.0f) {
        // isFaceSturdy(DOWN) of the fluid's own block (a waterlogged one): a full bottom face
        Box f;
        if (shape(fluidBlock, f) && f.min[1] <= 0.0f && f.min[0] <= 0.0f && f.min[2] <= 0.0f && f.max[0] >= 1.0f && f.max[2] >= 1.0f)
            spawn(x, x + 1, z, z + 1, y - 1 + 1 - 0.05);
        return;
    }
    const std::string& name = registry_.name(below);  // BlockTags.IMPERMEABLE: glass
    if ((name.find("glass") != std::string::npos && name.find("pane") == std::string::npos) || name == "minecraft:barrier") return;
    if (s.min[1] > 0.0f) {
        spawn(x + s.min[0], x + s.max[0], z + s.min[2], z + s.max[2], y - 1 + s.min[1] - 0.05);
        return;
    }
    const u16 under = blockAt(x, y - 2, z);
    Box u;
    if ((!shape(under, u) || u.max[1] < 1.0f) && !hasFluid(under))
        spawn(x + s.min[0], x + s.max[0], z + s.min[2], z + s.max[2], y - 1 - 0.05);
}

void Particles::weather(const double cam[3]) {
    const float rain = rainOverride_ >= 0 ? rainOverride_ : rain_;
    if (rain <= 0.0f) return;
    static int rainType = -2, smokeType = -2;
    if (rainType == -2) rainType = typeId("rain"), smokeType = typeId("smoke");
    const int cx = int(std::floor(cam[0])), cy = int(std::floor(cam[1])), cz = int(std::floor(cam[2]));
    const int radius = 10, diameter = 2 * radius + 1;
    const int count = int(0.225f * float(diameter * diameter) * rain * rain);
    for (int i = 0; i < count; ++i) {
        const int x = cx + int(rng_() % u32(diameter)) - radius, z = cz + int(rng_() % u32(diameter)) - radius;
        const u8 biome = world_.biomeAt(x, z);
        if (biome != World::kUnknownBiome) {
            const std::string_view b = World::biomeNameView(biome);
            if (b.find("desert") != b.npos || b.find("savanna") != b.npos || b.find("badlands") != b.npos ||
                b.find("snowy") != b.npos || b.find("frozen") != b.npos || b.find("ice") != b.npos ||
                b.find("grove") != b.npos || b.find("peaks") != b.npos || b.find("nether") != b.npos ||
                b.find("the_end") != b.npos)
                continue;
        }
        for (int y = cy + 10; y >= cy - 10; --y) {
            const u16 id = blockAt(x, y, z);
            if (!id) continue;
            const BlockInfo& b = registry_.infos()[id];
            if (!b.collisionCount && !b.fluid && !b.waterlogged) continue;
            const double bx = randf(), bz = randf();
            double top = (b.fluid || b.waterlogged) ? 0.875 : 0.0;
            for (u32 k = 0; k < b.collisionCount; ++k) {
                const Box& s = registry_.box(b.collisionFirst + k);
                if (bx >= s.min[0] && bx <= s.max[0] && bz >= s.min[2] && bz <= s.max[2]) top = std::max(top, double(s.max[1]));
            }
            const std::string& n = registry_.name(id);
            const bool hot = n.starts_with("minecraft:lava") || n == "minecraft:magma_block" ||
                             (n.find("campfire") != std::string::npos && n.find("lit=true") != std::string::npos);
            add(hot ? smokeType : rainType, x + bx, y + top, z + bz, 0, 0, 0);
            break;
        }
    }
}

// ClientExplosionTracker.tick: up to 512 debris / smoke particles per tick over this tick's explosions (picked by
// block count), each at a random point of the blast sphere that is air, flying outwards (ExplosionParticleInfo).
void Particles::tickExplosions() {
    if (trackedExplosions_.empty()) return;
    long long total = 0;
    for (const NetExplosion& e : trackedExplosions_) total += std::max(e.blockCount, 0);
    const int count = int(std::min<long long>(total, 512));
    for (int i = 0; i < count; ++i) {
        long long pick = std::uniform_int_distribution<long long>(0, total - 1)(rng_);
        const NetExplosion* ex = nullptr;
        for (const NetExplosion& e : trackedExplosions_)
            if ((pick -= std::max(e.blockCount, 0)) < 0) {
                ex = &e;
                break;
            }
        if (!ex) break;
        double dir[3] = {randf() * 2.0 - 1.0, randf() * 2.0 - 1.0, randf() * 2.0 - 1.0};
        const double len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len < 1.0e-5) dir[0] = dir[1] = dir[2] = 0;  // Vec3.normalize of a (near) zero vector
        else for (double& d : dir) d /= len;
        const float radius = std::cbrt(randf()) * ex->radius;
        const double local[3] = {dir[0] * radius, dir[1] * radius, dir[2] * radius};
        const double pos[3] = {ex->pos[0] + local[0], ex->pos[1] + local[1], ex->pos[2] + local[2]};
        const u16 at = blockAt(int(std::floor(pos[0])), int(std::floor(pos[1])), int(std::floor(pos[2])));
        if (registry_.infos()[at].kind != KindAir) continue;  // BlockState.isAir
        const float speed = 0.5f / (radius / ex->radius + 0.1f) * randf() * randf() + 0.3f;
        int weights = 0;
        for (const BlockParticleInfo& b : ex->blockParticles) weights += b.weight;
        int w = std::uniform_int_distribution<int>(0, std::max(weights - 1, 0))(rng_);
        const BlockParticleInfo* info = &ex->blockParticles.back();
        for (const BlockParticleInfo& b : ex->blockParticles)
            if ((w -= b.weight) < 0) {
                info = &b;
                break;
            }
        add(info->type, ex->pos[0] + local[0] * info->scaling, ex->pos[1] + local[1] * info->scaling,
            ex->pos[2] + local[2] * info->scaling, dir[0] * speed * info->speed, dir[1] * speed * info->speed,
            dir[2] * speed * info->speed, &info->opt);
    }
    trackedExplosions_.clear();
}

void Particles::tick() {
    ++ticks_;
    tickExplosions();
    // Weather from the server (ClientPacketListener.handleGameEvent).
    for (auto [type, value] : weatherQueue_) {
        if (type == 1) rainTarget_ = 1.0f, rain_ = 0.0f;  // start raining: the level ramps up via event 7
        else if (type == 2) rainTarget_ = 0.0f;
        else if (type == 7) rain_ = rainTarget_ = std::clamp(value, 0.0f, 1.0f);
    }
    weatherQueue_.clear();
    if (rain_ < rainTarget_) rain_ = std::min(rainTarget_, rain_ + 0.01f);
    if (rain_ > rainTarget_) rain_ = std::max(rainTarget_, rain_ - 0.01f);

    for (Particle& p : live_) {
        tickOne(p);
        if (!(p.flags & Removed) && (ticks_ + u64(&p - live_.data())) % 4 == 0) p.light = lightAt(p.x, p.y, p.z);
    }
    live_.erase(std::remove_if(live_.begin(), live_.end(), [](const Particle& p) { return (p.flags & Removed) != 0; }),
                live_.end());
    for (Starter& s : starters_) tickStarter(s);
    starters_.erase(std::remove_if(starters_.begin(), starters_.end(), [](const Starter& s) { return s.life < 0; }), starters_.end());
    ambient(cam_);
    weather(cam_);
    // CampfireBlockEntity.particleTick for campfires the sampler found (forgotten when broken or far away).
    static int cosy = -2, signal = -2;
    if (cosy == -2) cosy = typeId("campfire_cosy_smoke"), signal = typeId("campfire_signal_smoke");
    for (auto it = campfires_.begin(); it != campfires_.end();) {
        const u64 k = *it;
        const int x = int(i64(k) >> 38), y = int(i64(k << 52) >> 52), z = int(i64(k << 26) >> 38);
        const u16 id = blockAt(x, y, z);
        const std::string& n = registry_.name(id);
        const double dx = x - cam_[0], dz = z - cam_[2];
        if (n.find("campfire") == std::string::npos || n.find("lit=true") == std::string::npos || dx * dx + dz * dz > 64.0 * 64.0) {
            it = campfires_.erase(it);
            continue;
        }
        if (randf() < 0.11f) {
            const bool sig = n.find("signal_fire=true") != std::string::npos;
            const int n2 = int(rng_() % 2) + 2;
            for (int i = 0; i < n2; ++i)
                add(sig ? signal : cosy, x + 0.5 + randf() / 3.0 * (rng_() & 1 ? 1 : -1), y + randf() + randf(),
                    z + 0.5 + randf() / 3.0 * (rng_() & 1 ? 1 : -1), 0, 0.07, 0);
        }
        ++it;
    }
}

void Particles::frame(double dt, const double cam[3]) {
    Timer t;
    frameTicks_ = float(std::clamp(dt, 0.0, 0.1) * 20.0);
    std::copy(cam, cam + 3, cam_);
    std::vector<NetSpawn>& spawns = spawnsLocal_;
    std::vector<NetBreak>& breaks = breaksLocal_;
    spawns.clear(), breaks.clear();
    std::vector<NetExplosion> explosions;
    {
        std::lock_guard lock(netMutex_);
        spawns.swap(netSpawns_);
        breaks.swap(netBreaks_);
        weatherQueue_.insert(weatherQueue_.end(), netWeather_.begin(), netWeather_.end());
        netWeather_.clear();
        explosions.swap(netExplosions_);
    }
    for (const NetExplosion& e : explosions) {
        // level.playLocalSound(center, sound, BLOCKS, 4, (1 + (r - r) * 0.2) * 0.7); addParticle(particle, center, 1, 0, 0)
        if (e.playSound)
            if (audio::AudioSystem* a = audio::instance())
                a->playNetwork(e.soundId, e.soundName, audio::Category::Block, e.pos, 4.0f, (1.0f + (randf() - randf()) * 0.2f) * 0.7f);
        NetSpawn s{};
        s.type = e.particle, s.alwaysShow = true;
        s.x = e.pos[0], s.y = e.pos[1], s.z = e.pos[2];
        s.dx = 1, s.sx = 1, s.count = 0;
        spawns.push_back(s);
        if (!e.blockParticles.empty()) trackedExplosions_.push_back(e);  // ClientLevel.trackExplosionEffects
    }
    std::shared_lock lock(world_.mutex());
    for (CachedBlock& c : blockCache2_) c.y = INT32_MIN;  // the world may have changed since the last frame
    for (const NetBreak& b : breaks) {
        if (b.face >= 0) blockCrack(b.x, b.y, b.z, b.face);
        else blockDestroyed(b.x, b.y, b.z, localBlock(b.state));
    }
    for (const NetSpawn& s : spawns) {
        const double dx = s.x - cam[0], dy = s.y - cam[1], dz = s.z - cam[2];
        if (!s.alwaysShow && dx * dx + dy * dy + dz * dz > 1024.0) continue;
        if (s.count == 0) {
            add(s.type, s.x, s.y, s.z, double(s.sx) * s.dx, double(s.sy) * s.dy, double(s.sz) * s.dz, &s.opt);
        } else {
            for (int i = 0; i < std::min(s.count, 4096); ++i) {
                if (s.mode != 0) {
                    double xa = s.sx, ya = s.sy, za = s.sz;
                    if (s.mode == 2) xa *= randf(), ya *= randf(), za *= randf();
                    add(s.type, s.x + randf() * s.dx, s.y + randf() * s.dy, s.z + randf() * s.dz, xa, ya, za, &s.opt);
                } else {
                    add(s.type, s.x + gauss() * s.dx, s.y + gauss() * s.dy, s.z + gauss() * s.dz, gauss() * s.sx,
                        gauss() * s.sy, gauss() * s.sz, &s.opt);
                }
            }
        }
    }
    tickClock_ += std::min(dt, 0.25);
    int ticks = 0;
    while (tickClock_ >= 0.05 && ticks < 3) {
        tickClock_ -= 0.05;
        ++ticks;
        live_.insert(live_.end(), spawnQueue_.begin(), spawnQueue_.end());
        spawnQueue_.clear();
        tick();
    }
    if (tickClock_ >= 0.05) tickClock_ = 0;
    partial_ = float(tickClock_ / 0.05);
    live_.insert(live_.end(), spawnQueue_.begin(), spawnQueue_.end());
    spawnQueue_.clear();
    if (live_.size() > kMaxParticles) live_.erase(live_.begin(), live_.begin() + (live_.size() - kMaxParticles));
    stats_.live = u32(live_.size());
    stats_.tickMs = t.ms();
}

u32 Particles::build(GpuParticle* out, u32 max, const double cam[3], u32& opaque) {
    Timer t;
    const float pt = partial_;
    // Opaque from the front, translucent from the back of a CPU scratch buffer (the destination is write-combined
    // upload memory: never read back), then copied in order.
    if (scratch_.size() < max) scratch_.resize(max);
    GpuParticle* buf = scratch_.data();
    u32 n = 0, back = max;
    for (const Particle& p : live_) {
        if (p.flags & (NoRender | Removed)) continue;
        if (p.sprite < 0 || n >= back) continue;
        const Kind kind = types_[p.type].kind;
        if (kind == Kind::Shriek && p.e0 > 0) continue;  // still waiting for its delay
        if (kind == Kind::Spark && (int(p.e0) & kSparkTwinkle) && p.age >= p.lifetime / 3 && (p.age + p.lifetime) / 3 % 2 != 0)
            continue;  // twinkling: hidden every other three ticks
        const bool tr = (p.flags & Translucent) != 0;
        GpuParticle& g = tr ? buf[--back] : buf[n++];
        g.pos[0] = float(p.xo + (p.x - p.xo) * pt - cam[0]);
        g.pos[1] = float(p.yo + (p.y - p.yo) * pt - cam[1]);
        g.pos[2] = float(p.zo + (p.z - p.zo) * pt - cam[2]);
        float size = p.quadSize;
        const float s = (float(p.age) + pt) / float(std::max(p.lifetime, 1));
        switch (p.sizeMode) {
            case SizeGrow: size *= std::clamp(s * 32.0f, 0.0f, 1.0f); break;
            case SizeFlame: size *= 1.0f - s * s * 0.5f; break;
            case SizePortal: size *= 1.0f - (1.0f - s) * (1.0f - s); break;
            case SizeLava: size *= 1.0f - s * s; break;
            default: break;
        }
        float alpha = p.a;
        if (kind == Kind::Flash) {  // OverlayParticle: grows and fades
            const float t = float(p.age) + pt - 1.0f;
            size = 7.1f * std::sin(t * 0.25f * kPi), alpha = 0.6f - t * 0.25f * 0.5f;
        } else if (kind == Kind::Shriek) {
            const float t = (float(p.age) + pt) / float(p.lifetime);
            size *= std::clamp(t * 0.75f, 0.0f, 1.0f), alpha = 1.0f - std::clamp(t, 0.0f, 1.0f);
        }
        g.size = size;
        const ent::ItemModels::Rect& r = items_.rect(p.sprite);
        if (p.uo >= 0) {  // TerrainParticle: a quarter of the sprite, mirrored
            g.uv[0] = r.u0 + r.du * (p.uo + 1.0f) / 4.0f, g.uv[2] = r.u0 + r.du * p.uo / 4.0f;
            g.uv[1] = r.v0 + r.dv * p.vo / 4.0f, g.uv[3] = r.v0 + r.dv * (p.vo + 1.0f) / 4.0f;
        } else {
            g.uv[0] = r.u0, g.uv[1] = r.v0, g.uv[2] = r.u0 + r.du, g.uv[3] = r.v0 + r.dv;
        }
        g.color = abgrF(p.r, p.g, p.b, alpha);
        u32 light = p.light;
        if (kind == Kind::Firefly) {  // its own light only: block light by the fade
            light = u32(int(255.0f * fadeAmount(std::clamp((float(p.age) + pt) / float(p.lifetime), 0.0f, 1.0f), 0.1f, 0.3f)) >> 4);
        } else if (kind == Kind::Glow) {  // LightCoordsUtil.addSmoothBlockEmission over the lifetime
            const u32 add = u32(std::clamp((float(p.age) + pt) / float(p.lifetime), 0.0f, 1.0f) * 15.0f);
            light = (light & 0xF0) | std::min<u32>(15, (light & 15) + add);
        } else if (p.emissive == 255) light = 256;
        else if (p.emissive) light = (light & 0xF0) | std::max<u32>(light & 15, p.emissive);
        else if (types_[p.type].kind == Kind::Portal || types_[p.type].kind == Kind::Enchant) {
            float b = s * s;
            b *= b;
            light = (light & 0xF0) | std::max<u32>(light & 15, u32(b * 15.0f));
        }
        g.light = light;
        g.tex = ent::kTexItems;
        g.roll = p.oRoll + (p.roll - p.oRoll) * pt;
        // Moved since the last frame: the tick velocity over the frame's length (motion vectors).
        g.motion[0] = float(p.x - p.xo) * frameTicks_, g.motion[1] = float(p.y - p.yo) * frameTicks_;
        g.motion[2] = float(p.z - p.zo) * frameTicks_, g.orient = 0;
        if (kind == Kind::Shriek) {  // rotationX(-1.0472) and rotationYXZ(-pi, 1.0472, 0)
            g.roll = 0;
            g.orient = orientedQuad(0, -1.0472f, 0);
            if (n < back) {
                GpuParticle& g2 = buf[--back];
                g2 = g;
                g2.orient = orientedQuad(-kPi, 1.0472f, 0);
            }
        }
        if (kind == Kind::Vibration) {  // two quads along the path, swaying (VibrationSignalParticle.extract)
            const float sway = std::sin((float(p.age) + pt - 2.0f * kPi) * 0.05f) * 2.0f;
            const float rot = p.oRoll + (p.roll - p.oRoll) * pt, pitch = p.e2 + (p.e1 - p.e2) * pt + kPi / 2.0f;
            g.roll = 0;
            g.orient = orientedQuad(rot, -pitch, sway);
            if (n < back) {
                GpuParticle& g2 = buf[--back];
                g2 = g;
                g2.orient = orientedQuad(-kPi + rot, pitch, sway);
            }
        }
    }
    opaque = n;
    const u32 tcount = max - back;
    std::memcpy(out, buf, size_t(n) * sizeof(GpuParticle));
    std::memcpy(out + n, buf + back, size_t(tcount) * sizeof(GpuParticle));
    stats_.buildMs = t.ms();
    return n + tcount;
}

}  // namespace fx
