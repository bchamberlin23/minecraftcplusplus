#pragma once
// Species-specific client state: what the game's entity classes keep on the client for their renderers (keyframe
// AnimationStates, eat / stand / shake / roll timers, counters set by entity events) and the raw synched data they
// read. Ticked with the entity (species.cpp); read by the model animations (species_anim.cpp) and the renderer.
#include "core/common.h"

#include <climits>
#include <unordered_map>

namespace ent {

struct Entity;

struct SpeciesState {
    static constexpr int kData = 32;
    i64 data[kData] = {};  // synched values by data id (MetaValue::i / f[0]), when `seen`
    float dataF[kData] = {};
    u32 seen = 0;

    // AnimationState start ticks (INT_MIN: stopped), by species (see species.cpp).
    int anim[8] = {INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN};
    // Smoothed amounts (this tick, last tick) and counters, by species (see species.cpp).
    float a[6] = {}, ao[6] = {};
    int t[6] = {};
    float history[64][2] = {};  // ender dragon: flight history (y, yRot) of the last 64 ticks
    int historyPos = -1;
    float wither[2][4] = {};    // wither side heads: xRot, yRot, xRotO, yRotO (degrees)
    u8 kind = 0;          // species (species.cpp), resolved on the first tick
    i64 gameTime = 0;     // the level's game time at the last tick
    bool partyNearby = false;  // jukebox playing within reach (parrots, allays)
    int jukebox[3] = {};
};

// Slots of SpeciesState::a / ao (amounts), t (counters) and anim (AnimationStates) for each species.
enum SpeciesSlot : int {
    // a / ao
    kInterest = 0, kShake = 1,                 // wolf: interestedAngle, shakeAnim (t[0]: 1 shaking, t[1]: 1 wet)
    kFoxInterest = 0, kFoxCrouch = 1,          // fox
    kLie = 0, kLieTail = 1, kRelax = 2,        // cat
    kRoll = 0,                                 // bee
    kSit = 0, kOnBack = 1, kPandaRoll = 2,     // panda (t[0]: rollCounter)
    kEat = 0, kStand = 1, kMouth = 2,          // horses (t[0]: tailCounter); sheep t[0]: eatAnimationTick
    kBearStand = 0,                            // polar bear
    kHolding = 0, kSpin = 1,                   // allay (t[0]: spinning ticks)
    kTendril = 0, kHeart = 1,                  // warden (t[0]: tendrilAnimation ticks, t[1]: heartAnimation ticks)
    kSpikes = 0, kTail = 1,                    // guardian (t[0]: clientSideAttackTime)
    kTentacle = 0, kXBody = 1, kZBody = 2,     // squid (a[3]: tentacleMovement, t: random speeds as float bits)
    kSquish = 0,                               // slimes (a[1]: targetSquish)
    kFlap = 0,                                 // ender dragon flapTime
    kAxoPlayDead = 0, kAxoInWater = 1, kAxoOnGround = 2, kAxoMoving = 3,  // axolotl BinaryAnimators
    kWiggle = 0,                               // armor stand: lastHit wiggle (t[0]: last hit game time)
};
enum SpeciesAnim : int {
    kWardenRoar = 0, kWardenSniff = 1, kWardenEmerge = 2, kWardenDig = 3, kWardenAttack = 4, kWardenSonic = 5,
    kCamelSit = 0, kCamelSitPose = 1, kCamelSitUp = 2, kCamelIdle = 3, kCamelDash = 4,
    kSnifferHappy = 0, kSnifferScent = 1, kSnifferSniff = 2, kSnifferDig = 3, kSnifferRise = 4,
    kArmadilloRollOut = 0, kArmadilloRollUp = 1, kArmadilloPeek = 2,
    kFrogJump = 0, kFrogCroak = 1, kFrogTongue = 2, kFrogSwimIdle = 3,
    kAxoSwim = 0, kAxoWalk = 1, kAxoWalkWater = 2, kAxoIdleWater = 3, kAxoIdleWaterGround = 4, kAxoIdleGround = 5,
    kAxoPlayDeadAnim = 6,
    kBatFly = 0, kBatRest = 1,
    kRabbitHop = 0, kRabbitTilt = 1,
    kCreakingAttack = 0, kCreakingInvulnerable = 1, kCreakingDeath = 2,
    kGolemIdle = 0, kGolemGet = 1, kGolemGetNo = 2, kGolemDrop = 3, kGolemDropNo = 4,
    kBreezeIdle = 0, kBreezeSlide = 1, kBreezeSlideBack = 2, kBreezeJump = 3, kBreezeShoot = 4, kBreezeInhale = 5,
};

// The synched value of a field ("DATA_FLAGS_ID", any class of the type's hierarchy) or `def` when not received.
i64 dataInt(const Entity& e, const char* field, i64 def = 0);
bool isBlocking(const Entity& e);  // LivingEntity.isBlocking (entities.cpp): blocks_attacks in use past its block delay
float dataFloat(const Entity& e, const char* field, float def = 0);
bool dataSeen(const Entity& e, const char* field);
// EnderDragon.aiStep's tickPart: the eight parts (head, neck, body, tail x 3, wing x 2) as boxes relative to the
// dragon's position (min x y z, max x y z), from its flight history (the client's yRotA stays 0).
void dragonPartBoxes(const Entity& e, double boxes[8][6]);

// Client-side ticks (after the entity's own), entity events (ClientboundEntityEventPacket), synched data changes
// (onSyncedDataUpdated) and jukebox notifications (LevelRenderer.notifyNearbyEntities).
// entities: every tracked entity (wither heads follow their targets).
void tickSpecies(Entity& e, i64 gameTime, const std::unordered_map<int, Entity>& entities);
void speciesEvent(Entity& e, int event);
void speciesDataChanged(Entity& e, int index);
void speciesJukebox(Entity& e, const int pos[3], bool playing);
// Level weather (game events): Level.isThundering, for scared pandas.
void setSpeciesThundering(bool thundering);
bool speciesThundering();
// Armadillo.shouldHideInShell.
bool armadilloInShell(const Entity& e);

}  // namespace ent
