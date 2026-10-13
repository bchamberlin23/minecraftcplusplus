#include "entity/entity_anim.h"

#include <algorithm>
#include <cmath>

namespace ent {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

struct Parts {
    const Model& m;
    PartPose* p;
    PartPose* operator[](Role r) const {
        const i16 i = m.roles[r];
        return i >= 0 ? &p[i] : nullptr;
    }
};

void head(const Parts& P, const AnimState& s) {
    if (PartPose* h = P[Head]) {
        h->xRot += s.pitch * kDeg;
        h->yRot += s.headYaw * kDeg;
    }
}

float outQuart(float x) { return 1.0f - std::pow(1.0f - x, 4.0f); }

float wrapRad(float a) {
    a = std::fmod(a + kPi, 2.0f * kPi);
    return (a < 0 ? a + 2.0f * kPi : a) - kPi;
}
float rotLerpRad(float t, float from, float to) { return from + t * wrapRad(to - from); }
float lerp(float t, float a, float b) { return a + t * (b - a); }
float quadraticArmUpdate(float x) { return -65.0f * x + x * x; }

// HumanoidModel.setupSwimAnimation (the left arm turns the short way, the right arm linearly, as in the game).
void swim(PartPose* h, PartPose* ra, PartPose* la, PartPose* rl, PartPose* ll, const AnimState& s) {
    const float amount = s.swimAmount, pos = s.walkPos;
    if (h && !s.fallFlying) h->xRot = rotLerpRad(amount, h->xRot, -kPi / 4);
    const float swimPos = std::fmod(pos, 26.0f);
    const bool swinging = s.swing > 0;
    const bool rightSwings = swinging && !s.swingLeft, leftSwings = swinging && s.swingLeft;
    const float ra_ = (s.havePoses && s.rightPose == ArmPose::Spear) || rightSwings ? 0.0f : amount;
    const float la_ = (s.havePoses && s.leftPose == ArmPose::Spear) || leftSwings ? 0.0f : amount;
    if (!s.usingItem && ra && la) {
        if (swimPos < 14.0f) {
            const float q = 1.8707964f * quadraticArmUpdate(swimPos) / quadraticArmUpdate(14.0f);
            la->xRot = rotLerpRad(la_, la->xRot, 0.0f), ra->xRot = lerp(ra_, ra->xRot, 0.0f);
            la->yRot = rotLerpRad(la_, la->yRot, kPi), ra->yRot = lerp(ra_, ra->yRot, kPi);
            la->zRot = rotLerpRad(la_, la->zRot, kPi + q), ra->zRot = lerp(ra_, ra->zRot, kPi - q);
        } else if (swimPos < 22.0f) {
            const float t = (swimPos - 14.0f) / 8.0f;
            la->xRot = rotLerpRad(la_, la->xRot, kPi / 2 * t), ra->xRot = lerp(ra_, ra->xRot, kPi / 2 * t);
            la->yRot = rotLerpRad(la_, la->yRot, kPi), ra->yRot = lerp(ra_, ra->yRot, kPi);
            la->zRot = rotLerpRad(la_, la->zRot, 5.012389f - 1.8707964f * t);
            ra->zRot = lerp(ra_, ra->zRot, 1.2707963f + 1.8707964f * t);
        } else {
            const float t = (swimPos - 22.0f) / 4.0f;
            la->xRot = rotLerpRad(la_, la->xRot, kPi / 2 - kPi / 2 * t), ra->xRot = lerp(ra_, ra->xRot, kPi / 2 - kPi / 2 * t);
            la->yRot = rotLerpRad(la_, la->yRot, kPi), ra->yRot = lerp(ra_, ra->yRot, kPi);
            la->zRot = rotLerpRad(la_, la->zRot, kPi), ra->zRot = lerp(ra_, ra->zRot, kPi);
        }
    }
    if (ll) ll->xRot = lerp(amount, ll->xRot, 0.3f * std::cos(pos * 0.33333334f + kPi));
    if (rl) rl->xRot = lerp(amount, rl->xRot, 0.3f * std::cos(pos * 0.33333334f));
}

void bob(PartPose* arm, float age, float scale) {
    if (!arm) return;
    arm->zRot += scale * (std::cos(age * 0.09f) * 0.05f + 0.05f);
    arm->xRot += scale * (std::sin(age * 0.067f) * 0.05f);
}

void zombieArms(const Parts& P, const AnimState& s);

// AnimationUtils.swingWeaponDown (piglins, illagers).
void swingWeaponDown(PartPose* ra, PartPose* la, bool rightHanded, float attackTime, float age) {
    const float attack2 = std::sin(attackTime * kPi);
    const float attack = std::sin((1.0f - (1.0f - attackTime) * (1.0f - attackTime)) * kPi);
    ra->zRot = 0, la->zRot = 0;
    ra->yRot = kPi / 20, la->yRot = -kPi / 20;
    PartPose* main = rightHanded ? ra : la;
    PartPose* off = rightHanded ? la : ra;
    main->xRot = -1.8849558f + std::cos(age * 0.09f) * 0.15f + attack2 * 2.2f - attack * 0.4f;
    off->xRot = std::cos(age * 0.19f) * 0.5f + attack2 * 1.2f - attack * 0.4f;
    bob(ra, age, 1.0f);
    bob(la, age, -1.0f);
}

// DrownedModel.setupSwimAnimation.
void drownedSwim(PartPose* h, PartPose* ra, PartPose* la, PartPose* rl, PartPose* ll, const AnimState& s) {
    const float amount = s.swimAmount, wave = std::sin(0.1f * s.age);
    if (ra) ra->xRot = rotLerpRad(amount, ra->xRot, -kPi * 4.0f / 5.0f) + amount * 0.35f * wave, ra->zRot = rotLerpRad(amount, ra->zRot, -0.15f);
    if (la) la->xRot = rotLerpRad(amount, la->xRot, -kPi * 4.0f / 5.0f) - amount * 0.35f * wave, la->zRot = rotLerpRad(amount, la->zRot, 0.15f);
    if (ll) ll->xRot -= amount * 0.55f * wave;
    if (rl) rl->xRot += amount * 0.55f * wave;
    if (h) h->xRot = 0;
}

// HumanoidModel.setupAnim (walking, riding, held items, attack swing, crouching, arm bob); kind selects the
// subclasses' setupAttackAnimation (zombie arms, piglin weapon swing) and setupSwimAnimation (drowned).
void humanoid(const Parts& P, const AnimState& s, Anim kind) {
    PartPose *h = P[Head], *body = P[Body], *ra = P[RightArm], *la = P[LeftArm], *rl = P[RightLeg], *ll = P[LeftLeg];
    if (h) h->xRot = s.fallFlying ? -kPi / 4 : s.pitch * kDeg, h->yRot = s.headYaw * kDeg;
    const float pos = s.walkPos, speed = s.walkSpeed / s.speedValue;
    if (ra) ra->xRot = std::cos(pos * 0.6662f + kPi) * 2.0f * speed * 0.5f, ra->zRot = 0;
    if (la) la->xRot = std::cos(pos * 0.6662f) * 2.0f * speed * 0.5f, la->zRot = 0;
    if (rl) rl->xRot = std::cos(pos * 0.6662f) * 1.4f * speed, rl->yRot = 0.005f, rl->zRot = 0.005f;
    if (ll) ll->xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed, ll->yRot = -0.005f, ll->zRot = -0.005f;
    if (s.riding) {
        if (ra) ra->xRot += -kPi / 5;
        if (la) la->xRot += -kPi / 5;
        if (rl) rl->xRot = -1.4137167f, rl->yRot = kPi / 10, rl->zRot = 0.07853982f;
        if (ll) ll->xRot = -1.4137167f, ll->yRot = -kPi / 10, ll->zRot = -0.07853982f;
    }
    if (!s.havePoses) {
        if (ra) ra->yRot = 0;
        if (la) la->yRot = 0;
        if (ra && s.rightItem) ra->xRot = ra->xRot * 0.5f - kPi / 10;
        if (la && s.leftItem) la->xRot = la->xRot * 0.5f - kPi / 10;
    } else if (ra && la && h) {
        // HumanoidModel.poseRightArm / poseLeftArm, the prioritised arm first (the using one, else the main arm when
        // the off hand's pose is two-handed); a pose that affects the other hand skips it.
        auto twoHanded = [](ArmPose p) {
            return p == ArmPose::BowAndArrow || p == ArmPose::CrossbowCharge || p == ArmPose::CrossbowHold;
        };
        auto affectsOther = [&](ArmPose p) { return twoHanded(p) || p == ArmPose::ThrowTrident; };
        auto pose = [&](bool right) {
            PartPose* arm = right ? ra : la;
            const float sign = right ? 1.0f : -1.0f;
            switch (right ? s.rightPose : s.leftPose) {
                case ArmPose::Empty: arm->yRot = 0; break;
                case ArmPose::Item: arm->xRot = arm->xRot * 0.5f - kPi / 10, arm->yRot = 0; break;
                case ArmPose::Block:  // poseBlockingArm
                    arm->xRot = arm->xRot * 0.5f - 0.9424779f + std::clamp(h->xRot, -kPi * 4.0f / 9.0f, 0.43633232f);
                    arm->yRot = (right ? -30.0f : 30.0f) * kDeg + std::clamp(h->yRot, -kPi / 6, kPi / 6);
                    break;
                case ArmPose::BowAndArrow:
                    if (right) ra->yRot = -0.1f + h->yRot, la->yRot = 0.1f + h->yRot + 0.4f;
                    else ra->yRot = -0.1f + h->yRot - 0.4f, la->yRot = 0.1f + h->yRot;
                    ra->xRot = -kPi / 2 + h->xRot, la->xRot = -kPi / 2 + h->xRot;
                    break;
                case ArmPose::ThrowTrident: arm->xRot = arm->xRot * 0.5f - kPi, arm->yRot = 0; break;
                case ArmPose::CrossbowCharge: {  // AnimationUtils.animateCrossbowCharge
                    PartPose* holding = arm;
                    PartPose* pulling = right ? la : ra;
                    holding->yRot = right ? -0.8f : 0.8f;
                    holding->xRot = -0.97079635f;
                    pulling->xRot = holding->xRot;
                    const float a = std::clamp(s.useTicks, 0.0f, s.crossbowCharge) / std::max(s.crossbowCharge, 1.0f);
                    pulling->yRot = (0.4f + (0.85f - 0.4f) * a) * sign;
                    pulling->xRot = pulling->xRot + (-kPi / 2 - pulling->xRot) * a;
                    break;
                }
                case ArmPose::CrossbowHold: {  // AnimationUtils.animateCrossbowHold
                    PartPose* holding = arm;
                    PartPose* shooting = right ? la : ra;
                    holding->yRot = (right ? -0.3f : 0.3f) + h->yRot;
                    shooting->yRot = (right ? 0.6f : -0.6f) + h->yRot;
                    holding->xRot = -kPi / 2 + h->xRot + 0.1f;
                    shooting->xRot = -1.5f + h->xRot;
                    break;
                }
                case ArmPose::Spyglass:
                    arm->xRot = std::clamp(h->xRot - 1.9198622f - (s.crouching ? kPi / 12 : 0.0f), -2.4f, 3.3f);
                    arm->yRot = h->yRot - sign * kPi / 12;
                    break;
                case ArmPose::TootHorn:
                    arm->xRot = std::clamp(h->xRot, -1.2f, 1.2f) - 1.4835298f;
                    arm->yRot = h->yRot - sign * kPi / 6;
                    break;
                case ArmPose::Brush: arm->xRot = arm->xRot * 0.5f - kPi / 5, arm->yRot = 0; break;
                case ArmPose::Spear: {  // SpearAnimations.thirdPersonHandUse
                    arm->yRot = -0.1f * sign + h->yRot;
                    arm->xRot = -kPi / 2 + h->xRot + 0.8f;
                    if (s.fallFlying || s.swimAmount > 0) arm->xRot -= 0.9599311f;
                    arm->yRot = kDeg * std::clamp(arm->yRot / kDeg, -60.0f, 60.0f);
                    arm->xRot = kDeg * std::clamp(arm->xRot / kDeg, -120.0f, 30.0f);
                    const KineticParams& k = right ? s.rightKinetic : s.leftKinetic;
                    if (k.valid && s.useTicks > 0 && (!s.usingItem || s.useRight == right)) {
                        const SpearUse u = spearUse(k, s.useTicks);
                        arm->yRot += -sign * u.swayFast * kDeg * u.swayIntensity;
                        arm->zRot += -sign * u.swaySlow * kDeg * u.swayIntensity * 0.5f;
                        arm->xRot += kDeg * (-40.0f * u.raiseStart + 30.0f * u.raiseMiddle - 20.0f * u.raiseEnd + 20.0f * u.lower +
                                             10.0f * u.raiseBack + 0.6f * u.swaySlow * u.swayIntensity);
                    }
                    break;
                }
            }
        };
        // The off hand is the left arm for right-handed players.
        const bool offIsLeft = !s.leftHanded;
        const ArmPose offPose = offIsLeft ? s.leftPose : s.rightPose;
        const bool prioritiseRight = s.usingItem ? s.useRight : (twoHanded(offPose) ? offIsLeft : !offIsLeft);
        if (prioritiseRight) {
            pose(true);
            if (!affectsOther(s.rightPose)) pose(false);
        } else {
            pose(false);
            if (!affectsOther(s.leftPose)) pose(true);
        }
    }
    // setupAttackAnimation: PiglinModel swings a melee weapon down; otherwise the body turn, then the swing type's arm
    // (WHACK, STAB; NONE moves no arm).
    if (kind == Anim::Piglin && s.swing > 0 && s.mobPose == 1 && ra && la) {
        swingWeaponDown(ra, la, !s.swingLeft, s.swing, s.age);
    } else if (s.swing > 0 && body && ra && la) {
        float by = std::sin(std::sqrt(s.swing) * kPi * 2.0f) * 0.2f;
        if (s.swingLeft) by = -by;
        body->yRot = by;
        const float ageScale = s.baby ? 0.5f : 1.0f;
        ra->z = std::sin(by) * 5.0f * ageScale, ra->x = -std::cos(by) * 5.0f * ageScale;
        la->z = -std::sin(by) * 5.0f * ageScale, la->x = std::cos(by) * 5.0f * ageScale;
        ra->yRot += by, la->yRot += by, la->xRot += by;
    }
    if (s.swing > 0 && body && ra && la && s.swingType == 2) {  // SpearAnimations.thirdPersonAttackHand
        ra->yRot -= body->yRot, la->yRot -= body->yRot, la->xRot -= body->yRot;
        const float prepare = ease::inOutSine(ease::progress(s.swing, 0.0f, 0.05f));
        const float attack = ease::inQuad(ease::progress(s.swing, 0.05f, 0.2f));
        const float retract = ease::inOutExpo(ease::progress(s.swing, 0.4f, 1.0f));
        (s.swingLeft ? la : ra)->xRot += (90.0f * prepare - 120.0f * attack + 30.0f * retract) * kDeg;
    }
    if (s.swing > 0 && body && ra && la && s.swingType == 1) {
        const float by = body->yRot;
        const float aa = std::sin(outQuart(s.swing) * kPi);
        const float bb = std::sin(s.swing * kPi) * -((h ? h->xRot : 0) - 0.7f) * 0.75f;
        PartPose* arm = s.swingLeft ? la : ra;
        arm->xRot -= aa * 1.2f + bb;
        arm->yRot += by * 2.0f;
        arm->zRot += std::sin(s.swing * kPi) * -0.4f;
    }
    if (kind == Anim::Zombie || kind == Anim::Drowned || kind == Anim::ZombifiedPiglin) zombieArms(P, s);
    if (s.crouching) {
        if (body) body->xRot = 0.5f, body->y += 3.2f;
        if (ra) ra->xRot += 0.4f, ra->y += 3.2f;
        if (la) la->xRot += 0.4f, la->y += 3.2f;
        if (rl) rl->z += 4.0f;
        if (ll) ll->z += 4.0f;
        if (h) h->y += 4.2f;
    }
    if (!s.havePoses || s.rightPose != ArmPose::Spyglass) bob(ra, s.age, 1.0f);
    if (!s.havePoses || s.leftPose != ArmPose::Spyglass) bob(la, s.age, -1.0f);
    if (s.swimAmount > 0) kind == Anim::Drowned ? drownedSwim(h, ra, la, rl, ll, s) : swim(h, ra, la, rl, ll, s);
}

// AnimationUtils.animateZombieArms.
void zombieArms(const Parts& P, const AnimState& s) {
    if (s.havePoses && (s.rightPose == ArmPose::Spear || s.leftPose == ArmPose::Spear)) return;
    PartPose *ra = P[RightArm], *la = P[LeftArm];
    // AnimationUtils.animateZombieArms: not with a spear; a trident thrower's right arm keeps its pose.
    if (s.havePoses && (s.rightPose == ArmPose::Spear || s.leftPose == ArmPose::Spear)) return;
    if (s.havePoses && s.rightPose == ArmPose::ThrowTrident) ra = nullptr;
    const bool raise = !s.baby || !(s.leftHanded ? s.leftItem : s.rightItem);  // getMainHandItemStack: the main arm's item
    const float drop = raise ? -kPi / (s.aggressive ? 1.5f : 2.25f) : 0.0f;
    const float ay = (raise ? 1.0f : -1.0f) * std::sin(s.swing * kPi);
    const float ax = std::sin((1.0f - (1.0f - s.swing) * (1.0f - s.swing)) * kPi);
    const float xRot = drop + ay * 1.2f - ax * 0.4f, yRot = 0.1f - ay * 0.6f;
    if (ra && !(s.havePoses && s.rightPose == ArmPose::ThrowTrident)) ra->xRot = xRot, ra->yRot = raise ? -yRot : yRot, ra->zRot = 0;
    if (la && !(s.havePoses && s.leftPose == ArmPose::ThrowTrident)) la->xRot = xRot, la->yRot = raise ? yRot : -yRot, la->zRot = 0;
}

void quadrupedLegs(const Parts& P, const AnimState& s) {
    const float pos = s.walkPos, speed = s.walkSpeed;
    if (PartPose* l = P[RightHindLeg]) l->xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
    if (PartPose* l = P[LeftHindLeg]) l->xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
    if (PartPose* l = P[RightFrontLeg]) l->xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
    if (PartPose* l = P[LeftFrontLeg]) l->xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
}

float triangleWave(float t, float period) {
    return (std::abs(std::fmod(t, period) - period * 0.5f) - period * 0.25f) / (period * 0.25f);
}

}  // namespace

u64 animate(Anim kind, const AnimState& s, const Model& model, PartPose* poses) {
    Parts P{model, poses};
    u64 hidden = 0;
    switch (kind) {
        case Anim::None: break;
        case Anim::Head: head(P, s); break;
        case Anim::Humanoid:
        case Anim::Skeleton:
        case Anim::Zombie:
        case Anim::Drowned:
        case Anim::Piglin:
        case Anim::ZombifiedPiglin:
        case Anim::Enderman:
            humanoid(P, s, kind);
            if (kind == Anim::Skeleton && s.aggressive && !s.holdingBow) {  // SkeletonModel: arms out to strike
                PartPose *ra = P[RightArm], *la = P[LeftArm];
                if (ra && la) {
                    const float swing2 = std::sin(s.swing * kPi);
                    const float swing = std::sin((1.0f - (1.0f - s.swing) * (1.0f - s.swing)) * kPi);
                    ra->zRot = la->zRot = 0;
                    ra->yRot = -(0.1f - swing2 * 0.6f), la->yRot = 0.1f - swing2 * 0.6f;
                    ra->xRot = la->xRot = -kPi / 2 - (swing2 * 1.2f - swing * 0.4f);
                    bob(ra, s.age, 1.0f);
                    bob(la, s.age, -1.0f);
                }
            }
            if (kind != Anim::Humanoid && kind != Anim::Skeleton && kind != Anim::Zombie && kind != Anim::Drowned)
                hidden |= animateSpecies(kind, s, model, poses);  // the subclass' additions
            // Player model customisation: hat 6, jacket 1, sleeves 2/3, pants 4/5.
            if (!(s.skinParts & 64)) hidden |= 1ull << Hat;
            if (!(s.skinParts & 2)) hidden |= 1ull << Jacket;
            if (!(s.skinParts & 4)) hidden |= 1ull << LeftSleeve;
            if (!(s.skinParts & 8)) hidden |= 1ull << RightSleeve;
            if (!(s.skinParts & 16)) hidden |= 1ull << LeftPants;
            if (!(s.skinParts & 32)) hidden |= 1ull << RightPants;
            if (s.spectator)  // PlayerModel.setupAnim: !isSpectator shows the body, arms and legs (their outer layers too)
                for (int part : {Body, RightArm, LeftArm, RightLeg, LeftLeg, Jacket, RightSleeve, LeftSleeve, RightPants, LeftPants})
                    hidden |= 1ull << part;
            break;
        case Anim::Quadruped:
            head(P, s);
            quadrupedLegs(P, s);
            break;
        case Anim::Creeper: {
            // CreeperModel binds its fields crosswise: "left_hind_leg" drives as the right hind leg, etc.
            head(P, s);
            const float pos = s.walkPos, speed = s.walkSpeed;
            if (PartPose* l = P[LeftHindLeg]) l->xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
            if (PartPose* l = P[RightHindLeg]) l->xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
            if (PartPose* l = P[LeftFrontLeg]) l->xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
            if (PartPose* l = P[RightFrontLeg]) l->xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
            break;
        }
        case Anim::Spider: {
            if (PartPose* h = P[Head]) h->yRot = s.headYaw * kDeg, h->xRot = s.pitch * kDeg;
            const float pos = s.walkPos * 0.6662f, speed = s.walkSpeed;
            const float swing[4] = {-(std::cos(pos * 2.0f) * 0.4f) * speed, -(std::cos(pos * 2.0f + kPi) * 0.4f) * speed,
                                    -(std::cos(pos * 2.0f + kPi / 2) * 0.4f) * speed,
                                    -(std::cos(pos * 2.0f + kPi * 3 / 2) * 0.4f) * speed};
            const float step[4] = {std::abs(std::sin(pos) * 0.4f) * speed, std::abs(std::sin(pos + kPi) * 0.4f) * speed,
                                   std::abs(std::sin(pos + kPi / 2) * 0.4f) * speed,
                                   std::abs(std::sin(pos + kPi * 3 / 2) * 0.4f) * speed};
            const Role rights[4] = {RightHindLeg, RightMiddleHindLeg, RightMiddleFrontLeg, RightFrontLeg};
            const Role lefts[4] = {LeftHindLeg, LeftMiddleHindLeg, LeftMiddleFrontLeg, LeftFrontLeg};
            for (int i = 0; i < 4; ++i) {
                if (PartPose* r = P[rights[i]]) r->yRot += swing[i], r->zRot += step[i];
                if (PartPose* l = P[lefts[i]]) l->yRot -= swing[i], l->zRot -= step[i];
            }
            break;
        }
        case Anim::ArmorStand: {
            // ArmorStandArmorModel + ArmorStandModel: every limb follows the synched pose (degrees).
            if (!s.standPose) break;
            auto set = [&](Role r, const float* deg) {
                if (PartPose* p = P[r]) p->xRot = deg[0] * kDeg, p->yRot = deg[1] * kDeg, p->zRot = deg[2] * kDeg;
            };
            set(Head, s.standPose[0]);
            set(Body, s.standPose[1]);
            set(LeftArm, s.standPose[2]);
            set(RightArm, s.standPose[3]);
            set(LeftLeg, s.standPose[4]);
            set(RightLeg, s.standPose[5]);
            set(RightBodyStick, s.standPose[1]);
            set(LeftBodyStick, s.standPose[1]);
            set(ShoulderStick, s.standPose[1]);
            if (PartPose* b = P[BasePlate]) b->yRot = kDeg * -s.headYaw * 0.0f;
            if (!(s.standFlags & 4)) hidden |= (1ull << LeftArm) | (1ull << RightArm);
            if (s.standFlags & 8) hidden |= 1ull << BasePlate;
            break;
        }
        default: return animateSpecies(kind, s, model, poses);
        case Anim::Shulker: {  // ShulkerModel.setupAnim: the lid opens with the peek, the head looks around
            const float bs = (0.5f + s.peek) * kPi, q = -1.0f + std::sin(bs);
            const float extra = bs > kPi ? std::sin(s.age * 0.1f) * 0.7f : 0.0f;
            for (size_t i = 0; i < model.parts.size(); ++i)
                if (model.parts[i].name == "lid") {
                    poses[i].x = 0, poses[i].y = 16.0f + std::sin(bs) * 8.0f + extra, poses[i].z = 0;
                    poses[i].yRot = s.peek > 0.3f ? q * q * q * q * kPi * 0.125f : 0.0f;
                }
            if (PartPose* h = P[Head]) h->xRot = s.pitch * kDeg, h->yRot = (s.headYaw - 180.0f) * kDeg;
            break;
        }
    }
    return hidden;
}

}  // namespace ent
