// Species model animations: ports of the game's per-model setupAnim (net.minecraft.client.model.*) with the state
// their renderers extract (species.cpp keeps the entity-side timers), and the KeyframeAnimation player.
#include "entity/entities.h"
#include "entity/entity_anim.h"
#include "entity/species.h"

#include <algorithm>
#include <cmath>

namespace ent {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

float lerp(float t, float a, float b) { return a + t * (b - a); }
float wrapRad(float a) {
    a = std::fmod(a + kPi, 2.0f * kPi);
    return (a < 0 ? a + 2.0f * kPi : a) - kPi;
}
float rotLerpRad(float t, float from, float to) { return from + t * wrapRad(to - from); }
float wrapDegrees(float d) {
    d = std::fmod(d, 360.0f);
    if (d >= 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}
float rotLerpDeg(float t, float from, float to) { return from + t * wrapDegrees(to - from); }
float triangleWave(float t, float period) {
    return (std::abs(std::fmod(t, period) - period * 0.5f) - period * 0.25f) / (period * 0.25f);
}
float catmullrom(float a, float p0, float p1, float p2, float p3) {
    return 0.5f * (2.0f * p1 + (p2 - p0) * a + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * a * a +
                   (3.0f * p1 - p0 - 3.0f * p2 + p3) * a * a * a);
}

const Entity& defaultEntity() {
    static const Entity e{};
    return e;
}

// A model being posed: parts by name (a scratch pose when the model lacks the part), hiding, keyframes.
struct Ctx {
    const Model& m;
    PartPose* p;
    const AnimState& s;
    const Entity& e;
    float lerpA(int slot) const { return lerp(s.pt, e.sp.ao[slot], e.sp.a[slot]); }
    int idx(std::string_view name) const { return m.part(name); }
    PartPose& operator[](std::string_view name) const {
        const int i = m.part(name);
        if (i >= 0) return p[i];
        thread_local PartPose scratch;
        scratch = PartPose{};
        return scratch;
    }
    PartPose& at(int i) const {
        thread_local PartPose scratch;
        return i >= 0 ? p[i] : (scratch = PartPose{});
    }
    void visible(std::string_view name, bool visible) const {
        const int i = m.part(name);
        if (i >= 0 && s.hide) s.hide[i] = visible ? 0 : 1;
    }
    void skipDraw(std::string_view name, bool skip) const {
        const int i = m.part(name);
        if (i >= 0 && s.hide && s.hide[i] != 1) s.hide[i] = skip ? 2 : 0;
    }
    // The child "root" of the top part (models whose EntityModel root is root.getChild("root")).
    int innerRoot() const {
        for (size_t i = 1; i < m.parts.size(); ++i)
            if (m.parts[i].parent == 0 && m.parts[i].name == "root") return int(i);
        return 0;
    }
    void look(PartPose& h) const { h.xRot = s.pitch * kDeg, h.yRot = s.headYaw * kDeg; }

    const KeyAnimation* anim(const char* name) const {
        if (!s.assets) return nullptr;
        thread_local std::unordered_map<const char*, const KeyAnimation*> cache;
        auto it = cache.find(name);
        if (it == cache.end()) it = cache.emplace(name, s.assets->animation(name)).first;
        return it->second;
    }
    // KeyframeAnimation.apply(AnimationState, ageInTicks, speedFactor)
    void apply(const char* name, int slot, float speed = 1.0f, int root = 0) const {
        const int start = e.sp.anim[slot];
        const KeyAnimation* a = start == INT_MIN ? nullptr : anim(name);
        if (!a) return;
        const i64 ms = i64(float(i64((s.age - float(start)) * 50.0f)) * speed);
        applyKeyframes(*a, m, p, ms, 1.0f, root);
    }
    // KeyframeAnimation.applyWalk
    void walk(const char* name, float pos, float speed, float speedFactor, float scaleFactor, int root = 0) const {
        if (const KeyAnimation* a = anim(name))
            applyKeyframes(*a, m, p, i64(pos * 50.0f * speedFactor), std::min(speed * scaleFactor, 1.0f), root);
    }
    void applyStatic(const char* name, int root = 0) const {
        if (const KeyAnimation* a = anim(name)) applyKeyframes(*a, m, p, 0, 1.0f, root);
    }
};

void quadruped(const Ctx& c) {  // QuadrupedModel.setupAnim
    const float pos = c.s.walkPos, speed = c.s.walkSpeed;
    c.look(c["head"]);
    c["right_hind_leg"].xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
    c["left_hind_leg"].xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
    c["right_front_leg"].xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
    c["left_front_leg"].xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
}

float ageScale(const AnimState& s) { return s.baby ? 0.5f : 1.0f; }

// ---- animals ----

void sheep(const Ctx& c) {  // SheepModel / SheepFurModel: grazing head
    quadruped(c);
    const int tick = c.e.sp.t[0];
    const float a = c.s.pt;
    float position = 0, angle;
    if (tick <= 0) position = 0;
    else if (tick >= 4 && tick <= 36) position = 1;
    else position = tick < 4 ? (float(tick) - a) / 4.0f : -(float(tick) - 40.0f - a) / 4.0f;
    if (tick > 4 && tick <= 36) angle = kPi / 5 + 0.21991149f * std::sin((float(tick) - 4.0f - a) / 32.0f * 28.7f);
    else angle = tick > 0 ? kPi / 5 : c.s.pitch * kDeg;
    PartPose& h = c["head"];
    h.y += position * 9.0f * ageScale(c.s);
    h.xRot = angle;
}

void polarBear(const Ctx& c) {  // PolarBearModel
    quadruped(c);
    const float stand = c.lerpA(kBearStand) / 6.0f, k = stand * stand, age = ageScale(c.s);
    PartPose &body = c["body"], &rf = c["right_front_leg"], &lf = c["left_front_leg"], &h = c["head"];
    body.xRot -= k * kPi * 0.35f;
    body.y += k * age * 2.0f;
    rf.y -= k * age * 20.0f;
    rf.z += k * age * 4.0f;
    rf.xRot -= k * kPi * 0.45f;
    lf.y = rf.y, lf.z = rf.z;
    lf.xRot -= k * kPi * 0.45f;
    h.y -= k * 24.0f;
    h.z += k * 13.0f;
    h.xRot += k * kPi * 0.15f;
}

void goat(const Ctx& c) {  // GoatModel / BabyGoatModel
    quadruped(c);
    c.visible("left_horn", dataInt(c.e, "DATA_HAS_LEFT_HORN", 1) != 0);
    c.visible("right_horn", dataInt(c.e, "DATA_HAS_RIGHT_HORN", 1) != 0);
    const float ram = float(c.e.sp.t[0]) / 20.0f * (c.s.baby ? 52.5f : 30.0f) * kDeg;  // getRammingXHeadRot
    if (ram != 0) c["head"].xRot = ram;
    else if (c.m.baby) c["head"].xRot = kPi / 8;
}

void panda(const Ctx& c) {  // PandaModel / BabyPandaModel
    quadruped(c);
    const float age = c.s.age;
    PartPose &h = c["head"], &body = c["body"], &rf = c["right_front_leg"], &lf = c["left_front_leg"],
             &rh = c["right_hind_leg"], &lh = c["left_hind_leg"];
    if (dataInt(c.e, "UNHAPPY_COUNTER") > 0) {
        h.yRot = 0.35f * std::sin(0.6f * age), h.zRot = 0.35f * std::sin(0.6f * age);
        rf.xRot = -0.75f * std::sin(0.3f * age), lf.xRot = 0.75f * std::sin(0.3f * age);
    } else {
        h.zRot = 0;
    }
    if (dataInt(c.e, "DATA_ID_FLAGS") & 2) {  // sneezing
        const int t = int(dataInt(c.e, "SNEEZE_COUNTER"));
        if (t < 15) h.xRot = -kPi / 4 * float(t) / 14.0f;
        else if (t < 20) h.xRot = -kPi / 4 + kPi / 4 * float((t - 15) / 5);
    }
    const float sit = c.lerpA(kSit);
    if (sit > 0) {
        if (c.m.baby) {  // BabyPandaModel.animateSitting
            body.xRot = rotLerpRad(sit, body.xRot, kPi / 18);
            body.z = lerp(sit, body.z, -1.5f);
            h.z = lerp(sit, h.z, -11.5f), h.y = lerp(sit, h.y, 17.5f);
            rf.z = lerp(sit, rf.z, -5.0f), lf.z = lerp(sit, lf.z, -5.0f);
            rh.z = lerp(sit, rh.z, 3.0f), lh.z = lerp(sit, lh.z, 3.0f);
        } else {
            body.xRot = rotLerpRad(sit, body.xRot, 1.7407963f);
            h.xRot = rotLerpRad(sit, h.xRot, kPi / 2);
        }
        rf.zRot = -0.27079642f, lf.zRot = 0.27079642f, rh.zRot = 0.5707964f, lh.zRot = -0.5707964f;
        if (dataInt(c.e, "EAT_COUNTER") > 0) {
            h.xRot = kPi / 2 + 0.2f * std::sin(age * 0.6f);
            rf.xRot = lf.xRot = -0.4f - 0.2f * std::sin(age * 0.6f);
        }
        if (c.e.variant == 2 && speciesThundering()) {  // scared: a worried panda in a thunderstorm
            h.xRot = 2.1707964f;
            rf.xRot = lf.xRot = -0.9f;
        }
    } else {
        rh.zRot = lh.zRot = rf.zRot = lf.zRot = 0;
    }
    const float back = c.lerpA(kOnBack);
    if (back > 0) {
        rh.xRot = -0.6f * std::sin(age * 0.15f), lh.xRot = 0.6f * std::sin(age * 0.15f);
        rf.xRot = 0.3f * std::sin(age * 0.25f), lf.xRot = -0.3f * std::sin(age * 0.25f);
        h.xRot = rotLerpRad(back, h.xRot, kPi / 2);
    }
    const float roll = c.s.baby ? 0.0f : c.lerpA(kPandaRoll);
    if (roll > 0) {
        h.xRot = rotLerpRad(roll, h.xRot, 2.0561945f);
        rh.xRot = -0.5f * std::sin(age * 0.5f), lh.xRot = 0.5f * std::sin(age * 0.5f);
        rf.xRot = 0.5f * std::sin(age * 0.5f), lf.xRot = -0.5f * std::sin(age * 0.5f);
    }
}

void turtle(const Ctx& c) {  // TurtleModel / AdultTurtleModel
    quadruped(c);
    const float pos = c.s.walkPos, speed = c.s.walkSpeed;
    PartPose &rf = c["right_front_leg"], &lf = c["left_front_leg"], &rh = c["right_hind_leg"], &lh = c["left_hind_leg"];
    if (!c.e.inWater && c.e.onGround) {
        const bool laying = dataInt(c.e, "LAYING_EGG") != 0;
        const float layEgg = laying ? 4.0f : 1.0f, amp = laying ? 2.0f : 1.0f, swing = pos * 5.0f;
        const float front = std::cos(layEgg * swing), hind = std::cos(swing);
        rf.yRot = -front * 8.0f * speed * amp, lf.yRot = front * 8.0f * speed * amp;
        rh.yRot = -hind * 3.0f * speed, lh.yRot = hind * 3.0f * speed;
    } else {
        const float swing = std::cos(pos * 0.6662f * 0.6f) * 0.5f * speed;
        rh.xRot = swing, lh.xRot = -swing, rf.zRot = -swing, lf.zRot = swing;
    }
    if (!c.m.baby) {
        const bool egg = !c.s.baby && dataInt(c.e, "HAS_EGG") != 0;
        c.visible("egg_belly", egg);
        if (egg) c.p[0].y -= 1.0f;
    }
}

void hoglin(const Ctx& c) {  // HoglinModel / BabyHoglinModel
    const float speed = c.s.walkSpeed, pos = c.s.walkPos;
    PartPose& h = c["head"];
    c["right_ear"].zRot = -kPi * 2.0f / 9.0f - speed * std::sin(pos);
    c["left_ear"].zRot = kPi * 2.0f / 9.0f + speed * std::sin(pos);
    h.yRot = c.s.headYaw * kDeg;
    const float butt = 1.0f - float(std::abs(10 - 2 * c.e.sp.t[0])) / 10.0f;
    h.xRot = lerp(butt, 0.87266463f, -kPi / 9);
    if (c.m.baby) h.y += butt * 2.5f;
    PartPose &rf = c["right_front_leg"], &lf = c["left_front_leg"];
    rf.xRot = std::cos(pos) * 1.2f * speed;
    lf.xRot = std::cos(pos + kPi) * 1.2f * speed;
    c["right_hind_leg"].xRot = lf.xRot;
    c["left_hind_leg"].xRot = rf.xRot;
}

// AbstractEquineModel (+ BabyHorseModel, DonkeyModel, BabyDonkeyModel); variant: 0 horse, 1 baby horse, 2 baby donkey.
void equine(const Ctx& c) {
    const int variant = !c.m.baby ? 0 : c.m.name.find("donkey") != std::string::npos || c.m.name.find("mule") != std::string::npos ? 2 : 1;
    const float clampedYRot = std::clamp(c.s.headYaw, -20.0f, 20.0f);
    float headRotX = c.s.pitch * kDeg;
    const float speed = c.s.walkSpeed, pos = c.s.walkPos;
    if (speed > 0.2f) headRotX += std::cos(pos * 0.8f) * 0.15f * speed;
    const float eating = c.lerpA(kEat), standing = c.lerpA(kStand), iStanding = 1.0f - standing;
    const float feeding = c.lerpA(kMouth), age = c.s.age;
    PartPose &head = c["head_parts"], &body = c["body"], &tail = c["tail"];
    PartPose &lf = c["left_front_leg"], &rf = c["right_front_leg"], &lh = c["left_hind_leg"], &rh = c["right_hind_leg"];
    head.xRot = kPi / 6 + headRotX;
    head.yRot = clampedYRot * kDeg;
    const float water = c.e.inWater ? 0.2f : 1.0f;
    const float leg1 = std::cos(water * pos * 0.6662f + kPi), legX = leg1 * 0.8f * speed;
    const float base = (1.0f - std::max(standing, eating)) * (kPi / 6 + headRotX + feeding * std::sin(age) * 0.05f);
    head.xRot = standing * (kPi / 12 + headRotX) + eating * (2.1816616f + std::sin(age) * 0.05f) + base;
    head.yRot = standing * clampedYRot * kDeg + (1.0f - std::max(standing, eating)) * head.yRot;
    if (variant == 2) {  // BabyDonkeyModel.animateHeadPartsPlacement
        head.y = lerp(eating, head.y, -1.2f);
        head.z = lerp(standing, head.z, -3.6f);
    } else {
        head.y += lerp(eating, lerp(standing, 0.0f, variant == 1 ? -2.0f : -8.0f), variant == 1 ? 2.0f : 7.0f);
        head.z = lerp(standing, head.z, -4.0f);
    }
    body.xRot = standing * (-kPi / 4) + iStanding * body.xRot;
    const float standY = variant == 0 ? 12.0f : variant == 1 ? 4.0f : 1.0f, standZ = variant == 0 ? 4.0f : variant == 1 ? 0.0f : 0.5f;
    lf.y -= standY * standing;
    lf.z += standZ * standing;
    rf.y = lf.y, rf.z = lf.z;
    const float standAngle = (variant == 2 ? kPi / 3 : kPi / 12) * standing;
    const float bob = std::cos(age * 0.6f + kPi);
    const float standXRot = variant == 2 ? 0.0f : -kPi / 3;
    const float rlegRot = (standXRot + bob) * standing + legX * iStanding;
    const float llegRot = (standXRot - bob) * standing - legX * iStanding;
    lh.xRot = standAngle - leg1 * 0.5f * speed * iStanding;
    rh.xRot = standAngle + leg1 * 0.5f * speed * iStanding;
    lf.xRot = rlegRot;
    rf.xRot = llegRot;
    if (variant == 2) {  // offsetLegPositionWhenStanding
        lh.y = lerp(standing, lh.y, -0.3f);
        rh.y = lerp(standing, lh.y, -0.3f);
    }
    const float tailOffset = variant == 0 ? 0.0f : variant == 1 ? -kPi / 2 : -kPi / 4;
    tail.xRot = tailOffset + kPi / 6 + speed * 0.75f;
    tail.y += speed * ageScale(c.s);
    tail.z += speed * 2.0f * ageScale(c.s);
    tail.yRot = c.e.sp.t[0] > 0 ? std::cos(age * 0.7f) : 0.0f;
    if (variant == 2) {  // BabyDonkeyModel.setupAnim: the head again with a fixed -30 degree pitch
        const float hx = -30.0f * kDeg;
        const float b = (1.0f - std::max(standing, eating)) * (kPi / 6 + hx + feeding * std::sin(age) * 0.05f);
        head.xRot = standing * (kPi / 12 + hx) + eating * (kPi / 2 + std::sin(age) * 0.05f) + b;
    }
}

void camel(const Ctx& c) {  // CamelModel (adult / baby animation sets), CamelSaddleModel
    const bool baby = c.m.baby;
    float yRot = std::clamp(c.s.headYaw, -30.0f, 30.0f), xRot = std::clamp(c.s.pitch, -25.0f, 45.0f);
    const float cooldown = std::max(float(c.e.sp.t[1]) - c.s.pt, 0.0f);
    if (cooldown > 0) xRot = std::clamp(xRot + 45.0f * cooldown / 55.0f, -25.0f, 70.0f);
    PartPose& h = c["head"];
    h.yRot = yRot * kDeg, h.xRot = xRot * kDeg;
    c.walk(baby ? "CamelBabyAnimation.CAMEL_BABY_WALK" : "CamelAnimation.CAMEL_WALK", c.s.walkPos, c.s.walkSpeed, 2.0f, 2.5f);
    c.apply(baby ? "CamelBabyAnimation.CAMEL_BABY_SIT" : "CamelAnimation.CAMEL_SIT", kCamelSit);
    c.apply(baby ? "CamelBabyAnimation.CAMEL_BABY_SIT_POSE" : "CamelAnimation.CAMEL_SIT_POSE", kCamelSitPose);
    c.apply(baby ? "CamelBabyAnimation.CAMEL_BABY_STANDUP" : "CamelAnimation.CAMEL_STANDUP", kCamelSitUp);
    c.apply(baby ? "CamelBabyAnimation.CAMEL_BABY_IDLE" : "CamelAnimation.CAMEL_IDLE", kCamelIdle);
    c.apply(baby ? "CamelBabyAnimation.CAMEL_BABY_DASH" : "CamelAnimation.CAMEL_DASH", kCamelDash);
    c.visible("reins", c.s.riding);  // riding: the camel carries a passenger (set by the renderer)
}

void feline(const Ctx& c) {  // AdultFelineModel / BabyFelineModel
    const bool cat = c.e.info && c.e.info->name == "minecraft:cat";
    const bool crouching = c.e.pose == 5, sprinting = (c.e.sharedFlags & 8) != 0;
    const bool sitting = cat && (dataInt(c.e, "DATA_FLAGS_ID") & 1);
    const float lie = cat ? c.lerpA(kLie) : 0.0f, lieTail = cat ? c.lerpA(kLieTail) : 0.0f, relax = cat ? c.lerpA(kRelax) : 0.0f;
    const float age = ageScale(c.s);
    PartPose &body = c["body"], &head = c["head"], &t1 = c["tail1"], &t2 = c["tail2"];
    PartPose &lh = c["left_hind_leg"], &rh = c["right_hind_leg"], &lf = c["left_front_leg"], &rf = c["right_front_leg"];
    if (crouching) {
        body.y += 1.0f * age, head.y += 2.0f * age, t1.y += 1.0f * age, t2.y += -4.0f * age, t2.z += 2.0f * age;
        t1.xRot = kPi / 2, t2.xRot = kPi / 2;
    } else if (sprinting) {
        t2.y = t1.y, t2.z += 2.0f * age;
        t1.xRot = kPi / 2, t2.xRot = kPi / 2;
    }
    c.look(head);
    if (!sitting) {
        if (!c.m.baby) body.xRot = kPi / 2;
        const float speed = c.s.walkSpeed, pos = c.s.walkPos;
        if (sprinting) {
            lh.xRot = std::cos(pos * 0.6662f) * speed;
            rh.xRot = std::cos(pos * 0.6662f + 0.3f) * speed;
            lf.xRot = std::cos(pos * 0.6662f + kPi + 0.3f) * speed;
            rf.xRot = std::cos(pos * 0.6662f + kPi) * speed;
            t2.xRot = 1.7278761f + kPi / 10 * std::cos(pos) * speed;
        } else {
            lh.xRot = std::cos(pos * 0.6662f) * speed;
            rh.xRot = std::cos(pos * 0.6662f + kPi) * speed;
            lf.xRot = std::cos(pos * 0.6662f + kPi) * speed;
            rf.xRot = std::cos(pos * 0.6662f) * speed;
            t2.xRot = 1.7278761f + (crouching ? 0.47123894f : kPi / 4) * std::cos(pos) * speed;
        }
    }
    if (sitting) {
        if (c.m.baby) {
            body.xRot += -0.43633232f, body.y += 1.25f, head.z += 0.75f;
            t1.xRot += 0.5454154f, t1.y += 4.0f, t1.z -= 0.9f;
            lh.z -= 0.9f, rh.z -= 0.9f;
        } else {
            body.xRot = kPi / 4, body.y += -4.0f * age, body.z += 5.0f * age;
            head.y += -3.3f * age, head.z += 1.0f * age;
            t1.y += 8.0f * age, t1.z += -2.0f * age, t2.y += 2.0f * age, t2.z += -0.8f * age;
            t1.xRot = 1.7278761f, t2.xRot = 2.670354f;
            lf.xRot = -kPi / 20, lf.y += 2.0f * age, lf.z -= 2.0f * age;
            rf.xRot = -kPi / 20, rf.y += 2.0f * age, rf.z -= 2.0f * age;
            lh.xRot = -kPi / 2, lh.y += 3.0f * age, lh.z -= 4.0f * age;
            rh.xRot = -kPi / 2, rh.y += 3.0f * age, rh.z -= 4.0f * age;
        }
    }
    if (lie > 0) {
        if (c.m.baby) {
            body.x += 1;
            head.xRot = rotLerpDeg(lie, head.xRot, kPi / 18);
            head.zRot = rotLerpDeg(lie, head.zRot, -kPi * 5.0f / 12.0f);
            head.x += 1.5f, head.y += 0.75f, head.z -= 0.5f;
            rf.xRot = -kPi / 4, rf.x += 3.5f, rf.y -= 0.5f;
            lf.xRot = -kPi / 2, lf.x += 1.5f, lf.y -= 1, lf.z -= 2.0f;
            rh.xRot = kPi * 2.0f / 9.0f, rh.yRot = kPi / 9, rh.zRot = -kPi / 9;
            rh.x += 2.5f, rh.y -= 0.25f, rh.z += 0.5f;
            lh.x += 1.5f, lh.z -= 1;
            t1.xRot = t1.xRot + rotLerpDeg(lieTail, t1.xRot, -kPi / 6);
            t1.yRot = t1.yRot + rotLerpDeg(lieTail, t1.yRot, 0.0f);
            t1.zRot = t1.zRot + rotLerpDeg(lieTail, t1.zRot, -kPi / 18);
            t1.x += 1, t1.y += 0.5f, t1.z -= 0.25f;
        } else {
            head.zRot = rotLerpDeg(lie, head.zRot, -1.2707963f);
            head.yRot = rotLerpDeg(lie, head.yRot, 1.2707963f);
            lf.xRot = -1.2707963f;
            rf.xRot = -0.47079635f, rf.zRot = -0.2f, rf.x += age;
            lh.xRot = -0.4f;
            rh.xRot = 0.5f, rh.zRot = -0.5f, rh.x += 0.8f * age, rh.y += 2.0f * age;
            t1.xRot = rotLerpDeg(lieTail, t1.xRot, 0.8f);
            t2.xRot = rotLerpDeg(lieTail, t2.xRot, -0.4f);
        }
    }
    if (relax > 0) head.xRot = rotLerpDeg(relax, head.xRot, -0.58177644f);
}

float wolfBodyRoll(float shake, float offset) {  // WolfRenderState.getBodyRollAngle
    const float progress = std::clamp((shake + offset) / 1.8f, 0.0f, 1.0f);
    return std::sin(progress * kPi) * std::sin(progress * kPi * 11.0f) * 0.15f * kPi;
}

void wolf(const Ctx& c) {  // WolfModel / AdultWolfModel / BabyWolfModel
    const float pos = c.s.walkPos, speed = c.s.walkSpeed, age = ageScale(c.s);
    const bool angry = c.e.angry, sitting = (dataInt(c.e, "DATA_FLAGS_ID") & 1) != 0;
    PartPose &head = c["head"], &body = c["body"], &tail = c["tail"];
    PartPose &rh = c["right_hind_leg"], &lh = c["left_hind_leg"], &rf = c["right_front_leg"], &lf = c["left_front_leg"];
    tail.yRot = angry ? 0.0f : std::cos(pos * 0.6662f) * 1.4f * speed;
    if (sitting) {  // setSittingPose
        body.y += 4.0f * age, body.z -= 2.0f * age, body.xRot = kPi / 4;
        tail.y += 9.0f * age, tail.z -= 2.0f * age;
        rh.y += 6.7f * age, rh.z -= 5.0f * age, rh.xRot = kPi * 3.0f / 2.0f;
        lh.y += 6.7f * age, lh.z -= 5.0f * age, lh.xRot = kPi * 3.0f / 2.0f;
        rf.xRot = 5.811947f, rf.x += 0.01f * age, rf.y += 1.0f * age;
        lf.xRot = 5.811947f, lf.x -= 0.01f * age, lf.y += 1.0f * age;
        if (c.m.baby) {
            body.xRot -= kPi / 2;
        } else {
            PartPose& upper = c["upper_body"];
            upper.y += 2.0f, upper.xRot = kPi * 2.0f / 5.0f, upper.yRot = 0;
        }
    } else {
        rh.xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
        lh.xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
        rf.xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
        lf.xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
    }
    // shakeOffWater
    const float shake = c.lerpA(kShake);
    const float headRoll = c.lerpA(kInterest) * 0.15f * kPi;
    body.zRot = wolfBodyRoll(shake, -0.16f);
    if (c.m.baby) {
        head.zRot = headRoll + wolfBodyRoll(shake, 0.0f);
        tail.zRot = wolfBodyRoll(shake, -0.2f);
    } else {
        c["real_head"].zRot = headRoll + wolfBodyRoll(shake, 0.0f);
        c["upper_body"].zRot = wolfBodyRoll(shake, -0.08f);
        c["real_tail"].zRot = wolfBodyRoll(shake, -0.2f);
    }
    c.look(head);
    // Wolf.getTailAngle (tame: by health, 40 max)
    tail.xRot = angry ? 1.5393804f : c.e.tame ? (0.55f - (40.0f - c.e.health) / 40.0f * 0.4f) * kPi : kPi / 5;
}

void fox(const Ctx& c) {  // FoxModel / AdultFoxModel / BabyFoxModel
    const i64 flags = dataInt(c.e, "DATA_FLAGS_ID");
    const bool sitting = flags & 1, crouching = flags & 4, pouncing = flags & 16, sleeping = flags & 32, faceplanted = flags & 64;
    const float crouch = c.lerpA(kFoxCrouch), age = ageScale(c.s), headRoll = c.lerpA(kFoxInterest) * 0.11f * kPi;
    PartPose &head = c["head"], &body = c["body"], &tail = c["tail"];
    PartPose &rh = c["right_hind_leg"], &lh = c["left_hind_leg"], &rf = c["right_front_leg"], &lf = c["left_front_leg"];
    const float speed = c.s.walkSpeed, pos = c.s.walkPos;
    // setWalkingPose
    head.zRot = headRoll;
    if (c.m.baby) {
        c.walk("FoxBabyAnimation.FOX_BABY_WALK", pos, speed, 1.0f, 2.5f);
    } else {
        rh.xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
        lh.xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
        rf.xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
        lf.xRot = std::cos(pos * 0.6662f) * 1.4f * speed;
    }
    if (crouching) {  // setCrouchingPose
        body.xRot += 0.10471976f;
        head.y += crouch * age;
        const float wiggle = std::cos(c.s.age) * 0.05f;
        body.yRot = wiggle, rh.zRot = wiggle, lh.zRot = wiggle, rf.zRot = wiggle / 2.0f, lf.zRot = wiggle / 2.0f;
        body.y += c.m.baby ? crouch / 6.0f : crouch;
    } else if (sleeping) {  // setSleepingPose
        c.visible("right_hind_leg", false), c.visible("left_hind_leg", false);
        c.visible("right_front_leg", false), c.visible("left_front_leg", false);
        if (c.m.baby) {
            body.zRot = -kPi / 2, body.xRot = -kPi / 18, body.y += 1.5f, body.z -= 1.5f, body.x -= 1.5f;
            tail.xRot = -2.1816616f, tail.x -= 0.7f, tail.z += 0.6f, tail.y += 0.9f;
            head.x -= 2.0f, head.y += 2.8f, head.z -= 4.0f;
        } else {
            body.zRot = -kPi / 2, body.y += 5.0f;
            tail.xRot = -kPi * 5.0f / 6.0f;
            head.x += 2.0f, head.y += 2.99f;
        }
        head.yRot = -kPi * 2.0f / 3.0f, head.zRot = 0;
    } else if (sitting) {  // setSittingPose
        head.xRot = 0, head.yRot = 0;
        if (c.m.baby) {
            body.xRot = -0.959931f, body.z -= 4.5f * age, body.y += 3.0f * age;
            tail.y -= 0.6f, tail.z -= 2.0f * age, tail.xRot = 0.95993114f;
            head.y -= 0.75f;
            rf.xRot = lf.xRot = -kPi / 12;
            rf.z -= 1.5f, lf.z -= 1.5f, rf.x += 0.01f, lf.x -= 0.01f;
            rh.z -= 3.75f, lh.z -= 3.75f, rh.x += 0.01f, lh.x -= 0.01f;
        } else {
            body.xRot = kPi / 6, body.y -= 7.0f, body.z += 3.0f;
            tail.xRot = kPi / 4;
            head.y -= 6.5f, head.z += 2.75f;
            rf.xRot = lf.xRot = -kPi / 12;
            rh.xRot = -kPi * 5.0f / 12.0f, rh.y += 4.0f, rh.z -= 0.25f;
            lh.xRot = -kPi * 5.0f / 12.0f, lh.y += 4.0f, lh.z -= 0.25f;
            tail.z -= 1;
        }
    }
    if (pouncing && !c.m.baby) {  // AdultFoxModel.setPouncingPose
        body.y -= crouch / 2.0f;
        head.y -= crouch / 2.0f;
    }
    if (!sleeping && !faceplanted && !crouching) c.look(head);
    if (sleeping) {
        head.xRot = 0, head.yRot = -kPi * 2.0f / 3.0f;
        head.zRot = std::cos(c.s.age * 0.027f) / 22.0f;
    }
    if (faceplanted) {  // legMotionPos advances 0.67 per drawn frame; ~3 frames per tick here
        const float legPos = c.s.age * 2.0f;
        rh.xRot = std::cos(legPos * 0.4662f) * 0.1f;
        lh.xRot = std::cos(legPos * 0.4662f + kPi) * 0.1f;
        rf.xRot = std::cos(legPos * 0.4662f + kPi) * 0.1f;
        lf.xRot = std::cos(legPos * 0.4662f) * 0.1f;
    }
}

void rabbit(const Ctx& c) {  // RabbitModel (adult / baby animations)
    const bool baby = c.m.baby;
    if (c.e.sp.anim[kRabbitTilt] == INT_MIN) c.look(c["head"]);
    c.apply(baby ? "BabyRabbitAnimation.HOP" : "RabbitAnimation.HOP", kRabbitHop);
    c.apply(baby ? "BabyRabbitAnimation.IDLE_HEAD_TILT" : "RabbitAnimation.IDLE_HEAD_TILT", kRabbitTilt);
}

void parrot(const Ctx& c) {  // ParrotModel: FLYING / STANDING / SITTING / PARTY
    enum Pose { Flying, Standing, Sitting, Party };
    const Pose pose = c.e.sp.partyNearby ? Party : (dataInt(c.e, "DATA_FLAGS_ID") & 1) ? Sitting : !c.e.onGround ? Flying : Standing;
    PartPose &body = c["body"], &tail = c["tail"], &lw = c["left_wing"], &rw = c["right_wing"], &head = c["head"],
             &ll = c["left_leg"], &rl = c["right_leg"];
    // prepare
    if (pose == Flying) ll.xRot += kPi * 2.0f / 9.0f, rl.xRot += kPi * 2.0f / 9.0f;
    else if (pose == Sitting) {
        head.y += 1.9f, tail.xRot += kPi / 6, tail.y += 1.9f, body.y += 1.9f;
        lw.zRot = -0.0873f, lw.y += 1.9f, rw.zRot = 0.0873f, rw.y += 1.9f;
        ll.y += 1.9f, rl.y += 1.9f, ll.xRot += kPi / 2, rl.xRot += kPi / 2;
    } else if (pose == Party) {
        ll.zRot = -kPi / 9, rl.zRot = kPi / 9;
    }
    c.look(head);
    const float flapAngle = (std::sin(lerp(c.s.pt, c.e.oFlap, c.e.flap)) + 1.0f) * lerp(c.s.pt, c.e.oFlapSpeed, c.e.flapSpeed);
    const float pos = c.s.walkPos, speed = c.s.walkSpeed;
    if (pose == Standing) {
        ll.xRot += std::cos(pos * 0.6662f) * 1.4f * speed;
        rl.xRot += std::cos(pos * 0.6662f + kPi) * 1.4f * speed;
    }
    if (pose == Standing || pose == Flying) {
        const float bob = flapAngle * 0.3f;
        head.y += bob;
        tail.xRot += std::cos(pos * 0.6662f) * 0.3f * speed;
        tail.y += bob, body.y += bob;
        lw.zRot = -0.0873f - flapAngle, lw.y += bob;
        rw.zRot = 0.0873f + flapAngle, rw.y += bob;
        ll.y += bob, rl.y += bob;
    } else if (pose == Party) {
        const float x = std::cos(c.s.age), y = std::sin(c.s.age);
        head.x += x, head.y += y, head.xRot = 0, head.yRot = 0, head.zRot = std::sin(c.s.age) * 0.4f;
        body.x += x, body.y += y;
        lw.zRot = -0.0873f - flapAngle, lw.x += x, lw.y += y;
        rw.zRot = 0.0873f + flapAngle, rw.x += x, rw.y += y;
        tail.x += x, tail.y += y;
    }
}

void bee(const Ctx& c) {  // BeeModel / AdultBeeModel
    const i64 flags = dataInt(c.e, "DATA_FLAGS_ID");
    c.visible("stinger", !(flags & 4));
    const double v2 = c.e.vel[0] * c.e.vel[0] + c.e.vel[1] * c.e.vel[1] + c.e.vel[2] * c.e.vel[2];
    const bool onGround = c.e.onGround && v2 < 1e-7;
    PartPose &bone = c["bone"], &rw = c["right_wing"], &lw = c["left_wing"], &front = c["front_legs"],
             &mid = c["middle_legs"], &back = c["back_legs"];
    if (!onGround) {
        const float speed = c.s.age * 120.32113f * kDeg;
        rw.yRot = 0, rw.zRot = std::cos(speed) * kPi * 0.15f;
        lw.xRot = rw.xRot, lw.yRot = rw.yRot, lw.zRot = -rw.zRot;
        front.xRot = mid.xRot = back.xRot = kPi / 4;
    }
    if (!c.e.angry && !onGround) {  // bobUpAndDown
        const float speed = std::cos(c.s.age * 0.18f);
        bone.xRot = 0.1f + speed * kPi * 0.025f;
        bone.y -= std::cos(c.s.age * 0.18f) * 0.9f;
        front.xRot = -speed * kPi * 0.1f + kPi / 8;
        back.xRot = -speed * kPi * 0.05f + kPi / 4;
        if (!c.m.baby) c["left_antenna"].xRot = c["right_antenna"].xRot = speed * kPi * 0.03f;
    }
    const float roll = c.lerpA(kRoll);
    if (roll > 0) bone.xRot = rotLerpRad(roll, bone.xRot, 3.0915928f);
}

void allay(const Ctx& c) {  // AllayModel (its model root is the "root" child)
    PartPose& root = c.at(c.innerRoot());
    PartPose &head = c["head"], &body = c["body"], &ra = c["right_arm"], &la = c["left_arm"], &rw = c["right_wing"],
             &lw = c["left_wing"];
    const float speed = c.s.walkSpeed, pos = c.s.walkPos, age = c.s.age;
    const float flapSpeed = age * 20.0f * kDeg + pos;
    const float flapAmount = std::cos(flapSpeed) * kPi * 0.15f + speed;
    const float idleBobSpeed = age * 9.0f * kDeg;
    const float flying = std::min(speed / 0.3f, 1.0f), idleBob = 1.0f - flying;
    const float holding = c.lerpA(kHolding) / 5.0f;
    if (dataInt(c.e, "DATA_DANCING")) {
        const float danceSpeed = age * 8.0f * kDeg + speed;
        const float freq = std::cos(danceSpeed) * 16.0f * kDeg, spin = c.lerpA(kSpin) / 15.0f;
        const float tiltZ = std::cos(danceSpeed) * 14.0f * kDeg, tiltY = std::cos(danceSpeed) * 30.0f * kDeg;
        if (std::fmod(float(c.e.sp.t[0]), 55.0f) < 15.0f) root.yRot = kPi * 4 * spin;  // isSpinning
        root.zRot = freq * (1.0f - spin);
        head.yRot = tiltY * (1.0f - spin), head.zRot = tiltZ * (1.0f - spin);
    } else {
        c.look(head);
    }
    rw.xRot = 0.43633232f * (1.0f - flying), rw.yRot = -kPi / 4 + flapAmount;
    lw.xRot = 0.43633232f * (1.0f - flying), lw.yRot = kPi / 4 - flapAmount;
    body.xRot = flying * kPi / 4;
    const float armFly = holding * lerp(flying, -kPi / 3, -1.134464f);
    root.y += std::cos(idleBobSpeed) * 0.25f * idleBob;
    ra.xRot = armFly, la.xRot = armFly;
    const float armBob = idleBob * (1.0f - holding);
    const float armBobAmount = 0.43633232f - std::cos(idleBobSpeed + kPi * 3.0f / 2.0f) * kPi * 0.075f * armBob;
    la.zRot = -armBobAmount, ra.zRot = armBobAmount;
    ra.yRot = 0.27925268f * holding, la.yRot = -0.27925268f * holding;
}

void bat(const Ctx& c) {  // BatModel
    if (dataInt(c.e, "DATA_ID_FLAGS") & 1) c["head"].yRot = c.s.headYaw * kDeg;
    c.apply("BatAnimation.BAT_FLYING", kBatFly);
    c.apply("BatAnimation.BAT_RESTING", kBatRest);
}

void chicken(const Ctx& c) {  // ChickenModel / AdultChickenModel (the baby does not turn its head)
    const float flap = (std::sin(c.s.flap) + 1.0f) * c.s.flapSpeed;
    c["right_leg"].xRot = std::cos(c.s.walkPos * 0.6662f) * 1.4f * c.s.walkSpeed;
    c["left_leg"].xRot = std::cos(c.s.walkPos * 0.6662f + kPi) * 1.4f * c.s.walkSpeed;
    c["right_wing"].zRot = flap;
    c["left_wing"].zRot = -flap;
    if (!c.m.baby) c.look(c["head"]);
}

void dolphin(const Ctx& c) {  // DolphinModel
    PartPose& body = c["body"];
    body.xRot = c.s.pitch * kDeg, body.yRot = c.s.headYaw * kDeg;
    if (c.e.vel[0] * c.e.vel[0] + c.e.vel[2] * c.e.vel[2] > 1e-7) {
        body.xRot += -0.05f - 0.05f * std::cos(c.s.age * 0.3f);
        c["tail"].xRot = -0.1f * std::cos(c.s.age * 0.3f);
        c["tail_fin"].xRot = -0.2f * std::cos(c.s.age * 0.3f);
    }
}

void squid(const Ctx& c) {  // SquidModel
    const float angle = c.lerpA(kTentacle);
    for (int i = 0; i < 8; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "tentacle%d", i);
        c[name].xRot = angle;
    }
}

void guardian(const Ctx& c) {  // GuardianModel
    static const float kX[12] = {0, 0, 8, -8, -8, 8, 8, -8, 0, 0, 8, -8};
    static const float kY[12] = {-8, -8, -8, -8, 0, 0, 0, 0, 8, 8, 8, 8};
    static const float kZ[12] = {8, -8, 0, 0, -8, -8, 8, 8, 8, -8, 0, 0};
    PartPose& head = c["head"];
    c.look(head);
    const float withdrawal = (1.0f - c.lerpA(kSpikes)) * 0.55f;
    for (int i = 0; i < 12; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "spike%d", i);
        const float off = 1.0f + std::cos(c.s.age * 1.5f + float(i)) * 0.01f - withdrawal;
        PartPose& sp = c[name];
        sp.x = kX[i] * off, sp.y = 16.0f + kY[i] * off, sp.z = kZ[i] * off;
    }
    // The eye follows the attack target or the camera (the renderer passes the look target in the state).
    if (c.s.lookValid) {
        PartPose& eye = c["eye"];
        eye.y = c.s.lookAt[1] - c.s.eyePos[1] > 0 ? 0.0f : 1.0f;
        float vx = c.s.lookDir[0], vz = c.s.lookDir[2];
        float dx = c.s.eyePos[0] - c.s.lookAt[0], dz = c.s.eyePos[2] - c.s.lookAt[2];
        const float l = std::sqrt(dx * dx + dz * dz);
        if (l > 1e-6f) dx /= l, dz /= l;
        const float rx = dz, rz = -dx;  // yRot(pi/2)
        const float dot = vx * rx + vz * rz;
        eye.x = std::sqrt(std::abs(dot)) * 2.0f * (dot > 0 ? 1.0f : dot < 0 ? -1.0f : 0.0f);
    }
    const float swim = c.lerpA(kTail);
    c["tail0"].yRot = std::sin(swim) * kPi * 0.05f;
    c["tail1"].yRot = std::sin(swim) * kPi * 0.1f;
    c["tail2"].yRot = std::sin(swim) * kPi * 0.15f;
}

void frog(const Ctx& c) {  // FrogModel (keyframes baked on the whole model)
    c.apply("FrogAnimation.FROG_JUMP", kFrogJump);
    c.apply("FrogAnimation.FROG_CROAK", kFrogCroak);
    c.apply("FrogAnimation.FROG_TONGUE", kFrogTongue);
    if (c.e.inWater) c.walk("FrogAnimation.FROG_SWIM", c.s.walkPos, c.s.walkSpeed, 1.0f, 2.5f);
    else c.walk("FrogAnimation.FROG_WALK", c.s.walkPos, c.s.walkSpeed, 1.5f, 2.5f);
    c.apply("FrogAnimation.FROG_IDLE_WATER", kFrogSwimIdle);
    c.visible("croaking_body", c.e.sp.anim[kFrogCroak] != INT_MIN);
}

void axolotl(const Ctx& c) {
    if (c.m.baby) {  // BabyAxolotlModel
        if (c.e.sp.anim[kAxoWalk] != INT_MIN)
            c.walk("BabyAxolotlAnimation.AXOLOTL_WALK_FLOOR", c.s.walkPos, c.s.walkSpeed, 15.0f, 30.0f);
        c.apply("BabyAxolotlAnimation.BABY_AXOLOTL_SWIM", kAxoSwim);
        c.apply("BabyAxolotlAnimation.WALK_FLOOR_UNDERWATER", kAxoWalk);
        c.apply("BabyAxolotlAnimation.BABY_AXOLOTL_IDLE_FLOOR", kAxoIdleGround);
        c.apply("BabyAxolotlAnimation.IDLE_UNDERWATER", kAxoIdleWater);
        c.apply("BabyAxolotlAnimation.IDLE_FLOOR_UNDERWATER", kAxoIdleWaterGround);
        c.apply("BabyAxolotlAnimation.BABY_AXOLOTL_PLAY_DEAD", kAxoPlayDeadAnim);
        return;
    }
    // AdultAxolotlModel: BinaryAnimator factors (10 ticks, in-out sine)
    auto factor = [&](int i) {
        const float f = c.lerpA(i) / 10.0f;
        return -(std::cos(kPi * f) - 1.0f) / 2.0f;
    };
    const float dead = factor(kAxoPlayDead), water = factor(kAxoInWater), ground = factor(kAxoOnGround), moving = factor(kAxoMoving);
    const float still = 1.0f - moving, mirrored = 1.0f - std::min(ground, moving);
    PartPose &body = c["body"], &head = c["head"], &tail = c["tail"], &top = c["top_gills"], &lg = c["left_gills"],
             &rg = c["right_gills"], &lh = c["left_hind_leg"], &rh = c["right_hind_leg"], &lf = c["left_front_leg"],
             &rf = c["right_front_leg"];
    const float age = c.s.age;
    body.yRot += c.s.headYaw * kDeg;
    if (float f = std::min(moving, water); f > 1e-5f) {  // setupSwimmingAnimation
        const float a = age * 0.33f, sn = std::sin(a), cs = std::cos(a), sway = 0.13f * sn;
        body.xRot += (c.s.pitch * kDeg + sway) * f;
        head.xRot -= sway * 1.8f * f;
        body.y -= 0.45f * cs * f;
        top.xRot += (-0.5f * sn - 0.8f) * f;
        const float gy = (0.3f * sn + 0.9f) * f;
        lg.yRot += gy, rg.yRot -= gy;
        tail.yRot += 0.3f * std::cos(a * 0.9f) * f;
        lh.xRot += 1.8849558f * f, lh.yRot += -0.4f * sn * f, lh.zRot += kPi / 2 * f;
        lf.xRot += 1.8849558f * f, lf.yRot += (-0.2f * cs - 0.1f) * f, lf.zRot += kPi / 2 * f;
    }
    if (float f = std::min(still, water); f > 1e-5f) {  // setupWaterHoveringAnimation
        const float a = age * 0.075f, cs = std::cos(a), sn = std::sin(a) * 0.15f;
        const float bx = (-0.15f + 0.075f * cs) * f;
        body.xRot += bx, body.y -= sn * f, head.xRot -= bx;
        top.xRot += 0.2f * cs * f;
        const float gy = (-0.3f * cs - 0.19f) * f;
        lg.yRot += gy, rg.yRot -= gy;
        lh.xRot += (kPi * 3.0f / 4.0f - cs * 0.11f) * f, lh.yRot += 0.47123894f * f, lh.zRot += 1.7278761f * f;
        lf.xRot += (kPi / 4 - cs * 0.2f) * f, lf.yRot += 2.042035f * f;
        tail.yRot += 0.5f * cs * f;
    }
    if (float f = std::min(moving, ground); f > 1e-5f) {  // setupGroundCrawlingAnimation
        const float a = age * 0.11f, cs = std::cos(a);
        const float hindY = (cs * cs - 2.0f * cs) / 5.0f, frontY = 0.7f * cs, ht = 0.09f * cs * f;
        head.yRot += ht, tail.yRot += ht;
        const float gill = (0.6f - 0.08f * (cs * cs + 2.0f * std::sin(a))) * f;
        top.xRot += gill, lg.yRot -= gill, rg.yRot += gill;
        const float hx = 0.9424779f * f, fx = 1.0995574f * f;
        lh.xRot += hx, lh.yRot += (1.5f - hindY) * f, lh.zRot += -0.1f * f;
        lf.xRot += fx, lf.yRot += (kPi / 2 - frontY) * f;
        rh.xRot += hx, rh.yRot += (-1.0f - hindY) * f;
        rf.xRot += fx, rf.yRot += (-kPi / 2 - frontY) * f;
    }
    if (float f = std::min(still, ground); f > 1e-5f) {  // setupLayStillOnGroundAnimation
        const float a = age * 0.09f, sn = std::sin(a), cs = std::cos(a);
        const float mv = sn * sn - 2.0f * sn, mv2 = cs * cs - 3.0f * sn;
        head.xRot += -0.09f * mv * f, head.zRot += -0.2f * f;
        tail.yRot += (-0.1f + 0.1f * mv) * f;
        const float gill = (0.6f + 0.05f * mv2) * f;
        top.xRot += gill, lg.yRot -= gill, rg.yRot += gill;
        lh.xRot += 1.1f * f, lh.yRot += 1.0f * f;
        lf.xRot += 0.8f * f, lf.yRot += 2.3f * f, lf.zRot -= 0.5f * f;
    }
    if (dead > 1e-5f) {  // setupPlayDeadAnimation
        lh.xRot += 1.4137167f * dead, lh.yRot += 1.0995574f * dead, lh.zRot += kPi / 4 * dead;
        lf.xRot += kPi / 4 * dead, lf.yRot += 2.042035f * dead;
        body.xRot += -0.15f * dead, body.zRot += 0.35f * dead;
    }
    if (mirrored > 1e-5f) {  // applyMirrorLegRotations
        rh.xRot += lh.xRot * mirrored, rh.yRot += -lh.yRot * mirrored, rh.zRot += -lh.zRot * mirrored;
        rf.xRot += lf.xRot * mirrored, rf.yRot += -lf.yRot * mirrored, rf.zRot += -lf.zRot * mirrored;
    }
}

bool armadilloHidden(const Entity& e) {  // Armadillo.shouldHideInShell
    const int state = int(dataInt(e, "ARMADILLO_STATE"));
    const int t = e.sp.t[0];
    return state == 1 ? t > 5 : state == 2 ? true : state == 3 ? t < 26 : false;
}

void armadillo(const Ctx& c) {  // ArmadilloModel
    const bool baby = c.m.baby;
    const bool hidden = armadilloHidden(c.e);
    c.skipDraw("body", hidden);
    c.visible("left_hind_leg", !hidden), c.visible("right_hind_leg", !hidden), c.visible("tail", !hidden);
    c.visible("cube", hidden);
    if (!hidden) {
        PartPose& h = c["head"];
        h.xRot = std::clamp(c.s.pitch, -22.5f, 25.0f) * kDeg;
        h.yRot = std::clamp(c.s.headYaw, -32.5f, 32.5f) * kDeg;
        c.walk(baby ? "BabyArmadilloAnimation.ARMADILLO_BABY_WALK" : "ArmadilloAnimation.ARMADILLO_WALK", c.s.walkPos,
               c.s.walkSpeed, 16.5f, 2.5f);
    }
    c.apply(baby ? "BabyArmadilloAnimation.ARMADILLO_BABY_ROLL_OUT" : "ArmadilloAnimation.ARMADILLO_ROLL_OUT", kArmadilloRollOut);
    c.apply(baby ? "BabyArmadilloAnimation.ARMADILLO_BABY_ROLL_UP" : "ArmadilloAnimation.ARMADILLO_ROLL_UP", kArmadilloRollUp);
    c.apply(baby ? "BabyArmadilloAnimation.ARMADILLO_BABY_PEEK" : "ArmadilloAnimation.ARMADILLO_PEEK", kArmadilloPeek);
}

void sniffer(const Ctx& c) {  // SnifferModel / SniffletModel
    c.look(c["head"]);
    if (dataInt(c.e, "DATA_STATE") == 4)  // SEARCHING
        c.walk("SnifferAnimation.SNIFFER_SNIFF_SEARCH", c.s.walkPos, c.s.walkSpeed, 9.0f, 100.0f);
    else
        c.walk("SnifferAnimation.SNIFFER_WALK", c.s.walkPos, c.s.walkSpeed, 9.0f, 100.0f);
    c.apply("SnifferAnimation.SNIFFER_DIG", kSnifferDig);
    c.apply("SnifferAnimation.SNIFFER_LONGSNIFF", kSnifferSniff);
    c.apply("SnifferAnimation.SNIFFER_STAND_UP", kSnifferRise);
    c.apply("SnifferAnimation.SNIFFER_HAPPY", kSnifferHappy);
    c.apply("SnifferAnimation.SNIFFER_SNIFFSNIFF", kSnifferScent);
    if (c.m.baby) c.applyStatic("SnifferAnimation.BABY_TRANSFORM");
}

void copperGolem(const Ctx& c) {  // CopperGolemModel
    c.look(c["head"]);
    if (!c.s.rightItem && !c.s.leftItem) {
        c.walk("CopperGolemAnimation.COPPER_GOLEM_WALK", c.s.walkPos, c.s.walkSpeed, 2.0f, 2.5f);
    } else {
        c.walk("CopperGolemAnimation.COPPER_GOLEM_WALK_ITEM", c.s.walkPos, c.s.walkSpeed, 2.0f, 2.5f);
        PartPose &ra = c["right_arm"], &la = c["left_arm"];  // poseHeldItemArmsIfStill
        ra.xRot = std::min(ra.xRot, -0.87266463f), la.xRot = std::min(la.xRot, -0.87266463f);
        ra.yRot = std::min(ra.yRot, -0.1134464f), la.yRot = std::max(la.yRot, 0.1134464f);
        ra.zRot = std::min(ra.zRot, -0.064577185f), la.zRot = std::max(la.zRot, 0.064577185f);
    }
    c.apply("CopperGolemAnimation.COPPER_GOLEM_IDLE", kGolemIdle);
    c.apply("CopperGolemAnimation.COPPER_GOLEM_CHEST_INTERACTION_NOITEM_GET", kGolemGet);
    c.apply("CopperGolemAnimation.COPPER_GOLEM_CHEST_INTERACTION_NOITEM_NOGET", kGolemGetNo);
    c.apply("CopperGolemAnimation.COPPER_GOLEM_CHEST_INTERACTION_ITEM_DROP", kGolemDrop);
    c.apply("CopperGolemAnimation.COPPER_GOLEM_CHEST_INTERACTION_ITEM_NODROP", kGolemDropNo);
}

void snowGolem(const Ctx& c) {  // SnowGolemModel
    PartPose &head = c["head"], &upper = c["upper_body"], &la = c["left_arm"], &ra = c["right_arm"];
    c.look(head);
    upper.yRot = c.s.headYaw * kDeg * 0.25f;
    const float sn = std::sin(upper.yRot), cs = std::cos(upper.yRot);
    la.yRot = upper.yRot, ra.yRot = upper.yRot + kPi;
    la.x = cs * 5.0f, la.z = -sn * 5.0f;
    ra.x = -cs * 5.0f, ra.z = sn * 5.0f;
}

void ironGolem(const Ctx& c) {  // IronGolemModel
    const float attack = c.e.sp.t[0] > 0 ? float(c.e.sp.t[0]) - c.s.pt : 0.0f;
    const float speed = c.s.walkSpeed, pos = c.s.walkPos;
    PartPose &ra = c["right_arm"], &la = c["left_arm"];
    if (attack > 0) {
        ra.xRot = -2.0f + 1.5f * triangleWave(attack, 10.0f);
        la.xRot = -2.0f + 1.5f * triangleWave(attack, 10.0f);
    } else if (const int flower = c.e.sp.t[1]; flower > 0) {
        ra.xRot = -0.8f + 0.025f * triangleWave(float(flower), 70.0f);
        la.xRot = 0;
    } else {
        ra.xRot = (-0.2f + 1.5f * triangleWave(pos, 13.0f)) * speed;
        la.xRot = (-0.2f - 1.5f * triangleWave(pos, 13.0f)) * speed;
    }
    c.look(c["head"]);
    PartPose &rl = c["right_leg"], &ll = c["left_leg"];
    rl.xRot = -1.5f * triangleWave(pos, 13.0f) * speed;
    ll.xRot = 1.5f * triangleWave(pos, 13.0f) * speed;
    rl.yRot = ll.yRot = 0;
}

void villager(const Ctx& c) {  // VillagerModel (villagers, wandering traders)
    PartPose& head = c["head"];
    c.look(head);
    if (dataInt(c.e, "DATA_UNHAPPY_COUNTER") > 0) head.zRot = 0.3f * std::sin(0.45f * c.s.age), head.xRot = 0.4f;
    else head.zRot = 0;
    PartPose &rl = c["right_leg"], &ll = c["left_leg"];
    rl.xRot = std::cos(c.s.walkPos * 0.6662f) * 1.4f * c.s.walkSpeed * 0.5f;
    ll.xRot = std::cos(c.s.walkPos * 0.6662f + kPi) * 1.4f * c.s.walkSpeed * 0.5f;
    rl.yRot = ll.yRot = 0;
}

void witch(const Ctx& c) {  // WitchModel
    PartPose &head = c["head"], &nose = c["nose"];
    c.look(head);
    c["right_leg"].xRot = std::cos(c.s.walkPos * 0.6662f) * 1.4f * c.s.walkSpeed * 0.5f;
    c["left_leg"].xRot = std::cos(c.s.walkPos * 0.6662f + kPi) * 1.4f * c.s.walkSpeed * 0.5f;
    const float speed = 0.01f * float(((c.e.id % 10) + 10) % 10);
    nose.xRot = std::sin(c.s.age * speed) * 4.5f * kDeg;
    nose.zRot = std::cos(c.s.age * speed) * 2.5f * kDeg;
    if (c.s.rightItem || c.s.leftItem) nose.x = 0, nose.y = 1.0f, nose.z = -1.5f, nose.xRot = -0.9f;
}

void illager(const Ctx& c) {  // IllagerModel
    PartPose &head = c["head"], &ra = c["right_arm"], &la = c["left_arm"], &rl = c["right_leg"], &ll = c["left_leg"];
    c.look(head);
    if (c.s.riding) {
        ra.xRot = -kPi / 5, ra.yRot = 0, ra.zRot = 0;
        la.xRot = -kPi / 5, la.yRot = 0, la.zRot = 0;
        rl.xRot = -1.4137167f, rl.yRot = kPi / 10, rl.zRot = 0.07853982f;
        ll.xRot = -1.4137167f, ll.yRot = -kPi / 10, ll.zRot = -0.07853982f;
    } else {
        const float speed = c.s.walkSpeed, pos = c.s.walkPos;
        ra.xRot = std::cos(pos * 0.6662f + kPi) * 2.0f * speed * 0.5f, ra.yRot = 0, ra.zRot = 0;
        la.xRot = std::cos(pos * 0.6662f) * 2.0f * speed * 0.5f, la.yRot = 0, la.zRot = 0;
        rl.xRot = std::cos(pos * 0.6662f) * 1.4f * speed * 0.5f, rl.yRot = 0, rl.zRot = 0;
        ll.xRot = std::cos(pos * 0.6662f + kPi) * 1.4f * speed * 0.5f, ll.yRot = 0, ll.zRot = 0;
    }
    const int pose = c.s.mobPose;
    const float age = c.s.age;
    if (pose == 1) {  // ATTACKING
        const bool mainEmpty = !(c.s.swingLeft ? c.s.leftItem : c.s.rightItem);
        if (mainEmpty) {  // animateZombieArms(aggressive) + bobArms
            const float ay = std::sin(c.s.swing * kPi), ax = std::sin((1.0f - (1.0f - c.s.swing) * (1.0f - c.s.swing)) * kPi);
            const float x = -kPi / 1.5f + ay * 1.2f - ax * 0.4f, y = 0.1f - ay * 0.6f;
            ra.xRot = x, ra.yRot = -y, ra.zRot = 0;
            la.xRot = x, la.yRot = y, la.zRot = 0;
            ra.zRot += std::cos(age * 0.09f) * 0.05f + 0.05f, ra.xRot += std::sin(age * 0.067f) * 0.05f;
            la.zRot -= std::cos(age * 0.09f) * 0.05f + 0.05f, la.xRot -= std::sin(age * 0.067f) * 0.05f;
        } else {  // swingWeaponDown
            const float attack2 = std::sin(c.s.swing * kPi), attack = std::sin((1.0f - (1.0f - c.s.swing) * (1.0f - c.s.swing)) * kPi);
            ra.zRot = la.zRot = 0, ra.yRot = kPi / 20, la.yRot = -kPi / 20;
            PartPose& main = c.s.swingLeft ? la : ra;
            PartPose& off = c.s.swingLeft ? ra : la;
            main.xRot = -1.8849558f + std::cos(age * 0.09f) * 0.15f + attack2 * 2.2f - attack * 0.4f;
            off.xRot = std::cos(age * 0.19f) * 0.5f + attack2 * 1.2f - attack * 0.4f;
            ra.zRot += std::cos(age * 0.09f) * 0.05f + 0.05f, ra.xRot += std::sin(age * 0.067f) * 0.05f;
            la.zRot -= std::cos(age * 0.09f) * 0.05f + 0.05f, la.xRot -= std::sin(age * 0.067f) * 0.05f;
        }
    } else if (pose == 2) {  // SPELLCASTING
        ra.z = 0, ra.x = -5.0f, la.z = 0, la.x = 5.0f;
        ra.xRot = la.xRot = std::cos(age * 0.6662f) * 0.25f;
        ra.zRot = kPi * 3.0f / 4.0f, la.zRot = -kPi * 3.0f / 4.0f;
        ra.yRot = la.yRot = 0;
    } else if (pose == 3) {  // BOW_AND_ARROW
        ra.yRot = -0.1f + head.yRot, ra.xRot = -kPi / 2 + head.xRot;
        la.xRot = -0.9424779f + head.xRot, la.yRot = head.yRot - 0.4f, la.zRot = kPi / 2;
    } else if (pose == 4) {  // CROSSBOW_HOLD
        ra.yRot = -0.3f + head.yRot, la.yRot = 0.6f + head.yRot;
        ra.xRot = -kPi / 2 + head.xRot + 0.1f, la.xRot = -1.5f + head.xRot;
    } else if (pose == 5) {  // CROSSBOW_CHARGE
        ra.yRot = -0.8f, ra.xRot = -0.97079635f, la.xRot = ra.xRot;
        const float a = std::clamp(c.s.useTicks, 0.0f, c.s.crossbowCharge) / std::max(c.s.crossbowCharge, 1.0f);
        la.yRot = lerp(a, 0.4f, 0.85f);
        la.xRot = lerp(a, la.xRot, -kPi / 2);
    } else if (pose == 6) {  // CELEBRATING
        ra.z = 0, ra.x = -5.0f, ra.xRot = std::cos(age * 0.6662f) * 0.05f, ra.zRot = 2.670354f, ra.yRot = 0;
        la.z = 0, la.x = 5.0f, la.xRot = std::cos(age * 0.6662f) * 0.05f, la.zRot = -kPi * 3.0f / 4.0f, la.yRot = 0;
    }
    const bool crossed = pose == 0;
    c.visible("arms", crossed);
    c.visible("left_arm", !crossed), c.visible("right_arm", !crossed);
    c.visible("hat", false);
}

// AbstractPiglinModel / PiglinModel additions after HumanoidModel.setupAnim.
void piglin(const Ctx& c, bool zombified) {
    const float pos = c.s.walkPos, speed = c.s.walkSpeed;
    const float def = (c.m.baby ? 5.0f : 30.0f) * kDeg;
    const float freq = c.s.age * 0.1f + pos * 0.5f, amp = 0.08f + speed * 0.4f;
    PartPose &le = c["left_ear"], &re = c["right_ear"];
    le.zRot = -def - std::cos(freq * 1.2f) * amp;
    re.zRot = def + std::cos(freq) * amp;
    if (zombified) return;
    PartPose &head = c["head"], &ra = c["right_arm"], &la = c["left_arm"], &body = c["body"];
    const bool rightHanded = !c.s.swingLeft;
    switch (c.s.mobPose) {
        case 5: {  // DANCING
            const float d = c.s.age / 60.0f;
            re.zRot = kPi / 6 + kDeg * std::sin(d * 30.0f) * 10.0f;
            le.zRot = -kPi / 6 - kDeg * std::cos(d * 30.0f) * 10.0f;
            head.x += std::sin(d * 10.0f);
            head.y += std::sin(d * 40.0f) + 0.4f;
            ra.zRot = kDeg * (70.0f + std::cos(d * 40.0f) * 10.0f);
            la.zRot = ra.zRot * -1.0f;
            ra.y += std::sin(d * 40.0f) * 0.5f - 0.5f;
            la.y += std::sin(d * 40.0f) * 0.5f + 0.5f;
            body.y += std::sin(d * 40.0f) * 0.35f;
            break;
        }
        case 1:  // ATTACKING_WITH_MELEE_WEAPON, not swinging: holdWeaponHigh
            if (c.s.swing == 0) (rightHanded ? ra : la).xRot = -1.8f;
            break;
        case 2: {  // CROSSBOW_HOLD
            PartPose& holding = rightHanded ? ra : la;
            PartPose& shooting = rightHanded ? la : ra;
            holding.yRot = (rightHanded ? -0.3f : 0.3f) + head.yRot;
            shooting.yRot = (rightHanded ? 0.6f : -0.6f) + head.yRot;
            holding.xRot = -kPi / 2 + head.xRot + 0.1f;
            shooting.xRot = -1.5f + head.xRot;
            break;
        }
        case 3: {  // CROSSBOW_CHARGE
            PartPose& holding = rightHanded ? ra : la;
            PartPose& pulling = rightHanded ? la : ra;
            holding.yRot = rightHanded ? -0.8f : 0.8f;
            holding.xRot = -0.97079635f;
            pulling.xRot = holding.xRot;
            const float a = std::clamp(c.s.useTicks, 0.0f, c.s.crossbowCharge) / std::max(c.s.crossbowCharge, 1.0f);
            pulling.yRot = lerp(a, 0.4f, 0.85f) * (rightHanded ? 1.0f : -1.0f);
            pulling.xRot = lerp(a, pulling.xRot, -kPi / 2);
            break;
        }
        case 4:  // ADMIRING_ITEM
            head.xRot = 0.5f, head.yRot = 0;
            if (!rightHanded) ra.yRot = -0.5f, ra.xRot = -0.9f;
            else la.yRot = 0.5f, la.xRot = -0.9f;
            break;
        default: break;
    }
}

void enderman(const Ctx& c) {  // EndermanModel additions
    PartPose &ra = c["right_arm"], &la = c["left_arm"], &rl = c["right_leg"], &ll = c["left_leg"];
    for (PartPose* p : {&ra, &la, &rl, &ll}) p->xRot = std::clamp(p->xRot * 0.5f, -0.4f, 0.4f);
    if (dataInt(c.e, "DATA_CARRY_STATE") > 0) ra.xRot = la.xRot = -0.5f, ra.zRot = 0.05f, la.zRot = -0.05f;
    if (dataInt(c.e, "DATA_CREEPY")) c["head"].y -= 5.0f, c["hat"].y += 5.0f;
}

void ghast(const Ctx& c, bool happy) {  // GhastModel.animateTentacles / HappyGhastModel
    if (happy && c.e.equipment[6] >= 0) {
        PartPose& body = c["body"];
        body.xScale = body.yScale = body.zScale = 0.9375f;
    }
    for (int i = 0; i < 9; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "tentacle%d", i);
        c[name].xRot = 0.2f * std::sin(c.s.age * 0.3f + float(i)) + 0.4f;
    }
}

void nautilus(const Ctx& c) {  // NautilusModel / ZombieNautilusCoralModel
    PartPose& body = c["body"];
    body.yRot = std::clamp(c.s.headYaw, -10.0f, 10.0f) * kDeg;
    body.xRot = std::clamp(c.s.pitch, -10.0f, 10.0f) * kDeg;
    c.walk("NautilusAnimation.SWIMMING", c.s.walkPos + c.s.age / 5.0f, c.s.walkSpeed + 0.2f, 2.0f, 3.0f);
    c.visible("corals", c.e.equipment[6] < 0);
}

// ---- monsters ----

void blaze(const Ctx& c) {  // BlazeModel
    const float age = c.s.age;
    float angle = age * kPi * -0.1f;
    char name[16];
    for (int i = 0; i < 12; ++i) {
        if (i == 4) angle = kPi / 4 + age * kPi * 0.03f;
        if (i == 8) angle = 0.47123894f + age * kPi * -0.05f;
        std::snprintf(name, sizeof(name), "part%d", i);
        PartPose& p = c[name];
        if (i < 4) p.y = -2.0f + std::cos((float(i * 2) + age) * 0.25f), p.x = std::cos(angle) * 9.0f, p.z = std::sin(angle) * 9.0f;
        else if (i < 8) p.y = 2.0f + std::cos((float(i * 2) + age) * 0.25f), p.x = std::cos(angle) * 7.0f, p.z = std::sin(angle) * 7.0f;
        else p.y = 11.0f + std::cos((float(i) * 1.5f + age) * 0.5f), p.x = std::cos(angle) * 5.0f, p.z = std::sin(angle) * 5.0f;
        angle += kPi / 2;
    }
    PartPose& h = c["head"];
    h.yRot = c.s.headYaw * kDeg, h.xRot = c.s.pitch * kDeg;
}

void breeze(const Ctx& c) {  // BreezeModel
    c.apply("BreezeAnimation.IDLE", kBreezeIdle);
    c.apply("BreezeAnimation.SHOOT", kBreezeShoot);
    c.apply("BreezeAnimation.SLIDE", kBreezeSlide);
    c.apply("BreezeAnimation.SLIDE_BACK", kBreezeSlideBack);
    c.apply("BreezeAnimation.INHALE", kBreezeInhale);
    c.apply("BreezeAnimation.JUMP", kBreezeJump);
}

void creaking(const Ctx& c) {  // CreakingModel (keyframes baked on the "root" child)
    const int root = c.innerRoot();
    c.look(c["head"]);
    if (dataInt(c.e, "CAN_MOVE", 1)) c.walk("CreakingAnimation.CREAKING_WALK", c.s.walkPos, c.s.walkSpeed, 1.0f, 1.0f, root);
    c.apply("CreakingAnimation.CREAKING_ATTACK", kCreakingAttack, 1.0f, root);
    c.apply("CreakingAnimation.CREAKING_INVULNERABLE", kCreakingInvulnerable, 1.0f, root);
    c.apply("CreakingAnimation.CREAKING_DEATH", kCreakingDeath, 1.0f, root);
}

// DragonFlightHistory.get(delay, partialTicks) as (y, yRot).
void dragonSample(const Entity& e, int delay, float pt, float out[2]) {
    const SpeciesState& s = e.sp;
    const int head = std::max(s.historyPos, 0);
    const float* a = s.history[(head - delay) & 63];
    const float* b = s.history[(head - delay - 1) & 63];
    out[0] = lerp(pt, b[0], a[0]);
    out[1] = rotLerpDeg(pt, b[1], a[1]);
}

void dragon(const Ctx& c) {  // EnderDragonModel
    const Entity& e = c.e;
    const float pt = e.health <= 0 ? 0.0f : c.s.pt;
    const float flapTime = c.lerpA(kFlap) * kPi * 2;
    const int phase = int(dataInt(e, "DATA_PHASE"));
    const bool landing = phase == 3 || phase == 4, sitting = phase >= 5 && phase <= 7;
    // distance to the egg podium (0, top, 0): horizontal distance with the dragon's height above the fountain
    const double distToEgg = e.pos[0] * e.pos[0] + e.pos[2] * e.pos[2] + (e.pos[1] - 64.0) * (e.pos[1] - 64.0);
    auto partYOffset = [&](int part, const float* body, const float* p) -> float {
        if (landing) return float(part / std::max(distToEgg / 4.0, 1.0));
        if (sitting) return float(part);
        if (part == 6) return 0.0f;
        return p[0] - body[0];
    };
    PartPose& root = c.p[0];
    c["jaw"].xRot = (std::sin(flapTime) + 1.0f) * 0.2f;
    float bounce = std::sin(flapTime - 1.0f) + 1.0f;
    bounce = (bounce * bounce + bounce * 2.0f) * 0.05f;
    root.y = (bounce - 2.0f) * 16.0f;
    root.z = -48.0f;
    root.xRot = bounce * 2.0f * kDeg;
    char name[16];
    std::snprintf(name, sizeof(name), "neck%d", 0);
    PartPose& neck0 = c[name];
    float xx = neck0.x, yy = neck0.y, zz = neck0.z;
    float start[2], h5[2], h10[2];
    dragonSample(e, 6, pt, start);
    dragonSample(e, 5, pt, h5);
    dragonSample(e, 10, pt, h10);
    const float rot2 = wrapDegrees(h5[1] - h10[1]);
    const float rot = wrapDegrees(h5[1] + rot2 / 2.0f);
    for (int i = 0; i < 5; ++i) {
        std::snprintf(name, sizeof(name), "neck%d", i);
        PartPose& neck = c[name];
        float point[2];
        dragonSample(e, 5 - i, pt, point);
        const float neckX = std::cos(float(i) * 0.45f + flapTime) * 0.15f;
        neck.yRot = wrapDegrees(point[1] - start[1]) * kDeg * 1.5f;
        neck.xRot = neckX + partYOffset(i, start, point) * kDeg * 1.5f * 5.0f;
        neck.zRot = -wrapDegrees(point[1] - rot) * kDeg * 1.5f;
        neck.y = yy, neck.z = zz, neck.x = xx;
        xx -= std::sin(neck.yRot) * std::cos(neck.xRot) * 10.0f;
        yy += std::sin(neck.xRot) * 10.0f;
        zz -= std::cos(neck.yRot) * std::cos(neck.xRot) * 10.0f;
    }
    PartPose& head = c["head"];
    head.y = yy, head.z = zz, head.x = xx;
    float current[2];
    dragonSample(e, 0, pt, current);
    head.yRot = wrapDegrees(current[1] - start[1]) * kDeg;
    head.xRot = wrapDegrees(partYOffset(6, start, current)) * kDeg * 1.5f * 5.0f;
    head.zRot = -wrapDegrees(current[1] - rot) * kDeg;
    c["body"].zRot = -rot2 * 1.5f * kDeg;
    PartPose &lw = c["left_wing"], &lwt = c["left_wing_tip"], &rw = c["right_wing"], &rwt = c["right_wing_tip"];
    lw.xRot = 0.125f - std::cos(flapTime) * 0.2f;
    lw.yRot = -0.25f;
    lw.zRot = -(std::sin(flapTime) + 0.125f) * 0.8f;
    lwt.zRot = (std::sin(flapTime + 2.0f) + 0.5f) * 0.75f;
    rw.xRot = lw.xRot, rw.yRot = -lw.yRot, rw.zRot = -lw.zRot;
    rwt.zRot = -lwt.zRot;
    for (const char* side : {"left", "right"}) {  // poseLimbs
        char n[40];
        auto limb = [&](const char* fmt, float v) {
            std::snprintf(n, sizeof(n), fmt, side);
            c[n].xRot = v;
        };
        limb("%s_hind_leg", 1.0f + bounce * 0.1f);
        limb("%s_hind_leg_tip", 0.5f + bounce * 0.1f);
        limb("%s_hind_foot", 0.75f + bounce * 0.1f);
        limb("%s_front_leg", 1.3f + bounce * 0.1f);
        limb("%s_front_leg_tip", -0.5f - bounce * 0.1f);
        limb("%s_front_foot", 0.75f + bounce * 0.1f);
    }
    float tailX = 0;
    std::snprintf(name, sizeof(name), "tail%d", 0);
    PartPose& tail0 = c[name];
    yy = tail0.y, zz = tail0.z, xx = tail0.x;
    dragonSample(e, 11, pt, start);
    for (int i = 0; i < 12; ++i) {
        float point[2];
        dragonSample(e, 12 + i, pt, point);
        tailX += std::sin(float(i) * 0.45f + flapTime) * 0.05f;
        std::snprintf(name, sizeof(name), "tail%d", i);
        PartPose& tail = c[name];
        tail.yRot = (wrapDegrees(point[1] - start[1]) * 1.5f + 180.0f) * kDeg;
        tail.xRot = tailX + (point[0] - start[0]) * kDeg * 1.5f * 5.0f;
        tail.zRot = wrapDegrees(point[1] - rot) * kDeg * 1.5f;
        tail.y = yy, tail.z = zz, tail.x = xx;
        yy += std::sin(tail.xRot) * 10.0f;
        zz -= std::cos(tail.yRot) * std::cos(tail.xRot) * 10.0f;
        xx -= std::sin(tail.yRot) * std::cos(tail.xRot) * 10.0f;
    }
}

void segments(const Ctx& c, const char* fmt, int count, float yAmp, float xAmp) {  // EndermiteModel / SilverfishModel
    char name[16];
    for (int i = 0; i < count; ++i) {
        std::snprintf(name, sizeof(name), fmt, i);
        PartPose& p = c[name];
        const float a = c.s.age * 0.9f + float(i) * 0.15f * kPi;
        p.yRot = std::cos(a) * kPi * yAmp * float(1 + std::abs(i - 2));
        p.x = std::sin(a) * kPi * xAmp * float(std::abs(i - 2));
    }
}

void silverfish(const Ctx& c) {
    segments(c, "segment%d", 7, 0.05f, 0.2f);
    PartPose &s1 = c["segment1"], &s2 = c["segment2"], &s4 = c["segment4"];
    c["layer0"].yRot = s2.yRot;
    c["layer1"].yRot = s4.yRot, c["layer1"].x = s4.x;
    c["layer2"].yRot = s1.yRot, c["layer2"].x = s1.x;
}

void magmaCube(const Ctx& c) {  // MagmaCubeModel
    const float squish = std::max(0.0f, c.lerpA(kSquish));
    char name[16];
    for (int i = 0; i < 8; ++i) {
        std::snprintf(name, sizeof(name), "cube%d", i);
        c[name].y = -float(4 - i) * squish * 1.7f;
    }
}

void phantom(const Ctx& c) {  // PhantomModel (flap time: getUniqueFlapTickOffset = id * 3)
    const float anim = (float(c.e.id * 3) + c.s.age) * 7.448451f * kDeg;
    PartPose &lwb = c["left_wing_base"], &lwt = c["left_wing_tip"], &rwb = c["right_wing_base"], &rwt = c["right_wing_tip"];
    lwb.zRot = std::cos(anim) * 16.0f * kDeg;
    lwt.zRot = std::cos(anim) * 16.0f * kDeg;
    rwb.zRot = -lwb.zRot, rwt.zRot = -lwt.zRot;
    c["tail_base"].xRot = -(5.0f + std::cos(anim * 2.0f) * 5.0f) * kDeg;
    c["tail_tip"].xRot = -(5.0f + std::cos(anim * 2.0f) * 5.0f) * kDeg;
}

void strider(const Ctx& c) {  // StriderModel / AdultStriderModel / BabyStriderModel
    const float pos = c.s.walkPos, speed = std::min(c.s.walkSpeed, 0.25f), age = c.s.age;
    PartPose &body = c["body"], &ll = c["left_leg"], &rl = c["right_leg"];
    if (!c.s.riding) body.xRot = c.s.pitch * kDeg, body.yRot = c.s.headYaw * kDeg;
    else body.xRot = 0, body.yRot = 0;
    body.zRot = 0.1f * std::sin(pos * 1.5f) * 4.0f * speed;
    ll.xRot = std::sin(pos * 1.5f * 0.5f) * 2.0f * speed;
    rl.xRot = std::sin(pos * 1.5f * 0.5f + kPi) * 2.0f * speed;
    ll.zRot = kPi / 18 * std::cos(pos * 1.5f * 0.5f) * speed;
    rl.zRot = kPi / 18 * std::cos(pos * 1.5f * 0.5f + kPi) * speed;
    const float flow = std::cos(pos * 1.5f + kPi) * speed;
    auto bristle = [&](PartPose& a, PartPose& b, PartPose& d, bool z) {  // animateBristle
        float& ra = z ? a.zRot : a.xRot;
        float& rb = z ? b.zRot : b.xRot;
        float& rd = z ? d.zRot : d.xRot;
        ra += flow * 0.6f, rb += flow * 1.2f, rd += flow * 1.3f;
        ra += 0.1f * std::sin(age * 0.4f), rb += 0.1f * std::sin(age * 0.2f), rd += 0.05f * std::sin(age * -0.4f);
    };
    if (c.m.baby) {
        body.y = 17.25f - 1.0f * std::cos(pos * 1.5f) * 2.0f * speed;
        ll.y = 20.0f + 2.0f * std::sin(pos * 1.5f * 0.5f + kPi) * 2.0f * speed;
        rl.y = 20.0f + 2.0f * std::sin(pos * 1.5f * 0.5f) * 2.0f * speed;
        bristle(c["bristle2"], c["bristle1"], c["bristle0"], false);
    } else {
        PartPose &rb = c["right_bottom_bristle"], &rm = c["right_middle_bristle"], &rt = c["right_top_bristle"];
        PartPose &lt = c["left_top_bristle"], &lm = c["left_middle_bristle"], &lb = c["left_bottom_bristle"];
        rb.zRot = -1.2217305f, rm.zRot = -1.134464f, rt.zRot = -0.87266463f;
        lt.zRot = 0.87266463f, lm.zRot = 1.134464f, lb.zRot = 1.2217305f;
        bristle(rt, rm, rb, true);
        bristle(lt, lm, lb, true);
        body.y = 2.0f - 2.0f * std::cos(pos * 1.5f) * 2.0f * speed;
        ll.y = 8.0f + 2.0f * std::sin(pos * 1.5f * 0.5f + kPi) * 2.0f * speed;
        rl.y = 8.0f + 2.0f * std::sin(pos * 1.5f * 0.5f) * 2.0f * speed;
    }
}

void vex(const Ctx& c) {  // VexModel (its model root is the "root" child)
    PartPose &head = c["head"], &body = c["body"], &ra = c["right_arm"], &la = c["left_arm"], &rw = c["right_wing"],
             &lw = c["left_wing"];
    head.yRot = c.s.headYaw * kDeg, head.xRot = c.s.pitch * kDeg;
    const float bob = std::cos(c.s.age * 5.5f * kDeg) * 0.1f;
    ra.zRot = kPi / 5 + bob, la.zRot = -(kPi / 5 + bob);
    if (dataInt(c.e, "DATA_FLAGS_ID") & 1) {  // charging
        body.xRot = 0;
        if (!c.s.rightItem && !c.s.leftItem) {
            ra.xRot = -1.2217305f, ra.yRot = kPi / 12, ra.zRot = -0.47123888f - bob;
            la.xRot = -1.2217305f, la.yRot = -kPi / 12, la.zRot = 0.47123888f + bob;
        } else {
            if (c.s.rightItem) ra.xRot = kPi * 7.0f / 6.0f, ra.yRot = kPi / 12, ra.zRot = -0.47123888f - bob;
            if (c.s.leftItem) la.xRot = kPi * 7.0f / 6.0f, la.yRot = -kPi / 12, la.zRot = 0.47123888f + bob;
        }
    } else {
        body.xRot = kPi / 20;
    }
    lw.yRot = 1.0995574f + std::cos(c.s.age * 45.836624f * kDeg) * kDeg * 16.2f;
    rw.yRot = -lw.yRot;
    lw.xRot = 0.47123888f, lw.zRot = -0.47123888f;
    rw.xRot = 0.47123888f, rw.zRot = 0.47123888f;
}

void warden(const Ctx& c) {  // WardenModel
    PartPose &head = c["head"], &body = c["body"], &ll = c["left_leg"], &rl = c["right_leg"], &la = c["left_arm"],
             &ra = c["right_arm"];
    c.look(head);
    {  // animateWalk
        const float mod = std::min(0.5f, 3.0f * c.s.walkSpeed), pos = c.s.walkPos * 0.8662f;
        const float cs = std::cos(pos), sn = std::sin(pos), modMin = std::min(0.35f, mod);
        head.zRot += 0.3f * sn * mod;
        head.xRot += 1.2f * std::cos(pos + kPi / 2) * modMin;
        body.zRot = 0.1f * sn * mod;
        body.xRot = 1.0f * cs * modMin;
        ll.xRot = 1.0f * cs * mod;
        rl.xRot = 1.0f * std::cos(pos + kPi) * mod;
        la.xRot = -(0.8f * cs * mod), la.zRot = 0;
        ra.xRot = -(0.8f * sn * mod), ra.zRot = 0;
        la.yRot = 0, la.z = 1.0f, la.x = 13.0f, la.y = -13.0f;  // resetArmPoses
        ra.yRot = 0, ra.z = 1.0f, ra.x = -13.0f, ra.y = -13.0f;
    }
    {  // animateIdlePose
        const float a = c.s.age * 0.1f;
        head.zRot += 0.06f * std::cos(a), head.xRot += 0.06f * std::sin(a);
        body.zRot += 0.025f * std::sin(a), body.xRot += 0.025f * std::cos(a);
    }
    const float tendril = c.lerpA(kTendril) / 10.0f * float(std::cos(c.s.age * 2.25) * kPi * 0.1f);
    c["left_tendril"].xRot = tendril;
    c["right_tendril"].xRot = -tendril;
    c.apply("WardenAnimation.WARDEN_ATTACK", kWardenAttack);
    c.apply("WardenAnimation.WARDEN_SONIC_BOOM", kWardenSonic);
    c.apply("WardenAnimation.WARDEN_DIG", kWardenDig);
    c.apply("WardenAnimation.WARDEN_EMERGE", kWardenEmerge);
    c.apply("WardenAnimation.WARDEN_ROAR", kWardenRoar);
    c.apply("WardenAnimation.WARDEN_SNIFF", kWardenSniff);
}

void wither(const Ctx& c) {  // WitherBossModel
    const float bodyRot = lerp(c.s.pt, c.e.prevBodyYaw, c.e.bodyYaw);
    const char* heads[2] = {"right_head", "left_head"};
    for (int i = 0; i < 2; ++i) {
        const float* h = c.e.sp.wither[i];
        PartPose& p = c[heads[i]];
        p.yRot = (h[1] - bodyRot) * kDeg;
        p.xRot = h[0] * kDeg;
    }
    const float anim = std::cos(c.s.age * 0.1f);
    PartPose &rib = c["ribcage"], &tail = c["tail"];
    rib.xRot = (0.065f + 0.05f * anim) * kPi;
    tail.x = -2.0f, tail.y = 6.9f + std::cos(rib.xRot) * 10.0f, tail.z = -0.5f + std::sin(rib.xRot) * 10.0f;
    tail.xRot = (0.265f + 0.1f * anim) * kPi;
    PartPose& center = c["center_head"];
    center.yRot = c.s.headYaw * kDeg, center.xRot = c.s.pitch * kDeg;
}

// ---- objects ----

// Row-major rotation about a normalised axis.
void axisAngle(float angle, float x, float y, float z, float m[9]) {
    const float cs = std::cos(angle), sn = std::sin(angle), t = 1.0f - cs;
    m[0] = t * x * x + cs, m[1] = t * x * y - sn * z, m[2] = t * x * z + sn * y;
    m[3] = t * x * y + sn * z, m[4] = t * y * y + cs, m[5] = t * y * z - sn * x;
    m[6] = t * x * z - sn * y, m[7] = t * y * z + sn * x, m[8] = t * z * z + cs;
}
void mul3(const float a[9], const float b[9], float out[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}

void endCrystal(const Ctx& c) {  // EndCrystalModel
    c.visible("base", dataInt(c.e, "DATA_SHOW_BOTTOM", 1) != 0);
    const float age = c.s.age;  // EndCrystal.time + partialTick (the renderer's state)
    const float speed = age * 3.0f;
    float hh = std::sin(age * 0.2f) / 2.0f + 0.5f;
    hh = (hh * hh + hh) * 0.4f;
    const float crystalY = (hh - 1.4f) * 16.0f;  // EndCrystalRenderer.getY * 16
    const float s45 = std::sin(kPi / 4);
    float ry[9], ax[9], r[9];
    axisAngle(speed * kDeg, 0, 1, 0, ry);
    axisAngle(kPi / 3, s45, 0, s45, ax);
    PartPose& outer = c["outer_glass"];
    outer.y += crystalY / 2.0f;
    mul3(ry, ax, r);  // Axis.YP.rotationDegrees(speed).rotateAxis(pi / 3, ...)
    rotateBy(outer, r);
    mul3(ax, ry, r);  // setAngleAxis(pi / 3, ...).rotateY(speed)
    rotateBy(c["inner_glass"], r);
    rotateBy(c["cube"], r);
}

void evokerFangs(const Ctx& c) {  // EvokerFangsModel (EvokerFangs.getAnimationProgress)
    const SpeciesState& s = c.e.sp;
    float bite = 0;
    if (s.t[1]) {
        const int remaining = s.t[0] - 2;
        bite = remaining <= 0 ? 1.0f : 1.0f - (float(remaining) - c.s.pt) / 20.0f;
    }
    float amount = std::min(bite * 2.0f, 1.0f);
    amount = 1.0f - amount * amount * amount;
    c["upper_jaw"].zRot = kPi - amount * 0.35f * kPi;
    c["lower_jaw"].zRot = kPi + amount * 0.35f * kPi;
    c["base"].y -= (bite + std::sin(bite * 2.7f)) * 7.2f;
    float pre = 1.0f;
    if (bite > 0.9f) pre *= (1.0f - bite) / 0.1f;
    PartPose& root = c.p[0];
    root.y = 24.0f - 20.0f * pre;
    root.xScale = root.yScale = root.zScale = pre;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------

bool armadilloInShell(const Entity& e) { return armadilloHidden(e); }

void rotateBy(PartPose& p, const float r[9]) {
    // Matrix3f.rotationZYX(z, y, x) * rotation, back to ZYX Euler angles (getEulerAnglesZYX).
    const float cx = std::cos(p.xRot), sx = std::sin(p.xRot), cy = std::cos(p.yRot), sy = std::sin(p.yRot);
    const float cz = std::cos(p.zRot), sz = std::sin(p.zRot);
    const float m[9] = {cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
                        sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
                        -sy, cy * sx, cy * cx};
    float n[9];
    mul3(m, r, n);
    p.xRot = std::atan2(n[7], n[8]);
    p.yRot = std::atan2(-n[6], std::sqrt(std::max(0.0f, 1.0f - n[6] * n[6])));
    p.zRot = std::atan2(n[3], n[0]);
}

void applyKeyframes(const KeyAnimation& a, const Model& model, PartPose* poses, i64 millis, float scale, int root) {
    // KeyframeAnimation.bake: bones by name under the bake root ("root" is the root itself), shallowest first.
    struct Key {
        const KeyAnimation* a;
        const Model* m;
        int root;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<const void*>()(k.a) * 31 ^ std::hash<const void*>()(k.m) * 7 ^ size_t(k.root);
        }
    };
    thread_local std::unordered_map<Key, std::vector<i16>, KeyHash> bindings;
    auto it = bindings.find({&a, &model, root});
    if (it == bindings.end()) {
        std::vector<i16> parts(a.channels.size(), -1);
        for (size_t c = 0; c < a.channels.size(); ++c) {
            const std::string& bone = a.channels[c].bone;
            if (bone == "root") {
                parts[c] = i16(root);
                continue;
            }
            int best = -1, bestDepth = INT_MAX;
            for (size_t i = 0; i < model.parts.size(); ++i) {
                if (model.parts[i].name != bone) continue;
                int depth = 0, at = int(i);
                while (at >= 0 && at != root) at = model.parts[size_t(at)].parent, ++depth;
                if (at == root && depth < bestDepth) best = int(i), bestDepth = depth;
            }
            parts[c] = i16(best);
        }
        it = bindings.emplace(Key{&a, &model, root}, std::move(parts)).first;
    }
    float sec = float(millis) / 1000.0f;
    if (a.looping && a.length > 0) sec = std::fmod(sec, a.length);
    for (size_t c = 0; c < a.channels.size(); ++c) {
        const int part = it->second[c];
        if (part < 0) continue;
        const KeyChannel& ch = a.channels[c];
        const auto& k = ch.keys;
        const int n = int(k.size());
        int lo = 0, len = n;  // Mth.binarySearch(0, n, i -> sec <= k[i].t)
        while (len > 0) {
            const int half = len / 2, mid = lo + half;
            if (sec <= k[size_t(mid)].t) len = half;
            else lo = mid + 1, len -= half + 1;
        }
        const int prev = std::max(0, lo - 1), next = std::min(n - 1, prev + 1);
        const Keyframe &p = k[size_t(prev)], &q = k[size_t(next)];
        const float alpha = next != prev ? std::clamp((sec - p.t) / (q.t - p.t), 0.0f, 1.0f) : 0.0f;
        float v[3];
        if (q.catmullRom) {
            const Keyframe &p0 = k[size_t(std::max(0, prev - 1))], &p3 = k[size_t(std::min(n - 1, next + 1))];
            for (int i = 0; i < 3; ++i) v[i] = catmullrom(alpha, p0.post[i], p.post[i], q.post[i], p3.post[i]) * scale;
        } else {
            for (int i = 0; i < 3; ++i) v[i] = lerp(alpha, p.post[i], q.pre[i]) * scale;
        }
        PartPose& pose = poses[part];
        float* dst = ch.target == 0 ? &pose.x : ch.target == 1 ? &pose.xRot : &pose.xScale;
        for (int i = 0; i < 3; ++i) dst[i] += v[i];
    }
}

u64 animateSpecies(Anim kind, const AnimState& s, const Model& model, PartPose* poses) {
    const Ctx c{model, poses, s, s.entity ? *s.entity : defaultEntity()};
    switch (kind) {
        case Anim::Piglin: piglin(c, false); break;
        case Anim::ZombifiedPiglin: piglin(c, true); break;
        case Anim::Skeleton:  // BoggedModel: a sheared bogged has no mushrooms
            if (c.e.info && c.e.info->name == "minecraft:bogged") c.visible("mushrooms", !dataInt(c.e, "DATA_SHEARED"));
            break;
        case Anim::Enderman: enderman(c); break;
        case Anim::IronGolem: ironGolem(c); break;
        case Anim::Villager:
        case Anim::WanderingTrader: villager(c); break;
        case Anim::Chicken: chicken(c); break;
        case Anim::Witch: witch(c); break;
        case Anim::Illager: illager(c); break;
        case Anim::Sheep: sheep(c); break;
        case Anim::PolarBear: polarBear(c); break;
        case Anim::Goat: goat(c); break;
        case Anim::Panda: panda(c); break;
        case Anim::Turtle: turtle(c); break;
        case Anim::Horse: equine(c); break;
        case Anim::Llama: quadruped(c); break;  // LlamaModel (the chests: the renderer)
        case Anim::Camel: camel(c); break;
        case Anim::Feline: feline(c); break;
        case Anim::Wolf: wolf(c); break;
        case Anim::Fox: fox(c); break;
        case Anim::Rabbit: rabbit(c); break;
        case Anim::Parrot: parrot(c); break;
        case Anim::Bee: bee(c); break;
        case Anim::Allay: allay(c); break;
        case Anim::Bat: bat(c); break;
        case Anim::Hoglin: hoglin(c); break;
        case Anim::Ravager: {  // RavagerModel
            const float stunned = c.e.sp.t[1] > 0 ? float(c.e.sp.t[1]) - s.pt : 0.0f;
            const float attack = c.e.sp.t[0] > 0 ? float(c.e.sp.t[0]) - s.pt : 0.0f;
            const float roar = c.e.sp.t[2] > 0 ? (20.0f - float(c.e.sp.t[2]) + s.pt) / 20.0f : 0.0f;
            PartPose &neck = c["neck"], &mouth = c["mouth"];
            if (attack > 0) {
                const float headAnim = triangleWave(attack, 10.0f), scaled = (1.0f + headAnim) * 0.5f;
                const float headPos = scaled * scaled * scaled * 12.0f, yOff = headPos * std::sin(neck.xRot);
                neck.z = -6.5f + headPos, neck.y = -7.0f - yOff;
                mouth.xRot = attack > 5.0f ? std::sin((-4.0f + attack) / 4.0f) * kPi * 0.4f : kPi / 20 * std::sin(kPi * attack / 10.0f);
            } else {
                const float yOff = -1.0f * std::sin(neck.xRot);
                neck.x = 0, neck.y = -7.0f - yOff, neck.z = 5.5f;
                const bool isStunned = stunned > 0;
                neck.xRot = isStunned ? 0.21991149f : 0.0f;
                mouth.xRot = kPi * (isStunned ? 0.05f : 0.01f);
                if (isStunned) neck.x = float(std::sin(double(stunned) / 40.0 * 10.0)) * 3.0f;
                else if (roar > 0) mouth.xRot = kPi / 2 * std::sin(roar * kPi * 0.25f);
            }
            c.look(c["head"]);
            const float legRot = 0.4f * s.walkSpeed, pos = s.walkPos;
            c["right_hind_leg"].xRot = std::cos(pos * 0.6662f) * legRot;
            c["left_hind_leg"].xRot = std::cos(pos * 0.6662f + kPi) * legRot;
            c["right_front_leg"].xRot = std::cos(pos * 0.6662f + kPi) * legRot;
            c["left_front_leg"].xRot = std::cos(pos * 0.6662f) * legRot;
            break;
        }
        case Anim::Dolphin: dolphin(c); break;
        case Anim::Cod:
        case Anim::TropicalFish: c[kind == Anim::Cod ? "tail_fin" : "tail"].yRot = -(c.e.inWater ? 1.0f : 1.5f) * 0.45f * std::sin(0.6f * s.age); break;
        case Anim::Salmon:
            c["body_back"].yRot = -(c.e.inWater ? 1.0f : 1.3f) * 0.25f * std::sin((c.e.inWater ? 1.0f : 1.7f) * 0.6f * s.age);
            break;
        case Anim::Pufferfish: {  // PufferfishBig / Mid / SmallModel fins
            const bool small = model.part("left_fin") >= 0;
            c[small ? "right_fin" : "right_blue_fin"].zRot = -0.2f + 0.4f * std::sin(s.age * 0.2f);
            c[small ? "left_fin" : "left_blue_fin"].zRot = 0.2f - 0.4f * std::sin(s.age * 0.2f);
            break;
        }
        case Anim::Tadpole: c["tail"].yRot = -(c.e.inWater ? 1.0f : 1.5f) * 0.25f * std::sin(0.3f * s.age); break;
        case Anim::Squid: squid(c); break;
        case Anim::Guardian: guardian(c); break;
        case Anim::Frog: frog(c); break;
        case Anim::Axolotl: axolotl(c); break;
        case Anim::Armadillo: armadillo(c); break;
        case Anim::Sniffer: sniffer(c); break;
        case Anim::CopperGolem: copperGolem(c); break;
        case Anim::SnowGolem: snowGolem(c); break;
        case Anim::Ghast: ghast(c, false); break;
        case Anim::HappyGhast: ghast(c, true); break;
        case Anim::Nautilus: nautilus(c); break;
        case Anim::Blaze: blaze(c); break;
        case Anim::Breeze: breeze(c); break;
        case Anim::Creaking: creaking(c); break;
        case Anim::Dragon: dragon(c); break;
        case Anim::Endermite: segments(c, "segment%d", 4, 0.01f, 0.1f); break;
        case Anim::Silverfish: silverfish(c); break;
        case Anim::MagmaCube: magmaCube(c); break;
        case Anim::Phantom: phantom(c); break;
        case Anim::Strider: strider(c); break;
        case Anim::Vex: vex(c); break;
        case Anim::Warden: warden(c); break;
        case Anim::Wither: wither(c); break;
        case Anim::EndCrystal: endCrystal(c); break;
        case Anim::EvokerFangs: evokerFangs(c); break;
        case Anim::Arrow: {  // ArrowModel: the shake after hitting a block
            const float shake = float(c.e.sp.t[0]) - s.pt;
            if (shake > 0) c.p[0].zRot += -std::sin(shake * 3.0f) * shake * kDeg;
            break;
        }
        case Anim::ShulkerBullet: {  // ShulkerBulletModel
            PartPose& m = c["main"];
            m.yRot = lerp(s.pt, c.e.prevYaw, c.e.yaw) * kDeg, m.xRot = lerp(s.pt, c.e.prevPitch, c.e.pitch) * kDeg;
            break;
        }
        case Anim::WindCharge:  // WindChargeModel
            c["wind_charge"].yRot = -s.age * 16.0f * kDeg;
            c["wind"].yRot = s.age * 16.0f * kDeg;
            break;
        case Anim::WitherSkull: {  // SkullModel: the head follows the skull's rotation
            PartPose& h = c["head"];
            h.yRot = lerp(s.pt, c.e.prevYaw, c.e.yaw) * kDeg, h.xRot = lerp(s.pt, c.e.prevPitch, c.e.pitch) * kDeg;
            break;
        }
        default: break;
    }
    return 0;
}

}  // namespace ent
