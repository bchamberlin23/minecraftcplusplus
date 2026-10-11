#include "worldgen/materials.h"

#include "assets/assets.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace wg {

namespace {

using nlohmann::json;
using NoisePtr = std::shared_ptr<const Noise>;

constexpr u16 kNone = 0xFFFF;          // a rule that produced nothing (the game's null state)
constexpr int kWayBelowMinY = -32512;  // DimensionType.WAY_BELOW_MIN_Y
constexpr int kBands = 192;

// (int) of a double in Java: truncation, saturating, NaN -> 0.
i32 javaInt(double v) {
    if (v != v) return 0;
    if (v >= 2147483647.0) return INT_MAX;
    if (v <= -2147483648.0) return INT_MIN;
    return i32(v);
}

// Math.round(float).
i32 javaRound(float a) {
    u32 raw;
    std::memcpy(&raw, &a, 4);
    const i32 bits = i32(raw);
    const i32 shift = (24 - 2 + 127) - ((bits & 0x7F800000) >> 23);
    if ((shift & -32) == 0) {
        i32 r = (bits & 0x007FFFFF) | 0x00800000;
        if (bits < 0) r = -r;
        return ((r >> shift) + 1) >> 1;
    }
    return javaInt(double(a));
}

// Math.min(double, double), NaN and signed zero included.
double javaMin(double a, double b) {
    if (a != a) return a;
    if (a == 0.0 && b == 0.0 && std::signbit(b)) return b;
    return a <= b ? a : b;
}

float sampleNoise(const NoisePtr& n, double x, double y, double z) { return n ? n->get(x, y, z) : 0.0f; }

struct SvHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
template <class V>
using StrMap = std::unordered_map<std::string, V, SvHash, std::equal_to<>>;
using StrSet = std::unordered_set<std::string, SvHash, std::equal_to<>>;

std::string withNamespace(std::string_view id) {
    return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}
std::string_view stripNamespace(std::string_view id) {
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    return id;
}
std::string_view baseName(std::string_view stateKey) { return stateKey.substr(0, stateKey.find('[')); }

// Default property values for blocks the data names without properties (the game then uses the
// block's default state). Only blocks with properties that world generation places are listed.
struct DefaultProperty {
    const char *block, *key, *value;
};
constexpr DefaultProperty kDefaultProperties[] = {
    {"minecraft:grass_block", "snowy", "false"}, {"minecraft:mycelium", "snowy", "false"},
    {"minecraft:podzol", "snowy", "false"},      {"minecraft:water", "level", "0"},
    {"minecraft:lava", "level", "0"},            {"minecraft:deepslate", "axis", "y"},
    {"minecraft:basalt", "axis", "y"},           {"minecraft:polished_basalt", "axis", "y"},
};

std::string stateKey(const json& j) {
    if (!j.is_string() && !j.is_object()) return "minecraft:air";
    json s = j.is_string() ? json{{"Name", j}} : j;
    const std::string name = withNamespace(s.value("Name", std::string("minecraft:air")));
    s["Name"] = name;
    for (const DefaultProperty& d : kDefaultProperties) {
        if (name != d.block) continue;
        json& props = s["Properties"];
        if (!props.is_object()) props = json::object();
        if (!props.contains(d.key)) props[d.key] = d.value;
    }
    return blockStateKey(s);
}

// SimplexNoise, 2D only (Biome's temperature noises). Built with discardNoiseOffset = true.
class Simplex2D {
public:
    void init(RandomSource& random) {
        offsetX_ = random.nextDouble() * 0.0;
        offsetY_ = random.nextDouble() * 0.0;
        random.nextDouble();  // offsetZ, unused in 2D
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
        const double n0 = corner(gi0, x0, y0), n1 = corner(gi1, x1, y1), n2 = corner(gi2, x2, y2);
        return float(70.0 * (n0 + n1 + n2));
    }

private:
    int permute(int x) const { return perms_[x & 0xFF]; }
    // getCornerNoise3D(index, x, y, 0.0, 0.5)
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

// A positional random factory. The Xoroshiro / legacy factories are replicated so "at" needs no heap
// allocation; anything else goes through the factory object.
struct PosRandom {
    enum class Kind : u8 { Xoroshiro, Legacy, Generic };
    Kind kind = Kind::Generic;
    i64 lo = 0, hi = 0;
    const PositionalRandomFactory* generic = nullptr;
    std::unique_ptr<PositionalRandomFactory> owned;

    template <class F>
    auto with(int x, int y, int z, F&& f) const {
        switch (kind) {
        case Kind::Xoroshiro: {
            XoroshiroRandom r(positionSeed(x, y, z) ^ lo, hi);
            return f(r);
        }
        case Kind::Legacy: {
            LegacyRandom r(positionSeed(x, y, z) ^ lo);
            return f(r);
        }
        default: {
            const auto r = generic->at(x, y, z);
            return f(*r);
        }
        }
    }

    // Falls back to the factory itself unless the replica reproduces it.
    void verify(const PositionalRandomFactory& factory) {
        generic = &factory;
        if (kind == Kind::Generic) return;
        const int probes[2][3] = {{0, 0, 0}, {-123457, 71, 98765}};
        for (const auto& p : probes) {
            const i64 mine = with(p[0], p[1], p[2], [](auto& r) { return r.nextLong(); });
            if (mine != factory.at(p[0], p[1], p[2])->nextLong()) {
                LOG("materials: positional random replica mismatch; using the slow path");
                kind = Kind::Generic;
                return;
            }
        }
    }
};

struct Anchor {
    enum Kind : u8 { Absolute, AboveBottom, BelowTop, SeaLevel } kind = Absolute;
    int value = 0;
};

enum class CondType : u8 {
    Never, Biome, Noise, VerticalGradient, YAbove, Water, Temperature, Steep, Not, Hole, AboveSurface, StoneDepth
};

struct Cond {
    CondType type = CondType::Never;
    bool add = false;      // add_stone_depth / add_surface_depth
    bool ceiling = false;  // stone_depth surface_type
    int ref = 0;           // biome set / noise slot / random slot / inverted condition
    int offset = 0;
    int mul = 0;  // surface_depth_multiplier / secondary_depth_range
    Anchor lo, hi;
    double min = 0, max = 0;
};

enum class RuleType : u8 { Never, Block, Bandlands, Sequence, Condition, OreVein };

struct Rule {
    RuleType type = RuleType::Never;
    u16 block = kNone;
    int cond = 0, then = 0;    // Condition
    int first = 0, count = 0;  // Sequence: Impl::children[first, first + count)
    int ore = 0;               // OreVein
};

struct OreVein {
    u16 ore, raw, filler;
    float rawChance;
    int density, richness, gap;  // density slots
    int random;                  // "minecraft:ore" random slot
};

struct NoiseSlot {
    NoisePtr noise;
    bool is3d;
};

struct DensitySlot {
    DensityRef fn;
    bool prefill;  // sampled over the whole chunk volume up front (MaterialRuleContext.getDensitiesInChunk)
};

struct BiomeEntry {
    u16 id = 0;
    const u64* mask = nullptr;  // membership in the rules' biome sets
    bool erodedBadlands = false, frozenOcean = false, frozenModifier = false;
    float temperature = 0.0f;
};

}  // namespace

struct MaterialSystem::Impl {
    Impl(const NoiseState& n, BlockRegistry& b, const BiomeTable& t) : noise(n), blocks(b), biomes(t) {}

    const NoiseState& noise;
    BlockRegistry& blocks;
    const BiomeTable& biomes;

    int seaLevel = 63, genMinY = -64, genHeight = 384;
    u16 defaultBlock = 0, water = 0, waterSource = 0, packedIce = 0, snowBlock = 0;
    std::string defaultBlockName;
    u16 bands[kBands] = {};
    NoisePtr surface, surfaceSecondary, clayBandsOffset, badlandsPillar, badlandsPillarRoof, badlandsSurface,
        icebergPillar, icebergPillarRoof, icebergSurface;
    PosRandom noiseRandom;
    DensityRef preliminarySurface;
    Simplex2D temperatureNoise, frozenNoise[3], biomeInfoNoise;

    std::vector<Cond> conds;
    std::vector<Rule> rules;
    std::vector<int> children;
    std::vector<OreVein> ores;
    std::vector<NoiseSlot> noises;
    std::vector<PosRandom> randoms;
    std::vector<DensitySlot> densities;
    int root = 0;
    StrMap<std::vector<u64>> biomeMasks;
    StrSet motionBlocking;  // #blocks_motion_in_heightmap (OCEAN_FLOOR_WG)

    u16 stateId(const json& j) const { return blocks.idFor(stateKey(j)); }

    bool hasFluid(u16 id) const {
        const BlockInfo& b = blocks.infos()[id];
        return b.fluid || b.waterlogged;
    }
    bool isStone(u16 id) const { return id != GenChunk::kAir && !hasFluid(id); }
    bool isWater(u16 id) const {
        return id == water || id == waterSource ||
               (blocks.infos()[id].fluid && baseName(blocks.name(id)) == "minecraft:water");
    }
    bool isDefaultBlock(u16 id) const {
        return id == defaultBlock || (id != GenChunk::kAir && baseName(blocks.name(id)) == defaultBlockName);
    }
    bool blocksMotion(u16 id) const {
        return id != GenChunk::kAir && motionBlocking.find(baseName(blocks.name(id))) != motionBlocking.end();
    }

    // MaterialSystem.getSurfaceDepth
    int surfaceDepth(int x, int z) const {
        const double value = sampleNoise(surface, x, 0.0, z);
        const double r = noiseRandom.with(x, 0, z, [](auto& random) { return random.nextDouble(); });
        return javaInt(value * 2.75 + 3.0 + r * 0.25);
    }

    BiomeEntry describe(u16 id) const {
        BiomeEntry b;
        b.id = id;
        const std::string name = withNamespace(biomes.name(id));
        if (auto it = biomeMasks.find(name); it != biomeMasks.end()) b.mask = it->second.data();
        b.erodedBadlands = name == "minecraft:eroded_badlands";
        b.frozenOcean = name == "minecraft:frozen_ocean" || name == "minecraft:deep_frozen_ocean";
        if (const json* j = noise.data().worldgen("biome", name)) {
            b.temperature = float(j->value("temperature", 0.0));
            b.frozenModifier = j->value("temperature_modifier", std::string("none")) == "frozen";
        }
        return b;
    }

    // Biome.getTemperature (getHeightAdjustedTemperature with the climate's temperature modifier).
    float temperature(const BiomeEntry& b, int x, int y, int z) const {
        float t = b.temperature;
        if (b.frozenModifier) {
            float large = 0.0f;
            large += 0.14285715f * frozenNoise[0].get(x * 0.05 * 1.0, z * 0.05 * 1.0);
            large += 0.2857143f * frozenNoise[1].get(x * 0.05 * 0.5, z * 0.05 * 0.5);
            large += 0.5714286f * frozenNoise[2].get(x * 0.05 * 0.25, z * 0.05 * 0.25);
            const double icePatches = double(large * 7.0f) + double(biomeInfoNoise.get(x * 0.2, z * 0.2));
            if (icePatches < 0.3 && double(biomeInfoNoise.get(x * 0.09, z * 0.09)) < 0.8) t = 0.2f;
        }
        const int snowLevel = seaLevel + 17;
        if (y > snowLevel) {
            const float v = temperatureNoise.get(double(float(x) / 8.0f), double(float(z) / 8.0f)) * 8.0f;
            return t - (v + float(y) - float(snowLevel)) * 0.05f / 40.0f;
        }
        return t;
    }

    void generateBands(RandomSource& random);
};

void MaterialSystem::Impl::generateBands(RandomSource& random) {
    const u16 terracotta = blocks.idFor("minecraft:terracotta");
    const u16 orange = blocks.idFor("minecraft:orange_terracotta");
    const u16 white = blocks.idFor("minecraft:white_terracotta");
    const u16 lightGray = blocks.idFor("minecraft:light_gray_terracotta");
    std::fill(std::begin(bands), std::end(bands), terracotta);
    for (int i = 0; i < kBands; ++i) {
        i += random.nextInt(5) + 1;
        if (i < kBands) bands[i] = orange;
    }
    auto makeBands = [&](int baseWidth, u16 state) {
        const int bandCount = random.nextIntBetweenInclusive(6, 15);
        for (int i = 0; i < bandCount; ++i) {
            const int width = baseWidth + random.nextInt(3);
            const int start = random.nextInt(kBands);
            for (int p = 0; start + p < kBands && p < width; ++p) bands[start + p] = state;
        }
    };
    makeBands(1, blocks.idFor("minecraft:yellow_terracotta"));
    makeBands(2, blocks.idFor("minecraft:brown_terracotta"));
    makeBands(1, blocks.idFor("minecraft:red_terracotta"));
    const int whiteBandCount = random.nextIntBetweenInclusive(9, 15);
    int count = 0;
    for (int start = 0; count < whiteBandCount && start < kBands; start += random.nextInt(16) + 4) {
        bands[start] = white;
        if (start - 1 > 0 && random.nextBoolean()) bands[start - 1] = lightGray;
        if (start + 1 < kBands && random.nextBoolean()) bands[start + 1] = lightGray;
        ++count;
    }
}

namespace {

using Impl = MaterialSystem::Impl;

// RandomState.random (the world's positional factory), replicated from the seed.
PosRandom rootRandom(const NoiseState& noise) {
    PosRandom p;
    if (dynamic_cast<const XoroshiroPositional*>(&noise.random())) {
        XoroshiroRandom r(noise.seed());
        p.kind = PosRandom::Kind::Xoroshiro;
        p.lo = r.nextLong();
        p.hi = r.nextLong();
    } else if (dynamic_cast<const LegacyPositional*>(&noise.random())) {
        LegacyRandom r(noise.seed());
        p.kind = PosRandom::Kind::Legacy;
        p.lo = r.nextLong();
    }
    p.verify(noise.random());
    return p;
}

// RandomState.getOrCreateRandomFactory(name) = random.fromHashOf(name).forkPositional()
PosRandom namedRandom(const NoiseState& noise, const PosRandom& root, const std::string& name) {
    PosRandom p;
    if (root.kind == PosRandom::Kind::Xoroshiro) {
        const Seed128 s = seedFromHashOf(name);
        XoroshiroRandom r(s.lo ^ root.lo, s.hi ^ root.hi);
        p.kind = PosRandom::Kind::Xoroshiro;
        p.lo = r.nextLong();
        p.hi = r.nextLong();
    } else if (root.kind == PosRandom::Kind::Legacy) {
        LegacyRandom r(i64(javaStringHash(name)) ^ root.lo);
        p.kind = PosRandom::Kind::Legacy;
        p.lo = r.nextLong();
    }
    p.owned = noise.randomFactory(name);
    p.verify(*p.owned);
    return p;
}

// Turns material_rule / material_condition JSON into flat rule and condition tables.
class Compiler {
public:
    explicit Compiler(Impl& m) : m_(m), data_(m.noise.data()) {}

    int rule(const json& j) {
        if (j.is_string()) {
            const std::string id = withNamespace(j.get<std::string>());
            if (auto it = ruleRefs_.find(id); it != ruleRefs_.end()) return it->second;
            const json* def = data_.worldgen("material_rule", id);
            const int r = def ? rule(*def) : never("material_rule", id);
            ruleRefs_.emplace(id, r);
            return r;
        }
        const std::string type(stripNamespace(j.value("type", std::string())));
        Rule r;
        if (type == "block") {
            r.type = RuleType::Block;
            r.block = m_.stateId(j.at("result_state"));
        } else if (type == "bandlands") {
            r.type = RuleType::Bandlands;
        } else if (type == "sequence") {
            std::vector<int> kids;
            for (const json& c : j.at("sequence")) kids.push_back(rule(c));
            if (kids.size() == 1) return kids[0];
            r.type = RuleType::Sequence;
            r.first = int(m_.children.size());
            r.count = int(kids.size());
            m_.children.insert(m_.children.end(), kids.begin(), kids.end());
        } else if (type == "condition") {
            r.type = RuleType::Condition;
            r.cond = cond(j.at("if_true"));
            r.then = rule(j.at("then_run"));
        } else if (type == "ore_vein") {
            OreVein o;
            o.ore = m_.stateId(j.at("ore_block"));
            o.raw = m_.stateId(j.at("raw_ore_block"));
            o.filler = m_.stateId(j.at("filler_block"));
            o.rawChance = float(j.at("raw_ore_chance").get<double>());
            o.density = density(j.at("density"), true);
            o.richness = density(j.at("richness"), true);
            o.gap = density(j.at("filler_gap"), false);
            o.random = random("minecraft:ore");
            if (o.density < 0 || o.richness < 0 || o.gap < 0) return never("ore_vein density", j.dump());
            r.type = RuleType::OreVein;
            r.ore = int(m_.ores.size());
            m_.ores.push_back(o);
        } else {
            return never("material rule type", type);
        }
        m_.rules.push_back(r);
        return int(m_.rules.size()) - 1;
    }

    int cond(const json& j) {
        if (j.is_string()) {
            const std::string id = withNamespace(j.get<std::string>());
            if (auto it = condRefs_.find(id); it != condRefs_.end()) return it->second;
            const json* def = data_.worldgen("material_condition", id);
            const int c = def ? cond(*def) : neverCond("material_condition", id);
            condRefs_.emplace(id, c);
            return c;
        }
        const std::string type(stripNamespace(j.value("type", std::string())));
        Cond c;
        if (type == "biome") {
            c.type = CondType::Biome;
            c.ref = biomeSet(j.at("biome_is"));
        } else if (type == "noise_threshold") {
            c.type = CondType::Noise;
            c.ref = noiseSlot(j.at("noise").get<std::string>(), j.value("is_3d", false));
            c.min = j.at("min_threshold").get<double>();
            c.max = j.at("max_threshold").get<double>();
            if (c.ref < 0) return neverCond("noise", j.at("noise").get<std::string>());
        } else if (type == "vertical_gradient") {
            c.type = CondType::VerticalGradient;
            c.ref = random(withNamespace(j.at("random_name").get<std::string>()));
            c.lo = anchor(j.at("true_at_and_below"));
            c.hi = anchor(j.at("false_at_and_above"));
        } else if (type == "y_above") {
            c.type = CondType::YAbove;
            c.lo = anchor(j.at("anchor"));
            c.mul = j.at("surface_depth_multiplier").get<int>();
            c.add = j.at("add_stone_depth").get<bool>();
        } else if (type == "water") {
            c.type = CondType::Water;
            c.offset = j.at("offset").get<int>();
            c.mul = j.at("surface_depth_multiplier").get<int>();
            c.add = j.at("add_stone_depth").get<bool>();
        } else if (type == "stone_depth") {
            c.type = CondType::StoneDepth;
            c.offset = j.at("offset").get<int>();
            c.add = j.at("add_surface_depth").get<bool>();
            c.mul = j.at("secondary_depth_range").get<int>();
            c.ceiling = j.at("surface_type").get<std::string>() == "ceiling";
        } else if (type == "not") {
            c.type = CondType::Not;
            c.ref = cond(j.at("invert"));
        } else if (type == "temperature") {
            c.type = CondType::Temperature;
        } else if (type == "steep") {
            c.type = CondType::Steep;
        } else if (type == "hole") {
            c.type = CondType::Hole;
        } else if (type == "above_preliminary_surface") {
            c.type = CondType::AboveSurface;
        } else {
            return neverCond("material condition type", type);
        }
        m_.conds.push_back(c);
        return int(m_.conds.size()) - 1;
    }

    // Biome set membership masks, keyed by biome name.
    void finish() {
        const size_t words = (setNames_.size() + 63) / 64;
        for (size_t k = 0; k < setNames_.size(); ++k)
            for (const std::string& name : setNames_[k]) {
                auto& mask = m_.biomeMasks[name];
                mask.resize(words);
                mask[k >> 6] |= u64(1) << (k & 63);
            }
    }

private:
    int never(std::string_view what, const std::string& name) {
        warn(what, name);
        m_.rules.push_back(Rule{});
        return int(m_.rules.size()) - 1;
    }
    int neverCond(std::string_view what, const std::string& name) {
        warn(what, name);
        m_.conds.push_back(Cond{});
        return int(m_.conds.size()) - 1;
    }
    void warn(std::string_view what, const std::string& name) {
        if (logged_.insert(std::string(what) + name).second)
            LOG("materials: unknown or invalid %.*s '%s' (never matches)", int(what.size()), what.data(), name.c_str());
    }

    Anchor anchor(const json& j) {
        static constexpr std::pair<const char*, Anchor::Kind> kKinds[] = {{"absolute", Anchor::Absolute},
                                                                          {"above_bottom", Anchor::AboveBottom},
                                                                          {"below_top", Anchor::BelowTop},
                                                                          {"relative_to_sea_level", Anchor::SeaLevel}};
        for (const auto& [key, kind] : kKinds)
            if (j.contains(key)) return Anchor{kind, j[key].get<int>()};
        warn("vertical anchor", j.dump());
        return Anchor{};
    }

    int noiseSlot(const std::string& id, bool is3d) {
        const std::string key = withNamespace(id) + (is3d ? "#3d" : "#2d");
        if (auto it = noiseSlots_.find(key); it != noiseSlots_.end()) return it->second;
        NoisePtr n = m_.noise.noise(withNamespace(id));
        const int slot = n ? int(m_.noises.size()) : -1;
        if (n) m_.noises.push_back({std::move(n), is3d});
        noiseSlots_.emplace(key, slot);
        return slot;
    }

    int random(const std::string& name) {
        if (auto it = randomSlots_.find(name); it != randomSlots_.end()) return it->second;
        m_.randoms.push_back(namedRandom(m_.noise, m_.noiseRandom, name));
        const int slot = int(m_.randoms.size()) - 1;
        randomSlots_.emplace(name, slot);
        return slot;
    }

    int density(const json& j, bool prefill) {
        const std::string key = (prefill ? "1" : "0") + j.dump();
        if (auto it = densitySlots_.find(key); it != densitySlots_.end()) return it->second;
        DensityRef f = m_.noise.parse(j);
        const int slot = f ? int(m_.densities.size()) : -1;
        if (f) m_.densities.push_back({std::move(f), prefill});
        densitySlots_.emplace(key, slot);
        return slot;
    }

    int biomeSet(const json& j) {
        std::vector<std::string> names;
        auto add = [&](const std::string& s) {
            if (s.starts_with('#')) {
                for (const std::string& n : data_.tag("worldgen/biome", s.substr(1))) names.push_back(withNamespace(n));
            } else {
                names.push_back(withNamespace(s));
            }
        };
        if (j.is_string()) {
            add(j.get<std::string>());
        } else {
            for (const json& e : j) add(e.get<std::string>());
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        std::string key;
        for (const std::string& n : names) key += n + ",";
        if (auto it = biomeSets_.find(key); it != biomeSets_.end()) return it->second;
        const int set = int(setNames_.size());
        setNames_.push_back(std::move(names));
        biomeSets_.emplace(key, set);
        return set;
    }

    Impl& m_;
    const DataPack& data_;
    std::unordered_map<std::string, int> ruleRefs_, condRefs_, noiseSlots_, randomSlots_, densitySlots_, biomeSets_;
    std::vector<std::vector<std::string>> setNames_;
    std::set<std::string> logged_;
};

int gradientX(const GenChunk& c, int x, int z) {
    return c.worldSurfaceWG[z * 16 + std::min(x + 1, 15)] - c.worldSurfaceWG[z * 16 + std::max(x - 1, 0)];
}
int gradientZ(const GenChunk& c, int x, int z) {
    return c.worldSurfaceWG[std::min(z + 1, 15) * 16 + x] - c.worldSurfaceWG[std::max(z - 1, 0) * 16 + x];
}

// Noise biomes around one chunk, fetched once per quart. A block whose eight BiomeManager candidate
// quarts all hold the same biome gets it without the zoom computation (the zoom always picks one of them).
class BiomeGrid {
public:
    static constexpr u16 kUnknown = 0xFFFF, kMixed = 0xFFFE;

    BiomeGrid(const GenChunk& chunk, const BiomeZoom& zoom, const NoiseBiomeFn& source) : zoom_(zoom), source_(source) {
        qx0_ = (chunk.minBlockX() >> 2) - 1;
        qz0_ = (chunk.minBlockZ() >> 2) - 1;
        qy0_ = (chunk.minY - 2) >> 2;
        ny_ = ((chunk.minY + chunk.height - 2) >> 2) + 2 - qy0_;
        quarts_.assign(size_t(36) * ny_, kUnknown);
        cells_.assign(size_t(25) * (ny_ - 1), kUnknown);
        fn = [this](int qx, int qy, int qz) { return noise(qx, qy, qz); };
    }
    BiomeGrid(const BiomeGrid&) = delete;
    BiomeGrid& operator=(const BiomeGrid&) = delete;

    u16 noise(int qx, int qy, int qz) {
        const int ix = qx - qx0_, iy = qy - qy0_, iz = qz - qz0_;
        if (unsigned(ix) >= 6u || unsigned(iz) >= 6u || unsigned(iy) >= unsigned(ny_)) return source_(qx, qy, qz);
        u16& q = quarts_[(size_t(iy) * 6 + iz) * 6 + ix];
        if (q == kUnknown) q = source_(qx, qy, qz);
        return q;
    }

    u16 biome(int x, int y, int z) {
        const int px = (x - 2) >> 2, py = (y - 2) >> 2, pz = (z - 2) >> 2;
        const int ix = px - qx0_, iy = py - qy0_, iz = pz - qz0_;
        if (unsigned(ix) < 5u && unsigned(iz) < 5u && unsigned(iy) < unsigned(ny_ - 1)) {
            u16& c = cells_[(size_t(iy) * 5 + iz) * 5 + ix];
            if (c == kUnknown) {
                const u16 b = noise(px, py, pz);
                c = b;
                for (int i = 1; i < 8 && c != kMixed; ++i)
                    if (noise(px + ((i >> 2) & 1), py + ((i >> 1) & 1), pz + (i & 1)) != b) c = kMixed;
            }
            if (c != kMixed) return c;
        }
        return zoom_.biome(x, y, z, fn);
    }

    NoiseBiomeFn fn;  // noise() as a NoiseBiomeFn, for the zoom

private:
    const BiomeZoom& zoom_;
    const NoiseBiomeFn& source_;
    int qx0_, qy0_, qz0_, ny_;
    std::vector<u16> quarts_;  // 6 x ny x 6 quarts
    std::vector<u16> cells_;   // 5 x (ny - 1) x 5 zoom cells
};

// Per-thread scratch buffers, reused between chunks.
struct Scratch {
    struct NoiseValue {
        u32 stamp = 0;
        double value = 0;
    };
    std::vector<NoiseValue> noise;
    std::vector<std::vector<float>> density;
    std::vector<u8> densityFilled;
};
thread_local Scratch tScratch;

// MaterialRuleContext plus the compiled rule evaluation for one buildSurface / topMaterial call.
class Eval {
public:
    Eval(const Impl& m, GenChunk& chunk, DensityContext& dctx, const BiomeZoom& zoom, const NoiseBiomeFn& noiseBiome,
         BiomeGrid* grid, bool prefill)
        : m_(m), chunk_(chunk), dctx_(dctx), zoom_(zoom), noiseBiome_(noiseBiome), grid_(grid), prefill_(prefill),
          s_(tScratch) {
        minGenY_ = std::max(chunk.minY, m.genMinY);
        genDepth_ = std::min(chunk.height, m.genHeight);
        s_.noise.assign(m.noises.size(), Scratch::NoiseValue{});
        if (s_.density.size() < m.densities.size()) s_.density.resize(m.densities.size());
        s_.densityFilled.assign(m.densities.size(), 0);
        if (prefill) {
            // The game's narrowed volume: the chunk up to the top of its highest non-empty section.
            int top = chunk.minY;
            for (int h : chunk.worldSurfaceWG) top = std::max(top, h);
            const int maxBlockY = top > chunk.minY ? floorDiv(top - 1, 16) * 16 + 15 : chunk.minY - 1;
            volume_.sizeX = volume_.sizeZ = 16;
            volume_.sizeY = std::clamp(maxBlockY - chunk.minY + 1, 1, chunk.height);
            volume_.minX = chunk.minBlockX();
            volume_.minY = chunk.minY;
            volume_.minZ = chunk.minBlockZ();
        }
    }

    void updateXZ(int blockX, int blockZ, int gradX, int gradZ) {
        ++xzStamp_;
        ++yStamp_;
        blockX_ = blockX, blockZ_ = blockZ, gradX_ = gradX, gradZ_ = gradZ;
        surfaceDepth_ = m_.surfaceDepth(blockX, blockZ);
    }
    void updateY(int stoneAbove, int stoneBelow, int waterHeight, int blockY) {
        ++yStamp_;
        blockY_ = blockY, waterHeight_ = waterHeight, stoneBelow_ = stoneBelow, stoneAbove_ = stoneAbove;
    }

    u16 apply(int ri) {
        const Rule& r = m_.rules[ri];
        switch (r.type) {
        case RuleType::Block: return r.block;
        case RuleType::Bandlands: return band();
        case RuleType::Sequence:
            for (int i = 0; i < r.count; ++i)
                if (const u16 state = apply(m_.children[r.first + i]); state != kNone) return state;
            return kNone;
        case RuleType::Condition: return test(r.cond) ? apply(r.then) : kNone;
        case RuleType::OreVein: return oreVein(m_.ores[r.ore]);
        default: return kNone;
        }
    }

    u16 biomeAt(int x, int y, int z) { return grid_ ? grid_->biome(x, y, z) : zoom_.biome(x, y, z, noiseBiome_); }

    const BiomeEntry& entry(u16 id) {
        if (lastEntry_ >= 0 && entries_[lastEntry_].id == id) return entries_[lastEntry_];
        for (int i = 0; i < entryCount_; ++i)
            if (entries_[i].id == id) return entries_[lastEntry_ = i];
        const int slot = entryCount_ < kEntries ? entryCount_++ : (nextEntry_++ % kEntries);
        entries_[slot] = m_.describe(id);
        return entries_[lastEntry_ = slot];
    }

    int minSurfaceLevel() {
        if (minSurfaceStamp_ != xzStamp_) {
            float level = 0.0f;
            const int lx = blockX_ - chunk_.minBlockX(), lz = blockZ_ - chunk_.minBlockZ();
            if (!m_.preliminarySurface) {
                level = 0.0f;  // already reported at construction
            } else if (prefill_ && unsigned(lx) < 16u && unsigned(lz) < 16u) {
                if (!surfaceFilled_) {
                    Volume v;
                    v.sizeX = v.sizeZ = 16;
                    v.minX = chunk_.minBlockX();
                    v.minY = 0;
                    v.minZ = chunk_.minBlockZ();
                    m_.noise.sampleVolume(m_.preliminarySurface, dctx_, v, surface_);
                    surfaceFilled_ = true;
                }
                level = surface_[lx + lz * 16];
            } else {
                level = m_.noise.sample(m_.preliminarySurface, dctx_, blockX_, 0, blockZ_);
            }
            minSurface_ = jfloor(double(level)) + surfaceDepth_ - 8;
            minSurfaceStamp_ = xzStamp_;
        }
        return minSurface_;
    }

    // BlockColumn.setBlock + ProtoChunk.setBlockState's WORLD_SURFACE_WG / OCEAN_FLOOR_WG updates.
    void setBlock(int x, int y, int z, u16 id) {
        if (!chunk_.inside(y)) return;
        chunk_.set(x, y, z, id);
        updateHeightmap(chunk_.worldSurfaceWG[z * 16 + x], x, y, z, id, [](const Impl&, u16 b) { return b != 0; });
        updateHeightmap(chunk_.oceanFloorWG[z * 16 + x], x, y, z, id,
                        [](const Impl& m, u16 b) { return m.blocksMotion(b); });
    }

    void erodedBadlands(int x, int z, int blockX, int blockZ, int height) {
        const double pillarBuffer =
            javaMin(std::abs(double(sampleNoise(m_.badlandsSurface, blockX, 0.0, blockZ)) * 8.25),
                    double(sampleNoise(m_.badlandsPillar, blockX * 0.2, 0.0, blockZ * 0.2) * 15.0f));
        if (pillarBuffer <= 0.0) return;
        const double pillarFloor =
            std::abs(double(sampleNoise(m_.badlandsPillarRoof, blockX * 0.75, 0.0, blockZ * 0.75)) * 1.5);
        const double extensionTop =
            64.0 + javaMin(pillarBuffer * pillarBuffer * 2.5, std::ceil(pillarFloor * 50.0) + 24.0);
        const int startY = jfloor(extensionTop);
        if (height > startY) return;
        for (int y = startY; y >= chunk_.minY; --y) {
            const u16 old = chunk_.get(x, y, z);
            if (m_.isDefaultBlock(old)) break;
            if (m_.isWater(old)) return;
        }
        for (int y = startY; y >= chunk_.minY && chunk_.get(x, y, z) == GenChunk::kAir; --y)
            setBlock(x, y, z, m_.defaultBlock);
    }

    void frozenOcean(int minSurfaceLevel, const BiomeEntry& biome, int x, int z, int blockX, int blockZ, int height) {
        const double iceberg =
            javaMin(std::abs(double(sampleNoise(m_.icebergSurface, blockX, 0.0, blockZ)) * 8.25),
                    double(sampleNoise(m_.icebergPillar, blockX * 1.28, 0.0, blockZ * 1.28) * 15.0f));
        if (iceberg <= 1.8) return;
        const double roof = std::abs(double(sampleNoise(m_.icebergPillarRoof, blockX * 1.17, 0.0, blockZ * 1.17)) * 1.5);
        double top = javaMin(iceberg * iceberg * 1.2, std::ceil(roof * 40.0) + 14.0);
        if (m_.temperature(biome, blockX, m_.seaLevel, blockZ) > 0.1f) top -= 2.0;  // shouldMeltFrozenOceanIcebergSlightly
        if (top <= 2.0) return;
        const double extensionBottom = m_.seaLevel - top - 7.0;
        top += m_.seaLevel;
        const int topY = javaInt(top), bottomY = javaInt(extensionBottom);
        m_.noiseRandom.with(blockX, 0, blockZ, [&](auto& random) {
            const int maxSnowDepth = 2 + random.nextInt(4);
            const int minSnowHeight = m_.seaLevel + 18 + random.nextInt(10);
            int snowDepth = 0;
            for (int y = std::max(height, topY + 1); y >= minSurfaceLevel; --y) {
                if ((chunk_.get(x, y, z) == GenChunk::kAir && y < topY && random.nextDouble() > 0.01) ||
                    (m_.isWater(chunk_.get(x, y, z)) && y > bottomY && y < m_.seaLevel && random.nextDouble() > 0.15)) {
                    if (snowDepth <= maxSnowDepth && y > minSnowHeight) {
                        setBlock(x, y, z, m_.snowBlock);
                        ++snowDepth;
                    } else {
                        setBlock(x, y, z, m_.packedIce);
                    }
                }
            }
            return 0;
        });
    }

private:
    static constexpr int kEntries = 16;

    u16 biome() {
        if (biomeStamp_ != yStamp_) {
            biome_ = biomeAt(blockX_, blockY_, blockZ_);
            biomeStamp_ = yStamp_;
        }
        return biome_;
    }

    int resolve(const Anchor& a) const {
        switch (a.kind) {
        case Anchor::AboveBottom: return minGenY_ + a.value;
        case Anchor::BelowTop: return genDepth_ - 1 + minGenY_ - a.value;
        case Anchor::SeaLevel: return m_.seaLevel + a.value;
        default: return a.value;
        }
    }

    double surfaceSecondary() {
        if (secondaryStamp_ != xzStamp_) {
            secondary_ = sampleNoise(m_.surfaceSecondary, blockX_, 0.0, blockZ_);
            secondaryStamp_ = xzStamp_;
        }
        return secondary_;
    }

    double noiseValue(int slot) {
        const NoiseSlot& n = m_.noises[slot];
        Scratch::NoiseValue& v = s_.noise[slot];
        const u32 stamp = n.is3d ? yStamp_ : xzStamp_;
        if (v.stamp != stamp) {
            v.value = n.noise->get(blockX_, n.is3d ? double(blockY_) : 0.0, blockZ_);
            v.stamp = stamp;
        }
        return v.value;
    }

    bool test(int ci) {
        const Cond& c = m_.conds[ci];
        switch (c.type) {
        case CondType::Biome: {
            const u64* mask = entry(biome()).mask;
            return mask && ((mask[c.ref >> 6] >> (c.ref & 63)) & 1);
        }
        case CondType::Noise: {
            const double v = noiseValue(c.ref);
            return v >= c.min && v <= c.max;
        }
        case CondType::VerticalGradient: {
            const int trueAtAndBelow = resolve(c.lo), falseAtAndAbove = resolve(c.hi);
            if (blockY_ <= trueAtAndBelow) return true;
            if (blockY_ >= falseAtAndAbove) return false;
            const double probability = map(double(blockY_), double(trueAtAndBelow), double(falseAtAndAbove), 1.0, 0.0);
            return m_.randoms[c.ref].with(blockX_, blockY_, blockZ_,
                                          [&](auto& random) { return random.nextFloat() < probability; });
        }
        case CondType::YAbove:
            return blockY_ + (c.add ? stoneAbove_ : 0) >= resolve(c.lo) + surfaceDepth_ * c.mul;
        case CondType::Water:
            return waterHeight_ == INT_MIN ||
                   blockY_ + (c.add ? stoneAbove_ : 0) >= waterHeight_ + c.offset + surfaceDepth_ * c.mul;
        case CondType::Temperature:  // Biome.coldEnoughToSnow
            return !(m_.temperature(entry(biome()), blockX_, blockY_, blockZ_) >= 0.15f);
        case CondType::Steep: return gradX_ <= -4 || gradZ_ >= 4;
        case CondType::Not: return !test(c.ref);
        case CondType::Hole: return surfaceDepth_ <= 0;
        case CondType::AboveSurface: return blockY_ >= minSurfaceLevel();
        case CondType::StoneDepth: {
            const int stoneDepth = c.ceiling ? stoneBelow_ : stoneAbove_;
            const int surfaceDepth = c.add ? surfaceDepth_ : 0;
            const int secondary = c.mul == 0 ? 0 : javaInt(map(surfaceSecondary(), -1.0, 1.0, 0.0, double(c.mul)));
            return stoneDepth <= 1 + c.offset + surfaceDepth + secondary;
        }
        default: return false;
        }
    }

    u16 band() {
        if (bandStamp_ != xzStamp_) {
            bandOffset_ = javaRound(sampleNoise(m_.clayBandsOffset, blockX_, 0.0, blockZ_) * 4.0f);
            bandStamp_ = xzStamp_;
        }
        return m_.bands[floorMod(blockY_ + bandOffset_ + kBands, kBands)];
    }

    float density(int slot) {
        const DensitySlot& d = m_.densities[slot];
        if (prefill_ && d.prefill) {
            const int lx = blockX_ - volume_.minX, ly = blockY_ - volume_.minY, lz = blockZ_ - volume_.minZ;
            if (unsigned(lx) < 16u && unsigned(lz) < 16u && unsigned(ly) < unsigned(volume_.sizeY)) {
                std::vector<float>& buffer = s_.density[slot];
                if (!s_.densityFilled[slot]) {
                    buffer.resize(size_t(volume_.size()));
                    m_.noise.sampleVolume(d.fn, dctx_, volume_, buffer.data());
                    s_.densityFilled[slot] = 1;
                }
                return buffer[size_t(volume_.index(lx, ly, lz))];
            }
        }
        return m_.noise.sample(d.fn, dctx_, blockX_, blockY_, blockZ_);
    }

    u16 oreVein(const OreVein& o) {
        const float veinDensity = density(o.density);
        if (veinDensity <= 0.0f) return kNone;
        return m_.randoms[o.random].with(blockX_, blockY_, blockZ_, [&](auto& random) -> u16 {
            if (random.nextFloat() > veinDensity) return kNone;
            const float richness = density(o.richness);
            if (random.nextFloat() < richness && density(o.gap) < 0.0f)
                return random.nextFloat() < o.rawChance ? o.raw : o.ore;
            return o.filler;
        });
    }

    // Heightmap.update
    template <class Pred>
    void updateHeightmap(int& firstAvailable, int x, int y, int z, u16 id, Pred opaque) {
        if (y <= firstAvailable - 2) return;
        if (opaque(m_, id)) {
            if (y >= firstAvailable) firstAvailable = y + 1;
            return;
        }
        if (firstAvailable - 1 != y) return;
        for (int below = y - 1; below >= chunk_.minY; --below)
            if (opaque(m_, chunk_.get(x, below, z))) {
                firstAvailable = below + 1;
                return;
            }
        firstAvailable = chunk_.minY;
    }

    const Impl& m_;
    GenChunk& chunk_;
    DensityContext& dctx_;
    const BiomeZoom& zoom_;
    const NoiseBiomeFn& noiseBiome_;
    BiomeGrid* grid_;
    bool prefill_;
    Scratch& s_;
    int minGenY_ = 0, genDepth_ = 0;
    Volume volume_;  // ore vein prefill box

    u32 xzStamp_ = 0, yStamp_ = 0;
    int blockX_ = 0, blockZ_ = 0, gradX_ = 0, gradZ_ = 0, surfaceDepth_ = 0;
    int blockY_ = 0, waterHeight_ = INT_MIN, stoneBelow_ = 0, stoneAbove_ = 0;
    u32 secondaryStamp_ = 0, minSurfaceStamp_ = 0, bandStamp_ = 0, biomeStamp_ = 0;
    double secondary_ = 0;
    int minSurface_ = 0, bandOffset_ = 0;
    u16 biome_ = 0;
    bool surfaceFilled_ = false;
    float surface_[256];

    BiomeEntry entries_[kEntries];
    int entryCount_ = 0, nextEntry_ = 0, lastEntry_ = -1;
};

}  // namespace

MaterialSystem::MaterialSystem(const NoiseState& noise, BlockRegistry& blocks, const BiomeTable& biomes,
                               const nlohmann::json& settings)
    : impl_(std::make_unique<Impl>(noise, blocks, biomes)) {
    Impl& m = *impl_;
    m.seaLevel = settings.value("sea_level", 63);
    if (auto it = settings.find("noise"); it != settings.end() && it->is_object()) {
        m.genMinY = it->value("min_y", -64);
        m.genHeight = it->value("height", 384);
    }
    const json defaultBlock = settings.contains("default_block") ? settings["default_block"] : json("minecraft:stone");
    m.defaultBlock = m.stateId(defaultBlock);
    m.defaultBlockName = std::string(baseName(stateKey(defaultBlock)));
    m.water = blocks.idFor("minecraft:water");
    m.waterSource = blocks.idFor("minecraft:water[level=0]");
    m.packedIce = blocks.idFor("minecraft:packed_ice");
    m.snowBlock = blocks.idFor("minecraft:snow_block");
    for (const std::string& b : noise.data().tag("block", "blocks_motion_in_heightmap"))
        m.motionBlocking.insert(withNamespace(b));

    auto getNoise = [&](const char* id) {
        NoisePtr n = noise.noise(id);
        if (!n) LOG("materials: missing noise %s", id);
        return n;
    };
    m.clayBandsOffset = getNoise("minecraft:clay_bands_offset");
    m.surface = getNoise("minecraft:surface");
    m.surfaceSecondary = getNoise("minecraft:surface_secondary");
    m.badlandsPillar = getNoise("minecraft:badlands_pillar");
    m.badlandsPillarRoof = getNoise("minecraft:badlands_pillar_roof");
    m.badlandsSurface = getNoise("minecraft:badlands_surface");
    m.icebergPillar = getNoise("minecraft:iceberg_pillar");
    m.icebergPillarRoof = getNoise("minecraft:iceberg_pillar_roof");
    m.icebergSurface = getNoise("minecraft:iceberg_surface");

    m.noiseRandom = rootRandom(noise);
    m.generateBands(*noise.random().fromHashOf("minecraft:clay_bands"));

    // Biome's static temperature noises (WorldgenRandom over LegacyRandomSource).
    LegacyRandom temperatureRandom(1234), frozenRandom(3456), biomeInfoRandom(2345);
    m.temperatureNoise.init(temperatureRandom);
    for (Simplex2D& n : m.frozenNoise) n.init(frozenRandom);
    m.biomeInfoNoise.init(biomeInfoRandom);

    try {
        if (auto router = settings.find("noise_router"); router != settings.end() && router->contains("chunk_surface_level"))
            m.preliminarySurface = noise.parse((*router)["chunk_surface_level"]);
        if (!m.preliminarySurface) LOG("materials: noise_router.chunk_surface_level missing or invalid");
        Compiler compiler(m);
        if (settings.contains("material_rule")) {
            m.root = compiler.rule(settings["material_rule"]);
        } else {
            LOG("materials: noise settings have no material_rule");
            m.rules.push_back(Rule{});
            m.root = int(m.rules.size()) - 1;
        }
        compiler.finish();
    } catch (const std::exception& e) {
        LOG("materials: bad material rule data: %s", e.what());
        m.rules.push_back(Rule{});
        m.root = int(m.rules.size()) - 1;
    }
}

MaterialSystem::~MaterialSystem() = default;

void MaterialSystem::buildSurface(GenChunk& chunk, DensityContext& ctx, const BiomeZoom& zoom,
                                  const NoiseBiomeFn& noiseBiome) const {
    const Impl& m = *impl_;
    BiomeGrid grid(chunk, zoom, noiseBiome);
    Eval e(m, chunk, ctx, zoom, grid.fn, &grid, true);
    const int endY = chunk.minY, maxY = chunk.minY + chunk.height - 1;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            const int blockX = chunk.minBlockX() + x, blockZ = chunk.minBlockZ() + z;
            const int startingHeight = chunk.worldSurfaceWG[z * 16 + x];
            const BiomeEntry surfaceBiome = e.entry(grid.biome(blockX, startingHeight, blockZ));
            if (surfaceBiome.erodedBadlands) e.erodedBadlands(x, z, blockX, blockZ, startingHeight);

            const int height = chunk.worldSurfaceWG[z * 16 + x];
            e.updateXZ(blockX, blockZ, gradientX(chunk, x, z), gradientZ(chunk, x, z));
            int stoneAboveDepth = 0, waterHeight = INT_MIN, nextCeilingStoneY = INT_MAX;
            for (int y = height; y >= endY; --y) {
                const u16 old = chunk.get(x, y, z);
                if (old == GenChunk::kAir) {
                    stoneAboveDepth = 0;
                    waterHeight = INT_MIN;
                } else if (m.hasFluid(old)) {
                    if (waterHeight == INT_MIN) waterHeight = y + 1;
                } else {
                    if (nextCeilingStoneY >= y) {
                        nextCeilingStoneY = kWayBelowMinY;
                        for (int below = y - 1; below >= endY - 1; --below)
                            if (!m.isStone(chunk.get(x, below, z))) {
                                nextCeilingStoneY = below + 1;
                                break;
                            }
                    }
                    ++stoneAboveDepth;
                    e.updateY(stoneAboveDepth, y - nextCeilingStoneY + 1, waterHeight, y);
                    if (y >= endY && y <= maxY)
                        if (const u16 state = e.apply(m.root); state != kNone) e.setBlock(x, y, z, state);
                }
            }

            if (surfaceBiome.frozenOcean)
                e.frozenOcean(e.minSurfaceLevel(), surfaceBiome, x, z, blockX, blockZ, startingHeight);
        }
    }
}

std::optional<u16> MaterialSystem::topMaterial(GenChunk& chunk, DensityContext& ctx, const BiomeZoom& zoom,
                                               const NoiseBiomeFn& noiseBiome, int x, int y, int z,
                                               bool underFluid) const {
    // Works with world or chunk-local x/z: only the position inside the chunk matters.
    const int lx = x & 15, lz = z & 15;
    Eval e(*impl_, chunk, ctx, zoom, noiseBiome, nullptr, false);
    e.updateXZ(chunk.minBlockX() + lx, chunk.minBlockZ() + lz, gradientX(chunk, lx, lz), gradientZ(chunk, lx, lz));
    e.updateY(1, 1, underFluid ? y + 1 : INT_MIN, y);
    const u16 state = e.apply(impl_->root);
    if (state == kNone) return std::nullopt;
    return state;
}

std::optional<u16> MaterialSystem::materialAt(GenChunk& chunk, DensityContext& ctx, const BiomeZoom& zoom,
                                              const NoiseBiomeFn& noiseBiome, int x, int y, int z, int stoneAbove,
                                              int stoneBelow, int waterHeight) const {
    const int lx = x & 15, lz = z & 15;
    Eval e(*impl_, chunk, ctx, zoom, noiseBiome, nullptr, false);
    e.updateXZ(chunk.minBlockX() + lx, chunk.minBlockZ() + lz, gradientX(chunk, lx, lz), gradientZ(chunk, lx, lz));
    e.updateY(stoneAbove, stoneBelow, waterHeight, y);
    const u16 state = e.apply(impl_->root);
    if (state == kNone) return std::nullopt;
    return state;
}

}  // namespace wg
