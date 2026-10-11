// Density functions: a port of the game's densityfunction package (26.x "sampler" design).
// JSON is parsed into immutable definition trees (the game's DensityFunction records), rewritten
// with the game's compiler rules (reference inlining, cache deduplication, uniform-axis slicing)
// and compiled into sampler nodes that reproduce DensitySampler.sampleValue / sampleVolume exactly.
#include "worldgen/density.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace wg {

// Compiled sampler node (the game's DensitySampler). Stateless; per-call state lives in the context.
class DensityFunction {
public:
    virtual ~DensityFunction() = default;
    virtual float value(DensityContext::Impl& c, int x, int y, int z) const = 0;
    // out[v.index(x, y, z)] for the whole volume (DensitySampler.sampleVolume)
    virtual void fill(DensityContext::Impl& c, float* out, const Volume& v) const = 0;
};

namespace {

using Fn = DensityRef;
using Ctx = DensityContext::Impl;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// ---- Java float semantics ------------------------------------------------------------------
inline float jmin(float a, float b) {  // Math.min(float, float)
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && std::signbit(b)) return b;
    return a <= b ? a : b;
}
inline float jmax(float a, float b) {  // Math.max(float, float)
    if (a != a) return a;
    if (a == 0.0f && b == 0.0f && std::signbit(a)) return b;
    return a >= b ? a : b;
}
inline float jclamp(float v, float lo, float hi) { return v < lo ? lo : jmin(v, hi); }  // Mth.clamp
inline float jsignum(float f) { return (f == 0.0f || f != f) ? f : std::copysign(1.0f, f); }
inline float jsqrt(float f) { return float(std::sqrt(double(f))); }
inline i32 f2i(float f) {  // (int) of a float
    if (f != f) return 0;
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f <= -2147483648.0f) return INT32_MIN;
    return i32(f);
}
inline i32 jround(float a) {  // Math.round(float)
    u32 bits;
    std::memcpy(&bits, &a, 4);
    const int biasedExp = int((bits & 0x7F800000u) >> 23);
    const int shift = (23 - 1 + 127) - biasedExp;
    if ((shift & -32) == 0) {
        i32 r = i32((bits & 0x007FFFFFu) | 0x00800000u);
        if (i32(bits) < 0) r = -r;
        return ((r >> shift) + 1) >> 1;
    }
    return f2i(a);
}
inline double jpow(double a, double b) {  // Math.pow special cases that differ from C
    if (b != b) return b;
    if (std::isinf(b) && std::fabs(a) == 1.0) return std::numeric_limits<double>::quiet_NaN();
    return std::pow(a, b);
}
inline i64 packPos(int x, int y, int z) {  // BlockPos.asLong
    return i64((u64(i64(x)) & 0x3FFFFFFull) << 38 | (u64(i64(y)) & 0xFFFull) | (u64(i64(z)) & 0x3FFFFFFull) << 12);
}
inline bool isZeroBits(float f) { return f == 0.0f && !std::signbit(f); }

// ---- Volume helpers (DensityVolume) ----------------------------------------------------------
inline bool sameVolume(const Volume& a, const Volume& b) {
    return a.sizeX == b.sizeX && a.sizeY == b.sizeY && a.sizeZ == b.sizeZ && a.minX == b.minX && a.minY == b.minY &&
           a.minZ == b.minZ && a.stepX == b.stepX && a.stepY == b.stepY && a.stepZ == b.stepZ;
}
inline int maxBlockX(const Volume& v) { return v.minX + v.sizeX * v.stepX - 1; }
inline int maxBlockY(const Volume& v) { return v.minY + v.sizeY * v.stepY - 1; }
inline int maxBlockZ(const Volume& v) { return v.minZ + v.sizeZ * v.stepZ - 1; }
int indexOfBlock(const Volume& v, int x, int y, int z) {
    const int rx = x - v.minX, ry = y - v.minY, rz = z - v.minZ;
    if (v.stepX == 1 && v.stepY == 1 && v.stepZ == 1) {
        if (rx >= 0 && ry >= 0 && rz >= 0 && rx < v.sizeX && ry < v.sizeY && rz < v.sizeZ) return v.index(rx, ry, rz);
    } else if (rx >= 0 && ry >= 0 && rz >= 0 && rx < v.sizeX * v.stepX && ry < v.sizeY * v.stepY &&
               rz < v.sizeZ * v.stepZ && floorMod(rx, v.stepX) == 0 && floorMod(ry, v.stepY) == 0 &&
               floorMod(rz, v.stepZ) == 0) {
        return v.index(floorDiv(rx, v.stepX), floorDiv(ry, v.stepY), floorDiv(rz, v.stepZ));
    }
    return -1;
}

// ---- Interval (net.minecraft.util.Interval) ---------------------------------------------------
struct Range {
    float min = 0, max = 0;
    bool nai() const { return min != min; }
    bool contains(float v) const { return v >= min && v <= max; }
};
const Range kNaI{kNaN, kNaN};
const Range kInfinite{-kInf, kInf};
inline Range rExact(float v) { return {v, v}; }
inline Range rSym(float v) { return {-v, v}; }
inline float mulBound(float a, float b) { return a != 0.0f && b != 0.0f ? a * b : 0.0f; }
Range rAdd(Range l, Range r) {
    const float mn = l.min + r.min, mx = l.max + r.max;
    return mn == mn && mx == mx ? Range{mn, mx} : kNaI;
}
Range rSub(Range l, Range r) {
    const float mn = l.min - r.max, mx = l.max - r.min;
    return mn == mn && mx == mx ? Range{mn, mx} : kNaI;
}
Range rMul(Range l, Range r) {
    if (l.nai() || r.nai()) return kNaI;
    const float a = mulBound(l.min, r.min), b = mulBound(l.min, r.max), c = mulBound(l.max, r.min),
                d = mulBound(l.max, r.max);
    return {jmin(jmin(a, b), jmin(c, d)), jmax(jmax(a, b), jmax(c, d))};
}
Range rReciprocal(Range in) {
    if (in.nai() || (in.min == 0.0f && in.max == 0.0f)) return kNaI;
    if (!in.contains(0.0f)) return {1.0f / in.max, 1.0f / in.min};
    if (in.max == 0.0f) return {-kInf, 1.0f / in.min};
    return in.min == 0.0f ? Range{1.0f / in.max, kInf} : kInfinite;
}
Range rDiv(Range l, Range r) { return rMul(l, rReciprocal(r)); }
Range rMin(Range l, Range r) { return l.nai() || r.nai() ? kNaI : Range{jmin(l.min, r.min), jmin(l.max, r.max)}; }
Range rMax(Range l, Range r) { return l.nai() || r.nai() ? kNaI : Range{jmax(l.min, r.min), jmax(l.max, r.max)}; }
Range rClamp(Range in, float lo, float hi) {
    if (in.nai()) return kNaI;
    if (in.min >= hi) return {hi, hi};
    if (in.max <= lo) return {lo, lo};
    return {jmax(in.min, lo), jmin(in.max, hi)};
}
Range rAbs(Range in) {
    if (in.nai()) return kNaI;
    const float a = std::fabs(in.min), b = std::fabs(in.max), mx = jmax(a, b);
    return in.contains(0.0f) ? Range{0.0f, mx} : Range{jmin(a, b), mx};
}
Range rSquare(Range in) {
    if (in.nai()) return kNaI;
    const float a = in.min * in.min, b = in.max * in.max, mx = jmax(a, b);
    return in.contains(0.0f) ? Range{0.0f, mx} : Range{jmin(a, b), mx};
}
Range rEnc(float a, float b) {
    if (a != a && b != b) return kNaI;
    if (a != a) return rExact(b);
    if (b != b) return rExact(a);
    return {jmin(a, b), jmax(a, b)};
}
Range rEnc(Range first, float second) {
    if (second != second) return first;
    return first.nai() ? rExact(second) : Range{jmin(first.min, second), jmax(first.max, second)};
}
Range rEncList(std::initializer_list<Range> list) {
    float mn = kInf, mx = -kInf;
    for (const Range& r : list)
        if (!r.nai()) mn = jmin(r.min, mn), mx = jmax(r.max, mx);
    return mx < mn ? kNaI : Range{mn, mx};
}
Range rEncVec(const std::vector<Range>& list) {
    float mn = kInf, mx = -kInf;
    for (const Range& r : list)
        if (!r.nai()) mn = jmin(r.min, mn), mx = jmax(r.max, mx);
    return mx < mn ? kNaI : Range{mn, mx};
}
template <class F>
Range rMapMonotonic(Range in, F op) {
    if (in.nai()) return kNaI;
    const float a = op(in.min), b = op(in.max);
    if (a != a || b != b) return kNaI;  // the game throws here
    return {jmin(a, b), jmax(a, b)};
}
Range rLog(Range in) {
    if (in.max < 0.0f) return kNaI;
    return rMapMonotonic(rMax(in, rExact(0.0f)), [](float x) { return float(std::log(double(x))); });
}
Range rPowZeroBase(Range e) {
    if (e.contains(0.0f)) {
        if (e.max == 0.0f) return {1.0f, kInf};
        return e.min == 0.0f ? Range{0.0f, 1.0f} : Range{0.0f, kInf};
    }
    return e.max < 0.0f ? rExact(kInf) : rExact(0.0f);
}
inline float fpow(float b, float e) { return float(jpow(double(b), double(e))); }
Range rPowScalar(float base, Range e) {
    if (base != base || e.nai()) return kNaI;
    if (e.min == e.max) {
        const float v = fpow(base, e.min);
        return v != v ? kNaI : rExact(v);
    }
    if (base == 0.0f) return rMul(rPowZeroBase(e), rExact(std::copysign(1.0f, base)));
    if (base == 1.0f) return rExact(1.0f);
    if (base > 0.0f) {
        if (std::isfinite(e.min) && std::isfinite(e.max)) return rEnc(fpow(base, e.min), fpow(base, e.max));
        if (std::isinf(e.min) && std::isinf(e.max)) return {0.0f, kInf};
        if (std::isinf(e.min))
            return base < 1.0f ? Range{fpow(base, e.max), kInf} : Range{0.0f, fpow(base, e.max)};
        return base < 1.0f ? Range{0.0f, fpow(base, e.min)} : Range{fpow(base, e.min), kInf};
    }
    const float minInt = float(std::ceil(double(e.min))), maxInt = float(std::floor(double(e.max)));
    if (maxInt < minInt) return kNaI;
    const float bMin = fpow(base, minInt), bMax = fpow(base, maxInt);
    Range r = rEnc(bMin, bMax);
    if (std::isinf(minInt))
        r = rEnc(r, -bMin);
    else if (minInt + 1.0f < maxInt)
        r = rEnc(r, fpow(base, minInt + 1.0f));
    if (std::isinf(maxInt))
        r = rEnc(r, -bMax);
    else if (maxInt - 1.0f > minInt)
        r = rEnc(r, fpow(base, maxInt - 1.0f));
    return r;
}
Range rPow(Range base, Range e) {
    if (base.nai() || e.nai()) return kNaI;
    if (base.min == base.max) return rPowScalar(base.min, e);
    Range r = rEncList({rPowScalar(base.min, e), rPowScalar(base.max, e)});
    if (base.contains(0.0f)) {
        if (base.max > 0.0f) r = rEncList({r, rPowScalar(0.0f, e)});
        if (base.min < 0.0f) r = rEncList({r, rPowScalar(-0.0f, e)});
    }
    return r;
}
float lerpInfiniteBound(float alpha, float first, float second) {
    const float a = mulBound(1.0f - alpha, first), b = mulBound(alpha, second);
    if (!std::isinf(a) || !std::isinf(b)) return a + b;
    if (alpha <= 0.0f) return second > first ? -kInf : kInf;
    if (alpha >= 1.0f) return second > first ? kInf : -kInf;
    return kNaN;
}
Range rLerp(Range alpha, float first, float second) {
    if (alpha.nai() || first != first || second != second) return kNaI;
    if (std::isfinite(first) && std::isfinite(second))
        return rEnc(first + mulBound(alpha.min, second - first), first + mulBound(alpha.max, second - first));
    if (first == second) return rExact(first);
    const float a = lerpInfiniteBound(alpha.min, first, second), b = lerpInfiniteBound(alpha.max, first, second);
    return a == a && b == b ? rEnc(a, b) : kNaI;
}
Range rLerp(Range alpha, Range first, Range second) {
    if (alpha.nai() || first.nai() || second.nai()) return kNaI;
    return rEncList({rLerp(alpha, first.min, second.min), rLerp(alpha, first.max, second.min),
                     rLerp(alpha, first.min, second.max), rLerp(alpha, first.max, second.max)});
}
Range rSign(Range in) {
    if (in.nai()) return kNaI;
    if (in.min == in.max) return rExact(jsignum(in.min));
    if (in.contains(0.0f)) {
        if (in.min == 0.0f) return {0.0f, 1.0f};
        return in.max == 0.0f ? Range{-1.0f, 0.0f} : Range{-1.0f, 1.0f};
    }
    return rExact(in.min > 0.0f ? 1.0f : -1.0f);
}

// ---- Definitions (the game's DensityFunction records) -----------------------------------------
enum class K : u8 {
    Const, Ref, Noise, ShiftA, ShiftB, Shift, EndIslands, Distance, Gradient, BlendAlpha, BlendOffset, Beardifier,
    Unary, Round, Binary, Pow, Spline, Lerp, Clamp, RangeChoice, IntervalSelect, Cache, BlendDensity, Interpolated,
    Slice, FindTopSurface, OldBlendedNoise, Prepared
};
enum Unary { kAbs, kSquare, kCube, kSqrt, kHalfNeg, kQuarterNeg, kReciprocal, kNegate, kSqueeze, kLog, kSign };
enum Binary { kAdd, kSub, kMul, kDiv, kMin, kMax };
enum RoundType { kFloor, kRound, kCeil, kTruncate };
enum Tiling { kClampToEdge, kRepeat, kMirroredRepeat };
enum Metric { kEuclidean, kEuclideanSquared, kManhattan, kChebyshev };
constexpr int kAxisX = 1, kAxisY = 2, kAxisZ = 4;  // axis index a (0, 1, 2) has bit 1 << a

struct Def;
using DefPtr = std::shared_ptr<const Def>;
struct SplineDef;
using SplinePtr = std::shared_ptr<const SplineDef>;
struct SplineDef {  // CubicSpline: a constant when coord is null, else a Multipoint
    float value = 0;
    DefPtr coord;
    std::vector<float> loc, der;
    std::vector<SplinePtr> vals;
};

struct Def {
    K kind = K::Const;
    int op = 0;
    std::array<int, 4> i{};
    std::array<float, 2> f{};
    std::array<double, 5> d{};
    std::string str;              // noise key or registry id (normalized)
    std::vector<DefPtr> kids;     // child functions in record field order
    std::vector<float> floats;    // interval_select thresholds
    SplinePtr spline;
    Fn sampler;                   // Prepared: the caching sampler
    Range preparedRange;
    int preparedAxes = 0;
    u32 sid = 0;                  // structural identity: equal sids <=> equal records in the game
    mutable int axesMemo = -1;
    mutable bool rangeDone = false;
    mutable Range rangeMemo;
};

// ---- Noise with the game's volume path (Noise.addToVolume) ------------------------------------
struct PermsAccess : PerlinNoise {  // reads PerlinNoise's protected permutation table
    static const u8* of(const PerlinNoise& n) { return n.*(&PermsAccess::perms_); }
};

struct Grad {
    int x, y, z;
};
constexpr Grad kGrad[16] = {{1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
                            {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
                            {1, 1, 0},  {0, -1, 1},  {-1, 1, 0}, {0, -1, -1}};

struct NoiseSrc {
    struct Layer {
        const u8* perms;
        double ox, oy, oz;
        double frequency;
        float amplitude;
        double fudgeY;  // SmearedPerlinNoise only
        bool smeared;
    };
    std::shared_ptr<const Noise> noise;  // point path (NoiseStack.get)
    std::vector<Layer> layers;

    float get(double x, double y, double z) const { return noise->get(x, y, z); }
    void addToVolume(float* buf, const Volume& v, double xzScale, double yScale, float amplitude) const;
};

double computeFudgeY(double originalY, double relativeY, double fudgeYScale) {
    const double limit = (originalY >= 0.0 && originalY < relativeY) ? originalY : relativeY;
    return double(jfloor(limit / fudgeYScale + double(1.0E-7f))) * fudgeYScale;
}

// PerlinNoise.addToVolume / SmearedPerlinNoise.addToVolume.
template <bool Smeared>
void perlinAddToVolume(const NoiseSrc::Layer& n, float* buf, const Volume& v, double xzScale, double yScale,
                       float amplitude) {
    const u8* p = n.perms;
    auto permute = [p](int x) { return int(p[x & 0xFF]); };
    float d000xz = 0, d100xz = 0, d010xz = 0, d110xz = 0, d001xz = 0, d101xz = 0, d011xz = 0, d111xz = 0;
    float g000y = 0, g100y = 0, g010y = 0, g110y = 0, g001y = 0, g101y = 0, g011y = 0, g111y = 0;
    int index = 0;
    for (int iz = 0; iz < v.sizeZ; ++iz) {
        const double z = PerlinNoise::wrap(double(v.blockZ(iz)) * xzScale) + n.oz;
        const int floorZ = jfloor(z);
        const float relZ = float(z - floorZ);
        const float alphaZ = smoothstep(relZ);
        for (int ix = 0; ix < v.sizeX; ++ix) {
            const double x = PerlinNoise::wrap(double(v.blockX(ix)) * xzScale) + n.ox;
            const int floorX = jfloor(x);
            const float relX = float(x - floorX);
            const int x0 = permute(floorX), x1 = permute(floorX + 1);
            const float alphaX = smoothstep(relX);
            int lastFloorY = INT32_MIN;
            for (int iy = 0; iy < v.sizeY; ++iy) {
                int floorY;
                float relY = 0, alphaY;
                double originalY = 0, relativeY = 0;
                if constexpr (Smeared) {
                    originalY = double(v.blockY(iy)) * yScale;
                    const double y = PerlinNoise::wrap(originalY) + n.oy;
                    floorY = jfloor(y);
                    relativeY = y - floorY;
                    alphaY = smoothstep(float(relativeY));
                } else {
                    const double y = PerlinNoise::wrap(double(v.blockY(iy)) * yScale) + n.oy;
                    floorY = jfloor(y);
                    relY = float(y - floorY);
                    alphaY = smoothstep(relY);
                }
                if (lastFloorY != floorY) {
                    const int xy00 = permute(x0 + floorY), xy01 = permute(x0 + floorY + 1);
                    const int xy10 = permute(x1 + floorY), xy11 = permute(x1 + floorY + 1);
                    auto grad = [&](int h) -> const Grad& { return kGrad[permute(h) & 15]; };
                    auto dotXz = [](const Grad& g, float a, float b) { return float(g.x) * a + float(g.z) * b; };
                    const Grad& g000 = grad(xy00 + floorZ);
                    d000xz = dotXz(g000, relX, relZ), g000y = float(g000.y);
                    const Grad& g100 = grad(xy10 + floorZ);
                    d100xz = dotXz(g100, relX - 1.0f, relZ), g100y = float(g100.y);
                    const Grad& g010 = grad(xy01 + floorZ);
                    d010xz = dotXz(g010, relX, relZ), g010y = float(g010.y);
                    const Grad& g110 = grad(xy11 + floorZ);
                    d110xz = dotXz(g110, relX - 1.0f, relZ), g110y = float(g110.y);
                    const Grad& g001 = grad(xy00 + floorZ + 1);
                    d001xz = dotXz(g001, relX, relZ - 1.0f), g001y = float(g001.y);
                    const Grad& g101 = grad(xy10 + floorZ + 1);
                    d101xz = dotXz(g101, relX - 1.0f, relZ - 1.0f), g101y = float(g101.y);
                    const Grad& g011 = grad(xy01 + floorZ + 1);
                    d011xz = dotXz(g011, relX, relZ - 1.0f), g011y = float(g011.y);
                    const Grad& g111 = grad(xy11 + floorZ + 1);
                    d111xz = dotXz(g111, relX - 1.0f, relZ - 1.0f), g111y = float(g111.y);
                    lastFloorY = floorY;
                }
                if constexpr (Smeared) relY = float(relativeY - computeFudgeY(originalY, relativeY, n.fudgeY));
                buf[index] += amplitude * lerp3(alphaX, alphaY, alphaZ, d000xz + g000y * relY, d100xz + g100y * relY,
                                                d010xz + g010y * (relY - 1.0f), d110xz + g110y * (relY - 1.0f),
                                                d001xz + g001y * relY, d101xz + g101y * relY,
                                                d011xz + g011y * (relY - 1.0f), d111xz + g111y * (relY - 1.0f));
                ++index;
            }
        }
    }
}

void NoiseSrc::addToVolume(float* buf, const Volume& v, double xzScale, double yScale, float amplitude) const {
    for (const Layer& l : layers) {  // NoiseStack.addToVolume
        if (l.smeared)
            perlinAddToVolume<true>(l, buf, v, xzScale * l.frequency, yScale * l.frequency, amplitude * l.amplitude);
        else
            perlinAddToVolume<false>(l, buf, v, xzScale * l.frequency, yScale * l.frequency, amplitude * l.amplitude);
    }
}

std::shared_ptr<NoiseSrc> wrapStack(std::shared_ptr<NoiseStack> stack, const std::vector<double>* fudge = nullptr) {
    auto src = std::make_shared<NoiseSrc>();
    for (size_t i = 0; i < stack->layers().size(); ++i) {
        const NoiseStack::Layer& l = stack->layers()[i];
        const auto* p = dynamic_cast<const PerlinNoise*>(l.noise.get());
        if (!p) continue;  // every stack the game builds here is Perlin-based
        src->layers.push_back({PermsAccess::of(*p), p->offsetX, p->offsetY, p->offsetZ, l.frequency, l.amplitude,
                               fudge ? (*fudge)[i] : 0.0, fudge != nullptr});
    }
    src->noise = std::move(stack);
    return src;
}

// SimplexNoise (2D only), used by end_outer_islands.
class Simplex {
public:
    Simplex(RandomSource& random, double offsetScale) {
        ox_ = random.nextDouble() * offsetScale;
        oy_ = random.nextDouble() * offsetScale;
        oz_ = random.nextDouble() * offsetScale;
        for (int i = 0; i < 256; ++i) p_[i] = u8(i);
        for (int i = 0; i < 256; ++i) std::swap(p_[i], p_[random.nextInt(256 - i) + i]);
    }
    float get(double xin0, double yin0) const {
        static const double kSqrt3 = std::sqrt(3.0);
        static const double kF2 = 0.5 * (kSqrt3 - 1.0);
        static const double kG2 = (3.0 - kSqrt3) / 6.0;
        const double xin = xin0 + ox_, yin = yin0 + oy_;
        const double s = (xin + yin) * kF2;
        const int i = jfloor(xin + s), j = jfloor(yin + s);
        const double t = double(i + j) * kG2;
        const double x0 = xin - (i - t), y0 = yin - (j - t);
        const int i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
        const double x1 = x0 - i1 + kG2, y1 = y0 - j1 + kG2;
        const double x2 = x0 - 1.0 + 2.0 * kG2, y2 = y0 - 1.0 + 2.0 * kG2;
        const int ii = i & 0xFF, jj = j & 0xFF;
        const int gi0 = perm(ii + perm(jj)) % 12;
        const int gi1 = perm(ii + i1 + perm(jj + j1)) % 12;
        const int gi2 = perm(ii + 1 + perm(jj + 1)) % 12;
        const double n0 = corner(gi0, x0, y0), n1 = corner(gi1, x1, y1), n2 = corner(gi2, x2, y2);
        return float(70.0 * (n0 + n1 + n2));
    }

private:
    int perm(int x) const { return p_[x & 0xFF]; }
    static double corner(int index, double x, double y) {
        double t0 = 0.5 - x * x - y * y - 0.0 * 0.0;
        if (t0 < 0.0) return 0.0;
        t0 *= t0;
        const Grad& g = kGrad[index];
        return t0 * t0 * (double(g.x) * x + double(g.y) * y + double(g.z) * 0.0);
    }
    double ox_, oy_, oz_;
    u8 p_[256];
};

}  // namespace
}  // namespace wg

// ---- Evaluation context (SamplerContext with caches enabled) ---------------------------------
namespace wg {

struct DensityContext::Impl {
    struct Cell {
        bool hasVolume = false;
        Volume volume;
        std::vector<float> buffer;
        i64 valueKey = 0;
        float value = kNaN;
    };
    struct Buf {
        std::unique_ptr<float[]> data;
        int capacity = 0;
        bool used = false;
    };
    const void* owner = nullptr;
    std::vector<std::unique_ptr<Cell>> cells;
    std::vector<Buf> pool;

    void reset() {
        for (auto& c : cells) c->hasVolume = false, c->valueKey = 0, c->value = kNaN;
    }
    Cell& cell(int id) {
        while (int(cells.size()) <= id) cells.push_back(std::make_unique<Cell>());
        return *cells[size_t(id)];
    }
    int acquire(int size) {
        int best = -1;
        for (int i = 0; i < int(pool.size()); ++i)
            if (!pool[size_t(i)].used && pool[size_t(i)].capacity >= size &&
                (best < 0 || pool[size_t(i)].capacity < pool[size_t(best)].capacity))
                best = i;
        if (best < 0) {
            const int cap = std::max(16, (size + 15) & ~15);
            pool.push_back({std::make_unique<float[]>(size_t(cap)), cap, false});
            best = int(pool.size()) - 1;
        }
        pool[size_t(best)].used = true;
        return best;
    }
    void release(int slot) { pool[size_t(slot)].used = false; }
    float* data(int slot) { return pool[size_t(slot)].data.get(); }

    void volumeCached(int id, const DensityFunction& input, float* out, const Volume& v) {
        Cell& c = cell(id);
        if (!c.hasVolume || !sameVolume(v, c.volume)) {
            c.hasVolume = true;
            c.volume = v;
            c.buffer.resize(size_t(v.size()));
            input.fill(*this, c.buffer.data(), v);
        }
        std::copy(c.buffer.begin(), c.buffer.begin() + v.size(), out);
    }
    float valueCached(int id, const DensityFunction& input, int x, int y, int z) {
        Cell& c = cell(id);
        const i64 key = packPos(x, y, z);
        if (c.valueKey == key && c.value == c.value) return c.value;
        if (c.hasVolume) {
            const int index = indexOfBlock(c.volume, x, y, z);
            if (index != -1) return c.buffer[size_t(index)];
        }
        const float value = input.value(*this, x, y, z);
        c.valueKey = key;
        c.value = value;
        return value;
    }
};

DensityContext::DensityContext() : impl(std::make_unique<Impl>()) {}
DensityContext::~DensityContext() = default;
void DensityContext::clear() { impl->reset(); }

namespace {

class Scratch {  // a pooled temporary buffer (ScopedDensityBuffer)
public:
    Scratch(Ctx& c, int size) : c_(c), slot_(c.acquire(size)), data_(c.data(slot_)) {}
    ~Scratch() { c_.release(slot_); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    float* data() const { return data_; }
    float& operator[](int i) const { return data_[i]; }

private:
    Ctx& c_;
    int slot_;
    float* data_;
};

// ---- Sampler nodes -----------------------------------------------------------------------------
class ConstS final : public DensityFunction {
public:
    explicit ConstS(float v) : v_(v) {}
    float value(Ctx&, int, int, int) const override { return v_; }
    void fill(Ctx&, float* out, const Volume& v) const override { std::fill(out, out + v.size(), v_); }

private:
    float v_;
};

class CachingS final : public DensityFunction {  // CachingDensitySampler
public:
    CachingS(int id, Fn in) : id_(id), in_(std::move(in)) {}
    float value(Ctx& c, int x, int y, int z) const override { return c.valueCached(id_, *in_, x, y, z); }
    void fill(Ctx& c, float* out, const Volume& v) const override { c.volumeCached(id_, *in_, out, v); }

private:
    int id_;
    Fn in_;
};

class NoiseS final : public DensityFunction {  // NoiseFunction.Sampler
public:
    NoiseS(std::shared_ptr<const NoiseSrc> n, double xz, double y) : n_(std::move(n)), xz_(xz), y_(y) {}
    float value(Ctx&, int x, int y, int z) const override { return n_->get(x * xz_, y * y_, z * xz_); }
    void fill(Ctx&, float* out, const Volume& v) const override {
        std::fill(out, out + v.size(), 0.0f);
        n_->addToVolume(out, v, xz_, y_, 1.0f);
    }

private:
    std::shared_ptr<const NoiseSrc> n_;
    double xz_, y_;
};

class ShiftedNoiseS final : public DensityFunction {  // NoiseFunction.ShiftedXzSampler / ShiftedXyzSampler
public:
    ShiftedNoiseS(Fn sx, Fn sy, Fn sz, std::shared_ptr<const NoiseSrc> n, double xz, double y)
        : sx_(std::move(sx)), sy_(std::move(sy)), sz_(std::move(sz)), n_(std::move(n)), xz_(xz), y_(y) {}
    float value(Ctx& c, int x, int y, int z) const override {
        const double nx = x * xz_ + sx_->value(c, x, y, z);
        const double ny = sy_ ? y * y_ + sy_->value(c, x, y, z) : y * y_;
        const double nz = z * xz_ + sz_->value(c, x, y, z);
        return n_->get(nx, ny, nz);
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        sx_->fill(c, out, v);
        Scratch syb(c, sy_ ? v.size() : 1);
        if (sy_) sy_->fill(c, syb.data(), v);
        Scratch szb(c, v.size());
        sz_->fill(c, szb.data(), v);
        int index = 0;
        for (int z = 0; z < v.sizeZ; ++z) {
            const double bz = v.blockZ(z) * xz_;
            for (int x = 0; x < v.sizeX; ++x) {
                const double bx = v.blockX(x) * xz_;
                for (int y = 0; y < v.sizeY; ++y, ++index) {
                    const double nx = bx + out[index];
                    const double ny = sy_ ? v.blockY(y) * y_ + syb[index] : v.blockY(y) * y_;
                    const double nz = bz + szb[index];
                    out[index] = n_->get(nx, ny, nz);
                }
            }
        }
    }

private:
    Fn sx_, sy_, sz_;
    std::shared_ptr<const NoiseSrc> n_;
    double xz_, y_;
};

class ShiftBS final : public DensityFunction {  // ShiftNoiseFunction.ShiftB
public:
    explicit ShiftBS(std::shared_ptr<const NoiseSrc> n) : n_(std::move(n)) {}
    float value(Ctx&, int x, int, int z) const override { return n_->get(z * 0.25, x * 0.25, 0.0) * 4.0f; }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        const Volume t{v.sizeZ, v.sizeX, 1, v.minZ, v.minX, 0, v.stepZ, v.stepX, 1};
        Scratch tb(c, t.size());
        std::fill(tb.data(), tb.data() + t.size(), 0.0f);
        n_->addToVolume(tb.data(), t, 0.25, 0.25, 4.0f);
        for (int z = 0; z < v.sizeZ; ++z)
            for (int x = 0; x < v.sizeX; ++x) {
                float* o = out + v.index(x, 0, z);
                std::fill(o, o + v.sizeY, tb[t.index(z, x, 0)]);
            }
    }

private:
    std::shared_ptr<const NoiseSrc> n_;
};

class ConstBinS final : public DensityFunction {  // BinaryFunction.Const*Sampler
public:
    enum Op { kAddC, kMulC, kCSub, kCDiv, kMinC, kMaxC };
    ConstBinS(Op op, Fn in, float c) : op_(op), in_(std::move(in)), c_(c) {}
    float value(Ctx& c, int x, int y, int z) const override {
        const float v = in_->value(c, x, y, z);
        switch (op_) {
            case kAddC: return v + c_;
            case kMulC: return v * c_;
            case kCSub: return c_ - v;
            case kCDiv: return c_ / v;
            case kMinC: return jmin(v, c_);
            case kMaxC: return jmax(v, c_);
        }
        return v;
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        in_->fill(c, out, v);
        const int n = v.size();
        const float k = c_;
        switch (op_) {
            case kAddC: for (int i = 0; i < n; ++i) out[i] = out[i] + k; break;
            case kMulC: for (int i = 0; i < n; ++i) out[i] = out[i] * k; break;
            case kCSub: for (int i = 0; i < n; ++i) out[i] = k - out[i]; break;
            case kCDiv: for (int i = 0; i < n; ++i) out[i] = k / out[i]; break;
            case kMinC: for (int i = 0; i < n; ++i) if (k < out[i]) out[i] = k; break;
            case kMaxC: for (int i = 0; i < n; ++i) if (k > out[i]) out[i] = k; break;
        }
    }

private:
    Op op_;
    Fn in_;
    float c_;
};

class BinS final : public DensityFunction {  // BinaryFunction.{Add,Sub,Mul,Div,Min,Max}Sampler
public:
    BinS(int op, Fn l, Fn r, float bound) : op_(op), l_(std::move(l)), r_(std::move(r)), bound_(bound) {}
    float value(Ctx& c, int x, int y, int z) const override {
        const float a = l_->value(c, x, y, z);
        switch (op_) {
            case kAdd: return a + r_->value(c, x, y, z);
            case kSub: return a - r_->value(c, x, y, z);
            case kMul: return a == 0.0f ? 0.0f : a * r_->value(c, x, y, z);
            case kDiv: return a == 0.0f ? 0.0f : a / r_->value(c, x, y, z);
            case kMin: return a <= bound_ ? a : jmin(a, r_->value(c, x, y, z));
            case kMax: return a >= bound_ ? a : jmax(a, r_->value(c, x, y, z));
        }
        return a;
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        l_->fill(c, out, v);
        Scratch rb(c, v.size());
        r_->fill(c, rb.data(), v);
        const float* r = rb.data();
        const int n = v.size();
        switch (op_) {
            case kAdd: for (int i = 0; i < n; ++i) out[i] = out[i] + r[i]; break;
            case kSub: for (int i = 0; i < n; ++i) out[i] = out[i] + -r[i]; break;
            case kMul: for (int i = 0; i < n; ++i) out[i] = out[i] * r[i]; break;
            case kDiv: for (int i = 0; i < n; ++i) out[i] = out[i] / r[i]; break;
            case kMin: for (int i = 0; i < n; ++i) if (r[i] < out[i]) out[i] = r[i]; break;
            case kMax: for (int i = 0; i < n; ++i) if (r[i] > out[i]) out[i] = r[i]; break;
        }
    }

private:
    int op_;
    Fn l_, r_;
    float bound_;  // MinSampler.rightMinValue / MaxSampler.rightMaxValue
};

inline float applyUnary(int op, float v) {
    switch (op) {
        case kAbs: return std::fabs(v);
        case kSquare: return v * v;
        case kCube: return v * v * v;
        case kSqrt: return jsqrt(v);
        case kHalfNeg: return v > 0.0f ? v : v * 0.5f;
        case kQuarterNeg: return v > 0.0f ? v : v * 0.25f;
        case kReciprocal: return 1.0f / v;
        case kNegate: return -v;
        case kSqueeze: {
            const float c = jclamp(v, -1.0f, 1.0f);
            return c / 2.0f - c * c * c / 24.0f;
        }
        case kLog: return float(std::log(double(v)));
        case kSign: return jsignum(v);
    }
    return v;
}

class UnaryS final : public DensityFunction {
public:
    UnaryS(int op, Fn in) : op_(op), in_(std::move(in)) {}
    float value(Ctx& c, int x, int y, int z) const override { return applyUnary(op_, in_->value(c, x, y, z)); }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        in_->fill(c, out, v);
        const int n = v.size();
        switch (op_) {
            case kAbs: for (int i = 0; i < n; ++i) out[i] = std::fabs(out[i]); break;
            case kSquare: for (int i = 0; i < n; ++i) out[i] = out[i] * out[i]; break;
            default: for (int i = 0; i < n; ++i) out[i] = applyUnary(op_, out[i]); break;
        }
    }

private:
    int op_;
    Fn in_;
};

class ClampS final : public DensityFunction {
public:
    ClampS(Fn in, float lo, float hi) : in_(std::move(in)), lo_(lo), hi_(hi) {}
    float value(Ctx& c, int x, int y, int z) const override { return jclamp(in_->value(c, x, y, z), lo_, hi_); }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        in_->fill(c, out, v);
        for (int i = 0, n = v.size(); i < n; ++i) out[i] = jclamp(out[i], lo_, hi_);
    }

private:
    Fn in_;
    float lo_, hi_;
};

inline float roundToInteger(float v, int type) {
    switch (type) {
        case kFloor: return float(std::floor(double(v)));
        case kRound: return float(jround(v));
        case kCeil: return float(std::ceil(double(v)));
        case kTruncate: return v > 0.0f ? float(std::floor(double(v))) : float(std::ceil(double(v)));
    }
    return v;
}

class RoundS final : public DensityFunction {  // RoundFunction.{IntegerMultiple,}Sampler (multiple null = 1)
public:
    RoundS(int type, Fn in, Fn multiple) : type_(type), in_(std::move(in)), mul_(std::move(multiple)) {}
    float value(Ctx& c, int x, int y, int z) const override {
        const float v = in_->value(c, x, y, z);
        if (!mul_) return roundToInteger(v, type_);
        return apply(v, mul_->value(c, x, y, z));
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        in_->fill(c, out, v);
        const int n = v.size();
        if (!mul_) {
            for (int i = 0; i < n; ++i) out[i] = roundToInteger(out[i], type_);
            return;
        }
        Scratch mb(c, n);
        mul_->fill(c, mb.data(), v);
        for (int i = 0; i < n; ++i) out[i] = apply(out[i], mb[i]);
    }

private:
    float apply(float v, float m) const { return m == 0.0f ? v : roundToInteger(v / m, type_) * m; }
    int type_;
    Fn in_, mul_;
};

class PowS final : public DensityFunction {  // PowFunction.{ConstBase,ConstExponent,}Sampler
public:
    enum Mode { kGeneral, kConstBase, kConstExp };
    PowS(Mode mode, Fn base, Fn exp, double k) : mode_(mode), base_(std::move(base)), exp_(std::move(exp)), k_(k) {}
    float value(Ctx& c, int x, int y, int z) const override {
        if (mode_ == kConstBase) return float(jpow(k_, double(exp_->value(c, x, y, z))));
        const float b = base_->value(c, x, y, z);
        if (mode_ == kConstExp) return float(jpow(double(b), k_));
        return float(jpow(double(b), double(exp_->value(c, x, y, z))));
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        const int n = v.size();
        if (mode_ == kConstBase) {
            exp_->fill(c, out, v);
            for (int i = 0; i < n; ++i) out[i] = float(jpow(k_, double(out[i])));
            return;
        }
        base_->fill(c, out, v);
        if (mode_ == kConstExp) {
            for (int i = 0; i < n; ++i) out[i] = float(jpow(double(out[i]), k_));
            return;
        }
        Scratch eb(c, n);
        exp_->fill(c, eb.data(), v);
        for (int i = 0; i < n; ++i) out[i] = float(jpow(double(out[i]), double(eb[i])));
    }

private:
    Mode mode_;
    Fn base_, exp_;
    double k_;
};

class LerpS final : public DensityFunction {  // LerpFunction.{ConstFirst,ConstSecond,}Sampler
public:
    // A null first/second uses the constant instead.
    LerpS(Fn alpha, Fn first, float cFirst, Fn second, float cSecond)
        : a_(std::move(alpha)), f_(std::move(first)), s_(std::move(second)), cf_(cFirst), cs_(cSecond) {}
    float value(Ctx& c, int x, int y, int z) const override {
        const float a = a_->value(c, x, y, z);
        if (a == 0.0f) return f_ ? f_->value(c, x, y, z) : cf_;
        if (a == 1.0f) return s_ ? s_->value(c, x, y, z) : cs_;
        const float first = f_ ? f_->value(c, x, y, z) : cf_;
        const float second = s_ ? s_->value(c, x, y, z) : cs_;
        return lerp(a, first, second);
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        const int n = v.size();
        a_->fill(c, out, v);
        Scratch fb(c, f_ ? n : 1);
        if (f_) f_->fill(c, fb.data(), v);
        Scratch sb(c, s_ ? n : 1);
        if (s_) s_->fill(c, sb.data(), v);
        for (int i = 0; i < n; ++i) {
            const float a = out[i];
            const float first = f_ ? fb[i] : cf_, second = s_ ? sb[i] : cs_;
            out[i] = a == 0.0f ? first : a == 1.0f ? second : lerp(a, first, second);
        }
    }

private:
    Fn a_, f_, s_;
    float cf_, cs_;
};

class RangeChoiceS final : public DensityFunction {
public:
    RangeChoiceS(Fn in, float lo, float hi, Fn inRange, Fn outRange)
        : in_(std::move(in)), lo_(lo), hi_(hi), a_(std::move(inRange)), b_(std::move(outRange)) {}
    float value(Ctx& c, int x, int y, int z) const override {
        const float v = in_->value(c, x, y, z);
        return v >= lo_ && v < hi_ ? a_->value(c, x, y, z) : b_->value(c, x, y, z);
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        const int n = v.size();
        a_->fill(c, out, v);
        Scratch ib(c, n);
        in_->fill(c, ib.data(), v);
        Scratch ob(c, n);
        b_->fill(c, ob.data(), v);
        for (int i = 0; i < n; ++i)
            if (!(ib[i] >= lo_) || !(ib[i] < hi_)) out[i] = ob[i];
    }

private:
    Fn in_;
    float lo_, hi_;
    Fn a_, b_;
};

class RangeChoiceConstS final : public DensityFunction {
public:
    RangeChoiceConstS(Fn in, float lo, float hi, float a, float b)
        : in_(std::move(in)), lo_(lo), hi_(hi), a_(a), b_(b) {}
    float value(Ctx& c, int x, int y, int z) const override { return choose(in_->value(c, x, y, z)); }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        in_->fill(c, out, v);
        for (int i = 0, n = v.size(); i < n; ++i) out[i] = choose(out[i]);
    }

private:
    float choose(float v) const { return v >= lo_ && v < hi_ ? a_ : b_; }
    Fn in_;
    float lo_, hi_, a_, b_;
};

class IntervalSelectS final : public DensityFunction {  // both IntervalSelectFunction samplers
public:
    IntervalSelectS(Fn in, std::vector<float> thresholds, std::vector<Fn> fns)
        : in_(std::move(in)), thr_(std::move(thresholds)), fns_(std::move(fns)) {}
    float value(Ctx& c, int x, int y, int z) const override {
        return fns_[select(in_->value(c, x, y, z))]->value(c, x, y, z);
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        const int n = v.size();
        in_->fill(c, out, v);
        std::array<int, 16> fixed;
        std::vector<int> dyn;
        int* slots = fns_.size() <= fixed.size() ? fixed.data() : (dyn.resize(fns_.size()), dyn.data());
        for (size_t k = 0; k < fns_.size(); ++k) {
            slots[k] = c.acquire(n);
            fns_[k]->fill(c, c.data(slots[k]), v);
        }
        for (int i = 0; i < n; ++i) out[i] = c.data(slots[select(out[i])])[i];
        for (size_t k = 0; k < fns_.size(); ++k) c.release(slots[k]);
    }

private:
    size_t select(float v) const {
        for (size_t i = 0; i < thr_.size(); ++i)
            if (v < thr_[i]) return i;
        return fns_.size() - 1;
    }
    Fn in_;
    std::vector<float> thr_;
    std::vector<Fn> fns_;
};

class GradientS final : public DensityFunction {
public:
    GradientS(int axis, int tiling, int from, int to, float fromValue, float toValue)
        : axis_(axis), tiling_(tiling), from_(from), lo_(std::min(from, to)), hi_(std::max(from, to)),
          range_(to - from), fromValue_(fromValue), factor_((toValue - fromValue) / float(to - from)) {}
    float value(Ctx&, int x, int y, int z) const override { return compute(axis_ == 0 ? x : axis_ == 1 ? y : z); }
    void fill(Ctx&, float* out, const Volume& v) const override {
        if (axis_ == 0) {
            for (int x = 0; x < v.sizeX; ++x) {
                const float value = compute(v.blockX(x));
                for (int z = 0; z < v.sizeZ; ++z) std::fill_n(out + v.index(x, 0, z), v.sizeY, value);
            }
        } else if (axis_ == 1) {
            for (int y = 0; y < v.sizeY; ++y) {
                const float value = compute(v.blockY(y));
                for (int z = 0; z < v.sizeZ; ++z)
                    for (int x = 0; x < v.sizeX; ++x) out[v.index(x, y, z)] = value;
            }
        } else {
            for (int z = 0; z < v.sizeZ; ++z)
                std::fill_n(out + v.index(0, 0, z), v.sizeX * v.sizeY, compute(v.blockZ(z)));
        }
    }

private:
    float compute(int coordinate) const {
        if (tiling_ == kClampToEdge) return fromValue_ + float(std::clamp(coordinate, lo_, hi_) - from_) * factor_;
        const int rel = i32(u32(coordinate) - u32(from_));
        if (tiling_ == kRepeat) return fromValue_ + float(floorMod(rel, range_)) * factor_;
        const int tile = floorDiv(rel, range_);
        const int local = i32(u32(rel) - u32(jmul32(tile, range_)));
        return (tile & 1) == 0 ? fromValue_ + float(local) * factor_ : fromValue_ + float(range_ - local) * factor_;
    }
    int axis_, tiling_, from_, lo_, hi_, range_;
    float fromValue_, factor_;
};

class DistanceS final : public DensityFunction {
public:
    DistanceS(int x, int y, int z, int metric) : x_(x), y_(y), z_(z), metric_(metric) {}
    float value(Ctx&, int x, int y, int z) const override {
        auto delta = [](int a, int b) { return float(i32(u32(a) - u32(b))); };  // int subtraction, then (float)
        const float dx = delta(x_, x), dy = delta(y_, y), dz = delta(z_, z);
        switch (metric_) {
            case kEuclidean: return jsqrt(dx * dx + dy * dy + dz * dz);
            case kEuclideanSquared: return dx * dx + dy * dy + dz * dz;
            case kManhattan: return std::fabs(dx) + std::fabs(dy) + std::fabs(dz);
            default: return jmax(jmax(std::fabs(dx), std::fabs(dy)), std::fabs(dz));
        }
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        int index = 0;
        for (int z = 0; z < v.sizeZ; ++z)
            for (int x = 0; x < v.sizeX; ++x)
                for (int y = 0; y < v.sizeY; ++y) out[index++] = value(c, v.blockX(x), v.blockY(y), v.blockZ(z));
    }

private:
    int x_, y_, z_, metric_;
};

class EndIslandS final : public DensityFunction {
public:
    explicit EndIslandS(std::shared_ptr<const Simplex> n) : n_(std::move(n)) {}
    float value(Ctx&, int x, int, int z) const override { return (heightValue(x / 8, z / 8) - 8.0f) / 128.0f; }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        for (int z = 0; z < v.sizeZ; ++z)
            for (int x = 0; x < v.sizeX; ++x)
                std::fill_n(out + v.index(x, 0, z), v.sizeY, value(c, v.blockX(x), 0, v.blockZ(z)));
    }

private:
    float heightValue(int sectionX, int sectionZ) const {
        const int chunkX = sectionX / 2, chunkZ = sectionZ / 2, subX = sectionX % 2, subZ = sectionZ % 2;
        float doffs = -100.0f;
        for (int xo = -12; xo <= 12; ++xo)
            for (int zo = -12; zo <= 12; ++zo) {
                const i64 tx = chunkX + xo, tz = chunkZ + zo;
                if (tx * tx + tz * tz > 4096 && n_->get(double(tx), double(tz)) < -0.9f) {
                    const float size =
                        std::fmod(std::fabs(float(tx)) * 3439.0f + std::fabs(float(tz)) * 147.0f, 13.0f) + 9.0f;
                    const float xd = float(subX - xo * 2), zd = float(subZ - zo * 2);
                    const float d = jclamp(100.0f - jsqrt(xd * xd + zd * zd) * size, -100.0f, 80.0f);
                    doffs = jmax(doffs, d);
                }
            }
        return doffs;
    }
    std::shared_ptr<const Simplex> n_;
};

class InterpolatedS final : public DensityFunction {  // InterpolatedFunction.Sampler
public:
    InterpolatedS(Fn in, int xz, int y)
        : in_(std::move(in)), xz_(xz), y_(y), xzInv_(1.0f / float(xz)), yInv_(1.0f / float(y)) {}
    float value(Ctx& c, int x, int y, int z) const override {
        const int xi = floorMod(x, xz_), yi = floorMod(y, y_), zi = floorMod(z, xz_);
        if (xi == 0 && yi == 0 && zi == 0) return in_->value(c, x, y, z);
        const Volume v{2, 2, 2, x - xi, y - yi, z - zi, xz_, y_, xz_};
        Scratch b(c, 8);
        in_->fill(c, b.data(), v);
        return lerp3(float(xi) / float(xz_), float(yi) / float(y_), float(zi) / float(xz_), b[v.index(0, 0, 0)],
                     b[v.index(1, 0, 0)], b[v.index(0, 1, 0)], b[v.index(1, 1, 0)], b[v.index(0, 0, 1)],
                     b[v.index(1, 0, 1)], b[v.index(0, 1, 1)], b[v.index(1, 1, 1)]);
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        // Every point a cell corner (steps are whole cells): the interpolated value is the corner value.
        if ((v.stepX % xz_ == 0 || v.sizeX == 1) && (v.stepY % y_ == 0 || v.sizeY == 1) &&
            (v.stepZ % xz_ == 0 || v.sizeZ == 1) && floorMod(v.minX, xz_) == 0 && floorMod(v.minY, y_) == 0 &&
            floorMod(v.minZ, xz_) == 0) {
            in_->fill(c, out, v);
        } else if (v.stepX == 1 && v.stepY == 1 && v.stepZ == 1) {
            fillBlockStep(c, out, v);
        } else {
            const Volume bv{v.sizeX * v.stepX, v.sizeY * v.stepY, v.sizeZ * v.stepZ, v.minX, v.minY, v.minZ, 1, 1, 1};
            Scratch b(c, bv.size());
            fillBlockStep(c, b.data(), bv);
            for (int z = 0; z < v.sizeZ; ++z)
                for (int x = 0; x < v.sizeX; ++x)
                    for (int y = 0; y < v.sizeY; ++y)
                        out[v.index(x, y, z)] = b[bv.index(x * v.stepX, y * v.stepY, z * v.stepZ)];
        }
    }

private:
    void fillBlockStep(Ctx& c, float* out, const Volume& v) const {
        const int minCX = floorDiv(v.minX, xz_), minCY = floorDiv(v.minY, y_), minCZ = floorDiv(v.minZ, xz_);
        const int maxX = maxBlockX(v), maxY = maxBlockY(v), maxZ = maxBlockZ(v);
        const int countX = floorDiv(maxX, xz_) - minCX + 1, countY = floorDiv(maxY, y_) - minCY + 1,
                  countZ = floorDiv(maxZ, xz_) - minCZ + 1;
        const Volume cv{floorMod(maxX, xz_) == 0 ? countX : countX + 1, floorMod(maxY, y_) == 0 ? countY : countY + 1,
                        floorMod(maxZ, xz_) == 0 ? countZ : countZ + 1, minCX * xz_, minCY * y_, minCZ * xz_, xz_,
                        y_, xz_};
        Scratch cb(c, cv.size());
        in_->fill(c, cb.data(), cv);
        for (int cz = 0; cz < countZ; ++cz) {
            const int nz = std::min(cz + 1, cv.sizeZ - 1);
            for (int cx = 0; cx < countX; ++cx) {
                const int nx = std::min(cx + 1, cv.sizeX - 1);
                float v000 = cb[cv.index(cx, 0, cz)], v100 = cb[cv.index(nx, 0, cz)];
                float v001 = cb[cv.index(cx, 0, nz)], v101 = cb[cv.index(nx, 0, nz)];
                for (int cy = 0; cy < countY; ++cy) {
                    const int ny = std::min(cy + 1, cv.sizeY - 1);
                    const float v010 = cb[cv.index(cx, ny, cz)], v110 = cb[cv.index(nx, ny, cz)];
                    const float v011 = cb[cv.index(cx, ny, nz)], v111 = cb[cv.index(nx, ny, nz)];
                    fillCell(out, v, cv, cx, cy, cz, v000, v100, v010, v110, v001, v101, v011, v111);
                    v000 = v010, v100 = v110, v001 = v011, v101 = v111;
                }
            }
        }
    }
    void fillCell(float* out, const Volume& ov, const Volume& cv, int cellX, int cellY, int cellZ, float v000,
                  float v100, float v010, float v110, float v001, float v101, float v011, float v111) const {
        const int ox = cv.blockX(cellX) - ov.minX, oy = cv.blockY(cellY) - ov.minY, oz = cv.blockZ(cellZ) - ov.minZ;
        const int x0 = std::max(0, -ox), y0 = std::max(0, -oy), z0 = std::max(0, -oz);
        const int x1 = std::min(xz_, ov.sizeX - ox) - 1, y1 = std::min(y_, ov.sizeY - oy) - 1,
                  z1 = std::min(xz_, ov.sizeZ - oz) - 1;
        for (int z = z0; z <= z1; ++z) {
            const float az = float(z) * xzInv_;
            const float v00 = lerp(az, v000, v001), v01 = lerp(az, v010, v011);
            const float v10 = lerp(az, v100, v101), v11 = lerp(az, v110, v111);
            for (int x = x0; x <= x1; ++x) {
                const float ax = float(x) * xzInv_;
                const float lo = lerp(ax, v00, v10), hi = lerp(ax, v01, v11);
                const float step = (hi - lo) * yInv_;
                float value = lo + step * float(y0);
                float* o = out + ov.index(ox + x, oy + y0, oz + z);
                for (int y = y0; y <= y1; ++y) {
                    *o++ = value;
                    value += step;
                }
            }
        }
    }
    Fn in_;
    int xz_, y_;
    float xzInv_, yInv_;
};

class SliceS final : public DensityFunction {  // SliceFunction.{X,Y,Z,Xz}Sampler
public:
    enum Mode { kX, kY, kZ, kXz };
    SliceS(Mode mode, Fn in, int a, int b) : mode_(mode), in_(std::move(in)), a_(a), b_(b) {}
    float value(Ctx& c, int x, int y, int z) const override {
        switch (mode_) {
            case kX: return in_->value(c, a_, y, z);
            case kY: return in_->value(c, x, a_, z);
            case kZ: return in_->value(c, x, y, a_);
            case kXz: return in_->value(c, a_, y, b_);
        }
        return 0.0f;
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        switch (mode_) {
            case kX: {
                if (v.sizeX == 1 && v.minX == a_) return in_->fill(c, out, v);
                const Volume iv{1, v.sizeY, v.sizeZ, a_, v.minY, v.minZ, v.stepX, v.stepY, v.stepZ};
                Scratch b(c, iv.size());
                in_->fill(c, b.data(), iv);
                int index = 0;
                for (int z = 0; z < v.sizeZ; ++z)
                    for (int x = 0; x < v.sizeX; ++x)
                        for (int y = 0; y < v.sizeY; ++y) out[index++] = b[iv.index(0, y, z)];
                return;
            }
            case kY: {
                if (v.sizeY == 1 && v.minY == a_) return in_->fill(c, out, v);
                const Volume iv{v.sizeX, 1, v.sizeZ, v.minX, a_, v.minZ, v.stepX, v.stepY, v.stepZ};
                Scratch b(c, iv.size());
                in_->fill(c, b.data(), iv);
                for (int z = 0; z < v.sizeZ; ++z)
                    for (int x = 0; x < v.sizeX; ++x)
                        std::fill_n(out + v.index(x, 0, z), v.sizeY, b[iv.index(x, 0, z)]);
                return;
            }
            case kZ: {
                if (v.sizeZ == 1 && v.minZ == a_) return in_->fill(c, out, v);
                const Volume iv{v.sizeX, v.sizeY, 1, v.minX, v.minY, a_, v.stepX, v.stepY, v.stepZ};
                Scratch b(c, iv.size());
                in_->fill(c, b.data(), iv);
                int index = 0;
                for (int z = 0; z < v.sizeZ; ++z)
                    for (int x = 0; x < v.sizeX; ++x)
                        for (int y = 0; y < v.sizeY; ++y) out[index++] = b[iv.index(x, y, 0)];
                return;
            }
            case kXz: {
                if (v.sizeX == 1 && v.sizeZ == 1 && v.minX == a_ && v.minZ == b_) return in_->fill(c, out, v);
                const Volume iv{1, v.sizeY, 1, a_, v.minY, b_, v.stepX, v.stepY, v.stepZ};
                Scratch b(c, iv.size());
                in_->fill(c, b.data(), iv);
                for (int y = 0; y < v.sizeY; ++y) {
                    const float value = b[iv.index(0, y, 0)];
                    int index = v.index(0, y, 0);
                    for (int z = 0; z < v.sizeZ; ++z)
                        for (int x = 0; x < v.sizeX; ++x, index += v.sizeY) out[index] = value;
                }
                return;
            }
        }
    }

private:
    Mode mode_;
    Fn in_;
    int a_, b_;
};

class FindTopSurfaceS final : public DensityFunction {
public:
    FindTopSurfaceS(Fn density, Fn upper, int lower, int cell)
        : density_(std::move(density)), upper_(std::move(upper)), lower_(lower), cell_(cell) {}
    float value(Ctx& c, int x, int y, int z) const override {
        return float(find(c, x, z, upper_->value(c, x, y, z)));
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        if (v.sizeY != 1) {  // the game throws; only reachable through the Y slice it is wrapped in
            std::fill(out, out + v.size(), 0.0f);
            return;
        }
        upper_->fill(c, out, v);
        int index = 0;
        for (int z = 0; z < v.sizeZ; ++z)
            for (int x = 0; x < v.sizeX; ++x, ++index)
                out[index] = float(find(c, v.blockX(x), v.blockZ(z), out[index]));
    }

private:
    int find(Ctx& c, int x, int z, float upperBound) const {
        const int topY = jmul32(jfloor(double(upperBound / float(cell_))), cell_);
        if (topY <= lower_) return lower_;
        for (int y = topY; y >= lower_; y -= cell_)
            if (density_->value(c, x, y, z) > 0.0f) return y;
        return lower_;
    }
    Fn density_, upper_;
    int lower_, cell_;
};

class SplineS final : public DensityFunction {  // SplineFunction.Sampler
public:
    struct Node {
        int coord = -1;  // -1: constant
        float value = 0;
        std::vector<float> loc, der;
        std::vector<int> kids;
    };
    SplineS(std::vector<Node> nodes, std::vector<Fn> coords) : nodes_(std::move(nodes)), coords_(std::move(coords)) {}

    float value(Ctx& c, int x, int y, int z) const override {
        std::array<float, 32> fixed;
        std::vector<float> dyn;
        float* cache = coords_.size() <= fixed.size() ? fixed.data() : (dyn.resize(coords_.size()), dyn.data());
        std::fill(cache, cache + coords_.size(), kNaN);
        PointInput in{c, x, y, z, cache, coords_};
        return sample(0, in);
    }
    void fill(Ctx& c, float* out, const Volume& v) const override {
        std::array<int, 32> fixed;
        std::vector<int> dyn;
        int* slots = coords_.size() <= fixed.size() ? fixed.data() : (dyn.resize(coords_.size()), dyn.data());
        std::fill(slots, slots + coords_.size(), -1);
        BufferInput in{c, v, slots, coords_, 0};
        for (int i = 0, n = v.size(); i < n; ++i) {
            in.index = i;
            out[i] = sample(0, in);
        }
        for (size_t k = 0; k < coords_.size(); ++k)
            if (slots[k] >= 0) c.release(slots[k]);
    }

private:
    struct PointInput {
        Ctx& c;
        int x, y, z;
        float* cache;
        const std::vector<Fn>& coords;
        float get(int i) {
            if (cache[i] == cache[i]) return cache[i];
            return cache[i] = coords[size_t(i)]->value(c, x, y, z);
        }
    };
    struct BufferInput {
        Ctx& c;
        const Volume& v;
        int* slots;
        const std::vector<Fn>& coords;
        int index;
        float get(int i) {
            if (slots[i] < 0) {
                slots[i] = c.acquire(v.size());
                coords[size_t(i)]->fill(c, c.data(slots[i]), v);
            }
            return c.data(slots[i])[index];
        }
    };
    static float linearExtend(float input, const Node& n, float value, int index) {
        const float d = n.der[size_t(index)];
        return d == 0.0f ? value : value + d * (input - n.loc[size_t(index)]);
    }
    template <class In>
    float sample(int ni, In& in) const {
        const Node& n = nodes_[size_t(ni)];
        if (n.coord < 0) return n.value;
        const float input = in.get(n.coord);
        int from = 0, len = int(n.loc.size());  // Mth.binarySearch(0, n, i -> input < loc[i])
        while (len > 0) {
            const int half = len / 2, mid = from + half;
            if (input < n.loc[size_t(mid)])
                len = half;
            else
                from = mid + 1, len -= half + 1;
        }
        const int start = from - 1, last = int(n.loc.size()) - 1;
        if (start < 0) return linearExtend(input, n, sample(n.kids[0], in), 0);
        if (start == last) return linearExtend(input, n, sample(n.kids[size_t(last)], in), last);
        const float x1 = n.loc[size_t(start)], x2 = n.loc[size_t(start) + 1];
        const float t = (input - x1) / (x2 - x1);
        const float d1 = n.der[size_t(start)], d2 = n.der[size_t(start) + 1];
        const float y1 = sample(n.kids[size_t(start)], in), y2 = sample(n.kids[size_t(start) + 1], in);
        const float a = d1 * (x2 - x1) - (y2 - y1);
        const float b = -d2 * (x2 - x1) + (y2 - y1);
        return lerp(t, y1, y2) + t * (1.0f - t) * lerp(t, a, b);
    }
    std::vector<Node> nodes_;
    std::vector<Fn> coords_;
};

template <class T, class... A>
Fn make(A&&... a) {
    return std::make_shared<const T>(std::forward<A>(a)...);
}

// NormalNoise normalization factor (private in NormalNoise), for the legacy nether biome noises.
double javaSum(const std::vector<double>& values) {  // DoubleStream.sum: Kahan summation
    double sum = 0, comp = 0, simple = 0;
    for (double v : values) {
        const double y = v - comp, t = sum + y;
        comp = (t - sum) - y;
        sum = t;
        simple += v;
    }
    const double r = sum - comp;
    return (r != r && std::isinf(simple)) ? simple : r;
}
double normalizationFactor(const NormalNoise& p) {
    auto modifier = [&](int i) { return p.amplitudeModifiers.empty() ? 1.0 : p.amplitudeModifiers[size_t(i)]; };
    double amplitude = p.baseAmplitude;
    if (p.normalize != NormalNoise::Normalization::Disabled)
        amplitude = p.baseAmplitude * (std::pow(0.5, -(p.octaveCount - 1)) / (std::pow(0.5, -p.octaveCount) - 1.0));
    std::vector<double> amps;
    for (int i = 0; i < p.octaveCount; ++i) {
        if (modifier(i) != 0.0) amps.push_back(amplitude * modifier(i));
        amplitude *= 0.5;
    }
    std::vector<double> abs;
    for (double a : amps) abs.push_back(std::abs(a));
    const double target = javaSum(abs);
    double variance = 0;
    for (double a : abs) variance += (PerlinNoise::kStandardDeviation * a) * (PerlinNoise::kStandardDeviation * a);
    const double deviation = std::sqrt(variance);
    double factor = deviation == 0.0 ? 0.0 : (target * 0.3333333333333333) / (deviation * std::sqrt(2.0));
    if (p.normalize == NormalNoise::Normalization::Legacy && factor != 0.0) {
        int minOctave = INT32_MAX, maxOctave = INT32_MIN;
        for (int i = 0; i < p.octaveCount; ++i)
            if (modifier(i) != 0.0) minOctave = std::min(minOctave, i), maxOctave = std::max(maxOctave, i);
        factor = p.baseAmplitude * 0.5 * 0.3333333333333333 / (0.1 * (1.0 + 1.0 / double(maxOctave - minOctave + 1)));
    }
    return factor;
}

}  // namespace

// ---- NoiseState ---------------------------------------------------------------------------------
struct NoiseState::Impl {
    const DataPack& data;
    i64 seed;
    bool legacy;
    std::unique_ptr<PositionalRandomFactory> random;

    std::recursive_mutex mutex;  // guards everything below (the game's compileLock)
    std::unordered_map<std::string, std::unique_ptr<NormalNoise>> params;
    std::unordered_map<std::string, std::shared_ptr<NoiseSrc>> noises;
    std::unordered_map<std::string, DefPtr> registry;
    std::unordered_set<std::string> parsing, warned;
    std::unordered_map<std::string, u32> interned;
    std::unordered_map<u32, Fn> compiled;
    std::unordered_map<u32, DefPtr> preparedCaches;
    int nextCacheId = 0;

    Impl(const DataPack& d, i64 s, bool l) : data(d), seed(s), legacy(l) {
        if (legacy)
            random = LegacyRandom(seed).forkPositional();
        else
            random = XoroshiroRandom(seed).forkPositional();
    }

    void warnOnce(const std::string& what) {
        if (warned.insert(what).second) LOG("density: %s", what.c_str());
    }

    // ---- definitions ----
    DefPtr finish(Def&& d) {
        std::string key;
        auto put = [&key](const void* p, size_t n) { key.append(static_cast<const char*>(p), n); };
        auto putStr = [&](const std::string& s) {
            const u32 n = u32(s.size());
            put(&n, 4);
            key += s;
        };
        put(&d.kind, 1);
        put(&d.op, sizeof d.op);
        put(d.i.data(), sizeof d.i);
        put(d.f.data(), sizeof d.f);
        put(d.d.data(), sizeof d.d);
        putStr(d.str);
        const u32 nt = u32(d.floats.size());
        put(&nt, 4);
        put(d.floats.data(), d.floats.size() * sizeof(float));
        if (d.kind != K::Ref)
            for (const DefPtr& k : d.kids) put(&k->sid, 4);
        std::function<void(const SplineDef&)> putSpline = [&](const SplineDef& s) {
            const u8 isConst = s.coord ? 0 : 1;
            put(&isConst, 1);
            if (isConst) return put(&s.value, 4);
            put(&s.coord->sid, 4);
            const u32 n = u32(s.loc.size());
            put(&n, 4);
            put(s.loc.data(), n * sizeof(float));
            put(s.der.data(), n * sizeof(float));
            for (const SplinePtr& v : s.vals) putSpline(*v);
        };
        if (d.spline) putSpline(*d.spline);
        auto it = interned.emplace(std::move(key), u32(interned.size() + 1)).first;
        d.sid = it->second;
        return std::make_shared<const Def>(std::move(d));
    }
    DefPtr constant(float v) {
        Def d;
        d.kind = K::Const;
        d.f[0] = v;
        return finish(std::move(d));
    }

    DefPtr registryEntry(const std::string& id) {
        if (auto it = registry.find(id); it != registry.end()) return it->second;
        const nlohmann::json* j = data.worldgen("density_function", id);
        if (!j || parsing.count(id)) {
            warnOnce(j ? "reference cycle through minecraft:" + id : "missing density function minecraft:" + id);
            return registry[id] = constant(0.0f);
        }
        parsing.insert(id);
        DefPtr d = parseDef(*j);
        parsing.erase(id);
        return registry[id] = d;
    }

    DefPtr parseDef(const nlohmann::json& j) {
        using nlohmann::json;
        if (j.is_number()) return constant(float(j.get<double>()));
        if (j.is_string()) {
            Def d;
            d.kind = K::Ref;
            d.str = DataPack::normalize(j.get<std::string>());
            d.kids.push_back(registryEntry(d.str));
            return finish(std::move(d));
        }
        if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
            warnOnce("malformed density function " + j.dump().substr(0, 80));
            return constant(0.0f);
        }
        const std::string type = DataPack::normalize(j["type"].get<std::string>());
        auto num = [&](const char* k, double def = 0.0) {
            auto it = j.find(k);
            return it != j.end() && it->is_number() ? it->get<double>() : def;
        };
        auto fnum = [&](const char* k, double def = 0.0) { return float(num(k, def)); };
        auto inum = [&](const char* k, int def = 0) { return int(num(k, def)); };
        auto kid = [&](const char* k) {
            auto it = j.find(k);
            return it != j.end() ? parseDef(*it) : constant(0.0f);
        };
        auto axisOf = [&](const char* k) {
            const std::string a = j.value(k, std::string("y"));
            return a == "x" ? 0 : a == "z" ? 2 : 1;
        };
        auto noiseKey = [&]() { return DataPack::normalize(j.value("noise", std::string())); };

        static const std::unordered_map<std::string, int> unary = {
            {"abs", kAbs},         {"square", kSquare},         {"cube", kCube},         {"sqrt", kSqrt},
            {"half_negative", kHalfNeg}, {"quarter_negative", kQuarterNeg}, {"reciprocal", kReciprocal},
            {"negate", kNegate},   {"squeeze", kSqueeze},       {"log", kLog},           {"sign", kSign}};
        static const std::unordered_map<std::string, int> binary = {{"add", kAdd}, {"sub", kSub}, {"mul", kMul},
                                                                    {"div", kDiv}, {"min", kMin}, {"max", kMax}};
        static const std::unordered_map<std::string, int> rounding = {
            {"floor", kFloor}, {"round", kRound}, {"ceil", kCeil}, {"truncate", kTruncate}};

        Def d;
        if (type == "constant") return constant(fnum("value"));
        if (auto it = unary.find(type); it != unary.end()) {
            d.kind = K::Unary, d.op = it->second, d.kids = {kid("input")};
        } else if (auto it2 = binary.find(type); it2 != binary.end()) {
            d.kind = K::Binary, d.op = it2->second, d.kids = {kid("left"), kid("right")};
        } else if (auto it3 = rounding.find(type); it3 != rounding.end()) {
            d.kind = K::Round, d.op = it3->second;
            d.kids = {kid("input"), j.contains("multiple") ? kid("multiple") : constant(1.0f)};
        } else if (type == "noise") {
            d.kind = K::Noise, d.str = noiseKey(), d.d[0] = num("xz_scale"), d.d[1] = num("y_scale");
            d.kids = {kid("shift_x"), kid("shift_y"), kid("shift_z")};
        } else if (type == "shift_a" || type == "shift_b" || type == "shift") {
            d.kind = type == "shift_a" ? K::ShiftA : type == "shift_b" ? K::ShiftB : K::Shift, d.str = noiseKey();
        } else if (type == "end_outer_islands") {
            d.kind = K::EndIslands;
        } else if (type == "distance_to_point") {
            d.kind = K::Distance;
            if (auto it = j.find("point"); it != j.end() && it->is_array() && it->size() == 3)
                for (int k = 0; k < 3; ++k) d.i[size_t(k)] = (*it)[size_t(k)].get<int>();
            const std::string m = j.value("metric", std::string("euclidean"));
            d.op = m == "euclidean_squared" ? kEuclideanSquared : m == "manhattan" ? kManhattan
                   : m == "chebyshev"       ? kChebyshev
                                            : kEuclidean;
        } else if (type == "gradient") {
            d.kind = K::Gradient, d.i = {axisOf("axis"), 0, inum("from_coordinate"), inum("to_coordinate")};
            const std::string t = j.value("tiling", std::string("clamp_to_edge"));
            d.i[1] = t == "repeat" ? kRepeat : t == "mirrored_repeat" ? kMirroredRepeat : kClampToEdge;
            d.f = {fnum("from_value"), fnum("to_value")};
            if (d.i[2] == d.i[3]) {
                warnOnce("gradient with from_coordinate == to_coordinate");
                return constant(0.0f);
            }
        } else if (type == "blend_alpha") {
            d.kind = K::BlendAlpha;
        } else if (type == "blend_offset") {
            d.kind = K::BlendOffset;
        } else if (type == "beardifier") {
            d.kind = K::Beardifier;
        } else if (type == "pow") {
            d.kind = K::Pow, d.kids = {kid("base"), kid("exponent")};
        } else if (type == "spline") {
            d.kind = K::Spline;
            auto it = j.find("spline");
            d.spline = parseSpline(it != j.end() ? *it : json(0.0));
        } else if (type == "lerp") {
            d.kind = K::Lerp, d.kids = {kid("alpha"), kid("first"), kid("second")};
        } else if (type == "clamp") {
            d.kind = K::Clamp, d.kids = {kid("input")}, d.f = {fnum("min"), fnum("max")};
        } else if (type == "range_choice") {
            d.kind = K::RangeChoice, d.f = {fnum("min_inclusive"), fnum("max_exclusive")};
            d.kids = {kid("input"), kid("when_in_range"), kid("when_out_of_range")};
        } else if (type == "interval_select") {
            d.kind = K::IntervalSelect, d.kids = {kid("input")};
            if (auto it = j.find("thresholds"); it != j.end() && it->is_array())
                for (const auto& t : *it) d.floats.push_back(float(t.get<double>()));
            if (auto it = j.find("functions"); it != j.end() && it->is_array())
                for (const auto& f : *it) d.kids.push_back(parseDef(f));
            if (d.kids.size() < 3 || d.floats.size() != d.kids.size() - 2) {
                warnOnce("interval_select with mismatched thresholds/functions");
                return constant(0.0f);
            }
        } else if (type == "cache") {
            d.kind = K::Cache, d.kids = {kid("input")};
        } else if (type == "blend_density") {
            d.kind = K::BlendDensity, d.kids = {kid("input")};
        } else if (type == "interpolated") {
            d.kind = K::Interpolated, d.kids = {kid("input")};
            d.i[0] = std::max(1, inum("cell_size_xz", 1)), d.i[1] = std::max(1, inum("cell_size_y", 1));
        } else if (type == "slice") {
            d.kind = K::Slice, d.i[0] = axisOf("axis"), d.i[1] = inum("coordinate"), d.kids = {kid("input")};
        } else if (type == "find_top_surface") {
            d.kind = K::FindTopSurface, d.kids = {kid("density"), kid("upper_bound")};
            d.i[0] = inum("lower_bound"), d.i[1] = std::max(1, inum("cell_height", 1));
        } else if (type == "old_blended_noise") {
            d.kind = K::OldBlendedNoise;
            d.d = {num("xz_scale"), num("y_scale"), num("xz_factor"), num("y_factor"), num("smear_scale_multiplier")};
        } else {
            warnOnce("unknown density function type minecraft:" + type + " (evaluates to 0)");
            return constant(0.0f);
        }
        return finish(std::move(d));
    }

    SplinePtr parseSpline(const nlohmann::json& j) {
        auto s = std::make_shared<SplineDef>();
        if (j.is_number()) {
            s->value = float(j.get<double>());
            return s;
        }
        auto it = j.find("points");
        if (!j.is_object() || !j.contains("coordinate") || it == j.end() || !it->is_array() || it->empty()) {
            warnOnce("malformed spline");
            return s;
        }
        s->coord = parseDef(j["coordinate"]);
        for (const auto& p : *it) {
            s->loc.push_back(float(p.value("location", 0.0)));
            s->der.push_back(float(p.value("derivative", 0.0)));
            s->vals.push_back(parseSpline(p.contains("value") ? p["value"] : nlohmann::json(0.0)));
        }
        return s;
    }

    // ---- range() and domainAxes() ----
    const NormalNoise* noiseParams(const std::string& id) {
        auto it = params.find(id);
        if (it != params.end()) return it->second.get();
        const nlohmann::json* j = data.worldgen("noise", id);
        if (!j) warnOnce("missing noise minecraft:" + id);
        auto& slot = params[id];
        if (j) slot = std::make_unique<NormalNoise>(NormalNoise::fromJson(*j));
        return slot.get();
    }
    Range noiseRange(const std::string& id) {
        const NormalNoise* p = noiseParams(id);
        return p ? Range{p->range().min, p->range().max} : rExact(0.0f);
    }
    int axesOf(const Def& d) {
        if (d.axesMemo >= 0) return d.axesMemo;
        int a = 0;
        switch (d.kind) {
            case K::Const: a = 0; break;
            case K::Noise:
                a = 7;
                if (d.d[1] == 0.0) a &= ~kAxisY;
                if (d.d[0] == 0.0) a &= ~(kAxisX | kAxisZ);
                for (const DefPtr& k : d.kids) a |= axesOf(*k);
                break;
            case K::ShiftA: case K::ShiftB: case K::EndIslands: case K::BlendAlpha: case K::BlendOffset: a = 5; break;
            case K::Shift: case K::Distance: case K::Beardifier: case K::OldBlendedNoise: a = 7; break;
            case K::Gradient: a = 1 << d.i[0]; break;
            case K::Spline: {
                std::function<void(const SplineDef&)> visit = [&](const SplineDef& s) {
                    if (!s.coord) return;
                    a |= axesOf(*s.coord);
                    for (const SplinePtr& v : s.vals) visit(*v);
                };
                visit(*d.spline);
                break;
            }
            case K::Slice: a = axesOf(*d.kids[0]) & ~(1 << d.i[0]); break;
            case K::FindTopSurface: a = (axesOf(*d.kids[0]) | axesOf(*d.kids[1])) & ~kAxisY; break;
            case K::Prepared: a = d.preparedAxes; break;
            default:
                for (const DefPtr& k : d.kids) a |= axesOf(*k);
                break;
        }
        return d.axesMemo = a;
    }
    Range splineRange(const SplineDef& s) {
        if (!s.coord) return rExact(s.value);
        const int last = int(s.loc.size()) - 1;
        float mn = kInf, mx = -kInf;
        const Range in = rangeOf(*s.coord);
        if (in.nai()) return in;
        auto extend = [&](float input, float value, int index) {
            const float der = s.der[size_t(index)];
            return der == 0.0f ? value : value + der * (input - s.loc[size_t(index)]);
        };
        if (in.min < s.loc[0]) {
            const Range r = splineRange(*s.vals[0]);
            const float e1 = extend(in.min, r.min, 0), e2 = extend(in.min, r.max, 0);
            mn = jmin(mn, jmin(e1, e2)), mx = jmax(mx, jmax(e1, e2));
        }
        if (in.max > s.loc[size_t(last)]) {
            const Range r = splineRange(*s.vals[size_t(last)]);
            const float e1 = extend(in.max, r.min, last), e2 = extend(in.max, r.max, last);
            mn = jmin(mn, jmin(e1, e2)), mx = jmax(mx, jmax(e1, e2));
        }
        std::vector<Range> vr;
        for (const SplinePtr& v : s.vals) vr.push_back(splineRange(*v));
        for (const Range& r : vr) mn = jmin(mn, r.min), mx = jmax(mx, r.max);
        for (int i = 0; i < last; ++i) {
            const float xDiff = s.loc[size_t(i) + 1] - s.loc[size_t(i)];
            const Range r1 = vr[size_t(i)], r2 = vr[size_t(i) + 1];
            const float d1 = s.der[size_t(i)], d2 = s.der[size_t(i) + 1];
            if (d1 != 0.0f || d2 != 0.0f) {
                const float p1 = d1 * xDiff, p2 = d2 * xDiff;
                const float minLerp1 = jmin(r1.min, r2.min), maxLerp1 = jmax(r1.max, r2.max);
                const float minA = p1 - r2.max + r1.min, maxA = p1 - r2.min + r1.max;
                const float minB = -p2 + r2.min - r1.max, maxB = -p2 + r2.max - r1.min;
                mn = jmin(mn, minLerp1 + 0.25f * jmin(minA, minB));
                mx = jmax(mx, maxLerp1 + 0.25f * jmax(maxA, maxB));
            }
        }
        return {mn, mx};
    }
    Range rangeOf(const Def& d) {
        if (d.rangeDone) return d.rangeMemo;
        Range r = kNaI;
        auto kr = [&](size_t i) { return rangeOf(*d.kids[i]); };
        switch (d.kind) {
            case K::Const: r = rExact(d.f[0]); break;
            case K::Noise: r = noiseRange(d.str); break;
            case K::ShiftA: case K::ShiftB: case K::Shift: r = rMul(noiseRange(d.str), rExact(4.0f)); break;
            case K::EndIslands: r = {-0.84375f, 0.5625f}; break;
            case K::Distance: r = {0.0f, kInf}; break;
            case K::Gradient: r = rEnc(d.f[0], d.f[1]); break;
            case K::BlendAlpha: r = {0.0f, 1.0f}; break;
            case K::BlendOffset: case K::Beardifier: r = kInfinite; break;
            case K::Unary: {
                const Range in = kr(0);
                switch (d.op) {
                    case kAbs: r = rAbs(in); break;
                    case kSquare: r = rSquare(in); break;
                    case kSqrt: r = rPow(in, rExact(0.5f)); break;
                    case kReciprocal: r = rReciprocal(in); break;
                    case kNegate: r = rSub(rExact(0.0f), in); break;
                    case kLog: r = rLog(in); break;
                    case kSign: r = rSign(in); break;
                    default: r = rMapMonotonic(in, [&](float v) { return applyUnary(d.op, v); }); break;
                }
                break;
            }
            case K::Round: {
                const Range m = kr(1);
                r = rMul(rMapMonotonic(rDiv(kr(0), m), [&](float v) { return roundToInteger(v, d.op); }), m);
                break;
            }
            case K::Binary: {
                const Range a = kr(0), b = kr(1);
                switch (d.op) {
                    case kAdd: r = rAdd(a, b); break;
                    case kSub: r = rSub(a, b); break;
                    case kMul: r = rMul(a, b); break;
                    case kDiv: r = rDiv(a, b); break;
                    case kMin: r = rMin(a, b); break;
                    case kMax: r = rMax(a, b); break;
                }
                break;
            }
            case K::Pow: r = rPow(kr(0), kr(1)); break;
            case K::Spline: r = splineRange(*d.spline); break;
            case K::Lerp: r = rLerp(kr(0), kr(1), kr(2)); break;
            case K::Clamp: r = rClamp(kr(0), d.f[0], d.f[1]); break;
            case K::RangeChoice: r = rEncList({kr(1), kr(2)}); break;
            case K::IntervalSelect: {
                std::vector<Range> list;
                for (size_t i = 1; i < d.kids.size(); ++i) list.push_back(kr(i));
                r = rEncVec(list);
                break;
            }
            case K::FindTopSurface: r = {float(d.i[0]), jmax(float(d.i[0]), kr(1).max)}; break;
            case K::OldBlendedNoise: {
                const double smear = 684.412 * d.d[1] * d.d[4];
                double factor = 1.0, valueFactor = double(0.99998474f) / (std::pow(2.0, 16) - 1.0);
                r = rExact(0.0f);
                for (int i = 15; i >= 0; --i) {
                    r = rAdd(r, rMul(rSym(float(std::abs(smear * factor) + 2.0)), rExact(float(valueFactor))));
                    factor /= 2.0;
                    valueFactor *= 2.0;
                }
                break;
            }
            case K::Prepared: r = d.preparedRange; break;
            default: r = kr(0); break;  // Ref, Cache, BlendDensity, Interpolated, Slice
        }
        d.rangeMemo = r;
        d.rangeDone = true;
        return r;
    }

    // ---- DensityFunctionCompiler rewrite rules ----
    using Rule = std::function<DefPtr(const DefPtr&)>;
    DefPtr rewriteChildren(const DefPtr& f, const Rule& rule) {
        switch (f->kind) {
            case K::Ref: {
                DefPtr n = rule(f->kids[0]);
                return n == f->kids[0] ? f : n;
            }
            case K::Spline: {
                bool changed = false;
                std::function<SplinePtr(const SplinePtr&)> map = [&](const SplinePtr& s) -> SplinePtr {
                    if (!s->coord) return s;
                    auto n = std::make_shared<SplineDef>(*s);
                    n->coord = rule(s->coord);
                    if (n->coord != s->coord) changed = true;
                    for (SplinePtr& v : n->vals) v = map(v);
                    return n;
                };
                SplinePtr ns = map(f->spline);
                if (!changed) return f;
                Def d = *f;
                d.spline = ns;
                d.axesMemo = -1, d.rangeDone = false;
                return finish(std::move(d));
            }
            default: {
                if (f->kids.empty() || f->kind == K::Prepared) return f;
                std::vector<DefPtr> kids;
                bool changed = false;
                for (const DefPtr& k : f->kids) {
                    kids.push_back(rule(k));
                    changed |= kids.back() != k;
                }
                if (!changed) return f;
                Def d = *f;
                d.kids = std::move(kids);
                d.axesMemo = -1, d.rangeDone = false;
                return finish(std::move(d));
            }
        }
    }
    DefPtr inlineAndCache(const DefPtr& f) {  // INLINE_REFERENCE + cache deduplication
        const DefPtr g = f->kind == K::Ref ? f->kids[0] : f;
        if (g->kind == K::Cache) return prepareCache(g);
        return rewriteChildren(g, [this](const DefPtr& c) { return inlineAndCache(c); });
    }
    DefPtr sliceUniformAxes(const DefPtr& f, int parentAxes) {
        if (f->kind == K::Const || f->kind == K::Gradient) return f;
        const int axes = axesOf(*f);
        if (axes == parentAxes)
            return rewriteChildren(f, [this, parentAxes](const DefPtr& c) { return sliceUniformAxes(c, parentAxes); });
        DefPtr n = rewriteChildren(f, [this, axes](const DefPtr& c) { return sliceUniformAxes(c, axes); });
        int existing = 0;
        for (const Def* s = n.get(); s->kind == K::Slice; s = s->kids[0].get()) existing |= 1 << s->i[0];
        const int removed = parentAxes & ~axes & ~existing;
        for (int axis : {0, 2, 1}) {
            if (!(removed & (1 << axis))) continue;
            Def d;
            d.kind = K::Slice, d.i[0] = axis, d.i[1] = 0, d.kids = {n};
            n = finish(std::move(d));
        }
        return n;
    }
    DefPtr optimize(const DefPtr& f) { return sliceUniformAxes(inlineAndCache(f), 7); }
    DefPtr prepareCache(const DefPtr& cache) {
        const DefPtr& input = cache->kids[0];
        if (auto it = preparedCaches.find(input->sid); it != preparedCaches.end()) return it->second;
        const int id = nextCacheId++;
        const DefPtr optimized = optimize(input);
        Def d;
        d.kind = K::Prepared, d.i[0] = id;
        d.sampler = make<CachingS>(id, compile(optimized));
        d.preparedRange = rangeOf(*optimized);
        d.preparedAxes = axesOf(*optimized);
        DefPtr p = finish(std::move(d));
        preparedCaches[input->sid] = p;
        return p;
    }

    // ---- CompileContext ----
    std::shared_ptr<NoiseSrc> sharedNoise(const std::string& id) {  // RandomState.getOrCreateNoise
        if (auto it = noises.find(id); it != noises.end()) return it->second;
        const NormalNoise* p = noiseParams(id);
        auto rnd = random->fromHashOf(id.find(':') == std::string::npos ? "minecraft:" + id : id);  // data packs' own namespaces
        auto src = wrapStack(p ? p->create(*rnd) : std::make_shared<NoiseStack>());
        noises[id] = src;
        return src;
    }
    std::shared_ptr<NoiseSrc> legacyNetherNoise(const NormalNoise& p, i64 seedOffset) {
        LegacyRandom rnd(seed + seedOffset);
        std::vector<double> amps = p.amplitudeModifiers;
        if (amps.empty()) amps.assign(size_t(p.octaveCount), 1.0);
        struct L {
            std::shared_ptr<PerlinNoise> noise;
            double frequency;
            float amplitude;
        };
        auto fbm = [&]() {  // LegacyFbmInitializer.createForLegacyNetherBiome
            const int octaves = int(amps.size()), zeroIndex = -p.baseOctave;
            std::vector<std::shared_ptr<PerlinNoise>> levels(static_cast<size_t>(octaves));
            auto zero = std::make_shared<PerlinNoise>(rnd);
            if (zeroIndex >= 0 && zeroIndex < octaves && amps[size_t(zeroIndex)] != 0.0)
                levels[size_t(zeroIndex)] = zero;
            for (int i = zeroIndex - 1; i >= 0; --i) {
                if (i < octaves && amps[size_t(i)] != 0.0)
                    levels[size_t(i)] = std::make_shared<PerlinNoise>(rnd);
                else
                    rnd.consumeCount(262);
            }
            std::vector<L> out;
            double factor = std::pow(2.0, -zeroIndex);
            double valueFactor = std::pow(2.0, octaves - 1) / (std::pow(2.0, octaves) - 1.0);
            for (int i = 0; i < octaves; ++i) {
                if (levels[size_t(i)]) out.push_back({levels[size_t(i)], factor, float(valueFactor * amps[size_t(i)])});
                factor *= 2.0;
                valueFactor /= 2.0;
            }
            return out;
        };
        const std::vector<L> first = fbm(), second = fbm();
        const float valueFactor = float(normalizationFactor(p) * p.baseAmplitude);
        auto stack = std::make_shared<NoiseStack>();
        for (const L& l : first) stack->add(l.noise, l.frequency * 1.0, l.amplitude * valueFactor);
        for (const L& l : second) stack->add(l.noise, l.frequency * 1.0181268882175227, l.amplitude * valueFactor);
        return wrapStack(stack);
    }
    std::shared_ptr<NoiseSrc> compileNoise(const std::string& id) {  // CompileContext.createNoiseSampler
        if (id == "nether/temperature" || id == "nether/vegetation") {
            if (const NormalNoise* p = noiseParams(id))
                return legacyNetherNoise(*p, id == "nether/temperature" ? 0 : 1);
        }
        return sharedNoise(id);
    }
    Fn compileBlendedNoise(const Def& d) {
        auto rnd = legacy ? std::unique_ptr<RandomSource>(new LegacyRandom(seed))
                          : random->fromHashOf("minecraft:terrain");
        const double xzMul = 684.412 * d.d[0], yMul = 684.412 * d.d[1];
        const double limitSmear = yMul * d.d[4], mainSmear = limitSmear / d.d[3];
        auto fbm = [&](int firstOctave, double smearScaleY, double valueFactor) {  // BlendedNoise.createFbm
            const int octaves = -firstOctave + 1;
            double factor = 1.0;
            valueFactor /= std::pow(2.0, octaves) - 1.0;
            auto stack = std::make_shared<NoiseStack>();
            std::vector<double> fudge;
            for (int i = octaves - 1; i >= 0; --i) {
                auto layer = std::make_shared<SmearedPerlinNoise>(*rnd, smearScaleY * factor);
                stack->add(layer, factor, float(valueFactor));
                fudge.push_back(smearScaleY * factor);
                factor /= 2.0;
                valueFactor *= 2.0;
            }
            return wrapStack(stack, &fudge);
        };
        auto minLimit = fbm(-15, limitSmear, double(0.99998474f));
        auto maxLimit = fbm(-15, limitSmear, double(0.99998474f));
        auto main = fbm(-7, mainSmear, 12.75);
        Fn mainNoise = make<NoiseS>(main, xzMul / d.d[2], yMul / d.d[3]);
        Fn choice = make<ClampS>(make<ConstBinS>(ConstBinS::kAddC, mainNoise, 0.5f), 0.0f, 1.0f);
        Fn minNoise = make<NoiseS>(minLimit, xzMul, yMul), maxNoise = make<NoiseS>(maxLimit, xzMul, yMul);
        return make<LerpS>(choice, minNoise, 0.0f, maxNoise, 0.0f);
    }
    Fn compileSpline(const SplineDef& root) {
        std::vector<SplineS::Node> nodes;
        std::vector<Fn> coords;
        std::unordered_map<u32, int> coordIndex;
        std::function<int(const SplineDef&)> add = [&](const SplineDef& s) {
            const int index = int(nodes.size());
            nodes.emplace_back();
            if (!s.coord) {
                nodes[size_t(index)].value = s.value;
                return index;
            }
            auto it = coordIndex.find(s.coord->sid);
            if (it == coordIndex.end()) {
                it = coordIndex.emplace(s.coord->sid, int(coords.size())).first;
                coords.push_back(compile(s.coord));
            }
            std::vector<int> kids;
            for (const SplinePtr& v : s.vals) kids.push_back(add(*v));
            SplineS::Node& n = nodes[size_t(index)];
            n.coord = it->second, n.loc = s.loc, n.der = s.der, n.kids = std::move(kids);
            return index;
        };
        if (!root.coord) return make<ConstS>(root.value);
        add(root);
        return make<SplineS>(std::move(nodes), std::move(coords));
    }

    Fn compile(const DefPtr& dp) {
        const Def& d = *dp;
        auto kc = [&](size_t i) { return compile(d.kids[i]); };
        auto isConst = [&](size_t i) { return d.kids[i]->kind == K::Const; };
        auto cv = [&](size_t i) { return d.kids[i]->f[0]; };
        switch (d.kind) {
            case K::Const: return make<ConstS>(d.f[0]);
            case K::Ref: return kc(0);
            case K::Noise: {
                auto n = compileNoise(d.str);
                auto zero = [&](size_t i) { return isConst(i) && isZeroBits(cv(i)); };
                if (zero(0) && zero(1) && zero(2)) return make<NoiseS>(n, d.d[0], d.d[1]);
                Fn sx = kc(0), sz = kc(2);
                Fn sy = zero(1) ? nullptr : kc(1);
                return make<ShiftedNoiseS>(sx, sy, sz, n, d.d[0], d.d[1]);
            }
            case K::ShiftA:
            case K::Shift: {
                Fn noise = make<NoiseS>(compileNoise(d.str), 0.25, d.kind == K::Shift ? 0.25 : 0.0);
                return make<ConstBinS>(ConstBinS::kMulC, noise, 4.0f);
            }
            case K::ShiftB: return make<ShiftBS>(compileNoise(d.str));
            case K::EndIslands: {
                LegacyRandom rnd(seed);
                rnd.consumeCount(17292);
                return make<EndIslandS>(std::make_shared<const Simplex>(rnd, 0.0));
            }
            case K::Distance: return make<DistanceS>(d.i[0], d.i[1], d.i[2], d.op);
            case K::Gradient: return make<GradientS>(d.i[0], d.i[1], d.i[2], d.i[3], d.f[0], d.f[1]);
            case K::BlendAlpha: return make<ConstS>(1.0f);  // no blender: the context-bound fallbacks
            case K::BlendOffset: return make<ConstS>(0.0f);
            case K::Beardifier: return make<ConstS>(0.0f);  // no structures yet
            case K::Unary: return make<UnaryS>(d.op, kc(0));
            case K::Round: {
                Fn in = kc(0);
                if (isConst(1) && cv(1) == 1.0f) return make<RoundS>(d.op, in, nullptr);
                return make<RoundS>(d.op, in, kc(1));
            }
            case K::Binary: {
                Fn l = kc(0), r = kc(1);
                switch (d.op) {
                    case kAdd:
                        if (isConst(0)) return make<ConstBinS>(ConstBinS::kAddC, r, cv(0));
                        if (isConst(1)) return make<ConstBinS>(ConstBinS::kAddC, l, cv(1));
                        return make<BinS>(kAdd, l, r, 0.0f);
                    case kSub:
                        if (isConst(0)) return make<ConstBinS>(ConstBinS::kCSub, r, cv(0));
                        if (isConst(1)) return make<ConstBinS>(ConstBinS::kAddC, l, -cv(1));
                        return make<BinS>(kSub, l, r, 0.0f);
                    case kMul:
                        if (isConst(0)) return make<ConstBinS>(ConstBinS::kMulC, r, cv(0));
                        if (isConst(1)) return make<ConstBinS>(ConstBinS::kMulC, l, cv(1));
                        return make<BinS>(kMul, l, r, 0.0f);
                    case kDiv:
                        if (isConst(0)) return make<ConstBinS>(ConstBinS::kCDiv, r, cv(0));
                        if (isConst(1)) return make<ConstBinS>(ConstBinS::kMulC, l, 1.0f / cv(1));
                        return make<BinS>(kDiv, l, r, 0.0f);
                    case kMin: {
                        const Range lr = rangeOf(*d.kids[0]), rr = rangeOf(*d.kids[1]);
                        if (lr.max < rr.min) return l;
                        if (rr.max < lr.min) return r;
                        if (isConst(0)) return make<ConstBinS>(ConstBinS::kMinC, r, cv(0));
                        if (isConst(1)) return make<ConstBinS>(ConstBinS::kMinC, l, cv(1));
                        return make<BinS>(kMin, l, r, rr.min);
                    }
                    case kMax: {
                        const Range lr = rangeOf(*d.kids[0]), rr = rangeOf(*d.kids[1]);
                        if (lr.min > rr.max) return l;
                        if (rr.min > lr.max) return r;
                        if (isConst(0)) return make<ConstBinS>(ConstBinS::kMaxC, r, cv(0));
                        if (isConst(1)) return make<ConstBinS>(ConstBinS::kMaxC, l, cv(1));
                        return make<BinS>(kMax, l, r, rr.max);
                    }
                }
                return l;
            }
            case K::Pow: {
                Fn base = kc(0), exp = kc(1);
                if (isConst(0)) return make<PowS>(PowS::kConstBase, nullptr, exp, double(cv(0)));
                if (!isConst(1)) return make<PowS>(PowS::kGeneral, base, exp, 0.0);
                const float e = cv(1), ae = std::fabs(e);
                Fn special;
                if (ae == 0.5f)
                    special = make<UnaryS>(kSqrt, base);
                else if (ae == 1.0f)
                    special = base;
                else if (ae == 2.0f)
                    special = make<UnaryS>(kSquare, base);
                else if (ae == 3.0f)
                    special = make<UnaryS>(kCube, base);
                else
                    return make<PowS>(PowS::kConstExp, base, nullptr, double(e));
                return e >= 0.0f ? special : make<UnaryS>(kReciprocal, special);
            }
            case K::Spline: return compileSpline(*d.spline);
            case K::Lerp: {
                Fn a = kc(0), first = kc(1), second = kc(2);
                if (isConst(1)) return make<LerpS>(a, nullptr, cv(1), second, 0.0f);
                if (isConst(2)) return make<LerpS>(a, first, 0.0f, nullptr, cv(2));
                return make<LerpS>(a, first, 0.0f, second, 0.0f);
            }
            case K::Clamp: return make<ClampS>(kc(0), d.f[0], d.f[1]);
            case K::RangeChoice: {
                Fn in = kc(0);
                if (isConst(1) && isConst(2)) return make<RangeChoiceConstS>(in, d.f[0], d.f[1], cv(1), cv(2));
                return make<RangeChoiceS>(in, d.f[0], d.f[1], kc(1), kc(2));
            }
            case K::IntervalSelect: {
                Fn in = kc(0);
                std::vector<Fn> fns;
                if (d.floats.size() == 1) {
                    fns = {kc(1), kc(d.kids.size() - 1)};
                } else {
                    for (size_t i = 1; i < d.kids.size(); ++i) fns.push_back(kc(i));
                }
                return make<IntervalSelectS>(in, d.floats, std::move(fns));
            }
            case K::Cache:  // only reachable if a cache escaped the rewrite (never with the game's data)
                warnOnce("uncompiled cache node");
                return kc(0);
            case K::BlendDensity: return kc(0);  // no blender: passes its input through
            case K::Interpolated: return make<InterpolatedS>(kc(0), d.i[0], d.i[1]);
            case K::Slice: {
                const Def& in = *d.kids[0];
                if (in.kind == K::Slice && ((d.i[0] == 0 && in.i[0] == 2) || (d.i[0] == 2 && in.i[0] == 0))) {
                    const int x = d.i[0] == 0 ? d.i[1] : in.i[1], z = d.i[0] == 0 ? in.i[1] : d.i[1];
                    return make<SliceS>(SliceS::kXz, compile(in.kids[0]), x, z);
                }
                const SliceS::Mode mode = d.i[0] == 0 ? SliceS::kX : d.i[0] == 1 ? SliceS::kY : SliceS::kZ;
                return make<SliceS>(mode, kc(0), d.i[1], 0);
            }
            case K::FindTopSurface:
                return make<SliceS>(SliceS::kY, make<FindTopSurfaceS>(kc(0), kc(1), d.i[0], d.i[1]), 0, 0);
            case K::OldBlendedNoise: return compileBlendedNoise(d);
            case K::Prepared: return d.sampler;
        }
        return make<ConstS>(0.0f);
    }

    Fn compileRoot(const DefPtr& root) {  // DensityFunctionCompiler.getSampler
        if (auto it = compiled.find(root->sid); it != compiled.end()) return it->second;
        Fn f = compile(optimize(root));
        compiled[root->sid] = f;
        return f;
    }
};

NoiseState::NoiseState(const DataPack& data, i64 seed, bool legacyRandomSource)
    : impl_(std::make_unique<Impl>(data, seed, legacyRandomSource)) {}
NoiseState::~NoiseState() = default;

DensityRef NoiseState::parse(const nlohmann::json& j) const {
    std::lock_guard lock(impl_->mutex);
    try {
        return impl_->compileRoot(impl_->parseDef(j));
    } catch (const std::exception& e) {
        LOG("density: failed to build function: %s", e.what());
        return make<ConstS>(0.0f);
    }
}

DensityRef NoiseState::function(std::string_view id) const { return parse(nlohmann::json(std::string(id))); }

std::shared_ptr<const Noise> NoiseState::noise(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    try {
        return impl_->sharedNoise(DataPack::normalize(id))->noise;
    } catch (const std::exception& e) {
        LOG("density: failed to create noise %.*s: %s", int(id.size()), id.data(), e.what());
        return std::make_shared<NoiseStack>();
    }
}

const PositionalRandomFactory& NoiseState::random() const { return *impl_->random; }

std::unique_ptr<PositionalRandomFactory> NoiseState::randomFactory(std::string_view name) const {
    std::string n(name);
    if (n.find(':') == std::string::npos) n = "minecraft:" + n;
    return impl_->random->fromHashOf(n)->forkPositional();
}

i64 NoiseState::seed() const { return impl_->seed; }
const DataPack& NoiseState::data() const { return impl_->data; }

float NoiseState::sample(const DensityRef& f, DensityContext& ctx, int x, int y, int z) const {
    Ctx& c = *ctx.impl;
    if (c.owner != impl_.get()) c.reset(), c.owner = impl_.get();
    return f ? f->value(c, x, y, z) : 0.0f;
}

void NoiseState::sampleVolume(const DensityRef& f, DensityContext& ctx, const Volume& volume, float* out) const {
    if (volume.sizeX <= 0 || volume.sizeY <= 0 || volume.sizeZ <= 0 || volume.stepX <= 0 || volume.stepY <= 0 ||
        volume.stepZ <= 0) {
        LOG("density: invalid volume %dx%dx%d step %d/%d/%d", volume.sizeX, volume.sizeY, volume.sizeZ, volume.stepX,
            volume.stepY, volume.stepZ);
        return;
    }
    Ctx& c = *ctx.impl;
    if (c.owner != impl_.get()) c.reset(), c.owner = impl_.get();
    if (f)
        f->fill(c, out, volume);
    else
        std::fill(out, out + volume.size(), 0.0f);
}

}  // namespace wg
