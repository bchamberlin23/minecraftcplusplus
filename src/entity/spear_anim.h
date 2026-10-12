#pragma once
// SpearAnimations: the timing shared by the first- and third-person spear (kinetic_weapon) animations.
#include "core/ease.h"

namespace ent {

// A kinetic_weapon's timing: delay_ticks and the dismount / knockback / damage conditions' max_duration_ticks.
struct KineticParams {
    bool valid = false;
    int delay = 0, dismount = 0, knockback = 0, damage = 0;
    float forward = 0;  // forward_movement
};

// SpearAnimations.UseParams.fromKineticWeapon.
struct SpearUse {
    float raise, raiseStart, raiseMiddle, raiseEnd, sway, lower, raiseBack, swayIntensity, swaySlow, swayFast;
};

inline SpearUse spearUse(const KineticParams& k, float time) {
    using namespace ease;
    const float finishRaising = float(k.delay);
    const float finishSwaying = float(k.dismount) + finishRaising, startSwaying = finishSwaying - 20.0f;
    const float finishLowering = float(k.knockback) + finishRaising, startLowering = finishLowering - 40.0f;
    const float finishRaisingBack = float(k.damage) + finishRaising;
    SpearUse u;
    u.raise = progress(time, 0.0f, finishRaising);
    u.raiseStart = progress(u.raise, 0.0f, 0.5f);
    u.raiseMiddle = progress(u.raise, 0.5f, 0.8f);
    u.raiseEnd = progress(u.raise, 0.8f, 1.0f);
    u.sway = progress(time, startSwaying, finishSwaying);
    u.lower = outCubic(inOutElastic(progress(time - 20.0f, startLowering, finishLowering)));
    u.raiseBack = progress(time, finishRaisingBack - 5.0f, finishRaisingBack);
    u.swayIntensity = 2.0f * outCirc(u.sway) - 2.0f * inCirc(u.raiseBack);
    u.swaySlow = std::sin(time * 19.0f * 0.017453292f) * u.swayIntensity;
    u.swayFast = std::sin(time * 30.0f * 0.017453292f) * u.swayIntensity;
    return u;
}

// SpearAnimations.hitFeedbackAmount (ticks since the last kinetic hit; < 0: none).
inline float spearHitFeedback(float ticks) {
    if (ticks < 0) return 0.0f;
    return 0.4f * (ease::outQuart(ease::progress(ticks, 1.0f, 3.0f)) - ease::inOutSine(ease::progress(ticks, 3.0f, 10.0f)));
}

}  // namespace ent
