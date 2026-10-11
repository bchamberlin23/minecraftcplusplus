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
    Suspended, HappyVillager, Lava, Dust, Note, Cloud, Snowflake
};
enum class Opt : u8 { None, Block, Dust, DustTransition, Spell, Color, Float, VarInt, Int, IntFloat, Trail, Unsupported };
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
    {"lava", Kind::Lava}, {"dust", Kind::Dust}, {"dust_color_transition", Kind::Dust}, {"note", Kind::Note},
    {"cloud", Kind::Cloud}, {"sneeze", Kind::Cloud}, {"snowflake", Kind::Snowflake}};

const std::pair<const char*, Opt> kOptions[] = {
    {"block", Opt::Block}, {"block_marker", Opt::Block}, {"falling_dust", Opt::Block}, {"dust_pillar", Opt::Block},
    {"block_crumble", Opt::Block}, {"dust", Opt::Dust}, {"dust_color_transition", Opt::DustTransition},
    {"effect", Opt::Spell}, {"instant_effect", Opt::Spell}, {"entity_effect", Opt::Color}, {"tinted_leaves", Opt::Color},
    {"flash", Opt::Color}, {"dragon_breath", Opt::Float}, {"sculk_charge", Opt::Float}, {"shriek", Opt::VarInt},
    {"geyser", Opt::Int}, {"geyser_plume", Opt::Int}, {"geyser_base", Opt::IntFloat}, {"geyser_poof", Opt::IntFloat},
    {"trail", Opt::Trail}, {"item", Opt::Unsupported}, {"vibration", Opt::Unsupported}};

constexpr u32 kTints[8] = {0xFFFFFF, 0x91BD59, 0x77AB2F, 0x80A755, 0x619961, 0x3F76E4, 0x208030, 0x8B1A00};

u32 abgrF(float r, float g, float b, float a) {
    auto c = [](float v) { return u32(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
}
}  // namespace

struct Particles::Type {
    std::string name;
    Kind kind = Kind::None;
    Opt opt = Opt::None;
    std::vector<int> sprites;  // item atlas rects
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
        auto argb = [&] { return u32(r.i32v()); };
        switch (t.opt) {
            case Opt::None: break;
            case Opt::Block: s.opt.block = r.varInt(); break;
            case Opt::Dust: s.opt.color = 0xFF000000u | argb(), s.opt.scale = r.f32(), s.opt.hasColor = true; break;
            case Opt::DustTransition: s.opt.color = 0xFF000000u | argb(), argb(), s.opt.scale = r.f32(), s.opt.hasColor = true; break;
            case Opt::Spell: s.opt.color = argb(), s.opt.power = r.f32(), s.opt.hasColor = true; break;
            case Opt::Color: s.opt.color = argb(), s.opt.hasColor = true; break;
            case Opt::Float: r.f32(); break;
            case Opt::VarInt: r.varInt(); break;
            case Opt::Int: r.i32v(); break;
            case Opt::IntFloat: r.i32v(), r.f32(); break;
            case Opt::Trail: r.f64(), r.f64(), r.f64(), r.i32v(), r.varInt(); break;
            case Opt::Unsupported: return;  // item stacks / position sources: the rest of the packet is unreadable
        }
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
        r.f32();   // radius
        r.i32v();  // block count
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
            if (!skipOptions(&r, r.varInt())) return;
            r.f32(), r.f32(), r.varInt();  // scaling, speed, weight
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
    net::Reader& r = *static_cast<net::Reader*>(reader);
    if (type < 0 || size_t(type) >= types_.size()) return false;
    switch (types_[size_t(type)].opt) {
        case Opt::None: break;
        case Opt::Block: r.varInt(); break;
        case Opt::Dust: r.i32v(), r.f32(); break;
        case Opt::DustTransition: r.i32v(), r.i32v(), r.f32(); break;
        case Opt::Spell: r.i32v(), r.f32(); break;
        case Opt::Color: r.i32v(); break;
        case Opt::Float: r.f32(); break;
        case Opt::VarInt: r.varInt(); break;
        case Opt::Int: r.i32v(); break;
        case Opt::IntFloat: r.i32v(), r.f32(); break;
        case Opt::Trail: r.f64(), r.f64(), r.f64(), r.i32v(), r.varInt(); break;
        case Opt::Unsupported: return false;
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
    if (t.sprites.empty() && t.kind != Kind::Terrain && t.kind != Kind::Crumble && t.kind != Kind::DustPillar &&
        t.kind != Kind::ExplosionSeed) {
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
        case Kind::Dust: {  // DustParticleBase
            baseVel(x, y, z, xa, ya, za);
            p.friction = 0.96f;
            p.flags |= SpeedUp;
            p.xd *= 0.1f, p.yd *= 0.1f, p.zd *= 0.1f;
            const float scale = opt ? std::clamp(opt->scale, 0.01f, 4.0f) : 1.0f;
            const float br = randf() * 0.4f + 0.6f;
            if (opt && opt->hasColor) color(opt->color);
            p.r *= br, p.g *= br, p.b *= br;
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
            ageSprite();
            break;
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
    static int itemType = -2;  // ticked like terrain shards (same motion as BreakingItemParticle)
    if (itemType == -2) itemType = typeId("block");
    if (live_.size() + spawnQueue_.size() >= kMaxParticles) return;
    int& sprite = itemSprites_[item];  // 0 unresolved, -1 none, else rect + 1
    if (sprite == 0) {
        const std::string tex = items_.itemParticleTexture(item);
        const int rect = tex.empty() ? -1 : items_.texture(tex);
        sprite = rect >= 0 ? rect + 1 : -1;
    }
    if (sprite < 0) return;
    Particle p{};
    p.type = u16(std::max(itemType, 0));
    p.a = 1, p.r = p.g = p.b = 1;
    p.flags = Physics;
    p.x = p.xo = x, p.y = p.yo = y, p.z = p.zo = z;
    p.lifetime = int(4.0f / (randf() * 0.9f + 0.1f));
    // Particle(level, x, y, z, 0, 0, 0): a random push, then BreakingItemParticle keeps a tenth of it plus (xa, ya, za).
    p.xd = (randf() * 2.0f - 1.0f) * 0.4f, p.yd = (randf() * 2.0f - 1.0f) * 0.4f, p.zd = (randf() * 2.0f - 1.0f) * 0.4f;
    const double speed = (randf() + randf() + 1.0f) * 0.15f;
    const double dd = std::max(std::sqrt(p.xd * p.xd + p.yd * p.yd + p.zd * p.zd), 1e-9);
    p.xd = p.xd / dd * speed * 0.4f, p.yd = p.yd / dd * speed * 0.4f + 0.1f, p.zd = p.zd / dd * speed * 0.4f;
    p.xd = p.xd * 0.1f + xa, p.yd = p.yd * 0.1f + ya, p.zd = p.zd * 0.1f + za;
    p.quadSize = 0.1f * (randf() * 0.5f + 0.5f) * 2.0f / 2.0f;
    p.gravity = 1.0f, p.friction = 0.98f;
    p.sprite = sprite - 1;
    p.uo = randf() * 3.0f, p.vo = randf() * 3.0f;
    if (items_.rect(p.sprite).translucent) p.flags |= Translucent;
    p.light = lightAt(x, y, z);
    spawnQueue_.push_back(p);
    ++stats_.spawned;
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
    static int ids[8] = {-2};
    if (ids[0] == -2) {
        const char* names[8] = {"flame", "smoke", "soul_fire_flame", "tinted_leaves", "cherry_leaves", "pale_oak_leaves",
                                "lava", "campfire_cosy_smoke"};
        for (int i = 0; i < 8; ++i) ids[i] = typeId(names[i]);
    }
    enum Amb : u8 { AmbNone, AmbTorch, AmbWallTorch, AmbSoulTorch, AmbSoulWallTorch, AmbTinted, AmbCherry, AmbPale, AmbLava,
                    AmbCampfire, AmbFurnace };
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

void Particles::tick() {
    ++ticks_;
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
        g.size = size;
        const ent::ItemModels::Rect& r = items_.rect(p.sprite);
        if (p.uo >= 0) {  // TerrainParticle: a quarter of the sprite, mirrored
            g.uv[0] = r.u0 + r.du * (p.uo + 1.0f) / 4.0f, g.uv[2] = r.u0 + r.du * p.uo / 4.0f;
            g.uv[1] = r.v0 + r.dv * p.vo / 4.0f, g.uv[3] = r.v0 + r.dv * (p.vo + 1.0f) / 4.0f;
        } else {
            g.uv[0] = r.u0, g.uv[1] = r.v0, g.uv[2] = r.u0 + r.du, g.uv[3] = r.v0 + r.dv;
        }
        g.color = abgrF(p.r, p.g, p.b, p.a);
        u32 light = p.light;
        if (p.emissive == 255) light = 256;
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
        g.motion[2] = float(p.z - p.zo) * frameTicks_, g.pad = 0;
    }
    opaque = n;
    const u32 tcount = max - back;
    std::memcpy(out, buf, size_t(n) * sizeof(GpuParticle));
    std::memcpy(out + n, buf + back, size_t(tcount) * sizeof(GpuParticle));
    stats_.buildMs = t.ms();
    return n + tcount;
}

}  // namespace fx
