#pragma once
// Java-exact random sources and math helpers used by world generation.
// Everything here must produce bit-identical results to the game, so this code is
// compiled with strict floating point (see CMakeLists.txt) and uses wrapping integer math.
#include "core/common.h"

#include <cmath>
#include <memory>

namespace wg {

// ---- Java semantics helpers ---------------------------------------------------------------
inline i32 jint(i64 v) { return i32(u32(u64(v))); }               // (int) of a long
inline i64 jmul(i64 a, i64 b) { return i64(u64(a) * u64(b)); }     // wrapping long multiply
inline i64 jadd(i64 a, i64 b) { return i64(u64(a) + u64(b)); }
inline i32 jmul32(i32 a, i32 b) { return i32(u32(a) * u32(b)); }
inline i64 lshr(i64 v, int n) { return i64(u64(v) >> n); }         // >>>
inline i64 rotl(i64 v, int n) { return i64((u64(v) << n) | (u64(v) >> (64 - n))); }
// (int) Math.floor(v), with Java's saturating double -> int conversion.
inline i32 jfloor(double v) {
    const double f = std::floor(v);
    if (f != f) return 0;
    if (f >= 2147483647.0) return 2147483647;
    if (f <= -2147483648.0) return -2147483647 - 1;
    return i32(f);
}
inline i64 jlfloor(double v) {
    const double f = std::floor(v);
    if (f != f) return 0;
    if (f >= 9.2233720368547758e18) return INT64_MAX;
    if (f <= -9.2233720368547758e18) return INT64_MIN;
    return i64(f);
}
inline i32 floorDiv(i32 a, i32 b) {
    i32 q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
inline i32 floorMod(i32 a, i32 b) { return a - floorDiv(a, b) * b; }
i32 javaStringHash(std::string_view s);  // String.hashCode (ASCII/UTF-16 BMP)

// ---- Mth ----------------------------------------------------------------------------------
inline float smoothstep(float x) { return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f); }
inline float smoothstepDerivative(float x) { return 30.0f * x * x * (x - 1.0f) * (x - 1.0f); }
inline float lerp(float a, float p0, float p1) { return p0 + a * (p1 - p0); }
inline double lerp(double a, double p0, double p1) { return p0 + a * (p1 - p0); }
inline float lerp2(float a1, float a2, float x00, float x10, float x01, float x11) {
    return lerp(a2, lerp(a1, x00, x10), lerp(a1, x01, x11));
}
inline float lerp3(float a1, float a2, float a3, float x000, float x100, float x010, float x110, float x001,
                   float x101, float x011, float x111) {
    return lerp(a3, lerp2(a1, a2, x000, x100, x010, x110), lerp2(a1, a2, x001, x101, x011, x111));
}
inline float inverseLerp(float v, float lo, float hi) { return (v - lo) / (hi - lo); }
inline double inverseLerp(double v, double lo, double hi) { return (v - lo) / (hi - lo); }
inline float clampedLerp(float f, float lo, float hi) { return f < 0.0f ? lo : f > 1.0f ? hi : lerp(f, lo, hi); }
inline double clampedLerp(double f, double lo, double hi) { return f < 0.0 ? lo : f > 1.0 ? hi : lerp(f, lo, hi); }
inline float clampedMap(float v, float a, float b, float c, float d) { return clampedLerp(inverseLerp(v, a, b), c, d); }
inline double clampedMap(double v, double a, double b, double c, double d) { return clampedLerp(inverseLerp(v, a, b), c, d); }
inline float map(float v, float a, float b, float c, float d) { return lerp(inverseLerp(v, a, b), c, d); }
inline double map(double v, double a, double b, double c, double d) { return lerp(inverseLerp(v, a, b), c, d); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Mth.getSeed(x, y, z): note "x * 3129871" is an int multiply in Java.
inline i64 positionSeed(i32 x, i32 y, i32 z) {
    i64 seed = i64(jmul32(x, 3129871)) ^ jmul(z, 116129781LL) ^ i64(y);
    seed = jadd(jmul(jmul(seed, seed), 42317861LL), jmul(seed, 11LL));
    return seed >> 16;
}

// ---- seeds --------------------------------------------------------------------------------
struct Seed128 {
    i64 lo, hi;
};
i64 mixStafford13(i64 z);
Seed128 upgradeSeedTo128bit(i64 legacySeed);
Seed128 seedFromHashOf(std::string_view input);  // MD5 of the UTF-8 string

class PositionalRandomFactory;

// RandomSource with the game's exact algorithms and method semantics.
class RandomSource {
public:
    virtual ~RandomSource() = default;
    virtual std::unique_ptr<RandomSource> fork() = 0;
    virtual std::unique_ptr<PositionalRandomFactory> forkPositional() = 0;
    virtual void setSeed(i64 seed) = 0;
    virtual i32 nextInt() = 0;
    virtual i32 nextInt(i32 bound) = 0;
    virtual i64 nextLong() = 0;
    virtual bool nextBoolean() = 0;
    virtual float nextFloat() = 0;
    virtual double nextDouble() = 0;
    virtual void consumeCount(int rounds) = 0;
    double nextGaussian();

    i32 nextIntBetweenInclusive(i32 min, i32 max) { return nextInt(max - min + 1) + min; }
    i32 nextInt(i32 origin, i32 bound) { return origin + nextInt(bound - origin); }
    double triangle(double mean, double spread) { return mean + spread * (nextDouble() - nextDouble()); }
    float triangle(float mean, float spread) { return mean + spread * (nextFloat() - nextFloat()); }

protected:
    void resetGaussian() { haveNextGaussian_ = false; }

private:
    double nextNextGaussian_ = 0;
    bool haveNextGaussian_ = false;
};

class PositionalRandomFactory {
public:
    virtual ~PositionalRandomFactory() = default;
    virtual std::unique_ptr<RandomSource> at(i32 x, i32 y, i32 z) const = 0;
    virtual std::unique_ptr<RandomSource> fromHashOf(std::string_view name) const = 0;
    virtual std::unique_ptr<RandomSource> fromSeed(i64 seed) const = 0;
};

class Xoroshiro128PlusPlus {
public:
    Xoroshiro128PlusPlus(i64 lo, i64 hi) : lo_(lo), hi_(hi) {
        if ((lo_ | hi_) == 0) lo_ = -7046029254386353131LL, hi_ = 7640891576956012809LL;
    }
    i64 nextLong() {
        const i64 s0 = lo_;
        i64 s1 = hi_;
        const i64 result = jadd(rotl(jadd(s0, s1), 17), s0);
        s1 ^= s0;
        lo_ = rotl(s0, 49) ^ s1 ^ i64(u64(s1) << 21);
        hi_ = rotl(s1, 28);
        return result;
    }

private:
    i64 lo_, hi_;
};

class XoroshiroRandom final : public RandomSource {
public:
    explicit XoroshiroRandom(i64 seed);
    explicit XoroshiroRandom(Seed128 seed) : rng_(seed.lo, seed.hi) {}
    XoroshiroRandom(i64 lo, i64 hi) : rng_(lo, hi) {}
    std::unique_ptr<RandomSource> fork() override;
    std::unique_ptr<PositionalRandomFactory> forkPositional() override;
    void setSeed(i64 seed) override;
    i32 nextInt() override { return jint(rng_.nextLong()); }
    i32 nextInt(i32 bound) override;
    i64 nextLong() override { return rng_.nextLong(); }
    bool nextBoolean() override { return (rng_.nextLong() & 1) != 0; }
    float nextFloat() override { return float(nextBits(24)) * 5.9604645E-8f; }
    double nextDouble() override { return double(nextBits(53)) * 1.1102230246251565E-16; }
    void consumeCount(int rounds) override {
        for (int i = 0; i < rounds; ++i) rng_.nextLong();
    }

private:
    i64 nextBits(int bits) { return lshr(rng_.nextLong(), 64 - bits); }
    Xoroshiro128PlusPlus rng_;
};

class XoroshiroPositional final : public PositionalRandomFactory {
public:
    XoroshiroPositional(i64 lo, i64 hi) : lo_(lo), hi_(hi) {}
    std::unique_ptr<RandomSource> at(i32 x, i32 y, i32 z) const override {
        return std::make_unique<XoroshiroRandom>(positionSeed(x, y, z) ^ lo_, hi_);
    }
    std::unique_ptr<RandomSource> fromHashOf(std::string_view name) const override {
        const Seed128 s = seedFromHashOf(name);
        return std::make_unique<XoroshiroRandom>(s.lo ^ lo_, s.hi ^ hi_);
    }
    std::unique_ptr<RandomSource> fromSeed(i64 seed) const override {
        return std::make_unique<XoroshiroRandom>(seed ^ lo_, seed ^ hi_);
    }

private:
    i64 lo_, hi_;
};

// java.util.Random's 48-bit LCG ("legacy" random source).
class LegacyRandom : public RandomSource {
public:
    explicit LegacyRandom(i64 seed) { setSeed(seed); }
    std::unique_ptr<RandomSource> fork() override { return std::make_unique<LegacyRandom>(nextLong()); }
    std::unique_ptr<PositionalRandomFactory> forkPositional() override;
    void setSeed(i64 seed) override {
        seed_ = (seed ^ 25214903917LL) & 281474976710655LL;
        resetGaussian();
    }
    virtual i32 next(int bits) {
        seed_ = jadd(jmul(seed_, 25214903917LL), 11LL) & 281474976710655LL;
        return jint(seed_ >> (48 - bits));
    }
    i32 nextInt() override { return next(32); }
    i32 nextInt(i32 bound) override;
    i64 nextLong() override {
        const i32 upper = next(32), lower = next(32);
        return jadd(i64(u64(i64(upper)) << 32), i64(lower));
    }
    bool nextBoolean() override { return next(1) != 0; }
    float nextFloat() override { return float(next(24)) * 5.9604645E-8f; }
    double nextDouble() override {
        const i32 upper = next(26), lower = next(27);
        return double((i64(upper) << 27) + lower) * 1.1102230246251565E-16;
    }
    void consumeCount(int rounds) override {
        for (int i = 0; i < rounds; ++i) next(32);
    }

private:
    i64 seed_ = 0;
};

class LegacyPositional final : public PositionalRandomFactory {
public:
    explicit LegacyPositional(i64 seed) : seed_(seed) {}
    std::unique_ptr<RandomSource> at(i32 x, i32 y, i32 z) const override {
        return std::make_unique<LegacyRandom>(positionSeed(x, y, z) ^ seed_);
    }
    std::unique_ptr<RandomSource> fromHashOf(std::string_view name) const override {
        return std::make_unique<LegacyRandom>(i64(javaStringHash(name)) ^ seed_);
    }
    std::unique_ptr<RandomSource> fromSeed(i64 seed) const override { return std::make_unique<LegacyRandom>(seed); }

private:
    i64 seed_;
};

}  // namespace wg
