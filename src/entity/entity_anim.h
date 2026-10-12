#pragma once
// Ports of the game's model setupAnim code: the shared families here (HumanoidModel, QuadrupedModel, ChickenModel,
// CreeperModel, SpiderModel, IronGolemModel, VillagerModel, ArmorStandModel, ShulkerModel), every other species in
// species_anim.cpp, with a KeyframeAnimation player for the models driven by AnimationDefinitions.
#include "entity/entity_assets.h"
#include "entity/spear_anim.h"

namespace ent {

struct Entity;

enum class Anim : u8 {
    None, Head, Humanoid, Zombie, Skeleton, Quadruped, Chicken, Creeper, Spider, IronGolem, Villager, ArmorStand, Shulker,
    // species_anim.cpp
    Drowned, Piglin, ZombifiedPiglin, Enderman, Witch, Illager, WanderingTrader, Sheep, PolarBear, Goat, Panda, Turtle,
    Horse, Llama, Camel, Feline, Wolf, Fox, Rabbit, Parrot, Bee, Allay, Bat, Hoglin, Ravager, Dolphin, Cod, Salmon,
    Pufferfish, TropicalFish, Tadpole, Squid, Guardian, Frog, Axolotl, Armadillo, Sniffer, CopperGolem, SnowGolem, Ghast,
    HappyGhast, Nautilus, Blaze, Breeze, Creaking, Dragon, Endermite, Silverfish, MagmaCube, Phantom, Strider, Vex,
    Warden, Wither, EndCrystal, EvokerFangs, Arrow, ShulkerBullet, WindCharge, WitherSkull, LlamaSpit,
};
// HumanoidModel.ArmPose.
enum class ArmPose : u8 { Empty, Item, Block, BowAndArrow, ThrowTrident, CrossbowCharge, CrossbowHold, Spyglass, TootHorn, Brush, Spear };

struct AnimState {
    float walkPos = 0, walkSpeed = 0;  // WalkAnimationState position / speed (interpolated)
    float headYaw = 0, pitch = 0;      // degrees; head yaw relative to the body
    float age = 0;                     // ticks + partial tick
    float swing = 0;                   // attack animation 0..1 (0: none)
    float flap = 0, flapSpeed = 0;     // chicken
    bool baby = false, crouching = false, riding = false, aggressive = false;
    bool rightItem = false, leftItem = false;  // something held in the right / left hand
    bool swingLeft = false;            // the swinging arm is the left one
    bool leftHanded = false;           // the main arm is the left one
    int swingType = 1;                 // SwingAnimationType of the swing: 0 none, 1 whack, 2 stab
    KineticParams rightKinetic, leftKinetic;  // each arm's spear (SpearAnimations.thirdPersonHandUse)
    bool holdingBow = false;           // skeletons: a bow in the main hand (SkeletonModel)
    // Arm poses (AvatarRenderer.getArmPose); without them rightItem / leftItem give ITEM / EMPTY.
    bool havePoses = false;
    ArmPose rightPose = ArmPose::Empty, leftPose = ArmPose::Empty;
    bool usingItem = false, useRight = true;  // isUsingItem, and the arm of the used hand
    float useTicks = 0;                       // ticksUsingItem (+ partial tick)
    float crossbowCharge = 25;                // maxCrossbowChargeDuration
    int standFlags = 0;                // armor stand: 4 arms, 8 no base plate
    const float (*standPose)[3] = nullptr;  // armor stand head, body, left arm, right arm, left leg, right leg (degrees)
    int skinParts = 0x7F;              // player model customisation bits
    float swimAmount = 0;              // LivingEntity.getSwimAmount (swimming / crawling)
    bool fallFlying = false;           // elytra gliding: head down, limbs swing by speedValue
    float speedValue = 1;              // HumanoidRenderState.speedValue
    float peek = 0;                    // shulker lid (getClientPeekAmount)
    // Mob arm pose: PiglinArmPose (0 default, 1 melee attack, 2 crossbow hold, 3 crossbow charge, 4 admiring, 5 dancing)
    // or IllagerArmPose (0 crossed, 1 attacking, 2 spellcasting, 3 bow, 4 crossbow hold, 5 crossbow charge,
    // 6 celebrating, 7 neutral).
    u8 mobPose = 0;
    float pt = 0;                      // partial tick
    const Entity* entity = nullptr;    // species state and synched data (nullptr: defaults, e.g. spawner mobs)
    const EntityAssets* assets = nullptr;  // keyframe animations (nullptr: none)
    u8* hide = nullptr;                // per model part, set by the animation: 1 hidden (with its children), 2 skipDraw
    // Guardian eye (GuardianRenderState): the eye position, view vector and the position it looks at (world space).
    bool lookValid = false;
    float eyePos[3] = {}, lookDir[3] = {}, lookAt[3] = {};
};

// poses: one per model part, initialised by the caller from the model's initial poses.
// Returns a mask of roles whose parts are hidden.
u64 animate(Anim kind, const AnimState& s, const Model& model, PartPose* poses);
// The species models (species_anim.cpp).
u64 animateSpecies(Anim kind, const AnimState& s, const Model& model, PartPose* poses);
// KeyframeAnimation.apply(millis, targetScale) on the parts under `root` (the part the animation was baked on).
void applyKeyframes(const KeyAnimation& a, const Model& model, PartPose* poses, i64 millis, float scale, int root = 0);
// ModelPart.rotateBy: the part's Euler rotation times `rotation` (row-major 3x3).
void rotateBy(PartPose& p, const float rotation[9]);

}  // namespace ent
