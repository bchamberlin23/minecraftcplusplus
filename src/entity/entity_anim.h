#pragma once
// Ports of the game's model setupAnim code (HumanoidModel, QuadrupedModel, ChickenModel, CreeperModel,
// SpiderModel, IronGolemModel, VillagerModel, ArmorStandModel, ShulkerModel); other models only turn their head.
#include "entity/entity_assets.h"
#include "entity/spear_anim.h"

namespace ent {

enum class Anim : u8 { None, Head, Humanoid, Zombie, Skeleton, Quadruped, Chicken, Creeper, Spider, IronGolem, Villager, ArmorStand, Shulker };
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
};

// poses: one per model part, initialised by the caller from the model's initial poses.
// Returns a mask of roles whose parts are hidden.
u64 animate(Anim kind, const AnimState& s, const Model& model, PartPose* poses);

}  // namespace ent
