#include "fx/particles.h"

#include "audio/audio.h"

#include "assets/asset_source.h"
#include "assets/assets.h"
#include "assets/game_reports.h"
#include "entity/item_models.h"
#include "net/client.h"
#include "net/protocol.h"
#include "ui/resources.h"
#include "world/world.h"
#include "worldgen/rng.h"

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
    Totem, EndRod, Gust, GustSeed, Rising, SculkCharge, SculkChargePop, Shriek, Firefly, Glow, Trail, SulfurBubble,
    NoxiousGas, GasCloud, GeyserEruption, GeyserBase, GeyserPlume, Ash, SuspendedAir, ReversePortal, CurrentDown, BubbleUp,
    Marker, DragonBreath, Wake, Sweep, SquidInk, Vertical, DustPlume, TrialDetection, FlyStraight
};
enum class Opt : u8 { None, Block, Dust, DustTransition, Spell, Color, Float, VarInt, Int, IntFloat, Trail, Item, Vibration, Geyser, GeyserBase };
enum SizeMode : u8 { SizeConst, SizeGrow, SizeFlame, SizePortal, SizeLava, SizeReverse };
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
    {"wax_off", Kind::Glow}, {"electric_spark", Kind::Glow}, {"scrape", Kind::Glow}, {"trail", Kind::Trail},
    {"sulfur_bubbles", Kind::SulfurBubble}, {"noxious_gas", Kind::NoxiousGas}, {"noxious_gas_cloud", Kind::GasCloud},
    {"geyser", Kind::GeyserEruption}, {"geyser_base", Kind::GeyserBase}, {"geyser_poof", Kind::GeyserBase},
    {"geyser_plume", Kind::GeyserPlume}, {"sulfur_cube_goo", Kind::Item}, {"ash", Kind::Ash}, {"white_ash", Kind::Ash},
    {"underwater", Kind::SuspendedAir}, {"crimson_spore", Kind::SuspendedAir}, {"warped_spore", Kind::SuspendedAir},
    {"spore_blossom_air", Kind::SuspendedAir}, {"reverse_portal", Kind::ReversePortal}, {"current_down", Kind::CurrentDown},
    {"bubble_column_up", Kind::BubbleUp}, {"block_marker", Kind::Marker}, {"dragon_breath", Kind::DragonBreath},
    {"fishing", Kind::Wake}, {"spit", Kind::Poof}, {"sweep_attack", Kind::Sweep}, {"squid_ink", Kind::SquidInk},
    {"glow_squid_ink", Kind::SquidInk}, {"pause_mob_growth", Kind::Vertical}, {"reset_mob_growth", Kind::Vertical},
    {"dust_plume", Kind::DustPlume}, {"trial_spawner_detection", Kind::TrialDetection},
    {"trial_spawner_detection_ominous", Kind::TrialDetection}, {"vault_connection", Kind::Enchant},
    {"ominous_spawning", Kind::FlyStraight}};

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
    {"trail", Opt::Trail}, {"item", Opt::Item}, {"vibration", Opt::Vibration}, {"geyser", Opt::Geyser},
    {"geyser_plume", Opt::Geyser}, {"geyser_base", Opt::GeyserBase}, {"geyser_poof", Opt::GeyserBase}};

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
        const bool global = r.boolean();
        if (!r.ok) return;
        std::lock_guard lock(netMutex_);
        if (global) netLevelEvents_.push_back({-type, x, y, z, data});  // globalLevelEvent
        else if (type == 2001) netBreaks_.push_back({x, y, z, data});
        else if (type == 2019 || type == 2020) netBreaks_.push_back({x, y, z, 0, std::clamp(data, 0, 5)});  // Direction by ordinal
        else netLevelEvents_.push_back({type, x, y, z, data});
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
        if (!r.ok || (type != 1 && type != 2 && type != 7 && type != 8)) return;
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
        case Opt::Geyser: o.waterBlocks = r.i32v(); break;                          // GeyserParticleOptions
        case Opt::GeyserBase: o.waterBlocks = r.i32v(), o.power = r.f32(); break;  // GeyserBaseParticleOptions
        case Opt::Trail:  // TrailParticleOption: target, colour, duration
            o.target[0] = r.f64(), o.target[1] = r.f64(), o.target[2] = r.f64();
            o.color = 0xFF000000u | u32(r.i32v()), o.hasColor = true;
            o.arrival = r.varInt();
            break;
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
    if (t.name == "elder_guardian") {  // ElderGuardianParticle: the elder guardian's appearance (drawn by the entities)
        if (onElderGuardian) onElderGuardian();
        return nullptr;
    }
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
        } else if (t.name == "sulfur_cube_goo") {  // SulfurCubeProvider: the sprite set's (only) sprite, the plain random push
            const int rect = items_.texture("particle/sulfur_cube_goo");
            if (rect >= 0) shard(rect, x, y, z, 0, 0, 0, false);
        } else {  // SlimeProvider / SnowballProvider / CobwebProvider: a fixed item, the plain random push
            const char* tex = t.name == "item_slime" ? "item/slime_ball" : t.name == "item_snowball" ? "item/snowball" : "block/cobweb";
            const int rect = items_.texture(tex);
            if (rect >= 0) shard(rect, x, y, z, 0, 0, 0, false);
        }
        return nullptr;
    }
    if (t.sprites.empty() && t.kind != Kind::Terrain && t.kind != Kind::Crumble && t.kind != Kind::DustPillar &&
        t.kind != Kind::ExplosionSeed && t.kind != Kind::GustSeed && t.kind != Kind::GasCloud && t.kind != Kind::GeyserEruption &&
        t.kind != Kind::Marker) {
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
            if (t.name == "spit") p.gravity = 0.5f;  // SpitParticle
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
            if (t.name == "vault_connection") {  // VaultConnectionProvider: glowing, fading in (LifetimeAlpha 0 -> 0.6)
                p.emissive = 15, p.quadSize *= 1.5f, p.a = 0.0f, p.e0 = 1.0f;
                p.flags |= Translucent;
            }
            randomSprite();
            break;
        }
        case Kind::DragonBreath: {  // DragonBreathParticle (the power option scales the push)
            base(x, y, z);
            p.friction = 0.96f;
            p.xd = xa, p.yd = ya, p.zd = za;
            p.r = 0.7176471f + randf() * (0.8745098f - 0.7176471f), p.g = 0.0f, p.b = 0.8235294f + randf() * (0.9764706f - 0.8235294f);
            p.quadSize *= 0.75f;
            p.lifetime = int(20.0 / (randf() * 0.8 + 0.2));
            p.flags &= u8(~Physics);
            if (opt) {
                const float power = opt->scale;
                p.xd *= power, p.yd = (p.yd - 0.1f) * power + 0.1f, p.zd *= power;  // Particle.setPower
            }
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::Wake:  // WakeParticle (fishing): the given push, no gravity; the sprite cycles
            baseVel(x, y, z, 0, 0, 0);
            p.lifetime = int(8.0 / (randf() * 0.8 + 0.2));
            p.gravity = 0;
            p.xd = xa, p.yd = ya, p.zd = za;
            p.sprite = t.sprites.front();
            break;
        case Kind::Sweep:  // AttackSweepParticle
            baseVel(x, y, z, 0, 0, 0);
            p.xd = p.yd = p.zd = 0;
            p.lifetime = 4;
            p.r = p.g = p.b = randf() * 0.6f + 0.4f;
            p.quadSize = 1.0f - float(xa) * 0.5f;
            p.emissive = 255;
            p.flags &= u8(~Physics);
            ageSprite();
            break;
        case Kind::SquidInk: {  // SquidInkParticle (SimpleAnimatedParticle): fades over its second half, sinks in air
            base(x, y, z);
            p.friction = 0.92f, p.quadSize = 0.5f, p.gravity = 0;
            if (t.name == "glow_squid_ink") p.r = 0.2f, p.g = 0.8f, p.b = 0.6f;
            else p.r = p.g = p.b = 0.0f;
            p.lifetime = int(0.5f * 12.0f / (randf() * 0.8f + 0.2f));
            p.flags &= u8(~Physics);
            p.flags |= Translucent;
            p.xd = xa, p.yd = ya, p.zd = za;
            ageSprite();
            break;
        }
        case Kind::Vertical:  // SimpleVerticalParticle (pause / reset mob growth): drifting down / up
            baseVel(x, y, z, xa, ya, za);
            p.xd = xa, p.zd = za, p.yd = ya + (t.name == "reset_mob_growth" ? 0.03 : -0.03);
            p.gravity = 0;
            p.quadSize *= randf() * 0.6f + 0.5f;
            p.lifetime = 8;
            randomSprite();
            break;
        case Kind::DustPlume: {  // DustPlumeParticle (BaseAshSmokeParticle 0.7 / 0.6 / 0.7, gravity and friction decaying)
            baseVel(x, y, z, 0, 0, 0);
            p.friction = 0.96f, p.gravity = 0.5f;
            p.flags |= SpeedUp;
            p.flags &= u8(~Physics);
            p.xd = p.xd * 0.7f + xa, p.yd = p.yd * 0.6f + ya + 0.15f, p.zd = p.zd * 0.7f + za;
            const float shift = randf() * 0.2f;
            p.r = 0xBA / 255.0f - shift, p.g = 0xB1 / 255.0f - shift, p.b = 0xC2 / 255.0f - shift;
            p.quadSize *= 0.75f;
            p.lifetime = std::max(int(7.0 / (randf() * 0.8 + 0.2)), 1);
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::TrialDetection: {  // TrialSpawnerDetectionParticle (scale 1.5, glowing)
            baseVel(x, y, z, 0, 0, 0);
            p.friction = 0.96f, p.gravity = -0.1f;
            p.flags |= SpeedUp;
            p.xd = p.xd * 0.0 + xa, p.yd = p.yd * 0.9 + ya, p.zd = p.zd * 0.0 + za;
            p.quadSize *= 0.75f * 1.5f;
            p.lifetime = std::max(int(8.0f / (0.5f + randf() * 0.5f) * 1.5f), 1);
            p.emissive = 15;
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::FlyStraight: {  // FlyStraightTowardsParticle (ominous spawning): straight in, blue fading to white
            base(x, y, z);
            p.xd = xa, p.yd = ya, p.zd = za;
            p.sx = x, p.sy = y, p.sz = z;
            p.x = p.xo = x + xa, p.y = p.yo = y + ya, p.z = p.zo = z + za;
            p.quadSize = 0.1f * (randf() * 0.5f + 0.2f) * (3.0f + randf() * 2.0f);
            p.lifetime = int(randf() * 5.0f) + 25;
            p.flags &= u8(~Physics);
            p.emissive = 15;
            color(0x45B8FE);  // -12210434
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
        case Kind::Ash: {  // AshParticle / WhiteAshParticle (BaseAshSmokeParticle, direction 0.1 / -0.1 / 0.1)
            const bool white = t.name == "white_ash";
            double vx = 0, vy = 0, vz = 0;
            if (white) {  // WhiteAshParticle.Provider
                vx = randf() * -1.9 * randf() * 0.1, vy = randf() * -0.5 * randf() * 0.1 * 5.0, vz = randf() * -1.9 * randf() * 0.1;
            }
            baseVel(x, y, z, 0, 0, 0);
            p.friction = 0.96f, p.gravity = white ? 0.0125f : 0.1f;
            p.flags |= SpeedUp;
            p.flags &= u8(~Physics);
            p.xd = p.xd * 0.1f + vx, p.yd = p.yd * -0.1f + vy, p.zd = p.zd * 0.1f + vz;
            const float c = white ? 0.0f : randf() * 0.5f;
            p.r = p.g = p.b = c;
            if (white) color(0xBAB1C2);
            p.quadSize *= 0.75f;
            p.lifetime = std::max(int(20.0 / (randf() * 0.8 + 0.2)), 1);
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::SuspendedAir: {  // SuspendedParticle and its providers
            const bool still = t.name == "underwater";
            double vx = 0, vy = 0, vz = 0;
            if (t.name == "crimson_spore") vx = gauss() * 1.0e-6, vy = gauss() * 1.0e-4, vz = gauss() * 1.0e-6;
            else if (t.name == "warped_spore") vy = randf() * -1.9 * randf() * 0.1;
            else if (t.name == "spore_blossom_air") vy = -0.8f;
            if (still) base(x, y - 0.125, z);
            else baseVel(x, y - 0.125, z, vx, vy, vz);
            p.quadSize *= still ? randf() * 0.6f + 0.2f : randf() * 0.6f + 0.6f;
            p.lifetime = int(16.0 / (randf() * 0.8 + 0.2));
            p.flags &= u8(~Physics);
            p.friction = 1.0f, p.gravity = 0.0f;
            if (t.name == "underwater") p.r = 0.4f, p.g = 0.4f, p.b = 0.7f;
            else if (t.name == "crimson_spore") p.r = 0.9f, p.g = 0.4f, p.b = 0.5f;
            else if (t.name == "warped_spore") p.r = 0.1f, p.g = 0.1f, p.b = 0.3f;
            else p.r = 0.32f, p.g = 0.5f, p.b = 0.22f, p.lifetime = 500 + int(rng_() % 501), p.gravity = 0.01f;
            randomSprite();
            break;
        }
        case Kind::ReversePortal: {  // ReversePortalParticle (PortalParticle, bigger, 60 ticks, accelerating outwards)
            base(x, y, z);
            p.xd = xa, p.yd = ya, p.zd = za;
            p.quadSize = 0.1f * (randf() * 0.2f + 0.5f) * 1.5f;
            const float br = randf() * 0.6f + 0.4f;
            p.r = br * 0.9f, p.g = br * 0.3f, p.b = br;
            p.lifetime = int(randf() * 2.0f) + 60;
            p.flags &= u8(~Physics);
            p.sizeMode = SizeReverse;
            randomSprite();
            break;
        }
        case Kind::CurrentDown:  // WaterCurrentDownParticle
            base(x, y, z);
            p.lifetime = int(randf() * 60.0f) + 30;
            p.flags &= u8(~Physics);
            p.xd = 0, p.yd = -0.05, p.zd = 0;
            p.quadSize *= randf() * 0.6f + 0.2f;
            p.gravity = 0.002f;
            randomSprite();
            break;
        case Kind::BubbleUp:  // BubbleColumnUpParticle
            base(x, y, z);
            p.gravity = -0.125f, p.friction = 0.85f;
            p.quadSize *= randf() * 0.6f + 0.2f;
            p.xd = xa * 0.2f + (randf() * 2.0f - 1.0f) * 0.02f, p.yd = ya * 0.2f + (randf() * 2.0f - 1.0f) * 0.02f;
            p.zd = za * 0.2f + (randf() * 2.0f - 1.0f) * 0.02f;
            p.lifetime = int(40.0 / (randf() * 0.8 + 0.2));
            randomSprite();
            break;
        case Kind::Marker: {  // BlockMarker: the block's particle texture, half a block, 80 ticks
            const u16 block = opt ? localBlock(opt->block) : 0;
            if (!block) return nullptr;
            u32 tint;
            p.sprite = blockSprite(block, tint);
            if (p.sprite < 0) return nullptr;
            base(x, y, z);
            p.gravity = 0, p.lifetime = 80, p.quadSize = 0.5f;
            p.flags &= u8(~Physics);
            if (items_.rect(p.sprite).translucent) p.flags |= Translucent;
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
            if (t.name == "dolphin") p.r = 0.3f, p.g = 0.5f, p.b = 1.0f, p.lifetime /= 2;  // DolphinSpeedProvider (opaque: no alpha)
            if (t.name == "composter") p.lifetime = 3 + int(randf() * 5.0f);  // ComposterFillProvider
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
        case Kind::SulfurBubble: {  // SulfurBubbleParticle (the provider passes y speed as z speed)
            base(x, y, z);
            p.gravity = -0.04f, p.friction = 0.85f;
            p.xd = xa * 0.2f + (randf() * 2.0f - 1.0f) * 0.02f;
            p.zd = ya * 0.2f + (randf() * 2.0f - 1.0f) * 0.02f;
            p.e1 = 0.02f + 0.02f * randf();  // sizeStart
            p.quadSize = p.e1;
            p.lifetime = INT32_MAX;
            p.sy = y, p.e0 = 3.0f, p.sz = y;  // yStart, yEnd - yStart, yPrev
            randomSprite();
            break;
        }
        case Kind::NoxiousGas: {  // NoxiousGasParticle: BaseAshSmokeParticle(0.1, colour random 0.3, lifetime 5, gravity -0.02) x 3
            baseVel(x, y, z, 0, 0, 0);
            p.friction = 0.96f, p.gravity = -0.02f;
            p.flags |= SpeedUp | Translucent;
            p.xd = p.xd * 0.1f + xa, p.yd = p.yd * 0.1f + ya, p.zd = p.zd * 0.1f + za;
            (void)randf();  // the ash colour, made white below
            p.quadSize *= 0.75f * 3.0f;
            (void)randf();  // BaseAshSmokeParticle's lifetime, replaced below
            p.lifetime = std::max(int(6.0 / (randf() * 0.5 + 0.5) * 3.0), 1);
            p.e0 = float(p.lifetime) / 2.0f;  // fadeOutStartingPoint
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::GasCloud:  // NoxiousGasCloudParticle: no quad, a gas puff every other tick for 20 ticks
        case Kind::GeyserEruption:  // GeyserEruptionParticle: base, plume and poof particles for 20 ticks
            base(x, y, z);
            p.flags = NoRender;
            p.lifetime = 20;
            p.sx = xa, p.sy = ya, p.sz = za;
            p.e0 = float(opt ? opt->waterBlocks : 1);
            break;
        case Kind::GeyserBase: {  // GeyserBaseParticle (provider: a jitter of 0.25, 0.2 up)
            const int wb = opt ? opt->waterBlocks : 1;
            const float burstBase = opt ? opt->power : 1.5f;
            const double px = x + (randf() - 0.5f) * 0.5f, py = y + (randf() - 0.5f) * 0.5f + 0.2f, pz = z + (randf() - 0.5f) * 0.5f;
            const float burst = burstBase + 0.25f * float(wb), size = 3.0f + 0.125f * float(wb);
            baseVel(px, py, pz, 0, 0, 0);
            p.friction = 0.725f, p.gravity = 0;
            p.flags |= SpeedUp;
            p.xd = p.xd * burst + xa, p.yd = std::abs(p.yd * burst + ya), p.zd = p.zd * burst + za;
            (void)randf();  // the ash colour (white)
            p.quadSize *= 0.75f * size;
            p.lifetime = int(25.0f * (0.8f + 0.2f * randf()));
            p.sizeMode = SizeGrow;
            ageSprite();
            break;
        }
        case Kind::GeyserPlume: {  // GeyserPlumeParticle (provider: a jitter of 0.1, up to a block up)
            const int wb = opt ? opt->waterBlocks : 1;
            const double px = x + (randf() - 0.5f) * 0.2f, py = y + randf(), pz = z + (randf() - 0.5f) * 0.2f;
            baseVel(px, py, pz, xa, ya, za);
            const int plume = 5 * std::max(1, wb);
            p.flags |= SpeedUp;
            p.lifetime = plume * 5;
            p.yd = 0;
            p.sy = py, p.e0 = float(plume) - 1.0f;  // startY, maxY - startY
            p.e1 = (randf() - 0.5f) * 0.2f, p.e2 = (randf() - 0.5f) * 0.2f;  // horizontal spray
            p.friction = 1.0f;
            p.e3 = (wb == 1 ? 1.5f : 1.0f) * float(plume) * 1.45f;  // initialPropulsion
            p.gravity = -p.e3;
            const float size = p.quadSize * 0.75f;
            p.sx = size * (2.0f + float(plume) / 8.0f), p.sz = size * (3.0f + float(plume) / 8.0f);  // min / max size
            p.quadSize = float(p.sx);
            p.target = 0;  // done
            ageSprite();
            break;
        }
        case Kind::Trail: {  // TrailParticle: flies to its target over the duration, its colour varied a little, full bright
            if (!opt) return nullptr;
            base(x, y, z);
            p.flags &= u8(~Physics);
            p.quadSize = 0.26f;
            p.lifetime = opt->arrival;
            p.sx = opt->target[0], p.sy = opt->target[1], p.sz = opt->target[2];
            auto scaled = [&](int shift) {  // ARGB.scaleRGB by 0.875..1.125
                return float(std::clamp(int(float((opt->color >> shift) & 255) * (0.875f + randf() * 0.25f)), 0, 255)) / 255.0f;
            };
            p.r = scaled(16), p.g = scaled(8), p.b = scaled(0);
            p.emissive = 15;
            randomSprite();
            break;
        }
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

Particles::Particle* Particles::addNamed(std::string_view type, double x, double y, double z, double xa, double ya, double za,
                                         const Options* opt) {
    return add(typeId(type), x, y, z, xa, ya, za, opt);
}

int Particles::itemIdOf(std::string_view name) {
    auto it = itemIds_.find(std::string(name));
    if (it != itemIds_.end()) return it->second;
    int found = -1;
    for (int id = 0; id < 4096 && found < 0; ++id)
        if (items_.itemName(id) == name) found = id;
    return itemIds_[std::string(name)] = found;
}

void Particles::globalLevelEvent(int type, int x, int y, int z) {
    // 1023 wither spawned, 1028 dragon died, 1038 end portal opened: heard from two blocks towards it from the camera
    if (type != 1023 && type != 1028 && type != 1038) return;
    audio::AudioSystem* a = audio::instance();
    if (!a) return;
    double d[3] = {x + 0.5 - cam_[0], y + 0.5 - cam_[1], z + 0.5 - cam_[2]};
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (double& v : d) v = len > 1e-4 ? v / len : 0.0;
    const double pos[3] = {cam_[0] + d[0] * 2.0, cam_[1] + d[1] * 2.0, cam_[2] + d[2] * 2.0};
    const char* sound = type == 1023 ? "minecraft:entity.wither.spawn" : type == 1038 ? "minecraft:block.end_portal.spawn" : "minecraft:entity.ender_dragon.death";
    a->playNetwork(-1, sound, audio::Category::Hostile, pos, type == 1028 ? 5.0f : 1.0f, 1.0f);
}

void Particles::levelEvent(int type, int bx, int by, int bz, int data) {
    using audio::Category;
    const double center[3] = {bx + 0.5, by + 0.5, bz + 0.5};
    auto sound = [&](const char* name, Category cat, float volume, float pitch) {  // level.playLocalSound(pos, ...)
        if (audio::AudioSystem* a = audio::instance()) a->playNetwork(-1, name, cat, center, volume, pitch);
    };
    auto rnd = [&] { return double(randf()); };
    auto variedPitch = [&] { return (randf() - randf()) * 0.2f + 1.0f; };
    auto smallPitch = [&] { return randf() * 0.1f + 0.9f; };
    auto blockName = [&](int x, int y, int z) { return registry_.name(blockAt(x, y, z)); };
    // ParticleUtils.spawnParticleOnFace / spawnParticlesOnBlockFaces
    static const int kDirs[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    auto onFace = [&](int face, std::string_view particle, const double speed[3], double step, const Options* opt = nullptr) {
        const int* s = kDirs[face];
        const double x = center[0] + (s[0] == 0 ? rnd() - 0.5 : s[0] * step);
        const double y = center[1] + (s[1] == 0 ? rnd() - 0.5 : s[1] * step);
        const double z = center[2] + (s[2] == 0 ? rnd() - 0.5 : s[2] * step);
        addNamed(particle, x, y, z, s[0] == 0 ? speed[0] : 0.0, s[1] == 0 ? speed[1] : 0.0, s[2] == 0 ? speed[2] : 0.0, opt);
    };
    auto onFaces = [&](std::string_view particle, int lo, int hi) {
        for (int face = 0; face < 6; ++face)
            for (int i = 0, n = lo + int(randf() * float(hi - lo + 1)); i < n; ++i) {
                const double speed[3] = {rnd() - 0.5, rnd() - 0.5, rnd() - 0.5};
                onFace(face, particle, speed, 0.55);
            }
    };
    // ParticleUtils.spawnParticles (spread around the block, optionally only above ground)
    auto spread = [&](int x, int y, int z, int count, double width, double height, bool floating, std::string_view particle) {
        for (int i = 0; i < count; ++i) {
            const double vx = gauss() * 0.02, vy = gauss() * 0.02, vz = gauss() * 0.02;
            const double px = x + 0.5 - width + rnd() * width * 2.0, py = y + rnd() * height, pz = z + 0.5 - width + rnd() * width * 2.0;
            const u16 below = blockAt(int(std::floor(px)), int(std::floor(py)) - 1, int(std::floor(pz)));
            if (floating || below != 0) addNamed(particle, px, py, pz, vx, vy, vz);
        }
    };
    auto shapeTop = [&](int x, int y, int z) {  // the block's shape height (air: a whole block)
        const u16 id = blockAt(x, y, z);
        if (id == 0) return 1.0;
        const BlockInfo& b = registry_.infos()[id];
        float top = 0;
        for (u32 i = 0; i < b.selectionCount; ++i) top = std::max(top, registry_.box(b.selectionFirst + i).max[1]);
        return b.selectionCount ? double(top) : 1.0;
    };
    auto inBlock = [&](int x, int y, int z, int count, std::string_view particle) {  // ParticleUtils.spawnParticleInBlock
        spread(x, y, z, count, 0.5, shapeTop(x, y, z), true, particle);
    };
    auto shoot = [&](std::string_view particle) {  // shootParticles: out of the facing side (Direction 3D data)
        const int* n = kDirs[std::clamp(data, 0, 5)];
        for (int i = 0; i < 10; ++i) {
            const double pow = rnd() * 0.2 + 0.01;
            const double x = bx + n[0] * 0.6 + 0.5 + n[0] * 0.01 + (rnd() - 0.5) * n[2] * 0.5;
            const double y = by + n[1] * 0.6 + 0.5 + n[1] * 0.01 + (rnd() - 0.5) * n[1] * 0.5;
            const double z = bz + n[2] * 0.6 + 0.5 + n[2] * 0.01 + (rnd() - 0.5) * n[0] * 0.5;
            addNamed(particle, x, y, z, n[0] * pow + gauss() * 0.01, n[1] * pow + gauss() * 0.01, n[2] * pow + gauss() * 0.01);
        }
    };
    auto potionSplash = [&](std::string_view particle) {  // potionSplashParticles
        const int bottle = itemIdOf("splash_potion");
        for (int i = 0; i < 8; ++i)
            if (bottle >= 0) itemShard(bottle, bx + 0.5, by, bz + 0.5, gauss() * 0.15, rnd() * 0.2, gauss() * 0.15);
        const float red = float((data >> 16) & 255) / 255.0f, green = float((data >> 8) & 255) / 255.0f, blue = float(data & 255) / 255.0f;
        for (int i = 0; i < 100; ++i) {
            const double dist = rnd() * 4.0, angle = rnd() * 3.14159265358979 * 2.0;
            const double vx = std::cos(angle) * dist, vy = 0.01 + rnd() * 0.5, vz = std::sin(angle) * dist;
            const float bright = 0.75f + randf() * 0.25f;
            Options o;
            o.color = 0xFF000000u | u32(red * bright * 255.0f) << 16 | u32(green * bright * 255.0f) << 8 | u32(blue * bright * 255.0f);
            o.hasColor = true, o.power = float(dist);
            addNamed(particle, bx + 0.5 + vx * 0.1, by + 0.3, bz + 0.5 + vz * 0.1, vx, vy, vz, &o);
        }
    };
    auto teleport = [&](int tx, int ty, int tz) {  // singleBlockTeleportationParticles
        for (int i = 0; i < 128; ++i) {
            const double t = rnd();
            const float vx = (randf() - 0.5f) * 0.2f, vy = (randf() - 0.5f) * 0.2f, vz = (randf() - 0.5f) * 0.2f;
            addNamed("portal", tx + (bx - tx) * t + (rnd() - 0.5) + 0.5, ty + (by - ty) * t + rnd() - 0.5, tz + (bz - tz) * t + (rnd() - 0.5) + 0.5, vx, vy, vz);
        }
    };
    auto unpackDiff = [&](int xs, int ys, int zs, int& tx, int& ty, int& tz) {  // BlockUtil.unpackDifferenceInPosition
        const int bitsX = int(std::ceil(std::log2(double(xs * 2 + 1)))), bitsY = int(std::ceil(std::log2(double(ys * 2 + 1))));
        const int bitsZ = int(std::ceil(std::log2(double(zs * 2 + 1))));
        (void)bitsZ;
        const int dx = (data & ((1 << bitsX) - 1)) - xs;
        const int dy = ((data >> bitsX) & ((1 << bitsY) - 1)) - ys;
        const int dz = (data >> (bitsX + bitsY)) - zs;
        tx = bx + dx, ty = by + dy, tz = bz + dz;
    };
    auto trialSpawn = [&](const char* flame) {  // TrialSpawner.addSpawnParticles
        for (int i = 0; i < 20; ++i) {
            const double x = bx + 0.5 + (rnd() - 0.5) * 2.0, y = by + 0.5 + (rnd() - 0.5) * 2.0, z = bz + 0.5 + (rnd() - 0.5) * 2.0;
            addNamed("smoke", x, y, z, 0, 0, 0);
            addNamed(flame, x, y, z, 0, 0, 0);
        }
    };
    auto detectPlayer = [&](int n, const char* particle) {  // TrialSpawner.addDetectPlayerParticles
        for (int i = 0; i < 30 + std::min(n, 10) * 5; ++i)
            addNamed(particle, bx + 0.5 + (2.0 * rnd() - 1.0) * 0.65, by + 0.1 + rnd() * 0.8, bz + 0.5 + (2.0 * rnd() - 1.0) * 0.65, 0, 0, 0);
    };
    auto ejectItem = [&] {  // TrialSpawner.addEjectItemParticles
        for (int i = 0; i < 20; ++i) {
            const double x = bx + 0.4 + rnd() * 0.2, y = by + 0.4 + rnd() * 0.2, z = bz + 0.4 + rnd() * 0.2;
            const double xa = gauss() * 0.02, ya = gauss() * 0.02, za = gauss() * 0.02;
            addNamed("small_flame", x, y, z, xa, ya, za * 0.25);
            addNamed("smoke", x, y, z, xa, ya, za);
        }
    };
    auto flameOf = [&](int d) { return d == 1 ? "soul_fire_flame" : "flame"; };  // TrialSpawner.FlameParticle.decode
    switch (type) {
        case 1000: sound("minecraft:block.dispenser.dispense", Category::Block, 1.0f, 1.0f); break;
        case 1001: sound("minecraft:block.dispenser.fail", Category::Block, 1.0f, 1.2f); break;
        case 1002: sound("minecraft:block.dispenser.launch", Category::Block, 1.0f, 1.2f); break;
        case 1004: sound("minecraft:entity.firework_rocket.shoot", Category::Neutral, 1.0f, 1.2f); break;
        case 1009:
            if (data == 0) sound("minecraft:block.fire.extinguish", Category::Block, 0.5f, 2.6f + (randf() - randf()) * 0.8f);
            else if (data == 1) sound("minecraft:block.generic.extinguish_fire", Category::Block, 0.7f, 1.6f + (randf() - randf()) * 0.4f);
            break;
        case 1015: sound("minecraft:entity.ghast.warn", Category::Hostile, 10.0f, variedPitch()); break;
        case 1016: sound("minecraft:entity.ghast.shoot", Category::Hostile, 10.0f, variedPitch()); break;
        case 1017: sound("minecraft:entity.ender_dragon.shoot", Category::Hostile, 10.0f, variedPitch()); break;
        case 1018: sound("minecraft:entity.blaze.shoot", Category::Hostile, 2.0f, variedPitch()); break;
        case 1019: sound("minecraft:entity.zombie.attack_wooden_door", Category::Hostile, 2.0f, variedPitch()); break;
        case 1020: sound("minecraft:entity.zombie.attack_iron_door", Category::Hostile, 2.0f, variedPitch()); break;
        case 1021: sound("minecraft:entity.zombie.break_wooden_door", Category::Hostile, 2.0f, variedPitch()); break;
        case 1022: sound("minecraft:entity.wither.break_block", Category::Hostile, 2.0f, variedPitch()); break;
        case 1024: sound("minecraft:entity.wither.shoot", Category::Hostile, 2.0f, variedPitch()); break;
        case 1025: sound("minecraft:entity.bat.takeoff", Category::Neutral, 0.05f, variedPitch()); break;
        case 1026: sound("minecraft:entity.zombie.infect", Category::Hostile, 2.0f, variedPitch()); break;
        case 1027: sound("minecraft:entity.zombie_villager.converted", Category::Hostile, 2.0f, variedPitch()); break;
        case 1029: sound("minecraft:block.anvil.destroy", Category::Block, 1.0f, smallPitch()); break;
        case 1030: sound("minecraft:block.anvil.use", Category::Block, 1.0f, smallPitch()); break;
        case 1031: sound("minecraft:block.anvil.land", Category::Block, 0.3f, smallPitch()); break;
        case 1032:  // SimpleSoundInstance.forLocalAmbience: at the listener
            if (audio::AudioSystem* a = audio::instance()) a->playUi("minecraft:block.portal.travel", randf() * 0.4f + 0.8f, 0.25f);
            break;
        case 1033: sound("minecraft:block.chorus_flower.grow", Category::Block, 1.0f, 1.0f); break;
        case 1034: sound("minecraft:block.chorus_flower.death", Category::Block, 1.0f, 1.0f); break;
        case 1035: sound("minecraft:block.brewing_stand.brew", Category::Block, 1.0f, 1.0f); break;
        case 1039: sound("minecraft:entity.phantom.bite", Category::Hostile, 0.3f, smallPitch()); break;
        case 1040: sound("minecraft:entity.zombie.converted_to_drowned", Category::Hostile, 2.0f, variedPitch()); break;
        case 1041: sound("minecraft:entity.husk.converted_to_zombie", Category::Hostile, 2.0f, variedPitch()); break;
        case 1042: sound("minecraft:block.grindstone.use", Category::Block, 1.0f, smallPitch()); break;
        case 1043: sound("minecraft:item.book.page_turn", Category::Block, 1.0f, smallPitch()); break;
        case 1044: sound("minecraft:block.smithing_table.use", Category::Block, 1.0f, smallPitch()); break;
        case 1045: sound("minecraft:block.pointed_dripstone.land", Category::Block, 2.0f, smallPitch()); break;
        case 1046: sound("minecraft:block.pointed_dripstone.drip_lava_into_cauldron", Category::Block, 2.0f, smallPitch()); break;
        case 1047: sound("minecraft:block.pointed_dripstone.drip_water_into_cauldron", Category::Block, 2.0f, smallPitch()); break;
        case 1048: sound("minecraft:entity.skeleton.converted_to_stray", Category::Hostile, 2.0f, variedPitch()); break;
        case 1049: sound("minecraft:block.crafter.craft", Category::Block, 1.0f, 1.0f); break;
        case 1050: sound("minecraft:block.crafter.fail", Category::Block, 1.0f, 1.0f); break;
        case 1051: sound("minecraft:entity.wind_charge.throw", Category::Block, 0.5f, 0.4f / (randf() * 0.4f + 0.8f)); break;
        case 1052: sound("minecraft:block.sulfur_spike.land", Category::Block, 2.0f, smallPitch()); break;
        case 1053:
        case 1054: sound("minecraft:entity.splash_potion.break", Category::Neutral, 1.0f, smallPitch()); break;
        case 1500: {  // ComposterBlock.handleFill
            sound(data > 0 ? "minecraft:block.composter.fill_success" : "minecraft:block.composter.fill", Category::Block, 1.0f, 1.0f);
            const double h = shapeTop(bx, by, bz) + 0.03125;  // the shape's height at the middle
            for (int i = 0; i < 10; ++i)
                addNamed("composter", bx + 0.1875 + 0.625 * randf(), by + h + randf() * (1.0 - h), bz + 0.1875 + 0.625 * randf(), gauss() * 0.02,
                         gauss() * 0.02, gauss() * 0.02);
            break;
        }
        case 1501:
            sound("minecraft:block.lava.extinguish", Category::Block, 0.5f, 2.6f + (randf() - randf()) * 0.8f);
            for (int i = 0; i < 8; ++i) addNamed("large_smoke", bx + rnd(), by + 1.2, bz + rnd(), 0, 0, 0);
            break;
        case 1502:
            sound("minecraft:block.redstone_torch.burnout", Category::Block, 0.5f, 2.6f + (randf() - randf()) * 0.8f);
            for (int i = 0; i < 5; ++i) addNamed("smoke", bx + rnd() * 0.6 + 0.2, by + rnd() * 0.6 + 0.2, bz + rnd() * 0.6 + 0.2, 0, 0, 0);
            break;
        case 1503:
            sound("minecraft:block.end_portal_frame.fill", Category::Block, 1.0f, 1.0f);
            for (int i = 0; i < 16; ++i) addNamed("smoke", bx + (5.0 + rnd() * 6.0) / 16.0, by + 0.8125, bz + (5.0 + rnd() * 6.0) / 16.0, 0, 0, 0);
            break;
        case 1504: {  // PointedDripstoneBlock.spawnDripParticle: the fluid above the stalactite's root
            int y = by;
            while (y < by + 12 && blockName(bx, y + 1, bz).starts_with("minecraft:pointed_dripstone")) ++y;
            const std::string root = blockName(bx, y, bz), above = blockName(bx, y + 1, bz);
            const bool lava = above.starts_with("minecraft:lava");
            const bool water = above.starts_with("minecraft:water") || root.find("waterlogged=true") != std::string::npos;
            if (lava || water) addNamed(lava ? "dripping_dripstone_lava" : "dripping_dripstone_water", bx + 0.5, by + 0.625, bz + 0.5, 0, 0, 0);
            break;
        }
        case 1505: {  // BoneMealItem.addGrowthParticles
            const std::string n = blockName(bx, by, bz);
            const bool spreader = n == "minecraft:grass_block" || n == "minecraft:netherrack" || n.ends_with("_nylium") || n == "minecraft:moss_block" ||
                                  n == "minecraft:pale_moss_block";
            if (spreader) spread(bx, by + 1, bz, data * 3, 3.0, 1.0, false, "happy_villager");
            else if (n.starts_with("minecraft:water")) spread(bx, by, bz, data * 3, 3.0, 1.0, false, "happy_villager");
            else if (n != "minecraft:air") inBlock(bx, n == "minecraft:rooted_dirt" ? by - 1 : by, bz, data, "happy_villager");
            sound("minecraft:item.bone_meal.use", Category::Block, 1.0f, 1.0f);
            break;
        }
        case 2000: shoot("smoke"); break;
        case 2002: potionSplash("effect"); break;
        case 2003: {  // an eye of ender shatters
            const int eye = itemIdOf("ender_eye");
            for (int i = 0; i < 8; ++i)
                if (eye >= 0) itemShard(eye, bx + 0.5, by, bz + 0.5, gauss() * 0.15, rnd() * 0.2, gauss() * 0.15);
            for (double a = 0.0; a < 3.14159265358979 * 2; a += 3.14159265358979 / 20) {
                addNamed("portal", bx + 0.5 + std::cos(a) * 5.0, by - 0.4, bz + 0.5 + std::sin(a) * 5.0, std::cos(a) * -5.0, 0, std::sin(a) * -5.0);
                addNamed("portal", bx + 0.5 + std::cos(a) * 5.0, by - 0.4, bz + 0.5 + std::sin(a) * 5.0, std::cos(a) * -7.0, 0, std::sin(a) * -7.0);
            }
            break;
        }
        case 2004:
            for (int i = 0; i < 20; ++i) {
                const double x = bx + 0.5 + (rnd() - 0.5) * 2.0, y = by + 0.5 + (rnd() - 0.5) * 2.0, z = bz + 0.5 + (rnd() - 0.5) * 2.0;
                addNamed("smoke", x, y, z, 0, 0, 0);
                addNamed("flame", x, y, z, 0, 0, 0);
            }
            break;
        case 2006:
            for (int i = 0; i < 200; ++i) {
                const float dist = randf() * 4.0f, angle = randf() * 3.14159265f * 2.0f;
                const double vx = std::cos(angle) * dist, vy = 0.01 + rnd() * 0.5, vz = std::sin(angle) * dist;
                Options o;
                o.power = dist;  // PowerParticleOption
                addNamed("dragon_breath", bx + vx * 0.1, by + 0.3, bz + vz * 0.1, vx, vy, vz, &o);
            }
            if (data == 1) sound("minecraft:entity.dragon_fireball.explode", Category::Hostile, 1.0f, smallPitch());
            break;
        case 2007: potionSplash("instant_effect"); break;
        case 2008: addNamed("explosion", bx + 0.5, by + 0.5, bz + 0.5, 0, 0, 0); break;
        case 2009:
            for (int i = 0; i < 8; ++i) addNamed("cloud", bx + rnd(), by + 1.2, bz + rnd(), 0, 0, 0);
            break;
        case 2010: shoot("white_smoke"); break;
        case 2011:
        case 2012: inBlock(bx, by, bz, data, "happy_villager"); break;
        case 2013: {  // ParticleUtils.spawnSmashAttackParticles: dust pillars of the block hit
            Options o;
            o.block = 0;
            const u16 hit = blockAt(bx, by, bz);
            for (size_t i = 0; i < blockStates_.size(); ++i)
                if (localBlock(int(i)) == hit) {
                    o.block = int(i);
                    break;
                }
            const double cx = bx + 0.5, cy = by + 1.0, cz = bz + 0.5;
            for (int i = 0; i < float(data) / 3.0f; ++i)
                addNamed("dust_pillar", cx + gauss() / 2.0, cy, cz + gauss() / 2.0, gauss() * 0.2, gauss() * 0.2, gauss() * 0.2, &o);
            for (int i = 0; i < float(data) / 1.5f; ++i)
                addNamed("dust_pillar", cx + 3.5 * std::cos(i) + gauss() / 2.0, cy, cz + 3.5 * std::sin(i) + gauss() / 2.0, gauss() * 0.05, gauss() * 0.05,
                         gauss() * 0.05, &o);
            break;
        }
        case 2014: blockDestroyed(bx, by, bz, localBlock(data)); break;
        case 2015:
        case 2016:
        case 2017: {
            int tx, ty, tz;
            if (type == 2015) unpackDiff(16, 8, 16, tx, ty, tz);
            else if (type == 2016) unpackDiff(8, 8, 8, tx, ty, tz);
            else unpackDiff(127, 127, 127, tx, ty, tz);
            teleport(tx, ty, tz);
            break;
        }
        case 2018: {  // an enderman's teleport trail
            int tx, ty, tz;
            unpackDiff(127, 127, 127, tx, ty, tz);
            for (int i = 0; i < 128; ++i) {
                const double t = rnd();
                const float vx = (randf() - 0.5f) * 0.2f, vy = (randf() - 0.5f) * 0.2f, vz = (randf() - 0.5f) * 0.2f;
                addNamed("portal", tx + (bx - tx) * t + (rnd() - 0.5) * 0.6 * 2.0, ty + (by - ty) * t + rnd() * 2.9, tz + (bz - tz) * t + (rnd() - 0.5) * 0.6 * 2.0,
                         vx, vy, vz);
            }
            break;
        }
        case 3000:
            addNamed("explosion_emitter", bx + 0.5, by + 0.5, bz + 0.5, 0, 0, 0);
            sound("minecraft:block.end_gateway.spawn", Category::Block, 10.0f, (1.0f + (randf() - randf()) * 0.2f) * 0.7f);
            break;
        case 3001: sound("minecraft:entity.ender_dragon.growl", Category::Hostile, 64.0f, 0.8f + randf() * 0.3f); break;
        case 3002:
            if (data >= 0 && data < 3) {  // spawnParticlesAlongAxis(axis, 0.125, electric_spark, 10..19)
                for (int i = 0, n = 10 + int(randf() * 10.0f); i < n; ++i) {
                    const double x = center[0] + (rnd() * 2.0 - 1.0) * (data == 0 ? 0.5 : 0.125);
                    const double y = center[1] + (rnd() * 2.0 - 1.0) * (data == 1 ? 0.5 : 0.125);
                    const double z = center[2] + (rnd() * 2.0 - 1.0) * (data == 2 ? 0.5 : 0.125);
                    addNamed("electric_spark", x, y, z, data == 0 ? rnd() * 2.0 - 1.0 : 0.0, data == 1 ? rnd() * 2.0 - 1.0 : 0.0,
                             data == 2 ? rnd() * 2.0 - 1.0 : 0.0);
                }
            } else {
                onFaces("electric_spark", 3, 5);
            }
            break;
        case 3003: onFaces("wax_on", 3, 5); break;
        case 3004: onFaces("wax_off", 3, 5); break;
        case 3005: onFaces("scrape", 3, 5); break;
        case 3006: {  // sculk charge
            const int count = data >> 6;
            if (count > 0) {
                if (randf() < 0.3f + float(count) * 0.1f)
                    sound("minecraft:block.sculk.charge", Category::Block, 0.15f + 0.02f * float(count * count) * randf(), 0.4f + 0.3f * float(count) * randf());
                const int faces = data & 63;
                for (int face = 0; face < 6; ++face) {
                    if (faces != 0 && !((faces >> face) & 1)) continue;  // MultifaceBlock.unpack
                    Options o;
                    o.scale = faces == 0 ? (face == 0 ? 3.14159265f : 0.0f) : (face == 1 ? 3.14159265f : 0.0f);  // the roll
                    const double step = faces == 0 ? (face < 2 ? 0.65 : 0.57) : 0.35;
                    for (int i = 0, n = int(randf() * float(count + 1)); i < n; ++i) {
                        const double speed[3] = {(rnd() * 2.0 - 1.0) * 0.005, (rnd() * 2.0 - 1.0) * 0.005, (rnd() * 2.0 - 1.0) * 0.005};
                        onFace(face, "sculk_charge", speed, step, &o);
                    }
                }
            } else {
                sound("minecraft:block.sculk.charge", Category::Block, 1.0f, 1.0f);
                const BlockInfo& b = registry_.infos()[blockAt(bx, by, bz)];
                const bool solid = b.kind == KindCube && b.collisionCount;
                const int n = solid ? 40 : 20;
                const float spreadR = solid ? 0.45f : 0.25f;
                for (int i = 0; i < n; ++i) {
                    const float vx = 2.0f * randf() - 1.0f, vy = 2.0f * randf() - 1.0f, vz = 2.0f * randf() - 1.0f;
                    addNamed("sculk_charge_pop", center[0] + vx * spreadR, center[1] + vy * spreadR, center[2] + vz * spreadR, vx * 0.07f, vy * 0.07f, vz * 0.07f);
                }
            }
            break;
        }
        case 3007: {  // a sculk shrieker shrieks
            for (int i = 0; i < 10; ++i) {
                Options o;
                o.delay = i * 5;
                addNamed("shriek", bx + 0.5, by + 0.5, bz + 0.5, 0, 0, 0, &o);
            }
            if (blockName(bx, by, bz).find("waterlogged=true") == std::string::npos)
                if (audio::AudioSystem* a = audio::instance()) {
                    const double pos[3] = {bx + 0.5, by + 0.5, bz + 0.5};
                    a->playNetwork(-1, "minecraft:block.sculk_shrieker.shriek", Category::Block, pos, 2.0f, 0.6f + randf() * 0.4f);
                }
            break;
        }
        case 3008: {  // brushing finished
            const u16 state = localBlock(data);
            const std::string n = registry_.name(state);
            if (n.starts_with("minecraft:suspicious_sand")) sound("minecraft:item.brush.brushing.sand.complete", Category::Player, 1.0f, 1.0f);
            else if (n.starts_with("minecraft:suspicious_gravel")) sound("minecraft:item.brush.brushing.gravel.complete", Category::Player, 1.0f, 1.0f);
            blockDestroyed(bx, by, bz, state);
            break;
        }
        case 3009: onFaces("egg_crack", 3, 6); break;
        case 3011: trialSpawn(flameOf(data)); break;
        case 3012:
            sound("minecraft:block.trial_spawner.spawn_mob", Category::Block, 1.0f, variedPitch());
            trialSpawn(flameOf(data));
            break;
        case 3013:
            sound("minecraft:block.trial_spawner.detect_player", Category::Block, 1.0f, variedPitch());
            detectPlayer(data, "trial_spawner_detection");
            break;
        case 3014:
            sound("minecraft:block.trial_spawner.eject_item", Category::Block, 1.0f, variedPitch());
            ejectItem();
            break;
        case 3015:  // VaultBlockEntity.Client.emitActivationParticles
            for (int i = 0; i < 20; ++i) {
                const double x = bx + 0.1 + rnd() * 0.8, y = by + 0.25 + rnd() * 0.5, z = bz + 0.1 + rnd() * 0.8;
                addNamed("smoke", x, y, z, 0, 0, 0);
                addNamed(data == 0 ? "small_flame" : "soul_fire_flame", x, y, z, 0, 0, 0);
            }
            sound("minecraft:block.vault.activate", Category::Block, 1.0f, variedPitch());
            break;
        case 3016:  // emitDeactivationParticles
            for (int i = 0; i < 20; ++i)
                addNamed(data == 0 ? "small_flame" : "soul_fire_flame", bx + 0.4 + rnd() * 0.2, by + 0.4 + rnd() * 0.2, bz + 0.4 + rnd() * 0.2, gauss() * 0.02,
                         gauss() * 0.02, gauss() * 0.02);
            sound("minecraft:block.vault.deactivate", Category::Block, 1.0f, variedPitch());
            break;
        case 3017: ejectItem(); break;
        case 3018:
            for (int i = 0; i < 10; ++i) addNamed("poof", bx + rnd(), by + rnd(), bz + rnd(), gauss() * 0.02, gauss() * 0.02, gauss() * 0.02);
            sound("minecraft:block.cobweb.place", Category::Block, 1.0f, variedPitch());
            break;
        case 3019:
            sound("minecraft:block.trial_spawner.detect_player", Category::Block, 1.0f, variedPitch());
            detectPlayer(data, "trial_spawner_detection_ominous");
            break;
        case 3020:
            sound("minecraft:block.trial_spawner.ominous_activate", Category::Block, data == 0 ? 0.3f : 1.0f, variedPitch());
            detectPlayer(0, "trial_spawner_detection_ominous");
            for (int i = 0; i < 20; ++i) {  // addBecomeOminousParticles
                const double x = bx + 0.5 + (rnd() - 0.5) * 2.0, y = by + 0.5 + (rnd() - 0.5) * 2.0, z = bz + 0.5 + (rnd() - 0.5) * 2.0;
                const double xa = gauss() * 0.02, ya = gauss() * 0.02, za = gauss() * 0.02;
                addNamed("trial_omen", x, y, z, xa, ya, za);
                addNamed("soul_fire_flame", x, y, z, xa, ya, za);
            }
            break;
        case 3021:
            sound("minecraft:block.trial_spawner.spawn_item", Category::Block, 1.0f, variedPitch());
            trialSpawn(flameOf(data));
            break;
        default: break;
    }
}

void Particles::spawn(std::string_view type, double x, double y, double z, double vx, double vy, double vz) {
    add(typeId(type), x, y, z, vx, vy, vz);
}

void Particles::spawnBlock(std::string_view type, std::string_view stateKey, double x, double y, double z, double vx, double vy, double vz) {
    Options o;
    o.block = reports_.stateId(reports_.complete(stateKey));
    if (o.block > 0) add(typeId(type), x, y, z, vx, vy, vz, &o);
}

void Particles::sprintDust(double x, double y, double z, double width, const double vel[3]) {
    const int bx = int(std::floor(x)), by = int(std::floor(y - 0.2)), bz = int(std::floor(z));  // getOnPosLegacy
    const u16 id = blockAt(bx, by, bz);
    if (!id) return;
    const std::string_view n = baseName(id);
    if (n == "barrier" || n == "light" || n == "structure_void" || n == "moving_piston") return;  // RenderShape.INVISIBLE
    double px = x + (double(randf()) - 0.5) * width, pz = z + (double(randf()) - 0.5) * width;
    if (int(std::floor(x)) != bx) px = std::clamp(px, double(bx), bx + 1.0);
    if (int(std::floor(z)) != bz) pz = std::clamp(pz, double(bz), bz + 1.0);
    Options o;
    o.block = netState(id);
    static int block = -2;
    if (block == -2) block = typeId("block");
    add(block, px, y + 0.1, pz, vel[0] * -4.0, 1.5, vel[2] * -4.0, &o);
}

void Particles::spawnPower(std::string_view type, double x, double y, double z, double vx, double vy, double vz, float power) {
    Options o;
    o.scale = power;
    add(typeId(type), x, y, z, vx, vy, vz, &o);
}

void Particles::spawnColored(std::string_view type, double x, double y, double z, double vx, double vy, double vz, u32 argb) {
    Options o;
    o.hasColor = true, o.color = argb;
    add(typeId(type), x, y, z, vx, vy, vz, &o);
}

bool Particles::readEncoded(net::Reader& r, std::string& raw) const {
    const u8* start = r.p;
    const int type = r.varInt();
    Options o;
    if (!r.ok || type < 0 || size_t(type) >= types_.size() || !readOptions(&r, type, o)) return false;
    raw.assign(reinterpret_cast<const char*>(start), size_t(r.p - start));
    return true;
}

void Particles::spawnEncoded(const std::string& raw, double x, double y, double z, double vx, double vy, double vz) {
    net::Reader r(reinterpret_cast<const u8*>(raw.data()), raw.size());
    const int type = r.varInt();
    Options o;
    if (!r.ok || type < 0 || size_t(type) >= types_.size() || !readOptions(&r, type, o)) return;
    add(type, x, y, z, vx, vy, vz, &o);
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

// FluidState.isSourceOfType(WATER): a still water block, a bubble column or a waterlogged block.
bool Particles::waterSource(int x, int y, int z) {
    const u16 id = blockAt(x, y, z);
    if (registry_.infos()[id].waterlogged) return true;
    const std::string& n = registry_.name(id);
    if (n.starts_with("minecraft:bubble_column")) return true;
    return n.starts_with("minecraft:water") && registry_.property(id, "level") == "0";
}

// PotentSulfurBlockEntity.isGeyserPassableBlock: air, water, or no collision.
bool Particles::geyserPassable(int x, int y, int z) {
    const u16 id = blockAt(x, y, z);
    if (!id || registry_.name(id).starts_with("minecraft:water")) return true;
    return registry_.infos()[id].collisionCount == 0;
}

// Level.clip with COLLIDER blocks and no fluids: true when a block's collision box is in the way from a to b.
bool Particles::clipBlocks(const double a[3], const double b[3]) {
    const double d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    int v[3] = {int(std::floor(a[0])), int(std::floor(a[1])), int(std::floor(a[2]))};
    const int end[3] = {int(std::floor(b[0])), int(std::floor(b[1])), int(std::floor(b[2]))};
    int step[3];
    double tMax[3], tDelta[3];
    for (int k = 0; k < 3; ++k) {
        step[k] = d[k] > 0 ? 1 : d[k] < 0 ? -1 : 0;
        tDelta[k] = step[k] ? std::abs(1.0 / d[k]) : 1e30;
        const double next = step[k] > 0 ? std::floor(a[k]) + 1 - a[k] : a[k] - std::floor(a[k]);
        tMax[k] = step[k] ? next * tDelta[k] : 1e30;
    }
    for (int guard = 0; guard < 64; ++guard) {
        const BlockInfo& bi = registry_.infos()[blockAt(v[0], v[1], v[2])];
        for (u32 i = 0; i < bi.collisionCount; ++i) {  // the segment against the box (slabs)
            const auto& box = registry_.box(bi.collisionFirst + i);
            double t0 = 0, t1 = 1;
            bool hit = true;
            for (int k = 0; k < 3 && hit; ++k) {
                const double lo = v[k] + box.min[k], hi = v[k] + box.max[k];
                if (std::abs(d[k]) < 1e-12) {
                    hit = a[k] >= lo && a[k] <= hi;
                } else {
                    double ta = (lo - a[k]) / d[k], tb = (hi - a[k]) / d[k];
                    if (ta > tb) std::swap(ta, tb);
                    t0 = std::max(t0, ta), t1 = std::min(t1, tb);
                    hit = t0 <= t1;
                }
            }
            if (hit) return true;
        }
        if (v[0] == end[0] && v[1] == end[1] && v[2] == end[2]) return false;
        const int k = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
        if (tMax[k] > 1.0) return false;
        v[k] += step[k], tMax[k] += tDelta[k];
    }
    return false;
}

// PotentSulfurBlockEntity.canBeReachedByNoxiousGas
bool Particles::noxiousGasReaches(int sx, int sy, int sz, const double pos[3]) {
    const int bx = int(std::floor(pos[0])), by = int(std::floor(pos[1])), bz = int(std::floor(pos[2]));
    if (!geyserPassable(bx, by, bz)) return false;
    const double dx = pos[0] - (sx + 0.5), dy = pos[1] - (sy + 0.5), dz = pos[2] - (sz + 0.5);
    if (dx * dx + dy * dy + dz * dz > 9.0) return false;
    const double belowSource[3] = {sx + 0.5, sy - 0.5, sz + 0.5}, belowPos[3] = {pos[0], pos[1] - 1.0, pos[2]};
    return waterSource(int(std::floor(belowPos[0])), int(std::floor(belowPos[1])), int(std::floor(belowPos[2]))) && !clipBlocks(belowSource, belowPos);
}

// PotentSulfurBlockEntity.findNoxiousGasSourceBlock: the open block above up to 4 blocks of still water; false: none.
bool Particles::gasSource(int x, int y, int z, int& sourceY) {
    for (int py = y + 1; py <= y + 5; ++py) {
        const u16 id = blockAt(x, py, z);
        const bool waterSrc = waterSource(x, py, z);
        const bool water = registry_.name(id).starts_with("minecraft:water");
        if (!waterSrc || (!water && !geyserPassable(x, py, z))) {
            if (!id || geyserPassable(x, py, z)) {
                sourceY = py;
                return true;
            }
            return false;
        }
    }
    return false;
}

// The potent sulfur block entities' client tickers near the camera: WET / DORMANT puff noxious gas every 20 ticks,
// ERUPTING / CONTINUOUS throw a geyser plume every 20 ticks of the eruption with its sound every 40.
void Particles::tickPotentSulfur() {
    static int cloud = -2, geyser = -2;
    if (cloud == -2) cloud = typeId("noxious_gas_cloud"), geyser = typeId("geyser");
    const int ccx = int(std::floor(cam_[0])) >> 4, ccz = int(std::floor(cam_[2])) >> 4;
    std::unordered_map<u64, u64> seen;
    geyserColumns_.clear();
    for (int cz = ccz - 3; cz <= ccz + 3; ++cz)
        for (int cx = ccx - 3; cx <= ccx + 3; ++cx) {
            const auto* list = world_.blockEntities(cx, cz);
            if (!list) continue;
            for (const World::BlockEntityNbt& be : *list) {
                const u16 id = blockAt(be.x, be.y, be.z);
                if (!registry_.name(id).starts_with("minecraft:potent_sulfur")) continue;
                const std::string_view state = registry_.property(id, "potent_sulfur_state");
                const u64 key = (u64(u32(be.x)) << 40) ^ (u64(u32(be.y)) << 20) ^ u64(u32(be.z));
                if (state == "wet" || state == "dormant") {
                    int sy;
                    if (ticks_ % 20 == 0 && gasSource(be.x, be.y, be.z, sy)) add(cloud, be.x + 0.5, sy + 0.5, be.z + 0.5, 0, 0, 0);
                } else if (state == "erupting" || state == "continuous") {
                    // eruptionTick: when the block entity appears or the eruption starts (the block event on placing)
                    auto it = eruptions_.find(key);
                    if (it == eruptions_.end() || it->second.second != id) it = eruptions_.insert_or_assign(key, std::make_pair(ticks_, id)).first;
                    seen[key] = 1;
                    int sy;
                    if (!gasSource(be.x, be.y, be.z, sy)) continue;
                    {  // getUnobstructedBlockCount: up to 6 blocks per block of water, to the first obstruction
                        const int water = sy - be.y - 1;
                        int height = 0;
                        while (height < 6 * water && geyserPassable(be.x, be.y + 1 + height, be.z)) ++height;
                        if (height > 0) geyserColumns_.push_back({be.x, be.y + 1, be.z, height, water});
                    }
                    const u64 t = ticks_ - it->second.first;
                    if (t % 20 == 0) {
                        Options o;
                        o.waterBlocks = sy - be.y - 1;
                        add(geyser, be.x + 0.5, sy, be.z + 0.5, 0, 0, 0, &o);
                    }
                    if (t % 40 == 0)
                        if (audio::AudioSystem* a = audio::instance()) {
                            const double pos[3] = {be.x + 0.5, sy + 0.5, be.z + 0.5};
                            a->playNetwork(-1, state == "continuous" ? "minecraft:block.potent_sulfur.geyser_continuous_eruption_active"
                                                                     : "minecraft:block.potent_sulfur.geyser_eruption_active",
                                           audio::Category::Block, pos, 1.0f, 1.0f);
                        }
                }
            }
        }
    std::erase_if(eruptions_, [&](const auto& e) { return !seen.contains(e.first); });
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
            if (p.e0 > 0.5f) {  // vault_connection: LifetimeAlpha(0, 0.6, 0.25, 1)
                const float a = std::clamp((float(p.age) / float(p.lifetime) - 0.25f) / 0.75f, 0.0f, 1.0f);
                p.a = 0.6f * a;
            }
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
        case Kind::Ash:
            defaultTick();
            ageSprite();
            break;
        case Kind::DragonBreath:
            if (p.age++ >= p.lifetime) return remove();
            ageSprite();
            if (p.flags & OnGround) p.yd = 0, p.e3 = 1;  // hasHitGround
            if (p.e3 > 0.5f) p.yd += 0.002;
            move(p, p.xd, p.yd, p.zd);
            if (p.y == p.yo) p.xd *= 1.1, p.zd *= 1.1;
            p.xd *= p.friction, p.zd *= p.friction;
            if (p.e3 > 0.5f) p.yd *= p.friction;
            break;
        case Kind::Wake: {
            const int life = 60 - p.lifetime;
            if (p.lifetime-- <= 0) return remove();
            move(p, p.xd, p.yd, p.zd);
            p.xd *= 0.98f, p.yd *= 0.98f, p.zd *= 0.98f;
            if (!t.sprites.empty()) p.sprite = t.sprites[size_t(life % 4) * (t.sprites.size() - 1) / 4];  // SpriteSet.get(i, 4)
            break;
        }
        case Kind::Sweep:
            if (p.age++ >= p.lifetime) return remove();
            ageSprite();
            break;
        case Kind::SquidInk: {
            defaultTick();
            if (p.flags & Removed) break;
            ageSprite();
            if (p.age > p.lifetime / 2) p.a = 1.0f - (float(p.age) - float(p.lifetime / 2)) / float(p.lifetime);
            if (blockAt(int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z))) == 0) p.yd -= 0.0074f;
            break;
        }
        case Kind::Vertical:
            defaultTick();
            break;
        case Kind::DustPlume:
            p.gravity *= 0.88f, p.friction *= 0.92f;
            defaultTick();
            ageSprite();
            break;
        case Kind::TrialDetection:
            defaultTick();
            ageSprite();
            break;
        case Kind::FlyStraight: {
            if (p.age++ >= p.lifetime) return remove();
            const float n = float(p.age) / float(p.lifetime), k = 1.0f - n;
            p.x = p.sx + p.xd * k, p.y = p.sy + p.yd * k, p.z = p.sz + p.zd * k;
            auto lin = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
            auto srgb = [](float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; };
            const float from[3] = {0x45 / 255.0f, 0xB8 / 255.0f, 0xFE / 255.0f};  // ARGB.srgbLerp to white
            float out[3];
            for (int i = 0; i < 3; ++i) out[i] = srgb(lin(from[i]) + (1.0f - lin(from[i])) * n);
            p.r = out[0], p.g = out[1], p.b = out[2];
            break;
        }
        case Kind::SuspendedAir:
        case Kind::Marker:
            defaultTick();
            break;
        case Kind::ReversePortal: {
            if (p.age++ >= p.lifetime) return remove();
            const float speed = float(p.age) / float(p.lifetime);
            p.x += p.xd * speed, p.y += p.yd * speed, p.z += p.zd * speed;
            break;
        }
        case Kind::CurrentDown: {
            if (p.age++ >= p.lifetime) return remove();
            p.xd += 0.6f * std::cos(p.e0), p.zd += 0.6f * std::sin(p.e0);
            p.xd *= 0.07, p.zd *= 0.07;
            move(p, p.xd, p.yd, p.zd);
            const BlockInfo& b = registry_.infos()[blockAt(int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z)))];
            if (!(b.fluid || b.waterlogged) || (p.flags & OnGround)) return remove();
            p.e0 += 0.08f;
            break;
        }
        case Kind::BubbleUp: {
            defaultTick();
            if (p.flags & Removed) break;
            const BlockInfo& b = registry_.infos()[blockAt(int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z)))];
            if (!(b.fluid || b.waterlogged)) remove();
            break;
        }
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
        case Kind::SulfurBubble: {
            defaultTick();
            if (p.flags & Removed) break;
            const int bx = int(std::floor(p.x)), by = int(std::floor(p.y)), bz = int(std::floor(p.z));
            if (!waterSource(bx, by, bz) || p.y >= p.sy + p.e0 || p.y <= p.sz) return remove();
            auto wiggle = [&] { return double(randf() * 0.003f * ((rng_() & 1) ? 1.0f : -1.0f)) * 0.5; };
            p.xd += wiggle(), p.zd += wiggle();
            move(p, p.xd, 0, p.zd);
            const float progress = float((p.y - p.sy) / p.e0);
            p.quadSize = p.e1 + progress * (0.15f - p.e1);
            p.sz = p.y;
            break;
        }
        case Kind::NoxiousGas:
            defaultTick();
            ageSprite();
            if (float(p.age) > p.e0) p.a = (float(p.lifetime) - (float(p.age) - p.e0)) / float(p.lifetime);
            break;
        case Kind::GasCloud: {
            if (p.age++ >= p.lifetime) return remove();
            if (p.age % 2 == 0) {  // a random spot within 3 blocks the gas can reach
                static int gas = -2;
                if (gas == -2) gas = typeId("noxious_gas");
                const int sx = int(std::floor(p.x)), sy = int(std::floor(p.y)), sz = int(std::floor(p.z));
                double dx = randf() - 0.5f, dz = randf() - 0.5f;
                const double len = std::sqrt(dx * dx + dz * dz);
                if (len > 1e-9) dx /= len, dz /= len;
                const double dist = randf() * 3.0f;
                const double pos[3] = {sx + 0.5 + dx * dist, sy + 0.5 - 0.25, sz + 0.5 + dz * dist};
                if (noxiousGasReaches(sx, sy, sz, pos)) add(gas, pos[0], pos[1], pos[2], 0, 0, 0);
            }
            break;
        }
        case Kind::GeyserEruption: {
            if (p.age++ >= p.lifetime) return remove();
            static int baseId = -2, plumeId = -2, poofId = -2;
            if (baseId == -2) baseId = typeId("geyser_base"), plumeId = typeId("geyser_plume"), poofId = typeId("geyser_poof");
            Options o;
            o.waterBlocks = int(p.e0);
            if (p.age % 2 == 0) {
                o.power = 1.5f;
                for (int i = 0; i < 2; ++i) add(baseId, p.x, p.y, p.z, p.sx, p.sy, p.sz, &o);
            }
            for (int i = 0; i < o.waterBlocks + 2; ++i) add(plumeId, p.x, p.y, p.z, p.sx, p.sy, p.sz, &o);
            if (p.age % 10 == 0) {
                o.power = 2.0f;
                for (int i = 0; i < 20; ++i) add(poofId, p.x, p.y, p.z, p.sx, p.sy, p.sz, &o);
            }
            break;
        }
        case Kind::GeyserBase:
            defaultTick();
            ageSprite();
            break;
        case Kind::GeyserPlume: {
            defaultTick();
            if (p.flags & Removed) break;
            if (!p.target && (p.yd < 0.0 || p.y > p.sy + p.e0 || p.y == p.yo)) {
                p.lifetime = std::min(p.lifetime, p.age + 5);
                p.friction = 0;
                p.target = 1;
            }
            const double progress = std::clamp((p.y - p.sy) / double(p.e0), 0.0, 1.0);
            p.gravity = p.e3 * float(progress * progress * progress) * 0.12f;
            p.xd = progress * p.e1, p.zd = progress * p.e2;
            ageSprite();
            p.quadSize = float(p.sx + progress * (p.sz - p.sx));
            break;
        }
        case Kind::Trail: {
            if (p.age++ >= p.lifetime) return remove();
            const double alpha = 1.0 / double(p.lifetime - p.age);  // Mth.lerp(1 / ticksRemaining, pos, target)
            p.x += (p.sx - p.x) * alpha, p.y += (p.sy - p.y) * alpha, p.z += (p.sz - p.z) * alpha;
            break;
        }
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

// ---------------------------------------------------------------------------------------------------------
// ClientLevel.animateTick: blocks around the camera, their fluids, the held marker block and the biome's particles

namespace {

enum Amb : u8 {
    AmbNone, AmbTorch, AmbWallTorch, AmbSoulTorch, AmbSoulWallTorch, AmbCopperTorch, AmbCopperWallTorch, AmbRedstoneTorch,
    AmbRedstoneWallTorch, AmbLeaves, AmbCampfire, AmbSoulCampfire, AmbFurnace, AmbBlastFurnace, AmbSmoker, AmbSulfur,
    AmbCandle, AmbCandleCake, AmbFire, AmbSoulFire, AmbBeehive, AmbBrewing, AmbBrushable, AmbBubbleColumn, AmbCreakingHeart,
    AmbCryingObsidian, AmbDriedGhast, AmbDeadBush, AmbDryGrass, AmbEnchanting, AmbEndGateway, AmbEndPortal, AmbEndRod,
    AmbEnderChest, AmbEyeblossom, AmbFalling, AmbSand, AmbFireflyBush, AmbHangingMoss, AmbLever, AmbLightningRod,
    AmbMycelium, AmbNetherPortal, AmbDripstone, AmbRedstoneOre, AmbRedstoneWire, AmbRepeater, AmbRespawnAnchor,
    AmbSculkSensor, AmbSporeBlossom, AmbWetSponge, AmbWitherRose
};

// Leaves: the falling leaf particle (FallingParticlesLeavesBlock), its chance and colour (0: the block's tint here).
struct LeafDef {
    const char* block;
    const char* particle;  // nullptr: no falling leaves (spruce)
    float chance;
    u32 color;  // tinted_leaves colour; 1: biome foliage, 0: untinted
    bool poplar;
};
const LeafDef kLeaves[] = {
    {"oak_leaves", "tinted_leaves", 0.01f, 1, false},       {"spruce_leaves", nullptr, 0, 0, false},
    {"birch_leaves", "tinted_leaves", 0.01f, 0xFF80A755, false}, {"jungle_leaves", "tinted_leaves", 0.01f, 1, false},
    {"acacia_leaves", "tinted_leaves", 0.01f, 1, false},    {"dark_oak_leaves", "tinted_leaves", 0.01f, 1, false},
    {"mangrove_leaves", "tinted_leaves", 0.01f, 1, false},  {"cherry_leaves", "cherry_leaves", 0.1f, 0, false},
    {"pale_oak_leaves", "pale_oak_leaves", 0.02f, 0, false}, {"red_poplar_leaves", "red_poplar_leaves", 0.01f, 0, true},
    {"orange_poplar_leaves", "orange_poplar_leaves", 0.01f, 0, true}, {"yellow_poplar_leaves", "yellow_poplar_leaves", 0.01f, 0, true},
    {"azalea_leaves", "tinted_leaves", 0.01f, 0xFF70922D, false}, {"flowering_azalea_leaves", "tinted_leaves", 0.01f, 0xFF70922D, false},
};

const char* const kFlammable[] = {  // FireBlock.bootStrap (wool, wool stairs / slabs and dyed carpets by suffix)
#include "flammable_blocks.inc"
};

constexpr int kStep[6][3] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};  // Direction order
int opposite(int d) { return d ^ 1; }
int dirOf(std::string_view f) {
    return f == "down" ? 0 : f == "up" ? 1 : f == "north" ? 2 : f == "south" ? 3 : f == "west" ? 4 : f == "east" ? 5 : -1;
}

}  // namespace

std::string_view Particles::baseName(u16 id) const {
    const std::string& n = registry_.name(id);
    const size_t b = n.find('['), c = n.find(':');
    return std::string_view(n).substr(c == std::string::npos ? 0 : c + 1, b == std::string::npos ? std::string::npos : b - c - 1);
}

int Particles::netState(u16 local) {
    auto it = netStates_.find(local);
    if (it != netStates_.end()) return it->second;
    const int id = reports_.stateId(registry_.name(local));
    netStates_[local] = id;
    return id;
}

// BlockState.isFaceSturdy (FULL): a collision box covering the whole face.
bool Particles::faceSturdy(u16 id, int dir) const {
    const BlockInfo& b = registry_.infos()[id];
    constexpr float e = 1e-4f;
    for (u32 i = 0; i < b.collisionCount; ++i) {
        const Box& s = registry_.box(b.collisionFirst + i);
        const int axis = dir < 2 ? 1 : dir < 4 ? 2 : 0;
        const bool high = dir & 1;
        if (high ? s.max[axis] < 1 - e : s.min[axis] > e) continue;
        bool covers = true;
        for (int a = 0; a < 3; ++a)
            if (a != axis && (s.min[a] > e || s.max[a] < 1 - e)) covers = false;
        if (covers) return true;
    }
    return false;
}

bool Particles::fullCollision(u16 id) const {  // isCollisionShapeFullBlock
    const BlockInfo& b = registry_.infos()[id];
    for (u32 i = 0; i < b.collisionCount; ++i) {
        const Box& s = registry_.box(b.collisionFirst + i);
        if (s.min[0] <= 1e-4f && s.min[1] <= 1e-4f && s.min[2] <= 1e-4f && s.max[0] >= 1 - 1e-4f && s.max[1] >= 1 - 1e-4f &&
            s.max[2] >= 1 - 1e-4f)
            return true;
    }
    return false;
}

bool Particles::tagged(std::string_view tag, u16 id) const {
    if (!blockTag) return false;
    const std::string& n = registry_.name(id);
    return blockTag(tag, std::string_view(n).substr(0, n.find('[')));
}

void Particles::localSound(const char* id, double x, double y, double z, float volume, float pitch, bool ambientCategory) {
    if (audio::AudioSystem* a = audio::instance()) {
        const double pos[3] = {x, y, z};
        a->playNetwork(-1, std::string("minecraft:") + id, ambientCategory ? audio::Category::Ambient : audio::Category::Block, pos,
                       volume, pitch);
    }
}

// Level.isRainingAt: raining, the sky above (no motion-blocking block higher up) and a biome where it rains.
bool Particles::rainingAt(int x, int y, int z) {
    const float rain = rainOverride_ >= 0 ? rainOverride_ : rain_;
    if (rain <= 0.2f) return false;
    const u8 biome = world_.biomeAt(x, z);
    if (biome != World::kUnknownBiome) {
        const std::string_view b = World::biomeNameView(biome);
        if (b.find("desert") != b.npos || b.find("savanna") != b.npos || b.find("badlands") != b.npos || b.find("snowy") != b.npos ||
            b.find("frozen") != b.npos || b.find("ice") != b.npos || b.find("grove") != b.npos || b.find("peaks") != b.npos)
            return false;
    }
    for (int yy = y; yy < y + 64; ++yy) {
        const u16 id = blockAt(x, yy, z);
        if (id && (registry_.infos()[id].collisionCount || registry_.infos()[id].fluid)) return false;
    }
    return true;
}

int Particles::surfaceY(int x, int z, bool motionBlocking) {  // Heightmap WORLD_SURFACE / MOTION_BLOCKING_NO_LEAVES
    const int top = (world_.maxSectionY() + 1) * 16 - 1, bottom = world_.minSectionY() * 16;
    for (int y = top; y >= bottom; --y) {
        const u16 id = blockAt(x, y, z);
        if (!id) continue;
        if (!motionBlocking) return y + 1;
        const BlockInfo& b = registry_.infos()[id];
        if ((b.collisionCount || b.fluid || b.waterlogged) && baseName(id).find("leaves") == std::string_view::npos) return y + 1;
    }
    return bottom;
}

u8 Particles::ambientKind(u16 id) {
    if (blockKinds_.size() <= id) blockKinds_.resize(size_t(id) + 1, 0);
    if (blockKinds_[id]) return u8(blockKinds_[id] - 1);
    const std::string_view n = baseName(id);
    u8 k = AmbNone;
    if (n == "torch") k = AmbTorch;
    else if (n == "wall_torch") k = AmbWallTorch;
    else if (n == "soul_torch") k = AmbSoulTorch;
    else if (n == "soul_wall_torch") k = AmbSoulWallTorch;
    else if (n == "copper_torch") k = AmbCopperTorch;
    else if (n == "copper_wall_torch") k = AmbCopperWallTorch;
    else if (n == "redstone_torch") k = AmbRedstoneTorch;
    else if (n == "redstone_wall_torch") k = AmbRedstoneWallTorch;
    else if (n.ends_with("leaves")) k = AmbLeaves;
    else if (n == "campfire") k = AmbCampfire;
    else if (n == "soul_campfire") k = AmbSoulCampfire;
    else if (n == "furnace") k = AmbFurnace;
    else if (n == "blast_furnace") k = AmbBlastFurnace;
    else if (n == "smoker") k = AmbSmoker;
    else if (n == "potent_sulfur") k = AmbSulfur;
    else if (n.ends_with("candle_cake")) k = AmbCandleCake;
    else if (n == "candle" || n.ends_with("_candle")) k = AmbCandle;
    else if (n == "fire") k = AmbFire;
    else if (n == "soul_fire") k = AmbSoulFire;
    else if (n == "beehive" || n == "bee_nest") k = AmbBeehive;
    else if (n == "brewing_stand") k = AmbBrewing;
    else if (n == "suspicious_sand" || n == "suspicious_gravel") k = AmbBrushable;
    else if (n == "bubble_column") k = AmbBubbleColumn;
    else if (n == "creaking_heart") k = AmbCreakingHeart;
    else if (n == "crying_obsidian") k = AmbCryingObsidian;
    else if (n == "dried_ghast") k = AmbDriedGhast;
    else if (n == "dead_bush") k = AmbDeadBush;
    else if (n == "short_dry_grass" || n == "tall_dry_grass") k = AmbDryGrass;
    else if (n == "enchanting_table") k = AmbEnchanting;
    else if (n == "end_gateway") k = AmbEndGateway;
    else if (n == "end_portal") k = AmbEndPortal;
    else if (n == "end_rod") k = AmbEndRod;
    else if (n == "ender_chest") k = AmbEnderChest;
    else if (n == "open_eyeblossom") k = AmbEyeblossom;
    else if (n == "sand" || n == "red_sand") k = AmbSand;
    else if (n == "gravel" || n.ends_with("_concrete_powder") || n.ends_with("anvil") || n == "dragon_egg") k = AmbFalling;
    else if (n == "firefly_bush") k = AmbFireflyBush;
    else if (n == "pale_hanging_moss") k = AmbHangingMoss;
    else if (n == "lever") k = AmbLever;
    else if (n.ends_with("lightning_rod")) k = AmbLightningRod;
    else if (n == "mycelium") k = AmbMycelium;
    else if (n == "nether_portal") k = AmbNetherPortal;
    else if (n == "pointed_dripstone") k = AmbDripstone;
    else if (n == "redstone_ore" || n == "deepslate_redstone_ore") k = AmbRedstoneOre;
    else if (n == "redstone_wire") k = AmbRedstoneWire;
    else if (n == "repeater") k = AmbRepeater;
    else if (n == "respawn_anchor") k = AmbRespawnAnchor;
    else if (n == "sculk_sensor" || n == "calibrated_sculk_sensor") k = AmbSculkSensor;
    else if (n == "spore_blossom") k = AmbSporeBlossom;
    else if (n == "wet_sponge") k = AmbWetSponge;
    else if (n == "wither_rose") k = AmbWitherRose;
    blockKinds_[id] = u8(k + 1);
    return k;
}

bool Particles::canBurn(u16 id) {  // FireBlock.canBurn: flammable
    const std::string_view n = baseName(id);
    if (n.ends_with("_wool") || n.ends_with("_wool_stairs") || n.ends_with("_wool_slab") || (n.ends_with("_carpet") && n != "moss_carpet"))
        return true;
    if (flammable_.empty())
        for (const char* f : kFlammable) flammable_.insert(f);
    return flammable_.count(std::string(n)) != 0;
}

// The biome's visual/ambient_particles (worldgen/biome/<name>.json): (particle type, probability).
const std::vector<std::pair<int, float>>& Particles::biomeParticles(u8 biome) {
    auto it = biomeParticles_.find(biome);
    if (it != biomeParticles_.end()) return it->second;
    std::vector<std::pair<int, float>>& out = biomeParticles_[biome];
    const std::string name(World::biomeNameView(biome));
    const size_t c = name.find(':');
    std::vector<u8> bytes;
    if (c != std::string::npos &&
        AssetSource::data().read("data/" + name.substr(0, c) + "/worldgen/biome/" + name.substr(c + 1) + ".json", bytes)) {
        const nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_object() && j.contains("attributes") && j["attributes"].contains("minecraft:visual/ambient_particles")) {
            const nlohmann::json& a = j["attributes"]["minecraft:visual/ambient_particles"];
            const nlohmann::json& list = a.is_object() && a.contains("argument") ? a["argument"] : a;
            if (list.is_array())
                for (const auto& e : list) {
                    if (!e.is_object() || !e.contains("particle")) continue;
                    const nlohmann::json& p = e["particle"];
                    std::string type = p.is_object() ? p.value("type", "") : p.is_string() ? p.get<std::string>() : "";
                    if (type.starts_with("minecraft:")) type = type.substr(10);
                    const int t = typeId(type);
                    if (t >= 0) out.push_back({t, e.value("probability", 0.0f)});
                }
        }
    }
    return out;
}

void Particles::ambient(const double cam[3]) {
    const int cx = int(std::floor(cam[0])), cy = int(std::floor(cam[1])), cz = int(std::floor(cam[2]));
    static u16 markerBlock = 0;
    static std::string markerName;
    if (markerName != markerBlock_) markerName = markerBlock_, markerBlock = 0;
    for (int i = 0; i < 667; ++i)
        for (int radius : {16, 32}) {
            const int x = cx + int(rng_() % u32(radius)) - int(rng_() % u32(radius));
            const int y = cy + int(rng_() % u32(radius)) - int(rng_() % u32(radius));
            const int z = cz + int(rng_() % u32(radius)) - int(rng_() % u32(radius));
            const u16 id = blockAt(x, y, z);
            if (id) animateBlock(id, x, y, z);
            if (!markerName.empty() && id && baseName(id) == markerName) {  // the held barrier / light (creative)
                Options o;
                o.block = netState(id);
                static int marker = -2;
                if (marker == -2) marker = typeId("block_marker");
                add(marker, x + 0.5, y + 0.5, z + 0.5, 0, 0, 0, &o);
            }
            if (!fullCollision(id)) {  // EnvironmentAttributes.AMBIENT_PARTICLES of the biome
                const u8 biome = world_.biomeAt(x, z);
                if (biome != World::kUnknownBiome)
                    for (const auto& [type, chance] : biomeParticles(biome))
                        if (randf() <= chance) add(type, x + double(randf()), y + double(randf()), z + double(randf()), 0, 0, 0);
            }
        }
}

void Particles::animateBlock(u16 id, int x, int y, int z) {
    auto R = [&] { return double(randf()); };
    auto ri = [&](int n) { return int(rng_() % u32(n)); };
    auto type = [&](const char* n) {
        auto it = ambientTypes_.find(n);
        if (it == ambientTypes_.end()) it = ambientTypes_.emplace(n, typeId(n)).first;
        return it->second;
    };
    auto prop = [&](u16 b, std::string_view key) { return registry_.property(b, key); };
    auto dust = [&](u32 argb, float scale, double px, double py, double pz) {
        Options o;
        o.hasColor = true, o.color = argb, o.scale = scale;
        add(type("dust"), px, py, pz, 0, 0, 0, &o);
    };
    auto isFree = [&](u16 b) {  // FallingBlock.isFree
        const BlockInfo& i = registry_.infos()[b];
        return b == 0 || tagged("minecraft:fire", b) || i.fluid || i.replaceable;
    };
    auto below = [&](const char* particle, const Options* o) { add(type(particle), x + R(), y - 0.05, z + R(), 0, 0, 0, o); };
    const std::string_view facing = prop(id, "facing");
    const int fd = dirOf(facing);
    const BlockInfo& info = registry_.infos()[id];
    const u8 kind = ambientKind(id);
    switch (kind) {
        case AmbTorch:
        case AmbSoulTorch:
        case AmbCopperTorch: {  // TorchBlock
            add(type("smoke"), x + 0.5, y + 0.7, z + 0.5, 0, 0, 0);
            add(type(kind == AmbTorch ? "flame" : kind == AmbSoulTorch ? "soul_fire_flame" : "copper_fire_flame"), x + 0.5, y + 0.7, z + 0.5, 0, 0, 0);
            break;
        }
        case AmbWallTorch:
        case AmbSoulWallTorch:
        case AmbCopperWallTorch: {  // WallTorchBlock: 0.27 out from the wall, 0.22 higher
            if (fd < 2) break;
            const int o = opposite(fd);
            const double px = x + 0.5 + 0.27 * kStep[o][0], py = y + 0.7 + 0.22, pz = z + 0.5 + 0.27 * kStep[o][2];
            add(type("smoke"), px, py, pz, 0, 0, 0);
            add(type(kind == AmbWallTorch ? "flame" : kind == AmbSoulWallTorch ? "soul_fire_flame" : "copper_fire_flame"), px, py, pz, 0, 0, 0);
            break;
        }
        case AmbRedstoneTorch:
            if (prop(id, "lit") == "true")
                dust(0xFFFF0000, 1.0f, x + 0.5 + (R() - 0.5) * 0.2, y + 0.7 + (R() - 0.5) * 0.2, z + 0.5 + (R() - 0.5) * 0.2);
            break;
        case AmbRedstoneWallTorch:
            if (prop(id, "lit") == "true" && fd >= 2) {
                const int o = opposite(fd);
                dust(0xFFFF0000, 1.0f, x + 0.5 + (R() - 0.5) * 0.2 + 0.27 * kStep[o][0], y + 0.7 + (R() - 0.5) * 0.2 + 0.22,
                     z + 0.5 + (R() - 0.5) * 0.2 + 0.27 * kStep[o][2]);
            }
            break;
        case AmbLeaves: {
            const std::string_view n = baseName(id);
            const LeafDef* def = nullptr;
            for (const LeafDef& l : kLeaves)
                if (n == l.block) def = &l;
            if (!def) break;
            const u16 under = blockAt(x, y - 1, z);
            // LeavesBlock: water drips under leaves in the rain; poplars rustle beside a log
            if (rainingAt(x, y + 1, z) && ri(15) == 1 && (!registry_.infos()[under].occludes || !faceSturdy(under, 1)))
                below("dripping_water", nullptr);
            if (def->poplar && ri(300) == 0) {  // AmbientLeavesBlockSoundPlayer
                int logs = 0, same = 0;
                for (int d = 2; d < 6; ++d) {
                    const u16 nb = blockAt(x + kStep[d][0], y, z + kStep[d][2]);
                    if (tagged("minecraft:required_for_poplar_leaf_ambience", nb)) ++logs;
                    if (baseName(nb) == n) ++same;
                    if (logs == 1 && same == 3) {
                        localSound("block.poplar_leaves.ambient", x, y, z, 1.0f, 1.0f, true);
                        break;
                    }
                }
            }
            if (def->particle && randf() < def->chance && !faceSturdy(under, 1)) {  // makeFallingLeavesParticles
                Options o;
                if (def->color == 1) o.hasColor = true, o.color = 0xFF000000 | (foliageColor ? foliageColor(x, z) : 0x48B518);
                else if (def->color) o.hasColor = true, o.color = def->color;
                below(def->particle, o.hasColor ? &o : nullptr);
            }
            break;
        }
        case AmbCampfire:
        case AmbSoulCampfire:  // CampfireBlock (the cosy / signal smoke: CampfireBlockEntity, below)
            if (prop(id, "lit") != "true") break;
            campfires_.insert((u64(x & 0x3FFFFFF) << 38) | (u64(z & 0x3FFFFFF) << 12) | u64(y & 0xFFF));
            if (ri(10) == 0) localSound("block.campfire.crackle", x + 0.5, y + 0.5, z + 0.5, float(0.5 + R()), float(R() * 0.7 + 0.6), false);
            if (kind == AmbCampfire && ri(5) == 0) add(type("lava"), x + 0.5, y + 0.5, z + 0.5, R() / 2.0, 5.0e-5, R() / 2.0);
            break;
        case AmbFurnace:
        case AmbBlastFurnace: {
            if (prop(id, "lit") != "true" || fd < 2) break;
            if (R() < 0.1)
                localSound(kind == AmbFurnace ? "block.furnace.fire_crackle" : "block.blastfurnace.fire_crackle", x + 0.5, y, z + 0.5, 1, 1, false);
            const double ss = R() * 0.6 - 0.3;
            const double dx = kStep[fd][0] ? kStep[fd][0] * 0.52 : ss, dz = kStep[fd][2] ? kStep[fd][2] * 0.52 : ss;
            const double dy = R() * (kind == AmbFurnace ? 6.0 : 9.0) / 16.0;
            add(type("smoke"), x + 0.5 + dx, y + dy, z + 0.5 + dz, 0, 0, 0);
            if (kind == AmbFurnace) add(type("flame"), x + 0.5 + dx, y + dy, z + 0.5 + dz, 0, 0, 0);
            break;
        }
        case AmbSmoker:
            if (prop(id, "lit") != "true") break;
            if (R() < 0.1) localSound("block.smoker.smoke", x + 0.5, y, z + 0.5, 1, 1, false);
            add(type("smoke"), x + 0.5, y + 1.1, z + 0.5, 0, 0, 0);
            break;
        case AmbSulfur:  // PotentSulfurBlock: bubbles in the water above, now and then the gas sound
            if (prop(id, "potent_sulfur_state") != "dry" && waterSource(x, y + 1, z)) {
                for (int i = 0; i < 2; ++i) add(type("sulfur_bubbles"), x + R(), y + 1 + R(), z + R(), 0, 0, 0);
                if (ri(10) == 0) localSound("block.potent_sulfur.noxious_gas", x, y, z, 1, 1, true);
            }
            break;
        case AmbCandle:
        case AmbCandleCake: {  // AbstractCandleBlock.addParticlesAndSound at each wick
            if (prop(id, "lit") != "true") break;
            static const float kWicks[4][4][3] = {{{8, 8, 8}},
                                                  {{6, 7, 8}, {10, 8, 7}},
                                                  {{8, 5, 10}, {6, 7, 8}, {9, 8, 7}},
                                                  {{7, 5, 9}, {10, 7, 9}, {6, 7, 6}, {9, 8, 6}}};
            const std::string_view cs = prop(id, "candles");
            const int count = kind == AmbCandleCake ? 1 : std::clamp(cs.empty() ? 1 : cs[0] - '0', 1, 4);
            for (int w = 0; w < count; ++w) {
                const float* o = kind == AmbCandleCake ? nullptr : kWicks[count - 1][w];
                const double px = x + (o ? o[0] : 8) / 16.0, py = y + (o ? o[1] : 16) / 16.0, pz = z + (o ? o[2] : 8) / 16.0;
                const float chance = randf();
                if (chance < 0.3f) {
                    add(type("smoke"), px, py, pz, 0, 0, 0);
                    if (chance < 0.17f) localSound("block.candle.ambient", px + 0.5, py + 0.5, pz + 0.5, float(1 + R()), float(R() * 0.7 + 0.3), false);
                }
                add(type("small_flame"), px, py, pz, 0, 0, 0);
            }
            break;
        }
        case AmbFire:
        case AmbSoulFire: {  // BaseFireBlock
            if (ri(24) == 0) localSound("block.fire.ambient", x + 0.5, y + 0.5, z + 0.5, float(1 + R()), float(R() * 0.7 + 0.3), false);
            const u16 under = blockAt(x, y - 1, z);
            const bool soul = kind == AmbSoulFire;  // SoulFireBlock.canBurn: everything
            if (!(soul || canBurn(under)) && !faceSturdy(under, 1)) {
                auto side = [&](int dx, int dy, int dz, int axis, bool high) {
                    const u16 nb = blockAt(x + dx, y + dy, z + dz);
                    if (!canBurn(nb)) return;
                    for (int i = 0; i < 2; ++i) {
                        double p[3] = {x + R(), y + R(), z + R()};
                        p[axis] = (axis == 0 ? x : axis == 1 ? y : z) + (high ? 1 - R() * 0.1 : R() * 0.1);
                        add(type("large_smoke"), p[0], p[1], p[2], 0, 0, 0);
                    }
                };
                side(-1, 0, 0, 0, false), side(1, 0, 0, 0, true), side(0, 0, -1, 2, false), side(0, 0, 1, 2, true), side(0, 1, 0, 1, true);
            } else {
                for (int i = 0; i < 3; ++i) add(type("large_smoke"), x + R(), y + R() * 0.5 + 0.5, z + R(), 0, 0, 0);
            }
            break;
        }
        case AmbBeehive: {  // BeehiveBlock.trySpawnDripParticles (a full block: the drip forms under it)
            const std::string_view h = prop(id, "honey_level");
            if (h.empty() || h[0] - '0' < 5 || info.waterlogged || randf() < 0.3f) break;
            const u16 under = blockAt(x, y - 1, z);
            const BlockInfo& u = registry_.infos()[under];
            float top = 0;
            for (u32 i = 0; i < u.collisionCount; ++i) top = std::max(top, registry_.box(u.collisionFirst + i).max[1]);
            if ((top < 1.0f || !fullCollision(under)) && !u.fluid && !u.waterlogged) add(type("dripping_honey"), x + R(), y - 0.05, z + R(), 0, 0, 0);
            break;
        }
        case AmbBrewing:
            add(type("smoke"), x + 0.4 + R() * 0.2, y + 0.7 + R() * 0.3, z + 0.4 + R() * 0.2, 0, 0, 0);
            break;
        case AmbBrushable:
        case AmbFalling:
        case AmbSand:  // FallingBlock / BrushableBlock: dust under the block when it could fall
            if (ri(16) == 0 && isFree(blockAt(x, y - 1, z))) {
                Options o;
                o.block = netState(id);
                below("falling_dust", &o);
            }
            if (kind == AmbSand && blockAt(x, y + 1, z) == 0 && ri(2100) == 0 && desertSandAround(x, y, z))  // SandBlock
                localSound("block.sand.idle", x, y, z, 1, 1, true);
            break;
        case AmbBubbleColumn:
            if (prop(id, "drag") == "true") {
                add(type("current_down"), x + 0.5, y + 0.8, z, 0, 0, 0);
                if (ri(200) == 0) localSound("block.bubble_column.whirlpool_ambient", x, y, z, float(0.2 + R() * 0.2), float(0.9 + R() * 0.15), false);
            } else {
                add(type("bubble_column_up"), x + 0.5, y, z + 0.5, 0, 0.04, 0);
                add(type("bubble_column_up"), x + R(), y + R(), z + R(), 0, 0.04, 0);
                if (ri(200) == 0) localSound("block.bubble_column.upwards_ambient", x, y, z, float(0.2 + R() * 0.2), float(0.9 + R() * 0.15), false);
            }
            break;
        case AmbCreakingHeart: {  // the creaking_active timeline (night), not uprooted, between pale oak logs on its axis
            if (!ambientNight_ || prop(id, "creaking_heart_state") == "uprooted" || ri(16) != 0) break;
            const std::string_view axis = prop(id, "axis");
            const int a = axis == "x" ? 0 : axis == "z" ? 2 : 1;
            bool logs = true;
            for (int s : {-1, 1}) {
                int p[3] = {x, y, z};
                p[a] += s;
                const u16 nb = blockAt(p[0], p[1], p[2]);
                if (!tagged("minecraft:pale_oak_logs", nb) || prop(nb, "axis") != axis) logs = false;
            }
            if (logs) localSound("block.creaking_heart.idle", x, y, z, 1, 1, false);
            break;
        }
        case AmbCryingObsidian:
        case AmbWetSponge: {  // CryingObsidianBlock / WetSpongeBlock: a drop on a random open side (not the top)
            if (kind == AmbCryingObsidian && ri(5) != 0) break;
            const int d = ri(6);
            if (d == 1) break;
            const u16 nb = blockAt(x + kStep[d][0], y + kStep[d][1], z + kStep[d][2]);
            if (info.occludes && faceSturdy(nb, opposite(d))) break;
            if (kind == AmbCryingObsidian) {
                auto off = [&](int s) { return s == 0 ? R() : 0.5 + s * 0.6; };
                add(type("dripping_obsidian_tear"), x + off(kStep[d][0]), y + off(kStep[d][1]), z + off(kStep[d][2]), 0, 0, 0);
            } else {
                double px = x, py = y, pz = z;
                if (d == 0) py -= 0.05, px += R(), pz += R();
                else {
                    py += R() * 0.8;
                    if (d >= 4) pz += R(), px += d == 5 ? 1.1 : 0.05;
                    else px += R(), pz += d == 3 ? 1.1 : 0.05;
                }
                add(type("dripping_water"), px, py, pz, 0, 0, 0);
            }
            break;
        }
        case AmbDriedGhast:
            if (!info.waterlogged) {
                if (ri(40) == 0 && tagged("minecraft:triggers_ambient_dried_ghast_block_sounds", blockAt(x, y - 1, z)))
                    localSound("block.dried_ghast.ambient", x + 0.5, y + 0.5, z + 0.5, 1, 1, false);
                if (ri(6) == 0) add(type("white_smoke"), x + 0.5, y + 0.5, z + 0.5, 0, 0.02, 0);
            } else {
                if (ri(40) == 0) localSound("block.dried_ghast.ambient_water", x + 0.5, y + 0.5, z + 0.5, 1, 1, false);
                if (ri(6) == 0)
                    add(type("happy_villager"), x + 0.5 + (R() * 2 - 1) / 3.0, y + 0.9, z + 0.5 + (R() * 2 - 1) / 3.0, 0, R(), 0);
            }
            break;
        case AmbDeadBush:  // AmbientDesertBlockSoundsPlayer.playAmbientDeadBushSounds
            if (ri(130) == 0) {
                const u16 under = blockAt(x, y - 1, z);
                if ((baseName(under) == "red_sand" || tagged("minecraft:terracotta", under)) && ri(3) != 0) break;
                if (desertVegetationBelow(x, y, z)) localSound("block.deadbush.idle", x, y, z, 1, 1, true);
            }
            break;
        case AmbDryGrass:  // playAmbientDryGrassSounds: at the player (Level.playPlayerSound)
            if (ri(200) == 0 && desertVegetationBelow(x, y, z)) localSound("block.dry_grass.ambient", cam_[0], cam_[1], cam_[2], 1, 1, true);
            break;
        case AmbEnchanting:  // EnchantingTableBlock: glyphs from the bookshelves around it
            for (int ox = -2; ox <= 2; ++ox)
                for (int oz = -2; oz <= 2; ++oz) {
                    if (std::abs(ox) != 2 && std::abs(oz) != 2) continue;
                    for (int oy = 0; oy <= 1; ++oy) {
                        if (ri(16) != 0) continue;
                        if (!tagged("minecraft:enchantment_power_provider", blockAt(x + ox, y + oy, z + oz)) ||
                            !tagged("minecraft:enchantment_power_transmitter", blockAt(x + ox / 2, y + oy, z + oz / 2)))
                            continue;
                        add(type("enchant"), x + 0.5, y + 2.0, z + 0.5, ox + R() - 0.5, oy - R() - 1.0, oz + R() - 0.5);
                    }
                }
            break;
        case AmbEndGateway: {  // TheEndGatewayBlockEntity.getParticleAmount: the faces that show
            int count = 0;
            for (int d = 0; d < 6; ++d) count += registry_.infos()[blockAt(x + kStep[d][0], y + kStep[d][1], z + kStep[d][2])].occludes ? 0 : 1;
            for (int i = 0; i < count; ++i) {
                double px = x + R(), py = y + R(), pz = z + R();
                double xa = (R() - 0.5) * 0.5, ya = (R() - 0.5) * 0.5, za = (R() - 0.5) * 0.5;
                const int flip = ri(2) * 2 - 1;
                if (ri(2)) pz = z + 0.5 + 0.25 * flip, za = R() * 2.0 * flip;
                else px = x + 0.5 + 0.25 * flip, xa = R() * 2.0 * flip;
                add(type("portal"), px, py, pz, xa, ya, za);
            }
            break;
        }
        case AmbEndPortal:
            add(type("smoke"), x + R(), y + 0.8, z + R(), 0, 0, 0);
            break;
        case AmbEndRod: {
            if (fd < 0) break;
            const double px = x + 0.55 - R() * 0.1, py = y + 0.55 - R() * 0.1, pz = z + 0.55 - R() * 0.1;
            const double r = 0.4 - (R() + R()) * 0.4;
            if (ri(5) == 0)
                add(type("end_rod"), px + kStep[fd][0] * r, py + kStep[fd][1] * r, pz + kStep[fd][2] * r, gauss() * 0.005, gauss() * 0.005, gauss() * 0.005);
            break;
        }
        case AmbEnderChest:
            for (int i = 0; i < 3; ++i) {
                const int fx = ri(2) * 2 - 1, fz = ri(2) * 2 - 1;
                add(type("portal"), x + 0.5 + 0.25 * fx, y + R(), z + 0.5 + 0.25 * fz, R() * fx, (R() - 0.5) * 0.125, R() * fz);
            }
            break;
        case AmbEyeblossom:
            if (ri(700) == 0 && baseName(blockAt(x, y - 1, z)) == "pale_moss_block") localSound("block.eyeblossom.idle", x, y, z, 1, 1, true);
            break;
        case AmbFireflyBush: {  // the firefly_bush_sounds timeline (night), open sky; fireflies where it is dark enough
            if (ri(30) == 0 && ambientNight_ && surfaceY(x, z, true) <= y) localSound("block.firefly_bush.idle", x + 0.5, y + 0.5, z + 0.5, 1, 1, true);
            const u8 l = lightAt(x + 0.5, y + 0.5, z + 0.5);
            const int raw = std::max(int(l & 15), std::max(int(l >> 4) - skyDarken_, 0));  // getMaxLocalRawBrightness
            if (raw <= 13 && R() <= 0.7) add(type("firefly"), x + R() * 10 - 5, y + R() * 5, z + R() * 10 - 5, 0, 0, 0);
            break;
        }
        case AmbHangingMoss:
            if (ri(500) == 0) {
                const u16 above = blockAt(x, y + 1, z);
                if (tagged("minecraft:pale_oak_logs", above) || baseName(above) == "pale_oak_leaves")
                    localSound("block.pale_hanging_moss.idle", x, y, z, 1, 1, true);
            }
            break;
        case AmbLever: {  // LeverBlock.makeParticle(0.5)
            if (prop(id, "powered") != "true" || randf() >= 0.25f || fd < 0) break;
            const std::string_view face = prop(id, "face");
            const int conn = face == "ceiling" ? 0 : face == "floor" ? 1 : fd;  // getConnectedDirection
            const int a = opposite(fd), b = opposite(conn);
            dust(0xFFFF0000, 0.5f, x + 0.5 + 0.1 * kStep[a][0] + 0.2 * kStep[b][0], y + 0.5 + 0.1 * kStep[a][1] + 0.2 * kStep[b][1],
                 z + 0.5 + 0.1 * kStep[a][2] + 0.2 * kStep[b][2]);
            break;
        }
        case AmbLightningRod: {  // sparks on the top rod of a column while it thunders
            const float rain = rainOverride_ >= 0 ? rainOverride_ : rain_;
            if (thunder_ * rain <= 0.9f || ri(200) > int(ticks_ % 200) || fd < 0 || y != surfaceY(x, z, false) - 1) break;
            const int axis = fd < 2 ? 1 : fd < 4 ? 2 : 0;
            const int n = 1 + ri(2);
            for (int i = 0; i < n; ++i) {
                double p[3], v[3];
                for (int k = 0; k < 3; ++k) {
                    p[k] = (k == 0 ? x : k == 1 ? y : z) + 0.5 + (R() * 2 - 1) * (k == axis ? 0.5 : 0.125);
                    v[k] = k == axis ? R() * 2 - 1 : 0;
                }
                add(type("electric_spark"), p[0], p[1], p[2], v[0], v[1], v[2]);
            }
            break;
        }
        case AmbMycelium:
            if (ri(10) == 0) add(type("mycelium"), x + R(), y + 1.1, z + R(), 0, 0, 0);
            break;
        case AmbNetherPortal: {
            if (ri(100) == 0) localSound("block.portal.ambient", x + 0.5, y + 0.5, z + 0.5, 0.5f, float(R() * 0.4 + 0.8), false);
            const bool alongX = baseName(blockAt(x - 1, y, z)) == "nether_portal" || baseName(blockAt(x + 1, y, z)) == "nether_portal";
            for (int i = 0; i < 4; ++i) {
                double px = x + R(), py = y + R(), pz = z + R();
                double xa = (R() - 0.5) * 0.5, ya = (R() - 0.5) * 0.5, za = (R() - 0.5) * 0.5;
                const int flip = ri(2) * 2 - 1;
                if (!alongX) px = x + 0.5 + 0.25 * flip, xa = R() * 2.0 * flip;
                else pz = z + 0.5 + 0.25 * flip, za = R() * 2.0 * flip;
                add(type("portal"), px, py, pz, xa, ya, za);
            }
            break;
        }
        case AmbDripstone: {  // PointedDripstoneBlock: a free-hanging stalactite tip drips what is above its root
            if (prop(id, "vertical_direction") != "down" || prop(id, "thickness") != "tip" || info.waterlogged) break;
            const float r = randf();
            if (r > 0.12f) break;
            int top = y;
            for (int i = 0; i < 11 && baseName(blockAt(x, top + 1, z)) == "pointed_dripstone" && prop(blockAt(x, top + 1, z), "vertical_direction") == "down"; ++i)
                ++top;
            const u16 above = blockAt(x, top + 2, z);
            const std::string_view an = baseName(above);
            const BlockInfo& ai = registry_.infos()[above];
            int fluid = 0;  // 1 water, 2 lava
            if (an == "mud" && !ambientNether_) fluid = 1;
            else if (an == "lava") fluid = 2;
            else if (an == "water" || an == "bubble_column" || ai.waterlogged) fluid = 1;
            if (!(r < 0.02f || fluid)) break;
            const i64 seed = wg::positionSeed(x, 0, z);  // OffsetType.XZ, at most 0.125
            const double ox = std::clamp((double(float(seed & 15) / 15.0f) - 0.5) * 0.5, -0.125, 0.125);
            const double oz = std::clamp((double(float((seed >> 8) & 15) / 15.0f) - 0.5) * 0.5, -0.125, 0.125);
            const bool lava = fluid == 2 || (fluid == 0 && ambientNether_);  // visual/default_dripstone_particle
            add(type(lava ? "dripping_dripstone_lava" : "dripping_dripstone_water"), x + 0.5 + ox, y + 0.25, z + 0.5 + oz, 0, 0, 0);
            break;
        }
        case AmbRedstoneOre:  // RedStoneOreBlock.spawnParticles on the open sides
            if (prop(id, "lit") != "true") break;
            for (int d = 0; d < 6; ++d) {
                if (registry_.infos()[blockAt(x + kStep[d][0], y + kStep[d][1], z + kStep[d][2])].occludes) continue;
                const int axis = d < 2 ? 1 : d < 4 ? 2 : 0;
                double p[3];
                for (int k = 0; k < 3; ++k) p[k] = k == axis ? 0.5 + 0.5625 * kStep[d][k] : R();
                dust(0xFFFF0000, 1.0f, x + p[0], y + p[1], z + p[2]);
            }
            break;
        case AmbRedstoneWire: {  // RedstoneWireBlock: along each connection, coloured by the power
            const std::string_view ps = prop(id, "power");
            const int power = ps.empty() ? 0 : std::atoi(std::string(ps).c_str());
            if (power == 0) break;
            const float f = float(power) / 15.0f;
            const float red = f * 0.6f + 0.4f, green = std::clamp(f * f * 0.7f - 0.5f, 0.0f, 1.0f), blue = std::clamp(f * f * 0.6f - 0.7f, 0.0f, 1.0f);
            const u32 color = 0xFF000000u | u32(red * 255.0f) << 16 | u32(green * 255.0f) << 8 | u32(blue * 255.0f);
            auto line = [&](int from, int to, float a, float b) {  // spawnParticlesAlongLine
                const float span = b - a;
                if (randf() >= 0.2f * span) return;
                const float t = a + span * randf();
                dust(color, 1.0f, x + 0.5 + 0.4375 * kStep[from][0] + t * kStep[to][0], y + 0.5 + 0.4375 * kStep[from][1] + t * kStep[to][1],
                     z + 0.5 + 0.4375 * kStep[from][2] + t * kStep[to][2]);
            };
            static const char* kSides[4] = {"north", "south", "west", "east"};
            for (int i = 0; i < 4; ++i) {
                const int d = 2 + i;
                const std::string_view c = prop(id, kSides[i]);
                if (c == "up") line(d, 1, -0.5f, 0.5f);
                if (c == "up" || c == "side") line(0, d, 0.0f, 0.5f);
                else line(0, d, 0.0f, 0.3f);
            }
            break;
        }
        case AmbRepeater: {
            if (prop(id, "powered") != "true" || fd < 2) break;
            const double px = x + 0.5 + (R() - 0.5) * 0.2, py = y + 0.4 + (R() - 0.5) * 0.2, pz = z + 0.5 + (R() - 0.5) * 0.2;
            float offset = -5.0f;
            if (ri(2)) {
                const std::string_view ds = prop(id, "delay");
                offset = float((ds.empty() ? 1 : ds[0] - '0') * 2 - 1);
            }
            offset /= 16.0f;
            dust(0xFFFF0000, 1.0f, px + offset * kStep[fd][0], py, pz + offset * kStep[fd][2]);
            break;
        }
        case AmbRespawnAnchor: {
            const std::string_view ch = prop(id, "charges");
            if (ch.empty() || ch[0] == '0') break;
            if (ri(100) == 0) localSound("block.respawn_anchor.ambient", x + 0.5, y + 0.5, z + 0.5, 1, 1, false);
            add(type("reverse_portal"), x + 0.5 + (0.5 - R()), y + 1.0, z + 0.5 + (0.5 - R()), 0, R() * 0.04, 0);
            break;
        }
        case AmbSculkSensor: {
            if (prop(id, "sculk_sensor_phase") != "active") break;
            const int d = ri(6);
            if (d < 2) break;
            Options o;
            o.hasColor = true, o.color = 0xFF39D6E0, o.color2 = 0xFFFF0000, o.scale = 1.0f;  // SCULK_TO_REDSTONE
            add(type("dust_color_transition"), x + 0.5 + (kStep[d][0] == 0 ? 0.5 - R() : kStep[d][0] * 0.6), y + 0.25,
                z + 0.5 + (kStep[d][2] == 0 ? 0.5 - R() : kStep[d][2] * 0.6), 0, R() * 0.04, 0, &o);
            break;
        }
        case AmbSporeBlossom:
            add(type("falling_spore_blossom"), x + R(), y + 0.7, z + R(), 0, 0, 0);
            for (int i = 0; i < 14; ++i) {
                const int px = x + ri(21) - 10, py = y - ri(10), pz = z + ri(21) - 10;
                if (!fullCollision(blockAt(px, py, pz))) add(type("spore_blossom_air"), px + R(), py + R(), pz + R(), 0, 0, 0);
            }
            break;
        case AmbWitherRose:  // the flower shape's centre
            for (int i = 0; i < 3; ++i)
                if (ri(2)) add(type("smoke"), x + 0.5 + R() / 5.0, y + (0.5 - R()), z + 0.5 + R() / 5.0, 0, 0, 0);
            break;
        default: break;
    }
    // The fluid: WaterFluid / LavaFluid.animateTick, then FluidState drips through the block below.
    const std::string_view n = baseName(id);
    const bool water = n == "water" || n == "bubble_column" || info.waterlogged, lava = n == "lava";
    if (water) {
        const std::string_view lv = n == "water" ? prop(id, "level") : std::string_view("0");
        const int level = lv.empty() ? 0 : std::atoi(std::string(lv).c_str());
        if (level != 0 && level < 8) {  // flowing, not falling
            if (ri(64) == 0) localSound("block.water.ambient", x + 0.5, y + 0.5, z + 0.5, float(R() * 0.25 + 0.75), float(R() + 0.5), true);
        } else if (ri(10) == 0) {
            add(type("underwater"), x + R(), y + R(), z + R(), 0, 0, 0);
        }
        if (ri(10) == 0) fluidDrip(id, x, y, z, type("dripping_water"));
    } else if (lava) {
        if (blockAt(x, y + 1, z) == 0) {
            if (ri(100) == 0) {
                const double px = x + R(), py = y + 1.0, pz = z + R();
                add(type("lava"), px, py, pz, 0, 0, 0);
                localSound("block.lava.pop", px, py, pz, float(0.2 + R() * 0.2), float(0.9 + R() * 0.15), true);
            }
            if (ri(200) == 0) localSound("block.lava.ambient", x, y, z, float(0.2 + R() * 0.2), float(0.9 + R() * 0.15), true);
        }
        if (ri(10) == 0) fluidDrip(id, x, y, z, type("dripping_lava"));
    }
}

// AmbientDesertBlockSoundsPlayer.shouldPlayDesertDryVegetationBlockSounds: two such blocks under the plant.
bool Particles::desertVegetationBelow(int x, int y, int z) {
    return tagged("minecraft:triggers_ambient_desert_dry_vegetation_block_sounds", blockAt(x, y - 1, z)) &&
           tagged("minecraft:triggers_ambient_desert_dry_vegetation_block_sounds", blockAt(x, y - 2, z));
}

// AmbientDesertBlockSoundsPlayer.shouldPlayAmbientSandSound: sand surfaces 8 blocks away on 3 of the 4 sides.
bool Particles::desertSandAround(int x, int y, int z) {
    int found = 0, checked = 0;
    for (int d = 2; d < 6; ++d) {
        const int px = x + kStep[d][0] * 8, pz = z + kStep[d][2] * 8;
        bool hit = false;
        const int surface = surfaceY(px, pz, false) - 1;
        if (std::abs(surface - y) > 5) {
            u16 above = blockAt(px, y + 6, pz);
            for (int i = 0; i < 10 && !hit; ++i) {
                const u16 cur = blockAt(px, y + 5 - i, pz);
                if (above == 0 && tagged("minecraft:triggers_ambient_desert_sand_block_sounds", cur)) hit = true;
                above = cur;
            }
        } else {
            hit = blockAt(px, surface + 1, pz) == 0 && tagged("minecraft:triggers_ambient_desert_sand_block_sounds", blockAt(px, surface, pz));
        }
        if (hit && found++ >= 3) return true;
        ++checked;
        if (4 - checked + found < 3) return false;
    }
    return false;
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
    int soundAt[3] = {};
    bool haveSound = false;
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
            soundAt[0] = x, soundAt[1] = y, soundAt[2] = z, haveSound = true;
            break;
        }
    }
    // ClientLevel.tickRain: now and then the rain sound at the last drop (muffled when it falls on a roof above).
    if (haveSound && int(rng_() % 3) < rainSoundTime_++) {
        rainSoundTime_ = 0;
        const bool above = soundAt[1] > cy + 1 && surfaceY(cx, cz, true) > cy;
        if (audio::AudioSystem* a = audio::instance()) {
            const double pos[3] = {soundAt[0] + 0.5, soundAt[1] + 0.5, soundAt[2] + 0.5};
            a->playNetwork(-1, above ? "minecraft:weather.rain.above" : "minecraft:weather.rain", audio::Category::Weather, pos,
                           above ? 0.1f : 0.2f, above ? 0.5f : 1.0f);
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
        else if (type == 8) thunder_ = std::clamp(value, 0.0f, 1.0f);  // THUNDER_LEVEL_CHANGE
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
    tickPotentSulfur();
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
    std::vector<NetLevelEvent> levelEvents;
    {
        std::lock_guard lock(netMutex_);
        spawns.swap(netSpawns_);
        breaks.swap(netBreaks_);
        levelEvents.swap(netLevelEvents_);
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
    for (const NetLevelEvent& e : levelEvents) {
        if (e.type < 0) globalLevelEvent(-e.type, e.x, e.y, e.z);
        else levelEvent(e.type, e.x, e.y, e.z, e.data);
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
            case SizeReverse: size *= 1.0f - s / 1.5f; break;
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
