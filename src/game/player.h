#pragma once
#include "core/common.h"

#include <functional>

class World;
class BlockRegistry;

struct AABB {
    double min[3], max[3];
    bool intersects(const AABB& o) const {
        for (int a = 0; a < 3; ++a)
            if (max[a] <= o.min[a] || min[a] >= o.max[a]) return false;
        return true;
    }
};

struct RayHit {
    bool hit = false;
    int x = 0, y = 0, z = 0;  // block
    int face = 0;              // 0 down 1 up 2 north 3 south 4 west 5 east
    double distance = 0;
    double point[3] = {};
};

// Collision and picking queries against block shapes.
class Physics {
public:
    Physics(const World& world, const BlockRegistry& registry) : world_(world), registry_(registry) {}
    void collisionBoxes(const AABB& area, std::vector<AABB>& out) const;
    RayHit raycast(const double origin[3], const double dir[3], double maxDistance) const;
    bool inFluid(const AABB& box) const;
    bool inLava(const AABB& box) const;
    bool containsLiquid(const AABB& box) const;  // any cell the box touches holds a fluid
    bool noCollision(const AABB& box) const;
    float slipperiness(int x, int y, int z) const;
    bool climbable(int x, int y, int z) const;  // #minecraft:climbable (ladders, vines, scaffolding)
    float speedFactor(int x, int y, int z) const;  // Block.getSpeedFactor (soul sand, honey)
    float jumpFactor(int x, int y, int z) const;   // Block.getJumpFactor (honey)
    // Block.entityInside slowdowns (cobweb, sweet berry bush): the stuck multiplier of a box, 0 when none.
    bool stuckMultiplier(const AABB& box, double out[3]) const;
    // The moving player for context-dependent collision shapes (CollisionContext.of(entity)): scaffolding is solid
    // only from above and not while sneaking down; powder snow carries leather boots from above and catches falls.
    struct EntityContext {
        bool set = false;
        double feetY = 0, fallDistance = 0;
        bool descending = false, powderSnowWalker = false;
    };
    mutable EntityContext context;
    const World& world() const { return world_; }
    const BlockRegistry& registry() const { return registry_; }

private:
    const World& world_;
    const BlockRegistry& registry_;
    mutable std::vector<u8> special_;  // registry id -> 1 none, 2 scaffolding, 3 powder snow (0 unresolved)
    u8 specialKind(u16 id) const;
};

struct PlayerInput {
    float forward = 0, strafe = 0;  // -1..1 (strafe: +1 = left, like the game)
    bool jump = false, sneak = false, sprint = false;
};

// Vanilla player movement at 20 ticks per second.
class Player {
public:
    static constexpr double kWidth = 0.6, kHeight = 1.8, kEyeHeight = 1.62, kSneakEyeHeight = 1.27;
    // Pose dimensions (Avatar.POSES): standing 0.6 x 1.8 (eye 1.62), crouching 1.5 (1.27), swimming / crawling and
    // gliding 0.6 (0.4).
    enum class Pose : u8 { Standing, Crouching, Swimming, FallFlying };

    double pos[3] = {};   // feet, centre
    double prev[3] = {};  // position at the previous tick (for interpolation)
    double vel[3] = {};
    float yaw = 0, pitch = 0;  // radians
    bool onGround = false, flying = true, sneaking = false, inWater = false, inLava = false;
    bool sprinting = false;          // LocalPlayer sprint state (started by the sprint key, ends like the game)
    bool canSprint = true;           // food > 6 (or may fly), set from the server's health updates
    bool mayFly = true;              // Abilities.mayfly (double-tapping jump toggles flight only then)
    bool noClip = false;             // spectator: flies through blocks
    bool horizontalCollision = false;
    PlayerInput lastInput;           // this tick's raw input (sent to the server)
    float useSpeed = 1.0f;           // LocalPlayer.modifyInput: the used item's speed multiplier (1: not using)
    bool useBlocksSprint = false;    // isSlowDueToUsingItem: sprinting cannot start
    Pose pose = Pose::Standing;
    bool swimming = false;           // Entity.isSwimming (sprint-swimming)
    bool fallFlying = false;         // gliding with an elytra (shared flag 7)
    double fallDistance = 0;
    // Server-driven values the movement uses (attributes, effects, equipment); the gameplay layer keeps them.
    struct Attributes {
        double moveSpeed = 0.1, jumpStrength = 0.42, gravity = 0.08, stepHeight = 0.6, sneakSpeed = 0.3;
        double waterEfficiency = 0;  // water_movement_efficiency (Depth Strider)
        int jumpBoost = -1, slowFalling = -1, levitation = -1, dolphinsGrace = -1;  // effect amplifiers, -1 none
        bool canGlide = false;          // a usable glider (elytra) worn
        bool powderSnowWalker = false;  // leather boots
        bool rocketBoost = false;       // a firework rocket is attached (FireworkRocketEntity pushing a glider)
    } attr;
    std::function<void()> onStartFallFlying;
    bool autoJump = false;  // the Auto-Jump option (LocalPlayer.updateAutoJump)  // LocalPlayer.aiStep: ServerboundPlayerCommandPacket START_FALL_FLYING

    void tick(const Physics& physics, const PlayerInput& input);
    AABB box() const { return boxAt(pos, pose); }
    static AABB boxAt(const double p[3], Pose pose = Pose::Standing) {
        const double h = height(pose);
        return {{p[0] - kWidth / 2, p[1], p[2] - kWidth / 2}, {p[0] + kWidth / 2, p[1] + h, p[2] + kWidth / 2}};
    }
    static double height(Pose p) { return p == Pose::Standing ? kHeight : p == Pose::Crouching ? 1.5 : 0.6; }
    double eyeHeight() const {
        return pose == Pose::Standing ? kEyeHeight : pose == Pose::Crouching ? kSneakEyeHeight : 0.4;
    }

private:
    void updatePose(const Physics& physics);
    void moveRelative(const PlayerInput& input, double speed);
    void move(const Physics& physics, double dx, double dy, double dz);
    double stuck_[3] = {};  // Entity.stuckSpeedMultiplier, applied on the next move
    double moved_[3] = {};  // the last move after collisions
    bool wasJumping_ = false, wasInPowderSnow_ = false;
    bool canFallAtLeast(const Physics& physics, double dx, double dz, double minHeight) const;
    int autoJumpTime_ = 0;
    void updateAutoJump(const Physics& physics, double xa, double za, const PlayerInput& raw, double speed);
};
