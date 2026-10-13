#pragma once
// Client-side entity state: decodes the server's entity packets (network thread -> queue), applies them on
// the main thread, interpolates positions/rotations the way the game's client does (stepped interpolation over
// the type's update interval), and advances the per-tick animation state (walk animation, body/head turn,
// hurt/death/swing timers). Rendering lives in entity_render.*; picking for attack/use is here.
#include "core/common.h"
#include "entity/entity_assets.h"
#include "entity/species.h"
#include "game/tick_rate.h"

#include <atomic>
#include <climits>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

class World;
class BlockRegistry;
class GameReports;
namespace net {
class Client;
struct Reader;
}
namespace ui {
class Items;
struct ItemStack;
}

namespace ent {

// One synched-data value (set_entity_data). Only what the renderer needs is kept.
struct MetaValue {
    u8 index = 0, serializer = 0;
    i64 i = 0;          // byte / int / long / bool / enum / holder id / item id (-1 = empty) / block state
    float f[4] = {};    // float, rotations, vector, quaternion; f[0] = item count for item stacks
    i32 str = -1;       // index into the event string pool (names)
    i32 stack = -1;     // item stacks: index into the event stack pool (the whole stack with its components)
};

enum class EventType : u8 {
    Add, Remove, Move, PositionSync, Teleport, Motion, HeadRot, Data, Equipment, PlayerInfo, PlayerRemove, Swing,
    Hurt, Damage, EntityEvent, TakeItem, Passengers, Animate, Jukebox, ProjectilePower, EntityLink, TransientBlock,
    MinecartSteps, MobAppearance
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

struct MinecartStep {  // NewMinecartBehavior.MinecartStep
    double pos[3], movement[3];
    float yRot, xRot, weight;
};

struct EventQueue {
    std::vector<Event> events;
    std::vector<MinecartStep> minecartSteps;
    std::vector<Step> steps;
    std::vector<MetaValue> meta;
    std::vector<i32> ints;
    std::vector<std::string> strings;
    std::vector<std::shared_ptr<const ui::ItemStack>> stacks;
    void clear() {
        events.clear(), steps.clear(), meta.clear(), ints.clear(), minecartSteps.clear();
        strings.clear(), stacks.clear();
    }
};

enum class Category : u8 { Living, Player, Item, Block, Vehicle, Projectile, Other };

// Display entities (Display.java: block / item / text display): the synched transformation and render state,
// interpolated on the client like Display.tick / calculateInterpolationProgress. Block displays use
// Entity::blockState, item displays Entity::itemId.
struct DisplayState {
    bool is = false;
    // Transformation: translation, left rotation, scale, right rotation (quaternions x y z w).
    struct Xform {
        float t[3] = {}, l[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1}, r[4] = {0, 0, 0, 1};
    };
    Xform synched, from, to;  // synched data; the render state's TransformationInterpolator (from -> to)
    int startDelta = 0, duration = 0;            // synched interpolation start delay / duration (ticks)
    int startTick = INT_MIN, activeDuration = 0;  // applied at the next tick (updateStartTick / updateInterpolationDuration)
    bool updateStart = false, updateDuration = false, updateRender = false, hasRender = false;
    int posRotDuration = 0;  // teleport_duration: linear position / rotation interpolation steps
    u8 billboard = 0;        // fixed, vertical, horizontal, center
    int brightness = -1;     // packed block << 4 | sky << 20, -1: the world's light
    float viewRange = 1, width = 0, height = 0;
    int itemContext = 0;  // ItemDisplayContext id (0 none)
    // Text display: component NBT, line width, style flags (1 shadow, 2 see through, 4 default background, 8 left,
    // 16 right), background ARGB and text opacity (signed byte): synched and the render state's interpolators.
    std::string text;
    int lineWidth = 200;
    u8 style = 0;
    int background = 0x40000000, opacity = -1;
    int background0 = 0x40000000, background1 = 0x40000000, opacity0 = -1, opacity1 = -1;
    u32 textVersion = 0;  // changes with the text, line width or style (renderers cache the laid out text)
};

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
    double accelerationPower = 0.1;  // AbstractHurtingProjectile (projectile_power); wind charges 0
    // AreaEffectCloud: radius, waiting, the particle (encoded ParticleOptions; empty: entity_effect, white)
    std::string minecartCommand, minecartLastOutput;  // MinecartCommandBlock: command, last output (component NBT)
    float cloudRadius = 3.0f;
    bool cloudWaiting = false;
    std::string cloudParticle;
    bool dangerous = false;          // WitherSkull DATA_DANGEROUS (the blue skull)
    int leashHolder = 0;             // Leashable: the holder's entity id (set_entity_link), 0 none
    int pose = 0;
    bool baby = false;
    float health = 1;
    int variant = 0, variant2 = 0;  // registry/enum variant; secondary (markings, hidden gene, profession...)
    int villagerType = 0, villagerLevel = 1;
    int wool = 0;                   // sheep: color | 0x10 sheared
    int collar = 14;                // wolf/cat collar dye colour (red by default)
    int dyeColor = -1;              // cushion DATA_COLOR / shulker DATA_COLOR_ID (-1: the type's default)
    int attachFace = 0;             // shulker DATA_ATTACH_FACE_ID (Direction 3D value, down by default)
    int peekRaw = 0;                // shulker DATA_PEEK_ID (0..100)
    float peek = 0, peekO = 0;      // Shulker.currentPeekAmount (client side, 0..1)
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
    i32 equipDye[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // dyed_color (RGB) by slot, -1 none
    u8 equipFoil = 0;                                      // enchantment glint by slot (bits)
    i32 itemDye = -1;                                      // item / item frame stack: dyed_color
    // The whole stacks (data components: item models, trims, profiles, banner patterns ...); null when unknown.
    std::shared_ptr<const ui::ItemStack> itemStack, equipStack[8];
    bool itemFoil = false;
    int mapId = -1;             // item / item frame stack: map_id component (-1 none)
    int rotation = 0;           // item frame rotation (0..7)
    int direction = 0;          // add_entity data: facing of hanging entities (Direction 3D value)
    int painting = -1;          // painting_variant network id
    bool nameVisible = false;   // DATA_CUSTOM_NAME_VISIBLE

    // Animation timers (ticks).
    int age = 0, hurtTime = 0, deathTime = 0, swingTime = -1, swingDuration = 6;
    int swingHand = 0;     // the swinging hand: 0 main, 1 off (ClientboundSwingPacket)
    int swingType = -1;    // SwingAnimationType (0 none, 1 whack, 2 stab); -1: from the item in the swinging hand
    int kineticHitTicks = -1;  // ticks since a KINETIC_HIT entity event (SpearAnimations hit feedback), -1 none
    float flap = 0, oFlap = 0, flapSpeed = 0, oFlapSpeed = 0, flapping = 1;
    float bobOffset = 0;
    int vehicle = -1;
    u8 passengerCount = 0;  // ClientboundSetPassengersPacket: how many ride this entity (ourselves included)
    bool dying = false;
    u8 livingFlags = 0;   // LivingEntity DATA_LIVING_ENTITY_FLAGS: 1 using an item, 2 with the off hand, 4 spin attack
    int useTicks = 0;     // ticks the current item use has lasted (getTicksUsingItem)
    u8 chargedCrossbow = 0;  // bit 0: the main hand holds a charged crossbow, bit 1: the off hand (local player only)
    int attachedTo = -1;     // firework rocket: the entity it is attached to (DATA_ATTACHED_TO_TARGET), -1 none
    std::string fireworks;   // firework rocket: its item's "fireworks" component (encoded), "" without one
    bool silent = false;     // DATA_SILENT
    int attackTarget = 0;    // Guardian DATA_ID_ATTACK_TARGET (0 none) and clientSideAttackTime
    int attackTime = 0;
    int snifferState = 0;    // Sniffer DATA_STATE (Sniffer.State id)
    int cape = -1;                // the profile's cape (atlas texture), -1 none
    int elytraTexture = -1;       // the skin patch's elytra texture, -1 none
    // NewMinecartBehavior (experimental minecarts, ClientboundMoveMinecartPacket): the steps waiting, the ones being
    // lerped through over 3 ticks by weight, and where the lerp started.
    std::vector<MinecartStep> lerpSteps, currentLerpSteps;
    double currentLerpWeight = 0;
    MinecartStep oldLerp{};
    int lerpDelay = 0;
    bool newMinecart = false;
    std::string profileTextures;  // Mannequin DATA_PROFILE: its "textures" property; profileSkin 1 while it downloads
    std::string profileResolve;   // ... given only by name ("N" + name) or id ("U" + 16 bytes): profileSkin 4 meanwhile
    int profileSkin = 0;
    bool staysStill = false; // happy ghast: STAYS_STILL (holding still: its rider does not steer)
    int unloadedTicks = 0;  // ticks spent outside the loaded world
    // ItemPickupParticle: a collected item flies to its collector (entity id; the local player when it is not one of
    // the tracked entities) for 3 ticks, then goes. INT_MIN: not being picked up.
    int pickupBy = INT_MIN, pickupLife = 0;
    // Pose animation (ticked by tickPoseAnimation): LivingEntity swimAmount / fallFlyTicks, ElytraAnimationState,
    // the delta movement of this and the previous tick (fall-flying body turn), the bed (SLEEPING_POS_ID).
    float swimAmount = 0, swimAmountO = 0;
    int fallFlyTicks = 0;
    // ClientAvatarState (players, mannequins): the cloak trailing the position, the walking bob and walk distance (the
    // cape's flap and lean).
    double cloak[3] = {}, cloakO[3] = {};
    float bob = 0, bobO = 0, walkDist = 0, walkDistO = 0;
    float elytraRot[3] = {}, elytraRotO[3] = {};
    float move[3] = {}, moveO[3] = {};
    bool inWater = false;
    bool hasSleepPos = false;
    int sleepPos[3] = {};
    int bedFacing = -1;  // BedBlock.getBedOrientation as Direction 2D data value (0 S, 1 W, 2 N, 3 E), -1 none
    int ticksFrozen = 0;  // DATA_TICKS_FROZEN (fully frozen at 140)
    bool paddle[2] = {};  // boat: DATA_ID_PADDLE_LEFT / RIGHT (a boat the local player drives: its own input)
    bool hasChest = false;  // donkeys, mules, llamas: DATA_ID_CHEST
    // Minecarts: DATA_ID_CUSTOM_DISPLAY_BLOCK (network block state, 0: the type's own), DATA_ID_DISPLAY_OFFSET
    // (INT_MIN: the type's default), DATA_ID_FUEL (furnace minecart: lit).
    int displayBlock = 0, displayOffset = INT_MIN;
    int shoulderParrot[2] = {-1, -1};  // players: DATA_SHOULDER_PARROT_LEFT / RIGHT (Parrot.Variant id, -1 none)
    int arrowCount = 0, stingerCount = 0;  // LivingEntity DATA_ARROW_COUNT_ID / DATA_STINGER_COUNT_ID
    bool furnaceFuel = false;
    int fuse = -1;  // primed TNT (DATA_FUSE_ID) and TNT minecarts (entity event 70: 80 ticks), counted down each tick
    float paddlePos[2] = {};  // AbstractBoat.paddlePositions (radians)
    // Player skin (default skins chosen from the UUID like the game).
    bool slim = false;
    int skin = -1;
    DisplayState display;
    SpeciesState sp;  // species client state and raw synched data (species.h)
};

struct PlayerInfo {
    std::string name;
    int skin = -1;
    bool slim = false;
    std::string textures;  // the profile's "textures" property (skin / cape URLs), "" none
    int cape = -1;         // the profile's cape (atlas texture), -1 none
    int skinState = 0;     // 0 default skin, 1 the profile skin downloading, 2 applied, 3 failed
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

    // Item stacks in entity data / equipment carry data components: decode them with the game's item registry
    // (dyed colour, enchantment glint). Without it stacks with components end the packet's decoding. Before attach().
    static void setItemDecoder(const ui::Items* items);
    const PlayerInfo* playerNamed(const std::string& name) const {
        for (const auto& [k, p] : players_)
            if (p.name == name) return &p;
        return nullptr;
    }

    // Ray pick against entity bounding boxes (inflated by the target's pick radius); origin/dir in world space.
    PickResult pick(const double origin[3], const double dir[3], double maxDistance) const;
    // Sends attack (attack + punch) or use (interact) for an entity; false if not connected.
    bool attack(int id);
    bool use(int id, const double hit[3], bool sneaking);
    // MultiPlayerGameMode.spectate / spectatorNoAction: ServerboundSpectatorActionPacket with the entity under the
    // crosshair (closer than blockDistance, < 0: no block), or none.
    // Minecraft.pickBlockOrEntity on an entity: ServerboundPickItemFromEntityPacket (Ctrl: include its data). False
    // when no entity is under the crosshair in front of the targeted block.
    bool pickItem(const double eye[3], const double dir[3], double blockDistance, double reach, bool includeData);
    bool spectate(const double eye[3], const double dir[3], double blockDistance, double reach);

    const std::unordered_map<int, Entity>& entities() const { return entities_; }
    std::unordered_map<int, Entity>& entitiesMutable() { return entities_; }  // test scripts
    Entity* entity(int id) {
        auto it = entities_.find(id);
        return it == entities_.end() ? nullptr : &it->second;
    }
    // A vehicle the local player controls (Entity.isLocalInstanceAuthoritative): the server's moves for it are ignored
    // and `tick` moves it every entity tick instead of the interpolation (prevPos is already the old position).
    // id < 0: none.
    void setLocalControl(int id, std::function<void(Entity&)> tick) {
        localControl_ = id;
        localTick_ = std::move(tick);
    }
    int localControl() const { return localControl_; }
    // The entity the camera views from in first person (set_camera): not drawn (LevelRenderer skips the camera's
    // entity), its shadow still cast. -1: none.
    int viewEntity = -1;
    // The level's tick rate (ticking_state / ticking_step, kept by the gameplay layer): entities tick at the server's
    // slower rate; frozen, only players (and the vehicle we drive) tick and the rest are drawn where they stand.
    TickRate tickRate;
    float partialTick() const { return tickRate.runs ? float(tickAccumulator_ / tickSeconds_) : 1.0f; }
    const EntityAssets& assets() const { return assets_; }
    World& world() const { return world_; }
    BlockRegistry& registry() const { return registry_; }
    u16 localBlock(int networkState);  // network block state id -> registry id (main thread)
    const GameReports& reports() const { return reports_; }
    const std::string& villagerType(int id) const { return name(villagerTypes_, id); }
    const std::string& profession(int id) const { return name(professions_, id); }
    // Left / right click with an entity under the crosshair (before blocks): true when an entity closer than
    // blockDistance (< 0: no block targeted) is targeted; it then takes the click (attack / use when pressed).
    bool click(const double eye[3], const double dir[3], bool attack, bool use, double blockDistance, double reach,
               bool sneaking = false, double minReach = 0);
    // AttackRange.getClosesetHit for a weapon's attack_range: entities along the look from minReach to maxReach (+ the
    // forward movement), their boxes inflated by the hitbox margin (the hit projected back onto the box unless a block
    // is in the way); the closest to the eye. blockRay: the distance to the first block along a ray (max: none).
    // Attacks it (true) or, with nothing in range, only the swing goes out (false).
    using BlockRay = std::function<double(const double origin[3], const double dir[3], double max)>;
    PickResult pickAttackRange(const double eye[3], const double dir[3], double minReach, double maxReach, double margin,
                               double forward, const BlockRay& blockRay) const;
    bool attackInRange(const double eye[3], const double dir[3], double minReach, double maxReach, double margin,
                       double forward, const BlockRay& blockRay);  // sneaking: sent with interactions (sneak + use opens a
                                                             // mount's inventory); minReach: a weapon's attack range
    // Client effects for the visuals (particles), main thread: entity events (ClientboundEntityEventPacket ids, after
    // they are applied; 17 makes a firework rocket explode) and
    // animate actions (ClientboundAnimatePacket: 0 wake up, 1 critical hit, 2 magic critical hit).
    std::function<void(const Entity&, int event)> onEntityEvent;
    // Entity events for ids not tracked here (the local player's own, e.g. KINETIC_HIT).
    std::function<void(int id, int event)> onUntrackedEntityEvent;
    std::function<void(const Entity&, int action)> onAnimate;

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
           damage = -1, mainGene = -1, hiddenGene = -1, mobFlags = -1, noGravity = 5, inGround = -1, rotation = -1,
           painting = -1, nameVisible = 3, livingFlags = -1, attachedTo = -1, sleepPos = -1, ticksFrozen = -1,
           paddleLeft = -1, paddleRight = -1, chest = -1, color = -1, attachFace = -1, peek = -1, fireworksItem = -1,
           silent = 4, staysStill = -1, dangerous = -1, cloudRadius = -1, cloudWaiting = -1, cloudParticle = -1,
           commandName = -1, lastOutput = -1, attackTarget = -1, snifferState = -1, profile = -1, displayBlock = -1,
           displayOffset = -1, fuel = -1, fuse = -1, shoulderL = -1, shoulderR = -1, arrows = -1, stingers = -1;
        // Display entities.
        i8 dStart = -1, dDuration = -1, dPosRot = -1, dTranslation = -1, dScale = -1, dLeft = -1, dRight = -1,
           dBillboard = -1, dBrightness = -1, dViewRange = -1, dWidth = -1, dHeight = -1, dBlock = -1, dItem = -1,
           dItemContext = -1, dText = -1, dLineWidth = -1, dBackground = -1, dOpacity = -1, dStyle = -1;
    };

private:
    void apply(const Event& e, const EventQueue& q);
    void applyData(Entity& e, const MetaValue* values, u32 count, const EventQueue& q);
    bool applyDisplayData(Entity& e, const DataIds& d, const MetaValue& v, const EventQueue& q);
    void tick();
    void tickEntity(Entity& e);
    void updateProfileSkins();  // SkinManager: downloaded profile skins replace the default ones
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
    std::unordered_map<std::string, int> skinSlots_;  // profile skin texture hash -> atlas slot

public:
    // ClientboundAddTransientBlockPacket (a falling block landed): the block drawn where it landed until its section's
    // new mesh is in (the terrain's mesh serials), at most a second (TransientBlock.DEFAULT_TIME_TO_LIVE).
    struct TransientBlock {
        int x, y, z;
        int state;  // network block state id
        double until;
        u64 serial;  // the terrain's mesh serial when it arrived
    };
    std::vector<TransientBlock> transientBlocks;
    std::vector<double> mobAppearances;  // ElderGuardianParticle start times (nowSeconds), main thread
    // Our own player (not a tracked entity): its synched data as the server sends it (selfId: the session's entity id,
    // set by the owner); the avatar reads the stuck arrows / stingers and shoulder parrots from it.
    int selfId = INT_MIN;
    Entity self;
    // Particles of entities (set before attach): reading an encoded ParticleOptions (network thread) and spawning one.
    std::function<bool(net::Reader& r, std::string& raw)> readParticle;
    // The local player uses an entity (before the interact packet; main thread): true opens something client side
    // (the command block minecart's editor) and the packet is still sent.
    std::function<void(const Entity& e)> onUse;
    std::function<void(const std::string& raw, double x, double y, double z, double vx, double vy, double vz)> spawnParticle;
    std::function<u64()> meshSerial;
    std::function<u64(int, int, int)> sectionMeshSerial;

private:
    std::unordered_map<std::string, PlayerInfo> players_;  // by UUID bytes
    std::vector<DataIds> dataIds_;  // by entity type
    std::vector<Category> categories_;
    std::vector<int> defaultVariant_;  // by entity type
    std::vector<std::string> blockStates_;  // network block state id -> full state key
    std::unordered_map<int, u16> blockCache_;
    std::vector<std::string> villagerTypes_, professions_;
    int entityEffectParticle_ = -1;
    int localControl_ = -1;
    std::function<void(Entity&)> localTick_;
    double tickAccumulator_ = 0, tickSeconds_ = 0.05;
    i64 gameTime_ = 0;  // the level's game time (set_time, advanced each entity tick)
    std::atomic<i64> netGameTime_{INT64_MIN};
    std::atomic<float> rainLevel_{0}, thunderLevel_{0};  // game events 1 / 2 / 7 / 8
    u64 ticks_ = 0;
    Stats stats_;
    bool verbose_ = false;  // MCPP_ENTITY_VERBOSE=1: log spawns, variants, attacks
};

// Default player skin like DefaultPlayerSkin.get(uuid): floorMod(uuid.hashCode(), 18).
int defaultSkinIndex(const u8 uuid[16]);

// One entity tick of the pose animation state: swim amount (LivingEntity.updateSwimAmount), fall-flying ticks,
// ElytraAnimationState.tick. move: this tick's delta movement; fallFlying: shared flag 7 (the local player's own
// state for the avatar).
void tickPoseAnimation(Entity& e, const double move[3], bool fallFlying, bool crouching);
// ClientAvatarState.moveCloak (the position before this tick's move) and updateBob (tBob: AbstractClientPlayer.updateBob).
void tickCloak(Entity& e, const double pos[3], float tBob);
// The entity's bounding box size (EntityDimensions with the baby / slime size / small armor stand scales, player
// poses): width and height.
void entitySize(const Entity& e, double& w, double& h);
// BedBlock.getBedOrientation at a block (Direction 2D data value, -1 if it is not a bed). Caller holds the world lock.
int bedFacingAt(const World& world, const BlockRegistry& registry, int x, int y, int z);
// LivingEntity.isFullyFrozen.
inline bool fullyFrozen(const Entity& e) { return e.ticksFrozen >= 140; }
// getMainArm: players synch it (DATA_PLAYER_MAIN_HAND), mobs keep a left-handed flag (DATA_MOB_FLAGS_ID bit 2).
inline bool rightHanded(const Entity& e) { return e.category == Category::Player ? e.mainArm != 0 : !(e.mobFlags & 2); }

// Display.calculateInterpolationProgress at a partial tick (1 without an interpolation).
float displayProgress(const Entity& e, float partialTick);
// DisplayRenderer.submit before submitInner: the billboard orientation and the interpolated transformation as a
// row-major 3x4 matrix in the entity's space (translation to the entity position not included). camYaw / camPitch:
// the camera's yRot / xRot in degrees. False when the display has no render state yet (nothing is drawn).
bool displayMatrix(const Entity& e, float partialTick, float camYaw, float camPitch, float out[12]);
// The display's light (brightness override, else `worldLight`) as sky << 4 | block.
u32 displayLight(const Entity& e, u32 worldLight);
// Text display: the interpolated background (ARGB) and text opacity (signed byte value) at a partial tick.
void displayTextColors(const Entity& e, float partialTick, u32& background, int& opacity);

}  // namespace ent
