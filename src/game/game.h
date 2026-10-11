#pragma once
#include "core/common.h"
#include "game/player.h"
#include "render/renderer.h"
#include "world/lighting.h"

#include <algorithm>
#include <functional>
#include <unordered_map>

class World;
class BlockRegistry;
class Terrain;
class Window;
struct PlayerStart;

// Single-player creative gameplay on top of the world: movement, targeting, breaking,
// placing and picking blocks, with relighting and immediate remeshing of edits.
class Game {
public:
    Game(World& world, BlockRegistry& registry, Terrain& terrain, Renderer& renderer, const PlayerStart& start);

    // Reads input, advances the simulation in fixed 20 Hz ticks and builds the HUD.
    void frame(Window& window, double dt);
    CameraState camera() const;  // the player's eyes (targeting, sounds)
    // F5 perspective: 0 first person, 1 third person back, 2 third person front. viewCamera() is the rendered
    // camera (Camera.setup: pulled back up to 4 blocks, stopped by blocks); camera() stays at the eyes.
    int cameraMode = 0;
    CameraState viewCamera() const;
    // View override (UI: the lying-down camera while sleeping, Camera.setup); may adjust the camera. Runs for every
    // perspective, before the third-person pull-back.
    std::function<void(CameraState&)> viewCameraOverride;
    // The player's arm swings (attack press, block broken / placed): drives the hand animation.
    std::function<void()> onSwing;
    std::string status() const;
    Player& player() { return player_; }

    // Used by input handling and by the scripted self-test.
    void retarget();
    const RayHit& target() const { return target_; }
    // The level's world border (ClientLevel.getWorldBorder): collisions, block targeting, the wall and the vignette.
    WorldBorder border;
    bool useTarget(bool place);  // break or place at the current target; true if the world changed
    void setHeld(u16 id) { hotbar_[selected_] = id; }
    int selectedSlot() const { return selected_; }
    u16 hotbarBlock(int slot) const { return hotbar_[slot]; }

    // Multiplayer hook, called for every break or place before it is applied locally.
    // (x, y, z) and face: the targeted block; hit: the targeted point; at: the cell that changes.
    struct EditAction {
        bool place = false;
        int x = 0, y = 0, z = 0, face = 0;
        double hit[3] = {};
        int at[3] = {};
        u16 block = 0;  // state placed (0 when breaking)
        // Breaking: the ServerboundPlayerActionPacket action (kStartDestroy...) and whether the block is broken
        // locally (predicted). START / STOP carry a prediction sequence; ABORT / CHANGE_DIRECTION do not.
        int action = kStartDestroy;
        bool predicted = true;
    };
    static constexpr int kStartDestroy = 0, kChangeDestroyDirection = 1, kAbortDestroy = 2, kStopDestroy = 3;
    std::function<void(const EditAction&)> onEdit;
    // Adventure mode (Player.blockActionRestricted): the held item's can_break allows breaking this block.
    std::function<bool(int x, int y, int z)> adventureMayBreak;
    // Entity hook (multiplayer), called on clicks before blocks: returns true when an entity closer than
    // blockDistance (< 0: no block targeted) is under the crosshair; it then takes the click.
    std::function<bool(bool attack, bool use, double blockDistance)> onEntityClick;
    // Sound hooks (audio): a block the player broke (its old state) or placed (the new state), once
    // applied; and the end of every 20 Hz player tick.
    std::function<void(bool place, int x, int y, int z, u16 state)> onBlockChanged;
    std::function<void()> onPlayerTick;
    // UI hooks: while inputSuspended (a screen is open) the player gets no input; with drawHud false the UI draws
    // the crosshair and hotbar; onPickBlock replaces the local middle-click pick.
    bool inputSuspended = false, drawHud = true, handleEscape = true;
    float sensitivity = 0.0025f;  // mouse look, radians per count
    std::function<void(int x, int y, int z)> onPickBlock;
    // Right click on a block that reacts to use (containers, doors...): true sends the use to the server as is
    // instead of placing the held block.
    std::function<bool(int x, int y, int z)> onInteract;
    // Multiplayer: every attack press (Minecraft.startAttack sends ServerboundPunchPacket: the arm swing others see,
    // attack cooldown), a right press that did not place a block or use one (the held item is used: food, bows,
    // buckets...), and the use key released after that (bows, eating stop).
    // onUseItem gets the targeted block (nullptr: none) for the off hand's turn (Minecraft.startUseItem tries
    // the main hand, then the off hand); afterEntity: the press was also sent as a use of an entity.
    std::function<void()> onPunch, onReleaseUseItem;
    std::function<void(const RayHit* target, bool afterEntity)> onUseItem;
    void setHotbarBlock(int slot, u16 id) { hotbar_[slot] = id; }

    // --- Mining: MultiPlayerGameMode digging driven by Minecraft.startAttack / continueAttack at 20 Hz. ---
    // destroyProgress: BlockState.getDestroyProgress with the held item (per tick; >= 1 breaks at once); unset: 1.
    std::function<float(int x, int y, int z, u16 state)> destroyProgress;
    // heldIdentity: the main hand stack's item + components (MultiPlayerGameMode.sameDestroyTarget).
    std::function<u64()> heldIdentity;
    // canDestroyInCreative: Item.canDestroyBlock (swords and other tools with can_destroy_blocks_in_creative off).
    std::function<bool()> canDestroyInCreative;
    bool instabuild = true;  // Abilities.instabuild (creative); kept up to date by the session
    int gameMode = 1;        // 0 survival, 1 creative, 2 adventure, 3 spectator
    // ClientLevel.addBreakingBlockEffects when a break starts: hit particles on the face, and the hit sound.
    std::function<void(int x, int y, int z, int face, bool sound)> onBreakingEffects;
    // Crack overlays (ClientLevel.destroyBlockProgress): by breaker entity id; stage 0..9, anything else removes.
    struct Destruction {
        int x, y, z, stage;
        u64 tick;  // last update (entries not updated for 400 ticks are dropped, like LevelRenderer)
    };
    static constexpr int kLocalBreaker = -1;  // our own digging (the server never sends us our own progress)
    void destroyBlockProgress(int id, int x, int y, int z, int stage);
    const std::unordered_map<int, Destruction>& destructions() const { return destructions_; }
    bool isDestroying() const { return dig_.active; }

    // --- Item use: LivingEntity / LocalPlayer use-item state (eating, bow drawing, shield raised...). Started by the
    // gameplay layer (prediction on the use press, or the server's living-entity flags), counted down every tick. ---
    struct ItemUse {
        bool active = false;
        int hand = 0;           // 0 main hand, 1 off hand
        int item = -1;          // network item id in use
        int anim = 0;           // ItemRules::Anim
        int duration = 0;       // ItemStack.getUseDuration
        int remaining = 0;      // getUseItemRemainingTicks
        float speed = 0.2f;     // use_effects speed_multiplier (movement while using)
        bool canSprint = false;  // use_effects can_sprint
        int ticksUsing() const { return active ? duration - remaining : 0; }
    } use;
    // Every tick while an item is in use, before the count down (LivingEntity.updatingUsingItem: the gameplay layer
    // stops the use when the hand changed, plays eating particles / sounds).
    std::function<void()> onUseTick;
    // After a use press went to the server (onUseItem): the client's own prediction (Item.use -> startUsingItem).
    std::function<void(const RayHit* target, bool afterEntity)> predictUse;
    // Item shards (eating): ParticleTypes.ITEM at a position with a velocity.
    std::function<void(int item, const double pos[3], const double vel[3])> onItemParticle;
    bool usePressed() const { return usingItem_; }  // the use key is held after a use press
    // AbstractClientPlayer.getFieldOfViewModifier, item part: drawing a bow narrows the view (x 0.85 at full draw);
    // scoping with a spyglass in first person sets it to 0.1.
    float useFovModifier() const;
    bool scoping() const;

    // --- Riding (game/riding.h keeps these): the vehicle entity, -1 when not riding. While riding, rideTick replaces
    // the player's own movement every tick and rideEye gives the eye position as the vehicle is drawn. ---
    int vehicleId = -1;
    bool handsBusy = false;  // LocalPlayer.isHandsBusy (paddling): no attacks or uses, the hands go down
    std::function<void(const PlayerInput& input)> rideTick;
    std::function<bool(double eye[3])> rideEye;
    bool riding() const { return vehicleId >= 0 && rideTick != nullptr; }
    // Sleeping (UI bed module: our SLEEPING_POS_ID): the bed block; drawn lying in it in third person.
    bool sleeping = false;
    int bedPos[3] = {};
    // --- Camera feedback: view bobbing (ClientAvatarState walk distance / bob, GameRenderer.bobView), the hurt tilt
    // and death roll (bobHurt), and the attack strength (Player.attackStrengthTicker) for the attack indicator. ---
    bool bobView = true;  // the "View Bobbing" option (default on)
    float walkDist = 0, walkDistO = 0, bob = 0, bobO = 0;
    int hurtTime = 0, hurtDuration = 10, deathTime = 0;
    float hurtDir = 0;   // degrees (ClientboundHurtAnimationPacket yaw)
    bool dead = false;
    void hurt(float dir) { hurtTime = hurtDuration = 10, hurtDir = dir; }  // LivingEntity.animateHurt
    void hurt() { hurtTime = hurtDuration = 10; }                         // handleDamageEvent / LocalPlayer.hurtTo
    int attackStrengthTicker = 0;
    float attackSpeed = 4.0f;          // the attack_speed attribute (with the held item)
    bool crosshairOnLiving = false;    // a living entity is under the crosshair (Minecraft.crosshairPickEntity)
    float attackStrengthScale(float a) const {
        const float delay = 20.0f / std::max(attackSpeed, 1e-4f);
        return std::clamp((float(attackStrengthTicker) + a) / delay, 0.0f, 1.0f);
    }
    float attackStrengthDelay() const { return 20.0f / std::max(attackSpeed, 1e-4f); }
    float partialTick() const { return float(alpha_); }
    // The camera without the bobbing (the first-person hand applies its own).
    CameraState handCamera() const;
    struct VehicleHud {
        int hearts = 0;        // Hud.getVehicleMaxHearts (0: food bar as usual)
        float health = 0;      // the vehicle's health
        bool jumpable = false;  // the jump bar replaces the experience bar
        float jumpScale = 0;    // LocalPlayer.getJumpRidingScale
        int jumpCooldown = 0;
    } vehicleHud;
    float destroyProgressValue() const { return dig_.progress; }
    void setSelectedSlot(int slot) { selected_ = slot; }
    void simulate(const PlayerInput& input, int ticks);
    void buildOverlay();

private:
    PlayerInput readInput(Window& window);
    void handleClicks(Window& window, double dt, int rightPresses, int middlePresses, int wheel);
    void tickAttack(bool down);  // Minecraft.handleKeybinds: queued attack presses, then continueAttack
    void tickCameraFeedback();
    bool startAttack();
    void continueAttack(bool down);
    bool startDestroyBlock(int x, int y, int z, int face);
    bool continueDestroyBlock(int x, int y, int z, int face);
    void stopDestroyBlock();
    bool destroyBlock(int x, int y, int z);  // MultiPlayerGameMode.destroyBlock: the local prediction
    void sendDig(int action, int x, int y, int z, int face, bool predicted);
    float progressAt(int x, int y, int z, u16 state) const;
    void edit(int x, int y, int z, u16 id);
    u16 orientForPlacement(u16 id, const RayHit& hit, const int target[3]);

    World& world_;
    BlockRegistry& registry_;
    Terrain& terrain_;
    Renderer& renderer_;
    Physics physics_;
    Lighting lighting_;
    Player player_;
    double tickAccumulator_ = 0, alpha_ = 0;
    double eyeHeight_ = 1.62, eyeHeightOld_ = 1.62;  // Camera.tick: the eye height eases to the pose's
    RayHit target_;
    u16 hotbar_[9] = {};
    int selected_ = 0;
    double repeatTimer_ = 0;
    double lastJumpPress_ = -1, clock_ = 0;
    bool jumpWasDown_ = false, escWasDown_ = false, f5WasDown_ = false;
    bool useConsumed_ = false;  // the last useTarget(place) placed a block or used the targeted block
    bool usingItem_ = false;    // onUseItem sent, the use key not yet released
    bool keyWasDown_[9] = {};
    // MultiPlayerGameMode / Minecraft digging state.
    struct Dig {
        bool active = false;
        int x = 0, y = -1, z = 0, face = -1;
        u64 held = 0;
        float progress = 0;
        int ticks = 0;
    } dig_;
    int destroyDelay_ = 0, missTime_ = 0, pendingAttacks_ = 0;
    u64 ticks_ = 0;
    std::unordered_map<int, Destruction> destructions_;
};
