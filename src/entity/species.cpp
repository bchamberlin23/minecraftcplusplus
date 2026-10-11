#include "entity/species.h"

#include "entity/entities.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace ent {
namespace {

constexpr float kPi = 3.14159265f;

float wrapDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d >= 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

std::minstd_rand& rng() {
    static std::minstd_rand r(0x5eed);
    return r;
}
float frand() { return float(rng()() % 1000000u) / 1000000.0f; }
int irand(int n) { return n > 0 ? int(rng()() % u32(n)) : 0; }

// AnimationState.start / startIfStopped / stop / animateWhen on SpeciesState::anim slots.
void start(Entity& e, int slot) { e.sp.anim[slot] = e.age; }
void startIfStopped(Entity& e, int slot) {
    if (e.sp.anim[slot] == INT_MIN) e.sp.anim[slot] = e.age;
}
void stop(Entity& e, int slot) { e.sp.anim[slot] = INT_MIN; }
void animateWhen(Entity& e, int slot, bool on) { on ? startIfStopped(e, slot) : stop(e, slot); }

enum class Sp : u8 {
    Unknown, Other, Wolf, Fox, Cat, Parrot, Bee, Panda, Sheep, Goat, PolarBear, Horse, Camel, IronGolem, Ravager, Hoglin,
    Guardian, Squid, Slime, MagmaCube, Dragon, Wither, EvokerFangs, Arrow, Allay, Warden, Sniffer, Armadillo, Frog,
    Axolotl, Bat, Rabbit, Breeze, Creaking, CopperGolem, ArmorStand
};

Sp speciesOf(const Entity& e) {
    if (!e.info) return Sp::Other;
    std::string_view n = e.info->name;
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    static const std::pair<std::string_view, Sp> kNames[] = {
        {"wolf", Sp::Wolf}, {"fox", Sp::Fox}, {"cat", Sp::Cat}, {"parrot", Sp::Parrot}, {"bee", Sp::Bee},
        {"panda", Sp::Panda}, {"sheep", Sp::Sheep}, {"goat", Sp::Goat}, {"polar_bear", Sp::PolarBear},
        {"horse", Sp::Horse}, {"donkey", Sp::Horse}, {"mule", Sp::Horse}, {"skeleton_horse", Sp::Horse},
        {"zombie_horse", Sp::Horse}, {"llama", Sp::Horse}, {"trader_llama", Sp::Horse}, {"camel", Sp::Camel},
        {"camel_husk", Sp::Camel}, {"iron_golem", Sp::IronGolem}, {"ravager", Sp::Ravager}, {"hoglin", Sp::Hoglin},
        {"zoglin", Sp::Hoglin}, {"guardian", Sp::Guardian}, {"elder_guardian", Sp::Guardian}, {"squid", Sp::Squid},
        {"glow_squid", Sp::Squid}, {"slime", Sp::Slime}, {"magma_cube", Sp::MagmaCube}, {"ender_dragon", Sp::Dragon},
        {"wither", Sp::Wither}, {"evoker_fangs", Sp::EvokerFangs}, {"arrow", Sp::Arrow}, {"spectral_arrow", Sp::Arrow},
        {"trident", Sp::Arrow}, {"allay", Sp::Allay}, {"warden", Sp::Warden}, {"sniffer", Sp::Sniffer},
        {"armadillo", Sp::Armadillo}, {"frog", Sp::Frog}, {"axolotl", Sp::Axolotl}, {"bat", Sp::Bat},
        {"rabbit", Sp::Rabbit}, {"breeze", Sp::Breeze}, {"creaking", Sp::Creaking}, {"copper_golem", Sp::CopperGolem},
        {"armor_stand", Sp::ArmorStand},
    };
    for (const auto& [name, sp] : kNames)
        if (n == name) return sp;
    return Sp::Other;
}
Sp species(Entity& e) {
    if (e.sp.kind == 0) e.sp.kind = u8(speciesOf(e));
    return Sp(e.sp.kind);
}

// Smoothed amount helpers: a[i] steps toward on/off by (up, down) within [0, 1].
void stepAmount(Entity& e, int i, bool on, float up, float down) {
    e.sp.ao[i] = e.sp.a[i];
    e.sp.a[i] = on ? std::min(1.0f, e.sp.a[i] + up) : std::max(0.0f, e.sp.a[i] - down);
}
void approach(Entity& e, int i, bool on, float rate) {
    e.sp.ao[i] = e.sp.a[i];
    e.sp.a[i] += ((on ? 1.0f : 0.0f) - e.sp.a[i]) * rate;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------

namespace {
int fieldId(const Entity& e, const char* field) {
    // Cache by (type, field pointer): call sites pass string literals.
    struct Slot {
        const TypeInfo* type = nullptr;
        const char* field = nullptr;
        int id = -1;
    };
    thread_local Slot cache[64];
    Slot& s = cache[(reinterpret_cast<uintptr_t>(field) >> 3 ^ reinterpret_cast<uintptr_t>(e.info) >> 4) & 63];
    if (s.type != e.info || s.field != field) s.type = e.info, s.field = field, s.id = e.info ? e.info->dataId(field) : -1;
    return s.id;
}
}  // namespace

bool dataSeen(const Entity& e, const char* field) {
    const int id = fieldId(e, field);
    return id >= 0 && id < SpeciesState::kData && ((e.sp.seen >> id) & 1);
}
i64 dataInt(const Entity& e, const char* field, i64 def) {
    const int id = fieldId(e, field);
    return id >= 0 && id < SpeciesState::kData && ((e.sp.seen >> id) & 1) ? e.sp.data[id] : def;
}
float dataFloat(const Entity& e, const char* field, float def) {
    const int id = fieldId(e, field);
    return id >= 0 && id < SpeciesState::kData && ((e.sp.seen >> id) & 1) ? e.sp.dataF[id] : def;
}

// ---------------------------------------------------------------------------------------------------------

void speciesDataChanged(Entity& e, int index) {
    const Sp sp = species(e);
    const bool pose = index == 6;  // Entity.DATA_POSE
    if (sp == Sp::Warden && pose) {  // Warden.onSyncedDataUpdated
        switch (e.pose) {
            case 13: start(e, kWardenEmerge); break;
            case 14: start(e, kWardenDig); break;
            case 11: start(e, kWardenRoar); break;
            case 12: start(e, kWardenSniff); break;
            default: break;
        }
    } else if (sp == Sp::Frog && pose) {  // Frog.onSyncedDataUpdated
        e.pose == 6 ? start(e, kFrogJump) : stop(e, kFrogJump);
        e.pose == 8 ? start(e, kFrogCroak) : stop(e, kFrogCroak);
        e.pose == 9 ? start(e, kFrogTongue) : stop(e, kFrogTongue);
    } else if (sp == Sp::Breeze && pose) {  // Breeze.onSyncedDataUpdated
        stop(e, kBreezeShoot), stop(e, kBreezeIdle), stop(e, kBreezeInhale), stop(e, kBreezeJump);
        if (e.pose == 16) startIfStopped(e, kBreezeShoot);
        else if (e.pose == 17) startIfStopped(e, kBreezeInhale);
        else if (e.pose == 15) startIfStopped(e, kBreezeSlide);
    } else if (sp == Sp::Sniffer && index == fieldId(e, "DATA_STATE")) {  // Sniffer.onSyncedDataUpdated
        for (int k : {kSnifferDig, kSnifferSniff, kSnifferRise, kSnifferHappy, kSnifferScent}) stop(e, k);
        switch (int(dataInt(e, "DATA_STATE"))) {
            case 1: startIfStopped(e, kSnifferHappy); break;
            case 2: startIfStopped(e, kSnifferScent); break;
            case 3: startIfStopped(e, kSnifferSniff); break;
            case 5: startIfStopped(e, kSnifferDig); break;
            case 6: startIfStopped(e, kSnifferRise); break;
            default: break;
        }
    } else if (sp == Sp::Armadillo && index == fieldId(e, "ARMADILLO_STATE")) {
        e.sp.t[0] = 0;  // inStateTicks
    } else if (sp == Sp::Guardian && index == fieldId(e, "DATA_ID_ATTACK_TARGET")) {
        e.sp.t[0] = 0;  // clientSideAttackTime
    } else if (sp == Sp::Arrow && index == fieldId(e, "IN_GROUND") && e.age > 0 && e.sp.t[0] <= 0 && dataInt(e, "IN_GROUND")) {
        e.sp.t[0] = 7;  // AbstractArrow.shakeTime
    } else if (sp == Sp::Camel && index == fieldId(e, "DASH") && e.age > 0) {
        e.sp.t[1] = e.sp.t[1] == 0 ? 55 : e.sp.t[1];  // dashCooldown
    }
}

void speciesEvent(Entity& e, int id) {
    switch (species(e)) {
        case Sp::Wolf:
            if (id == 8) e.sp.t[0] = 1, e.sp.a[kShake] = e.sp.ao[kShake] = 0;  // shake off water
            else if (id == 56) e.sp.t[0] = 0, e.sp.a[kShake] = e.sp.ao[kShake] = 0;
            break;
        case Sp::Sheep:
            if (id == 10) e.sp.t[0] = 40;  // eatAnimationTick
            break;
        case Sp::Goat:
            if (id == 58) e.sp.t[1] = 1;  // isLoweringHead
            else if (id == 59) e.sp.t[1] = 0;
            break;
        case Sp::IronGolem:
            if (id == 4) e.sp.t[0] = 10;        // attackAnimationTick
            else if (id == 11) e.sp.t[1] = 400;  // offerFlowerTick
            else if (id == 34) e.sp.t[1] = 0;
            break;
        case Sp::Ravager:
            if (id == 4) e.sp.t[0] = 10;        // attackTick
            else if (id == 39) e.sp.t[1] = 40;  // stunnedTick
            break;
        case Sp::Hoglin:
            if (id == 4) e.sp.t[0] = 10;  // attackAnimationRemainingTicks
            break;
        case Sp::Squid:
            if (id == 19) e.sp.a[3] = 0;  // tentacleMovement
            break;
        case Sp::EvokerFangs:
            if (id == 4) e.sp.t[1] = 1;  // clientSideAttackStarted
            break;
        case Sp::Warden:
            if (id == 4) stop(e, kWardenRoar), start(e, kWardenAttack);
            else if (id == 61) e.sp.t[0] = 10;  // tendrilAnimation
            else if (id == 62) start(e, kWardenSonic);
            break;
        case Sp::Armadillo:
            if (id == 64) e.sp.t[1] = 1;  // peekReceivedClient
            break;
        case Sp::Rabbit:
            if (id == 1) e.sp.t[0] = 0, e.sp.t[1] = 15;  // jumpTicks, jumpDuration
            break;
        case Sp::Creaking:
            if (id == 66) e.sp.t[0] = 8;       // invulnerabilityAnimationRemainingTicks
            else if (id == 4) e.sp.t[1] = 15;  // attackAnimationRemainingTicks
            break;
        case Sp::Arrow:
            break;
        case Sp::ArmorStand:
            if (id == 32) e.sp.t[0] = e.age, e.sp.t[1] = 1;  // ArmorStand.lastHit (the wiggle)
            break;
        default: break;
    }
}

namespace {
bool g_thundering = false;
}
void setSpeciesThundering(bool thundering) { g_thundering = thundering; }
bool speciesThundering() { return g_thundering; }

void speciesJukebox(Entity& e, const int pos[3], bool playing) {
    const Sp sp = species(e);
    if (sp == Sp::Parrot) {  // Parrot.setRecordPlayingNearby
        std::copy(pos, pos + 3, e.sp.jukebox);
        e.sp.partyNearby = playing;
    } else if (sp == Sp::Allay) {  // Allay.setJukeboxPlaying (the server decides the dancing; this keeps the position)
        if (playing) std::copy(pos, pos + 3, e.sp.jukebox);
    }
}

void tickSpecies(Entity& e, i64 gameTime, const std::unordered_map<int, Entity>& entities) {
    SpeciesState& s = e.sp;
    s.gameTime = gameTime;
    const Sp sp = species(e);
    const bool moving = e.walkSpeed > 1e-5f;  // WalkAnimationState.isMoving
    switch (sp) {
        case Sp::Wolf: {  // Wolf.tick: interested head tilt, shaking off water
            approach(e, kInterest, dataInt(e, "DATA_INTERESTED_ID") != 0, 0.4f);
            if (e.inWater) s.t[1] = 1, s.t[0] = 0;  // isWet; a shake is cancelled by the server
            else if (s.t[0]) {
                s.ao[kShake] = s.a[kShake];
                s.a[kShake] += 0.05f;
                if (s.ao[kShake] >= 2.0f) s.t[0] = s.t[1] = 0, s.a[kShake] = s.ao[kShake] = 0;
            }
            break;
        }
        case Sp::Fox: {  // Fox.tick
            const i64 flags = dataInt(e, "DATA_FLAGS_ID");
            approach(e, kFoxInterest, (flags & 8) != 0, 0.4f);
            s.ao[kFoxCrouch] = s.a[kFoxCrouch];
            s.a[kFoxCrouch] = (flags & 4) ? std::min(5.0f, s.a[kFoxCrouch] + 0.2f) : 0.0f;
            break;
        }
        case Sp::Cat: {  // Cat.handleLieDown
            const bool lying = dataInt(e, "IS_LYING") != 0;
            s.ao[kLie] = s.a[kLie], s.ao[kLieTail] = s.a[kLieTail];
            s.a[kLie] = lying ? std::min(1.0f, s.a[kLie] + 0.15f) : std::max(0.0f, s.a[kLie] - 0.22f);
            s.a[kLieTail] = lying ? std::min(1.0f, s.a[kLieTail] + 0.08f) : std::max(0.0f, s.a[kLieTail] - 0.13f);
            stepAmount(e, kRelax, dataInt(e, "RELAX_STATE_ONE") != 0, 0.1f, 0.13f);
            break;
        }
        case Sp::Parrot: {  // Parrot.aiStep: the party ends away from the jukebox; calculateFlapping
            if (s.partyNearby) {
                const double dx = s.jukebox[0] + 0.5 - e.pos[0], dy = s.jukebox[1] + 0.5 - e.pos[1], dz = s.jukebox[2] + 0.5 - e.pos[2];
                if (dx * dx + dy * dy + dz * dz >= 3.46 * 3.46) s.partyNearby = false;
            }
            e.oFlap = e.flap, e.oFlapSpeed = e.flapSpeed;
            const bool air = !e.onGround && e.vehicle < 0;
            e.flapSpeed = std::clamp(e.flapSpeed + (air ? 4.0f : -1.0f) * 0.3f, 0.0f, 1.0f);
            if (!e.onGround && e.flapping < 1.0f) e.flapping = 1.0f;
            e.flapping *= 0.9f;
            e.flap += e.flapping * 2.0f;
            break;
        }
        case Sp::Bee:  // Bee.updateRollAmount (FLAG_ROLL)
            stepAmount(e, kRoll, (dataInt(e, "DATA_FLAGS_ID") & 2) != 0, 0.2f, 0.24f);
            break;
        case Sp::Panda: {  // Panda.tick
            const i64 flags = dataInt(e, "DATA_ID_FLAGS");
            if (flags & 4) {
                if (++s.t[0] > 32) s.t[0] = 0;  // rollCounter (the server ends the roll)
            } else {
                s.t[0] = 0;
            }
            stepAmount(e, kSit, (flags & 8) != 0, 0.15f, 0.19f);
            stepAmount(e, kOnBack, (flags & 16) != 0, 0.15f, 0.19f);
            stepAmount(e, kPandaRoll, (flags & 4) != 0, 0.15f, 0.19f);
            break;
        }
        case Sp::Sheep:  // Sheep.aiStep
            s.t[0] = std::max(0, s.t[0] - 1);
            break;
        case Sp::Goat:  // Goat.aiStep: lowerHeadTick
            s.t[0] = std::clamp(s.t[1] ? s.t[0] + 1 : s.t[0] - 2, 0, 20);
            break;
        case Sp::PolarBear:  // PolarBear.tick (clientSideStandAnimation, 6 ticks)
            s.ao[kBearStand] = s.a[kBearStand];
            s.a[kBearStand] = std::clamp(s.a[kBearStand] + (dataInt(e, "DATA_STANDING_ID") ? 1.0f : -1.0f), 0.0f, 6.0f);
            break;
        case Sp::Horse: {  // AbstractHorse.aiStep / tick: tail swish, eat / stand / mouth animations
            if (irand(200) == 0) s.t[0] = 1;
            if (s.t[0] > 0 && ++s.t[0] > 8) s.t[0] = 0;
            const i64 flags = dataInt(e, "DATA_ID_FLAGS");
            s.ao[kEat] = s.a[kEat];
            s.a[kEat] = (flags & 16) ? std::min(1.0f, s.a[kEat] + ((1.0f - s.a[kEat]) * 0.4f + 0.05f))
                                     : std::max(0.0f, s.a[kEat] + ((0.0f - s.a[kEat]) * 0.4f - 0.05f));
            s.ao[kStand] = s.a[kStand];
            if (flags & 32) {
                s.a[kEat] = s.ao[kEat] = 0;
                s.a[kStand] = std::min(1.0f, s.a[kStand] + ((1.0f - s.a[kStand]) * 0.4f + 0.05f));
            } else {
                const float st = s.a[kStand];
                s.a[kStand] = std::max(0.0f, st + ((0.8f * st * st * st - st) * 0.6f - 0.05f));
            }
            s.ao[kMouth] = s.a[kMouth];
            s.a[kMouth] = (flags & 64) ? std::min(1.0f, s.a[kMouth] + ((1.0f - s.a[kMouth]) * 0.7f + 0.05f))
                                       : std::max(0.0f, s.a[kMouth] + ((0.0f - s.a[kMouth]) * 0.7f - 0.05f));
            break;
        }
        case Sp::Arrow:
            if (s.t[0] > 0) --s.t[0];
            break;
        case Sp::Camel: {  // Camel.tick: dash cooldown; setupAnimationStates
            if (s.t[1] > 0) --s.t[1];
            if (s.t[0] <= 0) s.t[0] = irand(40) + 80, start(e, kCamelIdle);
            else --s.t[0];
            const i64 last = dataInt(e, "LAST_POSE_CHANGE_TICK");
            const bool sitting = last < 0;
            const i64 poseTime = s.gameTime - (last < 0 ? -last : last);
            const bool transition = poseTime < (sitting ? 40 : 52);
            const bool visuallySitting = (poseTime < 0) != sitting;
            if (visuallySitting || sitting) {  // isCamelVisuallySitting (sitting or in its transition)
                stop(e, kCamelSitUp), stop(e, kCamelDash);
                if (sitting && poseTime < 40 && poseTime >= 0) startIfStopped(e, kCamelSit), stop(e, kCamelSitPose);
                else stop(e, kCamelSit), startIfStopped(e, kCamelSitPose);
            } else {
                stop(e, kCamelSit), stop(e, kCamelSitPose);
                animateWhen(e, kCamelDash, dataInt(e, "DASH") != 0);
                animateWhen(e, kCamelSitUp, transition && poseTime >= 0);
            }
            break;
        }
        case Sp::IronGolem:
            if (s.t[0] > 0) --s.t[0];
            if (s.t[1] > 0) --s.t[1];
            break;
        case Sp::Ravager:  // Ravager.aiStep
            if (s.t[2] > 0) --s.t[2];  // roarTick
            if (s.t[0] > 0) --s.t[0];
            if (s.t[1] > 0 && --s.t[1] == 0) s.t[2] = 20;
            break;
        case Sp::Hoglin:
            if (s.t[0] > 0) --s.t[0];
            break;
        case Sp::Guardian: {  // Guardian.aiStep (client): tail and spikes
            const bool movingFlag = dataInt(e, "DATA_ID_MOVING") != 0;
            float& speed = s.a[2];
            s.ao[kTail] = s.a[kTail];
            if (!e.inWater) speed = 2.0f;
            else if (movingFlag) speed = speed < 0.5f ? 4.0f : speed + (0.5f - speed) * 0.1f;
            else speed += (0.125f - speed) * 0.2f;
            s.a[kTail] += speed;
            s.ao[kSpikes] = s.a[kSpikes];
            if (!e.inWater) s.a[kSpikes] = frand();
            else if (movingFlag) s.a[kSpikes] += (0.0f - s.a[kSpikes]) * 0.25f;
            else s.a[kSpikes] += (1.0f - s.a[kSpikes]) * 0.06f;
            const int duration = e.info && e.info->name == "minecraft:elder_guardian" ? 60 : 80;
            if (dataInt(e, "DATA_ID_ATTACK_TARGET") != 0 && s.t[0] < duration) ++s.t[0];
            break;
        }
        case Sp::Squid: {  // Squid.aiStep (client): tentacles and body rotation
            if (s.t[1] == 0) s.t[1] = 1, s.a[4] = 1.0f / (frand() + 1.0f) * 0.2f;  // tentacleSpeed
            s.ao[kXBody] = s.a[kXBody], s.ao[kZBody] = s.a[kZBody];
            s.ao[3] = s.a[3], s.ao[kTentacle] = s.a[kTentacle];
            s.a[3] += s.a[4];
            if (s.a[3] > kPi * 2) s.a[3] = kPi * 2;
            float& rotateSpeed = s.a[5];
            if (e.inWater) {
                if (s.a[3] < kPi) {
                    const float k = s.a[3] / kPi;
                    s.a[kTentacle] = std::sin(k * k * kPi) * kPi * 0.25f;
                    rotateSpeed = k > 0.75f ? 1.0f : rotateSpeed * 0.8f;
                } else {
                    s.a[kTentacle] = 0;
                    rotateSpeed *= 0.99f;
                }
                const double vx = e.vel[0], vy = e.vel[1], vz = e.vel[2];
                const double h = std::sqrt(vx * vx + vz * vz);
                e.bodyYaw += (-float(std::atan2(vx, vz)) * (180.0f / kPi) - e.bodyYaw) * 0.1f;
                s.a[kZBody] += kPi * rotateSpeed * 1.5f;
                s.a[kXBody] += (-float(std::atan2(h, vy)) * (180.0f / kPi) - s.a[kXBody]) * 0.1f;
            } else {
                s.a[kTentacle] = std::abs(std::sin(s.a[3])) * kPi * 0.25f;
                s.a[kXBody] += (-90.0f - s.a[kXBody]) * 0.02f;
            }
            break;
        }
        case Sp::Slime:
        case Sp::MagmaCube: {  // AbstractCubeMob.tick: squish on landing / jumping
            s.ao[kSquish] = s.a[kSquish];
            s.a[kSquish] += (s.a[1] - s.a[kSquish]) * 0.5f;
            if (e.onGround && !s.t[0]) s.a[1] = -0.5f;
            else if (!e.onGround && s.t[0]) s.a[1] = 1.0f;
            s.t[0] = e.onGround;
            s.a[1] *= sp == Sp::MagmaCube ? 0.9f : 0.6f;
            break;
        }
        case Sp::Dragon: {  // EnderDragon.aiStep (client): flap time, flight history
            s.ao[kFlap] = s.a[kFlap];
            if (e.health <= 0) {
                ++s.t[1];  // dragonDeathTime
                break;
            }
            const double hv = std::sqrt(e.vel[0] * e.vel[0] + e.vel[2] * e.vel[2]);
            float flapSpeed = 0.2f / (float(hv) * 10.0f + 1.0f);
            flapSpeed *= float(std::pow(2.0, e.vel[1]));
            const int phase = int(dataInt(e, "DATA_PHASE"));
            if (phase >= 5 && phase <= 7) s.a[kFlap] += 0.1f;  // sitting phases
            else s.a[kFlap] += flapSpeed;
            if (e.mobFlags & 1) {
                s.a[kFlap] = 0.5f;
            } else {  // DragonFlightHistory.record
                const float y = float(e.pos[1]), yRot = wrapDegrees(e.yaw);
                if (s.historyPos < 0)
                    for (auto& h : s.history) h[0] = y, h[1] = yRot;
                if (++s.historyPos == 64) s.historyPos = 0;
                s.history[s.historyPos][0] = y, s.history[s.historyPos][1] = yRot;
            }
            break;
        }
        case Sp::EvokerFangs:  // EvokerFangs.tick (client): life ticks once the bite started
            if (s.t[2] == 0) s.t[2] = 1, s.t[0] = 22;  // lifeTicks
            if (s.t[1]) --s.t[0];
            break;
        case Sp::Allay: {  // Allay.tick (client)
            const bool holding = e.equipment[0] >= 0;
            s.ao[kHolding] = s.a[kHolding];
            s.a[kHolding] = std::clamp(s.a[kHolding] + (holding ? 1.0f : -1.0f), 0.0f, 5.0f);
            if (dataInt(e, "DATA_DANCING")) {
                ++s.t[0];  // dancingAnimationTicks
                s.ao[kSpin] = s.a[kSpin];
                s.a[kSpin] = std::clamp(s.a[kSpin] + (std::fmod(float(s.t[0]), 55.0f) < 15.0f ? 1.0f : -1.0f), 0.0f, 15.0f);
            } else {
                s.t[0] = 0, s.a[kSpin] = s.ao[kSpin] = 0;
            }
            break;
        }
        case Sp::Warden: {  // Warden.tick (client): heartbeat, tendrils
            const float anger = float(dataInt(e, "CLIENT_ANGER_LEVEL")) / 80.0f;
            const int delay = 40 - int(std::floor(std::clamp(anger, 0.0f, 1.0f) * 30.0f));
            if (e.age % delay == 0) s.t[1] = 10;
            s.ao[kTendril] = float(s.t[0]);
            if (s.t[0] > 0) --s.t[0];
            s.a[kTendril] = float(s.t[0]);
            s.ao[kHeart] = float(s.t[1]);
            if (s.t[1] > 0) --s.t[1];
            s.a[kHeart] = float(s.t[1]);
            break;
        }
        case Sp::Armadillo: {  // Armadillo.setupAnimationStates
            switch (int(dataInt(e, "ARMADILLO_STATE"))) {
                case 0: stop(e, kArmadilloRollOut), stop(e, kArmadilloRollUp), stop(e, kArmadilloPeek); break;
                case 1: stop(e, kArmadilloRollOut), startIfStopped(e, kArmadilloRollUp), stop(e, kArmadilloPeek); break;
                case 2:
                    stop(e, kArmadilloRollOut), stop(e, kArmadilloRollUp);
                    if (s.t[1]) stop(e, kArmadilloPeek), s.t[1] = 0;
                    if (s.t[0] == 0) start(e, kArmadilloPeek), s.anim[kArmadilloPeek] -= 50;  // fastForward(50)
                    else startIfStopped(e, kArmadilloPeek);
                    break;
                case 3: startIfStopped(e, kArmadilloRollOut), stop(e, kArmadilloRollUp), stop(e, kArmadilloPeek); break;
                default: break;
            }
            ++s.t[0];
            break;
        }
        case Sp::Frog:
            animateWhen(e, kFrogSwimIdle, e.inWater && !moving);
            break;
        case Sp::Axolotl: {  // Axolotl.baseTick (client)
            const bool dead = dataInt(e, "DATA_PLAYING_DEAD") != 0;
            const bool turning = e.yaw != e.prevYaw || e.pitch != e.prevPitch;
            const bool isMoving = moving || turning;
            auto animator = [&](int i, bool on) {  // BinaryAnimator(10)
                s.ao[i] = s.a[i];
                s.a[i] = on ? std::min(10.0f, s.a[i] + 1) : std::max(0.0f, s.a[i] - 1);
            };
            if (e.baby) {
                animator(kAxoMoving, isMoving);
                auto solo = [&](int k) {
                    for (int i = 0; i <= kAxoPlayDeadAnim; ++i) i == k ? startIfStopped(e, i) : stop(e, i);
                };
                if (dead) solo(kAxoPlayDeadAnim);
                else if (isMoving) solo(e.inWater && !e.onGround ? kAxoSwim : !e.inWater && e.onGround ? kAxoWalk : kAxoWalkWater);
                else solo(e.inWater && !e.onGround ? kAxoIdleWater : e.inWater ? kAxoIdleWaterGround : kAxoIdleGround);
            } else {
                const int state = dead ? 0 : e.inWater ? 1 : e.onGround ? 2 : 3;
                animator(kAxoPlayDead, state == 0);
                animator(kAxoInWater, state == 1);
                animator(kAxoOnGround, state == 2);
                animator(kAxoMoving, isMoving);
            }
            break;
        }
        case Sp::Bat:  // Bat.setupAnimationStates
            if (dataInt(e, "DATA_ID_FLAGS") & 1) stop(e, kBatFly), startIfStopped(e, kBatRest);
            else stop(e, kBatRest), startIfStopped(e, kBatFly);
            break;
        case Sp::Rabbit: {  // Rabbit.aiStep (jump ticks) and baseTick (setupAnimationStates)
            if (s.t[0] != s.t[1]) ++s.t[0];
            else if (s.t[1] != 0) s.t[0] = s.t[1] = 0;
            if (s.t[2] <= 0 && !(e.mobFlags & 1)) s.t[2] = irand(40) + 180, start(e, kRabbitTilt);
            else if (s.t[0] > 0) startIfStopped(e, kRabbitHop), stop(e, kRabbitTilt);
            else --s.t[2], stop(e, kRabbitHop);
            break;
        }
        case Sp::Breeze:  // Breeze.tick
            if (e.pose == 6) startIfStopped(e, kBreezeJump);
            startIfStopped(e, kBreezeIdle);
            if (e.pose != 15 && s.anim[kBreezeSlide] != INT_MIN) start(e, kBreezeSlideBack), stop(e, kBreezeSlide);
            break;
        case Sp::Creaking: {  // Creaking.aiStep / tick (client)
            if (s.t[0] > 0) --s.t[0];
            if (s.t[1] > 0) --s.t[1];
            const bool tearingDown = dataInt(e, "IS_TEARING_DOWN") != 0;
            animateWhen(e, kCreakingAttack, s.t[1] > 0);
            animateWhen(e, kCreakingInvulnerable, s.t[0] > 0);
            animateWhen(e, kCreakingDeath, tearingDown);
            if (tearingDown) {  // tickDeath of a heart-bound creaking; checkEyeBlink
                ++s.t[2];
                if (s.t[2] > s.t[3]) {
                    const int lo = s.t[4] ? 2 : s.t[2] / 4, hi = s.t[4] ? 8 : s.t[2] / 2;
                    s.t[3] = s.t[2] + lo + irand(std::max(1, hi - lo + 1));
                    s.t[4] = !s.t[4];
                }
            }
            break;
        }
        case Sp::CopperGolem: {  // CopperGolem.setupAnimationStates
            if (e.mobFlags & 1) break;
            const int state = int(dataInt(e, "DATA_STATE"));
            if (state == 0) {
                for (int k = kGolemGet; k <= kGolemDropNo; ++k) stop(e, k);
                if (s.t[0] == e.age) start(e, kGolemIdle);
                else if (s.t[0] == 0) s.t[0] = e.age + 200 + irand(40);
                if (e.age == s.t[0] + 10) s.t[0] = 0;
            } else {
                stop(e, kGolemIdle), s.t[0] = 0;
                const int slot = state == 1 ? kGolemGet : state == 2 ? kGolemGetNo : state == 3 ? kGolemDrop : kGolemDropNo;
                for (int k = kGolemGet; k <= kGolemDropNo; ++k) k == slot ? startIfStopped(e, k) : stop(e, k);
            }
            break;
        }
        case Sp::Wither: {  // WitherBoss.aiStep: the side heads turn toward their targets (DATA_TARGET_B / C)
            static const char* kTargets[2] = {"DATA_TARGET_B", "DATA_TARGET_C"};
            auto rotlerp = [](float a, float b, float max) { return a + std::clamp(wrapDegrees(b - a), -max, max); };
            for (int i = 0; i < 2; ++i) {
                s.wither[i][2] = s.wither[i][0], s.wither[i][3] = s.wither[i][1];  // xRotO, yRotO
                const int id = int(dataInt(e, kTargets[i]));
                auto it = id > 0 ? entities.find(id) : entities.end();
                if (it != entities.end()) {
                    const Entity& t = it->second;
                    const float angle = (e.bodyYaw + 180.0f * float(i)) * kPi / 180.0f;
                    const double hx = e.pos[0] + std::cos(angle) * 1.3, hy = e.pos[1] + 2.2, hz = e.pos[2] + std::sin(angle) * 1.3;
                    const double xd = t.pos[0] - hx, yd = t.pos[1] + (t.info ? t.info->eye : 1.62) - hy, zd = t.pos[2] - hz;
                    const double sd = std::sqrt(xd * xd + zd * zd);
                    const float yRotD = float(std::atan2(zd, xd) * 180.0 / kPi) - 90.0f;
                    const float xRotD = float(-(std::atan2(yd, sd) * 180.0 / kPi));
                    s.wither[i][0] = rotlerp(s.wither[i][0], xRotD, 40.0f);
                    s.wither[i][1] = rotlerp(s.wither[i][1], yRotD, 10.0f);
                } else {
                    s.wither[i][1] = rotlerp(s.wither[i][1], e.bodyYaw, 10.0f);
                }
            }
            break;
        }
        default: break;
    }
}

}  // namespace ent
