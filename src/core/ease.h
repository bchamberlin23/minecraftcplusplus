#pragma once
// net.minecraft.util.Ease: the curves the game's item and arm animations use.
#include <algorithm>
#include <cmath>

namespace ease {

inline float inQuad(float x) { return x * x; }
inline float inOutSine(float x) { return -(std::cos(3.14159265f * x) - 1.0f) / 2.0f; }
inline float outBack(float x) {
    const float t = x - 1.0f;
    return 1.0f + 2.70158f * t * t * t + 1.70158f * t * t;
}
inline float outQuart(float x) {
    const float t = 1.0f - x;
    return 1.0f - t * t * t * t;
}
inline float outCubic(float x) {
    const float t = 1.0f - x;
    return 1.0f - t * t * t;
}
inline float inOutExpo(float x) {
    if (x < 0.5f) return x == 0.0f ? 0.0f : float(std::pow(2.0, 20.0 * x - 10.0) / 2.0);
    return x == 1.0f ? 1.0f : float((2.0 - std::pow(2.0, -20.0 * x + 10.0)) / 2.0);
}
inline float outCirc(float x) { return std::sqrt(std::max(0.0f, 1.0f - (x - 1.0f) * (x - 1.0f))); }
inline float inCirc(float x) { return -std::sqrt(std::max(0.0f, 1.0f - x * x)) + 1.0f; }
inline float inOutElastic(float x) {
    if (x == 0.0f) return 0.0f;
    if (x == 1.0f) return 1.0f;
    const double s = std::sin((20.0 * x - 11.125) * 3.14159265f * 4.0f / 9.0f);
    return x < 0.5f ? float(-(std::pow(2.0, 20.0 * x - 10.0) * s) / 2.0) : float(std::pow(2.0, -20.0 * x + 10.0) * s / 2.0 + 1.0);
}
inline float inOutBack(float x) {
    if (x < 0.5f) return 4.0f * x * x * (7.189819f * x - 2.5949094f) / 2.0f;
    const float dt = 2.0f * x - 2.0f;
    return (dt * dt * (3.5949094f * dt + 2.5949094f) + 2.0f) / 2.0f;
}
// Mth.clamp(Mth.inverseLerp(t, start, end), 0, 1) (SpearAnimations.progress).
inline float progress(float t, float start, float end) {
    return end == start ? (t >= end ? 1.0f : 0.0f) : std::clamp((t - start) / (end - start), 0.0f, 1.0f);
}

}  // namespace ease
