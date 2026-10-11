// Biome decoration: the game's ChunkGenerator.applyBiomeDecoration with FeatureSorter's global feature
// order, the WorldGenRegion the features write into, the WorldgenRandom decoration seeding, value/state
// providers, block predicates, rule tests, placed features with all placement modifiers (FeaturePlacer)
// and the small feature types (simple_block, block_column, disk, selectors, sequence, overlay...).
#include "worldgen/feature_api.h"

#include "assets/assets.h"
#include "worldgen/features_internal.h"
#include "worldgen/structures.h"

#include <algorithm>
#include <cstdlib>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace wg {

using json = nlohmann::json;

DecorationContext& decorationContext() {
    thread_local DecorationContext context;
    return context;
}

// ---- GenRegion --------------------------------------------------------------------------------

namespace {

// Final heightmap predicates (Heightmap.Types.isOpaque): WORLD_SURFACE, MOTION_BLOCKING,
// MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR.
bool heightmapOpaque(const BlockFacts::Impl& f, int type, u16 s) {
    switch (type) {
    case 0: return !f.has(s, fact::Air);
    case 1: return f.has(s, fact::MotionTag) || f.fluidType(s) != FluidNone;
    case 2: return f.has(s, fact::MotionNoLeaves) || f.fluidType(s) != FluidNone;
    default: return f.has(s, fact::MotionTag);
    }
}

// Heightmap.primeHeightmaps for the four final types.
void primeHeightmaps(GenChunk& c, const BlockFacts::Impl& f) {
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            const int col = z * 16 + x;
            int remaining = 4;
            bool done[4] = {};
            for (int t = 0; t < 4; ++t) c.finalHeightmaps[t][col] = c.minY;
            for (int y = c.minY + c.height - 1; y >= c.minY && remaining; --y) {
                const u16 s = c.get(x, y, z);
                if (s == GenChunk::kAir) continue;
                for (int t = 0; t < 4; ++t)
                    if (!done[t] && heightmapOpaque(f, t, s)) {
                        c.finalHeightmaps[t][col] = y + 1;
                        done[t] = true;
                        --remaining;
                    }
            }
        }
    c.finalHeightmapsPrimed = true;
}

// Heightmap.update
void updateHeightmap(GenChunk& c, const BlockFacts::Impl& f, int type, int lx, int y, int lz, u16 s) {
    int& first = c.finalHeightmaps[type][lz * 16 + lx];
    if (y <= first - 2) return;
    if (heightmapOpaque(f, type, s)) {
        if (y >= first) first = y + 1;
    } else if (first - 1 == y) {
        for (int yy = y - 1; yy >= c.minY; --yy)
            if (heightmapOpaque(f, type, c.get(lx, yy, lz))) {
                first = yy + 1;
                return;
            }
        first = c.minY;
    }
}

}  // namespace

GenRegion::GenRegion(const std::array<GenChunk*, 9>& chunks, const BlockFacts& facts, const BiomeZoom& zoom,
                     const NoiseBiomeFn& noiseBiome, i64 seed, int seaLevel)
    : chunks_(chunks), facts_(facts), zoom_(zoom), noiseBiome_(noiseBiome), seed_(seed), seaLevel_(seaLevel) {
    for (GenChunk* c : chunks_)
        if (c && !c->finalHeightmapsPrimed) primeHeightmaps(*c, facts.impl());
}

GenChunk* GenRegion::chunk(int cx, int cz) const {
    const GenChunk* centre = chunks_[4];
    const int dx = cx - centre->cx + 1, dz = cz - centre->cz + 1;
    if (dx < 0 || dx > 2 || dz < 0 || dz > 2) return nullptr;
    return chunks_[size_t(dz * 3 + dx)];
}

u16 GenRegion::block(int x, int y, int z) const {
    decorationContext().lastReadY = y;
    const GenChunk* c = chunk(x >> 4, z >> 4);
    return c ? c->get(x & 15, y, z & 15) : GenChunk::kAir;
}

bool GenRegion::setBlock(const BlockPos& p, u16 id) {
    GenChunk* c = chunk(p.x >> 4, p.z >> 4);
    if (!c) return false;
    if (!c->inside(p.y)) return true;  // ProtoChunk ignores it (void air) but the write "succeeds"
    const int lx = p.x & 15, lz = p.z & 15;
    c->set(lx, p.y, lz, id);
    const BlockFacts::Impl& f = facts_.impl();
    for (int t = 0; t < 4; ++t) updateHeightmap(*c, f, t, lx, p.y, lz, id);
    return true;
}

bool GenRegion::isEmpty(const BlockPos& p) const { return facts_.impl().has(block(p), fact::Air); }

int GenRegion::height(Heightmap type, int x, int z) const {
    const GenChunk* c = chunk(x >> 4, z >> 4);
    if (!c) return minY();
    const int col = (z & 15) * 16 + (x & 15);
    switch (type) {
    case Heightmap::WorldSurfaceWG: return c->worldSurfaceWG[col];
    case Heightmap::OceanFloorWG: return c->oceanFloorWG[col];
    default: return c->finalHeightmaps[int(type) - 2][col];
    }
}

u16 GenRegion::biome(const BlockPos& p) const {
    // LevelReader.getNoiseBiome: the chunk's stored biomes (all inside the 3x3 in practice).
    const NoiseBiomeFn fn = [this](int qx, int qy, int qz) -> u16 {
        if (const GenChunk* c = chunk(qx >> 2, qz >> 2)) return c->biome(qx & 3, qy - (c->minY >> 2), qz & 3);
        return noiseBiome_(qx, qy, qz);
    };
    return zoom_.biome(p.x, p.y, p.z, fn);
}

bool GenRegion::inside(const BlockPos& p) const {
    const GenChunk* c = chunk(p.x >> 4, p.z >> 4);
    return c && c->inside(p.y);
}

int GenRegion::minY() const { return chunks_[4]->minY; }
int GenRegion::maxY() const { return chunks_[4]->minY + chunks_[4]->height; }

// ---- WorldgenRandom -----------------------------------------------------------------------------
// A LegacyRandomSource whose next(bits) comes from the wrapped source ((int) (nextLong() >>> 64 - bits)
// for xoroshiro). setSeed reseeds only the wrapped source and keeps the gaussian state.

WorldgenRandom::WorldgenRandom(std::unique_ptr<RandomSource> inner) : inner_(std::move(inner)) {}

i32 WorldgenRandom::next(int bits) {
    if (auto* legacy = dynamic_cast<LegacyRandom*>(inner_.get())) return legacy->next(bits);
    return i32(lshr(inner_->nextLong(), 64 - bits));
}

i64 WorldgenRandom::setDecorationSeed(i64 worldSeed, int blockX, int blockZ) {
    setSeed(worldSeed);
    const i64 xScale = nextLong() | 1, zScale = nextLong() | 1;
    const i64 result = jadd(jmul(i64(blockX), xScale), jmul(i64(blockZ), zScale)) ^ worldSeed;
    setSeed(result);
    return result;
}

void WorldgenRandom::setFeatureSeed(i64 decorationSeed, int index, int step) {
    setSeed(jadd(jadd(decorationSeed, i64(index)), i64(jmul32(10000, step))));
}

std::unique_ptr<RandomSource> WorldgenRandom::fork() { return inner_->fork(); }
std::unique_ptr<PositionalRandomFactory> WorldgenRandom::forkPositional() { return inner_->forkPositional(); }
void WorldgenRandom::setSeed(i64 seed) { inner_->setSeed(seed); }
i32 WorldgenRandom::nextInt() { return next(32); }

i32 WorldgenRandom::nextInt(i32 bound) {
    if (bound <= 0) return 0;  // the game throws
    if ((bound & (bound - 1)) == 0) return i32((i64(bound) * i64(next(31))) >> 31);
    i32 sample, modulo;
    do {
        sample = next(31);
        modulo = sample % bound;
    } while (i32(u32(sample) - u32(modulo) + u32(bound - 1)) < 0);
    return modulo;
}

i64 WorldgenRandom::nextLong() {
    const i32 upper = next(32), lower = next(32);
    return jadd(i64(u64(i64(upper)) << 32), i64(lower));
}

bool WorldgenRandom::nextBoolean() { return next(1) != 0; }
float WorldgenRandom::nextFloat() { return float(next(24)) * 5.9604645E-8f; }

double WorldgenRandom::nextDouble() {
    const i32 upper = next(26), lower = next(27);
    return double((i64(upper) << 27) + lower) * double(1.110223E-16f);
}

void WorldgenRandom::consumeCount(int rounds) {
    for (int i = 0; i < rounds; ++i) next(32);
}

const FeatureFactory* FeatureTypes::find(std::string_view type) const {
    const std::string want = DataPack::normalize(type);
    for (const auto& [name, factory] : factories_)
        if (DataPack::normalize(name) == want) return &factory;
    return nullptr;
}

namespace {

// ---- shared decoration data -----------------------------------------------------------------

// SimplexNoise, 2D (Biome's static noises; discardNoiseOffset = true).
class Simplex2D {
public:
    void init(RandomSource& random) {
        offsetX_ = random.nextDouble() * 0.0;
        offsetY_ = random.nextDouble() * 0.0;
        random.nextDouble();
        for (int i = 0; i < 256; ++i) perms_[i] = u8(i);
        for (int i = 0; i < 256; ++i) std::swap(perms_[i], perms_[random.nextInt(256 - i) + i]);
    }

    float get(double xIn, double yIn) const {
        static const double kSqrt3 = std::sqrt(3.0);
        static const double kF2 = 0.5 * (kSqrt3 - 1.0);
        static const double kG2 = (3.0 - kSqrt3) / 6.0;
        const double xin = xIn + offsetX_, yin = yIn + offsetY_;
        const double s = (xin + yin) * kF2;
        const int i = jfloor(xin + s), j = jfloor(yin + s);
        const double t = double(i + j) * kG2;
        const double x0 = xin - (i - t), y0 = yin - (j - t);
        const int i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
        const double x1 = x0 - i1 + kG2, y1 = y0 - j1 + kG2;
        const double x2 = x0 - 1.0 + 2.0 * kG2, y2 = y0 - 1.0 + 2.0 * kG2;
        const int ii = i & 0xFF, jj = j & 0xFF;
        const int gi0 = permute(ii + permute(jj)) % 12;
        const int gi1 = permute(ii + i1 + permute(jj + j1)) % 12;
        const int gi2 = permute(ii + 1 + permute(jj + 1)) % 12;
        return float(70.0 * (corner(gi0, x0, y0) + corner(gi1, x1, y1) + corner(gi2, x2, y2)));
    }

private:
    int permute(int x) const { return perms_[x & 0xFF]; }
    static double corner(int index, double x, double y) {
        static constexpr int kGrad[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
                                             {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
        double t0 = 0.5 - x * x - y * y - 0.0 * 0.0;
        if (t0 < 0.0) return 0.0;
        t0 *= t0;
        const int* g = kGrad[index];
        return t0 * t0 * (g[0] * x + g[1] * y + g[2] * 0.0);
    }

    double offsetX_ = 0, offsetY_ = 0;
    u8 perms_[256] = {};
};

struct BiomeClimate {
    float temperature = 0.5f;
    bool frozen = false, precipitation = true;
};

struct Shared {
    const NoiseState* noise = nullptr;
    BlockRegistry* blocks = nullptr;
    BiomeTable* biomes = nullptr;
    const BlockFacts::Impl* facts = nullptr;
    int genMinY = -64, genDepth = 384, seaLevel = 63;  // WorldGenerationContext
    Simplex2D temperatureNoise, frozenNoise[3], biomeInfoNoise;
    std::vector<BiomeClimate> climate;  // by biome id
    std::vector<std::unordered_set<const PlacedFeature*>> biomeFeatures;  // BiomeGenerationSettings.hasFeature

    // Biome.getTemperature (getHeightAdjustedTemperature with the climate's temperature modifier).
    float temperature(u16 biome, const BlockPos& p) const {
        const BiomeClimate c = biome < climate.size() ? climate[biome] : BiomeClimate{};
        float t = c.temperature;
        if (c.frozen) {
            float large = 0.0f;
            large += 0.14285715f * frozenNoise[0].get(p.x * 0.05 * 1.0, p.z * 0.05 * 1.0);
            large += 0.2857143f * frozenNoise[1].get(p.x * 0.05 * 0.5, p.z * 0.05 * 0.5);
            large += 0.5714286f * frozenNoise[2].get(p.x * 0.05 * 0.25, p.z * 0.05 * 0.25);
            const double icePatches = double(large * 7.0f) + double(biomeInfoNoise.get(p.x * 0.2, p.z * 0.2));
            if (icePatches < 0.3 && double(biomeInfoNoise.get(p.x * 0.09, p.z * 0.09)) < 0.8) t = 0.2f;
        }
        const int snowLevel = seaLevel + 17;
        if (p.y > snowLevel) {
            const float v = temperatureNoise.get(double(float(p.x) / 8.0f), double(float(p.z) / 8.0f)) * 8.0f;
            return t - (v + float(p.y) - float(snowLevel)) * 0.05f / 40.0f;
        }
        return t;
    }
};

std::string typeOf(const json& j, const char* key = "type") { return DataPack::normalize(j.at(key).get<std::string>()); }
float jfloat(const json& j) { return float(j.get<double>()); }

BlockPos parsePos(const json& j) {
    if (j.is_array() && j.size() >= 3) return {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
    return {};
}
BlockPos offsetOf(const json& j) { return j.contains("offset") ? parsePos(j["offset"]) : BlockPos{}; }
BlockPos operator+(const BlockPos& a, const BlockPos& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
BlockPos relative(const BlockPos& p, int dir, int n = 1) {
    return p.offset(kDirOffset[dir][0] * n, kDirOffset[dir][1] * n, kDirOffset[dir][2] * n);
}

int parseDirection(const json& j) {
    static const char* const kNames[6] = {"down", "up", "north", "south", "west", "east"};
    const std::string s = j.get<std::string>();
    for (int d = 0; d < 6; ++d)
        if (s == kNames[d]) return d;
    throw std::runtime_error("bad direction " + s);
}
const char* const kFaceNames[6] = {"down", "up", "north", "south", "west", "east"};
constexpr int kHorizontal[4] = {North, East, South, West};  // Direction.Plane.HORIZONTAL

Heightmap parseHeightmap(const json& j) {
    const std::string s = j.get<std::string>();
    if (s == "WORLD_SURFACE_WG") return Heightmap::WorldSurfaceWG;
    if (s == "OCEAN_FLOOR_WG") return Heightmap::OceanFloorWG;
    if (s == "WORLD_SURFACE") return Heightmap::WorldSurface;
    if (s == "MOTION_BLOCKING") return Heightmap::MotionBlocking;
    if (s == "MOTION_BLOCKING_NO_LEAVES") return Heightmap::MotionBlockingNoLeaves;
    if (s == "OCEAN_FLOOR") return Heightmap::OceanFloor;
    throw std::runtime_error("bad heightmap " + s);
}

// Util.shuffle
template <class T>
void shuffle(std::vector<T>& list, RandomSource& r) {
    for (int i = int(list.size()); i > 1; --i) std::swap(list[size_t(i - 1)], list[size_t(r.nextInt(i))]);
}

// ---- int / float / height providers ---------------------------------------------------------

class ConstInt final : public IntProvider {
public:
    explicit ConstInt(i32 v) : v_(v) {}
    i32 sample(RandomSource&) const override { return v_; }
    i32 minValue() const override { return v_; }
    i32 maxValue() const override { return v_; }

private:
    i32 v_;
};

class RangeInt final : public IntProvider {  // uniform, biased_to_bottom, very_biased_to_bottom
public:
    enum Kind { Uniform, Biased, VeryBiased };
    RangeInt(Kind k, i32 lo, i32 hi) : k_(k), lo_(lo), hi_(hi) {}
    i32 sample(RandomSource& r) const override {
        switch (k_) {
        case Uniform: return r.nextInt(hi_ - lo_ + 1) + lo_;
        case Biased: return lo_ + r.nextInt(r.nextInt(hi_ - lo_ + 1) + 1);
        default: return lo_ + r.nextInt(r.nextInt(r.nextInt(hi_ - lo_ + 1) + 1) + 1);
        }
    }
    i32 minValue() const override { return lo_; }
    i32 maxValue() const override { return hi_; }

private:
    Kind k_;
    i32 lo_, hi_;
};

class ClampedInt final : public IntProvider {
public:
    ClampedInt(std::unique_ptr<IntProvider> src, i32 lo, i32 hi) : src_(std::move(src)), lo_(lo), hi_(hi) {}
    i32 sample(RandomSource& r) const override { return std::clamp(src_->sample(r), lo_, hi_); }
    i32 minValue() const override { return std::max(lo_, src_->minValue()); }
    i32 maxValue() const override { return std::min(hi_, src_->maxValue()); }

private:
    std::unique_ptr<IntProvider> src_;
    i32 lo_, hi_;
};

float clampF(float v, float lo, float hi) { return v < lo ? lo : std::min(v, hi); }  // Mth.clamp(float)

class ClampedNormalInt final : public IntProvider {
public:
    ClampedNormalInt(float mean, float dev, i32 lo, i32 hi) : mean_(mean), dev_(dev), lo_(lo), hi_(hi) {}
    i32 sample(RandomSource& r) const override {
        return i32(clampF(mean_ + float(r.nextGaussian()) * dev_, float(lo_), float(hi_)));
    }
    i32 minValue() const override { return lo_; }
    i32 maxValue() const override { return hi_; }

private:
    float mean_, dev_;
    i32 lo_, hi_;
};

class TrapezoidInt final : public IntProvider {
public:
    TrapezoidInt(i32 lo, i32 hi, i32 plateau) : lo_(lo), hi_(hi), plateau_(plateau) {}
    i32 sample(RandomSource& r) const override {
        if (plateau_ == 0 && hi_ == -lo_) {
            const i32 a = r.nextInt(hi_ + 1);
            return a - r.nextInt(hi_ + 1);
        }
        const i32 range = hi_ - lo_;
        if (plateau_ == range) return r.nextInt(hi_ - lo_ + 1) + lo_;
        const i32 plateauStart = (range - plateau_) / 2;
        const i32 plateauEnd = range - plateauStart;
        const i32 a = r.nextInt(plateauEnd + 1);
        return lo_ + a + r.nextInt(plateauStart + 1);
    }
    i32 minValue() const override { return lo_; }
    i32 maxValue() const override { return hi_; }

private:
    i32 lo_, hi_, plateau_;
};

// WeightedList.getRandomOrThrow: nextInt(total), then the cumulative pick.
template <class T>
struct Weighted {
    std::vector<T> items;
    std::vector<i32> weights;
    i32 total = 0;
    const T* pick(RandomSource& r) const {
        if (total <= 0) return nullptr;
        i32 selection = r.nextInt(total);
        for (size_t i = 0; i < items.size(); ++i) {
            selection -= weights[i];
            if (selection < 0) return &items[i];
        }
        return &items.back();
    }
    void add(T item, i32 weight) {
        items.push_back(std::move(item));
        weights.push_back(weight);
        total += weight;
    }
};

class WeightedInt final : public IntProvider {
public:
    explicit WeightedInt(Weighted<std::unique_ptr<IntProvider>> list) : list_(std::move(list)) {
        for (const auto& p : list_.items) lo_ = std::min(lo_, p->minValue()), hi_ = std::max(hi_, p->maxValue());
    }
    i32 sample(RandomSource& r) const override { return (*list_.pick(r))->sample(r); }
    i32 minValue() const override { return lo_; }
    i32 maxValue() const override { return hi_; }

private:
    Weighted<std::unique_ptr<IntProvider>> list_;
    i32 lo_ = INT_MAX, hi_ = INT_MIN;
};

std::unique_ptr<IntProvider> parseInt(const json& j) {
    if (j.is_number()) return std::make_unique<ConstInt>(j.get<i32>());
    const std::string type = typeOf(j);
    auto range = [&](RangeInt::Kind k) {
        return std::make_unique<RangeInt>(k, j.at("min_inclusive").get<i32>(), j.at("max_inclusive").get<i32>());
    };
    if (type == "constant") return std::make_unique<ConstInt>(j.at("value").get<i32>());
    if (type == "uniform") return range(RangeInt::Uniform);
    if (type == "biased_to_bottom") return range(RangeInt::Biased);
    if (type == "very_biased_to_bottom") return range(RangeInt::VeryBiased);
    if (type == "clamped")
        return std::make_unique<ClampedInt>(parseInt(j.at("source")), j.at("min_inclusive").get<i32>(),
                                            j.at("max_inclusive").get<i32>());
    if (type == "clamped_normal")
        return std::make_unique<ClampedNormalInt>(jfloat(j.at("mean")), jfloat(j.at("deviation")),
                                                  j.at("min_inclusive").get<i32>(), j.at("max_inclusive").get<i32>());
    if (type == "trapezoid")
        return std::make_unique<TrapezoidInt>(j.at("min").get<i32>(), j.at("max").get<i32>(), j.value("plateau", 0));
    if (type == "weighted_list") {
        Weighted<std::unique_ptr<IntProvider>> list;
        for (const json& e : j.at("distribution")) list.add(parseInt(e.at("data")), e.at("weight").get<i32>());
        return std::make_unique<WeightedInt>(std::move(list));
    }
    throw std::runtime_error("unknown int provider " + type);
}

class SimpleFloat final : public FloatProvider {
public:
    enum Kind { Constant, Uniform, ClampedNormal, Trapezoid };
    Kind kind = Constant;
    float a = 0, b = 0, c = 0, d = 0;
    float sample(RandomSource& r) const override {
        switch (kind) {
        case Constant: return a;
        case Uniform: return r.nextFloat() * (b - a) + a;
        case ClampedNormal: return clampF(a + float(r.nextGaussian()) * b, c, d);
        case Trapezoid: {
            const float range = b - a;
            const float plateauStart = (range - c) / 2.0f;
            const float plateauEnd = range - plateauStart;
            const float f1 = r.nextFloat();
            return a + f1 * plateauEnd + r.nextFloat() * plateauStart;
        }
        }
        return a;
    }
};

std::unique_ptr<FloatProvider> parseFloat(const json& j) {
    auto p = std::make_unique<SimpleFloat>();
    if (j.is_number()) {
        p->a = jfloat(j);
        return p;
    }
    const std::string type = typeOf(j);
    if (type == "constant") {
        p->a = jfloat(j.at("value"));
    } else if (type == "uniform") {
        p->kind = SimpleFloat::Uniform, p->a = jfloat(j.at("min_inclusive")), p->b = jfloat(j.at("max_exclusive"));
    } else if (type == "clamped_normal") {
        p->kind = SimpleFloat::ClampedNormal, p->a = jfloat(j.at("mean")), p->b = jfloat(j.at("deviation"));
        p->c = jfloat(j.at("min")), p->d = jfloat(j.at("max"));
    } else if (type == "trapezoid") {
        p->kind = SimpleFloat::Trapezoid, p->a = jfloat(j.at("min")), p->b = jfloat(j.at("max"));
        p->c = jfloat(j.at("plateau"));
    } else {
        throw std::runtime_error("unknown float provider " + type);
    }
    return p;
}

struct Anchor {  // VerticalAnchor
    enum Kind : u8 { Absolute, AboveBottom, BelowTop, SeaLevel } kind = Absolute;
    i32 value = 0;
    i32 resolve(int minY, int height, int seaLevel) const {
        switch (kind) {
        case Absolute: return value;
        case AboveBottom: return minY + value;
        case BelowTop: return height - 1 + minY - value;
        case SeaLevel: return seaLevel + value;
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

class HeightProv final : public HeightProvider {
public:
    enum Kind : u8 { Constant, Uniform, BiasedToBottom, VeryBiasedToBottom, Trapezoid, WeightedList } kind = Constant;
    Anchor lo, hi;
    i32 inner = 1, plateau = 0, seaLevel = 63;
    Weighted<std::unique_ptr<HeightProvider>> list;

    i32 sample(RandomSource& r, int minY, int height) const override {
        if (kind == Constant) return lo.resolve(minY, height, seaLevel);
        if (kind == WeightedList) return (*list.pick(r))->sample(r, minY, height);
        const i32 a = lo.resolve(minY, height, seaLevel), b = hi.resolve(minY, height, seaLevel);
        auto between = [&](i32 x, i32 y) { return r.nextInt(y - x + 1) + x; };  // Mth.randomBetweenInclusive
        auto nextInt = [&](i32 x, i32 y) { return x >= y ? x : r.nextInt(y - x + 1) + x; };  // Mth.nextInt
        switch (kind) {
        case Uniform: return a > b ? a : between(a, b);
        case Trapezoid: {
            if (a > b) return a;
            const i32 range = b - a;
            if (plateau >= range) return between(a, b);
            const i32 plateauStart = (range - plateau) / 2;
            const i32 plateauEnd = range - plateauStart;
            const i32 v = between(0, plateauEnd);
            return a + v + between(0, plateauStart);
        }
        case BiasedToBottom: {
            if (b - a - inner + 1 <= 0) return a;
            const i32 limit = r.nextInt(b - a - inner + 1);
            return r.nextInt(limit + inner) + a;
        }
        case VeryBiasedToBottom: {
            if (b - a - inner + 1 <= 0) return a;
            const i32 upper = nextInt(a + inner, b);
            const i32 biasedUpper = nextInt(a, upper - 1);
            return nextInt(a, biasedUpper - 1 + inner);
        }
        default: return a;
        }
    }
};

std::unique_ptr<HeightProvider> parseHeight(const json& j, int seaLevel) {
    auto p = std::make_unique<HeightProv>();
    p->seaLevel = seaLevel;
    if (Anchor::is(j)) {
        p->lo = Anchor::parse(j);
        return p;
    }
    const std::string type = typeOf(j);
    auto range = [&](HeightProv::Kind k) {
        p->kind = k, p->lo = Anchor::parse(j.at("min_inclusive")), p->hi = Anchor::parse(j.at("max_inclusive"));
    };
    if (type == "constant") {
        p->lo = Anchor::parse(j.at("value"));
    } else if (type == "uniform") {
        range(HeightProv::Uniform);
    } else if (type == "trapezoid") {
        range(HeightProv::Trapezoid);
        p->plateau = j.value("plateau", 0);
    } else if (type == "biased_to_bottom") {
        range(HeightProv::BiasedToBottom);
        p->inner = j.value("inner", 1);
    } else if (type == "very_biased_to_bottom") {
        range(HeightProv::VeryBiasedToBottom);
        p->inner = j.value("inner", 1);
    } else if (type == "weighted_list") {
        p->kind = HeightProv::WeightedList;
        for (const json& e : j.at("distribution")) p->list.add(parseHeight(e.at("data"), seaLevel), e.at("weight").get<i32>());
    } else {
        throw std::runtime_error("unknown height provider " + type);
    }
    return p;
}

// ---- block state providers -------------------------------------------------------------------

constexpr u16 kNone = StateProvider::kNone;

// Base: the positional form reads the level being decorated on this thread.
class Provider : public StateProvider {
public:
    using StateProvider::state;
    u16 state(RandomSource& r, const BlockPos& p) const override { return state(*decorationContext().level, r, p); }
};
using ProviderRef = std::shared_ptr<const StateProvider>;

u16 getState(const StateProvider& sp, const GenRegion& level, RandomSource& r, const BlockPos& p) {
    return sp.state(level, r, p);
}
u16 getOptional(const StateProvider& sp, const GenRegion& level, RandomSource& r, const BlockPos& p) {
    return sp.optionalState(level, r, p);
}

class SimpleProvider final : public Provider {
public:
    explicit SimpleProvider(u16 s) : s_(s) {}
    u16 state(const GenRegion&, RandomSource&, const BlockPos&) const override { return s_; }

private:
    u16 s_;
};

class WeightedProvider final : public Provider {
public:
    explicit WeightedProvider(Weighted<u16> list) : list_(std::move(list)) {}
    u16 state(const GenRegion&, RandomSource& r, const BlockPos&) const override { return *list_.pick(r); }

private:
    Weighted<u16> list_;
};

class RandomBlockProvider final : public Provider {
public:
    explicit RandomBlockProvider(std::vector<u16> states) : states_(std::move(states)) {}
    u16 state(const GenRegion& level, RandomSource& r, const BlockPos& p) const override {
        const u16 s = optionalState(level, r, p);
        return s == kNone ? level.block(p) : s;
    }
    u16 optionalState(const GenRegion&, RandomSource& r, const BlockPos&) const override {
        if (states_.empty()) return kNone;
        return states_[size_t(r.nextInt(i32(states_.size())))];
    }

private:
    std::vector<u16> states_;
};

class RandomizedIntProvider final : public Provider {
public:
    RandomizedIntProvider(ProviderRef source, std::string property, std::unique_ptr<IntProvider> values,
                          BlockRegistry& blocks)
        : source_(std::move(source)), property_(std::move(property)), values_(std::move(values)), blocks_(blocks) {}
    u16 state(const GenRegion& level, RandomSource& r, const BlockPos& p) const override {
        const u16 s = getState(*source_, level, r, p);
        if (blocks_.property(s, property_).empty()) return s;
        return blocks_.withProperty(s, property_, std::to_string(values_->sample(r)));
    }

private:
    ProviderRef source_;
    std::string property_;
    std::unique_ptr<IntProvider> values_;
    BlockRegistry& blocks_;
};

class RotatedProvider final : public Provider {
public:
    RotatedProvider(ProviderRef inner, int direction, BlockRegistry& blocks)
        : inner_(std::move(inner)), direction_(direction), blocks_(blocks) {}
    u16 state(const GenRegion& level, RandomSource& r, const BlockPos& p) const override {
        static const char* const kAxis[6] = {"y", "y", "z", "z", "x", "x"};
        const int dir = direction_ >= 0 ? direction_ : r.nextInt(6);  // Direction.getRandom
        u16 s = getState(*inner_, level, r, p);
        if (!blocks_.property(s, "axis").empty()) s = blocks_.withProperty(s, "axis", kAxis[dir]);
        if (!blocks_.property(s, "facing").empty()) s = blocks_.withProperty(s, "facing", kFaceNames[dir]);
        return s;
    }

private:
    ProviderRef inner_;
    int direction_;
    BlockRegistry& blocks_;
};

class RuleBasedProvider final : public Provider {
public:
    struct Rule {
        std::unique_ptr<BlockPredicate> ifTrue;
        ProviderRef then;
    };
    RuleBasedProvider(ProviderRef fallback, std::vector<Rule> rules) : fallback_(std::move(fallback)), rules_(std::move(rules)) {}
    u16 state(const GenRegion& level, RandomSource& r, const BlockPos& p) const override {
        const u16 s = optionalState(level, r, p);
        return s == kNone ? level.block(p) : s;
    }
    u16 optionalState(const GenRegion& level, RandomSource& r, const BlockPos& p) const override {
        for (const Rule& rule : rules_)
            if (rule.ifTrue->test(level, p)) {
                const u16 s = getOptional(*rule.then, level, r, p);
                if (s != kNone) return s;
            }
        return fallback_ ? getOptional(*fallback_, level, r, p) : kNone;
    }

private:
    ProviderRef fallback_;
    std::vector<Rule> rules_;
};

class CopyPropertiesProvider final : public Provider {
public:
    CopyPropertiesProvider(ProviderRef source, BlockRegistry& blocks) : source_(std::move(source)), blocks_(blocks) {}
    u16 state(const GenRegion& level, RandomSource& r, const BlockPos& p) const override {
        u16 s = getState(*source_, level, r, p);
        const std::string existing = blocks_.name(level.block(p));
        for (const auto& [k, v] : stateProperties(existing))
            if (!blocks_.property(s, k).empty()) s = blocks_.withProperty(s, k, v);
        return s;
    }

private:
    ProviderRef source_;
    BlockRegistry& blocks_;
};

// NoiseBasedStateProvider family: NormalNoise seeded with WorldgenRandom(LegacyRandomSource(seed)).
class NoiseProvider final : public Provider {
public:
    enum Kind { Plain, Dual, Threshold } kind = Plain;
    std::shared_ptr<NoiseStack> noise, slowNoise;
    float scale = 1, slowScale = 1, threshold = 0, highChance = 0;
    int varietyMin = 1, varietyMax = 1;
    std::vector<u16> states, lowStates, highStates;
    u16 defaultState = 0;

    u16 state(const GenRegion&, RandomSource& r, const BlockPos& p) const override {
        switch (kind) {
        case Plain: return pick(states, noiseAt(p, scale));
        case Dual: {
            const double variety = double(slowAt(p));
            const int local = i32(clampedMap(variety, -1.0, 1.0, double(varietyMin), double(varietyMax + 1)));
            std::vector<u16> possible;
            for (int i = 0; i < local; ++i)
                possible.push_back(pick(states, slowAt(p.offset(jmul32(i, 54545), 0, jmul32(i, 34234)))));
            return pick(possible, noiseAt(p, scale));
        }
        case Threshold: {
            const double local = double(noiseAt(p, scale));
            if (local < double(threshold)) return lowStates[size_t(r.nextInt(i32(lowStates.size())))];
            return r.nextFloat() < highChance ? highStates[size_t(r.nextInt(i32(highStates.size())))] : defaultState;
        }
        }
        return 0;
    }

private:
    float noiseAt(const BlockPos& p, double s) const { return noise->get(p.x * s, p.y * s, p.z * s); }
    float slowAt(const BlockPos& p) const {  // int * float: float math
        return slowNoise->get(double(float(p.x) * slowScale), double(float(p.y) * slowScale),
                              double(float(p.z) * slowScale));
    }
    static u16 pick(const std::vector<u16>& list, float value) {
        const float placement = clampF((1.0f + value) / 2.0f, 0.0f, 0.9999f);
        return list[size_t(i32(placement * float(list.size())))];
    }
};

// ---- block predicates --------------------------------------------------------------------------

class Pred : public BlockPredicate {};
using PredPtr = std::unique_ptr<BlockPredicate>;

class TruePred final : public Pred {
public:
    bool test(const GenRegion&, const BlockPos&) const override { return true; }
};

class BlocksPred final : public Pred {  // matching_blocks / matching_block_tag
public:
    BlocksPred(const BlockFacts::Impl& f, BlockPos offset, BlockBits set) : f_(f), offset_(offset), set_(std::move(set)) {}
    bool test(const GenRegion& level, const BlockPos& p) const override {
        return set_.test(f_.blockOf(level.block(p + offset_)));
    }

private:
    const BlockFacts::Impl& f_;
    BlockPos offset_;
    BlockBits set_;
};

class FluidsPred final : public Pred {
public:
    FluidsPred(const BlockFacts::Impl& f, BlockPos offset, u8 mask) : f_(f), offset_(offset), mask_(mask) {}
    bool test(const GenRegion& level, const BlockPos& p) const override {
        return (mask_ >> f_.fluidType(level.block(p + offset_))) & 1;
    }

private:
    const BlockFacts::Impl& f_;
    BlockPos offset_;
    u8 mask_;  // bit per FluidType (bit 0: empty)
};

class FactPred final : public Pred {  // solid / replaceable / has_sturdy_face
public:
    FactPred(const BlockFacts::Impl& f, BlockPos offset, u64 bits) : f_(f), offset_(offset), bits_(bits) {}
    bool test(const GenRegion& level, const BlockPos& p) const override {
        return (f_.facts(level.block(p + offset_)) & bits_) != 0;
    }

private:
    const BlockFacts::Impl& f_;
    BlockPos offset_;
    u64 bits_;
};

class BiomesPred final : public Pred {
public:
    explicit BiomesPred(std::vector<bool> biomes) : biomes_(std::move(biomes)) {}
    bool test(const GenRegion& level, const BlockPos& p) const override {
        const u16 b = level.biome(p);
        return b < biomes_.size() && biomes_[b];
    }

private:
    std::vector<bool> biomes_;
};

class SurvivePred final : public Pred {
public:
    SurvivePred(const BlockFacts::Impl& f, BlockPos offset, u16 state) : f_(f), offset_(offset), state_(state) {}
    bool test(const GenRegion& level, const BlockPos& p) const override {
        return f_.canSurvive(level, state_, p + offset_);
    }

private:
    const BlockFacts::Impl& f_;
    BlockPos offset_;
    u16 state_;
};

class InsideWorldPred final : public Pred {
public:
    explicit InsideWorldPred(BlockPos offset) : offset_(offset) {}
    bool test(const GenRegion& level, const BlockPos& p) const override {
        const int y = p.y + offset_.y;
        return y >= level.minY() && y < level.maxY();
    }

private:
    BlockPos offset_;
};

class HeightRangePred final : public Pred {
public:
    HeightRangePred(const Shared& s, Anchor lo, Anchor hi) : s_(s), lo_(lo), hi_(hi) {}
    bool test(const GenRegion&, const BlockPos& p) const override {
        return p.y >= lo_.resolve(s_.genMinY, s_.genDepth, s_.seaLevel) &&
               p.y <= hi_.resolve(s_.genMinY, s_.genDepth, s_.seaLevel);
    }

private:
    const Shared& s_;
    Anchor lo_, hi_;
};

class CombinePred final : public Pred {  // all_of / any_of / not
public:
    enum Kind { All, Any, Not };
    CombinePred(Kind k, std::vector<PredPtr> preds) : k_(k), preds_(std::move(preds)) {}
    bool test(const GenRegion& level, const BlockPos& p) const override {
        if (k_ == Not) return !preds_[0]->test(level, p);
        for (const PredPtr& pr : preds_)
            if (pr->test(level, p) == (k_ == Any)) return k_ == Any;
        return k_ == All;
    }

private:
    Kind k_;
    std::vector<PredPtr> preds_;
};

class VolumePred final : public Pred {
public:
    VolumePred(BlockPos lo, BlockPos hi, PredPtr match) : lo_(lo), hi_(hi), match_(std::move(match)) {}
    bool test(const GenRegion& level, const BlockPos& p) const override {
        for (int x = lo_.x; x <= hi_.x; ++x)
            for (int z = lo_.z; z <= hi_.z; ++z)
                for (int y = lo_.y; y <= hi_.y; ++y)
                    if (!match_->test(level, p.offset(x, y, z))) return false;
        return true;
    }

private:
    BlockPos lo_, hi_;
    PredPtr match_;
};

// ---- rule tests -----------------------------------------------------------------------------------

class Rule : public RuleTest {
public:
    using RuleTest::test;
    bool test(u16 state, RandomSource& r) const override {
        return test(state, BlockPos{0, decorationContext().lastReadY, 0}, r);
    }
};
using RulePtr = std::unique_ptr<RuleTest>;

class RuleImpl final : public Rule {
public:
    enum Kind { True, Block, State, Tag, RandomBlock, RandomState, All, Any, Not, Height };
    Kind kind = True;
    const BlockFacts::Impl* facts = nullptr;
    BlockBits blocks;  // Block / Tag / RandomBlock
    u16 state = 0;
    float probability = 0;
    int lo = 0, hi = 0;
    std::vector<RulePtr> rules;

    using Rule::test;
    bool test(u16 s, const BlockPos& p, RandomSource& r) const override {
        switch (kind) {
        case True: return true;
        case Block:
        case Tag: return blocks.test(facts->blockOf(s));
        case State: return s == state;
        case RandomBlock: return blocks.test(facts->blockOf(s)) && r.nextFloat() < probability;
        case RandomState: return s == state && r.nextFloat() < probability;
        case All:
            for (const RulePtr& rule : rules)
                if (!rule->test(s, p, r)) return false;
            return true;
        case Any:
            for (const RulePtr& rule : rules)
                if (rule->test(s, p, r)) return true;
            return false;
        case Not: return !rules[0]->test(s, p, r);
        case Height: return lo <= p.y && p.y <= hi;
        }
        return false;
    }
};

}  // namespace

// ---- placed features and placement modifiers (FeaturePlacer) ------------------------------------

namespace {

struct PlaceCtx {
    GenRegion& level;
    const PlacedFeature* top;  // the biome-checked top level feature, or null
};

class PlacementModifier {
public:
    virtual ~PlacementModifier() = default;
    virtual void modify(PlaceCtx& ctx, RandomSource& r, const BlockPos& origin, std::vector<BlockPos>& out) const = 0;
};

}  // namespace

class PlacedFeature {
public:
    std::string id;
    std::shared_ptr<const ConfiguredFeature> feature;
    std::vector<std::unique_ptr<PlacementModifier>> modifiers;

    // FeaturePlacer.place: modifiers applied depth first; each position's outputs are visited in order.
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin, const PlacedFeature* top) const {
        if (modifiers.empty()) return feature->place(level, r, origin);
        PlaceCtx ctx{level, top};
        std::vector<BlockPos> positions{origin}, out;
        std::vector<size_t> indices{0};
        bool placedAny = false;
        while (!positions.empty()) {
            const BlockPos pos = positions.back();
            const size_t index = indices.back();
            positions.pop_back();
            indices.pop_back();
            out.clear();
            modifiers[index]->modify(ctx, r, pos, out);
            if (index + 1 < modifiers.size()) {
                for (size_t i = out.size(); i-- > 0;) {
                    positions.push_back(out[i]);
                    indices.push_back(index + 1);
                }
            } else {
                for (const BlockPos& p : out) placedAny |= feature->place(level, r, p);
            }
        }
        return placedAny;
    }
};

namespace {

// (int) of a double in Java: truncation, saturating, NaN -> 0.
i32 javaInt(double v) {
    if (v != v) return 0;
    if (v >= 2147483647.0) return INT_MAX;
    if (v <= -2147483648.0) return INT_MIN;
    return i32(v);
}

class CountMod final : public PlacementModifier {
public:
    explicit CountMod(std::unique_ptr<IntProvider> count) : count_(std::move(count)) {}
    void modify(PlaceCtx&, RandomSource& r, const BlockPos& o, std::vector<BlockPos>& out) const override {
        const int n = count_->sample(r);
        for (int i = 0; i < n; ++i) out.push_back(o);
    }

private:
    std::unique_ptr<IntProvider> count_;
};

class NoiseCountMod final : public PlacementModifier {  // noise_based_count / noise_threshold_count
public:
    NoiseCountMod(const Shared& s, bool threshold, const json& j) : s_(s), threshold_(threshold) {
        if (threshold) {
            level_ = j.at("noise_level").get<double>();
            below_ = j.at("below_noise").get<int>();
            above_ = j.at("above_noise").get<int>();
        } else {
            ratio_ = j.at("noise_to_count_ratio").get<int>();
            factor_ = j.at("noise_factor").get<double>();
            offset_ = j.value("noise_offset", 0.0);
        }
    }
    void modify(PlaceCtx&, RandomSource&, const BlockPos& o, std::vector<BlockPos>& out) const override {
        int n;
        if (threshold_) {
            const double noise = s_.biomeInfoNoise.get(o.x / 200.0, o.z / 200.0);
            n = noise < level_ ? below_ : above_;
        } else {
            const double noise = s_.biomeInfoNoise.get(o.x / factor_, o.z / factor_);
            n = javaInt(std::ceil((noise + offset_) * ratio_));
        }
        for (int i = 0; i < n; ++i) out.push_back(o);
    }

private:
    const Shared& s_;
    bool threshold_;
    double level_ = 0, factor_ = 1, offset_ = 0;
    int below_ = 0, above_ = 0, ratio_ = 0;
};

class ChanceMod final : public PlacementModifier {  // rarity_filter / random_chance
public:
    explicit ChanceMod(float chance) : chance_(chance) {}
    void modify(PlaceCtx&, RandomSource& r, const BlockPos& o, std::vector<BlockPos>& out) const override {
        if (r.nextFloat() < chance_) out.push_back(o);
    }

private:
    float chance_;
};

class InSquareMod final : public PlacementModifier {
public:
    void modify(PlaceCtx&, RandomSource& r, const BlockPos& o, std::vector<BlockPos>& out) const override {
        const int x = r.nextInt(16) + o.x;
        const int z = r.nextInt(16) + o.z;
        out.push_back({x, o.y, z});
    }
};

class HeightRangeMod final : public PlacementModifier {
public:
    HeightRangeMod(const Shared& s, std::unique_ptr<HeightProvider> h) : s_(s), h_(std::move(h)) {}
    void modify(PlaceCtx&, RandomSource& r, const BlockPos& o, std::vector<BlockPos>& out) const override {
        out.push_back({o.x, h_->sample(r, s_.genMinY, s_.genDepth), o.z});
    }

private:
    const Shared& s_;
    std::unique_ptr<HeightProvider> h_;
};

class HeightmapMod final : public PlacementModifier {
public:
    explicit HeightmapMod(Heightmap type) : type_(type) {}
    void modify(PlaceCtx& ctx, RandomSource&, const BlockPos& o, std::vector<BlockPos>& out) const override {
        const int h = ctx.level.height(type_, o.x, o.z);
        if (h > ctx.level.minY()) out.push_back({o.x, h, o.z});
    }

private:
    Heightmap type_;
};

class BiomeMod final : public PlacementModifier {
public:
    explicit BiomeMod(const Shared& s) : s_(s) {}
    void modify(PlaceCtx& ctx, RandomSource&, const BlockPos& o, std::vector<BlockPos>& out) const override {
        if (!ctx.top) return;  // the game throws: biome filters only run on top level features
        const u16 b = ctx.level.biome(o);
        if (b < s_.biomeFeatures.size() && s_.biomeFeatures[b].count(ctx.top)) out.push_back(o);
    }

private:
    const Shared& s_;
};

class PredicateMod final : public PlacementModifier {
public:
    explicit PredicateMod(PredPtr p) : p_(std::move(p)) {}
    void modify(PlaceCtx& ctx, RandomSource&, const BlockPos& o, std::vector<BlockPos>& out) const override {
        if (p_->test(ctx.level, o)) out.push_back(o);
    }

private:
    PredPtr p_;
};

class EnvironmentScanMod final : public PlacementModifier {
public:
    EnvironmentScanMod(int dir, PredPtr target, PredPtr allowed, int maxSteps)
        : dir_(dir), target_(std::move(target)), allowed_(std::move(allowed)), maxSteps_(maxSteps) {}
    void modify(PlaceCtx& ctx, RandomSource&, const BlockPos& o, std::vector<BlockPos>& out) const override {
        const GenRegion& level = ctx.level;
        BlockPos p = o;
        if (!allowed_->test(level, p)) return;
        for (int i = 0; i < maxSteps_; ++i) {
            if (target_->test(level, p)) {
                out.push_back(p);
                return;
            }
            p = relative(p, dir_);
            if (p.y < level.minY() || p.y >= level.maxY()) return;
            if (!allowed_->test(level, p)) break;
        }
        if (target_->test(level, p)) out.push_back(p);
    }

private:
    int dir_;
    PredPtr target_, allowed_;
    int maxSteps_;
};

class SurfaceRelativeMod final : public PlacementModifier {
public:
    SurfaceRelativeMod(Heightmap type, i64 lo, i64 hi) : type_(type), lo_(lo), hi_(hi) {}
    void modify(PlaceCtx& ctx, RandomSource&, const BlockPos& o, std::vector<BlockPos>& out) const override {
        const i64 surface = ctx.level.height(type_, o.x, o.z);
        if (surface + lo_ <= o.y && o.y <= surface + hi_) out.push_back(o);
    }

private:
    Heightmap type_;
    i64 lo_, hi_;
};

class WaterDepthMod final : public PlacementModifier {
public:
    explicit WaterDepthMod(int maxDepth) : maxDepth_(maxDepth) {}
    void modify(PlaceCtx& ctx, RandomSource&, const BlockPos& o, std::vector<BlockPos>& out) const override {
        const int floor = ctx.level.height(Heightmap::OceanFloor, o.x, o.z);
        const int surface = ctx.level.height(Heightmap::WorldSurface, o.x, o.z);
        if (surface - floor <= maxDepth_) out.push_back(o);
    }

private:
    int maxDepth_;
};

class EveryLayerMod final : public PlacementModifier {
public:
    EveryLayerMod(const BlockFacts::Impl& f, std::unique_ptr<IntProvider> count) : f_(f), count_(std::move(count)) {}
    void modify(PlaceCtx& ctx, RandomSource& r, const BlockPos& o, std::vector<BlockPos>& out) const override {
        int layer = 0;
        bool foundAny;
        do {
            foundAny = false;
            for (int i = 0; i < count_->sample(r); ++i) {  // the count is resampled every iteration
                const int x = r.nextInt(16) + o.x;
                const int z = r.nextInt(16) + o.z;
                const int startY = ctx.level.height(Heightmap::MotionBlocking, x, z);
                const int y = onGround(ctx.level, x, startY, z, layer);
                if (y != INT_MAX) {
                    out.push_back({x, y, z});
                    foundAny = true;
                }
            }
            ++layer;
        } while (foundAny);
    }

private:
    bool empty(u16 s) const { return f_.has(s, fact::Air) || f_.is(s, f_.bWater) || f_.is(s, f_.bLava); }
    int onGround(const GenRegion& level, int x, int yStart, int z, int wanted) const {
        int layer = 0;
        u16 current = level.block(x, yStart, z);
        for (int y = yStart; y >= level.minY() + 1; --y) {
            const u16 below = level.block(x, y - 1, z);
            if (!empty(below) && empty(current) && !f_.is(below, f_.bBedrock)) {
                if (layer == wanted) return y;
                ++layer;
            }
            current = below;
        }
        return INT_MAX;
    }

    const BlockFacts::Impl& f_;
    std::unique_ptr<IntProvider> count_;
};

class OffsetMod final : public PlacementModifier {
public:
    OffsetMod(std::unique_ptr<IntProvider> x, std::unique_ptr<IntProvider> y, std::unique_ptr<IntProvider> z)
        : x_(std::move(x)), y_(std::move(y)), z_(std::move(z)) {}
    void modify(PlaceCtx&, RandomSource& r, const BlockPos& o, std::vector<BlockPos>& out) const override {
        const int dx = x_->sample(r);
        const int dy = y_->sample(r);
        const int dz = z_->sample(r);
        out.push_back(o.offset(dx, dy, dz));
    }

private:
    std::unique_ptr<IntProvider> x_, y_, z_;
};

class FixedMod final : public PlacementModifier {
public:
    explicit FixedMod(std::vector<BlockPos> positions) : positions_(std::move(positions)) {}
    void modify(PlaceCtx&, RandomSource&, const BlockPos& o, std::vector<BlockPos>& out) const override {
        for (const BlockPos& p : positions_)
            if ((p.x >> 4) == (o.x >> 4) && (p.z >> 4) == (o.z >> 4)) out.push_back(p);
    }

private:
    std::vector<BlockPos> positions_;
};

class CuboidMod final : public PlacementModifier {
public:
    CuboidMod(std::unique_ptr<IntProvider> xz, std::unique_ptr<IntProvider> y, bool edges, bool interior)
        : xz_(std::move(xz)), y_(std::move(y)), edges_(edges), interior_(interior) {}
    void modify(PlaceCtx&, RandomSource& r, const BlockPos& o, std::vector<BlockPos>& out) const override {
        const int height = y_->sample(r);
        const int width = xz_->sample(r);
        const int length = xz_->sample(r);
        for (int x = 0; x <= width; ++x)
            for (int y = 0; y <= height; ++y)
                for (int z = 0; z <= length; ++z) {
                    const bool xIn = x != 0 && x != width, yIn = y != 0 && y != height, zIn = z != 0 && z != length;
                    if ((edges_ || xIn || yIn) && (edges_ || zIn || yIn) && (edges_ || xIn || zIn) &&
                        (interior_ || !xIn || !yIn || !zIn))
                        out.push_back(o.offset(x, y, z));
                }
    }

private:
    std::unique_ptr<IntProvider> xz_, y_;
    bool edges_, interior_;
};

class RandomlySelectedMod final : public PlacementModifier {
public:
    explicit RandomlySelectedMod(std::vector<std::unique_ptr<PlacementModifier>> list) : list_(std::move(list)) {}
    void modify(PlaceCtx& ctx, RandomSource& r, const BlockPos& o, std::vector<BlockPos>& out) const override {
        list_[size_t(r.nextInt(i32(list_.size())))]->modify(ctx, r, o, out);
    }

private:
    std::vector<std::unique_ptr<PlacementModifier>> list_;
};

// ---- the feature environment (JSON -> objects, shared per registry id) --------------------------

using PlacedRef = std::shared_ptr<const PlacedFeature>;

class NoOpFeature final : public ConfiguredFeature {
public:
    explicit NoOpFeature(bool result) : result_(result) {}
    bool place(GenRegion&, RandomSource&, const BlockPos&) const override { return result_; }

private:
    bool result_;
};

class ProviderRefWrapper final : public StateProvider {  // a shared provider behind the unique_ptr interface
public:
    explicit ProviderRefWrapper(ProviderRef p) : p_(std::move(p)) {}
    u16 state(RandomSource& r, const BlockPos& p) const override { return p_->state(r, p); }
    u16 state(const GenRegion& level, RandomSource& r, const BlockPos& p) const override { return p_->state(level, r, p); }
    u16 optionalState(const GenRegion& level, RandomSource& r, const BlockPos& p) const override {
        return p_->optionalState(level, r, p);
    }

private:
    ProviderRef p_;
};

class EnvImpl final : public FeatureEnv {
public:
    EnvImpl(Shared& shared, const FeatureTypes& types, const BlockFacts& facts)
        : s_(shared), types_(types), facts_(facts) {}

    const Shared& shared() const { return s_; }
    const BlockFacts::Impl& f() const { return *s_.facts; }

    const NoiseState& noise() const override { return *s_.noise; }
    const DataPack& data() const override { return s_.noise->data(); }
    const BlockFacts& facts() const override { return facts_; }
    BlockRegistry& blocks() const override { return *s_.blocks; }

    u16 state(const json& j) const override { return s_.blocks->idFor(stateKeyOf(j)); }
    u16 defaultState(std::string_view name) const override {
        return s_.blocks->idFor(stateKeyOf(json(std::string(name))));
    }
    std::unique_ptr<IntProvider> intProvider(const json& j) const override { return parseInt(j); }
    std::unique_ptr<FloatProvider> floatProvider(const json& j) const override { return parseFloat(j); }
    std::unique_ptr<HeightProvider> heightProvider(const json& j) const override { return parseHeight(j, s_.seaLevel); }
    std::unique_ptr<StateProvider> stateProvider(const json& j) const override {
        return std::make_unique<ProviderRefWrapper>(provider(j));
    }
    std::unique_ptr<BlockPredicate> blockPredicate(const json& j) const override;
    std::unique_ptr<RuleTest> ruleTest(const json& j) const override;
    std::shared_ptr<const ConfiguredFeature> configured(const json& j) const override;
    std::shared_ptr<const PlacedFeature> placed(const json& j) const override;
    bool placePlaced(const PlacedFeature& pf, GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        return pf.place(level, r, origin, nullptr);
    }

    ProviderRef provider(const json& j) const;
    std::vector<PlacedRef> placedList(const json& j) const;  // HolderSet<PlacedFeature>
    std::vector<bool> biomeSet(const json& j) const;
    std::unique_ptr<PlacementModifier> modifier(const json& j) const;

private:
    static std::string normalized(const json& j) { return DataPack::normalize(j.get<std::string>()); }

    Shared& s_;
    const FeatureTypes& types_;
    const BlockFacts& facts_;
    mutable std::recursive_mutex mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<const ConfiguredFeature>> configured_;
    mutable std::unordered_map<std::string, PlacedRef> placed_;
    mutable std::unordered_map<std::string, ProviderRef> providers_;
    mutable std::set<std::string> unknownTypes_;
};

ProviderRef EnvImpl::provider(const json& j) const {
    if (j.is_string()) {  // a block_state_provider registry entry
        std::lock_guard lock(mutex_);
        const std::string id = normalized(j);
        if (auto it = providers_.find(id); it != providers_.end()) return it->second;
        const json* def = data().worldgen("block_state_provider", id);
        if (!def) throw std::runtime_error("missing block state provider " + id);
        ProviderRef p = provider(*def);
        providers_.emplace(id, p);
        return p;
    }
    if (!j.is_object() || !j.contains("type")) return std::make_shared<SimpleProvider>(state(j));
    const std::string type = typeOf(j);
    BlockRegistry& reg = blocks();
    if (type == "simple") return std::make_shared<SimpleProvider>(state(j.at("state")));
    if (type == "weighted") {
        Weighted<u16> list;
        for (const json& e : j.at("entries")) list.add(state(e.at("data")), e.at("weight").get<i32>());
        return std::make_shared<WeightedProvider>(std::move(list));
    }
    if (type == "random_block") {
        std::vector<u16> states;
        for (const std::string& b : f().holderList(j.at("blocks"))) states.push_back(defaultState(b));
        return std::make_shared<RandomBlockProvider>(std::move(states));
    }
    if (type == "randomized_int")
        return std::make_shared<RandomizedIntProvider>(provider(j.at("source")), j.at("property").get<std::string>(),
                                                       parseInt(j.at("values")), reg);
    if (type == "rotated")
        return std::make_shared<RotatedProvider>(provider(j.at("state")),
                                                 j.contains("direction") ? parseDirection(j["direction"]) : -1, reg);
    if (type == "rule_based") {
        std::vector<RuleBasedProvider::Rule> rules;
        for (const json& r : j.at("rules")) rules.push_back({blockPredicate(r.at("if_true")), provider(r.at("then"))});
        return std::make_shared<RuleBasedProvider>(j.contains("fallback") ? provider(j["fallback"]) : nullptr,
                                                   std::move(rules));
    }
    if (type == "copy_properties") return std::make_shared<CopyPropertiesProvider>(provider(j.at("source")), reg);
    if (type == "noise" || type == "dual_noise" || type == "noise_threshold") {
        auto p = std::make_shared<NoiseProvider>();
        LegacyRandom seedRandom(j.at("seed").get<i64>());
        p->noise = NormalNoise::fromJson(j.at("noise")).create(seedRandom);
        p->scale = jfloat(j.at("scale"));
        auto states = [&](const char* key) {
            std::vector<u16> out;
            for (const json& s : j.at(key)) out.push_back(state(s));
            return out;
        };
        if (type == "noise_threshold") {
            p->kind = NoiseProvider::Threshold;
            p->threshold = jfloat(j.at("threshold"));
            p->highChance = jfloat(j.at("high_chance"));
            p->defaultState = state(j.at("default_state"));
            p->lowStates = states("low_states");
            p->highStates = states("high_states");
        } else {
            p->states = states("states");
            if (type == "dual_noise") {
                p->kind = NoiseProvider::Dual;
                LegacyRandom slowRandom(j.at("seed").get<i64>());
                p->slowNoise = NormalNoise::fromJson(j.at("slow_noise")).create(slowRandom);
                p->slowScale = jfloat(j.at("slow_scale"));
                const json& v = j.at("variety");
                if (v.is_array()) {
                    p->varietyMin = v[0].get<int>(), p->varietyMax = v[1].get<int>();
                } else {
                    p->varietyMin = v.at("min_inclusive").get<int>(), p->varietyMax = v.at("max_inclusive").get<int>();
                }
            }
        }
        return p;
    }
    throw std::runtime_error("unknown block state provider " + type);
}

std::vector<bool> EnvImpl::biomeSet(const json& j) const {
    std::vector<std::string> names;
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '#')
            names = f().tagOrdered("worldgen/biome", s);
        else
            names.push_back(s);
    } else if (j.is_array()) {
        for (const json& e : j) names.push_back(e.get<std::string>());
    }
    std::vector<bool> set;
    for (const std::string& n : names) {
        const u16 id = s_.biomes->id(n);
        if (id >= set.size()) set.resize(size_t(id) + 1);
        set[id] = true;
    }
    return set;
}

std::unique_ptr<BlockPredicate> EnvImpl::blockPredicate(const json& j) const {
    const std::string type = typeOf(j);
    const BlockPos off = offsetOf(j);
    if (type == "true" || type == "unobstructed") return std::make_unique<TruePred>();
    if (type == "matching_blocks") return std::make_unique<BlocksPred>(f(), off, f().holderSet(j.at("blocks")));
    if (type == "matching_block_tag")
        return std::make_unique<BlocksPred>(f(), off, *f().tag(j.at("tag").get<std::string>()));
    if (type == "matching_fluids") {
        static const char* const kNames[5] = {"minecraft:empty", "minecraft:water", "minecraft:flowing_water",
                                              "minecraft:lava", "minecraft:flowing_lava"};
        std::vector<std::string> fluids;
        const json& fl = j.at("fluids");
        if (fl.is_string() && !fl.get<std::string>().empty() && fl.get<std::string>()[0] == '#')
            fluids = f().tagOrdered("fluid", fl.get<std::string>());
        else if (fl.is_string())
            fluids.push_back(fl.get<std::string>());
        else
            for (const json& e : fl) fluids.push_back(e.get<std::string>());
        u8 mask = 0;
        for (std::string name : fluids) {
            if (name.find(':') == std::string::npos) name = "minecraft:" + name;
            for (int i = 0; i < 5; ++i)
                if (name == kNames[i]) mask |= u8(1 << i);
        }
        return std::make_unique<FluidsPred>(f(), off, mask);
    }
    if (type == "matching_biomes") return std::make_unique<BiomesPred>(biomeSet(j.at("biomes")));
    if (type == "solid") return std::make_unique<FactPred>(f(), off, fact::Solid);
    if (type == "replaceable") return std::make_unique<FactPred>(f(), off, fact::Replaceable);
    if (type == "has_sturdy_face")
        return std::make_unique<FactPred>(f(), off, 1ull << (fact::SturdyShift + parseDirection(j.at("direction"))));
    if (type == "would_survive") return std::make_unique<SurvivePred>(f(), off, state(j.at("state")));
    if (type == "inside_world_bounds") return std::make_unique<InsideWorldPred>(off);
    if (type == "height_range")
        return std::make_unique<HeightRangePred>(s_, Anchor::parse(j.at("min_inclusive")),
                                                 Anchor::parse(j.at("max_inclusive")));
    if (type == "all_of" || type == "any_of") {
        std::vector<PredPtr> preds;
        for (const json& p : j.at("predicates")) preds.push_back(blockPredicate(p));
        return std::make_unique<CombinePred>(type == "all_of" ? CombinePred::All : CombinePred::Any, std::move(preds));
    }
    if (type == "not") {
        std::vector<PredPtr> preds;
        preds.push_back(blockPredicate(j.at("predicate")));
        return std::make_unique<CombinePred>(CombinePred::Not, std::move(preds));
    }
    if (type == "volume_match")
        return std::make_unique<VolumePred>(parsePos(j.at("min")), parsePos(j.at("max")), blockPredicate(j.at("match")));
    throw std::runtime_error("unknown block predicate " + type);
}

std::unique_ptr<RuleTest> EnvImpl::ruleTest(const json& j) const {
    auto t = std::make_unique<RuleImpl>();
    t->facts = &f();
    const std::string type = typeOf(j, "predicate_type");
    if (type == "always_true") {
        t->kind = RuleImpl::True;
    } else if (type == "block_match" || type == "random_block_match") {
        t->kind = type == "block_match" ? RuleImpl::Block : RuleImpl::RandomBlock;
        t->blocks.set(f().blockIndex(j.at("block").get<std::string>()));
        t->probability = j.contains("probability") ? jfloat(j["probability"]) : 0.0f;
    } else if (type == "blockstate_match" || type == "random_blockstate_match") {
        t->kind = type == "blockstate_match" ? RuleImpl::State : RuleImpl::RandomState;
        t->state = state(j.at("block_state"));
        t->probability = j.contains("probability") ? jfloat(j["probability"]) : 0.0f;
    } else if (type == "tag_match") {
        t->kind = RuleImpl::Tag;
        t->blocks = *f().tag(j.at("tag").get<std::string>());
    } else if (type == "all_of" || type == "any_of") {
        t->kind = type == "all_of" ? RuleImpl::All : RuleImpl::Any;
        for (const json& r : j.at("rules")) t->rules.push_back(ruleTest(r));
    } else if (type == "not") {
        t->kind = RuleImpl::Not;
        t->rules.push_back(ruleTest(j.at("rule")));
    } else if (type == "height_match") {
        t->kind = RuleImpl::Height;
        t->lo = j.at("min_inclusive").get<int>(), t->hi = j.at("max_inclusive").get<int>();
    } else {
        throw std::runtime_error("unknown rule test " + type);
    }
    return t;
}

std::shared_ptr<const ConfiguredFeature> EnvImpl::configured(const json& j) const {
    std::lock_guard lock(mutex_);
    std::string id;
    const json* def = &j;
    if (j.is_string()) {
        id = normalized(j);
        if (auto it = configured_.find(id); it != configured_.end()) return it->second;
        def = data().worldgen("feature", id);
        if (!def) def = data().worldgen("configured_feature", id);
        if (!def) {
            LOG("features: missing feature %s", id.c_str());
            return configured_[id] = std::make_shared<NoOpFeature>(false);
        }
    }
    std::shared_ptr<const ConfiguredFeature> result;
    const std::string type = typeOf(*def);
    if (const FeatureFactory* factory = types_.find(type)) {
        try {
            result = (*factory)(*def, *this);
        } catch (const std::exception& e) {
            LOG("features: %s (%s): %s", id.empty() ? "inline feature" : id.c_str(), type.c_str(), e.what());
        }
    } else if (unknownTypes_.insert(type).second) {
        LOG("features: no implementation of feature type %s", type.c_str());
    }
    if (!result) result = std::make_shared<NoOpFeature>(false);
    if (!id.empty()) configured_[id] = result;
    return result;
}

std::unique_ptr<PlacementModifier> EnvImpl::modifier(const json& j) const {
    const std::string type = typeOf(j);
    if (type == "count") return std::make_unique<CountMod>(parseInt(j.at("count")));
    if (type == "noise_based_count") return std::make_unique<NoiseCountMod>(s_, false, j);
    if (type == "noise_threshold_count") return std::make_unique<NoiseCountMod>(s_, true, j);
    if (type == "rarity_filter") return std::make_unique<ChanceMod>(1.0f / float(j.at("chance").get<int>()));
    if (type == "random_chance") return std::make_unique<ChanceMod>(jfloat(j.at("chance")));
    if (type == "in_square") return std::make_unique<InSquareMod>();
    if (type == "height_range") return std::make_unique<HeightRangeMod>(s_, parseHeight(j.at("height"), s_.seaLevel));
    if (type == "heightmap") return std::make_unique<HeightmapMod>(parseHeightmap(j.at("heightmap")));
    if (type == "biome") return std::make_unique<BiomeMod>(s_);
    if (type == "block_predicate_filter") return std::make_unique<PredicateMod>(blockPredicate(j.at("predicate")));
    if (type == "environment_scan")
        return std::make_unique<EnvironmentScanMod>(
            parseDirection(j.at("direction_of_search")), blockPredicate(j.at("target_condition")),
            j.contains("allowed_search_condition") ? blockPredicate(j["allowed_search_condition"])
                                                   : std::make_unique<TruePred>(),
            j.at("max_steps").get<int>());
    if (type == "surface_relative_threshold_filter")
        return std::make_unique<SurfaceRelativeMod>(parseHeightmap(j.at("heightmap")), j.value("min_inclusive", i64(INT_MIN)),
                                                    j.value("max_inclusive", i64(INT_MAX)));
    if (type == "surface_water_depth_filter") return std::make_unique<WaterDepthMod>(j.at("max_water_depth").get<int>());
    if (type == "count_on_every_layer") return std::make_unique<EveryLayerMod>(f(), parseInt(j.at("count")));
    if (type == "offset") return std::make_unique<OffsetMod>(parseInt(j.at("x")), parseInt(j.at("y")), parseInt(j.at("z")));
    if (type == "fixed_placement") {
        std::vector<BlockPos> positions;
        for (const json& p : j.at("positions")) positions.push_back(parsePos(p));
        return std::make_unique<FixedMod>(std::move(positions));
    }
    if (type == "cuboid")
        return std::make_unique<CuboidMod>(parseInt(j.at("xz_size")), parseInt(j.at("y_size")),
                                           j.value("include_edges", true), j.value("include_interior", true));
    if (type == "randomly_selected") {
        std::vector<std::unique_ptr<PlacementModifier>> list;
        for (const json& m : j.at("placements")) list.push_back(modifier(m));
        return std::make_unique<RandomlySelectedMod>(std::move(list));
    }
    throw std::runtime_error("unknown placement modifier " + type);
}

std::shared_ptr<const PlacedFeature> EnvImpl::placed(const json& j) const {
    std::lock_guard lock(mutex_);
    std::string id;
    const json* def = &j;
    if (j.is_string()) {
        id = normalized(j);
        if (auto it = placed_.find(id); it != placed_.end()) return it->second;
        def = data().worldgen("placed_feature", id);
    }
    auto pf = std::make_shared<PlacedFeature>();
    pf->id = id;
    if (!def) {
        LOG("features: missing placed feature %s", id.c_str());
        pf->feature = std::make_shared<NoOpFeature>(false);
    } else {
        pf->feature = configured(def->at("feature"));
        try {
            for (const json& m : def->at("placement")) pf->modifiers.push_back(modifier(m));
        } catch (const std::exception& e) {
            LOG("features: placed feature %s: %s", id.c_str(), e.what());
            pf->modifiers.clear();
            pf->feature = std::make_shared<NoOpFeature>(false);
        }
    }
    if (!id.empty()) placed_[id] = pf;
    return pf;
}

std::vector<PlacedRef> EnvImpl::placedList(const json& j) const {
    std::vector<PlacedRef> out;
    if (j.is_array()) {
        for (const json& e : j) out.push_back(placed(e));
    } else if (j.is_string() && !j.get<std::string>().empty() && j.get<std::string>()[0] == '#') {
        for (const std::string& id : f().tagOrdered("worldgen/placed_feature", j.get<std::string>()))
            out.push_back(placed(json(id)));
    } else {
        out.push_back(placed(j));
    }
    return out;
}

const EnvImpl& envOf(const FeatureEnv& env) {
    const EnvImpl* e = dynamic_cast<const EnvImpl*>(&env);
    if (!e) throw std::runtime_error("core features need the feature system's environment");
    return *e;
}

// ---- core feature types ------------------------------------------------------------------------

class CoreFeature : public ConfiguredFeature {
protected:
    explicit CoreFeature(const FeatureEnv& env) : e_(envOf(env)), f_(e_.f()), blocks_(e_.blocks()) {}
    u16 with(u16 s, std::string_view key, std::string_view value) const {
        return blocks_.property(s, key).empty() ? s : blocks_.withProperty(s, key, value);
    }
    bool hasProp(u16 s, std::string_view key, std::string_view value) const { return blocks_.property(s, key) == value; }

    const EnvImpl& e_;
    const BlockFacts::Impl& f_;
    BlockRegistry& blocks_;
};

class SimpleBlockF final : public CoreFeature {
public:
    SimpleBlockF(const json& j, const FeatureEnv& env) : CoreFeature(env), toPlace_(e_.provider(j.at("to_place"))) {
        for (const char* n : {"tall_grass", "large_fern", "lilac", "peony", "rose_bush", "sunflower", "tall_seagrass",
                              "small_dripleaf", "pitcher_plant", "pitcher_crop"})
            doublePlants_.set(f_.blockIndex(n));
        paleMossCarpet_ = f_.blockIndex("minecraft:pale_moss_carpet");
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        const u16 s = getOptional(*toPlace_, level, r, origin);
        if (s == kNone || !f_.canSurvive(level, s, origin)) return false;
        if (f_.in(s, doublePlants_)) {
            const BlockPos up = origin.offset(0, 1, 0);
            const u16 above = level.block(up);
            if (!f_.has(above, fact::Air) && (f_.fluidCode(s) != f_.fluidCode(above) || !f_.has(above, fact::Replaceable)))
                return false;
            // DoublePlantBlock.placeAt
            const u16 lower = waterlogged(level, origin, with(s, "half", "lower"));
            level.setBlock(origin, lower);
            level.setBlock(up, waterlogged(level, up, with(s, "half", "upper")));
        } else if (f_.is(s, paleMossCarpet_) && decorationContext().regionRandom) {
            placeMossyCarpet(level, origin, *decorationContext().regionRandom);
        } else {
            level.setBlock(origin, s);
        }
        return true;
    }

private:
    // MossyCarpetBlock.placeAt
    void placeMossyCarpet(GenRegion& level, const BlockPos& p, RandomSource& r) const {
        const u16 base = updatedCarpet(level, e_.defaultState("minecraft:pale_moss_carpet"), p, true);
        level.setBlock(p, base);
        const BlockPos up = p.offset(0, 1, 0);
        const u16 previous = level.block(up);
        const bool carpetAbove = f_.is(previous, paleMossCarpet_);
        if ((carpetAbove && hasProp(previous, "bottom", "true")) || (!carpetAbove && !f_.has(previous, fact::Replaceable)))
            return;
        u16 topper = updatedCarpet(level, with(e_.defaultState("minecraft:pale_moss_carpet"), "bottom", "false"), up, true);
        for (int d : kHorizontal)
            if (!hasProp(topper, kFaceNames[d], "none") && !r.nextBoolean()) topper = with(topper, kFaceNames[d], "none");
        bool faces = false;
        for (int d : kHorizontal) faces |= !hasProp(topper, kFaceNames[d], "none");
        if (!faces || topper == previous) return;
        level.setBlock(up, topper);
        level.setBlock(p, updatedCarpet(level, base, p, true));
    }
    // MossyCarpetBlock.getUpdatedState
    u16 updatedCarpet(const GenRegion& level, u16 state, const BlockPos& p, bool createSides) const {
        const bool bottom = hasProp(state, "bottom", "true");
        createSides |= bottom;
        u16 above = kNone, below = kNone;
        for (int d : kHorizontal) {
            const char* face = kFaceNames[d];
            std::string side = "none";
            if (f_.canAttachTo(level.block(relative(p, d)), d))
                side = createSides ? "low" : std::string(blocks_.property(state, face));
            if (side == "low") {
                if (above == kNone) above = level.block(p.offset(0, 1, 0));
                if (f_.is(above, paleMossCarpet_) && !hasProp(above, face, "none") && !hasProp(above, "bottom", "true"))
                    side = "tall";
                if (!bottom) {
                    if (below == kNone) below = level.block(p.offset(0, -1, 0));
                    if (f_.is(below, paleMossCarpet_) && hasProp(below, face, "none")) side = "none";
                }
            }
            state = with(state, face, side);
        }
        return state;
    }

    u16 waterlogged(const GenRegion& level, const BlockPos& p, u16 s) const {  // copyWaterloggedFrom
        return with(s, "waterlogged", f_.isWaterFluid(level.block(p)) ? "true" : "false");
    }
    ProviderRef toPlace_;
    BlockBits doublePlants_;
    u16 paleMossCarpet_ = 0;
};

class BlockColumnF final : public CoreFeature {
public:
    BlockColumnF(const json& j, const FeatureEnv& env) : CoreFeature(env) {
        for (const json& l : j.at("layers")) layers_.push_back({parseInt(l.at("height")), e_.provider(l.at("provider"))});
        dir_ = parseDirection(j.at("direction"));
        allowed_ = e_.blockPredicate(j.at("allowed_placement"));
        prioritizeTip_ = j.at("prioritize_tip").get<bool>();
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        std::vector<int> heights(layers_.size());
        int total = 0;
        for (size_t i = 0; i < layers_.size(); ++i) total += heights[i] = layers_[i].height->sample(r);
        if (total == 0) return false;
        BlockPos next = relative(origin, dir_);
        for (int y = 0; y < total; ++y) {
            if (!allowed_->test(level, next)) {
                truncate(heights, total, y);
                break;
            }
            next = relative(next, dir_);
        }
        BlockPos p = origin;
        for (size_t i = 0; i < layers_.size(); ++i)
            for (int y = 0; y < heights[i]; ++y) {
                level.setBlock(p, getState(*layers_[i].provider, level, r, p));
                p = relative(p, dir_);
            }
        return true;
    }

private:
    void truncate(std::vector<int>& heights, int total, int newHeight) const {
        int remove = total - newHeight;
        const int n = int(heights.size());
        const int step = prioritizeTip_ ? 1 : -1;
        for (int i = prioritizeTip_ ? 0 : n - 1; i != (prioritizeTip_ ? n : -1) && remove > 0; i += step) {
            const int take = std::min(heights[size_t(i)], remove);
            remove -= take;
            heights[size_t(i)] -= take;
        }
    }
    struct Layer {
        std::unique_ptr<IntProvider> height;
        ProviderRef provider;
    };
    std::vector<Layer> layers_;
    int dir_ = Up;
    PredPtr allowed_;
    bool prioritizeTip_ = false;
};

class BlockPileF final : public CoreFeature {
public:
    BlockPileF(const json& j, const FeatureEnv& env) : CoreFeature(env), provider_(e_.provider(j.at("state_provider"))) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        if (origin.y < level.minY() + 5) return false;
        const int xr = 2 + r.nextInt(2);
        const int zr = 2 + r.nextInt(2);
        for (int z = -zr; z <= zr; ++z)
            for (int y = 0; y <= 1; ++y)
                for (int x = -xr; x <= xr; ++x) {
                    const BlockPos p = origin.offset(x, y, z);
                    const int d = x * x + z * z;
                    const float a = r.nextFloat() * 10.0f;
                    if (float(d) <= a - r.nextFloat() * 6.0f) {
                        tryPlace(level, p, r);
                    } else if (double(r.nextFloat()) < 0.031) {
                        tryPlace(level, p, r);
                    }
                }
        return true;
    }

private:
    void tryPlace(GenRegion& level, const BlockPos& p, RandomSource& r) const {
        if (!level.isEmpty(p)) return;
        const u16 below = level.block(p.offset(0, -1, 0));
        const bool mayPlace = f_.is(below, f_.bDirtPath) ? r.nextBoolean() : f_.sturdy(below, Up);
        if (mayPlace) level.setBlock(p, getState(*provider_, level, r, p));
    }
    ProviderRef provider_;
};

class DiskF final : public CoreFeature {
public:
    DiskF(const json& j, const FeatureEnv& env)
        : CoreFeature(env), provider_(e_.provider(j.at("state_provider"))), target_(e_.blockPredicate(j.at("target"))),
          radius_(parseInt(j.at("radius"))), halfHeight_(j.at("half_height").get<int>()) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        bool placedAny = false;
        const int top = origin.y + halfHeight_, bottom = origin.y - halfHeight_ - 1;
        const int rad = radius_->sample(r);
        for (int z = -rad; z <= rad; ++z)
            for (int x = -rad; x <= rad; ++x)
                if (x * x + z * z <= rad * rad) placedAny |= column(level, r, top, bottom, origin.x + x, origin.z + z);
        return placedAny;
    }

private:
    bool column(GenRegion& level, RandomSource& r, int top, int bottom, int x, int z) const {
        bool placedAny = false;
        for (int y = top; y > bottom; --y) {
            const BlockPos p{x, y, z};
            if (target_->test(level, p)) {
                const u16 s = getOptional(*provider_, level, r, p);
                if (s != kNone) {
                    level.setBlock(p, s);
                    placedAny = true;
                }
            }
        }
        return placedAny;
    }
    ProviderRef provider_;
    PredPtr target_;
    std::unique_ptr<IntProvider> radius_;
    int halfHeight_;
};

class FillLayerF final : public CoreFeature {
public:
    FillLayerF(const json& j, const FeatureEnv& env)
        : CoreFeature(env), height_(j.at("height").get<int>()), state_(e_.state(j.at("state"))) {}
    bool place(GenRegion& level, RandomSource&, const BlockPos& origin) const override {
        for (int dx = 0; dx < 16; ++dx)
            for (int dz = 0; dz < 16; ++dz) {
                const BlockPos p{origin.x + dx, level.minY() + height_, origin.z + dz};
                if (f_.has(level.block(p), fact::Air)) level.setBlock(p, state_);
            }
        return true;
    }

private:
    int height_;
    u16 state_;
};

class FreezeTopLayerF final : public CoreFeature {
public:
    explicit FreezeTopLayerF(const FeatureEnv& env)
        : CoreFeature(env), ice_(e_.defaultState("minecraft:ice")), snow_(e_.defaultState("minecraft:snow")) {}
    bool place(GenRegion& level, RandomSource&, const BlockPos& origin) const override {
        const Shared& s = e_.shared();
        for (int dx = 0; dx < 16; ++dx)
            for (int dz = 0; dz < 16; ++dz) {
                const int x = origin.x + dx, z = origin.z + dz;
                const BlockPos top{x, level.height(Heightmap::MotionBlocking, x, z), z};
                const BlockPos below = top.offset(0, -1, 0);
                const u16 biome = level.biome(top);
                if (shouldFreeze(level, s, biome, below)) level.setBlock(below, ice_);
                if (shouldSnow(level, s, biome, top)) {
                    level.setBlock(top, snow_);
                    const u16 belowState = level.block(below);
                    if (!blocks_.property(belowState, "snowy").empty())
                        level.setBlock(below, blocks_.withProperty(belowState, "snowy", "true"));
                }
            }
        return true;
    }

private:
    // Biome.shouldFreeze(level, pos, false); block light is 0 while decorating.
    bool shouldFreeze(const GenRegion& level, const Shared& s, u16 biome, const BlockPos& p) const {
        if (s.temperature(biome, p) >= 0.15f) return false;
        if (p.y < level.minY() || p.y >= level.maxY()) return false;
        const u16 st = level.block(p);
        return f_.is(st, f_.bWater) && f_.fluidType(st) == FluidWater;
    }
    bool shouldSnow(const GenRegion& level, const Shared& s, u16 biome, const BlockPos& p) const {
        const bool precipitation = biome < s.climate.size() ? s.climate[biome].precipitation : true;
        if (!precipitation || s.temperature(biome, p) >= 0.15f) return false;
        if (p.y < level.minY() || p.y >= level.maxY()) return false;
        const u16 st = level.block(p);
        return (f_.has(st, fact::Air) || f_.is(st, f_.bSnow)) && f_.canSurvive(level, snow_, p);
    }
    u16 ice_, snow_;
};

class ConstantF final : public CoreFeature {  // no_op
public:
    explicit ConstantF(const FeatureEnv& env) : CoreFeature(env) {}
    bool place(GenRegion&, RandomSource&, const BlockPos&) const override { return true; }
};

class OverlayF final : public CoreFeature {
public:
    OverlayF(const json& j, const FeatureEnv& env, bool sequence)
        : CoreFeature(env), features_(e_.placedList(j.at("features"))), sequence_(sequence) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        bool placedAny = false;
        for (const PlacedRef& pf : features_) {
            const bool placed = pf->place(level, r, origin, nullptr);
            if (sequence_ && !placed) return false;
            placedAny |= placed;
        }
        return sequence_ ? true : placedAny;
    }

private:
    std::vector<PlacedRef> features_;
    bool sequence_;
};

class ProjectedSquareF final : public CoreFeature {  // projected_random_patchy_square
public:
    ProjectedSquareF(const json& j, const FeatureEnv& env)
        : CoreFeature(env), block_(e_.provider(j.at("block"))), through_(e_.blockPredicate(j.at("project_through"))),
          size_(parseInt(j.at("size"))), maxDrop_(j.at("max_projection_height").get<int>()) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        const int size = size_->sample(r);
        const int bound = size * size + 1;
        for (int dx = -size; dx <= size; ++dx)
            for (int dz = -size; dz <= size; ++dz) {
                const int probability = std::abs(dx) * std::abs(dz);
                if (r.nextInt(bound) >= bound - probability) continue;
                BlockPos base = origin.offset(dx, 0, dz);
                int drop = maxDrop_;
                while (through_->test(level, base.offset(0, -1, 0))) {
                    base = base.offset(0, -1, 0);
                    if (--drop <= 0) break;
                }
                const u16 s = getOptional(*block_, level, r, base);
                if (s != kNone) level.setBlock(base, s);
            }
        return true;
    }

private:
    ProviderRef block_;
    PredPtr through_;
    std::unique_ptr<IntProvider> size_;
    int maxDrop_;
};

class RandomBooleanF final : public CoreFeature {
public:
    RandomBooleanF(const json& j, const FeatureEnv& env)
        : CoreFeature(env), yes_(e_.placed(j.at("feature_true"))), no_(e_.placed(j.at("feature_false"))) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        return (r.nextBoolean() ? yes_ : no_)->place(level, r, origin, nullptr);
    }

private:
    PlacedRef yes_, no_;
};

class NeighborSpreadF final : public CoreFeature {  // random_neighbor_spread
public:
    NeighborSpreadF(const json& j, const FeatureEnv& env)
        : CoreFeature(env), block_(e_.provider(j.at("block"))), accepted_(f_.holderSet(j.at("accepted_neighbors"))),
          canReplace_(e_.blockPredicate(j.at("can_replace"))), attempts_(parseInt(j.at("attempts"))),
          xz_(parseInt(j.at("xz_offset"))), y_(parseInt(j.at("y_offset"))) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        level.setBlock(origin, getState(*block_, level, r, origin));
        const int attempts = attempts_->sample(r);
        for (int i = 0; i < attempts; ++i) {
            const int dx = xz_->sample(r);
            const int dy = y_->sample(r);
            const int dz = xz_->sample(r);
            const BlockPos p = origin.offset(dx, dy, dz);
            if (!canReplace_->test(level, p)) continue;
            int neighbours = 0;
            for (int d = 0; d < 6; ++d) {
                if (f_.in(level.block(relative(p, d)), accepted_)) ++neighbours;
                if (neighbours > 1) break;
            }
            if (neighbours == 1) level.setBlock(p, getState(*block_, level, r, p));
        }
        return true;
    }

private:
    ProviderRef block_;
    BlockBits accepted_;
    PredPtr canReplace_;
    std::unique_ptr<IntProvider> attempts_, xz_, y_;
};

class RandomSelectorF final : public CoreFeature {
public:
    RandomSelectorF(const json& j, const FeatureEnv& env) : CoreFeature(env), default_(e_.placed(j.at("default"))) {
        for (const json& e : j.at("features")) features_.push_back({e_.placed(e.at("feature")), jfloat(e.at("chance"))});
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        for (const auto& [feature, chance] : features_)
            if (r.nextFloat() < chance) return feature->place(level, r, origin, nullptr);
        return default_->place(level, r, origin, nullptr);
    }

private:
    std::vector<std::pair<PlacedRef, float>> features_;
    PlacedRef default_;
};

class SimpleRandomSelectorF final : public CoreFeature {
public:
    SimpleRandomSelectorF(const json& j, const FeatureEnv& env) : CoreFeature(env), features_(e_.placedList(j.at("features"))) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        return features_[size_t(r.nextInt(i32(features_.size())))]->place(level, r, origin, nullptr);
    }

private:
    std::vector<PlacedRef> features_;
};

class WeightedSelectorF final : public CoreFeature {
public:
    WeightedSelectorF(const json& j, const FeatureEnv& env) : CoreFeature(env) {
        for (const json& e : j.at("features")) list_.add(e_.placed(e.at("data")), e.at("weight").get<i32>());
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        const PlacedRef* pf = list_.pick(r);
        return pf && (*pf)->place(level, r, origin, nullptr);
    }

private:
    Weighted<PlacedRef> list_;
};

class ReplaceSingleF final : public CoreFeature {
public:
    ReplaceSingleF(const json& j, const FeatureEnv& env) : CoreFeature(env) {
        for (const json& t : j.at("targets")) targets_.push_back({e_.ruleTest(t.at("target")), e_.state(t.at("state"))});
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        const u16 current = level.block(origin);
        for (const auto& [target, state] : targets_)
            if (target->test(current, origin, r)) {
                level.setBlock(origin, state);
                break;
            }
        return true;
    }

private:
    std::vector<std::pair<RulePtr, u16>> targets_;
};

class PillarF final : public CoreFeature {  // single_block_pillar
public:
    PillarF(const json& j, const FeatureEnv& env)
        : CoreFeature(env), block_(e_.provider(j.at("block"))),
          canReplace_(j.contains("can_replace") ? e_.blockPredicate(j["can_replace"]) : std::make_unique<TruePred>()),
          dir_(parseDirection(j.at("direction"))), chance_(j.contains("chance_to_continue") ? jfloat(j["chance_to_continue"]) : 1.0f),
          cap_(j.contains("cap_feature") ? e_.placed(j["cap_feature"]) : nullptr) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        BlockPos p = origin;
        while (canReplace_->test(level, p) && r.nextFloat() < chance_ && p.y >= level.minY() && p.y < level.maxY()) {
            level.setBlock(p, getState(*block_, level, r, p));
            p = relative(p, dir_);
        }
        p = relative(p, dir_ ^ 1);
        if (cap_) cap_->place(level, r, p, nullptr);
        return true;
    }

private:
    ProviderRef block_;
    PredPtr canReplace_;
    int dir_;
    float chance_;
    PlacedRef cap_;
};

class VinesF final : public CoreFeature {
public:
    explicit VinesF(const FeatureEnv& env) : CoreFeature(env), vine_(e_.defaultState("minecraft:vine")) {}
    bool place(GenRegion& level, RandomSource&, const BlockPos& origin) const override {
        if (!level.isEmpty(origin)) return false;
        for (int d = 0; d < 6; ++d)
            if (d != Down && f_.canAttachTo(level.block(relative(origin, d)), d)) {
                level.setBlock(origin, blocks_.withProperty(vine_, kFaceNames[d], "true"));
                return true;
            }
        return false;
    }

private:
    u16 vine_;
};

class VoidStartPlatformF final : public CoreFeature {
public:
    explicit VoidStartPlatformF(const FeatureEnv& env)
        : CoreFeature(env), stone_(e_.defaultState("minecraft:stone")), cobble_(e_.defaultState("minecraft:cobblestone")) {}
    bool place(GenRegion& level, RandomSource&, const BlockPos& origin) const override {
        const int cx = origin.x >> 4, cz = origin.z >> 4;  // platform origin (8, 3, 8) is in chunk 0, 0
        if (std::max(std::abs(cx), std::abs(cz)) > 1) return true;
        const BlockPos platform{8, origin.y + 3, 8};
        for (int z = cz * 16; z <= cz * 16 + 15; ++z)
            for (int x = cx * 16; x <= cx * 16 + 15; ++x)
                if (std::max(std::abs(platform.x - x), std::abs(platform.z - z)) <= 16) {
                    const BlockPos p{x, platform.y, z};
                    level.setBlock(p, p == platform ? cobble_ : stone_);
                }
        return true;
    }

private:
    u16 stone_, cobble_;
};

class BlockBlobF final : public CoreFeature {
public:
    BlockBlobF(const json& j, const FeatureEnv& env)
        : CoreFeature(env), state_(e_.state(j.at("state"))), canPlaceOn_(e_.blockPredicate(j.at("can_place_on"))) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& start) const override {
        BlockPos origin = start;
        while (origin.y > level.minY() + 3 && !canPlaceOn_->test(level, origin.offset(0, -1, 0))) origin.y--;
        if (origin.y <= level.minY() + 3) return false;
        for (int c = 0; c < 3; ++c) {
            const int xr = r.nextInt(2);
            const int yr = r.nextInt(2);
            const int zr = r.nextInt(2);
            const float tr = float(xr + yr + zr) * 0.333f + 0.5f;
            for (int z = -zr; z <= zr; ++z)
                for (int y = -yr; y <= yr; ++y)
                    for (int x = -xr; x <= xr; ++x)
                        if (double(x * x + y * y + z * z) <= double(tr * tr)) level.setBlock(origin.offset(x, y, z), state_);
            const int dx = -1 + r.nextInt(2);
            const int dy = -r.nextInt(2);
            const int dz = -1 + r.nextInt(2);
            origin = origin.offset(dx, dy, dz);
        }
        return true;
    }

private:
    u16 state_;
    PredPtr canPlaceOn_;
};

class UnderwaterMagmaF final : public CoreFeature {
public:
    UnderwaterMagmaF(const json& j, const FeatureEnv& env)
        : CoreFeature(env), range_(j.at("floor_search_range").get<int>()),
          radius_(j.at("placement_radius_around_floor").get<int>()),
          probability_(jfloat(j.at("placement_probability_per_valid_position"))),
          magma_(e_.defaultState("minecraft:magma_block")) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        // Column.scan(level, origin, range, water, not water).getFloor()
        auto water = [&](int y) { return f_.is(level.block(origin.x, y, origin.z), f_.bWater); };
        if (!water(origin.y)) return false;
        int y = origin.y;
        for (int i = 1; i < range_ && water(y); ++i) --y;
        if (water(y)) return false;  // no floor found
        const BlockPos floor{origin.x, y, origin.z};
        int placed = 0;
        for (int z = -radius_; z <= radius_; ++z)
            for (int yy = -radius_; yy <= radius_; ++yy)
                for (int x = -radius_; x <= radius_; ++x) {
                    const BlockPos p = floor.offset(x, yy, z);
                    if (!(r.nextFloat() < probability_)) continue;
                    if (!valid(level, p)) continue;
                    level.setBlock(p, magma_);
                    ++placed;
                }
        return placed > 0;
    }

private:
    bool visible(const GenRegion& level, const BlockPos& p, int covered) const {
        return !f_.occlusionFull(level.block(p), covered);
    }
    bool valid(const GenRegion& level, const BlockPos& p) const {
        const u16 s = level.block(p);
        if (f_.is(s, f_.bWater) || f_.has(s, fact::Air) || visible(level, p.offset(0, -1, 0), Up)) return false;
        for (int d : kHorizontal)
            if (visible(level, relative(p, d), d ^ 1)) return false;
        return true;
    }
    int range_, radius_;
    float probability_;
    u16 magma_;
};

class MultifaceF final : public CoreFeature {  // multiface_growth with MultifaceSpreader
public:
    MultifaceF(const json& j, const FeatureEnv& env) : CoreFeature(env) {
        const std::string name = j.at("block").get<std::string>();
        block_ = f_.blockIndex(name);
        default_ = e_.defaultState(name);
        sculk_ = DataPack::normalize(name) == "sculk_vein";
        searchRange_ = j.value("search_range", 10);
        chance_ = j.contains("chance_of_spreading") ? jfloat(j["chance_of_spreading"]) : 0.5f;
        placeOn_ = f_.holderSet(j.at("can_be_placed_on"));
        if (j.value("can_place_on_ceiling", false)) valid_.push_back(Up);
        if (j.value("can_place_on_floor", false)) valid_.push_back(Down);
        if (j.value("can_place_on_wall", false))
            for (int d : kHorizontal) valid_.push_back(d);
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        if (!airOrWater(level.block(origin))) return false;
        std::vector<int> dirs = valid_;
        shuffle(dirs, r);
        if (placeGrowth(level, origin, level.block(origin), r, dirs)) return true;
        for (int search : dirs) {
            std::vector<int> placement;
            for (int d : valid_)
                if (d != (search ^ 1)) placement.push_back(d);
            shuffle(placement, r);
            for (int i = 0; i < searchRange_; ++i) {
                const BlockPos p = relative(origin, search);  // the game never moves past the first step
                const u16 s = level.block(p);
                if (!airOrWater(s) && !f_.is(s, block_)) break;
                if (placeGrowth(level, p, s, r, placement)) return true;
            }
        }
        return false;
    }

private:
    bool airOrWater(u16 s) const { return f_.has(s, fact::Air) || f_.is(s, f_.bWater); }
    bool hasFace(u16 s, int d) const { return f_.is(s, block_) && hasProp(s, kFaceNames[d], "true"); }

    bool validForPlacement(const GenRegion& level, u16 old, const BlockPos& p, int d) const {
        if (hasFace(old, d)) return false;
        return f_.canAttachTo(level.block(relative(p, d)), d);
    }
    u16 stateForPlacement(const GenRegion& level, u16 old, const BlockPos& p, int d) const {
        if (!validForPlacement(level, old, p, d)) return kNone;
        u16 s;
        if (f_.is(old, block_)) s = old;
        else if (f_.fluidType(old) == FluidWater) s = with(default_, "waterlogged", "true");
        else s = default_;
        return with(s, kFaceNames[d], "true");
    }

    bool placeGrowth(GenRegion& level, const BlockPos& p, u16 old, RandomSource& r, const std::vector<int>& dirs) const {
        for (int d : dirs) {
            if (!f_.in(level.block(relative(p, d)), placeOn_)) continue;
            const u16 s = stateForPlacement(level, old, p, d);
            if (s == kNone) return false;
            level.setBlock(p, s);
            if (r.nextFloat() < chance_) spread(level, s, p, d, r);
            return true;
        }
        return false;
    }

    // MultifaceSpreader.spreadFromFaceTowardRandomDirection (SAME_POSITION, SAME_PLANE, WRAP_AROUND).
    void spread(GenRegion& level, u16 state, const BlockPos& p, int from, RandomSource& r) const {
        std::vector<int> dirs{0, 1, 2, 3, 4, 5};
        shuffle(dirs, r);
        for (int d : dirs) {
            if (d / 2 == from / 2) continue;
            if (!hasFace(state, from) || hasFace(state, d)) continue;
            const std::pair<BlockPos, int> candidates[3] = {
                {p, d}, {relative(p, d), from}, {relative(relative(p, d), from), d ^ 1}};
            for (const auto& [sp, face] : candidates) {
                if (!canSpreadInto(level, p, sp, face)) continue;
                const u16 placed = stateForPlacement(level, level.block(sp), sp, face);
                if (placed != kNone && level.setBlock(sp, placed)) return;
                break;  // the first spread position decides this direction
            }
        }
    }

    bool canSpreadInto(const GenRegion& level, const BlockPos& source, const BlockPos& p, int face) const {
        const u16 existing = level.block(p);
        return replaceable(level, source, p, face, existing) && validForPlacement(level, existing, p, face);
    }
    bool replaceable(const GenRegion& level, const BlockPos& source, const BlockPos& p, int face, u16 existing) const {
        const bool base = f_.has(existing, fact::Air) || f_.is(existing, block_) ||
                          (f_.is(existing, f_.bWater) && f_.fluidType(existing) == FluidWater);
        if (!sculk_) return base;
        const u16 against = level.block(relative(p, face));
        if (f_.is(against, f_.bSculk) || f_.is(against, f_.bSculkCatalyst) || f_.is(against, f_.bMovingPiston))
            return false;
        if (std::abs(source.x - p.x) + std::abs(source.y - p.y) + std::abs(source.z - p.z) == 2) {
            const BlockPos neighbour = relative(source, face ^ 1);
            if (f_.sturdy(level.block(neighbour), face)) return false;
        }
        const FluidType fluid = f_.fluidType(existing);
        if (fluid != FluidNone && fluid != FluidWater) return false;
        if (f_.in(existing, *f_.tagFire)) return false;
        return f_.has(existing, fact::Replaceable) || base;
    }

    u16 block_ = 0, default_ = 0;
    bool sculk_ = false;
    int searchRange_ = 10;
    float chance_ = 0.5f;
    BlockBits placeOn_;
    std::vector<int> valid_;  // validDirections: up, down, then north, east, south, west
};

template <class T>
FeatureFactory factoryOf() {
    return [](const json& j, const FeatureEnv& env) -> std::unique_ptr<ConfiguredFeature> {
        return std::make_unique<T>(j, env);
    };
}

}  // namespace

void registerCoreFeatures(FeatureTypes& types) {
    types.add("minecraft:simple_block", factoryOf<SimpleBlockF>());
    types.add("minecraft:block_column", factoryOf<BlockColumnF>());
    types.add("minecraft:block_pile", factoryOf<BlockPileF>());
    types.add("minecraft:disk", factoryOf<DiskF>());
    types.add("minecraft:fill_layer", factoryOf<FillLayerF>());
    types.add("minecraft:freeze_top_layer",
              [](const json&, const FeatureEnv& env) { return std::make_unique<FreezeTopLayerF>(env); });
    types.add("minecraft:no_op", [](const json&, const FeatureEnv& env) { return std::make_unique<ConstantF>(env); });
    types.add("minecraft:overlay",
              [](const json& j, const FeatureEnv& env) { return std::make_unique<OverlayF>(j, env, false); });
    types.add("minecraft:sequence",
              [](const json& j, const FeatureEnv& env) { return std::make_unique<OverlayF>(j, env, true); });
    types.add("minecraft:projected_random_patchy_square", factoryOf<ProjectedSquareF>());
    types.add("minecraft:random_boolean_selector", factoryOf<RandomBooleanF>());
    types.add("minecraft:random_neighbor_spread", factoryOf<NeighborSpreadF>());
    types.add("minecraft:random_selector", factoryOf<RandomSelectorF>());
    types.add("minecraft:simple_random_selector", factoryOf<SimpleRandomSelectorF>());
    types.add("minecraft:weighted_random_selector", factoryOf<WeightedSelectorF>());
    types.add("minecraft:replace_single_block", factoryOf<ReplaceSingleF>());
    types.add("minecraft:single_block_pillar", factoryOf<PillarF>());
    types.add("minecraft:vines", [](const json&, const FeatureEnv& env) { return std::make_unique<VinesF>(env); });
    types.add("minecraft:void_start_platform",
              [](const json&, const FeatureEnv& env) { return std::make_unique<VoidStartPlatformF>(env); });
    types.add("minecraft:block_blob", factoryOf<BlockBlobF>());
    types.add("minecraft:underwater_magma", factoryOf<UnderwaterMagmaF>());
    types.add("minecraft:multiface_growth", factoryOf<MultifaceF>());
}

// ---- FeatureSystem ----------------------------------------------------------------------------------

struct FeatureSystem::Impl {
    Impl(const NoiseState& n, BlockRegistry& b, BiomeTable& t) : noise(n), blocks(b), biomes(t), facts(b, n.data()) {}

    const NoiseState& noise;
    BlockRegistry& blocks;
    BiomeTable& biomes;
    BlockFacts facts;
    FeatureTypes types;
    Shared shared;
    std::unique_ptr<EnvImpl> env;

    struct Step {
        std::vector<const PlacedFeature*> features;  // FeatureSorter.StepFeatureData
        std::unordered_map<const PlacedFeature*, int> index;
    };
    std::vector<Step> steps;
    std::vector<std::vector<std::vector<PlacedRef>>> biomeSteps;  // by biome id
    std::vector<bool> possible;                                   // by biome id
    const StructureSystem* structures = nullptr;

    void buildFeatureOrder(const std::vector<u16>& sources);
};

// FeatureSorter.buildFeaturesPerStep with Graph.depthFirstSearch: a global order per step that respects
// every biome's own feature order.
void FeatureSystem::Impl::buildFeatureOrder(const std::vector<u16>& sources) {
    struct Node {
        int step, index;
        bool operator<(const Node& o) const { return step != o.step ? step < o.step : index < o.index; }
    };
    std::unordered_map<const PlacedFeature*, int> featureIndex;
    std::map<Node, const PlacedFeature*> nodeFeature;
    std::map<Node, std::set<Node>> edges;
    size_t maxStep = 0;
    for (u16 biome : sources) {
        const auto& lists = biomeSteps[biome];
        maxStep = std::max(maxStep, lists.size());
        std::vector<Node> list;
        for (size_t step = 0; step < lists.size(); ++step)
            for (const PlacedRef& pf : lists[step]) {
                const int index = featureIndex.emplace(pf.get(), int(featureIndex.size())).first->second;
                const Node node{int(step), index};
                nodeFeature[node] = pf.get();
                list.push_back(node);
            }
        for (size_t i = 0; i < list.size(); ++i) {
            auto& next = edges[list[i]];
            if (i + 1 < list.size()) next.insert(list[i + 1]);
        }
    }
    std::set<Node> discovered, visiting;
    std::vector<Node> sorted;
    auto dfs = [&](auto&& self, const Node& n) -> bool {
        if (discovered.count(n)) return false;
        if (visiting.count(n)) return true;
        visiting.insert(n);
        if (auto it = edges.find(n); it != edges.end())
            for (const Node& next : it->second)
                if (self(self, next)) return true;
        visiting.erase(n);
        discovered.insert(n);
        sorted.push_back(n);
        return false;
    };
    for (const auto& [node, next] : edges)
        if (!discovered.count(node) && dfs(dfs, node)) {
            LOG("features: feature order cycle found");
            break;
        }
    std::reverse(sorted.begin(), sorted.end());
    steps.assign(maxStep, {});
    for (const Node& n : sorted) {
        Step& s = steps[size_t(n.step)];
        s.index.emplace(nodeFeature[n], int(s.features.size()));
        s.features.push_back(nodeFeature[n]);
    }
}

FeatureSystem::FeatureSystem(const NoiseState& noise, BlockRegistry& blocks, BiomeTable& biomes,
                             const std::vector<u16>& possibleBiomes)
    : impl_(std::make_unique<Impl>(noise, blocks, biomes)) {
    Timer t;
    Impl& im = *impl_;
    const DataPack& data = noise.data();
    registerCoreFeatures(im.types);
    registerTreeFeatures(im.types);
    registerMiscFeatures(im.types);

    Shared& s = im.shared;
    s.noise = &noise;
    s.blocks = &blocks;
    s.biomes = &biomes;
    s.facts = &im.facts.impl();
    if (const json* settings = data.worldgen("noise_settings", "overworld")) {
        s.seaLevel = settings->value("sea_level", 63);
        if (settings->contains("noise")) {
            s.genMinY = (*settings)["noise"].value("min_y", -64);
            s.genDepth = (*settings)["noise"].value("height", 384);
        }
    }
    LegacyRandom temperatureRandom(1234), frozenRandom(3456), biomeInfoRandom(2345);
    s.temperatureNoise.init(temperatureRandom);
    for (Simplex2D& n : s.frozenNoise) n.init(frozenRandom);
    s.biomeInfoNoise.init(biomeInfoRandom);
    im.env = std::make_unique<EnvImpl>(s, im.types, im.facts);

    // Each possible biome's feature lists (BiomeGenerationSettings.features).
    const size_t biomeCount = std::max<size_t>(biomes.size(), 1);
    im.biomeSteps.assign(biomeCount, {});
    im.possible.assign(biomeCount, false);
    s.biomeFeatures.assign(biomeCount, {});
    s.climate.assign(biomeCount, {});
    for (u16 b : possibleBiomes) {
        im.possible[b] = true;
        const json* def = data.worldgen("biome", biomes.name(b));
        if (!def) {
            LOG("features: biome %s missing from the game data", biomes.name(b).c_str());
            continue;
        }
        s.climate[b].temperature = float(def->value("temperature", 0.5));
        s.climate[b].precipitation = def->value("has_precipitation", true);
        s.climate[b].frozen = def->value("temperature_modifier", std::string("none")) == "frozen";
        if (const auto it = def->find("features"); it != def->end())
            for (const json& step : *it) {
                im.biomeSteps[b].push_back(im.env->placedList(step));
                for (const PlacedRef& pf : im.biomeSteps[b].back()) s.biomeFeatures[b].insert(pf.get());
            }
    }
    im.buildFeatureOrder(possibleBiomes);
    size_t total = 0;
    for (const auto& step : im.steps) total += step.features.size();
    LOG("features: %zu placed features in %zu steps ready in %.1f ms", total, im.steps.size(), t.ms());
}

FeatureSystem::~FeatureSystem() = default;

void FeatureSystem::setStructures(const StructureSystem* structures) { impl_->structures = structures; }
const StructureSystem* FeatureSystem::structures() const { return impl_->structures; }
const FeatureEnv& FeatureSystem::env() const { return *impl_->env; }
float FeatureSystem::temperature(u16 biome, const BlockPos& p) const { return impl_->shared.temperature(biome, p); }

// ChunkGenerator.applyBiomeDecoration: each step places its structures (when a StructureSystem is set; their
// per-step indices are separate from the features' global indices), then its features.
void FeatureSystem::decorate(const std::array<GenChunk*, 9>& chunks, const BiomeZoom& zoom,
                             const NoiseBiomeFn& noiseBiome) const {
    const Impl& im = *impl_;
    const GenChunk& centre = *chunks[4];
    GenRegion level(chunks, im.facts, zoom, noiseBiome, im.noise.seed(), im.shared.seaLevel);
    DecorationContext& context = decorationContext();
    const DecorationContext saved = context;
    context.level = &level;
    const auto regionRandom = im.noise.randomFactory("minecraft:worldgen_region_random")->at(centre.cx * 16, 0, centre.cz * 16);
    context.regionRandom = regionRandom.get();

    const BlockPos origin{centre.cx * 16, (centre.minY >> 4) * 16, centre.cz * 16};  // SectionPos origin
    WorldgenRandom random(std::make_unique<XoroshiroRandom>(i64(0)));
    const i64 decorationSeed = random.setDecorationSeed(im.noise.seed(), origin.x, origin.z);

    std::vector<bool> present(im.possible.size(), false);
    for (const GenChunk* c : chunks)
        if (c)
            for (u16 b : c->biomes)
                if (b < present.size() && im.possible[b]) present[b] = true;

    const size_t stepCount = std::max<size_t>(11, im.steps.size());  // GenerationStep.Decoration values
    std::vector<int> indices;
    for (size_t step = 0; step < stepCount; ++step) {
        if (im.structures) im.structures->placeStep(int(step), level, centre.cx, centre.cz, random, decorationSeed);
        if (step >= im.steps.size()) continue;
        const Impl::Step& data = im.steps[step];
        indices.clear();
        for (size_t b = 0; b < present.size(); ++b) {
            if (!present[b] || step >= im.biomeSteps[b].size()) continue;
            for (const PlacedRef& pf : im.biomeSteps[b][step]) indices.push_back(data.index.at(pf.get()));
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        for (int index : indices) {
            random.setFeatureSeed(decorationSeed, index, int(step));
            const PlacedFeature* feature = data.features[size_t(index)];
            static const bool profile = std::getenv("MCPP_DECORATE_PROFILE") != nullptr;
            const double t0 = profile ? nowSeconds() : 0.0;
            feature->place(level, random, origin, feature);
            if (profile && nowSeconds() - t0 > 0.005)
                LOG("decorate %d %d: %s took %.1f ms", centre.cx, centre.cz, feature->id.c_str(), (nowSeconds() - t0) * 1000.0);
        }
    }
    context = saved;
}

}  // namespace wg
