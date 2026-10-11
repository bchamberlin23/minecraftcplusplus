#pragma once
// Ports of the game's model setupAnim code (HumanoidModel, QuadrupedModel, ChickenModel, CreeperModel,
// SpiderModel, IronGolemModel, VillagerModel, ArmorStandModel); other models only turn their head.
#include "entity/entity_assets.h"

namespace ent {

enum class Anim : u8 { None, Head, Humanoid, Zombie, Skeleton, Quadruped, Chicken, Creeper, Spider, IronGolem, Villager, ArmorStand };

struct AnimState {
    float walkPos = 0, walkSpeed = 0;  // WalkAnimationState position / speed (interpolated)
    float headYaw = 0, pitch = 0;      // degrees; head yaw relative to the body
    float age = 0;                     // ticks + partial tick
    float swing = 0;                   // attack animation 0..1 (0: none)
    float flap = 0, flapSpeed = 0;     // chicken
    bool baby = false, crouching = false, riding = false, aggressive = false;
    bool rightItem = false, leftItem = false;  // something held in the right / left hand
    bool swingLeft = false;
    int standFlags = 0;                // armor stand: 4 arms, 8 no base plate
    const float (*standPose)[3] = nullptr;  // armor stand head, body, left arm, right arm, left leg, right leg (degrees)
    int skinParts = 0x7F;              // player model customisation bits
};

// poses: one per model part, initialised by the caller from the model's initial poses.
// Returns a mask of roles whose parts are hidden.
u64 animate(Anim kind, const AnimState& s, const Model& model, PartPose* poses);

}  // namespace ent
