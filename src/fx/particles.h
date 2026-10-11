#pragma once
// Particles: the game's common particle types simulated on the CPU at 20 Hz (ports of the client particle
// classes: TerrainParticle, WaterDropParticle / SplashParticle, ExplodeParticle, HugeExplosion(Seed)Particle,
// SmokeParticle / LargeSmokeParticle, FlameParticle, CritParticle, HeartParticle, BubbleParticle, FallingDust /
// FallingLeaves, PortalParticle, FlyTowardsPositionParticle (enchant), CampfireSmokeParticle, SpellParticle,
// SuspendedTownParticle, LavaParticle, DustParticle, NoteParticle, ...; other simple types get a generic motion).
// Sprite sets come from assets/minecraft/particles/<type>.json + textures/particle/*, block shards use the block
// model's particle texture. All textures live in the item atlas (entity/item_models.h).
//
// Sources: local block breaks (ClientLevel.addDestroyBlockEffect), server level_particles (types whose options
// cannot be decoded drop the packet), level_event 2001 (others' block breaks), rain splashes
// (ClientLevel.tickWeatherEffects, rain level from game_event) and block animateTick near the camera (torch
// flames / smoke, falling leaves, lava pops, campfire smoke).
//
// Rendering: build() writes camera-relative billboards (shaders/visuals.hlsl ParticleVS) for the visuals pass.
#include "core/common.h"

#include <functional>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>

class World;
class BlockRegistry;
class GameReports;
namespace net {
class Client;
struct Reader;
}
namespace ent {
class ItemModels;
}
namespace ui {
class Resources;
}

namespace fx {

// GPU billboard (shaders/visuals.hlsl struct Particle).
struct GpuParticle {
    float pos[3];
    float size;
    float uv[4];
    u32 color;  // 0xAABBGGRR
    u32 light;  // sky << 4 | block (256: full bright)
    u32 tex;    // ent::kTexItems or a block texture layer
    float roll;
    float motion[3];  // movement since the previous frame (blocks): motion vectors
    // 0: a camera-facing billboard. Else (bit 31 set) a quad turned by rotationY(a).rotateX(b).rotateY(c) instead of
    // the camera, the angles in 10 bits each (a | b << 10 | c << 20, units of 2 pi / 1024): orientedQuad().
    u32 orient;
};
static_assert(sizeof(GpuParticle) == 64);

class Particles {
public:
    Particles(World& world, BlockRegistry& registry, const GameReports& reports, ent::ItemModels& items);
    ~Particles();
    bool init(const ui::Resources& res);
    // level_particles, level_event (2001 block break), game_event (rain); before Client::start.
    void attach(net::Client& client);

    // Main thread, once per frame: applies network spawns, ticks at 20 Hz around the camera.
    void frame(double dt, const double cam[3]);
    // Local actions.
    void blockDestroyed(int x, int y, int z, u16 block);
    // ClientLevel.addBreakingParticles: one shard on the hit face of the block being broken (world lock held).
    void blockCrack(int x, int y, int z, int face);
    // BreakingItemParticle (ParticleTypes.ITEM): a shard of an item's texture (eating, items breaking).
    void itemShard(int item, double x, double y, double z, double xa, double ya, double za);
    void spawn(std::string_view type, double x, double y, double z, double vx, double vy, double vz);

    // Billboards for this frame: opaque ones first (count `opaque`), then translucent ones.
    u32 build(GpuParticle* out, u32 max, const double cam[3], u32& opaque);
    u32 count() const;
    struct Stats {
        double tickMs = 0, buildMs = 0;
        u32 live = 0, spawned = 0;
    };
    const Stats& stats() const { return stats_; }
    void setRainOverride(float level) { rainOverride_ = level; }
    // ItemParticleOption (ItemStackTemplate.STREAM_CODEC): reads the stack and returns its item network id (network
    // thread; unset or false: the packet is dropped).
    std::function<bool(net::Reader& r, int& item)> readItem;
    // EntityPositionSource: an entity's position (main thread); false when unknown (the vibration ends).
    std::function<bool(int entity, double pos[3])> entityPosition;

    struct Particle;  // particles.cpp

private:
    struct Type;
    struct Options {
        int block = 0;        // network block state (block, falling_dust, ...)
        u32 color = 0;        // ARGB (dust, entity_effect, tinted_leaves, effect)
        float scale = 1, power = 1;
        bool hasColor = false;
        u32 color2 = 0;       // dust_color_transition: the colour it fades to (ARGB)
        int item = -1;        // item: network item id
        // vibration: the destination (a block's centre, or entity >= 0 with a y offset) and the travel time
        int entity = -1;
        double target[3] = {};
        float yOffset = 0;
        int arrival = 0;
    };
    void tick();
    void ambient(const double cam[3]);
    void weather(const double cam[3]);
    Particle* add(int type, double x, double y, double z, double xa, double ya, double za, const Options* opt = nullptr);
    void tickOne(Particle& p);
    void move(Particle& p, double xa, double ya, double za);
    u16 blockAt(int x, int y, int z);  // through a small per-frame cache (collisions repeat lookups)
    u8 lightAt(double x, double y, double z) const;
    int typeId(std::string_view name) const;
    u16 localBlock(int networkState);
    // BreakingItemParticle with an item atlas rect: scaled keeps a tenth of the random push plus (xa, ya, za).
    void shard(int rect, double x, double y, double z, double xa, double ya, double za, bool scaled);
    bool vibrationTarget(const Particle& p, double out[3]);
    int blockSprite(u16 block, u32& tint);  // item atlas rect of the block's particle texture
    u32 dustColor(u16 block);
    float randf() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }
    double gauss() { return std::normal_distribution<double>(0.0, 1.0)(rng_); }

    World& world_;
    BlockRegistry& registry_;
    const GameReports& reports_;
    ent::ItemModels& items_;
    std::vector<Type> types_;                  // by particle_type network id
    std::unordered_map<std::string, int> typeIds_;
    std::vector<std::string> blockStates_;     // network block state -> key
    std::unordered_map<int, u16> blockCache_;
    std::vector<i32> blockSprites_;            // registry id -> rect + 1 (0 unresolved, -1 none)
    std::vector<u32> blockTints_;
    std::vector<u8> blockKinds_;               // registry id -> ambient kind + 1 (0 unresolved)
    std::unordered_map<int, int> itemSprites_;  // item id -> item atlas rect + 1 (-1 none)
    struct CachedBlock {
        i32 x = 0, y = INT32_MIN, z = 0;
        u16 id = 0;
    };
    CachedBlock blockCache2_[1024];
    std::vector<Particle> live_;
    std::vector<GpuParticle> scratch_;         // build(): CPU copy, written to the upload heap in one pass
    std::vector<Particle> spawnQueue_;         // spawned while ticking
    std::mt19937 rng_{0x51A7E5u};
    double tickClock_ = 0, cam_[3] = {};
    u64 ticks_ = 0;
    float rain_ = 0, rainTarget_ = 0, rainOverride_ = -1;
    std::unordered_set<u64> campfires_;        // lit campfires found by the ambient sampler
    float partial_ = 0;
    float frameTicks_ = 0;  // last frame's length in ticks (particle motion vectors)

    // Network (decoded on the network thread).
    struct NetSpawn {
        int type;
        Options opt;
        bool alwaysShow;
        double x, y, z;
        float dx, dy, dz, sx, sy, sz;
        int count, mode;
    };
    struct NetBreak {
        int x, y, z, state;
        int face = -1;  // level event 2019 / 2020 (destroy progress): the hit face; -1: 2001 (block destroyed)
    };
    std::mutex netMutex_;
    std::vector<NetSpawn> netSpawns_, spawnsLocal_;  // network thread / main thread (swapped, capacity kept)
    std::vector<NetBreak> netBreaks_, breaksLocal_;
    std::vector<std::pair<int, float>> netWeather_;
    struct BlockParticleInfo {  // ExplosionParticleInfo with its WeightedList weight
        int type;
        Options opt;
        float scaling, speed;
        int weight;
    };
    struct NetExplosion {  // ClientboundExplodePacket
        double pos[3];
        float radius;
        int blockCount;
        int particle;
        int soundId;  // registry id, -1: inline name
        std::string soundName;
        bool playSound;
        std::vector<BlockParticleInfo> blockParticles;
    };
    std::vector<NetExplosion> netExplosions_;
    std::vector<NetExplosion> trackedExplosions_;  // ClientExplosionTracker: this tick's explosions (main thread)
    void tickExplosions();
    bool skipOptions(void* reader, int type) const;  // net::Reader*: the particle's options (false: unreadable)
    bool readOptions(void* reader, int type, Options& out) const;
    std::vector<std::pair<int, float>> weatherQueue_;  // main thread
    const ui::Resources* res_ = nullptr;
    Stats stats_;
    static constexpr u32 kMaxParticles = 16384;
};

}  // namespace fx
