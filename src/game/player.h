#pragma once
#include "core/common.h"
#include "game/world_border.h"

#include <algorithm>
#include <functional>

class World;
class BlockRegistry;
class ItemRules;

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
    bool inside = false;       // BlockHitResult.isInside: the ray started inside the block's shape
};

// Collision and picking queries against block shapes.
class Physics {
public:
    Physics(const World& world, const BlockRegistry& registry) : world_(world), registry_(registry) {}
    void collisionBoxes(const AABB& area, std::vector<AABB>& out) const;
    // Entity.collideWithShapes: moves box `b` by `in` through `boxes` (Y first, then the larger horizontal axis).
    static void collideBoxes(const std::vector<AABB>& boxes, AABB& b, double out[3], const double in[3]);
    // Block hit along a ray against the outline shapes (ClipContext.Block.OUTLINE), or with `visual` against the
    // visual shapes (VISUAL: the third-person camera), or with `collider` against the collision shapes (COLLIDER:
    // projectiles).
    RayHit raycast(const double origin[3], const double dir[3], double maxDistance, bool visual = false, bool collider = false) const;
    bool inFluid(const AABB& box) const;
    bool inLava(const AABB& box) const;
    bool containsLiquid(const AABB& box) const;  // any cell the box touches holds a fluid
    bool noCollision(const AABB& box) const;
    float slipperiness(int x, int y, int z) const;
    bool climbable(int x, int y, int z) const;  // #minecraft:climbable (ladders, vines, scaffolding)
    float speedFactor(int x, int y, int z) const;  // Block.getSpeedFactor (soul sand, honey)
    float jumpFactor(int x, int y, int z) const;   // Block.getJumpFactor (honey)
    float bounceRestitution(int x, int y, int z) const;  // Block.getBounceRestitution (slime 1, beds 0.75)
    bool suppressesBounce(int x, int y, int z) const;    // #minecraft:suppresses_bounce (honey)
    bool isSuffocating(int x, int y, int z) const;       // BlockState.isSuffocating (full solid blocks)
    bool canGlideThrough(int x, int y, int z) const;  // #minecraft:can_glide_through (vines)
    // Level.findSupportingBlock: the colliding cell in the area closest to pos (by its centre).
    bool supportingBlock(const AABB& area, const double pos[3], int out[3]) const;
    // Block.entityInside slowdowns (cobweb, sweet berry bush): the stuck multiplier of a box, 0 when none.
    // cells: the swept ones (sweptCells); box: where the move ended. weaving: the Weaving effect.
    bool stuckMultiplier(const std::vector<std::array<int, 3>>& cells, const AABB& box, double out[3], bool weaving = false) const;

    // FluidState of a cell (Level.getFluidState): water or lava, amount 1..8 (8: sources and falling fluid).
    struct Fluid {
        u8 type = 0;  // 0 none, 1 water, 2 lava
        u8 amount = 0;
        bool falling = false, source = false;
    };
    Fluid fluid(int x, int y, int z) const;
    double fluidHeight(int x, int y, int z, const Fluid& f) const;           // FlowingFluid.getHeight
    double fluidHeightForCamera(int x, int y, int z, const Fluid& f) const;  // FluidState.getHeightForCamera
    void fluidFlow(int x, int y, int z, const Fluid& f, double out[3]) const;  // FlowingFluid.getFlow (normalized)

    // The moving player for context-dependent collision shapes (CollisionContext.of(entity)): scaffolding is solid
    // only from above and not while sneaking down; powder snow carries leather boots from above and catches falls.
    struct EntityContext {
        bool set = false;
        double feetY = 0, fallDistance = 0;
        bool descending = false, powderSnowWalker = false;
        double x = 0, z = 0;  // the entity's position (the world border collides only near it)
        int pistonNoClip = -1;  // PistonMovingBlockEntity.NOCLIP: a moving block pushing along this direction
    };
    mutable EntityContext context;
    // EntityCollisionContext.isHoldingItem(scaffolding): the outline of scaffolding is a full block (ScaffoldingBlock.getShape).
    bool holdingScaffolding = false;
    // The level's world border: with an entity context, Entity.collectColliders adds its collision shape (everything
    // outside the border's block-aligned box) when the entity is close to it (WorldBorder.isInsideCloseToBorder).
    const WorldBorder* border = nullptr;
    // Block tags (climbable, blocks_fluid_flow, suppresses_bounce) from the game's data; names without it.
    const ItemRules* rules = nullptr;
    // With an entity context: entities the player collides with (Level.getEntityCollisions: boats, shulkers, happy
    // ghasts from above) near an area.
    std::function<void(const AABB& area, std::vector<AABB>& out)> entityColliders;

    // PistonMovingBlockEntity on the client (the block entity layer keeps them): the moving_piston cell, the moved
    // state, the piston head states the source piston's shapes use, its facing and kind. progress / progressO advance
    // 0.5 per player tick here (Player::tickPistons).
    struct Piston {
        int x = 0, y = 0, z = 0;
        u16 moved = 0;
        u16 head[2] = {};     // the source piston's head facing `dir`: short=false, short=true (default type)
        u16 headMoved[2] = {};  // the head of a retracting source piston base (its own facing and type): long, short
        u16 extendedBase = 0;  // a retracting source piston base with extended=true
        u8 dir = 0;            // facing (0 down 1 up 2 north 3 south 4 west 5 east)
        bool extending = false, source = false;
        u64 key = 0;           // identity across ticks
        float progress = 0, progressO = 0;
    };
    std::vector<Piston> pistons;
    void pistonShape(const Piston& p, std::vector<AABB>& out) const;  // getCollisionShape (with NOCLIP)
    const World& world() const { return world_; }
    const BlockRegistry& registry() const { return registry_; }

private:
    const World& world_;
    const BlockRegistry& registry_;
    mutable std::vector<u8> visual_;   // registry id -> getVisualShape: 1 collision, 2 outline, 3 none, 4 full block
    mutable std::vector<u8> special_;  // registry id -> 1 none, 2 scaffolding, 3 powder snow (0 unresolved)
    u8 specialKind(u16 id) const;
    mutable std::vector<u16> fluids_;  // registry id -> 0x8000 | type << 8 | source 0x40 | falling 0x20 | amount
    mutable std::vector<u8> tags_;     // registry id -> 1 | climbable 2 | blocks_fluid_flow 4 | suppresses_bounce 8
    u8 tagBits(u16 id) const;
    bool solidFace(int x, int y, int z, int dir, u8 fluidType) const;  // FlowingFluid.isSolidFace
    void addBoxes(u16 state, double x, double y, double z, const AABB& area, std::vector<AABB>& out) const;
    void cellBoxes(int x, int y, int z, const AABB& area, std::vector<AABB>& out) const;
};

struct PlayerInput {
    float forward = 0, strafe = 0;  // -1..1 (strafe: +1 = left, like the game)
    bool jump = false, sneak = false, sprint = false;
    bool backward() const { return forward < 0; }
};

// Vanilla player movement at 20 ticks per second.
class Player {
public:
    static constexpr double kWidth = 0.6, kHeight = 1.8, kEyeHeight = 1.62, kSneakEyeHeight = 1.27;
    // Pose dimensions (Avatar.POSES): standing 0.6 x 1.8 (eye 1.62), crouching 1.5 (1.27), swimming / crawling,
    // gliding and the riptide spin 0.6 (0.4).
    enum class Pose : u8 { Standing, Crouching, Swimming, FallFlying, SpinAttack };

    double pos[3] = {};   // feet, centre
    double prev[3] = {};  // position at the previous tick (for interpolation)
    double vel[3] = {};
    float yaw = 0, pitch = 0;  // radians
    bool onGround = false, flying = true, sneaking = false, inWater = false, inLava = false;
    bool sprinting = false;          // LocalPlayer sprint state (sprint key or a double-tapped forward key)
    bool canSprint = true;           // Player.hasEnoughFoodToSprint: foodSprint or may fly
    bool foodSprint = true;          // food > 6 (the server's health updates)
    bool mayFly = true;              // Abilities.mayfly (double-tapping jump toggles flight only then)
    bool asleep = false;             // isSleeping (Game::sleeping): no forced crouch under a low ceiling
    int portalCooldown = 0;          // Entity.portalCooldown (counted down each tick, the spawn info sets it)
    float flyingSpeed = 0.05f;       // Abilities flying / walking speed (the server's player_abilities)
    float walkingSpeed = 0.1f;
    bool noClip = false;             // spectator: flies through blocks
    bool horizontalCollision = false;
    PlayerInput lastInput;           // this tick's raw input (sent to the server)
    float useSpeed = 1.0f;           // LocalPlayer.modifyInput: the used item's speed multiplier (1: not using)
    bool useBlocksSprint = false;    // isSlowDueToUsingItem: sprinting cannot start
    Pose pose = Pose::Standing;
    bool swimming = false;           // Entity.isSwimming (sprint-swimming)
    bool fallFlying = false;         // gliding with an elytra (shared flag 7)
    double fallDistance = 0;
    int tickCount = 0;  // Entity.tickCount: ticks since this player was created (a respawn makes a new one)
    // Fluids (EntityFluidInteraction): the water / lava height over the feet, the eyes in water (this tick and the
    // last one: Entity.wasEyeInWater).
    double waterHeight = 0, lavaHeight = 0;
    bool eyeInWater = false, wasEyeInWater = false;
    bool fastLava = false;           // the dimension's FAST_LAVA (the nether): lava currents push harder
    bool waterEvaporates = false;    // the dimension's WATER_EVAPORATES (the nether): water buckets fizzle
    int sprintWindow = 7;            // the Sprint Window option (double-tap forward to sprint; 0 off)
    // Riptide (LivingEntity.autoSpinAttackTicks): the client starts it when a riptide trident is released;
    // spinFlag: the server's living-entity flag 4 (the SPIN_ATTACK pose follows it).
    int autoSpinAttackTicks = 0;
    bool spinFlag = false;
    // Server-driven values the movement uses (attributes, effects, equipment); the gameplay layer keeps them.
    struct Attributes {
        double moveSpeed = 0.1, jumpStrength = 0.42, gravity = 0.08, stepHeight = 0.6, sneakSpeed = 0.3;
        double waterEfficiency = 0;  // water_movement_efficiency (Depth Strider)
        double movementEfficiency = 0;  // movement_efficiency (Soul Speed): the block speed factor towards 1
        double frictionModifier = 1, airDragModifier = 1;  // friction_modifier / air_drag_modifier
        double bounciness = 0;  // the bounciness attribute (LivingEntity.getEntityBounciness): bounces off anything
        double safeFallDistance = 3, fallDamageMultiplier = 1;  // safe_fall_distance / fall_damage_multiplier
        int jumpBoost = -1, slowFalling = -1, levitation = -1, dolphinsGrace = -1;  // effect amplifiers, -1 none
        bool blind = false;             // Blindness (LivingEntity.isMobilityRestricted: no sprinting)
        bool weaving = false;           // Weaving (WebBlock.entityInside: cobwebs slow less)
        bool canGlide = false;          // a usable glider (elytra) worn
        bool powderSnowWalker = false;  // leather boots
        bool rocketBoost = false;       // a firework rocket is attached (FireworkRocketEntity pushing a glider)
    } attr;
    // Entity.checkFallDamage on landing: the fall distance the ground took (for the fall sounds), counted per landing.
    double landedFall = 0;
    bool slidingOnHoney = false;  // HoneyBlock.isSlidingDown this tick
    int landings = 0;
    std::function<void()> onStartFallFlying;  // LocalPlayer.aiStep: ServerboundPlayerCommandPacket START_FALL_FLYING
    // Entities touching the player while it spins (LivingEntity.checkAutoSpinAttack: the spin hits the first living
    // one and bounces back): true when a living entity's box intersects the swept box.
    std::function<bool(const AABB& swept)> spinAttackHits;
    bool autoJump = false;  // the Auto-Jump option (LocalPlayer.updateAutoJump)

    void tick(const Physics& physics, const PlayerInput& input);
    // PistonMovingBlockEntity.tick for the moving blocks around the player: moveCollidedEntities (pushes, slime
    // blocks launch), moveStuckEntities (honey drags along), then the progress; before the player's own tick.
    void tickPistons(Physics& physics);
    // Entity.move(MoverType.SHULKER_BOX): an opening shulker box's lid moves the player (with collisions)
    void moveByShulkerBox(const Physics& physics, double dx, double dy, double dz) { move(physics, dx, dy, dz); }
    // Riptide: TridentItem.releaseUsing on the client (push along the look, spin for 20 ticks, a hop off the ground).
    void riptide(const Physics& physics, float strength);
    // LivingEntity.getDimensions: the pose's dimensions scaled by getScale (the scale attribute).
    double scale = 1.0;
    AABB box() const { return boxAt(pos, pose); }
    AABB boxAt(const double p[3], Pose pose = Pose::Standing) const {
        const double h = height(pose), w = width() / 2;
        return {{p[0] - w, p[1], p[2] - w}, {p[0] + w, p[1] + h, p[2] + w}};
    }
    double width() const { return kWidth * scale; }
    double height(Pose p) const { return (p == Pose::Standing ? kHeight : p == Pose::Crouching ? 1.5 : 0.6) * scale; }
    double eyeHeight() const {
        return (pose == Pose::Standing ? kEyeHeight : pose == Pose::Crouching ? kSneakEyeHeight : 0.4) * scale;
    }
    bool underWater() const { return wasEyeInWater; }  // LocalPlayer.isUnderWater: wasUnderwater (the eyes alone)
    bool onClimbable(const Physics& physics) const;  // LivingEntity.onClimbable

private:
    void updatePose(const Physics& physics);
    void updateFluids(const Physics& physics);  // Entity.updateFluidInteraction
    void moveRelative(const PlayerInput& input, double speed);
    // Entity.move (SELF / PISTON): collisions, step up, bounces, the block speed factor.
    void move(const Physics& physics, double dx, double dy, double dz, bool piston = false);
    void applyEffectsFromBlocks(const Physics& physics);  // stepOn, then the blocks the player is inside
    void moveTowardsClosestSpace(const Physics& physics, double x, double z);
    double stuck_[3] = {};  // Entity.stuckSpeedMultiplier, applied on the next move
    double moved_[3] = {};  // the last move after collisions
    double pistonDeltas_[3] = {};  // Entity.pistonDeltas (this tick)
    bool wasJumping_ = false, wasInPowderSnow_ = false, isInPowderSnow_ = false, wasShift_ = false, hadForwardImpulse_ = false;
    bool minorHorizontalCollision_ = false;
    float xxa_ = 0, zza_ = 0;  // this tick's modified input (LivingEntity.xxa / zza)
    // Entity.mainSupportingBlockPos / onGroundNoBlocks.
    bool hasSupport_ = false, onGroundNoBlocks_ = false;
    int support_[3] = {};
    void onPos(const Physics& physics, double offset, int out[3]) const;  // Entity.getOnPos
    bool fits(const Physics& physics, Pose p) const;  // canPlayerFitWithinBlocksAndEntitiesWhen
    double gravityNow() const;
    double fluidJumpThreshold() const;
    float blockJumpFactor(const Physics& physics) const;
    float blockSpeedFactor(const Physics& physics) const;
    void jumpFromGround(const Physics& physics);
    int noJumpDelay_ = 0, sprintTriggerTime_ = 0, jumpTriggerTime_ = 0;
    bool canFallAtLeast(const Physics& physics, double dx, double dz, double minHeight) const;
    int autoJumpTime_ = 0;
    void updateAutoJump(const Physics& physics, double xa, double za, const PlayerInput& raw, double speed);
    void travel(const Physics& physics, const PlayerInput& input, bool jumping);
    double frictionModified(double friction, double modifier) const {  // LivingEntity.computeModifiedFriction
        return std::clamp(1.0 - (1.0 - friction) * modifier, 0.0, 1.0);
    }
};
