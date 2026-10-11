// Port of the game's carvers: CaveWorldCarver, CanyonWorldCarver, CarvingMask and the source-chunk
// loop of NoiseBasedChunkGenerator.generateCarvers, with the int/float/height providers they sample.
#include "worldgen/carvers.h"

#include "worldgen/rng.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace wg {

// ---- CarvingMask --------------------------------------------------------------------------
// Bit index y - minY + (z + (x << 4)) * height, like the game's BitSet layout.

void CarvingMask::init(int minY, int maxY) {
    minY_ = minY;
    maxY_ = maxY;
    count_ = 0;
    const size_t bits = maxY >= minY ? size_t(256) * size_t(maxY - minY + 1) : 0;
    bits_.assign((bits + 63) / 64, 0);
}

void CarvingMask::set(int x, int y, int z) {
    const size_t i = size_t(y - minY_) + size_t(z + (x << 4)) * size_t(maxY_ - minY_ + 1);
    u64& word = bits_[i >> 6];
    const u64 bit = u64(1) << (i & 63);
    if (!(word & bit)) {
        word |= bit;
        ++count_;
    }
}

bool CarvingMask::get(int x, int y, int z) const {
    if (y < minY_ || y > maxY_) return false;
    const size_t i = size_t(y - minY_) + size_t(z + (x << 4)) * size_t(maxY_ - minY_ + 1);
    return (bits_[i >> 6] >> (i & 63)) & 1;
}

// CarvingMask.visit: runs of set bits (BitSet.nextSetBit / nextClearBit), split into columns.
void CarvingMask::visit(const std::function<void(int x, int z, int bottomY, int topY)>& fn) const {
    if (count_ == 0) return;
    const i64 height = maxY_ - minY_ + 1;
    const i64 words = i64(bits_.size());
    auto nextSet = [&](i64 from) -> i64 {
        i64 w = from >> 6;
        if (w >= words) return -1;
        u64 v = bits_[size_t(w)] & (~u64(0) << (from & 63));
        while (!v) {
            if (++w == words) return -1;
            v = bits_[size_t(w)];
        }
        return w * 64 + std::countr_zero(v);
    };
    auto nextClear = [&](i64 from) -> i64 {
        i64 w = from >> 6;
        if (w >= words) return from;
        u64 v = ~bits_[size_t(w)] & (~u64(0) << (from & 63));
        while (!v) {
            if (++w == words) return words * 64;
            v = ~bits_[size_t(w)];
        }
        return w * 64 + std::countr_zero(v);
    };
    for (i64 start = nextSet(0); start != -1;) {
        const i64 end = nextClear(start) - 1;
        for (i64 column = start / height; column <= end / height; ++column) {
            const i64 base = column * height;
            const int bottomY = int(std::max<i64>(start - base, 0)) + minY_;
            const int topY = int(std::min<i64>(end - base, height - 1)) + minY_;
            fn(int(column >> 4 & 15), int(column & 15), bottomY, topY);
        }
        start = nextSet(end + 1);
    }
}

namespace {

constexpr double kPi = 3.141592653589793;  // Math.PI

// Mth.sin / Mth.cos: 65536-entry table, index (long)(v * 10430.378350470453) & 65535.
struct SinTable {
    float v[65536];
    SinTable() {
        for (int i = 0; i < 65536; ++i) v[i] = float(std::sin(i / 10430.378350470453));
    }
};
const SinTable kSin;
inline float msin(double v) { return kSin.v[i64(v * 10430.378350470453) & 65535]; }
inline float mcos(double v) { return kSin.v[i64(v * 10430.378350470453 + 16384.0) & 65535]; }

// java.util.Random's LCG with the BitRandomSource methods (LegacyRandomSource / SingleThreadedRandomSource).
// setSeed keeps the gaussian state: the game's WorldgenRandom.setSeed only reseeds the wrapped source, and
// tunnel randoms are always fresh instances.
struct Rng {
    i64 seed = 0;
    double nextNextGaussian = 0;
    bool haveNextGaussian = false;

    explicit Rng(i64 s) { setSeed(s); }
    void setSeed(i64 s) { seed = (s ^ 25214903917LL) & 281474976710655LL; }
    i32 next(int bits) {
        seed = jadd(jmul(seed, 25214903917LL), 11LL) & 281474976710655LL;
        return jint(seed >> (48 - bits));
    }
    i32 nextInt(i32 bound) {
        if (bound <= 0) return 0;  // the game throws; unreachable with valid data
        if ((bound & (bound - 1)) == 0) return i32((i64(bound) * next(31)) >> 31);
        i32 sample, modulo;
        do {
            sample = next(31);
            modulo = sample % bound;
        } while (i32(u32(sample) - u32(modulo) + u32(bound - 1)) < 0);
        return modulo;
    }
    i64 nextLong() {
        const i32 upper = next(32), lower = next(32);
        return jadd(i64(u64(i64(upper)) << 32), i64(lower));
    }
    float nextFloat() { return float(next(24)) * 5.9604645E-8f; }
    double nextDouble() {
        const i32 upper = next(26), lower = next(27);
        return double((i64(upper) << 27) + lower) * double(1.110223E-16f);
    }
    double nextGaussian() {  // MarsagliaPolarGaussian
        if (haveNextGaussian) {
            haveNextGaussian = false;
            return nextNextGaussian;
        }
        double x, y, r;
        do {
            x = 2.0 * nextDouble() - 1.0;
            y = 2.0 * nextDouble() - 1.0;
            r = x * x + y * y;
        } while (r >= 1.0 || r == 0.0);
        const double m = std::sqrt(-2.0 * std::log(r) / r);
        nextNextGaussian = y * m;
        haveNextGaussian = true;
        return x * m;
    }
};

// WorldGenerationContext
struct GenContext {
    int minY = -64, depth = 384, seaLevel = 63;
};

// ---- value providers ------------------------------------------------------------------------
using json = nlohmann::json;

std::string typeOf(const json& j) { return DataPack::normalize(j.at("type").get<std::string>()); }
float jfloat(const json& j) { return float(j.get<double>()); }

template <class T>
const T& pickWeighted(const std::vector<T>& items, const std::vector<i32>& weights, i32 total, Rng& r) {
    i32 selection = r.nextInt(total);  // WeightedList.getRandomOrThrow
    for (size_t i = 0; i < items.size(); ++i) {
        selection -= weights[i];
        if (selection < 0) return items[i];
    }
    return items.back();
}

template <class T, class Parse>
void parseWeighted(const json& j, std::vector<T>& items, std::vector<i32>& weights, i32& total, Parse parse) {
    total = 0;
    for (const json& e : j.at("distribution")) {
        items.push_back(parse(e.at("data")));
        weights.push_back(e.at("weight").get<i32>());
        total += weights.back();
    }
    if (total <= 0) throw std::runtime_error("empty weighted list");
}

struct FloatProvider {
    enum Kind : u8 { Constant, Uniform, ClampedNormal, Trapezoid } kind = Constant;
    float a = 0, b = 0, c = 0, d = 0;  // value | min, max | mean, deviation, min, max | min, max, plateau

    float sample(Rng& r) const {
        switch (kind) {
        case Constant: return a;
        case Uniform: return r.nextFloat() * (b - a) + a;  // Mth.randomBetween
        case ClampedNormal: return clampf(a + float(r.nextGaussian()) * b, c, d);
        case Trapezoid: {
            const float range = b - a;
            const float plateauStart = (range - c) / 2.0f;
            const float plateauEnd = range - plateauStart;
            const float f1 = r.nextFloat();
            const float f2 = r.nextFloat();
            return a + f1 * plateauEnd + f2 * plateauStart;
        }
        }
        return a;
    }

    static FloatProvider parse(const json& j) {
        FloatProvider p;
        if (j.is_number()) {
            p.a = jfloat(j);
            return p;
        }
        const std::string type = typeOf(j);
        if (type == "constant") {
            p.a = jfloat(j.at("value"));
        } else if (type == "uniform") {
            p.kind = Uniform, p.a = jfloat(j.at("min_inclusive")), p.b = jfloat(j.at("max_exclusive"));
        } else if (type == "clamped_normal") {
            p.kind = ClampedNormal, p.a = jfloat(j.at("mean")), p.b = jfloat(j.at("deviation"));
            p.c = jfloat(j.at("min")), p.d = jfloat(j.at("max"));
        } else if (type == "trapezoid") {
            p.kind = Trapezoid, p.a = jfloat(j.at("min")), p.b = jfloat(j.at("max")), p.c = jfloat(j.at("plateau"));
        } else {
            throw std::runtime_error("unknown float provider " + type);
        }
        return p;
    }
};

inline i32 randomBetweenInclusive(Rng& r, i32 min, i32 max) { return r.nextInt(max - min + 1) + min; }

struct IntProvider {
    enum Kind : u8 { Constant, Uniform, BiasedToBottom, VeryBiasedToBottom, Clamped, ClampedNormal, Trapezoid, Weighted };
    Kind kind = Constant;
    i32 min = 0, max = 0, plateau = 0;  // Constant: min
    float mean = 0, deviation = 0;
    std::vector<IntProvider> children;  // Clamped: source; Weighted: entries
    std::vector<i32> weights;
    i32 totalWeight = 0;

    i32 sample(Rng& r) const {
        switch (kind) {
        case Constant: return min;
        case Uniform: return randomBetweenInclusive(r, min, max);
        case BiasedToBottom: return min + r.nextInt(r.nextInt(max - min + 1) + 1);
        case VeryBiasedToBottom: return min + r.nextInt(r.nextInt(r.nextInt(max - min + 1) + 1) + 1);
        case Clamped: return std::min(std::max(children[0].sample(r), min), max);
        case ClampedNormal: return i32(clampf(mean + float(r.nextGaussian()) * deviation, float(min), float(max)));
        case Trapezoid: {
            if (plateau == 0 && max == -min) return r.nextInt(max + 1) - r.nextInt(max + 1);
            const i32 range = max - min;
            if (plateau == range) return randomBetweenInclusive(r, min, max);
            const i32 plateauStart = (range - plateau) / 2;
            const i32 plateauEnd = range - plateauStart;
            const i32 v1 = randomBetweenInclusive(r, 0, plateauEnd);
            return min + v1 + randomBetweenInclusive(r, 0, plateauStart);
        }
        case Weighted: return pickWeighted(children, weights, totalWeight, r).sample(r);
        }
        return min;
    }

    static IntProvider parse(const json& j) {
        IntProvider p;
        if (j.is_number()) {
            p.min = j.get<i32>();
            return p;
        }
        const std::string type = typeOf(j);
        auto range = [&](Kind k, const char* lo, const char* hi) {
            p.kind = k, p.min = j.at(lo).get<i32>(), p.max = j.at(hi).get<i32>();
        };
        if (type == "constant") {
            p.min = j.at("value").get<i32>();
        } else if (type == "uniform") {
            range(Uniform, "min_inclusive", "max_inclusive");
        } else if (type == "biased_to_bottom") {
            range(BiasedToBottom, "min_inclusive", "max_inclusive");
        } else if (type == "very_biased_to_bottom") {
            range(VeryBiasedToBottom, "min_inclusive", "max_inclusive");
        } else if (type == "clamped") {
            range(Clamped, "min_inclusive", "max_inclusive");
            p.children.push_back(parse(j.at("source")));
        } else if (type == "clamped_normal") {
            range(ClampedNormal, "min_inclusive", "max_inclusive");
            p.mean = jfloat(j.at("mean")), p.deviation = jfloat(j.at("deviation"));
        } else if (type == "trapezoid") {
            range(Trapezoid, "min", "max");
            p.plateau = j.at("plateau").get<i32>();
        } else if (type == "weighted_list") {
            p.kind = Weighted;
            parseWeighted(j, p.children, p.weights, p.totalWeight, parse);
        } else {
            throw std::runtime_error("unknown int provider " + type);
        }
        return p;
    }
};

struct Anchor {  // VerticalAnchor
    enum Kind : u8 { Absolute, AboveBottom, BelowTop, RelativeToSeaLevel } kind = Absolute;
    i32 value = 0;

    i32 resolve(const GenContext& c) const {
        switch (kind) {
        case Absolute: return value;
        case AboveBottom: return c.minY + value;
        case BelowTop: return c.depth - 1 + c.minY - value;
        case RelativeToSeaLevel: return c.seaLevel + value;
        }
        return value;
    }

    static bool is(const json& j) {
        return j.is_object() && (j.contains("absolute") || j.contains("above_bottom") || j.contains("below_top") ||
                                 j.contains("relative_to_sea_level"));
    }
    static Anchor parse(const json& j) {
        static const char* const keys[4] = {"absolute", "above_bottom", "below_top", "relative_to_sea_level"};
        for (int k = 0; k < 4; ++k)
            if (j.contains(keys[k])) return {Kind(k), j[keys[k]].get<i32>()};
        throw std::runtime_error("bad vertical anchor");
    }
};

struct HeightProvider {
    enum Kind : u8 { Constant, Uniform, BiasedToBottom, VeryBiasedToBottom, Trapezoid, Weighted } kind = Constant;
    Anchor min, max;  // Constant: min
    i32 inner = 1, plateau = 0;
    std::vector<HeightProvider> children;
    std::vector<i32> weights;
    i32 totalWeight = 0;

    i32 sample(Rng& r, const GenContext& c) const {
        if (kind == Constant) return min.resolve(c);
        if (kind == Weighted) return pickWeighted(children, weights, totalWeight, r).sample(r, c);
        const i32 lo = min.resolve(c), hi = max.resolve(c);
        switch (kind) {
        case Uniform: return lo > hi ? lo : randomBetweenInclusive(r, lo, hi);
        case Trapezoid: {
            if (lo > hi) return lo;
            const i32 range = hi - lo;
            if (plateau >= range) return randomBetweenInclusive(r, lo, hi);
            const i32 plateauStart = (range - plateau) / 2;
            const i32 plateauEnd = range - plateauStart;
            const i32 v1 = randomBetweenInclusive(r, 0, plateauEnd);
            return lo + v1 + randomBetweenInclusive(r, 0, plateauStart);
        }
        case BiasedToBottom: {
            if (hi - lo - inner + 1 <= 0) return lo;
            const i32 limit = r.nextInt(hi - lo - inner + 1);
            return r.nextInt(limit + inner) + lo;
        }
        case VeryBiasedToBottom: {
            if (hi - lo - inner + 1 <= 0) return lo;
            auto nextInt = [&](i32 a, i32 b) { return a >= b ? a : r.nextInt(b - a + 1) + a; };  // Mth.nextInt
            const i32 upper = nextInt(lo + inner, hi);
            const i32 biasedUpper = nextInt(lo, upper - 1);
            return nextInt(lo, biasedUpper - 1 + inner);
        }
        default: return lo;
        }
    }

    static HeightProvider parse(const json& j) {
        HeightProvider p;
        if (Anchor::is(j)) {
            p.min = Anchor::parse(j);
            return p;
        }
        const std::string type = typeOf(j);
        auto range = [&](Kind k) {
            p.kind = k, p.min = Anchor::parse(j.at("min_inclusive")), p.max = Anchor::parse(j.at("max_inclusive"));
        };
        if (type == "constant") {
            p.min = Anchor::parse(j.at("value"));
        } else if (type == "uniform") {
            range(Uniform);
        } else if (type == "trapezoid") {
            range(Trapezoid);
            p.plateau = j.value("plateau", 0);
        } else if (type == "biased_to_bottom") {
            range(BiasedToBottom);
            p.inner = j.value("inner", 1);
        } else if (type == "very_biased_to_bottom") {
            range(VeryBiasedToBottom);
            p.inner = j.value("inner", 1);
        } else if (type == "weighted_list") {
            p.kind = Weighted;
            parseWeighted(j, p.children, p.weights, p.totalWeight, parse);
        } else {
            throw std::runtime_error("unknown height provider " + type);
        }
        return p;
    }
};

// ---- carver configs -------------------------------------------------------------------------

struct Carver {
    bool canyon = false;
    float probability = 0;
    HeightProvider y;
    FloatProvider thickness;  // cave thickness, or the canyon shape's thickness
    // cave
    IntProvider count;
    bool weirdThicknessBias = false;
    FloatProvider roomVerticalRadiusMultiplier, horizontalRadiusMultiplier, verticalRadiusMultiplier;
    FloatProvider startVerticalRadiusMultiplier, floorLevel;
    // canyon
    FloatProvider verticalRotation, distanceFactor, horizontalRadiusFactor, yScale;
    i32 widthSmoothness = 1;
    float verticalRadiusDefaultFactor = 0, verticalRadiusCenterFactor = 0;

    static Carver parse(const json& j) {
        Carver c;
        const std::string type = typeOf(j);
        c.probability = jfloat(j.at("probability"));
        c.y = HeightProvider::parse(j.at("y"));
        if (type == "cave") {
            c.count = IntProvider::parse(j.at("count"));
            c.thickness = FloatProvider::parse(j.at("thickness"));
            c.weirdThicknessBias = j.value("weird_thickness_bias", false);
            c.roomVerticalRadiusMultiplier = FloatProvider::parse(j.at("room_vertical_radius_multiplier"));
            c.horizontalRadiusMultiplier = FloatProvider::parse(j.at("horizontal_radius_multiplier"));
            c.verticalRadiusMultiplier = FloatProvider::parse(j.at("vertical_radius_multiplier"));
            c.startVerticalRadiusMultiplier.a = 1.0f;
            if (j.contains("start_vertical_radius_multiplier"))
                c.startVerticalRadiusMultiplier = FloatProvider::parse(j["start_vertical_radius_multiplier"]);
            c.floorLevel = FloatProvider::parse(j.at("floor_level"));
        } else if (type == "canyon") {
            c.canyon = true;
            c.verticalRotation = FloatProvider::parse(j.at("vertical_rotation"));
            const json& s = j.at("shape");
            c.distanceFactor = FloatProvider::parse(s.at("distance_factor"));
            c.thickness = FloatProvider::parse(s.at("thickness"));
            c.widthSmoothness = s.at("width_smoothness").get<i32>();
            c.horizontalRadiusFactor = FloatProvider::parse(s.at("horizontal_radius_factor"));
            c.verticalRadiusDefaultFactor = jfloat(s.at("vertical_radius_default_factor"));
            c.verticalRadiusCenterFactor = jfloat(s.at("vertical_radius_center_factor"));
            c.yScale = FloatProvider::parse(s.at("y_scale"));
        } else {
            throw std::runtime_error("unknown carver type " + type);
        }
        return c;
    }
};

using CarverList = std::vector<const Carver*>;

// ---- carving ------------------------------------------------------------------------------

// State of one carve() call: the target chunk (the game's chunkPos) and the mask (CarverOutput).
struct Target {
    int minX, minZ;
    double midX, midZ;  // getMiddleBlockX / Z
    GenContext ctx;
    CarvingMask& mask;
    std::vector<float> widthFactors;  // canyon scratch
};

// WorldCarver.carveEllipsoid
template <class Skip>
void carveEllipsoid(Target& t, double x, double y, double z, double horizontalRadius, double verticalRadius,
                    const Skip& skip) {
    const double maxDelta = 16.0 + horizontalRadius * 2.0;
    if (std::abs(x - t.midX) > maxDelta || std::abs(z - t.midZ) > maxDelta) return;
    const int minXIndex = std::max(jfloor(x - horizontalRadius) - t.minX - 1, 0);
    const int maxXIndex = std::min(jfloor(x + horizontalRadius) - t.minX, 15);
    const int minY = std::max(jfloor(y - verticalRadius) - 1, t.mask.minY());
    const int maxY = std::min(jfloor(y + verticalRadius) + 1, t.mask.maxY());
    const int minZIndex = std::max(jfloor(z - horizontalRadius) - t.minZ - 1, 0);
    const int maxZIndex = std::min(jfloor(z + horizontalRadius) - t.minZ, 15);
    for (int xi = minXIndex; xi <= maxXIndex; ++xi) {
        const double xd = (double(t.minX + xi) + 0.5 - x) / horizontalRadius;
        for (int zi = minZIndex; zi <= maxZIndex; ++zi) {
            const double zd = (double(t.minZ + zi) + 0.5 - z) / horizontalRadius;
            if (xd * xd + zd * zd >= 1.0) continue;
            for (int worldY = maxY; worldY > minY; --worldY) {
                const double yd = (double(worldY) - 0.5 - y) / verticalRadius;
                if (!skip(xd, yd, zd, worldY)) t.mask.set(xi, worldY, zi);
            }
        }
    }
}

// WorldCarver.canReach
inline bool canReach(const Target& t, double x, double z, int currentStep, int totalSteps, float thickness) {
    const double xd = x - t.midX, zd = z - t.midZ;
    const double remaining = totalSteps - currentStep;
    const double rr = thickness + 2.0f + 16.0f;
    return xd * xd + zd * zd - remaining * remaining <= rr * rr;
}

constexpr int kMaxDistance = (4 * 2 - 1) << 4;  // (getRange() * 2 - 1) * 16

// The (a - b) * c * scale rotation jitter, drawing a, b, c in order.
inline float jitter(Rng& r, float scale) {
    const float a = r.nextFloat(), b = r.nextFloat(), c = r.nextFloat();
    return (a - b) * c * scale;
}

template <class Skip>
void caveTunnel(Target& t, i64 tunnelSeed, double x, double y, double z, double hMul, double vMul, float thickness,
                float hRot, float vRot, int step, int dist, double yScale, const Skip& skip) {
    Rng random(tunnelSeed);
    const int splitPoint = random.nextInt(dist / 2) + dist / 4;
    const bool steep = random.nextInt(6) == 0;
    float yRota = 0.0f, xRota = 0.0f;
    for (int cur = step; cur < dist; ++cur) {
        const double horizontalRadius = 1.5 + msin(float(kPi) * float(cur) / float(dist)) * thickness;
        const double verticalRadius = horizontalRadius * yScale;
        const float cosX = mcos(vRot);
        x += mcos(hRot) * cosX;
        y += msin(vRot);
        z += msin(hRot) * cosX;
        vRot *= steep ? 0.92f : 0.7f;
        vRot += xRota * 0.1f;
        hRot += yRota * 0.1f;
        xRota *= 0.9f;
        yRota *= 0.75f;
        xRota += jitter(random, 2.0f);
        yRota += jitter(random, 4.0f);
        if (cur == splitPoint && thickness > 1.0f) {
            for (const float turn : {-float(kPi / 2), float(kPi / 2)}) {
                const i64 seed = random.nextLong();
                const float branchThickness = random.nextFloat() * 0.5f + 0.5f;
                caveTunnel(t, seed, x, y, z, hMul, vMul, branchThickness, hRot + turn, vRot / 3.0f, cur, dist, 1.0,
                           skip);
            }
            return;
        }
        if (random.nextInt(4) != 0) {
            if (!canReach(t, x, z, cur, dist, thickness)) return;
            carveEllipsoid(t, x, y, z, horizontalRadius * hMul, verticalRadius * vMul, skip);
        }
    }
}

// CaveWorldCarver.carve
void carveCave(const Carver& cv, Rng& random, Target& t, int sourceX, int sourceZ) {
    const int caveCount = cv.count.sample(random);
    for (int cave = 0; cave < caveCount; ++cave) {
        const double x = double((sourceX << 4) + random.nextInt(16));
        const double y = double(cv.y.sample(random, t.ctx));
        const double z = double((sourceZ << 4) + random.nextInt(16));
        const double hMul = cv.horizontalRadiusMultiplier.sample(random);
        const double vMul = cv.verticalRadiusMultiplier.sample(random);
        const double startVMul = cv.startVerticalRadiusMultiplier.sample(random);
        const double floorLevel = cv.floorLevel.sample(random);
        const auto skip = [floorLevel](double xd, double yd, double zd, int) {
            return yd <= floorLevel ? true : xd * xd + yd * yd + zd * zd >= 1.0;
        };
        int tunnels = 1;
        if (random.nextInt(4) == 0) {  // createRoom
            const double yScale = cv.roomVerticalRadiusMultiplier.sample(random);
            const float thickness = 1.0f + random.nextFloat() * 6.0f;
            const double horizontalRadius = 1.5 + msin(float(kPi / 2)) * thickness;
            carveEllipsoid(t, x + 1.0, y, z, horizontalRadius, horizontalRadius * yScale, skip);
            tunnels += random.nextInt(4);
        }
        for (int i = 0; i < tunnels; ++i) {
            const float hRot = random.nextFloat() * float(kPi * 2);
            const float vRot = (random.nextFloat() - 0.5f) / 4.0f;
            float thickness = cv.thickness.sample(random);  // getThickness
            if (cv.weirdThicknessBias && random.nextInt(10) == 0) {
                const float a = random.nextFloat(), b = random.nextFloat();
                thickness *= a * b * 3.0f + 1.0f;
            }
            const int distance = kMaxDistance - random.nextInt(kMaxDistance / 4);
            const i64 seed = random.nextLong();
            caveTunnel(t, seed, x, y, z, hMul, vMul, thickness, hRot, vRot, 0, distance, startVMul, skip);
        }
    }
}

// CanyonWorldCarver.carve / doCarve
void carveCanyon(const Carver& cv, Rng& outer, Target& t, int sourceX, int sourceZ) {
    double x = double((sourceX << 4) + outer.nextInt(16));
    double y = double(cv.y.sample(outer, t.ctx));
    double z = double((sourceZ << 4) + outer.nextInt(16));
    float hRot = outer.nextFloat() * float(kPi * 2);
    float vRot = cv.verticalRotation.sample(outer);
    const double yScale = cv.yScale.sample(outer);
    const float thickness = cv.thickness.sample(outer);
    const int distance = int(float(kMaxDistance) * cv.distanceFactor.sample(outer));

    Rng random(outer.nextLong());
    std::vector<float>& widthFactors = t.widthFactors;  // initWidthFactors
    widthFactors.resize(size_t(std::max(t.ctx.depth, 0)));
    float widthFactor = 1.0f;
    for (int yi = 0; yi < t.ctx.depth; ++yi) {
        if (yi == 0 || random.nextInt(cv.widthSmoothness) == 0) {
            const float a = random.nextFloat(), b = random.nextFloat();
            widthFactor = 1.0f + a * b;
        }
        widthFactors[size_t(yi)] = widthFactor * widthFactor;
    }
    const int minGenY = t.ctx.minY;
    const auto skip = [&widthFactors, minGenY](double xd, double yd, double zd, int worldY) {
        return (xd * xd + zd * zd) * widthFactors[size_t(worldY - minGenY - 1)] + yd * yd / 6.0 >= 1.0;
    };

    float yRota = 0.0f, xRota = 0.0f;
    for (int cur = 0; cur < distance; ++cur) {
        double horizontalRadius = 1.5 + msin(float(cur) * float(kPi) / float(distance)) * thickness;
        double verticalRadius = horizontalRadius * yScale;
        horizontalRadius *= cv.horizontalRadiusFactor.sample(random);
        {  // updateVerticalRadius
            const float verticalMultiplier = 1.0f - std::abs(0.5f - float(cur) / float(distance)) * 2.0f;
            const float factor = cv.verticalRadiusDefaultFactor + cv.verticalRadiusCenterFactor * verticalMultiplier;
            const float between = random.nextFloat() * (1.0f - 0.75f) + 0.75f;
            verticalRadius = factor * verticalRadius * between;
        }
        const float xc = mcos(vRot), xs = msin(vRot);
        x += mcos(hRot) * xc;
        y += xs;
        z += msin(hRot) * xc;
        vRot *= 0.7f;
        vRot += xRota * 0.05f;
        hRot += yRota * 0.05f;
        xRota *= 0.8f;
        yRota *= 0.5f;
        xRota += jitter(random, 2.0f);
        yRota += jitter(random, 4.0f);
        if (random.nextInt(4) != 0) {
            if (!canReach(t, x, z, cur, distance, thickness)) return;
            carveEllipsoid(t, x, y, z, horizontalRadius, verticalRadius, skip);
        }
    }
}

}  // namespace

// ---- Carvers ------------------------------------------------------------------------------

struct Carvers::Impl {
    const DataPack& data;
    const BiomeTable& biomes;
    i64 seed;

    std::mutex mutex;  // guards byId, owned and lists
    std::unordered_map<std::string, const Carver*> byId;
    std::vector<std::unique_ptr<Carver>> owned;
    std::unordered_map<u16, CarverList> lists;  // node-based: references stay valid

    // Carver list per source chunk (the game memoises it per chunk too, via ChunkAccess.carverBiome).
    struct Shard {
        std::mutex mutex;
        std::unordered_map<i64, const CarverList*> map;
    };
    static constexpr size_t kShards = 32, kShardCapacity = 4096;
    std::array<Shard, kShards> shards;

    Impl(const DataPack& d, const BiomeTable& b, i64 s) : data(d), biomes(b), seed(s) {}

    const Carver* parseCarver(const json& j, const std::string& what) {
        try {
            owned.push_back(std::make_unique<Carver>(Carver::parse(j)));
            return owned.back().get();
        } catch (const std::exception& e) {
            LOG("error: carver %s: %s", what.c_str(), e.what());
            return nullptr;
        }
    }

    const Carver* carverById(const std::string& rawId) {
        const std::string id = DataPack::normalize(rawId);
        if (auto it = byId.find(id); it != byId.end()) return it->second;
        const json* j = data.worldgen("carver", id);
        const Carver* c = nullptr;
        if (j) c = parseCarver(*j, "'" + id + "'");
        else LOG("error: carver '%s' missing from the game data", id.c_str());
        byId.emplace(id, c);
        return c;
    }

    // BiomeGenerationSettings.getCarvers (a HolderSet: id, inline object, "#tag" or a list of those).
    const CarverList& listFor(u16 biome) {
        std::lock_guard lock(mutex);
        if (auto it = lists.find(biome); it != lists.end()) return it->second;
        CarverList& list = lists[biome];
        const std::string name = biomes.name(biome);
        const json* b = data.worldgen("biome", name);
        if (!b || !b->contains("carvers")) {
            LOG("error: biome '%s' has no carvers entry", name.c_str());
            return list;
        }
        auto add = [&](const json& e) {
            const Carver* c = nullptr;
            if (e.is_string()) c = carverById(e.get<std::string>());
            else if (e.is_object()) c = parseCarver(e, "inline in " + name);
            if (c) list.push_back(c);
        };
        const json& carvers = (*b)["carvers"];
        if (carvers.is_string() && carvers.get<std::string>().starts_with("#")) {
            for (const std::string& id : data.tag("worldgen/carver", carvers.get<std::string>().substr(1)))
                if (const Carver* c = carverById(id)) list.push_back(c);
        } else if (carvers.is_array()) {
            for (const json& e : carvers) add(e);
        } else {
            add(carvers);
        }
        return list;
    }

    const CarverList& at(int sourceX, int sourceZ, const NoiseBiomeFn& noiseBiome) {
        const i64 key = i64(u64(u32(sourceX)) << 32 | u32(sourceZ));
        Shard& shard = shards[size_t(u64(key * 0x9E3779B97F4A7C15ULL) >> 59) % kShards];
        {
            std::lock_guard lock(shard.mutex);
            if (auto it = shard.map.find(key); it != shard.map.end()) return *it->second;
        }
        // getBiomeGenerationSettingsForCarver: the noise biome at the source chunk's minimum corner, quart y 0.
        const CarverList& list = listFor(noiseBiome(sourceX << 2, 0, sourceZ << 2));
        std::lock_guard lock(shard.mutex);
        if (shard.map.size() >= kShardCapacity) shard.map.clear();
        shard.map.emplace(key, &list);
        return list;
    }
};

Carvers::Carvers(const NoiseState& noise, const BiomeTable& biomes)
    : impl_(std::make_unique<Impl>(noise.data(), biomes, noise.seed())) {
    for (size_t id = 0; id < biomes.size(); ++id) impl_->listFor(u16(id));
}

Carvers::~Carvers() = default;

// NoiseBasedChunkGenerator.generateCarvers. The caller sets the mask's range (the game's
// CarvingMask(minGenY + 1, top - protectedBlocksOnTop)); carvers clamp to it like CarverOutput.
void Carvers::carve(int chunkX, int chunkZ, const NoiseBiomeFn& noiseBiome, CarvingMask& mask, int genMinY,
                    int genDepth) const {
    Target t{chunkX << 4, chunkZ << 4, double((chunkX << 4) + 8), double((chunkZ << 4) + 8),
             GenContext{genMinY, genDepth, 63}, mask, {}};
    Rng random(0);  // WorldgenRandom; every use is preceded by setLargeFeatureSeed
    for (int dx = -8; dx <= 8; ++dx) {
        for (int dz = -8; dz <= 8; ++dz) {
            const int sourceX = chunkX + dx, sourceZ = chunkZ + dz;
            i64 index = 0;
            for (const Carver* cv : impl_->at(sourceX, sourceZ, noiseBiome)) {
                // WorldgenRandom.setLargeFeatureSeed(seed + index, sourceX, sourceZ)
                const i64 seed = jadd(impl_->seed, index++);
                random.setSeed(seed);
                const i64 xScale = random.nextLong();
                const i64 zScale = random.nextLong();
                random.setSeed(jmul(sourceX, xScale) ^ jmul(sourceZ, zScale) ^ seed);
                if (!(random.nextFloat() <= cv->probability)) continue;  // isStartChunk
                if (cv->canyon) carveCanyon(*cv, random, t, sourceX, sourceZ);
                else carveCave(*cv, random, t, sourceX, sourceZ);
            }
        }
    }
}

}  // namespace wg
