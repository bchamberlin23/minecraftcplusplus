#include "worldgen/noise.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace wg {

namespace {
struct Gradient {
    int x, y, z;
};
constexpr Gradient kGradient[16] = {{1, 1, 0},  {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
                                    {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
                                    {1, 1, 0},  {0, -1, 1}, {-1, 1, 0}, {0, -1, -1}};
// Gradient.dot(float...): int * float terms summed left to right in float.
inline float gradDot(int hash, float x, float y, float z) {
    const Gradient& g = kGradient[hash & 15];
    return float(g.x) * x + float(g.y) * y + float(g.z) * z;
}
const double kHalfRoundOff = std::nextafter(1.6777216E7, 0.0);
}  // namespace

Interval Interval::mul(Interval a, Interval b) {
    const float c[4] = {a.min * b.min, a.min * b.max, a.max * b.min, a.max * b.max};
    return {*std::min_element(c, c + 4), *std::max_element(c, c + 4)};
}

double PerlinNoise::wrap(double x) {
    return x >= -kHalfRoundOff && x < kHalfRoundOff ? x : x - std::floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}

PerlinNoise::PerlinNoise(RandomSource& random, double offsetScale) {
    offsetX = random.nextDouble() * offsetScale;
    offsetY = random.nextDouble() * offsetScale;
    offsetZ = random.nextDouble() * offsetScale;
    for (int i = 0; i < 256; ++i) perms_[i] = u8(i);
    for (int i = 0; i < 256; ++i) {
        const int offset = random.nextInt(256 - i);
        std::swap(perms_[i], perms_[offset + i]);
    }
}

float PerlinNoise::get(double _x, double _y, double _z) const {
    const double x = wrap(_x) + offsetX, y = wrap(_y) + offsetY, z = wrap(_z) + offsetZ;
    const int fx = jfloor(x), fy = jfloor(y), fz = jfloor(z);
    const float rx = float(x - fx), ry = float(y - fy), rz = float(z - fz);
    return sampleAndLerp(fx, fy, fz, rx, ry, rz, ry);
}

float PerlinNoise::sampleAndLerp(int x, int y, int z, float rx, float ry, float rz, float originalRy) const {
    const int x0 = permute(x), x1 = permute(x + 1);
    const int xy00 = permute(x0 + y), xy01 = permute(x0 + y + 1), xy10 = permute(x1 + y), xy11 = permute(x1 + y + 1);
    const float d000 = gradDot(permute(xy00 + z), rx, ry, rz);
    const float d100 = gradDot(permute(xy10 + z), rx - 1.0f, ry, rz);
    const float d010 = gradDot(permute(xy01 + z), rx, ry - 1.0f, rz);
    const float d110 = gradDot(permute(xy11 + z), rx - 1.0f, ry - 1.0f, rz);
    const float d001 = gradDot(permute(xy00 + z + 1), rx, ry, rz - 1.0f);
    const float d101 = gradDot(permute(xy10 + z + 1), rx - 1.0f, ry, rz - 1.0f);
    const float d011 = gradDot(permute(xy01 + z + 1), rx, ry - 1.0f, rz - 1.0f);
    const float d111 = gradDot(permute(xy11 + z + 1), rx - 1.0f, ry - 1.0f, rz - 1.0f);
    return lerp3(smoothstep(rx), smoothstep(originalRy), smoothstep(rz), d000, d100, d010, d110, d001, d101, d011,
                 d111);
}

double SmearedPerlinNoise::computeFudgeY(double originalY, double relativeY) const {
    const double limit = (originalY >= 0.0 && originalY < relativeY) ? originalY : relativeY;
    return double(jfloor(limit / fudgeYScale_ + double(1.0E-7f))) * fudgeYScale_;
}

float SmearedPerlinNoise::get(double _x, double _y, double _z) const {
    const double x = wrap(_x) + offsetX, y = wrap(_y) + offsetY, z = wrap(_z) + offsetZ;
    const int fx = jfloor(x), fy = jfloor(y), fz = jfloor(z);
    const float rx = float(x - fx), rz = float(z - fz);
    const double ry = y - fy;
    const float fudged = float(ry - computeFudgeY(_y, ry));
    return sampleAndLerp(fx, fy, fz, rx, fudged, rz, float(ry));
}

void NoiseStack::add(std::shared_ptr<Noise> noise, double frequency, float amplitude) {
    range_ = Interval::add(range_, Interval::mul(noise->range(), Interval::exact(amplitude)));
    layers_.push_back({std::move(noise), frequency, amplitude});
}

float NoiseStack::get(double x, double y, double z) const {
    float value = 0.0f;
    for (const Layer& l : layers_) value += l.amplitude * l.noise->get(x * l.frequency, y * l.frequency, z * l.frequency);
    return value;
}

float NoiseStack::get(double x, double z) const {
    float value = 0.0f;
    for (const Layer& l : layers_) value += l.amplitude * l.noise->get(x * l.frequency, z * l.frequency);
    return value;
}

namespace {
double modifierAt(const std::vector<double>& m, int i) { return m.empty() ? 1.0 : m[size_t(i)]; }

double parityNormalizationFactor(double baseAmplitude, int octaveCount, const std::vector<double>& modifiers) {
    int minOctave = INT32_MAX, maxOctave = INT32_MIN;
    for (int i = 0; i < octaveCount; ++i)
        if (modifierAt(modifiers, i) != 0.0) minOctave = std::min(minOctave, i), maxOctave = std::max(maxOctave, i);
    const double expected = 0.1 * (1.0 + 1.0 / double(maxOctave - minOctave + 1));
    return baseAmplitude * 0.5 * 0.3333333333333333 / expected;
}
}  // namespace

NormalNoise NormalNoise::fromJson(const nlohmann::json& j) {
    NormalNoise n;
    if (auto it = j.find("base_amplitude"); it != j.end()) n.baseAmplitude = it->get<double>();
    if (auto it = j.find("base_octave"); it != j.end()) n.baseOctave = it->get<int>();
    if (auto it = j.find("octave_count"); it != j.end()) n.octaveCount = it->get<int>();
    if (auto it = j.find("normalize"); it != j.end()) {
        if (it->is_boolean())
            n.normalize = it->get<bool>() ? Normalization::Enabled : Normalization::Disabled;
        else
            n.normalize = Normalization::Legacy;
    }
    if (auto it = j.find("amplitude_modifiers"); it != j.end())
        for (const auto& v : *it) n.amplitudeModifiers.push_back(v.get<double>());
    n.finish();
    return n;
}

void NormalNoise::finish() {
    // buildOctaves
    double frequency = std::pow(2.0, baseOctave);
    double amplitude = baseAmplitude;
    if (normalize != Normalization::Disabled)
        amplitude = baseAmplitude * (std::pow(0.5, -(octaveCount - 1)) / (std::pow(0.5, -octaveCount) - 1.0));
    octaves_.clear();
    for (int i = 0; i < octaveCount; ++i) {
        const double m = modifierAt(amplitudeModifiers, i);
        if (m != 0.0) octaves_.push_back({baseOctave + i, frequency, amplitude * m});
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    double target = 0;
    for (const Octave& o : octaves_) target += std::abs(o.amplitude);
    // computeNormalizationFactor
    double variance = 0;
    for (const Octave& o : octaves_) {
        const double dev = PerlinNoise::kStandardDeviation * std::abs(o.amplitude);
        variance += dev * dev;
    }
    const double inputDeviation = std::sqrt(variance);
    double factor = inputDeviation == 0.0 ? 0.0 : (target * 0.3333333333333333) / (inputDeviation * std::sqrt(2.0));
    if (normalize == Normalization::Legacy && factor != 0.0) {
        const double parity = parityNormalizationFactor(baseAmplitude, octaveCount, amplitudeModifiers);
        target *= parity / factor;
        factor = parity;
    }
    normalizationFactor_ = factor;
    range_ = Interval::symmetric(float(target * 0.3333333333333333 * 6.0));
}

std::shared_ptr<NoiseStack> NormalNoise::create(RandomSource& random) const {
    auto first = random.forkPositional();
    auto second = random.forkPositional();
    auto stack = std::make_shared<NoiseStack>();
    for (const Octave& o : octaves_) {
        const std::string seed = "octave_" + std::to_string(o.index);
        auto r1 = first->fromHashOf(seed);
        auto r2 = second->fromHashOf(seed);
        auto n1 = std::make_shared<PerlinNoise>(*r1);
        auto n2 = std::make_shared<PerlinNoise>(*r2);
        const float valueFactor = float(normalizationFactor_ * o.amplitude);
        stack->add(n1, o.frequency, valueFactor);
        stack->add(n2, o.frequency * 1.0181268882175227, valueFactor);
    }
    return stack;
}

}  // namespace wg
