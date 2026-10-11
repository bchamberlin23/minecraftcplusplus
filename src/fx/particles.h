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

#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>

class World;
class BlockRegistry;
class GameReports;
namespace net {
class Client;
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
};
static_assert(sizeof(GpuParticle) == 48);

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

    struct Particle;  // particles.cpp

private:
    struct Type;
    struct Options {
        int block = 0;        // network block state (block, falling_dust, ...)
        u32 color = 0;        // ARGB (dust, entity_effect, tinted_leaves, effect)
        float scale = 1, power = 1;
        bool hasColor = false;
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
    };
    std::mutex netMutex_;
    std::vector<NetSpawn> netSpawns_, spawnsLocal_;  // network thread / main thread (swapped, capacity kept)
    std::vector<NetBreak> netBreaks_, breaksLocal_;
    std::vector<std::pair<int, float>> netWeather_;
    std::vector<std::pair<int, float>> weatherQueue_;  // main thread
    const ui::Resources* res_ = nullptr;
    Stats stats_;
    static constexpr u32 kMaxParticles = 16384;
};

}  // namespace fx
