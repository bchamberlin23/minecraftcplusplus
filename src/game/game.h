#pragma once
#include "core/common.h"
#include "entity/spear_anim.h"
#include "game/player.h"
#include "game/tick_rate.h"
#include "render/renderer.h"
#include "world/lighting.h"

#include <algorithm>
#include <array>
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
    // Minecraft.setCameraEntity (set_camera): the entity the view follows, -1 the player. While it is another entity
    // the player gets no movement input (LocalPlayer.isControlledCamera; its keys still go to the server). entityView
    // gives that entity's eye position and view rotation at the partial tick (false: unknown, the player's view).
    int cameraEntity = -1;
    std::function<bool(int id, CameraState& out)> entityView;
    // The player's arm swings (attack press, block broken / placed): drives the hand animation. hand: 0 main, 1 off;
    // type: the item's SwingAnimationType (0 none, 1 whack, 2 stab).
    std::function<void(int hand, int type)> onSwing;
    // LocalPlayer.itemUsed: a use of the hand's item went through (Minecraft.startUseItem); its hand drops for the
    // re-equip motion.
    std::function<void(int hand)> onItemUsed;
    bool useOnTakesItem = false;  // the last predicted Item.useOn used up one of the stack (set by the gameplay layer)
    std::string status() const;
    Player& player() { return player_; }
    Physics& physics() { return physics_; }
    // Moving blocks of pistons (the block entity layer) for the player's collisions and pushes, every tick; and the
    // entities pushing the player after its tick (living entities' pushEntities, boats).
    std::function<void(std::vector<Physics::Piston>& out)> pistonSource;
    std::function<void(Player& player)> entityPush;

    // Used by input handling and by the scripted self-test.
    void retarget();
    const RayHit& target() const { return target_; }
    // LocalPlayer.pick: blocks within blockReach are targeted; an entity is targeted when it is in front of the
    // first block the ray meets (within max(blockReach, entityReach)) and within entityReach. The ranges are the
    // block_interaction_range / entity_interaction_range attributes (gameplay layer); creative's until then.
    double blockReach = 5.0, entityReach = 5.0;
    double pickBlockDistance() const { return blockDistance_; }  // that first block's distance, < 0: none
    // The level's world border (ClientLevel.getWorldBorder): collisions, block targeting, the wall and the vignette.
    WorldBorder border;
    // The level's tick rate manager (gameplay layer): the client tick length follows a slower server rate; frozen,
    // the border stands still (the player keeps ticking).
    TickRate tickRate;
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
        int hand = 0;         // use_item_on: 0 main hand, 1 off hand
        bool inside = false;  // use_item_on: BlockHitResult.isInside (the eyes inside the targeted shape)
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
    // Key bindings and control options (the UI's Settings, set every frame): Windows virtual keys, mouse buttons as
    // VK_LBUTTON (1) / VK_RBUTTON (2) / VK_MBUTTON (4)...; 0 is unbound. Defaults are the game's.
    struct Keys {
        int forward = 'W', left = 'A', back = 'S', right = 'D', jump = 0x20, sneak = 0xA0, sprint = 0xA2;  // Space, LShift, LCtrl
        int attack = 1, use = 2, pickItem = 4, perspective = 0x74;                                           // mouse, F5
        int hotbar[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    } keys;
    bool toggleCrouch = false, toggleSprint = false;  // ToggleKeyMapping: a press switches the key on / off
    bool invertMouseX = false, invertMouseY = false, discreteScroll = false;
    double wheelSensitivity = 1.0;  // mouseWheelSensitivity (ScrollWheelHandler)
    // Singleplayer pause (Minecraft.isPaused): the client does not tick (no movement, no input, no digging).
    bool paused = false;
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
    std::function<void(int hand)> onUseItem;  // ServerboundUseItemPacket for a hand
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
    // Minecraft.startUseItem: each hand in turn uses the targeted block (useItemOn) then its item (useItem) until one
    // consumes the click or fails. The gameplay layer predicts the item parts from the stacks' components:
    // predictItemUse (ItemStack.use: starts the use, cooldowns, buckets), predictUseOn (ItemStack.useOn of items that
    // are not blocks); handHasItem / handOnCooldown describe the hands; predictRelease (Item.releaseUsing: riptide).
    static constexpr int kUsePass = 0, kUseConsume = 1, kUseFail = 2;
    void startUseItem(bool afterEntity);
    int useItemOn(int hand);
    std::function<int(int hand, const RayHit* target)> predictItemUse;
    std::function<int(int hand, const RayHit& hit)> predictUseOn;
    std::function<bool(int hand)> handHasItem, handOnCooldown;
    std::function<void()> predictRelease;
    void setOffhandBlock(u16 id) { offhandBlock_ = id; }  // the off hand's block (0: not a block item)
    void predictBlock(int x, int y, int z, u16 id) { edit(x, y, z, id); }  // a client-side block change of an item use
    // --- Item cooldowns: Player.getCooldowns (ItemCooldowns) by cooldown group ("minecraft:ender_pearl"), set by the
    // server's cooldown packets and counted in player ticks. While the main hand's group cools down, a right click
    // does not predict its use (MultiPlayerGameMode.useItemOn / useItem pass). ---
    void addCooldown(const std::string& group, int ticks) { cooldowns_[group] = {cooldownTick_, cooldownTick_ + ticks}; }
    void removeCooldown(const std::string& group) { cooldowns_.erase(group); }
    void clearCooldowns() { cooldowns_.clear(); }
    float cooldownPercent(const std::string& group, float partial) const;  // ItemCooldowns.getCooldownPercent
    // Item shards (eating): ParticleTypes.ITEM at a position with a velocity.
    std::function<void(int item, const double pos[3], const double vel[3])> onItemParticle;
    // A particle of a type by name ("minecraft:large_smoke") at a position with a velocity (the client's own effects).
    std::function<void(std::string_view type, const double pos[3], const double vel[3])> onParticle;
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
    // The main hand's weapon components (gameplay layer): piercing_weapon (Minecraft.startAttack stabs instead:
    // ServerboundPlayerActionPacket STAB through onStab, no block breaking), minimum_attack_charge
    // (Player.cannotAttackWithItem), attack_range (entities hit only from min to max reach; max < 0: none) and the
    // swing's duration (attack_animation with Haste / Mining Fatigue, LivingEntity.getModifiedSwingDuration).
    bool heldPiercing = false;
    float heldMinimumCharge = 0;
    double attackMinReach = 0, attackMaxReach = -1, attackMargin = 0;
    std::string playerName;  // our name (scoreboard team membership)  // attack_range: reaches (-1 none), hitbox margin
    int swingTicks = 6, useSwingTicks = 6;  // attack / interact swings (onSwing reads swingTicks)
    int swingType = 1;                      // the main hand's attack_animation type
    int useSwingTicksOf[2] = {6, 6}, useSwingTypeOf[2] = {1, 1};  // each hand's interact_animation
    ent::KineticParams kinetic[2];          // each hand's kinetic_weapon timing (spear use animations)
    // LivingEntity.getTicksSinceLastKineticHitFeedback: ticks since the server's KINETIC_HIT event, -1 none.
    int kineticHitTicks = -1;
    std::function<void()> onStab;
    std::function<void()> onStabEffects;  // PiercingWeapon.makeSound for the local player (gameplay layer)
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
    bool traceTicks = false;  // logs the player's state every tick (the script's "trace on")
    void buildOverlay();

private:
    PlayerInput readInput(Window& window);
    bool held(Window& window, int vk) const;  // a bound key / button is down while the mouse is captured
    void handleClicks(Window& window, double dt, int rightPresses, int middlePresses, int wheel);
    void refreshPistons();
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
    double blockDistance_ = -1;
    u16 hotbar_[9] = {};
    int selected_ = 0;
    double repeatTimer_ = 0;
    double clock_ = 0;
    bool escWasDown_ = false;
    bool useConsumed_ = false;  // the last use press placed a block, used the targeted block or failed
    bool placedLast_ = false;   // the last useItemOn placed a block
    u16 offhandBlock_ = 0;
    bool usingItem_ = false;    // onUseItem sent, the use key not yet released
    bool crouchToggled_ = false, sprintToggled_ = false;
    double scrollAccum_ = 0;  // ScrollWheelHandler.accumulatedScrollY
    std::array<u16, 256> presses_{};  // this frame's key / button presses (Window::takePresses)
    int presses(int vk) const { return vk > 0 && vk < 256 ? presses_[size_t(vk)] : 0; }
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
    struct Cooldown {
        int start, end;  // player ticks
    };
    std::unordered_map<std::string, Cooldown> cooldowns_;
    int cooldownTick_ = 0;
};
