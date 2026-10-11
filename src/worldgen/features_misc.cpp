// Biome decoration features other than the framework's simple ones and the trees: ores, geodes,
// speleothems (dripstone / sulfur spikes), vegetation patches, root systems, sculk, lakes, springs,
// icebergs, coral, dungeons, fossils and the other structure-template features, the nether column /
// blob features and the End features. Ports of the game's levelgen/feature classes: random calls,
// iteration orders and writes follow the Java code exactly. Block entities and entities are not
// created (their random draws, e.g. loot seeds, still are) and scheduled ticks / post-processing marks
// are not recorded.
#include "worldgen/feature_api.h"
#include "worldgen/java_pos_set.h"

#include "assets/assets.h"

#include <libdeflate.h>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace wg {
namespace {

using json = nlohmann::json;

constexpr float kPiF = 3.1415927f;  // (float) Math.PI
constexpr double kPi = 3.141592653589793;
constexpr const char* kDirName[6] = {"down", "up", "north", "south", "west", "east"};
constexpr Direction kAllDirs[6] = {Down, Up, North, South, West, East};  // Direction.values()
constexpr Direction kHorizontal[4] = {North, East, South, West};         // Direction.Plane.HORIZONTAL

// ---- Java semantics ------------------------------------------------------------------------
inline i32 d2i(double v) {  // (int) of a double / float
    if (v != v) return 0;
    if (v >= 2147483647.0) return 2147483647;
    if (v <= -2147483648.0) return -2147483647 - 1;
    return i32(v);
}
inline i32 jceil(double v) { return d2i(std::ceil(v)); }                     // Mth.ceil
inline i32 jround(float v) { return d2i(std::floor(double(v) + 0.5)); }      // Math.round(float)
inline i32 iabs(i32 v) { return v < 0 ? -v : v; }

// Mth.sin / Mth.cos: 65536-entry table.
struct SinTable {
    float v[65536];
    SinTable() {
        for (int i = 0; i < 65536; ++i) v[i] = float(std::sin(i / 10430.378350470453));
    }
};
const SinTable& sinTable() {
    static const SinTable t;
    return t;
}
inline float msin(double v) { return sinTable().v[i64(v * 10430.378350470453) & 65535]; }
inline float mcos(double v) { return sinTable().v[i64(v * 10430.378350470453 + 16384.0) & 65535]; }

// ---- directions and positions ----------------------------------------------------------------
inline Direction opposite(Direction d) { return Direction(d ^ 1); }
inline Direction clockWise(Direction d) {
    switch (d) {
    case North: return East;
    case East: return South;
    case South: return West;
    case West: return North;
    default: return d;
    }
}
inline Direction counterClockWise(Direction d) {
    switch (d) {
    case North: return West;
    case West: return South;
    case South: return East;
    case East: return North;
    default: return d;
    }
}
inline BlockPos rel(const BlockPos& p, Direction d, int n = 1) {
    return p.offset(kDirOffset[d][0] * n, kDirOffset[d][1] * n, kDirOffset[d][2] * n);
}
inline BlockPos above(const BlockPos& p, int n = 1) { return p.offset(0, n, 0); }
inline BlockPos below(const BlockPos& p, int n = 1) { return p.offset(0, -n, 0); }
inline BlockPos atY(const BlockPos& p, int y) { return {p.x, y, p.z}; }
std::optional<Direction> dirFromName(std::string_view n) {
    for (int i = 0; i < 6; ++i)
        if (n == kDirName[i]) return Direction(i);
    return std::nullopt;
}

template <class T>
void jshuffle(T* a, int n, RandomSource& r) {  // Util.shuffle
    for (int i = n; i > 1; --i) std::swap(a[i - 1], a[r.nextInt(i)]);
}
std::array<Direction, 6> allShuffled(RandomSource& r) {  // Direction.allShuffled
    std::array<Direction, 6> a{Down, Up, North, South, West, East};
    jshuffle(a.data(), 6, r);
    return a;
}

inline double distSqr(const BlockPos& a, const BlockPos& b) {  // Vec3i.distSqr
    const double dx = double(a.x) - double(b.x), dy = double(a.y) - double(b.y), dz = double(a.z) - double(b.z);
    return dx * dx + dy * dy + dz * dz;
}
inline int distManhattan(const BlockPos& a, const BlockPos& b) {  // float sum, as Vec3i does
    const float xd = float(iabs(b.x - a.x)), yd = float(iabs(b.y - a.y)), zd = float(iabs(b.z - a.z));
    return d2i(xd + yd + zd);
}

// BlockPos.betweenClosed order: x fastest, then y, then z.
template <class F>
void forBetweenClosed(int x0, int y0, int z0, int x1, int y1, int z1, F&& f) {
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) f(BlockPos{x, y, z});
}

// BlockPos.withinBoxByManhattanDistance (lazy iterator).
class ManhattanIter {
public:
    ManhattanIter(const BlockPos& o, int rx, int ry, int rz) : o_(o), rx_(rx), ry_(ry), rz_(rz), maxDepth_(rx + ry + rz) {}
    bool next(BlockPos& out) {
        if (zMirror_) {
            zMirror_ = false;
            cur_.z = o_.z - (cur_.z - o_.z);
            out = cur_;
            return true;
        }
        for (;;) {
            if (y_ > maxY_) {
                ++x_;
                if (x_ > maxX_) {
                    ++depth_;
                    if (depth_ > maxDepth_) return false;
                    maxX_ = std::min(rx_, depth_);
                    x_ = -maxX_;
                }
                maxY_ = std::min(ry_, depth_ - iabs(x_));
                y_ = -maxY_;
            }
            const int xx = x_, yy = y_, zz = depth_ - iabs(xx) - iabs(yy);
            ++y_;
            if (zz <= rz_) {
                zMirror_ = zz != 0;
                cur_ = {o_.x + xx, o_.y + yy, o_.z + zz};
                out = cur_;
                return true;
            }
        }
    }

private:
    BlockPos o_, cur_;
    int rx_, ry_, rz_, maxDepth_;
    int depth_ = 0, maxX_ = 0, maxY_ = 0, x_ = 0, y_ = 0;
    bool zMirror_ = false;
};

// ---- block facts -----------------------------------------------------------------------------
// Everything the features read about block states, built once per configured feature.
class Ctx {
public:
    explicit Ctx(const FeatureEnv& env) : env_(&env), blocks_(&env.blocks()), facts_(&env.facts()) {}
    const FeatureEnv& env() const { return *env_; }
    BlockRegistry& blocks() const { return *blocks_; }

    std::string_view name(u16 id) const {  // block name without properties
        std::string_view n = blocks_->name(id);
        const size_t k = n.find('[');
        return k == std::string_view::npos ? n : n.substr(0, k);
    }
    bool is(u16 id, std::string_view block) const { return facts_->is(id, block); }
    bool air(u16 id) const { return facts_->isAir(id); }
    bool water(u16 id) const { return is(id, "minecraft:water"); }  // Blocks.WATER (any level)
    bool lava(u16 id) const { return is(id, "minecraft:lava"); }
    bool liquidBlock(u16 id) const { return water(id) || lava(id); }   // BlockState.liquid()
    bool fluidWater(u16 id) const { return facts_->isWater(id); }      // FluidTags.WATER
    bool fluidLava(u16 id) const { return lava(id); }                  // FluidTags.LAVA
    bool sourceWater(u16 id) const { return facts_->isSourceWater(id); }  // fluid is Fluids.WATER
    bool fluidEmpty(u16 id) const { return !facts_->isFluid(id); }
    bool fluidSource(u16 id) const { return sourceWater(id) || (lava(id) && prop(id, "level") == "0"); }
    bool solid(u16 id) const { return facts_->isSolid(id); }                   // BlockState.isSolid()
    bool solidRender(u16 id) const { return facts_->isSolidRender(id); }
    bool canBeReplaced(u16 id) const { return facts_->canBeReplaced(id); }
    bool tag(u16 id, std::string_view t) const { return facts_->inTag(id, t); }
    std::string_view prop(u16 id, std::string_view key) const { return blocks_->property(id, key); }
    bool hasProp(u16 id, std::string_view key) const { return !prop(id, key).empty(); }
    u16 with(u16 id, std::string_view key, std::string_view value) const { return blocks_->withProperty(id, key, value); }
    template <class B>
        requires std::same_as<B, bool>
    u16 with(u16 id, std::string_view key, B value) const {
        return with(id, key, std::string_view(value ? "true" : "false"));
    }
    // isFaceSturdy(level, p, face) of the block at p.
    bool sturdy(const GenRegion& level, const BlockPos& p, Direction face) const { return facts_->isFaceSturdy(level.block(p), face); }
    bool collisionFaceFull(u16 id, Direction face) const { return facts_->isCollisionFaceFull(id, face); }
    bool fullCollision(u16 id) const {  // isCollisionShapeFullBlock: every collision face is full
        for (Direction d : kAllDirs)
            if (!collisionFaceFull(id, d)) return false;
        return true;
    }
    bool canSurvive(const GenRegion& level, u16 id, const BlockPos& p) const { return facts_->canSurvive(level, id, p); }

    u16 def(std::string_view block) const { return env_->defaultState(block); }
    u16 state(const json& j) const { return env_->state(j); }

private:
    const FeatureEnv* env_;
    BlockRegistry* blocks_;
    const BlockFacts* facts_;
};

// HolderSet<Block>: "#tag", "minecraft:block" or a list of blocks.
struct BlockSet {
    std::vector<std::string> names, tags;
    bool test(const Ctx& c, u16 id) const {
        if (!names.empty()) {
            const std::string_view n = c.name(id);
            for (const std::string& s : names)
                if (n == s) return true;
        }
        for (const std::string& t : tags)
            if (c.tag(id, t)) return true;
        return false;
    }
};
BlockSet blockSet(const json& j) {
    BlockSet s;
    auto add = [&](const std::string& v) {
        if (!v.empty() && v[0] == '#')
            s.tags.push_back(v.substr(1));
        else if (!v.empty())
            s.names.push_back(v.find(':') == std::string::npos ? "minecraft:" + v : v);
    };
    if (j.is_string())
        add(j.get<std::string>());
    else if (j.is_array())
        for (const json& e : j)
            if (e.is_string()) add(e.get<std::string>());
    return s;
}

const json& cfgOf(const json& j) {
    auto it = j.find("config");
    return it != j.end() && it->is_object() ? *it : j;
}
const json& req(const json& j, const char* key) {
    static const json kNull;
    auto it = j.find(key);
    if (it == j.end()) {
        LOG("features_misc: missing field \"%s\"", key);
        return kNull;
    }
    return *it;
}
template <class T>
T opt(const json& j, const char* key, T fallback) {
    auto it = j.find(key);
    return it == j.end() || it->is_null() ? fallback : it->get<T>();
}
using IntP = std::shared_ptr<const IntProvider>;
using FloatP = std::shared_ptr<const FloatProvider>;
using StateP = std::shared_ptr<const StateProvider>;
using PredP = std::shared_ptr<const BlockPredicate>;
using PlacedP = std::shared_ptr<const PlacedFeature>;

// Column.scan: floor / ceiling around an inside position.
struct Column {
    std::optional<int> floor, ceiling;
    std::optional<int> height() const {
        if (floor && ceiling) return *ceiling - *floor - 1;
        return std::nullopt;
    }
};
template <class In, class Edge>
std::optional<Column> scanColumn(const GenRegion& level, const BlockPos& pos, int range, In&& inside, Edge&& edge) {
    if (!inside(level.block(pos))) return std::nullopt;
    auto scan = [&](int dy) -> std::optional<int> {
        BlockPos p = pos;
        for (int i = 1; i < range && inside(level.block(p)); ++i) p.y += dy;
        if (edge(level.block(p))) return p.y;
        return std::nullopt;
    };
    Column c;
    c.ceiling = scan(1);
    c.floor = scan(-1);
    return c;
}

inline int javaMaxY(const GenRegion& level) { return level.maxY() - 1; }  // getMaxY() is inclusive
inline bool outsideBuildHeight(const GenRegion& level, int y) { return y < level.minY() || y >= level.maxY(); }

// =============================================================================================
// Ores
// =============================================================================================
struct OreTarget {
    std::shared_ptr<const RuleTest> target;
    u16 state;
};

class AbstractOre : public ConfiguredFeature {
public:
    AbstractOre(const json& cfg, const FeatureEnv& env) : c_(env) {
        for (const json& t : req(cfg, "targets")) targets_.push_back({env.ruleTest(req(t, "target")), env.state(req(t, "state"))});
        size_ = opt<int>(cfg, "size", 0);
        discard_ = opt<float>(cfg, "discard_chance_on_air_exposure", 0.0f);
    }

protected:
    bool canPlaceOre(const GenRegion& level, u16 state, RandomSource& r, const OreTarget& t, const BlockPos& pos) const {
        if (!t.target->test(state, pos, r)) return false;
        bool skipAirCheck;
        if (discard_ <= 0.0f)
            skipAirCheck = true;
        else
            skipAirCheck = discard_ >= 1.0f ? false : r.nextFloat() >= discard_;
        if (skipAirCheck) return true;
        for (Direction d : kAllDirs)
            if (c_.air(level.block(rel(pos, d)))) return false;
        return true;
    }

    Ctx c_;
    std::vector<OreTarget> targets_;
    int size_ = 0;
    float discard_ = 0.0f;
};

class OreFeature final : public AbstractOre {
public:
    using AbstractOre::AbstractOre;
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        const float dir = r.nextFloat() * kPiF;
        const float spreadXY = float(size_) / 8.0f;
        const int maxRadius = jceil((float(size_) / 16.0f * 2.0f + 1.0f) / 2.0f);
        const double x0 = o.x + std::sin(double(dir)) * double(spreadXY);
        const double x1 = o.x - std::sin(double(dir)) * double(spreadXY);
        const double z0 = o.z + std::cos(double(dir)) * double(spreadXY);
        const double z1 = o.z - std::cos(double(dir)) * double(spreadXY);
        const double y0 = double(o.y + r.nextInt(3) - 2);
        const double y1 = double(o.y + r.nextInt(3) - 2);
        const int xStart = o.x - jceil(spreadXY) - maxRadius;
        const int yStart = o.y - 2 - maxRadius;
        const int zStart = o.z - jceil(spreadXY) - maxRadius;
        const int sizeXZ = 2 * (jceil(spreadXY) + maxRadius);
        const int sizeY = 2 * (2 + maxRadius);
        for (int xp = xStart; xp <= xStart + sizeXZ; ++xp)
            for (int zp = zStart; zp <= zStart + sizeXZ; ++zp)
                if (yStart <= level.height(Heightmap::OceanFloorWG, xp, zp))
                    return doPlace(level, r, x0, x1, z0, z1, y0, y1, xStart, yStart, zStart, sizeXZ, sizeY);
        return false;
    }

private:
    bool doPlace(GenRegion& level, RandomSource& r, double x0, double x1, double z0, double z1, double y0, double y1,
                 int xStart, int yStart, int zStart, int sizeXZ, int sizeY) const {
        int placed = 0;
        std::vector<u8> tested(size_t(std::max(0, sizeXZ * sizeY * sizeXZ)) + 1, 0);
        std::vector<double> data(size_t(size_) * 4);
        for (int i = 0; i < size_; ++i) {
            const float step = float(i) / float(size_);
            const double xx = lerp(double(step), x0, x1), yy = lerp(double(step), y0, y1), zz = lerp(double(step), z0, z1);
            const double ss = r.nextDouble() * size_ / 16.0;
            const double rr = (double(msin(double(kPiF * step)) + 1.0f) * ss + 1.0) / 2.0;
            data[i * 4 + 0] = xx, data[i * 4 + 1] = yy, data[i * 4 + 2] = zz, data[i * 4 + 3] = rr;
        }
        for (int i1 = 0; i1 < size_ - 1; ++i1) {
            if (data[i1 * 4 + 3] <= 0.0) continue;
            for (int i2 = i1 + 1; i2 < size_; ++i2) {
                if (data[i2 * 4 + 3] <= 0.0) continue;
                const double dx = data[i1 * 4 + 0] - data[i2 * 4 + 0];
                const double dy = data[i1 * 4 + 1] - data[i2 * 4 + 1];
                const double dz = data[i1 * 4 + 2] - data[i2 * 4 + 2];
                const double dr = data[i1 * 4 + 3] - data[i2 * 4 + 3];
                if (dr * dr > dx * dx + dy * dy + dz * dz) {
                    if (dr > 0.0)
                        data[i2 * 4 + 3] = -1.0;
                    else
                        data[i1 * 4 + 3] = -1.0;
                }
            }
        }
        for (int i = 0; i < size_; ++i) {
            const double rr = data[i * 4 + 3];
            if (rr < 0.0) continue;
            const double xx = data[i * 4 + 0], yy = data[i * 4 + 1], zz = data[i * 4 + 2];
            const int xMin = std::max(jfloor(xx - rr), xStart), yMin = std::max(jfloor(yy - rr), yStart);
            const int zMin = std::max(jfloor(zz - rr), zStart);
            const int xMax = std::max(jfloor(xx + rr), xMin), yMax = std::max(jfloor(yy + rr), yMin);
            const int zMax = std::max(jfloor(zz + rr), zMin);
            for (int x = xMin; x <= xMax; ++x) {
                const double xd = (x + 0.5 - xx) / rr;
                if (!(xd * xd < 1.0)) continue;
                for (int y = yMin; y <= yMax; ++y) {
                    const double yd = (y + 0.5 - yy) / rr;
                    if (!(xd * xd + yd * yd < 1.0)) continue;
                    for (int z = zMin; z <= zMax; ++z) {
                        const double zd = (z + 0.5 - zz) / rr;
                        if (!(xd * xd + yd * yd + zd * zd < 1.0) || outsideBuildHeight(level, y)) continue;
                        const size_t bit = size_t(x - xStart + (y - yStart) * sizeXZ + (z - zStart) * sizeXZ * sizeY);
                        if (bit >= tested.size()) tested.resize(bit + 1, 0);
                        if (tested[bit]) continue;
                        tested[bit] = 1;
                        const BlockPos p{x, y, z};
                        if (!level.inside(p)) continue;
                        const u16 state = level.block(p);
                        for (const OreTarget& t : targets_)
                            if (canPlaceOre(level, state, r, t, p)) {
                                level.setBlock(p, t.state);
                                ++placed;
                                break;
                            }
                    }
                }
            }
        }
        return placed > 0;
    }
};

class ScatteredOreFeature final : public AbstractOre {
public:
    using AbstractOre::AbstractOre;
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        const int tries = r.nextInt(size_ + 1);
        for (int i = 0; i < tries; ++i) {
            const int maxDist = std::min(i, 7);
            const int xd = axis(r, maxDist), yd = axis(r, maxDist), zd = axis(r, maxDist);
            const BlockPos p = o.offset(xd, yd, zd);
            const u16 state = level.block(p);
            for (const OreTarget& t : targets_)
                if (canPlaceOre(level, state, r, t, p)) {
                    level.setBlock(p, t.state);
                    break;
                }
        }
        return true;
    }

private:
    static int axis(RandomSource& r, int maxDist) {
        const float a = r.nextFloat();
        const float b = r.nextFloat();
        return jround((a - b) * float(maxDist));
    }
};

// =============================================================================================
// Geode
// =============================================================================================
std::shared_ptr<const Noise> geodeNoise(i64 seed) {
    // NormalNoise.createParity(-4, 1.0): one octave, base amplitude rescaled to the legacy normalisation.
    const double dev = PerlinNoise::kStandardDeviation * 1.0;
    const double newFactor = (1.0 * 0.3333333333333333) / (std::sqrt(dev * dev) * std::sqrt(2.0));
    const double oldFactor = 1.0 * 0.5 * 0.3333333333333333 / (0.1 * (1.0 + 1.0 / 1.0));
    const NormalNoise params = NormalNoise::fromJson(json{{"base_amplitude", oldFactor / newFactor}, {"base_octave", -4}, {"octave_count", 1}});
    LegacyRandom random(seed);
    return params.create(random);
}

class GeodeFeature final : public ConfiguredFeature {
public:
    GeodeFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        const json& b = req(cfg, "blocks");
        filling_ = env.stateProvider(req(b, "filling_provider"));
        inner_ = env.stateProvider(req(b, "inner_layer_provider"));
        alternateInner_ = env.stateProvider(req(b, "alternate_inner_layer_provider"));
        middle_ = env.stateProvider(req(b, "middle_layer_provider"));
        outer_ = env.stateProvider(req(b, "outer_layer_provider"));
        for (const json& s : req(b, "inner_placements")) innerPlacements_.push_back(env.state(s));
        cannotReplace_ = blockSet(req(b, "cannot_replace"));
        invalidBlocks_ = blockSet(req(b, "invalid_blocks"));
        const json& layers = opt<json>(cfg, "layers", json::object());
        layerFilling_ = opt<double>(layers, "filling", 1.7);
        innerLayer_ = opt<double>(layers, "inner_layer", 2.2);
        middleLayer_ = opt<double>(layers, "middle_layer", 3.2);
        outerLayer_ = opt<double>(layers, "outer_layer", 4.2);
        const json& crack = opt<json>(cfg, "crack", json::object());
        generateCrackChance_ = opt<double>(crack, "generate_crack_chance", 1.0);
        baseCrackSize_ = opt<double>(crack, "base_crack_size", 2.0);
        crackPointOffset_ = opt<int>(crack, "crack_point_offset", 2);
        usePotentialPlacementsChance_ = opt<double>(cfg, "use_potential_placements_chance", 0.35);
        useAlternateLayer0Chance_ = opt<double>(cfg, "use_alternate_layer0_chance", 0.0);
        placementsRequireLayer0Alternate_ = opt<bool>(cfg, "placements_require_layer0_alternate", true);
        outerWallDistance_ = intOr(env, cfg, "outer_wall_distance", 4, 5);
        distributionPoints_ = intOr(env, cfg, "distribution_points", 3, 4);
        pointOffset_ = intOr(env, cfg, "point_offset", 1, 2);
        minGenOffset_ = opt<int>(cfg, "min_gen_offset", -16);
        maxGenOffset_ = opt<int>(cfg, "max_gen_offset", 16);
        noiseMultiplier_ = opt<double>(cfg, "noise_multiplier", 0.05);
        invalidBlocksThreshold_ = opt<int>(cfg, "invalid_blocks_threshold", 0);
        noise_ = geodeNoise(env.noise().seed());
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        std::vector<std::pair<BlockPos, int>> points;
        const int numPoints = distributionPoints_->sample(r);
        std::vector<BlockPos> crackPoints;
        const double crackSizeAdjustment = double(numPoints) / double(outerWallDistance_->maxValue());
        const double innerAir = 1.0 / std::sqrt(layerFilling_);
        const double innermostBlockLayer = 1.0 / std::sqrt(innerLayer_ + crackSizeAdjustment);
        const double innerCrust = 1.0 / std::sqrt(middleLayer_ + crackSizeAdjustment);
        const double outerCrust = 1.0 / std::sqrt(outerLayer_ + crackSizeAdjustment);
        const double crackSize =
            1.0 / std::sqrt(baseCrackSize_ + r.nextDouble() / 2.0 + (numPoints > 3 ? crackSizeAdjustment : 0.0));
        const bool shouldGenerateCrack = double(r.nextFloat()) < generateCrackChance_;
        int numInvalidPoints = 0;
        for (int i = 0; i < numPoints; ++i) {
            const int x = outerWallDistance_->sample(r);
            const int y = outerWallDistance_->sample(r);
            const int z = outerWallDistance_->sample(r);
            const BlockPos p = o.offset(x, y, z);
            const u16 s = level.block(p);
            if (c_.air(s) || invalidBlocks_.test(c_, s))
                if (++numInvalidPoints > invalidBlocksThreshold_) return false;
            points.push_back({p, pointOffset_->sample(r)});
        }
        if (shouldGenerateCrack) {
            const int offsetIndex = r.nextInt(4);
            const int k = numPoints * 2 + 1;
            const int dx = offsetIndex == 0 || offsetIndex == 2 ? k : 0;
            const int dz = offsetIndex == 1 || offsetIndex == 2 ? k : 0;
            crackPoints.push_back(o.offset(dx, 7, dz));
            crackPoints.push_back(o.offset(dx, 5, dz));
            crackPoints.push_back(o.offset(dx, 1, dz));
        }
        std::vector<BlockPos> potentialCrystalPlacements;
        auto safeSet = [&](const BlockPos& p, u16 s) {
            if (!cannotReplace_.test(c_, level.block(p))) level.setBlock(p, s);
        };
        forBetweenClosed(o.x + minGenOffset_, o.y + minGenOffset_, o.z + minGenOffset_, o.x + maxGenOffset_, o.y + maxGenOffset_,
                         o.z + maxGenOffset_, [&](const BlockPos& p) {
            const double noiseOffset = double(noise_->get(double(p.x), double(p.y), double(p.z))) * noiseMultiplier_;
            double distSumShell = 0.0;
            for (const auto& point : points) distSumShell += 1.0 / std::sqrt(distSqr(p, point.first) + point.second) + noiseOffset;
            if (distSumShell < outerCrust) return;
            if (distSumShell >= innerAir) {
                safeSet(p, filling_->state(r, p));
                return;
            }
            double distSumCrack = 0.0;
            for (const BlockPos& cp : crackPoints) distSumCrack += 1.0 / std::sqrt(distSqr(p, cp) + crackPointOffset_) + noiseOffset;
            if (shouldGenerateCrack && distSumCrack >= crackSize) {
                safeSet(p, air_);  // (the game also schedules fluid ticks next to the crack)
            } else if (distSumShell >= innermostBlockLayer) {
                const bool useAlternateLayer = double(r.nextFloat()) < useAlternateLayer0Chance_;
                safeSet(p, (useAlternateLayer ? alternateInner_ : inner_)->state(r, p));
                if ((!placementsRequireLayer0Alternate_ || useAlternateLayer) && double(r.nextFloat()) < usePotentialPlacementsChance_)
                    potentialCrystalPlacements.push_back(p);
            } else if (distSumShell >= innerCrust) {
                safeSet(p, middle_->state(r, p));
            } else if (distSumShell >= outerCrust) {
                safeSet(p, outer_->state(r, p));
            }
        });
        for (const BlockPos& crystalPos : potentialCrystalPlacements) {
            u16 state = innerPlacements_[size_t(r.nextInt(int(innerPlacements_.size())))];
            for (Direction d : kAllDirs) {
                if (c_.hasProp(state, "facing")) state = c_.with(state, "facing", kDirName[d]);
                const BlockPos placePos = rel(crystalPos, d);
                const u16 placeState = level.block(placePos);
                if (c_.hasProp(state, "waterlogged")) state = c_.with(state, "waterlogged", c_.fluidSource(placeState));
                if (canClusterGrowAt(placeState)) {
                    safeSet(placePos, state);
                    break;
                }
            }
        }
        return true;
    }

private:
    static IntP intOr(const FeatureEnv& env, const json& cfg, const char* key, int lo, int hi) {
        auto it = cfg.find(key);
        if (it != cfg.end()) return env.intProvider(*it);
        return env.intProvider(json{{"type", "minecraft:uniform"}, {"min_inclusive", lo}, {"max_inclusive", hi}});
    }
    bool canClusterGrowAt(u16 s) const {  // BuddingAmethystBlock.canClusterGrowAtState: air, or water with amount 8
        if (c_.air(s)) return true;
        if (!c_.water(s)) return false;
        const std::string_view lvl = c_.prop(s, "level");
        const int level = lvl.empty() ? 0 : std::atoi(std::string(lvl).c_str());
        return level == 0 || level >= 8;  // source or falling
    }

    Ctx c_;
    u16 air_ = c_.def("minecraft:air");
    StateP filling_, inner_, alternateInner_, middle_, outer_;
    std::vector<u16> innerPlacements_;
    BlockSet cannotReplace_, invalidBlocks_;
    double layerFilling_ = 1.7, innerLayer_ = 2.2, middleLayer_ = 3.2, outerLayer_ = 4.2;
    double generateCrackChance_ = 1.0, baseCrackSize_ = 2.0;
    int crackPointOffset_ = 2;
    double usePotentialPlacementsChance_ = 0.35, useAlternateLayer0Chance_ = 0.0;
    bool placementsRequireLayer0Alternate_ = true;
    IntP outerWallDistance_, distributionPoints_, pointOffset_;
    int minGenOffset_ = -16, maxGenOffset_ = 16;
    double noiseMultiplier_ = 0.05;
    int invalidBlocksThreshold_ = 0;
    std::shared_ptr<const Noise> noise_;
};

// =============================================================================================
// Speleothems (pointed dripstone, sulfur spikes)
// =============================================================================================
struct Speleothem {
    Ctx c;
    std::string baseBlock, pointedBlock;
    u16 baseDefault = 0, pointedDefault = 0;
    BlockSet replaceable;

    Speleothem(const json& cfg, const FeatureEnv& env) : c(env) {
        baseDefault = c.def(c.name(env.state(req(cfg, "base_block"))));
        pointedDefault = c.def(c.name(env.state(req(cfg, "pointed_block"))));
        baseBlock = std::string(c.name(baseDefault));
        pointedBlock = std::string(c.name(pointedDefault));
        replaceable = blockSet(req(cfg, "replaceable_blocks"));
    }
    bool emptyOrWater(u16 s) const { return c.air(s) || c.water(s); }
    bool emptyOrWaterOrLava(u16 s) const { return c.air(s) || c.water(s) || c.lava(s); }
    bool neitherEmptyNorWater(u16 s) const { return !c.air(s) && !c.water(s); }
    bool isBase(u16 s) const { return c.is(s, baseBlock) || replaceable.test(c, s); }
    bool placeBaseBlockIfPossible(GenRegion& level, const BlockPos& p) const {
        if (!replaceable.test(c, level.block(p))) return false;
        level.setBlock(p, baseDefault);
        return true;
    }
    u16 pointed(Direction d, const char* thickness) const {
        return c.with(c.with(pointedDefault, "vertical_direction", kDirName[d]), "thickness", thickness);
    }
    void grow(GenRegion& level, const BlockPos& start, Direction tip, int height, bool mergedTip) const {
        if (!isBase(level.block(rel(start, opposite(tip))))) return;
        BlockPos p = start;
        auto put = [&](u16 s) {
            s = c.with(s, "waterlogged", c.fluidWater(level.block(p)));
            level.setBlock(p, s);
            p = rel(p, tip);
        };
        if (height >= 3) {
            put(pointed(tip, "base"));
            for (int i = 0; i < height - 3; ++i) put(pointed(tip, "middle"));
        }
        if (height >= 2) put(pointed(tip, "frustum"));
        if (height >= 1) put(pointed(tip, mergedTip ? "tip_merge" : "tip"));
    }
};

class SpeleothemFeature final : public ConfiguredFeature {
public:
    SpeleothemFeature(const json& cfg, const FeatureEnv& env) : s_(cfg, env) {
        tallerChance_ = opt<float>(cfg, "chance_of_taller_generation", 0.2f);
        directionalSpread_ = opt<float>(cfg, "chance_of_directional_spread", 0.7f);
        spreadRadius2_ = opt<float>(cfg, "chance_of_spread_radius2", 0.5f);
        spreadRadius3_ = opt<float>(cfg, "chance_of_spread_radius3", 0.5f);
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        const bool canAbove = s_.isBase(level.block(above(o)));
        const bool canBelow = s_.isBase(level.block(below(o)));
        Direction tip;
        if (canAbove && canBelow)
            tip = r.nextBoolean() ? Down : Up;
        else if (canAbove)
            tip = Down;
        else if (canBelow)
            tip = Up;
        else
            return false;
        const BlockPos root = rel(o, opposite(tip));
        s_.placeBaseBlockIfPossible(level, root);
        for (Direction d : kHorizontal) {
            if (r.nextFloat() > directionalSpread_) continue;
            const BlockPos p1 = rel(root, d);
            s_.placeBaseBlockIfPossible(level, p1);
            if (r.nextFloat() > spreadRadius2_) continue;
            const BlockPos p2 = rel(p1, kAllDirs[r.nextInt(6)]);
            s_.placeBaseBlockIfPossible(level, p2);
            if (r.nextFloat() > spreadRadius3_) continue;
            const BlockPos p3 = rel(p2, kAllDirs[r.nextInt(6)]);
            s_.placeBaseBlockIfPossible(level, p3);
        }
        const int height = r.nextFloat() < tallerChance_ && s_.emptyOrWater(level.block(rel(o, tip))) ? 2 : 1;
        s_.grow(level, o, tip, height, false);
        return true;
    }

private:
    Speleothem s_;
    float tallerChance_, directionalSpread_, spreadRadius2_, spreadRadius3_;
};

class SpeleothemClusterFeature final : public ConfiguredFeature {
public:
    SpeleothemClusterFeature(const json& cfg, const FeatureEnv& env) : s_(cfg, env) {
        searchRange_ = opt<int>(cfg, "floor_to_ceiling_search_range", 12);
        height_ = env.intProvider(req(cfg, "height"));
        radius_ = env.intProvider(req(cfg, "radius"));
        maxHeightDiff_ = opt<int>(cfg, "max_stalagmite_stalactite_height_diff", 0);
        heightDeviation_ = opt<int>(cfg, "height_deviation", 1);
        layerThickness_ = env.intProvider(req(cfg, "speleothem_block_layer_thickness"));
        density_ = env.floatProvider(req(cfg, "density"));
        wetness_ = env.floatProvider(req(cfg, "wetness"));
        chanceAtMaxDistance_ = opt<float>(cfg, "chance_of_speleothem_at_max_distance_from_center", 0.0f);
        maxDistanceFromEdge_ = opt<int>(cfg, "max_distance_from_edge_affecting_chance_of_speleothem", 1);
        maxDistanceFromCenter_ = opt<int>(cfg, "max_distance_from_center_affecting_height_bias", 1);
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (!s_.emptyOrWater(level.block(o))) return false;
        const int height = height_->sample(r);
        const float wetness = wetness_->sample(r);
        const float density = density_->sample(r);
        const int xRadius = radius_->sample(r);
        const int zRadius = radius_->sample(r);
        for (int dx = -xRadius; dx <= xRadius; ++dx)
            for (int dz = -zRadius; dz <= zRadius; ++dz) {
                const double chance = chanceOfSpeleothem(xRadius, zRadius, dx, dz);
                placeColumn(level, r, o.offset(dx, 0, dz), dx, dz, wetness, chance, height, density);
            }
        return true;
    }

private:
    void placeColumn(GenRegion& level, RandomSource& r, const BlockPos& pos, int dx, int dz, float chanceOfWater, double chance,
                     int clusterHeight, float density) const {
        auto base = scanColumn(level, pos, searchRange_, [&](u16 s) { return s_.emptyOrWater(s); },
                               [&](u16 s) { return s_.neitherEmptyNorWater(s); });
        if (!base) return;
        const std::optional<int> ceiling = base->ceiling, baseFloor = base->floor;
        if (!ceiling && !baseFloor) return;
        const bool wantPool = r.nextFloat() < chanceOfWater;
        Column column;
        if (wantPool && baseFloor && canPlacePool(level, atY(pos, *baseFloor))) {
            const int baseFloorY = *baseFloor;
            column = {baseFloorY - 1, base->ceiling};
            level.setBlock(atY(pos, baseFloorY), waterState_);
        } else {
            column = *base;
        }
        const std::optional<int> floor = column.floor;
        const bool wantStalactite = r.nextDouble() < chance;
        int stalactiteHeight = 0;
        if (ceiling && wantStalactite && !s_.c.lava(level.block(atY(pos, *ceiling)))) {
            const int thickness = layerThickness_->sample(r);
            replaceWithBase(level, atY(pos, *ceiling), thickness, Up);
            const int maxHeight = floor ? std::min(clusterHeight, *ceiling - *floor) : clusterHeight;
            stalactiteHeight = speleothemHeight(r, dx, dz, density, maxHeight);
        }
        const bool wantStalagmite = r.nextDouble() < chance;
        int stalagmiteHeight = 0;
        if (floor && wantStalagmite && !s_.c.lava(level.block(atY(pos, *floor)))) {
            const int thickness = layerThickness_->sample(r);
            replaceWithBase(level, atY(pos, *floor), thickness, Down);
            if (ceiling)
                stalagmiteHeight = std::max(0, stalactiteHeight + r.nextIntBetweenInclusive(-maxHeightDiff_, maxHeightDiff_));
            else
                stalagmiteHeight = speleothemHeight(r, dx, dz, density, clusterHeight);
        }
        int actualStalagmite, actualStalactite;
        if (ceiling && floor && *ceiling - stalactiteHeight <= *floor + stalagmiteHeight) {
            const int floorY = *floor, ceilingY = *ceiling;
            const int lowestStalactiteBottom = std::max(ceilingY - stalactiteHeight, floorY + 1);
            const int highestStalagmiteTop = std::min(floorY + stalagmiteHeight, ceilingY - 1);
            const int actualStalactiteBottom = r.nextIntBetweenInclusive(lowestStalactiteBottom, highestStalagmiteTop + 1);
            actualStalactite = ceilingY - actualStalactiteBottom;
            actualStalagmite = actualStalactiteBottom - 1 - floorY;
        } else {
            actualStalactite = stalactiteHeight;
            actualStalagmite = stalagmiteHeight;
        }
        const bool mergeTips = r.nextBoolean() && actualStalactite > 0 && actualStalagmite > 0 && column.height() &&
                               actualStalactite + actualStalagmite == *column.height();
        if (ceiling) s_.grow(level, atY(pos, *ceiling - 1), Down, actualStalactite, mergeTips);
        if (floor) s_.grow(level, atY(pos, *floor + 1), Up, actualStalagmite, mergeTips);
    }

    int speleothemHeight(RandomSource& r, int dx, int dz, float density, int maxHeight) const {
        if (r.nextFloat() > density) return 0;
        const int distanceFromCenter = iabs(dx) + iabs(dz);
        const float mean = float(clampedMap(double(distanceFromCenter), 0.0, double(maxDistanceFromCenter_), maxHeight / 2.0, 0.0));
        // ClampedNormalFloat.sample(random, mean, deviation, 0, maxHeight)
        const float v = clampf(mean + float(r.nextGaussian()) * float(heightDeviation_), 0.0f, float(maxHeight));
        return d2i(v);
    }
    bool canPlacePool(const GenRegion& level, const BlockPos& p) const {
        const u16 s = level.block(p);
        if (s_.c.water(s) || s_.c.is(s, s_.baseBlock) || s_.c.is(s, s_.pointedBlock)) return false;
        if (s_.c.fluidWater(level.block(above(p)))) return false;
        for (Direction d : kHorizontal)
            if (!canBeAdjacentToWater(level, rel(p, d))) return false;
        return canBeAdjacentToWater(level, below(p));
    }
    bool canBeAdjacentToWater(const GenRegion& level, const BlockPos& p) const {
        const u16 s = level.block(p);
        return s_.c.tag(s, "minecraft:base_stone_overworld") || s_.c.fluidWater(s);
    }
    void replaceWithBase(GenRegion& level, BlockPos p, int maxCount, Direction d) const {
        for (int i = 0; i < maxCount; ++i) {
            if (!s_.placeBaseBlockIfPossible(level, p)) return;
            p = rel(p, d);
        }
    }
    double chanceOfSpeleothem(int xRadius, int zRadius, int dx, int dz) const {
        const int edge = std::min(xRadius - iabs(dx), zRadius - iabs(dz));
        return double(clampedMap(float(edge), 0.0f, float(maxDistanceFromEdge_), chanceAtMaxDistance_, 1.0f));
    }

    Speleothem s_;
    u16 waterState_ = s_.c.def("minecraft:water");
    int searchRange_, maxHeightDiff_, heightDeviation_, maxDistanceFromEdge_, maxDistanceFromCenter_;
    IntP height_, radius_, layerThickness_;
    FloatP density_, wetness_;
    float chanceAtMaxDistance_;
};

double speleothemHeightAt(double xzDistance, double radius, double scale, double bluntness) {  // SpeleothemUtils
    if (xzDistance < bluntness) xzDistance = bluntness;
    const double r = xzDistance / radius * 0.384;
    const double part1 = 0.75 * std::pow(r, 1.3333333333333333);
    const double part2 = std::pow(r, 0.6666666666666666);
    const double part3 = 0.3333333333333333 * std::log(r);
    double h = scale * (part1 - part2 - part3);
    h = std::max(h, 0.0);
    return h / 0.384 * radius;
}

class LargeDripstoneFeature final : public ConfiguredFeature {
public:
    LargeDripstoneFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        replaceable_ = blockSet(req(cfg, "replaceable_blocks"));
        searchRange_ = opt<int>(cfg, "floor_to_ceiling_search_range", 30);
        columnRadius_ = env.intProvider(req(cfg, "column_radius"));
        heightScale_ = env.floatProvider(req(cfg, "height_scale"));
        maxRadiusRatio_ = opt<float>(cfg, "max_column_radius_to_cave_height_ratio", 0.33f);
        stalactiteBluntness_ = env.floatProvider(req(cfg, "stalactite_bluntness"));
        stalagmiteBluntness_ = env.floatProvider(req(cfg, "stalagmite_bluntness"));
        windSpeed_ = env.floatProvider(req(cfg, "wind_speed"));
        minRadiusForWind_ = opt<int>(cfg, "min_radius_for_wind", 0);
        minBluntnessForWind_ = opt<float>(cfg, "min_bluntness_for_wind", 0.0f);
        dripstone_ = c_.def("minecraft:dripstone_block");
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (!emptyOrWater(level.block(o))) return false;
        auto column = scanColumn(level, o, searchRange_, [&](u16 s) { return emptyOrWater(s); },
                                 [&](u16 s) { return c_.is(s, "minecraft:dripstone_block") || replaceable_.test(c_, s) || c_.lava(s); });
        if (!column || !column->floor || !column->ceiling) return false;
        const int columnHeight = *column->height();
        if (columnHeight < 4) return false;
        const int byHeight = d2i(float(columnHeight) * maxRadiusRatio_);
        const int maxColumnRadius = std::clamp(byHeight, columnRadius_->minValue(), columnRadius_->maxValue());
        const int radius = r.nextIntBetweenInclusive(columnRadius_->minValue(), maxColumnRadius);
        Dripstone stalactite = make(atY(o, *column->ceiling - 1), false, r, radius, *stalactiteBluntness_);
        Dripstone stalagmite = make(atY(o, *column->floor + 1), true, r, radius, *stalagmiteBluntness_);
        Wind wind;
        if (stalactite.suitableForWind(minRadiusForWind_, minBluntnessForWind_) &&
            stalagmite.suitableForWind(minRadiusForWind_, minBluntnessForWind_)) {
            wind.originY = o.y;
            wind.maxOffset = 16 - radius;
            const float speed = windSpeed_->sample(r);
            const float direction = r.nextFloat() * (kPiF - 0.0f) + 0.0f;
            wind.x = double(mcos(double(direction)) * speed);
            wind.z = double(msin(double(direction)) * speed);
            wind.active = true;
        }
        const bool stalactiteEmbedded = moveBack(level, stalactite, wind);
        const bool stalagmiteEmbedded = moveBack(level, stalagmite, wind);
        if (stalactiteEmbedded) placeBlocks(level, r, stalactite, wind);
        if (stalagmiteEmbedded) placeBlocks(level, r, stalagmite, wind);
        return true;
    }

private:
    struct Dripstone {
        BlockPos root;
        bool pointingUp;
        int radius;
        double bluntness, scale;
        int heightAt(float checkRadius) const { return d2i(speleothemHeightAt(checkRadius, radius, scale, bluntness)); }
        bool suitableForWind(int minRadius, float minBluntness) const { return radius >= minRadius && bluntness >= minBluntness; }
    };
    struct Wind {
        bool active = false;
        int originY = 0, maxOffset = 0;
        double x = 0, z = 0;
        BlockPos offset(const BlockPos& p) const {
            if (!active) return p;
            const int dy = originY - p.y;
            const int dx = std::clamp(jfloor(x * dy), -maxOffset, maxOffset);
            const int dz = std::clamp(jfloor(z * dy), -maxOffset, maxOffset);
            return p.offset(dx, 0, dz);
        }
    };
    Dripstone make(const BlockPos& root, bool up, RandomSource& r, int radius, const FloatProvider& bluntness) const {
        const double b = double(bluntness.sample(r));
        const double s = double(heightScale_->sample(r));
        return {root, up, radius, b, s};
    }
    bool emptyOrWater(u16 s) const { return c_.air(s) || c_.water(s); }
    bool emptyOrWaterOrLava(u16 s) const { return c_.air(s) || c_.water(s) || c_.lava(s); }
    bool circleEmbedded(const GenRegion& level, const BlockPos& center, int xzRadius) const {
        if (emptyOrWaterOrLava(level.block(center))) return false;
        const float increment = 6.0f / float(xzRadius);
        for (float angle = 0.0f; angle < float(kPi * 2); angle += increment) {
            const int dx = d2i(mcos(double(angle)) * float(xzRadius));
            const int dz = d2i(msin(double(angle)) * float(xzRadius));
            if (emptyOrWaterOrLava(level.block(center.offset(dx, 0, dz)))) return false;
        }
        return true;
    }
    bool moveBack(const GenRegion& level, Dripstone& d, const Wind& wind) const {
        while (d.radius > 1) {
            BlockPos root = d.root;
            const int maxTries = std::min(10, d.heightAt(0.0f));
            for (int i = 0; i < maxTries; ++i) {
                if (c_.lava(level.block(root))) return false;
                if (circleEmbedded(level, wind.offset(root), d.radius)) {
                    d.root = root;
                    return true;
                }
                root = rel(root, d.pointingUp ? Down : Up);
            }
            d.radius /= 2;
        }
        return false;
    }
    void placeBlocks(GenRegion& level, RandomSource& r, const Dripstone& d, const Wind& wind) const {
        for (int dx = -d.radius; dx <= d.radius; ++dx)
            for (int dz = -d.radius; dz <= d.radius; ++dz) {
                const float currentRadius = std::sqrt(float(dx * dx + dz * dz));
                if (currentRadius > float(d.radius)) continue;
                int height = d.heightAt(currentRadius);
                if (height <= 0) continue;
                if (double(r.nextFloat()) < 0.2) height = d2i(float(height) * (r.nextFloat() * (1.0f - 0.8f) + 0.8f));
                BlockPos p = d.root.offset(dx, 0, dz);
                bool hasBeenOutOfStone = false;
                const int maxY = d.pointingUp ? level.height(Heightmap::WorldSurfaceWG, p.x, p.z) : INT32_MAX;
                for (int i = 0; i < height && p.y < maxY; ++i) {
                    const BlockPos w = wind.offset(p);
                    const u16 s = level.block(w);
                    if (emptyOrWaterOrLava(s)) {
                        hasBeenOutOfStone = true;
                        level.setBlock(w, dripstone_);
                    } else if (hasBeenOutOfStone && c_.tag(s, "minecraft:base_stone_overworld")) {
                        break;
                    }
                    p = rel(p, d.pointingUp ? Up : Down);
                }
            }
    }

    Ctx c_;
    BlockSet replaceable_;
    int searchRange_, minRadiusForWind_;
    IntP columnRadius_;
    FloatP heightScale_, stalactiteBluntness_, stalagmiteBluntness_, windSpeed_;
    float maxRadiusRatio_, minBluntnessForWind_;
    u16 dripstone_;
};

// =============================================================================================
// Vegetation patches
// =============================================================================================
class VegetationPatchFeature : public ConfiguredFeature {
public:
    VegetationPatchFeature(const json& cfg, const FeatureEnv& env) : c_(env), env_(&env) {
        replaceable_ = blockSet(req(cfg, "replaceable"));
        groundState_ = env.stateProvider(req(cfg, "ground_state"));
        vegetation_ = env.placed(req(cfg, "vegetation_feature"));
        inwards_ = opt<std::string>(cfg, "surface", "floor") == "ceiling" ? Up : Down;
        depth_ = env.intProvider(req(cfg, "depth"));
        extraBottomBlockChance_ = opt<float>(cfg, "extra_bottom_block_chance", 0.0f);
        verticalRange_ = opt<int>(cfg, "vertical_range", 1);
        vegetationChance_ = opt<float>(cfg, "vegetation_chance", 0.0f);
        xzRadius_ = env.intProvider(req(cfg, "xz_radius"));
        extraEdgeColumnChance_ = opt<float>(cfg, "extra_edge_column_chance", 0.0f);
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        const int xRadius = xzRadius_->sample(r) + 1;
        const int zRadius = xzRadius_->sample(r) + 1;
        const std::vector<BlockPos> surface = placeGroundPatch(level, r, o, xRadius, zRadius);
        for (const BlockPos& p : surface)
            if (vegetationChance_ > 0.0f && r.nextFloat() < vegetationChance_) placeVegetation(level, r, p);
        return !surface.empty();
    }

protected:
    // The surface set in java.util.HashSet order.
    virtual std::vector<BlockPos> placeGroundPatch(GenRegion& level, RandomSource& r, const BlockPos& o, int xRadius, int zRadius) const {
        return groundPatch(level, r, o, xRadius, zRadius).toList();
    }
    virtual bool placeVegetation(GenRegion& level, RandomSource& r, const BlockPos& p) const {
        return env_->placePlaced(*vegetation_, level, r, rel(p, opposite(inwards_)));
    }

    JavaPosSet groundPatch(GenRegion& level, RandomSource& r, const BlockPos& o, int xRadius, int zRadius) const {
        const Direction outwards = opposite(inwards_);
        JavaPosSet surface;
        for (int dx = -xRadius; dx <= xRadius; ++dx) {
            const bool isXEdge = dx == -xRadius || dx == xRadius;
            for (int dz = -zRadius; dz <= zRadius; ++dz) {
                const bool isZEdge = dz == -zRadius || dz == zRadius;
                const bool isCorner = isXEdge && isZEdge;
                const bool isEdgeButNotCorner = (isXEdge || isZEdge) && !isCorner;
                if (isCorner) continue;
                if (isEdgeButNotCorner && !(extraEdgeColumnChance_ != 0.0f && !(r.nextFloat() > extraEdgeColumnChance_))) continue;
                BlockPos p = o.offset(dx, 0, dz);
                for (int i = 0; c_.air(level.block(p)) && i < verticalRange_; ++i) p = rel(p, inwards_);
                for (int i = 0; !c_.air(level.block(p)) && i < verticalRange_; ++i) p = rel(p, outwards);
                BlockPos belowPos = rel(p, inwards_);
                if (level.isEmpty(p) && c_.sturdy(level, belowPos, outwards)) {
                    const int depth =
                        depth_->sample(r) + (extraBottomBlockChance_ > 0.0f && r.nextFloat() < extraBottomBlockChance_ ? 1 : 0);
                    const BlockPos groundPos = belowPos;
                    if (placeGround(level, r, belowPos, depth)) surface.add(groundPos);
                }
            }
        }
        return surface;
    }

    Ctx c_;
    const FeatureEnv* env_;
    Direction inwards_ = Down;

private:
    bool placeGround(GenRegion& level, RandomSource& r, BlockPos& p, int depth) const {
        for (int i = 0; i < depth; ++i) {
            const u16 toPlace = groundState_->state(r, p);
            const u16 current = level.block(p);
            if (c_.name(toPlace) == c_.name(current)) continue;
            if (!replaceable_.test(c_, current)) return i != 0;
            level.setBlock(p, toPlace);
            p = rel(p, inwards_);
        }
        return true;
    }

    BlockSet replaceable_;
    StateP groundState_;
    PlacedP vegetation_;
    IntP depth_, xzRadius_;
    float extraBottomBlockChance_, vegetationChance_, extraEdgeColumnChance_;
    int verticalRange_;
};

class WaterloggedVegetationPatchFeature final : public VegetationPatchFeature {
public:
    WaterloggedVegetationPatchFeature(const json& cfg, const FeatureEnv& env) : VegetationPatchFeature(cfg, env) {
        water_ = c_.def("minecraft:water");
    }

protected:
    std::vector<BlockPos> placeGroundPatch(GenRegion& level, RandomSource& r, const BlockPos& o, int xRadius, int zRadius) const override {
        const JavaPosSet surface = groundPatch(level, r, o, xRadius, zRadius);
        JavaPosSet waterSurface;
        for (const BlockPos& p : surface.toList())
            if (!exposed(level, p)) waterSurface.add(p);
        const std::vector<BlockPos> ordered = waterSurface.toList();
        for (const BlockPos& p : ordered) level.setBlock(p, water_);
        return ordered;
    }
    bool placeVegetation(GenRegion& level, RandomSource& r, const BlockPos& p) const override {
        if (!VegetationPatchFeature::placeVegetation(level, r, below(p))) return false;
        const u16 placed = level.block(p);
        if (c_.prop(placed, "waterlogged") == "false") level.setBlock(p, c_.with(placed, "waterlogged", true));
        return true;
    }

private:
    bool exposed(const GenRegion& level, const BlockPos& p) const {
        for (Direction d : {North, East, South, West, Down})
            if (!c_.sturdy(level, rel(p, d), opposite(d))) return true;
        return false;
    }
    u16 water_ = 0;
};

// =============================================================================================
// Root system
// =============================================================================================
class RootSystemFeature final : public ConfiguredFeature {
public:
    RootSystemFeature(const json& cfg, const FeatureEnv& env) : c_(env), env_(&env) {
        tree_ = env.placed(req(cfg, "feature"));
        requiredVerticalSpace_ = opt<int>(cfg, "required_vertical_space_for_tree", 1);
        levelTestDistance_ = opt<int>(cfg, "level_test_distance", 0);
        maxLevelDeviation_ = opt<int>(cfg, "max_level_deviation", 0);
        rootRadius_ = opt<int>(cfg, "root_radius", 1);
        rootReplaceable_ = blockSet(req(cfg, "root_replaceable"));
        rootState_ = env.stateProvider(req(cfg, "root_state_provider"));
        rootAttempts_ = opt<int>(cfg, "root_placement_attempts", 1);
        rootColumnMaxHeight_ = opt<int>(cfg, "root_column_max_height", 1);
        hangingRootRadius_ = opt<int>(cfg, "hanging_root_radius", 1);
        hangingVerticalSpan_ = opt<int>(cfg, "hanging_roots_vertical_span", 1);
        hangingState_ = env.stateProvider(req(cfg, "hanging_root_state_provider"));
        hangingAttempts_ = opt<int>(cfg, "hanging_root_placement_attempts", 1);
        allowedVerticalWater_ = opt<int>(cfg, "allowed_vertical_water_for_tree", 1);
        allowedTreePosition_ = env.blockPredicate(req(cfg, "allowed_tree_position"));
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (!c_.air(level.block(o))) return false;
        BlockPos working = o;
        if (placeDirtAndTree(level, r, working, o)) placeRoots(level, r, o);
        return true;
    }

private:
    bool spaceForTree(const GenRegion& level, const BlockPos& p) const {
        BlockPos up = p;
        for (int i = 1; i <= requiredVerticalSpace_; ++i) {
            up = above(up);
            const u16 s = level.block(up);
            if (!(c_.air(s) || (i + 1 <= allowedVerticalWater_ && c_.fluidWater(s)))) return false;
        }
        if (levelTestDistance_ > 0) {
            static constexpr Direction kBy2D[4] = {South, West, North, East};
            for (int i = 0; i < 4; ++i) {
                const BlockPos corner = rel(p, kBy2D[i], levelTestDistance_);
                if (c_.air(level.block(below(corner, maxLevelDeviation_))) || !c_.air(level.block(above(corner, maxLevelDeviation_))))
                    return false;
            }
        }
        return true;
    }
    bool placeDirtAndTree(GenRegion& level, RandomSource& r, BlockPos& working, const BlockPos& o) const {
        for (int y = 0; y < rootColumnMaxHeight_; ++y) {
            working = above(working);
            if (level.height(Heightmap::WorldSurface, working.x, working.z) < working.y) return false;
            if (allowedTreePosition_->test(level, working) && spaceForTree(level, working)) {
                const BlockPos belowPos = below(working);
                const u16 b = level.block(belowPos);
                if (c_.fluidLava(b) || !c_.solid(b)) return false;
                if (env_->placePlaced(*tree_, level, r, working)) {
                    for (int yy = o.y; yy < o.y + y; ++yy) placeRootedDirt(level, r, BlockPos{o.x, yy, o.z});
                    return true;
                }
            }
        }
        return false;
    }
    void placeRootedDirt(GenRegion& level, RandomSource& r, BlockPos p) const {
        const int ox = p.x, oz = p.z;
        for (int i = 0; i < rootAttempts_; ++i) {
            const int a = r.nextInt(rootRadius_), b = r.nextInt(rootRadius_);
            const int cc = r.nextInt(rootRadius_), d = r.nextInt(rootRadius_);
            p = p.offset(a - b, 0, cc - d);
            if (rootReplaceable_.test(c_, level.block(p))) level.setBlock(p, rootState_->state(r, p));
            p.x = ox, p.z = oz;
        }
    }
    void placeRoots(GenRegion& level, RandomSource& r, const BlockPos& o) const {
        for (int i = 0; i < hangingAttempts_; ++i) {
            const int a = r.nextInt(hangingRootRadius_), b = r.nextInt(hangingRootRadius_);
            const int cc = r.nextInt(hangingVerticalSpan_), d = r.nextInt(hangingVerticalSpan_);
            const int e = r.nextInt(hangingRootRadius_), f = r.nextInt(hangingRootRadius_);
            const BlockPos p = o.offset(a - b, cc - d, e - f);
            if (!level.isEmpty(p)) continue;
            const u16 target = hangingState_->state(r, p);
            if (c_.canSurvive(level, target, p) && c_.sturdy(level, above(p), Down)) level.setBlock(p, target);
        }
    }

    Ctx c_;
    const FeatureEnv* env_;
    PlacedP tree_;
    int requiredVerticalSpace_, levelTestDistance_, maxLevelDeviation_, rootRadius_, rootAttempts_, rootColumnMaxHeight_;
    int hangingRootRadius_, hangingVerticalSpan_, hangingAttempts_, allowedVerticalWater_;
    BlockSet rootReplaceable_;
    StateP rootState_, hangingState_;
    PredP allowedTreePosition_;
};

// =============================================================================================
// Sculk patch (SculkSpreader with the world-gen settings, SculkBlock / SculkVeinBlock behaviour)
// =============================================================================================
class SculkPatchFeature final : public ConfiguredFeature {
public:
    SculkPatchFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        chargeCount_ = opt<int>(cfg, "charge_count", 1);
        amountPerCharge_ = opt<int>(cfg, "amount_per_charge", 1);
        spreadAttempts_ = opt<int>(cfg, "spread_attempts", 1);
        growthRounds_ = opt<int>(cfg, "growth_rounds", 0);
        spreadRounds_ = opt<int>(cfg, "spread_rounds", 0);
        sculk_ = c_.def("minecraft:sculk");
        vein_ = c_.def("minecraft:sculk_vein");
        air_ = c_.def("minecraft:air");
        water_ = c_.def("minecraft:water");
        sensor_ = c_.def("minecraft:sculk_sensor");
        shrieker_ = c_.with(c_.def("minecraft:sculk_shrieker"), "can_summon", true);
        // ChargeCursor.NON_CORNER_NEIGHBOURS
        for (int z = -1; z <= 1; ++z)
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                    if ((x == 0 || y == 0 || z == 0) && !(x == 0 && y == 0 && z == 0)) nonCorner_.push_back({x, y, z});
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (!canSpreadFrom(level, o)) return false;
        std::vector<Cursor> cursors;
        const int totalRounds = spreadRounds_ + growthRounds_;
        for (int round = 0; round < totalRounds; ++round) {
            for (int i = 0; i < chargeCount_; ++i)
                for (int charge = amountPerCharge_; charge > 0;) {  // addCursors
                    const int current = std::min(charge, 1000);
                    if (cursors.size() < 32) cursors.push_back(Cursor{o, current});
                    charge -= current;
                }
            const bool spreadVeins = round < spreadRounds_;
            for (int i = 0; i < spreadAttempts_; ++i) {
                if (cursors.empty()) continue;
                std::vector<Cursor> processed;
                for (Cursor& cursor : cursors) {
                    if (chessboard(cursor.pos, o) > 1024) continue;
                    update(level, cursor, o, r, spreadVeins);
                    if (cursor.charge > 0) processed.push_back(cursor);  // world gen never merges cursors
                }
                cursors = std::move(processed);
            }
            cursors.clear();
        }
        return true;
    }

private:
    enum class Kind { Default, Sculk, Vein };
    struct Cursor {
        BlockPos pos;
        int charge;
        int updateDelay = 0, decayDelay = 1;
        bool hasFacings = false;
        u8 facings = 0;  // bit per direction
    };
    // MultifaceSpreader.SpreadType
    enum class Spread { SamePosition, SamePlane, WrapAround };

    static int chessboard(const BlockPos& a, const BlockPos& b) { return std::max({iabs(a.x - b.x), iabs(a.y - b.y), iabs(a.z - b.z)}); }
    // Per state: kind and the six face flags, decoded once (spreading asks thousands of times per patch).
    u16 info(u16 s) const {
        u16 v = info_[s].load(std::memory_order_relaxed);
        if (v & 0x8000) return v;
        const std::string_view n = c_.name(s);
        v = 0x8000 | u16(n == "minecraft:sculk" ? 1 : n == "minecraft:sculk_vein" ? 2 : 0) << 6;
        for (Direction d : kAllDirs)
            if (c_.prop(s, kDirName[d]) == "true") v |= u16(1u << d);
        info_[s].store(v, std::memory_order_relaxed);
        return v;
    }
    Kind kind(u16 s) const {
        const int k = (info(s) >> 6) & 3;
        return k == 1 ? Kind::Sculk : k == 2 ? Kind::Vein : Kind::Default;
    }
    bool isVein(u16 s) const { return ((info(s) >> 6) & 3) == 2; }
    bool hasFace(u16 s, Direction d) const { return (info(s) >> d) & 1; }
    bool anyFace(u16 s) const {
        for (Direction d : kAllDirs)
            if (hasFace(s, d)) return true;
        return false;
    }

    bool canSpreadFrom(const GenRegion& level, const BlockPos& o) const {
        const u16 start = level.block(o);
        if (kind(start) != Kind::Default) return true;
        if (!c_.air(start) && !(c_.water(start) && c_.fluidSource(start))) return false;
        for (Direction d : kAllDirs)
            if (c_.fullCollision(level.block(rel(o, d)))) return true;
        return false;
    }

    // MultifaceBlock.canAttachTo: the neighbour's support or collision face is full.
    bool canAttachTo(const GenRegion& level, Direction towards, const BlockPos& neighbourPos) const {
        return c_.sturdy(level, neighbourPos, opposite(towards)) || c_.collisionFaceFull(level.block(neighbourPos), opposite(towards));
    }
    bool isValidStateForPlacement(const GenRegion& level, u16 oldState, const BlockPos& p, Direction d) const {
        if (isVein(oldState) && hasFace(oldState, d)) return false;
        return canAttachTo(level, d, rel(p, d));
    }
    // SculkVeinSpreaderConfig.stateCanBeReplaced
    bool stateCanBeReplaced(const GenRegion& level, const BlockPos& source, const BlockPos& p, Direction d, u16 existing) const {
        const u16 against = level.block(rel(p, d));
        const std::string_view an = c_.name(against);
        if (an == "minecraft:sculk" || an == "minecraft:sculk_catalyst" || an == "minecraft:moving_piston") return false;
        if (distManhattan(source, p) == 2) {
            const BlockPos neighbour = rel(source, opposite(d));
            if (c_.sturdy(level, neighbour, d)) return false;
        }
        if (!c_.fluidEmpty(existing) && !c_.sourceWater(existing)) return false;
        if (c_.tag(existing, "minecraft:fire")) return false;
        return c_.canBeReplaced(existing) || c_.air(existing) || isVein(existing) || (c_.water(existing) && c_.fluidSource(existing));
    }
    bool canSpreadInto(const GenRegion& level, const BlockPos& source, const BlockPos& p, Direction face) const {
        const u16 existing = level.block(p);
        return stateCanBeReplaced(level, source, p, face, existing) && isValidStateForPlacement(level, existing, p, face);
    }
    // MultifaceBlock.getStateForPlacement for the vein; 0 = null.
    std::optional<u16> stateForPlacement(const GenRegion& level, u16 oldState, const BlockPos& p, Direction d) const {
        if (!isValidStateForPlacement(level, oldState, p, d)) return std::nullopt;
        u16 s;
        if (isVein(oldState))
            s = oldState;
        else if (c_.sourceWater(oldState))
            s = c_.with(vein_, "waterlogged", true);
        else
            s = vein_;
        return c_.with(s, kDirName[d], true);
    }
    bool spreadFromFaceTowardDirection(GenRegion& level, u16 state, const BlockPos& p, Direction from, Direction spreadDir,
                                       const Spread* types, int typeCount) const {
        if (spreadDir / 2 == from / 2) return false;
        const bool otherValid = !isVein(state);
        if (!(otherValid || (hasFace(state, from) && !hasFace(state, spreadDir)))) return false;
        for (int t = 0; t < typeCount; ++t) {
            BlockPos sp;
            Direction face;
            switch (types[t]) {
            case Spread::SamePosition: sp = p, face = spreadDir; break;
            case Spread::SamePlane: sp = rel(p, spreadDir), face = from; break;
            default: sp = rel(rel(p, spreadDir), from), face = opposite(spreadDir); break;
            }
            if (canSpreadInto(level, p, sp, face)) {
                const std::optional<u16> placed = stateForPlacement(level, level.block(sp), sp, face);
                return placed && level.setBlock(sp, *placed);
            }
        }
        return false;
    }
    // MultifaceSpreader.spreadAll with the vein spreader (sameSpace = SAME_POSITION only).
    long long spreadAll(GenRegion& level, u16 state, const BlockPos& p, bool sameSpace) const {
        static constexpr Spread kOrder[3] = {Spread::SamePosition, Spread::SamePlane, Spread::WrapAround};
        const int count = sameSpace ? 1 : 3;  // the same-space spreader only tries SAME_POSITION
        long long total = 0;
        for (Direction face : kAllDirs) {
            if (!(!isVein(state) || hasFace(state, face))) continue;
            for (Direction spreadDir : kAllDirs)
                if (spreadFromFaceTowardDirection(level, state, p, face, spreadDir, kOrder, count)) ++total;
        }
        return total;
    }
    bool regrow(GenRegion& level, const BlockPos& p, u16 existing, u8 faces) const {
        bool any = false;
        u16 s = vein_;
        for (Direction d : kAllDirs)
            if ((faces >> d) & 1)
                if (canAttachTo(level, d, rel(p, d))) s = c_.with(s, kDirName[d], true), any = true;
        if (!any) return false;
        if (!c_.fluidEmpty(existing)) s = c_.with(s, "waterlogged", true);
        level.setBlock(p, s);
        return true;
    }
    bool attemptSpreadVein(GenRegion& level, Kind k, const BlockPos& p, u16 state, const Cursor& cursor) const {
        if (k == Kind::Default) {
            if (!cursor.hasFacings) return spreadAll(level, level.block(p), p, true) > 0;
            if (cursor.facings != 0) {
                if (!c_.air(state) && !c_.sourceWater(state)) return false;
                return regrow(level, p, state, cursor.facings);
            }
        }
        return spreadAll(level, state, p, false) > 0;
    }
    void veinDischarged(GenRegion& level, u16 state, const BlockPos& p) const {
        if (!isVein(state)) return;
        for (Direction d : kAllDirs)
            if (hasFace(state, d) && c_.is(level.block(rel(p, d)), "minecraft:sculk")) state = c_.with(state, kDirName[d], false);
        if (!anyFace(state)) state = c_.fluidEmpty(level.block(p)) ? air_ : water_;
        level.setBlock(p, state);
    }
    void onDischarged(GenRegion& level, Kind k, u16 state, const BlockPos& p) const {
        if (k == Kind::Vein) veinDischarged(level, state, p);
    }
    bool attemptPlaceSculk(GenRegion& level, const BlockPos& p, RandomSource& r) const {
        const u16 state = level.block(p);
        for (Direction support : allShuffled(r)) {
            if (!hasFace(state, support)) continue;
            const BlockPos supportPos = rel(p, support);
            if (!c_.tag(level.block(supportPos), "minecraft:sculk_replaceable_world_gen")) continue;
            level.setBlock(supportPos, sculk_);
            spreadAll(level, sculk_, supportPos, false);
            const Direction skip = opposite(support);
            for (Direction d : kAllDirs) {
                if (d == skip) continue;
                const BlockPos veinPos = rel(supportPos, d);
                const u16 possible = level.block(veinPos);
                if (isVein(possible)) veinDischarged(level, possible, veinPos);
            }
            return true;
        }
        return false;
    }
    bool canPlaceGrowth(const GenRegion& level, const BlockPos& p) const {
        const u16 a = level.block(above(p));
        if (!(c_.air(a) || (c_.water(a) && c_.sourceWater(a)))) return false;
        int count = 0;
        for (int y = p.y; y <= p.y + 2; ++y)
            for (int z = p.z - 4; z <= p.z + 4; ++z)
                for (int x = p.x - 4; x <= p.x + 4; ++x)
                    if (c_.tag(level.block(BlockPos{x, y, z}), "minecraft:sculk_growth_inhibitors") && ++count > 2) return false;
        return true;
    }
    int attemptUseCharge(GenRegion& level, Kind k, const Cursor& cursor, const BlockPos& origin, RandomSource& r, bool spreadVeins) const {
        constexpr int kChargeDecayRate = 5, kAdditionalDecayRate = 10, kGrowthSpawnCost = 50, kNoGrowthRadius = 1;
        if (k == Kind::Default) return cursor.decayDelay > 0 ? cursor.charge : 0;
        if (k == Kind::Vein) {
            if (spreadVeins && attemptPlaceSculk(level, cursor.pos, r)) return cursor.charge - 1;
            return r.nextInt(kChargeDecayRate) == 0 ? jfloor(double(float(cursor.charge) * 0.5f)) : cursor.charge;
        }
        const int charge = cursor.charge;
        if (charge == 0 || r.nextInt(kChargeDecayRate) != 0) return charge;
        const BlockPos chargePos = cursor.pos;
        const bool closeToCatalyst = distSqr(chargePos, origin) < double(kNoGrowthRadius) * kNoGrowthRadius;
        if (!closeToCatalyst && canPlaceGrowth(level, chargePos)) {
            if (r.nextInt(kGrowthSpawnCost) < charge) {
                const BlockPos growthPos = above(chargePos);
                u16 growth = r.nextInt(11) == 0 ? shrieker_ : sensor_;
                if (c_.hasProp(growth, "waterlogged") && !c_.fluidEmpty(level.block(growthPos))) growth = c_.with(growth, "waterlogged", true);
                level.setBlock(growthPos, growth);
            }
            return std::max(0, charge - kGrowthSpawnCost);
        }
        if (r.nextInt(kAdditionalDecayRate) != 0) return charge;
        if (closeToCatalyst) return charge - 1;
        const float outer = float(std::sqrt(distSqr(chargePos, origin))) - float(kNoGrowthRadius);
        const float outerSq = outer * outer;
        const int maxReachSq = (24 - kNoGrowthRadius) * (24 - kNoGrowthRadius);
        const float factor = std::min(1.0f, outerSq / float(maxReachSq));
        return charge - std::max(1, d2i(float(charge) * factor * 0.5f));
    }
    bool movementUnobstructed(const GenRegion& level, const BlockPos& from, const BlockPos& to) const {
        if (distManhattan(from, to) == 1) return true;
        const BlockPos d{to.x - from.x, to.y - from.y, to.z - from.z};
        const Direction dx = d.x < 0 ? West : East, dy = d.y < 0 ? Down : Up, dz = d.z < 0 ? North : South;
        auto unobstructed = [&](Direction dir) { return !c_.sturdy(level, rel(from, dir), opposite(dir)); };
        if (d.x == 0) return unobstructed(dy) || unobstructed(dz);
        return d.y == 0 ? unobstructed(dx) || unobstructed(dz) : unobstructed(dx) || unobstructed(dy);
    }
    bool hasSubstrateAccess(const GenRegion& level, u16 s, const BlockPos& p) const {
        if (!isVein(s)) return false;
        for (Direction d : kAllDirs)
            if (hasFace(s, d) && c_.tag(level.block(rel(p, d)), "minecraft:sculk_replaceable")) return true;
        return false;
    }
    std::optional<BlockPos> validMovementPos(const GenRegion& level, const BlockPos& p, RandomSource& r, const BlockPos& origin) const {
        std::vector<BlockPos> offsets = nonCorner_;
        jshuffle(offsets.data(), int(offsets.size()), r);
        BlockPos result = p;
        for (const BlockPos& off : offsets) {
            const BlockPos n = p.offset(off.x, off.y, off.z);
            const int dx = origin.x - n.x, dz = origin.z - n.z;
            if (dx * dx + dz * dz > 144) continue;
            const u16 t = level.block(n);
            if (kind(t) != Kind::Default && movementUnobstructed(level, p, n)) {
                result = n;
                if (hasSubstrateAccess(level, t, n)) break;
            }
        }
        if (result == p) return std::nullopt;
        return result;
    }
    void update(GenRegion& level, Cursor& cursor, const BlockPos& origin, RandomSource& r, bool spreadVeins) const {
        if (cursor.charge <= 0) return;
        if (cursor.updateDelay > 0) {
            --cursor.updateDelay;
            return;
        }
        u16 current = level.block(cursor.pos);
        Kind k = kind(current);
        if (spreadVeins && attemptSpreadVein(level, k, cursor.pos, current, cursor)) {
            if (k != Kind::Sculk) {  // canChangeBlockStateOnSpread
                current = level.block(cursor.pos);
                k = kind(current);
            }
        }
        cursor.charge = attemptUseCharge(level, k, cursor, origin, r, spreadVeins);
        if (cursor.charge <= 0) {
            onDischarged(level, k, current, cursor.pos);
            return;
        }
        const std::optional<BlockPos> transfer = validMovementPos(level, cursor.pos, r, origin);
        if (transfer) {
            onDischarged(level, k, current, cursor.pos);
            cursor.pos = *transfer;
            current = level.block(*transfer);
        } else {
            onDischarged(level, k, current, cursor.pos);
            cursor.charge = 0;
            return;
        }
        if (kind(current) != Kind::Default) {
            cursor.hasFacings = true;
            cursor.facings = 0;
            if (isVein(current))
                for (Direction d : kAllDirs)
                    if (hasFace(current, d)) cursor.facings |= u8(1 << d);
        }
        cursor.decayDelay = k == Kind::Default ? std::max(cursor.decayDelay - 1, 0) : 1;
        cursor.updateDelay = 1;
    }

    Ctx c_;
    int chargeCount_, amountPerCharge_, spreadAttempts_, growthRounds_, spreadRounds_;
    u16 sculk_, vein_, air_, water_, sensor_, shrieker_;
    std::unique_ptr<std::atomic<u16>[]> info_ = std::make_unique<std::atomic<u16>[]>(65536);
    std::vector<BlockPos> nonCorner_;
};

// =============================================================================================
// Lake
// =============================================================================================
class LakeFeature final : public ConfiguredFeature {
public:
    LakeFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        fluid_ = env.stateProvider(req(cfg, "fluid"));
        barrier_ = env.stateProvider(req(cfg, "barrier"));
        canPlace_ = env.blockPredicate(req(cfg, "can_place_feature"));
        canReplaceWithAirOrFluid_ = env.blockPredicate(req(cfg, "can_replace_with_air_or_fluid"));
        canReplaceWithBarrier_ = env.blockPredicate(req(cfg, "can_replace_with_barrier"));
        caveAir_ = c_.def("minecraft:cave_air");
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        if (origin.y <= level.minY() + 4) return false;
        const BlockPos o = origin.offset(-8, -4, -8);
        bool grid[2048] = {};
        const int spots = r.nextInt(4) + 4;
        for (int i = 0; i < spots; ++i) {
            const double xr = r.nextDouble() * 6.0 + 3.0;
            const double yr = r.nextDouble() * 4.0 + 2.0;
            const double zr = r.nextDouble() * 6.0 + 3.0;
            const double xp = r.nextDouble() * (16.0 - xr - 2.0) + 1.0 + xr / 2.0;
            const double yp = r.nextDouble() * (8.0 - yr - 4.0) + 2.0 + yr / 2.0;
            const double zp = r.nextDouble() * (16.0 - zr - 2.0) + 1.0 + zr / 2.0;
            for (int xx = 1; xx < 15; ++xx)
                for (int zz = 1; zz < 15; ++zz)
                    for (int yy = 1; yy < 7; ++yy) {
                        const double xd = (xx - xp) / (xr / 2.0), yd = (yy - yp) / (yr / 2.0), zd = (zz - zp) / (zr / 2.0);
                        if (xd * xd + yd * yd + zd * zd < 1.0) grid[(xx * 16 + zz) * 8 + yy] = true;
                    }
        }
        auto g = [&](int x, int z, int y) { return grid[(x * 16 + z) * 8 + y]; };
        auto border = [&](int x, int z, int y) {
            return !g(x, z, y) && ((x < 15 && g(x + 1, z, y)) || (x > 0 && g(x - 1, z, y)) || (z < 15 && g(x, z + 1, y)) ||
                                   (z > 0 && g(x, z - 1, y)) || (y < 7 && g(x, z, y + 1)) || (y > 0 && g(x, z, y - 1)));
        };
        const u16 fluid = fluid_->state(r, o);
        for (int xx = 0; xx < 16; ++xx)
            for (int zz = 0; zz < 16; ++zz)
                for (int yy = 0; yy < 8; ++yy) {
                    if (!border(xx, zz, yy)) continue;
                    const BlockPos p = o.offset(xx, yy, zz);
                    const u16 s = level.block(p);
                    if (yy >= 4 && c_.liquidBlock(s)) return false;
                    if (yy < 4 && !c_.solid(s) && s != fluid) return false;
                    if (!canPlace_->test(level, p)) return false;
                }
        for (int xx = 0; xx < 16; ++xx)
            for (int zz = 0; zz < 16; ++zz)
                for (int yy = 0; yy < 8; ++yy) {
                    if (!g(xx, zz, yy)) continue;
                    const BlockPos p = o.offset(xx, yy, zz);
                    if (canReplaceWithAirOrFluid_->test(level, p)) level.setBlock(p, yy >= 4 ? caveAir_ : fluid);
                }
        const u16 barrier = barrier_->state(r, o);
        if (!c_.air(barrier))
            for (int xx = 0; xx < 16; ++xx)
                for (int zz = 0; zz < 16; ++zz)
                    for (int yy = 0; yy < 8; ++yy) {
                        if (!border(xx, zz, yy) || !(yy < 4 || r.nextInt(2) != 0)) continue;
                        const BlockPos p = o.offset(xx, yy, zz);
                        if (c_.solid(level.block(p)) && canReplaceWithBarrier_->test(level, p)) level.setBlock(p, barrier);
                    }
        // Water lakes then freeze their top layer in cold biomes (Biome.shouldFreeze); that needs the
        // biome climate, which the region does not expose, and only matters where a water block sits in
        // the lake's y+4 layer, so it is not done here.
        return true;
    }

private:
    Ctx c_;
    StateP fluid_, barrier_;
    PredP canPlace_, canReplaceWithAirOrFluid_, canReplaceWithBarrier_;
    u16 caveAir_;
};

// =============================================================================================
// Spring, iceberg, blue ice, underwater magma
// =============================================================================================
u16 fluidLegacyBlock(const Ctx& c, const json& j) {  // FluidState.createLegacyBlock
    std::string id = j.is_string() ? j.get<std::string>() : j.value("id", j.value("Name", std::string("minecraft:empty")));
    if (id.find(':') == std::string::npos) id = "minecraft:" + id;
    const json props = j.is_object() ? j.value("properties", j.value("Properties", json::object())) : json::object();
    std::string block;
    int level = 0;
    if (id == "minecraft:water" || id == "minecraft:lava") {
        block = id;
    } else if (id == "minecraft:flowing_water" || id == "minecraft:flowing_lava") {
        block = id == "minecraft:flowing_water" ? "minecraft:water" : "minecraft:lava";
        auto str = [&](const char* k, const char* d) {
            auto it = props.find(k);
            return it == props.end() ? std::string(d) : it->is_string() ? it->get<std::string>() : it->dump();
        };
        const int amount = std::stoi(str("level", "8"));
        level = 8 - std::min(amount, 8) + (str("falling", "false") == "true" ? 8 : 0);
    } else {
        return c.def("minecraft:air");
    }
    return c.with(c.def(block), "level", std::to_string(level));
}

class SpringFeature final : public ConfiguredFeature {
public:
    SpringFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        state_ = fluidLegacyBlock(c_, req(cfg, "state"));
        requiresBlockBelow_ = opt<bool>(cfg, "requires_block_below", true);
        rockCount_ = opt<int>(cfg, "rock_count", 4);
        holeCount_ = opt<int>(cfg, "hole_count", 1);
        valid_ = blockSet(req(cfg, "valid_blocks"));
    }
    bool place(GenRegion& level, RandomSource&, const BlockPos& o) const override {
        auto valid = [&](const BlockPos& p) { return valid_.test(c_, level.block(p)); };
        if (!valid(above(o))) return false;
        if (requiresBlockBelow_ && !valid(below(o))) return false;
        const u16 current = level.block(o);
        if (!c_.air(current) && !valid_.test(c_, current)) return false;
        const BlockPos sides[5] = {rel(o, West), rel(o, East), rel(o, North), rel(o, South), below(o)};
        int rocks = 0, holes = 0;
        for (const BlockPos& p : sides) rocks += valid(p) ? 1 : 0;
        for (const BlockPos& p : sides) holes += level.isEmpty(p) ? 1 : 0;
        if (rocks != rockCount_ || holes != holeCount_) return false;
        level.setBlock(o, state_);  // (plus a scheduled fluid tick)
        return true;
    }

private:
    Ctx c_;
    u16 state_;
    bool requiresBlockBelow_;
    int rockCount_, holeCount_;
    BlockSet valid_;
};

class IcebergFeature final : public ConfiguredFeature {
public:
    IcebergFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        state_ = env.state(req(cfg, "state"));
        snowBlock_ = c_.def("minecraft:snow_block");
        air_ = c_.def("minecraft:air");
        water_ = c_.def("minecraft:water");
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        const BlockPos o{origin.x, level.seaLevel(), origin.z};
        const bool snowOnTop = r.nextDouble() > 0.7;
        const double shapeAngle = r.nextDouble() * 2.0 * kPi;
        const int shapeEllipseA = 11 - r.nextInt(5);
        const int shapeEllipseC = 3 + r.nextInt(3);
        const bool isEllipse = r.nextDouble() > 0.7;
        int overWaterHeight = isEllipse ? r.nextInt(6) + 6 : r.nextInt(15) + 3;
        if (!isEllipse && r.nextDouble() > 0.9) overWaterHeight += r.nextInt(19) + 7;
        const int underWaterHeight = std::min(overWaterHeight + r.nextInt(11), 18);
        int width;
        {
            const int a = r.nextInt(7);
            const int b = r.nextInt(5);
            width = std::min(overWaterHeight + a - b, 11);
        }
        const int a = isEllipse ? shapeEllipseA : 11;
        for (int xo = -a; xo < a; ++xo)
            for (int zo = -a; zo < a; ++zo)
                for (int yOff = 0; yOff < overWaterHeight; ++yOff) {
                    const int radius = isEllipse ? radiusEllipse(yOff, overWaterHeight, width) : radiusRound(r, yOff, overWaterHeight, width);
                    if (isEllipse || xo < radius)
                        generateBlock(level, r, o, overWaterHeight, xo, yOff, zo, radius, a, isEllipse, shapeEllipseC, shapeAngle, snowOnTop);
                }
        smooth(level, o, width, overWaterHeight, isEllipse, shapeEllipseA);
        for (int xo = -a; xo < a; ++xo)
            for (int zo = -a; zo < a; ++zo)
                for (int yOff = -1; yOff > -underWaterHeight; --yOff) {
                    const int newA =
                        isEllipse ? jceil(float(a) * (1.0f - float(double(yOff) * double(yOff)) / (float(underWaterHeight) * 8.0f))) : a;
                    const int radius = radiusSteep(r, -yOff, underWaterHeight, width);
                    if (xo < radius)
                        generateBlock(level, r, o, underWaterHeight, xo, yOff, zo, radius, newA, isEllipse, shapeEllipseC, shapeAngle, snowOnTop);
                }
        const bool doCutOut = isEllipse ? r.nextDouble() > 0.1 : r.nextDouble() > 0.7;
        if (doCutOut) cutOut(r, level, width, overWaterHeight, o, isEllipse, shapeEllipseA, shapeAngle, shapeEllipseC);
        return true;
    }

private:
    bool icebergState(u16 s) const {
        return c_.is(s, "minecraft:packed_ice") || c_.is(s, "minecraft:snow_block") || c_.is(s, "minecraft:blue_ice");
    }
    void cutOut(RandomSource& r, GenRegion& level, int width, int height, const BlockPos& origin, bool isEllipse, int shapeEllipseA,
                double shapeAngle, int shapeEllipseC) const {
        const int signX = r.nextBoolean() ? -1 : 1;
        const int signZ = r.nextBoolean() ? -1 : 1;
        int xOff = r.nextInt(std::max(width / 2 - 2, 1));
        if (r.nextBoolean()) xOff = width / 2 + 1 - r.nextInt(std::max(width - width / 2 - 1, 1));
        int zOff = r.nextInt(std::max(width / 2 - 2, 1));
        if (r.nextBoolean()) zOff = width / 2 + 1 - r.nextInt(std::max(width - width / 2 - 1, 1));
        if (isEllipse) xOff = zOff = r.nextInt(std::max(shapeEllipseA - 5, 1));
        const BlockPos local{signX * xOff, 0, signZ * zOff};
        const double angle = isEllipse ? shapeAngle + kPi / 2 : r.nextDouble() * 2.0 * kPi;
        for (int yOff = 0; yOff < height - 3; ++yOff) {
            const int radius = radiusRound(r, yOff, height, width);
            carve(radius, yOff, origin, level, false, angle, local, shapeEllipseA, shapeEllipseC);
        }
        for (int yOff = -1; yOff > -height + r.nextInt(5); --yOff) {
            const int radius = radiusSteep(r, -yOff, height, width);
            carve(radius, yOff, origin, level, true, angle, local, shapeEllipseA, shapeEllipseC);
        }
    }
    void carve(int radius, int yOff, const BlockPos& origin, GenRegion& level, bool underWater, double angle, const BlockPos& local,
               int shapeEllipseA, int shapeEllipseC) const {
        const int a = radius + 1 + shapeEllipseA / 3;
        const int c = std::min(radius - 3, 3) + shapeEllipseC / 2 - 1;
        for (int xo = -a; xo < a; ++xo)
            for (int zo = -a; zo < a; ++zo) {
                if (!(ellipseDistance(xo, zo, local, a, c, angle) < 0.0)) continue;
                const BlockPos p = origin.offset(xo, yOff, zo);
                const u16 s = level.block(p);
                if (!icebergState(s) && !c_.is(s, "minecraft:snow_block")) continue;
                if (underWater) {
                    level.setBlock(p, water_);
                } else {
                    level.setBlock(p, air_);
                    if (c_.is(level.block(above(p)), "minecraft:snow")) level.setBlock(above(p), air_);
                }
            }
    }
    void generateBlock(GenRegion& level, RandomSource& r, const BlockPos& origin, int height, int xo, int yOff, int zo, int radius, int a,
                       bool isEllipse, int shapeEllipseC, double shapeAngle, bool snowOnTop) const {
        const double dist = isEllipse ? ellipseDistance(xo, zo, BlockPos{}, a, ellipseC(yOff, height, shapeEllipseC), shapeAngle)
                                      : circleDistance(xo, zo, radius, r);
        if (!(dist < 0.0)) return;
        const BlockPos p = origin.offset(xo, yOff, zo);
        const double compareVal = isEllipse ? -0.5 : double(-6 - r.nextInt(3));
        if (dist > compareVal && r.nextDouble() > 0.9) return;
        setIcebergBlock(p, level, r, height - yOff, height, isEllipse, snowOnTop);
    }
    void setIcebergBlock(const BlockPos& p, GenRegion& level, RandomSource& r, int hDiff, int height, bool isEllipse, bool snowOnTop) const {
        const u16 s = level.block(p);
        if (!(c_.air(s) || c_.is(s, "minecraft:snow_block") || c_.is(s, "minecraft:ice") || c_.water(s))) return;
        const bool randomness = !isEllipse || r.nextDouble() > 0.05;
        const int divisor = isEllipse ? 3 : 2;
        if (snowOnTop && !c_.water(s) && double(hDiff) <= double(r.nextInt(std::max(1, height / divisor))) + height * 0.6 && randomness)
            level.setBlock(p, snowBlock_);
        else
            level.setBlock(p, state_);
    }
    static int ellipseC(int yOff, int height, int shapeEllipseC) {
        int c = shapeEllipseC;
        if (yOff > 0 && height - yOff <= 3) c = shapeEllipseC - (4 - (height - yOff));
        return c;
    }
    static double circleDistance(int xo, int zo, int radius, RandomSource& r) {
        const float off = 10.0f * clampf(r.nextFloat(), 0.2f, 0.8f) / float(radius);
        return double(off) + double(xo) * double(xo) + double(zo) * double(zo) - double(radius) * double(radius);
    }
    static double ellipseDistance(int xo, int zo, const BlockPos& o, int a, int c, double angle) {
        const double u = ((xo - o.x) * std::cos(angle) - (zo - o.z) * std::sin(angle)) / a;
        const double v = ((xo - o.x) * std::sin(angle) + (zo - o.z) * std::cos(angle)) / c;
        return u * u + v * v - 1.0;
    }
    static int radiusRound(RandomSource& r, int yOff, int height, int width) {
        const float k = 3.5f - r.nextFloat();
        float scale = (1.0f - float(double(yOff) * double(yOff)) / (float(height) * k)) * float(width);
        if (height > 15 + r.nextInt(5)) {
            const int tempYOff = yOff < 3 + r.nextInt(6) ? yOff / 2 : yOff;
            scale = (1.0f - float(tempYOff) / (float(height) * k * 0.4f)) * float(width);
        }
        return jceil(scale / 2.0f);
    }
    static int radiusEllipse(int yOff, int height, int width) {
        const float scale = (1.0f - float(double(yOff) * double(yOff)) / (float(height) * 1.0f)) * float(width);
        return jceil(scale / 2.0f);
    }
    static int radiusSteep(RandomSource& r, int yOff, int height, int width) {
        const float k = 1.0f + r.nextFloat() / 2.0f;
        const float scale = (1.0f - float(yOff) / (float(height) * k)) * float(width);
        return jceil(scale / 2.0f);
    }
    void smooth(GenRegion& level, const BlockPos& origin, int width, int height, bool isEllipse, int shapeEllipseA) const {
        const int a = isEllipse ? shapeEllipseA : width / 2;
        for (int x = -a; x <= a; ++x)
            for (int z = -a; z <= a; ++z)
                for (int yOff = 0; yOff <= height; ++yOff) {
                    const BlockPos p = origin.offset(x, yOff, z);
                    const u16 s = level.block(p);
                    if (!icebergState(s) && !c_.is(s, "minecraft:snow")) continue;
                    if (c_.air(level.block(below(p)))) {
                        level.setBlock(p, air_);
                        level.setBlock(above(p), air_);
                    } else if (icebergState(s)) {
                        const u16 sides[4] = {level.block(rel(p, West)), level.block(rel(p, East)), level.block(rel(p, North)),
                                              level.block(rel(p, South))};
                        int counter = 0;
                        for (u16 side : sides)
                            if (!icebergState(side)) ++counter;
                        if (counter >= 3) level.setBlock(p, air_);
                    }
                }
    }

    Ctx c_;
    u16 state_, snowBlock_, air_, water_;
};

class BlueIceFeature final : public ConfiguredFeature {
public:
    BlueIceFeature(const json&, const FeatureEnv& env) : c_(env) { blueIce_ = c_.def("minecraft:blue_ice"); }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (o.y > level.seaLevel() - 1) return false;
        if (!c_.water(level.block(o)) && !c_.water(level.block(below(o)))) return false;
        bool foundPackedIce = false;
        for (Direction d : kAllDirs)
            if (d != Down && c_.is(level.block(rel(o, d)), "minecraft:packed_ice")) {
                foundPackedIce = true;
                break;
            }
        if (!foundPackedIce) return false;
        level.setBlock(o, blueIce_);
        for (int i = 0; i < 200; ++i) {
            const int a = r.nextInt(5);
            const int yOff = a - r.nextInt(6);
            int xzDiff = 3;
            if (yOff < 2) xzDiff += yOff / 2;
            if (xzDiff < 1) continue;
            const int x1 = r.nextInt(xzDiff), x2 = r.nextInt(xzDiff);
            const int z1 = r.nextInt(xzDiff), z2 = r.nextInt(xzDiff);
            const BlockPos p = o.offset(x1 - x2, yOff, z1 - z2);
            const u16 s = level.block(p);
            if (!(c_.air(s) || c_.water(s) || c_.is(s, "minecraft:packed_ice") || c_.is(s, "minecraft:ice"))) continue;
            for (Direction d : kAllDirs)
                if (c_.is(level.block(rel(p, d)), "minecraft:blue_ice")) {
                    level.setBlock(p, blueIce_);
                    break;
                }
        }
        return true;
    }

private:
    Ctx c_;
    u16 blueIce_;
};

// =============================================================================================
// Coral
// =============================================================================================
class CoralTreeFeature final : public ConfiguredFeature {
public:
    CoralTreeFeature(const json& cfg, const FeatureEnv& env) : env_(&env), feature_(env.placed(req(cfg, "feature"))) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        BlockPos p = o;
        const int trunkHeight = r.nextInt(3) + 1;
        for (int i = 0; i < trunkHeight; ++i) {
            if (!env_->placePlaced(*feature_, level, r, p)) return true;
            p = above(p);
        }
        const BlockPos top = p;
        const int nBranches = r.nextInt(3) + 2;
        Direction dirs[4] = {North, East, South, West};
        jshuffle(dirs, 4, r);
        for (int b = 0; b < nBranches; ++b) {
            const Direction branch = dirs[b];
            p = rel(top, branch);
            const int branchHeight = r.nextInt(5) + 2;
            int segment = 0;
            for (int j = 0; j < branchHeight && env_->placePlaced(*feature_, level, r, p); ++j) {
                ++segment;
                p = above(p);
                if (j == 0 || (segment >= 2 && r.nextFloat() < 0.25f)) {
                    p = rel(p, branch);
                    segment = 0;
                }
            }
        }
        return true;
    }

private:
    const FeatureEnv* env_;
    PlacedP feature_;
};

class CoralClawFeature final : public ConfiguredFeature {
public:
    CoralClawFeature(const json& cfg, const FeatureEnv& env) : env_(&env), feature_(env.placed(req(cfg, "feature"))) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (!env_->placePlaced(*feature_, level, r, o)) return false;
        const Direction claw = kHorizontal[r.nextInt(4)];
        const int nBranches = r.nextInt(2) + 2;
        Direction possible[3] = {claw, clockWise(claw), counterClockWise(claw)};
        jshuffle(possible, 3, r);
        for (int b = 0; b < nBranches; ++b) {
            const Direction branch = possible[b];
            BlockPos p = o;
            const int sideways = r.nextInt(2) + 1;
            p = rel(p, branch);
            int inway;
            Direction segment;
            if (branch == claw) {
                segment = claw;
                inway = r.nextInt(3) + 2;
            } else {
                p = above(p);
                const Direction options[2] = {branch, Up};
                segment = options[r.nextInt(2)];
                inway = r.nextInt(3) + 3;
            }
            for (int i = 0; i < sideways && env_->placePlaced(*feature_, level, r, p); ++i) p = rel(p, segment);
            p = rel(p, opposite(segment));
            p = above(p);
            for (int i = 0; i < inway; ++i) {
                p = rel(p, claw);
                if (!env_->placePlaced(*feature_, level, r, p)) break;
                if (r.nextFloat() < 0.25f) p = above(p);
            }
        }
        return true;
    }

private:
    const FeatureEnv* env_;
    PlacedP feature_;
};

// =============================================================================================
// Dungeon
// =============================================================================================
// RandomizableContainer block entities: placing one consumes a loot seed (random.nextLong()).
bool isLootContainer(std::string_view n) {
    return n == "minecraft:chest" || n == "minecraft:trapped_chest" || n.ends_with("copper_chest") || n == "minecraft:barrel" ||
           n == "minecraft:dispenser" || n == "minecraft:dropper" || n == "minecraft:hopper" || n.ends_with("shulker_box") ||
           n == "minecraft:decorated_pot" || n == "minecraft:crafter";
}

class MonsterRoomFeature final : public ConfiguredFeature {
public:
    MonsterRoomFeature(const json&, const FeatureEnv& env) : c_(env) {
        caveAir_ = c_.def("minecraft:cave_air");
        cobble_ = c_.def("minecraft:cobblestone");
        mossy_ = c_.def("minecraft:mossy_cobblestone");
        chest_ = c_.def("minecraft:chest");
        spawner_ = c_.def("minecraft:spawner");
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        auto replaceable = [&](u16 s) { return !c_.tag(s, "minecraft:features_cannot_replace"); };
        auto safeSet = [&](const BlockPos& p, u16 s) {
            if (replaceable(level.block(p))) level.setBlock(p, s);
        };
        const int xr = r.nextInt(2) + 2;
        const int minX = -xr - 1, maxX = xr + 1;
        const int zr = r.nextInt(2) + 2;
        const int minZ = -zr - 1, maxZ = zr + 1;
        int holeCount = 0;
        for (int dx = minX; dx <= maxX; ++dx)
            for (int dy = -1; dy <= 4; ++dy)
                for (int dz = minZ; dz <= maxZ; ++dz) {
                    const BlockPos p = o.offset(dx, dy, dz);
                    const bool solid = c_.solid(level.block(p));
                    if ((dy == -1 || dy == 4) && !solid) return false;
                    if ((dx == minX || dx == maxX || dz == minZ || dz == maxZ) && dy == 0 && level.isEmpty(p) && level.isEmpty(above(p)))
                        ++holeCount;
                }
        if (holeCount < 1 || holeCount > 5) return false;
        for (int dx = minX; dx <= maxX; ++dx)
            for (int dy = 3; dy >= -1; --dy)
                for (int dz = minZ; dz <= maxZ; ++dz) {
                    const BlockPos p = o.offset(dx, dy, dz);
                    const u16 s = level.block(p);
                    if (dx == minX || dy == -1 || dz == minZ || dx == maxX || dy == 4 || dz == maxZ) {
                        if (p.y >= level.minY() && !c_.solid(level.block(below(p)))) {
                            level.setBlock(p, caveAir_);
                        } else if (c_.solid(s) && !c_.is(s, "minecraft:chest")) {
                            if (dy == -1 && r.nextInt(4) != 0)
                                safeSet(p, mossy_);
                            else
                                safeSet(p, cobble_);
                        }
                    } else if (!c_.is(s, "minecraft:chest") && !c_.is(s, "minecraft:spawner")) {
                        safeSet(p, caveAir_);
                    }
                }
        for (int cc = 0; cc < 2; ++cc)
            for (int i = 0; i < 3; ++i) {
                const int xc = o.x + r.nextInt(xr * 2 + 1) - xr;
                const int zc = o.z + r.nextInt(zr * 2 + 1) - zr;
                const BlockPos chestPos{xc, o.y, zc};
                if (!level.isEmpty(chestPos)) continue;
                int wallCount = 0;
                for (Direction d : kHorizontal)
                    if (c_.solid(level.block(rel(chestPos, d)))) ++wallCount;
                if (wallCount != 1) continue;
                safeSet(chestPos, reorient(level, chestPos, chest_));
                if (c_.is(level.block(chestPos), "minecraft:chest")) r.nextLong();  // loot table seed
                break;
            }
        safeSet(o, spawner_);
        if (c_.is(level.block(o), "minecraft:spawner")) r.nextInt(4);  // spawner mob (skeleton, zombie x2, spider)
        return true;
    }

private:
    u16 reorient(const GenRegion& level, const BlockPos& p, u16 chest) const {  // StructurePiece.reorient
        std::optional<Direction> solidNeighbor;
        for (Direction d : kHorizontal) {
            const u16 s = level.block(rel(p, d));
            if (c_.is(s, "minecraft:chest")) return chest;
            if (c_.solidRender(s)) {
                if (solidNeighbor) {
                    solidNeighbor.reset();
                    break;
                }
                solidNeighbor = d;
            }
        }
        if (solidNeighbor) return c_.with(chest, "facing", kDirName[opposite(*solidNeighbor)]);
        Direction lock = dirFromName(c_.prop(chest, "facing")).value_or(North);
        if (c_.solidRender(level.block(rel(p, lock)))) lock = opposite(lock);
        if (c_.solidRender(level.block(rel(p, lock)))) lock = clockWise(lock);
        if (c_.solidRender(level.block(rel(p, lock)))) lock = opposite(lock);
        return c_.with(chest, "facing", kDirName[lock]);
    }

    Ctx c_;
    u16 caveAir_, cobble_, mossy_, chest_, spawner_;
};

// =============================================================================================
// Structure templates (data/minecraft/structure/*.nbt) for fossils and the "template" feature
// =============================================================================================
struct Nbt {
    u8 type = 0;
    i64 i = 0;
    double d = 0;
    std::string s;
    std::vector<Nbt> list;
    std::vector<std::pair<std::string, Nbt>> compound;
    const Nbt* get(std::string_view k) const {
        for (const auto& [key, v] : compound)
            if (key == k) return &v;
        return nullptr;
    }
};

class NbtReader {
public:
    NbtReader(const u8* p, size_t n) : p_(p), end_(p + n) {}
    bool readRoot(Nbt& out) {
        const u8 t = u1();
        if (t != 10) return false;
        str();
        read(10, out, 0);
        return ok_;
    }

private:
    u8 u1() {
        if (p_ >= end_) return ok_ = false, 0;
        return *p_++;
    }
    u64 be(int n) {
        u64 v = 0;
        for (int k = 0; k < n; ++k) v = (v << 8) | u1();
        return v;
    }
    std::string str() {
        const size_t n = size_t(be(2));
        if (size_t(end_ - p_) < n) return ok_ = false, std::string();
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    void read(u8 t, Nbt& out, int depth) {
        out.type = t;
        if (!ok_ || depth > 64) return void(ok_ = false);
        switch (t) {
        case 1: out.i = i8(u1()); break;
        case 2: out.i = i16(be(2)); break;
        case 3: out.i = i32(be(4)); break;
        case 4: out.i = i64(be(8)); break;
        case 5: {
            const u32 bits = u32(be(4));
            float f;
            std::memcpy(&f, &bits, 4);
            out.d = f;
            break;
        }
        case 6: {
            const u64 bits = be(8);
            std::memcpy(&out.d, &bits, 8);
            break;
        }
        case 7: case 11: case 12: {
            const i32 n = i32(be(4));
            const int w = t == 7 ? 1 : t == 11 ? 4 : 8;
            for (i32 k = 0; k < n && ok_; ++k) {
                Nbt e;
                e.type = t == 7 ? 1 : t == 11 ? 3 : 4;
                e.i = w == 1 ? i64(i8(u1())) : w == 4 ? i64(i32(be(4))) : i64(be(8));
                out.list.push_back(std::move(e));
            }
            break;
        }
        case 8: out.s = str(); break;
        case 9: {
            const u8 et = u1();
            const i32 n = i32(be(4));
            for (i32 k = 0; k < n && ok_; ++k) {
                Nbt e;
                read(et, e, depth + 1);
                out.list.push_back(std::move(e));
            }
            break;
        }
        case 10:
            for (;;) {
                const u8 et = u1();
                if (!ok_ || et == 0) break;
                std::string key = str();
                Nbt e;
                read(et, e, depth + 1);
                out.compound.emplace_back(std::move(key), std::move(e));
            }
            break;
        default: ok_ = false;
        }
    }
    const u8* p_;
    const u8* end_;
    bool ok_ = true;
};

// Rotation: NONE, CLOCKWISE_90, CLOCKWISE_180, COUNTERCLOCKWISE_90 (the game's ordinal order).
Direction rotateDir(Direction d, int rot) {
    if (d == Down || d == Up) return d;
    switch (rot) {
    case 1: return clockWise(d);
    case 2: return opposite(d);
    case 3: return counterClockWise(d);
    default: return d;
    }
}
BlockPos rotatePos(const BlockPos& p, int rot) {  // StructureTemplate.transform, pivot 0, no mirror
    switch (rot) {
    case 1: return {-p.z, p.y, p.x};
    case 2: return {-p.x, p.y, -p.z};
    case 3: return {p.z, p.y, -p.x};
    default: return p;
    }
}
int rotationFromName(std::string_view n) {
    if (n == "clockwise_90") return 1;
    if (n == "180") return 2;
    if (n == "counterclockwise_90") return 3;
    return 0;
}

// BlockState.rotate for the properties templates use (facing, axis, rotation, side connections).
using Rotated = std::array<u16, 4>;
Rotated rotations(const Ctx& c, u16 id) {
    Rotated out{id, id, id, id};
    for (int rot = 1; rot < 4; ++rot) {
        u16 s = id;
        if (auto f = dirFromName(c.prop(id, "facing")); f) s = c.with(s, "facing", kDirName[rotateDir(*f, rot)]);
        if (rot != 2) {
            const std::string_view axis = c.prop(id, "axis");
            if (axis == "x") s = c.with(s, "axis", "z");
            else if (axis == "z") s = c.with(s, "axis", "x");
        }
        if (const std::string_view rs = c.prop(id, "rotation"); !rs.empty()) s = c.with(s, "rotation", std::to_string((std::stoi(std::string(rs)) + 4 * rot) % 16));
        bool sides = true;
        for (Direction d : kHorizontal) sides = sides && c.hasProp(id, kDirName[d]);
        if (sides)
            for (Direction d : kHorizontal) s = c.with(s, kDirName[rotateDir(d, rot)], c.prop(id, kDirName[d]));
        out[size_t(rot)] = s;
    }
    return out;
}

struct Template {
    BlockPos size;
    struct Block {
        BlockPos pos;
        Rotated state;
        bool nbt;
    };
    std::vector<std::vector<Block>> palettes;
};
using TemplateP = std::shared_ptr<const Template>;

bool gunzip(const std::vector<u8>& in, std::vector<u8>& out) {
    if (in.size() < 18) return false;
    const size_t n = size_t(in[in.size() - 4]) | size_t(in[in.size() - 3]) << 8 | size_t(in[in.size() - 2]) << 16 | size_t(in[in.size() - 1]) << 24;
    out.resize(n);
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    size_t actual = 0;
    const libdeflate_result res = libdeflate_gzip_decompress(d, in.data(), in.size(), out.data(), out.size(), &actual);
    libdeflate_free_decompressor(d);
    out.resize(actual);
    return res == LIBDEFLATE_SUCCESS;
}

TemplateP parseTemplate(const Ctx& c, const Nbt& root) {
    auto t = std::make_shared<Template>();
    if (const Nbt* s = root.get("size"); s && s->list.size() == 3)
        t->size = {i32(s->list[0].i), i32(s->list[1].i), i32(s->list[2].i)};
    std::vector<const Nbt*> palettes;
    if (const Nbt* ps = root.get("palettes"))
        for (const Nbt& p : ps->list) palettes.push_back(&p);
    else if (const Nbt* p = root.get("palette"))
        palettes.push_back(p);
    const Nbt* blocks = root.get("blocks");
    for (const Nbt* pal : palettes) {
        std::vector<Rotated> states;
        for (const Nbt& e : pal->list) {
            const Nbt* name = e.get("id");
            if (!name) name = e.get("Name");
            const Nbt* props = e.get("properties");
            if (!props) props = e.get("Properties");
            json js{{"Name", name ? name->s : std::string("minecraft:air")}};
            if (props)
                for (const auto& [k, v] : props->compound) js["Properties"][k] = v.s;
            states.push_back(rotations(c, c.blocks().idFor(blockStateKey(js))));
        }
        // buildInfoList: full blocks, then other blocks, then block entities, each sorted by y, x, z.
        std::vector<Template::Block> full, other, entities;
        if (blocks)
            for (const Nbt& b : blocks->list) {
                const Nbt* pos = b.get("pos");
                const Nbt* st = b.get("state");
                if (!pos || pos->list.size() != 3) continue;
                const size_t si = st ? size_t(st->i) : 0;
                Template::Block info{{i32(pos->list[0].i), i32(pos->list[1].i), i32(pos->list[2].i)},
                                     si < states.size() ? states[si] : rotations(c, c.def("minecraft:air")), b.get("nbt") != nullptr};
                if (info.nbt)
                    entities.push_back(info);
                else if (c.fullCollision(info.state[0]))
                    full.push_back(info);
                else
                    other.push_back(info);
            }
        auto byYXZ = [](const Template::Block& a, const Template::Block& b) {
            if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
            if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
            return a.pos.z < b.pos.z;
        };
        std::stable_sort(full.begin(), full.end(), byYXZ);
        std::stable_sort(other.begin(), other.end(), byYXZ);
        std::stable_sort(entities.begin(), entities.end(), byYXZ);
        std::vector<Template::Block> list = std::move(full);
        list.insert(list.end(), other.begin(), other.end());
        list.insert(list.end(), entities.begin(), entities.end());
        t->palettes.push_back(std::move(list));
    }
    return t;
}

// Loads templates by id ("minecraft:fossil/spine_1"), shared process-wide.
std::vector<TemplateP> loadTemplates(const Ctx& c, const std::vector<std::string>& ids) {
    static std::mutex mutex;
    static std::unordered_map<std::string, TemplateP> cache;
    std::lock_guard lock(mutex);
    std::vector<TemplateP> out(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        std::string id = ids[i].find(':') == std::string::npos ? "minecraft:" + ids[i] : ids[i];
        if (auto it = cache.find(id); it != cache.end()) {
            out[i] = it->second;
            continue;
        }
        const size_t colon = id.find(':');
        const std::string path = "data/" + id.substr(0, colon) + "/structure/" + id.substr(colon + 1) + ".nbt";
        std::vector<u8> raw, bytes;
        Nbt root;
        // From the game's data with the data packs.
        const bool ok = c.env().data().readFile(path, raw) && gunzip(raw, bytes) &&
                        NbtReader(bytes.data(), bytes.size()).readRoot(root);
        if (!ok) {
            LOG("features_misc: structure template %s not found or unreadable", id.c_str());
            continue;
        }
        out[i] = cache[id] = parseTemplate(c, root);
    }
    return out;
}

struct BBox {
    int x0, y0, z0, x1, y1, z1;
    bool inside(const BlockPos& p) const { return p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1 && p.y >= y0 && p.y <= y1; }
};

// StructureProcessor subset used by template features (block_rot, protected_blocks, rule, block_ignore).
struct Processor {
    enum class Kind { Rot, Protected, Rule, Ignore, Nop } kind = Kind::Nop;
    float integrity = 1.0f;
    bool hasSet = false;
    BlockSet set;
    struct Rule {
        std::shared_ptr<const RuleTest> input, location;
        Rotated output;
        int nbt;  // -1 keep, 0 clear, 1 set
    };
    std::vector<Rule> rules;
    std::vector<std::string> ignored;
};

std::vector<Processor> processorList(const Ctx& c, const json& j) {
    const json* list = &j;
    if (j.is_string()) {
        list = c.env().data().worldgen("processor_list", j.get<std::string>());
        if (!list) {
            LOG("features_misc: unknown processor list %s", j.get<std::string>().c_str());
            return {};
        }
    }
    if (list->is_object()) list = &req(*list, "processors");
    std::vector<Processor> out;
    if (!list->is_array()) return out;
    for (const json& pj : *list) {
        Processor p;
        const std::string type = DataPack::normalize(pj.value("processor_type", std::string()));
        if (type == "block_rot") {
            p.kind = Processor::Kind::Rot;
            p.integrity = pj.value("integrity", 1.0f);
            if (pj.contains("rottable_blocks")) p.hasSet = true, p.set = blockSet(pj["rottable_blocks"]);
        } else if (type == "protected_blocks") {
            p.kind = Processor::Kind::Protected;
            p.set = blockSet(req(pj, "value"));
        } else if (type == "rule") {
            p.kind = Processor::Kind::Rule;
            for (const json& rj : req(pj, "rules")) {
                if (auto pp = rj.find("position_predicate");
                    pp != rj.end() && DataPack::normalize(pp->value("predicate_type", std::string())) != "always_true")
                    LOG("features_misc: rule processor position predicates are not supported (treated as always true)");
                int nbt = -1;
                if (auto it = rj.find("block_entity_modifier"); it != rj.end()) {
                    const std::string m = DataPack::normalize(it->value("type", std::string()));
                    nbt = m == "passthrough" ? -1 : m == "clear" ? 0 : 1;
                }
                p.rules.push_back({c.env().ruleTest(req(rj, "input_predicate")), c.env().ruleTest(req(rj, "location_predicate")),
                                   rotations(c, c.state(req(rj, "output_state"))), nbt});
            }
        } else if (type == "block_ignore") {
            p.kind = Processor::Kind::Ignore;
            for (const json& b : req(pj, "blocks")) p.ignored.push_back(std::string(c.name(c.state(b))));
        } else if (type != "nop") {
            LOG("features_misc: structure processor %s is not supported (ignored)", type.c_str());
        }
        out.push_back(std::move(p));
    }
    return out;
}

// BlockState.updateShape for the blocks whose shape update can change them during world generation
// (speleothems, potent sulfur, snowy dirt); every other block only schedules ticks there.
class ShapeUpdater {
public:
    ShapeUpdater(const Ctx& c, GenRegion& level) : c_(c), level_(level) {}

    u16 update(u16 s, const BlockPos& p, Direction toNeighbour, u16 neighbour) {
        if (c_.tag(s, "minecraft:speleothems")) return speleothem(s, p, toNeighbour);
        if (c_.is(s, "minecraft:potent_sulfur")) return potentSulfur(s, p);
        if (toNeighbour == Up && c_.hasProp(s, "snowy")) return c_.with(s, "snowy", c_.tag(neighbour, "minecraft:snow"));
        return s;
    }
    // Block.updateFromNeighbourShapes
    u16 fromNeighbours(u16 s, const BlockPos& p) {
        static constexpr Direction kOrder[6] = {West, East, North, South, Down, Up};
        for (Direction d : kOrder) s = update(s, p, d, level_.block(rel(p, d)));
        return s;
    }
    // StructureTemplate.updateShapeAtEdge
    void edge(const BlockPos& p, Direction d) {
        const BlockPos n = rel(p, d);
        const u16 state = level_.block(p), neighbour = level_.block(n);
        const u16 newState = update(state, p, d, neighbour);
        if (newState != state) level_.setBlock(p, newState);
        const u16 newNeighbour = update(neighbour, n, opposite(d), newState);
        if (newNeighbour != neighbour) level_.setBlock(n, newNeighbour);
    }

private:
    bool speleothemWithDirection(u16 s, Direction tip) const {
        return c_.tag(s, "minecraft:speleothems") && dirFromName(c_.prop(s, "vertical_direction")) == tip;
    }
    u16 speleothem(u16 s, const BlockPos& p, Direction toNeighbour) {
        if (toNeighbour != Up && toNeighbour != Down) return s;
        const Direction tip = dirFromName(c_.prop(s, "vertical_direction")).value_or(Up);
        if (tip == Down && std::find(scheduled_.begin(), scheduled_.end(), p) != scheduled_.end()) return s;
        const Direction base = opposite(tip);
        if (toNeighbour == base) {
            const BlockPos behind = rel(p, base);
            const u16 b = level_.block(behind);
            if (!(c_.sturdy(level_, behind, tip) || (speleothemWithDirection(b, tip) && c_.name(b) == c_.name(s)))) {
                scheduled_.push_back(p);  // breaks on its scheduled tick
                return s;
            }
        }
        const bool mergeOpposingTips = c_.prop(s, "thickness") == "tip_merge";
        const u16 front = level_.block(rel(p, tip));
        const char* thickness;
        if (speleothemWithDirection(front, base) && c_.name(front) == c_.name(s))
            thickness = !mergeOpposingTips && c_.prop(front, "thickness") != "tip_merge" ? "tip" : "tip_merge";
        else if (!speleothemWithDirection(front, tip))
            thickness = "tip";
        else if (const std::string_view ft = c_.prop(front, "thickness"); ft != "tip" && ft != "tip_merge")
            thickness = speleothemWithDirection(level_.block(rel(p, base)), tip) ? "middle" : "base";
        else
            thickness = "frustum";
        return c_.with(s, "thickness", thickness);
    }
    u16 potentSulfur(u16 s, const BlockPos& p) {  // PotentSulfurBlock.validBlockState
        if (!c_.sourceWater(level_.block(above(p)))) return c_.with(s, "potent_sulfur_state", "dry");
        const u16 b = level_.block(below(p));
        const bool sourceIfFluid = c_.fluidEmpty(b) || c_.fluidSource(b);
        if (c_.tag(b, "minecraft:causes_continuous_geyser_eruptions") && sourceIfFluid) return c_.with(s, "potent_sulfur_state", "continuous");
        if (c_.tag(b, "minecraft:causes_periodic_geyser_eruptions") && sourceIfFluid)
            return c_.prop(s, "potent_sulfur_state") == "erupting" ? s : c_.with(s, "potent_sulfur_state", "dormant");
        return c_.with(s, "potent_sulfur_state", "wet");
    }

    const Ctx& c_;
    GenRegion& level_;
    std::vector<BlockPos> scheduled_;
};

// StructureTemplate.placeInWorld (no mirror, pivot 0, waterlogging applied, entities ignored).
bool placeTemplate(const Ctx& c, GenRegion& level, const Template& t, const BlockPos& position, int rot, const BBox* box,
                   const std::vector<Processor>& processors, u16 barrier, RandomSource& r) {
    if (t.palettes.empty()) return false;
    const std::vector<Template::Block>& blocks = t.palettes[size_t(r.nextInt(int(t.palettes.size())))];
    if (blocks.empty() || t.size.x < 1 || t.size.y < 1 || t.size.z < 1) return false;
    // processBlockInfos
    struct Info {
        BlockPos pos;
        Rotated state;
        bool nbt;
    };
    std::vector<Info> processed;
    for (const Template::Block& b : blocks) {
        const BlockPos rp = rotatePos(b.pos, rot);
        const BlockPos wp = rp.offset(position.x, position.y, position.z);
        if (box && !box->inside(wp)) continue;
        Info info{wp, b.state, b.nbt};
        bool keep = true;
        for (const Processor& p : processors) {
            switch (p.kind) {
            case Processor::Kind::Rot:
                if ((!p.hasSet || p.set.test(c, info.state[0])) && !(r.nextFloat() <= p.integrity)) keep = false;
                break;
            case Processor::Kind::Protected:
                if (p.set.test(c, level.block(info.pos))) keep = false;
                break;
            case Processor::Kind::Rule: {
                LegacyRandom rr(positionSeed(info.pos.x, info.pos.y, info.pos.z));
                for (const Processor::Rule& rule : p.rules)
                    if (rule.input->test(info.state[0], info.pos, rr) && rule.location->test(level.block(info.pos), info.pos, rr)) {
                        info.state = rule.output;
                        if (rule.nbt >= 0) info.nbt = rule.nbt == 1;
                        break;
                    }
                break;
            }
            case Processor::Kind::Ignore:
                for (const std::string& n : p.ignored)
                    if (c.is(info.state[0], n)) keep = false;
                break;
            case Processor::Kind::Nop: break;
            }
            if (!keep) break;
        }
        if (keep) processed.push_back(info);
    }
    std::vector<BlockPos> toFill, locked, placed;
    auto waterlog = [&](const BlockPos& p, u16 s, u16 fluidFrom) {  // SimpleWaterloggedBlock.placeLiquid
        if (c.prop(s, "waterlogged") == "false" && c.sourceWater(fluidFrom)) level.setBlock(p, c.with(s, "waterlogged", true));
    };
    for (const Info& info : processed) {
        if (box && !box->inside(info.pos)) continue;
        const u16 previous = level.block(info.pos);
        const u16 state = info.state[size_t(rot)];
        if (info.nbt) level.setBlock(info.pos, barrier);
        if (!level.setBlock(info.pos, state)) continue;
        placed.push_back(info.pos);
        if (info.nbt && isLootContainer(c.name(state))) r.nextLong();  // LootTableSeed
        if (c.fluidSource(state)) {
            locked.push_back(info.pos);
        } else if (c.hasProp(state, "waterlogged")) {
            waterlog(info.pos, state, previous);
            if (!c.fluidSource(previous)) toFill.push_back(info.pos);
        }
    }
    static constexpr Direction kFillDirs[5] = {Up, North, East, South, West};
    for (bool filled = true; filled && !toFill.empty();) {
        filled = false;
        for (size_t i = 0; i < toFill.size();) {
            const BlockPos p = toFill[i];
            u16 toPlace = level.block(p);
            for (int k = 0; k < 5 && !c.fluidSource(toPlace); ++k) {
                const BlockPos n = rel(p, kFillDirs[k]);
                const u16 ns = level.block(n);
                if (c.fluidSource(ns) && std::find(locked.begin(), locked.end(), n) == locked.end()) toPlace = ns;
            }
            const u16 s = level.block(p);
            if (c.fluidSource(toPlace) && c.hasProp(s, "waterlogged")) {
                waterlog(p, s, toPlace);
                filled = true;
                toFill.erase(toFill.begin() + std::ptrdiff_t(i));
            } else {
                ++i;
            }
        }
    }
    if (placed.empty()) return true;
    // Shape updates: the placed volume's boundary faces (DiscreteVoxelShape.forAllFaces order), then every
    // placed block against its neighbours.
    BlockPos lo = placed[0], hi = placed[0];
    for (const BlockPos& p : placed) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const int sx = hi.x - lo.x + 1, sy = hi.y - lo.y + 1, sz = hi.z - lo.z + 1;
    std::vector<u8> shape(size_t(sx) * sy * sz, 0);
    for (const BlockPos& p : placed) shape[(size_t(p.y - lo.y) * sz + (p.z - lo.z)) * sx + (p.x - lo.x)] = 1;
    auto full = [&](int x, int y, int z) { return shape[(size_t(y) * sz + z) * sx + x] != 0; };
    ShapeUpdater updater(c, level);
    auto emit = [&](Direction d, int x, int y, int z) { updater.edge(lo.offset(x, y, z), d); };
    for (int x = 0; x < sx; ++x)  // z faces
        for (int y = 0; y < sy; ++y)
            for (int z = 0, last = 0; z <= sz; ++z) {
                const int f = z != sz && full(x, y, z);
                if (!last && f) emit(North, x, y, z);
                if (last && !f) emit(South, x, y, z - 1);
                last = f;
            }
    for (int z = 0; z < sz; ++z)  // y faces
        for (int x = 0; x < sx; ++x)
            for (int y = 0, last = 0; y <= sy; ++y) {
                const int f = y != sy && full(x, y, z);
                if (!last && f) emit(Down, x, y, z);
                if (last && !f) emit(Up, x, y - 1, z);
                last = f;
            }
    for (int y = 0; y < sy; ++y)  // x faces
        for (int z = 0; z < sz; ++z)
            for (int x = 0, last = 0; x <= sx; ++x) {
                const int f = x != sx && full(x, y, z);
                if (!last && f) emit(West, x, y, z);
                if (last && !f) emit(East, x - 1, y, z);
                last = f;
            }
    for (const BlockPos& p : placed) {
        const u16 s = level.block(p);
        const u16 n = updater.fromNeighbours(s, p);
        if (n != s) level.setBlock(p, n);
    }
    return true;
}

class FossilFeature final : public ConfiguredFeature {
public:
    FossilFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        std::vector<std::string> fossils, overlays;
        for (const json& j : req(cfg, "fossil_structures")) fossils.push_back(j.get<std::string>());
        for (const json& j : req(cfg, "overlay_structures")) overlays.push_back(j.get<std::string>());
        fossils_ = loadTemplates(c_, fossils);
        overlays_ = loadTemplates(c_, overlays);
        fossilProcessors_ = processorList(c_, req(cfg, "fossil_processors"));
        overlayProcessors_ = processorList(c_, req(cfg, "overlay_processors"));
        maxEmptyCorners_ = opt<int>(cfg, "max_empty_corners_allowed", 0);
        barrier_ = c_.def("minecraft:barrier");
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        const int rot = r.nextInt(4);
        const int index = r.nextInt(int(fossils_.size()));
        const TemplateP base = fossils_[size_t(index)];
        const TemplateP overlay = index < int(overlays_.size()) ? overlays_[size_t(index)] : nullptr;
        if (!base || !overlay) return false;
        const int cx = o.x >> 4, cz = o.z >> 4;
        const BBox box{cx * 16 - 16, level.minY(), cz * 16 - 16, cx * 16 + 15 + 16, javaMaxY(level), cz * 16 + 15 + 16};
        const BlockPos size = rot == 1 || rot == 3 ? BlockPos{base->size.z, base->size.y, base->size.x} : base->size;
        const BlockPos lowCorner = o.offset(-size.x / 2, 0, -size.z / 2);
        int lowestSurfaceY = o.y;
        for (int x = 0; x < size.x; ++x)
            for (int z = 0; z < size.z; ++z)
                lowestSurfaceY = std::min(lowestSurfaceY, level.height(Heightmap::OceanFloorWG, lowCorner.x + x, lowCorner.z + z));
        const int targetY = std::max(lowestSurfaceY - 15 - r.nextInt(10), level.minY() + 10);
        const BlockPos target = zeroPosition(atY(lowCorner, targetY), rot, base->size.x, base->size.z);
        if (emptyCorners(level, target, rot, base->size) > maxEmptyCorners_) return false;
        placeTemplate(c_, level, *base, target, rot, &box, fossilProcessors_, barrier_, r);
        placeTemplate(c_, level, *overlay, target, rot, &box, overlayProcessors_, barrier_, r);
        return true;
    }

private:
    static BlockPos zeroPosition(const BlockPos& zero, int rot, int sizeX, int sizeZ) {
        --sizeX, --sizeZ;
        switch (rot) {
        case 3: return zero.offset(0, 0, sizeX);
        case 1: return zero.offset(sizeZ, 0, 0);
        case 2: return zero.offset(sizeX, 0, sizeZ);
        default: return zero;
        }
    }
    int emptyCorners(const GenRegion& level, const BlockPos& pos, int rot, const BlockPos& size) const {
        const BlockPos a = rotatePos({0, 0, 0}, rot), b = rotatePos(size.offset(-1, -1, -1), rot);
        const int x0 = std::min(a.x, b.x) + pos.x, y0 = std::min(a.y, b.y) + pos.y, z0 = std::min(a.z, b.z) + pos.z;
        const int x1 = std::max(a.x, b.x) + pos.x, y1 = std::max(a.y, b.y) + pos.y, z1 = std::max(a.z, b.z) + pos.z;
        int count = 0;
        for (int z : {z1, z0})
            for (int y : {y1, y0})
                for (int x : {x1, x0}) {
                    const u16 s = level.block(BlockPos{x, y, z});
                    if (c_.air(s) || c_.lava(s) || c_.water(s)) ++count;
                }
        return count;
    }

    Ctx c_;
    std::vector<TemplateP> fossils_, overlays_;
    std::vector<Processor> fossilProcessors_, overlayProcessors_;
    int maxEmptyCorners_;
    u16 barrier_;
};

class TemplateFeature final : public ConfiguredFeature {
public:
    TemplateFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        std::vector<std::string> ids;
        for (const json& e : req(cfg, "templates")) {
            const json& data = req(e, "data");
            Entry entry;
            entry.weight = e.value("weight", 1);
            if (auto it = data.find("rotations"); it != data.end())
                for (const json& rj : *it) entry.rotations.push_back(rotationFromName(DataPack::normalize(rj.get<std::string>())));
            else
                entry.rotations = {0, 1, 2, 3};
            totalWeight_ += entry.weight;
            entries_.push_back(std::move(entry));
            ids.push_back(data.value("id", std::string()));
        }
        const std::vector<TemplateP> loaded = loadTemplates(c_, ids);
        for (size_t i = 0; i < entries_.size(); ++i) entries_[i].tmpl = loaded[i];
        if (auto it = cfg.find("processors"); it != cfg.end()) processors_ = processorList(c_, *it);
        barrier_ = c_.def("minecraft:barrier");
    }

    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (totalWeight_ <= 0) return false;
        int selection = r.nextInt(totalWeight_);
        const Entry* entry = &entries_.back();
        for (const Entry& e : entries_)
            if ((selection -= e.weight) < 0) {
                entry = &e;
                break;
            }
        const int rot = entry->rotations[size_t(r.nextInt(int(entry->rotations.size())))];
        if (!entry->tmpl) return false;
        const Template& t = *entry->tmpl;
        // getRotatedOffset: rotation.rotate(axis.getNegative()) * (size along the axis / 2)
        const Direction dx = rotateDir(West, rot), dz = rotateDir(North, rot);
        BlockPos p = o;
        p = p.offset(kDirOffset[dx][0] * (t.size.x / 2), 0, kDirOffset[dx][2] * (t.size.x / 2));
        p = p.offset(kDirOffset[dz][0] * (t.size.z / 2), 0, kDirOffset[dz][2] * (t.size.z / 2));
        return placeTemplate(c_, level, t, p, rot, nullptr, processors_, barrier_, r);
    }

private:
    struct Entry {
        TemplateP tmpl;
        std::vector<int> rotations;
        int weight = 1;
    };
    Ctx c_;
    std::vector<Entry> entries_;
    int totalWeight_ = 0;
    std::vector<Processor> processors_;
    u16 barrier_;
};

// =============================================================================================
// Blobs, columns, pillars, spikes
// =============================================================================================
class SpikeFeature final : public ConfiguredFeature {
public:
    SpikeFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        state_ = env.state(req(cfg, "state"));
        canPlaceOn_ = env.blockPredicate(req(cfg, "can_place_on"));
        canReplace_ = env.blockPredicate(req(cfg, "can_replace"));
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& origin) const override {
        BlockPos o = origin;
        while (level.isEmpty(o) && o.y > level.minY() + 2) o = below(o);
        if (!canPlaceOn_->test(level, o)) return false;
        o = above(o, r.nextInt(4));
        const int height = r.nextInt(4) + 7;
        const int width = height / 4 + r.nextInt(2);
        if (width > 1 && r.nextInt(60) == 0) o = above(o, 10 + r.nextInt(30));
        auto replaceable = [&](const BlockPos& p) { return c_.air(level.block(p)) || canReplace_->test(level, p); };
        for (int yOff = 0; yOff < height; ++yOff) {
            const float scale = (1.0f - float(yOff) / float(height)) * float(width);
            const int newWidth = jceil(scale);
            for (int xo = -newWidth; xo <= newWidth; ++xo) {
                const float dx = float(iabs(xo)) - 0.25f;
                for (int zo = -newWidth; zo <= newWidth; ++zo) {
                    const float dz = float(iabs(zo)) - 0.25f;
                    if (!((xo == 0 && zo == 0) || !(dx * dx + dz * dz > scale * scale))) continue;
                    if (!((xo != -newWidth && xo != newWidth && zo != -newWidth && zo != newWidth) || !(r.nextFloat() > 0.75f))) continue;
                    const BlockPos pos = o.offset(xo, yOff, zo);
                    if (replaceable(pos)) level.setBlock(pos, state_);
                    if (yOff != 0 && newWidth > 1) {
                        const BlockPos neg = o.offset(xo, -yOff, zo);
                        if (replaceable(neg)) level.setBlock(neg, state_);
                    }
                }
            }
        }
        const int pillarWidth = std::clamp(width - 1, 0, 1);
        for (int xo = -pillarWidth; xo <= pillarWidth; ++xo)
            for (int zo = -pillarWidth; zo <= pillarWidth; ++zo) {
                BlockPos cursor = o.offset(xo, -1, zo);
                int runLength = 50;
                if (iabs(xo) == 1 && iabs(zo) == 1) runLength = r.nextInt(5);
                while (cursor.y > 50) {
                    const u16 s = level.block(cursor);
                    if (!c_.air(s) && !canReplace_->test(level, cursor) && s != state_) break;
                    level.setBlock(cursor, state_);
                    cursor = below(cursor);
                    if (--runLength <= 0) {
                        cursor = below(cursor, r.nextInt(5) + 1);
                        runLength = r.nextInt(5);
                    }
                }
            }
        return true;
    }

private:
    Ctx c_;
    u16 state_;
    PredP canPlaceOn_, canReplace_;
};

class ReplaceBlobsFeature final : public ConfiguredFeature {
public:
    ReplaceBlobsFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        target_ = std::string(c_.name(env.state(req(cfg, "target"))));
        state_ = env.state(req(cfg, "state"));
        radius_ = env.intProvider(req(cfg, "radius"));
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        BlockPos cursor{o.x, std::clamp(o.y, level.minY() + 1, javaMaxY(level)), o.z};
        std::optional<BlockPos> center;
        for (; cursor.y > level.minY() + 1; cursor = below(cursor))
            if (c_.is(level.block(cursor), target_)) {
                center = cursor;
                break;
            }
        if (!center) return false;
        const int rx = radius_->sample(r), ry = radius_->sample(r), rz = radius_->sample(r);
        const int maximumRadius = std::max(rx, std::max(ry, rz));
        bool replacedAny = false;
        ManhattanIter it(*center, rx, ry, rz);
        for (BlockPos p; it.next(p);) {
            if (distManhattan(p, *center) > maximumRadius) break;
            if (c_.is(level.block(p), target_)) {
                level.setBlock(p, state_);
                replacedAny = true;
            }
        }
        return replacedAny;
    }

private:
    Ctx c_;
    std::string target_;
    u16 state_;
    IntP radius_;
};

class DeltaFeature final : public ConfiguredFeature {
public:
    DeltaFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        contents_ = env.state(req(cfg, "contents"));
        rim_ = env.state(req(cfg, "rim"));
        size_ = env.intProvider(req(cfg, "size"));
        rimSize_ = env.intProvider(req(cfg, "rim_size"));
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        bool anyPlaced = false;
        const bool spawnRim = r.nextDouble() < 0.9;
        const int rimX = spawnRim ? rimSize_->sample(r) : 0;
        const int rimZ = spawnRim ? rimSize_->sample(r) : 0;
        const bool hasRim = spawnRim && rimX != 0 && rimZ != 0;
        const int radiusX = size_->sample(r);
        const int radiusZ = size_->sample(r);
        const int limit = std::max(radiusX, radiusZ);
        ManhattanIter it(o, radiusX, 0, radiusZ);
        for (BlockPos p; it.next(p);) {
            if (distManhattan(p, o) > limit) break;
            if (!clear(level, p)) continue;
            if (hasRim) {
                anyPlaced = true;
                level.setBlock(p, rim_);
            }
            const BlockPos q = p.offset(rimX, 0, rimZ);
            if (clear(level, q)) {
                anyPlaced = true;
                level.setBlock(q, contents_);
            }
        }
        return anyPlaced;
    }

private:
    bool clear(const GenRegion& level, const BlockPos& p) const {
        const u16 s = level.block(p);
        if (c_.name(s) == c_.name(contents_)) return false;
        static constexpr std::string_view kCannotReplace[] = {"minecraft:bedrock", "minecraft:nether_bricks", "minecraft:nether_brick_fence",
                                                              "minecraft:nether_brick_stairs", "minecraft:nether_wart", "minecraft:chest",
                                                              "minecraft:spawner"};
        for (std::string_view n : kCannotReplace)
            if (c_.is(s, n)) return false;
        for (Direction d : kAllDirs) {
            const bool isAir = c_.air(level.block(rel(p, d)));
            if ((isAir && d != Up) || (!isAir && d == Up)) return false;
        }
        return true;
    }
    Ctx c_;
    u16 contents_, rim_;
    IntP size_, rimSize_;
};

class SteppedColumnClusterFeature final : public ConfiguredFeature {
public:
    SteppedColumnClusterFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        block_ = env.stateProvider(req(cfg, "block"));
        continueThrough_ = env.blockPredicate(req(cfg, "continue_through"));
        canReplace_ = env.blockPredicate(req(cfg, "can_replace"));
        cannotPlaceOn_ = blockSet(req(cfg, "cannot_place_on"));
        clusterReach_ = env.intProvider(req(cfg, "cluster_reach"));
        columnCount_ = env.intProvider(req(cfg, "column_count"));
        columnReach_ = env.intProvider(req(cfg, "column_reach"));
        height_ = env.intProvider(req(cfg, "height"));
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (!canPlaceAt(level, o)) return false;
        const int columnHeight = height_->sample(r);
        const int clusterReach = std::min(columnHeight, clusterReach_->sample(r));
        const int count = columnCount_->sample(r);
        bool placed = false;
        const int width = 2 * clusterReach + 1;
        for (int i = 0; i < count; ++i) {  // BlockPos.randomBetweenClosed (lazy)
            const int x = o.x - clusterReach + r.nextInt(width);
            const int y = o.y + r.nextInt(1);
            const int z = o.z - clusterReach + r.nextInt(width);
            const BlockPos p{x, y, z};
            const int blocksToPlaceY = columnHeight - distManhattan(p, o);
            if (blocksToPlaceY >= 0) placed |= placeColumn(level, r, p, blocksToPlaceY, columnReach_->sample(r));
        }
        return placed;
    }

private:
    bool placeColumn(GenRegion& level, RandomSource& r, const BlockPos& o, int columnHeight, int reach) const {
        bool placedAny = false;
        forBetweenClosed(o.x - reach, o.y, o.z - reach, o.x + reach, o.y, o.z + reach, [&](const BlockPos& p) {
            const int stepLimit = distManhattan(p, o);
            const std::optional<BlockPos> columnPos = canReplace_->test(level, p) ? findSurface(level, p, stepLimit) : findAir(level, p, stepLimit);
            if (!columnPos) return;
            BlockPos cursor = *columnPos;
            for (int blocksY = columnHeight - stepLimit / 2; blocksY >= 0; --blocksY) {
                if (canReplace_->test(level, cursor)) {
                    level.setBlock(cursor, block_->state(r, cursor));
                    cursor = above(cursor);
                    placedAny = true;
                } else {
                    if (!continueThrough_->test(level, cursor)) break;
                    cursor = above(cursor);
                }
            }
        });
        return placedAny;
    }
    std::optional<BlockPos> findSurface(const GenRegion& level, BlockPos cursor, int limit) const {
        while (cursor.y > level.minY() + 1 && limit > 0) {
            --limit;
            if (canPlaceAt(level, cursor)) return cursor;
            cursor = below(cursor);
        }
        return std::nullopt;
    }
    bool canPlaceAt(const GenRegion& level, const BlockPos& cursor) const {
        if (!canReplace_->test(level, cursor)) return false;
        const u16 s = level.block(below(cursor));
        return !c_.air(s) && !cannotPlaceOn_.test(c_, s);
    }
    std::optional<BlockPos> findAir(const GenRegion& level, BlockPos cursor, int limit) const {
        while (cursor.y <= javaMaxY(level) && limit > 0) {
            --limit;
            const u16 s = level.block(cursor);
            if (cannotPlaceOn_.test(c_, s)) return std::nullopt;
            if (c_.air(s)) return cursor;
            cursor = above(cursor);
        }
        return std::nullopt;
    }
    Ctx c_;
    StateP block_;
    PredP continueThrough_, canReplace_;
    BlockSet cannotPlaceOn_;
    IntP clusterReach_, columnCount_, columnReach_, height_;
};

// =============================================================================================
// End and chorus
// =============================================================================================
class ChorusPlantFeature final : public ConfiguredFeature {
public:
    ChorusPlantFeature(const json&, const FeatureEnv& env) : c_(env) {
        plant_ = c_.def("minecraft:chorus_plant");
        flower_ = c_.with(c_.def("minecraft:chorus_flower"), "age", "5");
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        if (!level.isEmpty(o) || !c_.tag(level.block(below(o)), "minecraft:supports_chorus_plant")) return false;
        level.setBlock(o, connections(level, o));
        grow(level, o, r, o, 8, 0);
        return true;
    }

private:
    bool chorus(u16 s) const { return c_.is(s, "minecraft:chorus_plant") || c_.is(s, "minecraft:chorus_flower"); }
    u16 connections(const GenRegion& level, const BlockPos& p) const {  // ChorusPlantBlock.getStateWithConnections
        const u16 down = level.block(below(p));
        u16 s = c_.with(plant_, "down", chorus(down) || c_.tag(down, "minecraft:supports_chorus_plant"));
        s = c_.with(s, "up", chorus(level.block(above(p))));
        s = c_.with(s, "north", chorus(level.block(rel(p, North))));
        s = c_.with(s, "east", chorus(level.block(rel(p, East))));
        s = c_.with(s, "south", chorus(level.block(rel(p, South))));
        return c_.with(s, "west", chorus(level.block(rel(p, West))));
    }
    bool allNeighborsEmpty(const GenRegion& level, const BlockPos& p, std::optional<Direction> ignore) const {
        for (Direction d : kHorizontal)
            if (d != ignore && !level.isEmpty(rel(p, d))) return false;
        return true;
    }
    void grow(GenRegion& level, const BlockPos& current, RandomSource& r, const BlockPos& start, int maxSpread, int depth) const {
        int height = r.nextInt(4) + 1;
        if (depth == 0) ++height;
        for (int i = 0; i < height; ++i) {
            const BlockPos t = above(current, i + 1);
            if (!allNeighborsEmpty(level, t, std::nullopt)) return;
            level.setBlock(t, connections(level, t));
            level.setBlock(below(t), connections(level, below(t)));
        }
        bool placedStem = false;
        if (depth < 4) {
            int stems = r.nextInt(4);
            if (depth == 0) ++stems;
            for (int i = 0; i < stems; ++i) {
                const Direction d = kHorizontal[r.nextInt(4)];
                const BlockPos t = rel(above(current, height), d);
                if (iabs(t.x - start.x) < maxSpread && iabs(t.z - start.z) < maxSpread && level.isEmpty(t) && level.isEmpty(below(t)) &&
                    allNeighborsEmpty(level, t, opposite(d))) {
                    placedStem = true;
                    level.setBlock(t, connections(level, t));
                    const BlockPos back = rel(t, opposite(d));
                    level.setBlock(back, connections(level, back));
                    grow(level, t, r, start, maxSpread, depth + 1);
                }
            }
        }
        if (!placedStem) level.setBlock(above(current, height), flower_);
    }

    Ctx c_;
    u16 plant_, flower_;
};

class EndIslandFeature final : public ConfiguredFeature {
public:
    EndIslandFeature(const json&, const FeatureEnv& env) : endStone_(env.defaultState("minecraft:end_stone")) {}
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        float size = float(r.nextInt(3)) + 4.0f;
        for (int y = 0; size > 0.5f; --y) {
            for (int x = jfloor(-size); x <= jceil(size); ++x)
                for (int z = jfloor(-size); z <= jceil(size); ++z)
                    if (float(x * x + z * z) <= (size + 1.0f) * (size + 1.0f)) level.setBlock(o.offset(x, y, z), endStone_);
            size -= float(r.nextInt(2)) + 0.5f;
        }
        return true;
    }

private:
    u16 endStone_;
};

class EndPlatformFeature final : public ConfiguredFeature {
public:
    EndPlatformFeature(const json&, const FeatureEnv& env) : c_(env) {
        obsidian_ = c_.def("minecraft:obsidian");
        air_ = c_.def("minecraft:air");
    }
    bool place(GenRegion& level, RandomSource&, const BlockPos& o) const override {
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx)
                for (int dy = -1; dy < 3; ++dy) {
                    const BlockPos p = o.offset(dx, dy, dz);
                    const u16 want = dy == -1 ? obsidian_ : air_;
                    if (c_.name(level.block(p)) != c_.name(want)) level.setBlock(p, want);
                }
        return true;
    }

private:
    Ctx c_;
    u16 obsidian_, air_;
};

class EndPodiumFeature final : public ConfiguredFeature {
public:
    EndPodiumFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        active_ = opt<bool>(cfg, "active", false);
        bedrock_ = c_.def("minecraft:bedrock");
        endStone_ = c_.def("minecraft:end_stone");
        air_ = c_.def("minecraft:air");
        portal_ = c_.def("minecraft:end_portal");
        torch_ = c_.def("minecraft:wall_torch");
    }
    bool place(GenRegion& level, RandomSource&, const BlockPos& o) const override {
        auto dropAndSet = [&](const BlockPos& p, u16 s) {  // destroyBlock + set: the same final state
            if (c_.name(level.block(p)) != c_.name(s)) level.setBlock(p, s);
        };
        forBetweenClosed(o.x - 4, o.y - 1, o.z - 4, o.x + 4, o.y + 32, o.z + 4, [&](const BlockPos& p) {
            const double d = distSqr(p, o);
            const bool insideRim = d < 2.5 * 2.5;
            if (!insideRim && !(d < 3.5 * 3.5)) return;
            if (p.y < o.y) {
                if (insideRim)
                    level.setBlock(p, bedrock_);
                else if (active_)
                    dropAndSet(p, endStone_);
                else
                    level.setBlock(p, endStone_);
            } else if (p.y > o.y) {
                if (active_)
                    dropAndSet(p, air_);
                else
                    level.setBlock(p, air_);
            } else if (!insideRim) {
                level.setBlock(p, bedrock_);
            } else if (active_) {
                dropAndSet(p, portal_);
            } else {
                level.setBlock(p, air_);
            }
        });
        for (int y = 0; y < 4; ++y) level.setBlock(above(o, y), bedrock_);
        const BlockPos pillar = above(o, 2);
        for (Direction d : kHorizontal) level.setBlock(rel(pillar, d), c_.with(torch_, "facing", kDirName[d]));
        return true;
    }

private:
    Ctx c_;
    bool active_;
    u16 bedrock_, endStone_, air_, portal_, torch_;
};

class EndGatewayFeature final : public ConfiguredFeature {
public:
    EndGatewayFeature(const json&, const FeatureEnv& env) {
        gateway_ = env.defaultState("minecraft:end_gateway");
        air_ = env.defaultState("minecraft:air");
        bedrock_ = env.defaultState("minecraft:bedrock");
    }
    bool place(GenRegion& level, RandomSource&, const BlockPos& o) const override {
        forBetweenClosed(o.x - 1, o.y - 2, o.z - 1, o.x + 1, o.y + 2, o.z + 1, [&](const BlockPos& p) {
            const bool sameX = p.x == o.x, sameY = p.y == o.y, sameZ = p.z == o.z;
            const bool end = iabs(p.y - o.y) == 2;
            if (sameX && sameY && sameZ)
                level.setBlock(p, gateway_);  // (the exit position lives in the block entity)
            else if (sameY)
                level.setBlock(p, air_);
            else if (end && sameX && sameZ)
                level.setBlock(p, bedrock_);
            else if ((sameX || sameZ) && !end)
                level.setBlock(p, bedrock_);
            else
                level.setBlock(p, air_);
        });
        return true;
    }

private:
    u16 gateway_, air_, bedrock_;
};

class EndSpikeFeature final : public ConfiguredFeature {
public:
    EndSpikeFeature(const json& cfg, const FeatureEnv& env) : c_(env) {
        for (const json& s : opt<json>(cfg, "spikes", json::array()))
            spikes_.push_back({s.value("centerX", 0), s.value("centerZ", 0), s.value("radius", 0), s.value("height", 0), s.value("guarded", false)});
        if (spikes_.empty()) {  // getSpikesForLevel
            LegacyRandom seedRandom(env.noise().seed());
            LegacyRandom random(seedRandom.nextLong() & 65535LL);
            int sizes[10];
            for (int i = 0; i < 10; ++i) sizes[i] = i;
            jshuffle(sizes, 10, random);
            for (int i = 0; i < 10; ++i) {
                const int x = jfloor(42.0 * std::cos(2.0 * (-kPi + (kPi / 10) * i)));
                const int z = jfloor(42.0 * std::sin(2.0 * (-kPi + (kPi / 10) * i)));
                const int size = sizes[i];
                spikes_.push_back({x, z, 2 + size / 3, 76 + size * 3, size == 1 || size == 2});
            }
        }
        obsidian_ = c_.def("minecraft:obsidian");
        air_ = c_.def("minecraft:air");
        bedrock_ = c_.def("minecraft:bedrock");
        fire_ = c_.def("minecraft:fire");
        bars_ = c_.def("minecraft:iron_bars");
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        for (const Spike& s : spikes_)
            if ((o.x >> 4) == (s.x >> 4) && (o.z >> 4) == (s.z >> 4)) placeSpike(level, r, s);
        return true;
    }

private:
    struct Spike {
        int x, z, radius, height;
        bool guarded;
    };
    void placeSpike(GenRegion& level, RandomSource& r, const Spike& s) const {
        const int rad = s.radius;
        forBetweenClosed(s.x - rad, level.minY(), s.z - rad, s.x + rad, s.height + 10, s.z + rad, [&](const BlockPos& p) {
            const double dx = double(p.x) - s.x, dz = double(p.z) - s.z;
            if (dx * dx + 0.0 + dz * dz <= double(rad * rad + 1) && p.y < s.height)
                level.setBlock(p, obsidian_);
            else if (p.y > 65)
                level.setBlock(p, air_);
        });
        if (s.guarded)
            for (int dx = -2; dx <= 2; ++dx)
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dy = 0; dy <= 3; ++dy) {
                        const bool xSide = iabs(dx) == 2, zSide = iabs(dz) == 2, top = dy == 3;
                        if (!xSide && !zSide && !top) continue;
                        const bool xEdge = dx == -2 || dx == 2 || top, zEdge = dz == -2 || dz == 2 || top;
                        u16 b = c_.with(bars_, "north", xEdge && dz != -2);
                        b = c_.with(b, "south", xEdge && dz != 2);
                        b = c_.with(b, "west", zEdge && dx != -2);
                        b = c_.with(b, "east", zEdge && dx != 2);
                        level.setBlock(BlockPos{s.x + dx, s.height + dy, s.z + dz}, b);
                    }
        // The end crystal entity is not spawned; its yaw still uses the random, and its block position
        // gets bedrock below and fire.
        r.nextFloat();
        const BlockPos crystal{s.x, s.height + 1, s.z};
        level.setBlock(below(crystal), bedrock_);
        level.setBlock(crystal, fire_);
    }
    Ctx c_;
    std::vector<Spike> spikes_;
    u16 obsidian_, air_, bedrock_, fire_, bars_;
};

class BonusChestFeature final : public ConfiguredFeature {
public:
    BonusChestFeature(const json&, const FeatureEnv& env) : c_(env) {
        chest_ = c_.def("minecraft:chest");
        torch_ = c_.def("minecraft:torch");
    }
    bool place(GenRegion& level, RandomSource& r, const BlockPos& o) const override {
        const int cx = o.x >> 4, cz = o.z >> 4;
        int xs[16], zs[16];
        for (int i = 0; i < 16; ++i) xs[i] = cx * 16 + i, zs[i] = cz * 16 + i;
        jshuffle(xs, 16, r);
        jshuffle(zs, 16, r);
        for (int x : xs)
            for (int z : zs) {
                const BlockPos p{x, level.height(Heightmap::MotionBlockingNoLeaves, x, z), z};
                const u16 s = level.block(p);
                if (!level.isEmpty(p) && blocksCollision(s)) continue;
                level.setBlock(p, chest_);
                if (c_.is(level.block(p), "minecraft:chest")) r.nextLong();  // loot table seed
                for (Direction d : kHorizontal) {
                    const BlockPos t = rel(p, d);
                    if (c_.canSurvive(level, torch_, t)) level.setBlock(t, torch_);
                }
                return true;
            }
        return false;
    }

private:
    bool blocksCollision(u16 s) const { return c_.blocks().infos()[s].collisionCount != 0; }  // collision shape not empty
    Ctx c_;
    u16 chest_, torch_;
};

template <class T>
FeatureFactory factory() {
    return [](const json& config, const FeatureEnv& env) -> std::unique_ptr<ConfiguredFeature> {
        return std::make_unique<T>(cfgOf(config), env);
    };
}

}  // namespace

void registerMiscFeatures(FeatureTypes& types) {
    types.add("minecraft:ore", factory<OreFeature>());
    types.add("minecraft:scattered_ore", factory<ScatteredOreFeature>());
    types.add("minecraft:geode", factory<GeodeFeature>());
    types.add("minecraft:speleothem", factory<SpeleothemFeature>());
    types.add("minecraft:speleothem_cluster", factory<SpeleothemClusterFeature>());
    types.add("minecraft:large_dripstone", factory<LargeDripstoneFeature>());
    types.add("minecraft:vegetation_patch", factory<VegetationPatchFeature>());
    types.add("minecraft:waterlogged_vegetation_patch", factory<WaterloggedVegetationPatchFeature>());
    types.add("minecraft:root_system", factory<RootSystemFeature>());
    types.add("minecraft:sculk_patch", factory<SculkPatchFeature>());
    types.add("minecraft:lake", factory<LakeFeature>());
    types.add("minecraft:spring_feature", factory<SpringFeature>());
    types.add("minecraft:iceberg", factory<IcebergFeature>());
    types.add("minecraft:blue_ice", factory<BlueIceFeature>());
    types.add("minecraft:coral_tree", factory<CoralTreeFeature>());
    types.add("minecraft:coral_claw", factory<CoralClawFeature>());
    types.add("minecraft:monster_room", factory<MonsterRoomFeature>());
    types.add("minecraft:fossil", factory<FossilFeature>());
    types.add("minecraft:template", factory<TemplateFeature>());
    types.add("minecraft:spike", factory<SpikeFeature>());
    types.add("minecraft:netherrack_replace_blobs", factory<ReplaceBlobsFeature>());
    types.add("minecraft:delta_feature", factory<DeltaFeature>());
    types.add("minecraft:stepped_column_cluster", factory<SteppedColumnClusterFeature>());
    types.add("minecraft:chorus_plant", factory<ChorusPlantFeature>());
    types.add("minecraft:end_island", factory<EndIslandFeature>());
    types.add("minecraft:end_platform", factory<EndPlatformFeature>());
    types.add("minecraft:end_podium", factory<EndPodiumFeature>());
    types.add("minecraft:end_gateway", factory<EndGatewayFeature>());
    types.add("minecraft:end_spike", factory<EndSpikeFeature>());
    types.add("minecraft:bonus_chest", factory<BonusChestFeature>());
}

}  // namespace wg
