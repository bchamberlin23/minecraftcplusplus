#pragma once
// Noise primitives matching the game's 26.x implementations bit for bit (float math).
#include "worldgen/rng.h"

#include <json/json.hpp>

#include <memory>
#include <vector>

namespace wg {

// Closed value range used by the game for density function analysis.
struct Interval {
    float min = 0, max = 0;
    static Interval exact(float v) { return {v, v}; }
    static Interval symmetric(float v) { return {-v, v}; }
    static Interval add(Interval a, Interval b) { return {a.min + b.min, a.max + b.max}; }
    static Interval mul(Interval a, Interval b);
};

class Noise {
public:
    virtual ~Noise() = default;
    virtual Interval range() const = 0;
    virtual float get(double x, double y, double z) const = 0;
    virtual float get(double x, double z) const = 0;
};

// GradientNoise + PerlinNoise.
class PerlinNoise : public Noise {
public:
    static constexpr double kStandardDeviation = 0.2702247831245211;
    explicit PerlinNoise(RandomSource& random, double offsetScale = 256.0);
    Interval range() const override { return Interval::symmetric(2.0f); }
    float get(double x, double y, double z) const override;
    float get(double x, double z) const override { return get(wrap(x), 0.0, wrap(z)); }

    static double wrap(double x);
    double offsetX = 0, offsetY = 0, offsetZ = 0;

protected:
    int permute(int x) const { return perms_[x & 0xFF]; }
    float sampleAndLerp(int x, int y, int z, float rx, float ry, float rz, float originalRy) const;
    u8 perms_[256];
};

// Legacy terrain noise: Perlin with the y fraction "smeared" in steps.
class SmearedPerlinNoise final : public PerlinNoise {
public:
    SmearedPerlinNoise(RandomSource& random, double fudgeYScale) : PerlinNoise(random), fudgeYScale_(fudgeYScale) {}
    Interval range() const override { return Interval::symmetric(float(std::abs(fudgeYScale_) + 2.0)); }
    float get(double x, double y, double z) const override;

private:
    double computeFudgeY(double originalY, double relativeY) const;
    double fudgeYScale_;
};

// Sum of scaled noise layers (octaves).
class NoiseStack final : public Noise {
public:
    struct Layer {
        std::shared_ptr<Noise> noise;
        double frequency;
        float amplitude;
    };
    void add(std::shared_ptr<Noise> noise, double frequency, float amplitude);
    Interval range() const override { return range_; }
    float get(double x, double y, double z) const override;
    float get(double x, double z) const override;
    const std::vector<Layer>& layers() const { return layers_; }

private:
    std::vector<Layer> layers_;
    Interval range_ = Interval::exact(0.0f);
};

// NormalNoise parameters ("noise/*.json") and the noise they create from a random source.
class NormalNoise {
public:
    enum class Normalization { Disabled, Enabled, Legacy };
    static NormalNoise fromJson(const nlohmann::json& j);
    std::shared_ptr<NoiseStack> create(RandomSource& random) const;
    Interval range() const { return range_; }

    double baseAmplitude = 1.0;
    int baseOctave = 0;
    int octaveCount = 1;
    Normalization normalize = Normalization::Enabled;
    std::vector<double> amplitudeModifiers;

private:
    struct Octave {
        int index;
        double frequency, amplitude;
    };
    void finish();
    std::vector<Octave> octaves_;
    double normalizationFactor_ = 0;
    Interval range_;
};

}  // namespace wg
