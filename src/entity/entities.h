#pragma once
// Client-side entity state: decodes the server's entity packets (network thread -> queue), applies them on
// the main thread, interpolates positions/rotations the way the game's client does (stepped interpolation over
// the type's update interval), and advances the per-tick animation state (walk animation, body/head turn,
// hurt/death/swing timers). Rendering lives in entity_render.*; picking for attack/use is here.
#include "core/common.h"
#include "entity/entity_assets.h"

#include <mutex>
#include <unordered_map>

class World;
class BlockRegistry;
class GameReports;
namespace net {
class Client;
}

namespace ent {

// One synched-data value (set_entity_data). Only what the renderer needs is kept.
struct MetaValue {
    u8 index = 0, serializer = 0;
    i64 i = 0;          // byte / int / long / bool / enum / holder id / item id (-1 = empty) / block state
    float f[4] = {};    // float, rotations, vector, quaternion; f[0] = item count for item stacks
    i32 str = -1;       // index into the event string pool (names)
};

enum class EventType : u8 {
    Add, Remove, Move, PositionSync, Teleport, Motion, HeadRot, Data, Equipment, PlayerInfo, PlayerRemove, Swing,
    Hurt, Damage, EntityEvent, TakeItem, Passengers
};

struct Step {
    double pos[3];
    i32 ticks;  // tick offset of a stepped path; -1 = use the type's interpolation length
    i16 delta[3];
};

// Decoded packet. Variable-size payloads live in the queue's pools (first/count).
struct Event {
    EventType type{};
    i32 id = 0;
    i32 a = 0, b = 0;        // add: type, data | move flags | slot ...
    double d[6] = {};        // positions, velocity
    float f[3] = {};         // yaw, pitch, head yaw (degrees)
    bool flag = false;       // on ground / has position / ...
    bool hasRot = false;
    u8 uuid[16] = {};
    u32 first = 0, count = 0;  // pool range (steps, meta values, ids)
};

struct EventQueue {
    std::vector<Event> events;
    std::vector<Step> steps;
    std::vector<MetaValue> meta;
    std::vector<i32> ints;
    std::vector<std::string> strings;
    void clear() {
        events.clear(), steps.clear(), meta.clear(), ints.clear();
        strings.clear();
    }
};

enum class Category : u8 { Living, Player, Item, Block, Vehicle, Projectile, Other };

struct Entity {
    i32 id = 0, type = -1;
    u8 uuid[16] = {};
    const TypeInfo* info = nullptr;
    Category category = Category::Other;
    double pos[3] = {}, prevPos[3] = {};
    double codecBase[3] = {};  // VecDeltaCodec base (last position the server sent)
    double vel[3] = {};
    float yaw = 0, pitch = 0, prevYaw = 0, prevPitch = 0;              // entity yRot / xRot (degrees)
    float headYaw = 0, prevHeadYaw = 0, bodyYaw = 0, prevBodyYaw = 0;  // living entities
    float lerpHeadYaw = 0;
    int lerpHeadSteps = 0;
    bool onGround = false;

    // Stepped interpolation (SteppedInterpolationHandler); linear handlers use one step per target.
    struct IStep {
        double pos[3];
        float yaw, pitch;
        int ticks;
    };
    static constexpr int kMaxSteps = 16;
    IStep steps[kMaxSteps];
    int stepHead = 0, stepCount = 0;
    IStep lastStep{};
    float currentStepTicks = 0, remainingTicks = 0, interpSpeed = 1;
    int linearRemaining = 0;  // LinearInterpolationHandler
    double linearTarget[3] = {};
    float linearYaw = 0, linearPitch = 0;

    // Walk animation (WalkAnimationState).
    float walkSpeedOld = 0, walkSpeed = 0, walkPos = 0, walkScale = 1;

    // Synched data the renderer uses.
    u8 sharedFlags = 0;  // 0x02 crouching, 0x20 invisible, ...
    u8 mobFlags = 0;     // Mob: 1 no AI, 2 left handed, 4 aggressive
    bool noGravity = false, inGround = false;
    int pose = 0;
    bool baby = false;
    float health = 1;
    int variant = 0, variant2 = 0;  // registry/enum variant; secondary (markings, hidden gene, profession...)
    int villagerType = 0;
    int wool = 0;                   // sheep: color | 0x10 sheared
    int collar = 14;                // wolf/cat collar dye colour (red by default)
    bool tame = false;
    bool angry = false;
    int itemId = -1, itemCount = 1;
    int blockState = 0;             // falling block: network block state id
    int size = 1;                   // slime size
    int swellDir = -1, swell = 0, oldSwell = 0;
    bool powered = false;
    int skinParts = 0, mainArm = 1;  // Avatar defaults: no outer layers until set, right handed
    int standFlags = 0;  // armor stand: 1 small, 4 arms, 8 no base plate
    float standPose[6][3] = {{0, 0, 0}, {0, 0, 0}, {-10, 0, -10}, {-15, 0, 10}, {-1, 0, -1}, {1, 0, 1}};
    int vehicleHurt = 0, vehicleHurtDir = 1;
    float vehicleDamage = 0;
    std::string name;  // custom name (or player name)
    int equipment[8] = {-1, -1, -1, -1, -1, -1, -1, -1};  // item ids by EquipmentSlot

    // Animation timers (ticks).
    int age = 0, hurtTime = 0, deathTime = 0, swingTime = -1, swingDuration = 6;
    float flap = 0, oFlap = 0, flapSpeed = 0, oFlapSpeed = 0, flapping = 1;
    float bobOffset = 0;
    int vehicle = -1;
    bool dying = false;
    int unloadedTicks = 0;  // ticks spent outside the loaded world
    // Player skin (default skins chosen from the UUID like the game).
    bool slim = false;
    int skin = -1;
};

struct PlayerInfo {
    std::string name;
    int skin = -1;
    bool slim = false;
};

// Block hit distance for picking: entities further than this are behind the targeted block.
struct PickResult {
    int id = -1;
    double distance = 0;
    double hit[3] = {};
};

class EntityWorld {
public:
    EntityWorld(EntityAssets& assets, World& world, BlockRegistry& registry, const GameReports& reports);

    // Registers the play-packet handlers; call right after creating each net::Client, before start().
    void attach(net::Client& client);
    // Main thread, once per frame: applies queued packets and advances 20 Hz ticks.
    void frame(double dt);
    void clear();

    // Ray pick against entity bounding boxes (inflated by the target's pick radius); origin/dir in world space.
    PickResult pick(const double origin[3], const double dir[3], double maxDistance) const;
    // Sends attack (attack + punch) or use (interact) for an entity; false if not connected.
    bool attack(int id);
    bool use(int id, const double hit[3], bool sneaking);

    const std::unordered_map<int, Entity>& entities() const { return entities_; }
    float partialTick() const { return float(tickAccumulator_ / 0.05); }
    const EntityAssets& assets() const { return assets_; }
    World& world() const { return world_; }
    BlockRegistry& registry() const { return registry_; }
    u16 localBlock(int networkState);  // network block state id -> registry id (main thread)
    const GameReports& reports() const { return reports_; }
    const std::string& villagerType(int id) const { return name(villagerTypes_, id); }
    const std::string& profession(int id) const { return name(professions_, id); }
    // Left / right click with an entity under the crosshair (before blocks): true when an entity closer than
    // blockDistance (< 0: no block targeted) is targeted; it then takes the click (attack / use when pressed).
    bool click(const double eye[3], const double dir[3], bool attack, bool use, double blockDistance, double reach);
    struct Stats {
        double applyMs = 0, tickMs = 0;
        size_t events = 0;
    };
    const Stats& stats() const { return stats_; }

    // Per-type synched-data ids used by the decoder (resolved once from the dumped field names).
    struct DataIds {
        i8 flags = 0, pose = 6, customName = 2, health = -1, baby = -1, wool = -1, variant = -1, item = -1, size = -1,
           swellDir = -1, powered = -1, mainHand = -1, skinParts = -1, horseVariant = -1, villagerData = -1,
           tameFlags = -1, collar = -1, anger = -1, standFlags = -1, standPose = -1, hurt = -1, hurtDir = -1,
           damage = -1, mainGene = -1, hiddenGene = -1, mobFlags = -1, noGravity = 5, inGround = -1;
    };

private:
    void apply(const Event& e, const EventQueue& q);
    void applyData(Entity& e, const MetaValue* values, u32 count, const EventQueue& q);
    void tick();
    void tickEntity(Entity& e);
    void startInterpolation(Entity& e, const Step* steps, u32 count, float yaw, float pitch, bool hasRot);
    Category categoryFor(const std::string& name) const;
    static const std::string& name(const std::vector<std::string>& v, int id) {
        static const std::string kNone;
        return id >= 0 && size_t(id) < v.size() ? v[size_t(id)] : kNone;
    }

    EntityAssets& assets_;
    World& world_;
    BlockRegistry& registry_;
    const GameReports& reports_;
    net::Client* client_ = nullptr;

    std::mutex mutex_;
    EventQueue incoming_, processing_;  // network thread appends to incoming_; swapped each frame

    std::unordered_map<int, Entity> entities_;
    std::unordered_map<std::string, PlayerInfo> players_;  // by UUID bytes
    std::vector<DataIds> dataIds_;  // by entity type
    std::vector<Category> categories_;
    std::vector<int> defaultVariant_;  // by entity type
    std::vector<std::string> blockStates_;  // network block state id -> full state key
    std::unordered_map<int, u16> blockCache_;
    std::vector<std::string> villagerTypes_, professions_;
    int entityEffectParticle_ = -1;
    double tickAccumulator_ = 0;
    u64 ticks_ = 0;
    Stats stats_;
    bool verbose_ = false;  // MCPP_ENTITY_VERBOSE=1: log spawns, variants, attacks
};

// Default player skin like DefaultPlayerSkin.get(uuid): floorMod(uuid.hashCode(), 18).
int defaultSkinIndex(const u8 uuid[16]);

}  // namespace ent
