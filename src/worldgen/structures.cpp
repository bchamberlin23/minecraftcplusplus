// StructureSystem: structure sets and placements (ChunkGeneratorStructureState, RandomSpread / ConcentricRings
// placements with frequency reduction and exclusion zones), ChunkGenerator.createStructures / tryGenerateStructure,
// structure references, the Beardifier and the structure part of applyBiomeDecoration.
#include "worldgen/structures.h"

#include "assets/assets.h"
#include "worldgen/jigsaw.h"
#include "worldgen/structure_core.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>

namespace wg {

using namespace st;

namespace {

int stepOrdinal(std::string_view n) {
    static constexpr const char* kSteps[] = {"raw_generation", "lakes", "local_modifications", "underground_structures",
                                             "surface_structures", "strongholds", "underground_ores", "underground_decoration",
                                             "fluid_springs", "vegetal_decoration", "top_layer_modification"};
    for (int i = 0; i < 11; ++i)
        if (n == kSteps[i]) return i;
    return 4;
}

Adapt adaptFromName(std::string_view n) {
    if (n == "bury") return Adapt::Bury;
    if (n == "beard_thin") return Adapt::BeardThin;
    if (n == "beard_box") return Adapt::BeardBox;
    if (n == "encapsulate") return Adapt::Encapsulate;
    return Adapt::None;
}

i64 packChunk(int x, int z) { return i64(u64(u32(x)) | (u64(u32(z)) << 32)); }

// fastutil LongOpenHashSet iteration order for a few keys inserted in order (expected size 16: a 32 slot table).
std::vector<i64> longSetOrder(const std::vector<i64>& inserted) {
    if (inserted.size() <= 1) return inserted;
    int n = 32;
    while (int(inserted.size()) > n * 3 / 4) n *= 2;
    std::vector<i64> table(size_t(n), 0);
    std::vector<bool> used(size_t(n), false);
    bool hasNull = false;
    for (i64 k : inserted) {
        if (k == 0) {
            hasNull = true;
            continue;
        }
        u64 h = u64(k) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 32;
        h ^= h >> 16;
        int pos = int(h) & (n - 1);
        while (used[size_t(pos)] && table[size_t(pos)] != k) pos = (pos + 1) & (n - 1);
        used[size_t(pos)] = true;
        table[size_t(pos)] = k;
    }
    std::vector<i64> out;
    if (hasNull) out.push_back(0);
    for (int i = n - 1; i >= 0; --i)
        if (used[size_t(i)]) out.push_back(table[size_t(i)]);
    return out;
}

struct Placement {
    enum class Type { RandomSpread, ConcentricRings, Origin } type = Type::RandomSpread;
    BlockPos locateOffset{0, 0, 0};
    enum class Reduction { Default, Legacy1, Legacy2, Legacy3 } reduction = Reduction::Default;
    float frequency = 1.0f;
    int salt = 0;
    int exclusionSet = -1, exclusionChunks = 0;
    std::string exclusionId;
    // random_spread
    int spacing = 1, separation = 0;
    bool triangular = false;
    // concentric_rings
    int distance = 0, spread = 0, count = 0;
    std::vector<bool> preferred;
};

struct Ring {
    int initialX, initialZ;
    i64 forkSeed;
    std::once_flag once;
    int x = 0, z = 0;
};

struct Set {
    std::string id;
    std::vector<std::pair<const Structure*, int>> structures;
    Placement placement;
    std::vector<std::unique_ptr<Ring>> rings;
};

struct StartEntry {
    std::once_flag once;
    std::vector<std::shared_ptr<Start>> starts;
};

struct LockedStart {
    std::shared_ptr<Start> start;
    std::mutex* mutex;
};

}  // namespace

struct StructureSystem::Impl {
    std::unique_ptr<StructureEnv> env;
    std::map<std::string, std::unique_ptr<Structure>> structures;  // by id
    std::vector<const Structure*> ordered;                         // registry order
    std::vector<std::vector<const Structure*>> byStep;             // GenerationStep -> structures in registry order
    std::vector<Set> sets;
    bool ok = false;

    mutable std::mutex startsMutex;
    mutable std::unordered_map<u64, std::shared_ptr<StartEntry>> startCache;
    mutable std::mutex placeMutex;  // pieces change while they are placed (heights found on first placement)
    mutable double startMs = 0;

    // ---- placement ----
    bool isPlacementChunk(const Set& s, int x, int z) const;
    bool isStructureChunk(const Set& s, int x, int z) const;
    bool frequencyOk(const Placement& p, int x, int z) const;
    void resolveRing(const Set& s, Ring& r) const;

    // ---- starts ----
    const std::vector<std::shared_ptr<Start>>& starts(int cx, int cz) const;
    void createStructures(int cx, int cz, std::vector<std::shared_ptr<Start>>& out) const;
    std::shared_ptr<Start> tryGenerate(const Structure& s, int cx, int cz) const;
    // references: structure -> source chunks (LongOpenHashSet order), for the structures matching `filter`
    std::vector<std::shared_ptr<Start>> startsFor(int cx, int cz, const Structure* only, bool adaptingOnly) const;
};

// ---- placement --------------------------------------------------------------------------------------------------

bool StructureSystem::Impl::frequencyOk(const Placement& p, int x, int z) const {
    if (!(p.frequency < 1.0f)) return true;
    const i64 seed = env->seed;
    switch (p.reduction) {
    case Placement::Reduction::Default: {
        LegacyRandom r(0);
        r.setSeed(jadd(jadd(jadd(jmul(i64(x), 341873128712LL), jmul(i64(z), 132897987541LL)), seed), i64(p.salt)));
        return r.nextFloat() < p.frequency;
    }
    case Placement::Reduction::Legacy1: {
        const int cx = x >> 4, cz = z >> 4;
        LegacyRandom r(0);
        r.setSeed(i64(cx ^ (cz << 4)) ^ seed);
        r.nextInt();
        return r.nextInt(int(1.0f / p.frequency)) == 0;
    }
    case Placement::Reduction::Legacy2: {
        LegacyRandom r(0);
        r.setSeed(jadd(jadd(jadd(jmul(i64(x), 341873128712LL), jmul(i64(z), 132897987541LL)), seed), 10387320LL));
        return r.nextFloat() < p.frequency;
    }
    case Placement::Reduction::Legacy3: {
        LegacyRandom r(0);
        r.setSeed(seed);
        const i64 xs = r.nextLong(), zs = r.nextLong();
        r.setSeed(jmul(i64(x), xs) ^ jmul(i64(z), zs) ^ seed);
        return r.nextDouble() < double(p.frequency);
    }
    }
    return true;
}

void StructureSystem::Impl::resolveRing(const Set& s, Ring& r) const {
    std::call_once(r.once, [&] {
        // BiomeSource.findBiomeHorizontal(x, 0, z, 112, 1, preferred, random, false)
        LegacyRandom random(r.forkSeed);
        const int ox = r.initialX * 16 + 8, oz = r.initialZ * 16 + 8;
        const int ncx = ox >> 2, ncz = oz >> 2, radius = 112 >> 2;
        DensityContext ctx;
        bool found = false;
        int count = 0, rx = 0, rz = 0;
        for (int z = -radius; z <= radius; ++z)
            for (int x = -radius; x <= radius; ++x) {
                const u16 biome = env->gen.noiseBiome(ctx, ncx + x, 0, ncz + z);
                if (biome >= s.placement.preferred.size() || !s.placement.preferred[biome]) continue;
                if (!found || random.nextInt(count + 1) == 0) {
                    rx = (ncx + x) * 4, rz = (ncz + z) * 4;
                    found = true;
                }
                ++count;
            }
        if (found)
            r.x = rx >> 4, r.z = rz >> 4;
        else
            r.x = r.initialX, r.z = r.initialZ;
    });
}

bool StructureSystem::Impl::isPlacementChunk(const Set& s, int x, int z) const {
    const Placement& p = s.placement;
    switch (p.type) {
    case Placement::Type::RandomSpread: {
        const int gx = floorDiv(x, p.spacing), gz = floorDiv(z, p.spacing);
        LegacyRandom r(0);
        r.setSeed(jadd(jadd(jadd(jmul(i64(gx), 341873128712LL), jmul(i64(gz), 132897987541LL)), env->seed), i64(p.salt)));
        const int limit = p.spacing - p.separation;
        auto eval = [&] { return p.triangular ? (r.nextInt(limit) + r.nextInt(limit)) / 2 : r.nextInt(limit); };
        const int sx = eval();
        const int sz = eval();
        return gx * p.spacing + sx == x && gz * p.spacing + sz == z;
    }
    case Placement::Type::ConcentricRings: {
        for (const auto& ring : s.rings) {
            // The biome search moves a position at most 112 blocks from its initial chunk.
            if (std::abs(ring->initialX - x) > 8 || std::abs(ring->initialZ - z) > 8) continue;
            resolveRing(s, *ring);
            if (ring->x == x && ring->z == z) return true;
        }
        return false;
    }
    case Placement::Type::Origin: return x == 0 && z == 0;
    }
    return false;
}

bool StructureSystem::Impl::isStructureChunk(const Set& s, int x, int z) const {
    if (!isPlacementChunk(s, x, z)) return false;
    if (s.placement.type == Placement::Type::Origin) return true;
    if (!frequencyOk(s.placement, x, z)) return false;
    if (s.placement.exclusionSet >= 0) {
        const Set& other = sets[size_t(s.placement.exclusionSet)];
        const int r = s.placement.exclusionChunks;
        for (int tx = x - r; tx <= x + r; ++tx)
            for (int tz = z - r; tz <= z + r; ++tz)
                if (isStructureChunk(other, tx, tz)) return false;
    }
    return true;
}

// ---- starts ---------------------------------------------------------------------------------------------------------

std::shared_ptr<Start> StructureSystem::Impl::tryGenerate(const Structure& s, int cx, int cz) const {
    thread_local DensityContext ctx;
    GenCtx c(*env, ctx, env->seed, cx, cz, &s.biomes);
    std::optional<Stub> stub = s.findGenerationPoint(c);
    if (!stub || !c.isValidBiomeAt(stub->position)) return nullptr;
    PiecesBuilder builder;
    stub->generator(builder);
    if (builder.empty()) return nullptr;
    auto start = std::make_shared<Start>();
    start->structure = &s;
    start->chunkX = cx, start->chunkZ = cz;
    start->pieces = std::move(builder.pieces);
    BBox b = start->pieces.front()->box;
    for (const auto& p : start->pieces) b.encapsulate(p->box);
    start->box = s.adjustBoundingBox(b);
    return start;
}

void StructureSystem::Impl::createStructures(int cx, int cz, std::vector<std::shared_ptr<Start>>& out) const {
    for (const Set& set : sets) {
        if (set.structures.empty() || !isStructureChunk(set, cx, cz)) continue;
        if (set.structures.size() == 1) {
            if (auto st = tryGenerate(*set.structures[0].first, cx, cz)) out.push_back(std::move(st));
            continue;
        }
        std::vector<std::pair<const Structure*, int>> options = set.structures;
        WorldgenRandom random(std::make_unique<LegacyRandom>(0));
        random.setSeed(env->seed);
        const i64 xs = random.nextLong(), zs = random.nextLong();
        random.setSeed(jmul(i64(cx), xs) ^ jmul(i64(cz), zs) ^ env->seed);
        int total = 0;
        for (const auto& o : options) total += o.second;
        while (!options.empty()) {
            int choice = random.nextInt(total);
            size_t index = 0;
            for (const auto& o : options) {
                choice -= o.second;
                if (choice < 0) break;
                ++index;
            }
            const auto selected = options[index];
            if (auto st = tryGenerate(*selected.first, cx, cz)) {
                out.push_back(std::move(st));
                break;
            }
            options.erase(options.begin() + std::ptrdiff_t(index));
            total -= selected.second;
        }
    }
}

const std::vector<std::shared_ptr<Start>>& StructureSystem::Impl::starts(int cx, int cz) const {
    std::shared_ptr<StartEntry> e;
    {
        std::lock_guard lock(startsMutex);
        auto& slot = startCache[(u64(u32(cx)) << 32) | u32(cz)];
        if (!slot) slot = std::make_shared<StartEntry>();
        e = slot;
    }
    std::call_once(e->once, [&] {
        Timer t;
        createStructures(cx, cz, e->starts);
        std::lock_guard lock(startsMutex);
        startMs += t.ms();
    });
    return e->starts;
}

std::vector<std::shared_ptr<Start>> StructureSystem::Impl::startsFor(int cx, int cz, const Structure* only, bool adaptingOnly) const {
    // createReferences: sources within 8 chunks whose start box reaches this chunk, per structure in source order.
    const int x0 = cx * 16, z0 = cz * 16;
    std::vector<std::pair<const Structure*, std::vector<std::pair<i64, std::shared_ptr<Start>>>>> refs;
    for (int sx = cx - 8; sx <= cx + 8; ++sx)
        for (int sz = cz - 8; sz <= cz + 8; ++sz)
            for (const auto& start : starts(sx, sz)) {
                if (only && start->structure != only) continue;
                if (adaptingOnly && start->structure->adapt == Adapt::None) continue;
                if (!start->box.intersectsXZ(x0, z0, x0 + 15, z0 + 15)) continue;
                auto it = std::find_if(refs.begin(), refs.end(), [&](const auto& r) { return r.first == start->structure; });
                if (it == refs.end()) refs.push_back({start->structure, {}}), it = refs.end() - 1;
                it->second.push_back({packChunk(sx, sz), start});
            }
    std::sort(refs.begin(), refs.end(), [](const auto& a, const auto& b) { return a.first->id < b.first->id; });
    std::vector<std::shared_ptr<Start>> out;
    for (auto& [structure, list] : refs) {
        std::vector<i64> keys;
        for (const auto& [k, s] : list) keys.push_back(k);
        for (i64 k : longSetOrder(keys))
            for (const auto& [kk, s] : list)
                if (kk == k) out.push_back(s);
    }
    return out;
}

// ---- Beardifier ---------------------------------------------------------------------------------------------------------

namespace {

float beardKernel(int xi, int yi, int zi) {
    static const std::vector<float> kernel = [] {
        std::vector<float> k(24 * 24 * 24);
        for (int z = 0; z < 24; ++z)
            for (int x = 0; x < 24; ++x)
                for (int y = 0; y < 24; ++y) {
                    const double dx = x - 12, dy = double(y - 12) + 0.5, dz = z - 12;
                    const double d2 = dx * dx + dy * dy + dz * dz;
                    k[size_t(z * 24 * 24 + x * 24 + y)] = float(std::pow(std::numbers::e, -d2 / 16.0));
                }
        return k;
    }();
    return kernel[size_t(zi * 24 * 24 + xi * 24 + yi)];
}

double fastInvSqrt(double x) {
    const double half = 0.5 * x;
    i64 i;
    std::memcpy(&i, &x, 8);
    i = 6910469410427058090LL - (i >> 1);
    std::memcpy(&x, &i, 8);
    return x * (1.5 - half * x * x);
}

float beardContribution(int dx, int dy, int dz, int yToGround) {
    const int xi = dx + 12, yi = dy + 12, zi = dz + 12;
    if (xi < 0 || xi >= 24 || yi < 0 || yi >= 24 || zi < 0 || zi >= 24) return 0.0f;
    const float dyo = float(yToGround) + 0.5f;
    const float d2 = float(dx) * float(dx) + dyo * dyo + float(dz) * float(dz);
    const float value = -dyo * float(fastInvSqrt(double(d2 / 2.0f))) / 2.0f;
    return value * beardKernel(xi, yi, zi);
}

float buryContribution(float dx, float dy, float dz) {
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 >= 36.0f ? 0.0f : 1.0f - float(std::sqrt(double(d2))) / 6.0f;
}

class Beardifier final : public DensityAddition {
public:
    struct Rigid {
        BBox box;
        Adapt adapt;
        int groundLevelDelta;
    };
    std::vector<Rigid> rigids;
    std::vector<Junction> junctions;
    BBox affected;

    float sample(int x, int y, int z) const {
        float v = 0.0f;
        for (const Rigid& r : rigids) {
            const BBox& b = r.box;
            const int dx = std::max(0, std::max(b.minX - x, x - b.maxX));
            const int dz = std::max(0, std::max(b.minZ - z, z - b.maxZ));
            const int groundY = b.minY + r.groundLevelDelta;
            const int dyToGround = y - groundY;
            int dy = 0;
            switch (r.adapt) {
            case Adapt::Bury: case Adapt::BeardThin: dy = dyToGround; break;
            case Adapt::BeardBox: dy = std::max(0, std::max(groundY - y, y - b.maxY)); break;
            case Adapt::Encapsulate: dy = std::max(0, std::max(b.minY - y, y - b.maxY)); break;
            default: break;
            }
            switch (r.adapt) {
            case Adapt::Bury: v += buryContribution(float(dx), float(dy) / 2.0f, float(dz)); break;
            case Adapt::BeardThin: case Adapt::BeardBox: v += beardContribution(dx, dy, dz, dyToGround) * 0.8f; break;
            case Adapt::Encapsulate: v += buryContribution(float(dx) / 2.0f, float(dy) / 2.0f, float(dz) / 2.0f) * 0.8f; break;
            default: break;
            }
        }
        for (const Junction& j : junctions) {
            const int dx = x - j.sourceX, dy = y - j.sourceGroundY, dz = z - j.sourceZ;
            v += beardContribution(dx, dy, dz, dy) * 0.4f;
        }
        return v;
    }

    void add(const Volume& vol, float* density) const override {
        // DensityVolume intersects / the clamped index range, like Beardifier.sampleVolume.
        const int maxX = vol.minX + (vol.sizeX - 1) * vol.stepX, maxY = vol.minY + (vol.sizeY - 1) * vol.stepY,
                  maxZ = vol.minZ + (vol.sizeZ - 1) * vol.stepZ;
        if (affected.maxX < vol.minX || affected.minX > maxX || affected.maxY < vol.minY || affected.minY > maxY ||
            affected.maxZ < vol.minZ || affected.minZ > maxZ)
            return;
        const int ix0 = floorDiv(std::max(0, affected.minX - vol.minX), vol.stepX);
        const int iy0 = floorDiv(std::max(0, affected.minY - vol.minY), vol.stepY);
        const int iz0 = floorDiv(std::max(0, affected.minZ - vol.minZ), vol.stepZ);
        const int ix1 = std::min(vol.sizeX - 1, floorDiv(affected.maxX - vol.minX, vol.stepX));
        const int iy1 = std::min(vol.sizeY - 1, floorDiv(affected.maxY - vol.minY, vol.stepY));
        const int iz1 = std::min(vol.sizeZ - 1, floorDiv(affected.maxZ - vol.minZ, vol.stepZ));
        for (int z = iz0; z <= iz1; ++z)
            for (int x = ix0; x <= ix1; ++x)
                for (int y = iy0; y <= iy1; ++y) {
                    float& d = density[size_t(vol.index(x, y, z))];
                    d = d + sample(vol.blockX(x), vol.blockY(y), vol.blockZ(z));
                }
    }
};

}  // namespace

// ---- StructureSystem ---------------------------------------------------------------------------------------------------

StructureSystem::StructureSystem(const ChunkGenerator& generator, const FeatureSystem* features, BlockRegistry& blocks, i64 seed)
    : impl_(std::make_unique<Impl>()) {
    static std::once_flag registered;
    std::call_once(registered, [] {
        registerJigsaw();
        registerScattered();
        registerMineshaft();
        registerStronghold();
        registerMonument();
        registerMansion();
        registerOceanStructures();
    });
    Timer t;
    Impl& im = *impl_;
    const DataPack& data = generator.noise().data();
    im.env = std::make_unique<StructureEnv>(generator, features, blocks, data, seed);
    std::vector<bool> possible;
    for (u16 b : generator.possibleBiomes()) {
        if (b >= possible.size()) possible.resize(size_t(b) + 1, false);
        possible[b] = true;
    }
    auto anyPossible = [&](const std::vector<bool>& set) {
        for (size_t i = 0; i < set.size() && i < possible.size(); ++i)
            if (set[i] && possible[i]) return true;
        return false;
    };
    // Structures (registry order).
    size_t unsupported = 0;
    for (const std::string& id : data.ids("structure")) {
        const json* def = data.worldgen("structure", id);
        if (!def) continue;
        const std::string type = def->value("type", std::string());
        const StructureFactory* f = findStructureType(type);
        std::unique_ptr<Structure> s;
        if (f) {
            try {
                s = (*f)(*def, *im.env);
            } catch (const std::exception& e) {
                LOG("structures: %s: %s", id.c_str(), e.what());
            }
        }
        if (!s) {
            ++unsupported;
            continue;
        }
        s->id = id;
        s->type = type;
        s->biomes = im.env->biomeSet(def->value("biomes", json()));
        s->step = stepOrdinal(def->value("step", std::string("surface_structures")));
        s->adapt = adaptFromName(def->value("terrain_adaptation", std::string("none")));
        im.ordered.push_back(s.get());
        im.structures.emplace(id, std::move(s));
    }
    im.byStep.assign(11, {});
    for (const Structure* s : im.ordered) {
        if (size_t(s->step) >= im.byStep.size()) im.byStep.resize(size_t(s->step) + 1);
        im.byStep[size_t(s->step)].push_back(s);
    }
    // Structure sets with at least one structure that can be in this dimension's biomes.
    std::vector<std::string> setIds = data.ids("structure_set");
    for (const std::string& id : setIds) {
        const json* def = data.worldgen("structure_set", id);
        if (!def) continue;
        Set set;
        set.id = id;
        bool anyBiome = false;
        for (const json& e : def->value("structures", json::array())) {
            std::string sid = e.value("structure", std::string());
            if (sid.find(':') == std::string::npos) sid = "minecraft:" + sid;
            auto it = im.structures.find(sid);
            if (it == im.structures.end()) continue;
            if (anyPossible(it->second->biomes)) anyBiome = true;
            set.structures.push_back({it->second.get(), e.value("weight", 1)});
        }
        if (!anyBiome) continue;
        const json& pj = def->at("placement");
        Placement& p = set.placement;
        const std::string ptype = DataPack::normalize(pj.value("type", std::string()));
        p.type = ptype == "concentric_rings" ? Placement::Type::ConcentricRings
                 : ptype == "dimension_origin" ? Placement::Type::Origin : Placement::Type::RandomSpread;
        p.salt = pj.value("salt", 0);
        p.frequency = pj.value("frequency", 1.0f);
        const std::string red = pj.value("frequency_reduction_method", std::string("default"));
        p.reduction = red == "legacy_type_1" ? Placement::Reduction::Legacy1 : red == "legacy_type_2" ? Placement::Reduction::Legacy2
                      : red == "legacy_type_3" ? Placement::Reduction::Legacy3 : Placement::Reduction::Default;
        if (auto it = pj.find("exclusion_zone"); it != pj.end()) {
            p.exclusionId = it->value("other_set", std::string());
            if (p.exclusionId.find(':') == std::string::npos) p.exclusionId = "minecraft:" + p.exclusionId;
            p.exclusionChunks = it->value("chunk_count", 1);
        }
        if (auto it = pj.find("locate_offset"); it != pj.end() && it->is_array() && it->size() == 3)
            p.locateOffset = {(*it)[0].get<int>(), (*it)[1].get<int>(), (*it)[2].get<int>()};
        p.spacing = pj.value("spacing", 1);
        p.separation = pj.value("separation", 0);
        p.triangular = pj.value("spread_type", std::string("linear")) == "triangular";
        p.distance = pj.value("distance", 0);
        p.spread = pj.value("spread", 0);
        p.count = pj.value("count", 0);
        if (pj.contains("preferred_biomes")) p.preferred = im.env->biomeSet(pj["preferred_biomes"]);
        im.sets.push_back(std::move(set));
    }
    for (Set& set : im.sets) {
        Placement& p = set.placement;
        if (!p.exclusionId.empty())
            for (size_t i = 0; i < im.sets.size(); ++i)
                if (im.sets[i].id == p.exclusionId) p.exclusionSet = int(i);
        if (p.type != Placement::Type::ConcentricRings || p.count == 0) continue;
        // ChunkGeneratorStructureState.generateRingPositions: the initial positions and each one's biome search seed
        // (concentric rings seed = the level seed for normal worlds).
        LegacyRandom random(0);
        random.setSeed(seed);
        double angle = random.nextDouble() * std::numbers::pi * 2.0;
        int positionInCircle = 0, circle = 0, spread = p.spread;
        for (int i = 0; i < p.count; ++i) {
            const double dist = double(4 * p.distance + p.distance * circle * 6) + (random.nextDouble() - 0.5) * (double(p.distance) * 2.5);
            auto ring = std::make_unique<Ring>();
            ring->initialX = int(std::llround(std::cos(angle) * dist));
            ring->initialZ = int(std::llround(std::sin(angle) * dist));
            ring->forkSeed = random.nextLong();  // random.fork()
            set.rings.push_back(std::move(ring));
            angle += std::numbers::pi * 2.0 / double(spread);
            if (++positionInCircle == spread) {
                ++circle;
                positionInCircle = 0;
                spread += 2 * spread / (circle + 1);
                spread = std::min(spread, p.count - i);
                angle += random.nextDouble() * std::numbers::pi * 2.0;
            }
        }
    }
    im.ok = true;
    LOG("structures: %zu structures (%zu of unsupported types), %zu sets ready in %.1f ms", im.ordered.size(), unsupported,
        im.sets.size(), t.ms());
}

StructureSystem::~StructureSystem() = default;

bool StructureSystem::ok() const { return impl_->ok; }
st::StructureEnv& StructureSystem::env() const { return *impl_->env; }
const st::Pools& StructureSystem::pools() const { return *impl_->env->pools; }
double StructureSystem::startMs() const {
    std::lock_guard lock(impl_->startsMutex);
    return impl_->startMs;
}

std::unique_ptr<DensityAddition> StructureSystem::beardifier(int cx, int cz) const {
    const Impl& im = *impl_;
    const std::vector<std::shared_ptr<Start>> starts = im.startsFor(cx, cz, nullptr, true);
    if (starts.empty()) return nullptr;
    auto b = std::make_unique<Beardifier>();
    const int x0 = cx * 16, z0 = cz * 16;
    std::optional<BBox> any;
    auto include = [&](const BBox& box) {
        if (any) any = BBox(*any).encapsulate(box);
        else any = box;
    };
    for (const auto& start : starts) {
        const Adapt adapt = start->structure->adapt;
        for (const auto& piece : start->pieces) {
            if (!piece->isCloseToChunk(cx, cz, 12)) continue;
            if (const auto* pp = dynamic_cast<const PoolPiece*>(piece.get())) {
                if (pp->element->rigid) {
                    b->rigids.push_back({pp->box, adapt, pp->groundLevelDelta});
                    include(pp->box);
                }
                for (const Junction& j : pp->junctions)
                    if (j.sourceX > x0 - 12 && j.sourceZ > z0 - 12 && j.sourceX < x0 + 15 + 12 && j.sourceZ < z0 + 15 + 12) {
                        b->junctions.push_back(j);
                        include(BBox(j.sourceX, j.sourceGroundY, j.sourceZ, j.sourceX, j.sourceGroundY, j.sourceZ));
                    }
            } else {
                b->rigids.push_back({piece->box, adapt, 0});
                include(piece->box);
            }
        }
    }
    if (!any) return nullptr;
    b->affected = any->inflatedBy(24);
    return b;
}

void StructureSystem::placeStep(int step, GenRegion& level, int cx, int cz, WorldgenRandom& random, i64 decorationSeed) const {
    const Impl& im = *impl_;
    if (step < 0 || size_t(step) >= im.byStep.size() || im.byStep[size_t(step)].empty()) return;
    // The chunk's references, once for all of its steps.
    thread_local const Impl* refOwner = nullptr;
    thread_local int refX = INT32_MIN, refZ = INT32_MIN;
    thread_local std::vector<std::shared_ptr<Start>> refs;
    if (refOwner != &im || refX != cx || refZ != cz) {
        refs = im.startsFor(cx, cz, nullptr, false);
        refOwner = &im, refX = cx, refZ = cz;
    }
    if (refs.empty()) return;
    const StructureEnv& env = *im.env;
    // ChunkGenerator.getWritableArea
    const BBox chunkBB(cx * 16, env.minY() + 1, cz * 16, cx * 16 + 15, env.minY() + env.height() - 1, cz * 16 + 15);
    PlaceCtx c{level, env, env.b, cx, cz};
    int index = 0;
    for (const Structure* s : im.byStep[size_t(step)]) {
        random.setFeatureSeed(decorationSeed, index++, step);
        for (const auto& start : refs) {
            if (start->structure != s) continue;
            std::lock_guard lock(start->placeMutex);
            start->placeInChunk(c, random, chunkBB);
        }
    }
}

std::vector<StructureSystem::StartReport> StructureSystem::startsIn(int cx, int cz) const {
    std::vector<StartReport> out;
    for (const auto& s : impl_->starts(cx, cz)) {
        StartReport r;
        r.id = s->structure->id;
        r.chunkX = s->chunkX, r.chunkZ = s->chunkZ;
        const BBox& b = s->box;
        const int bb[6] = {b.minX, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ};
        std::copy(bb, bb + 6, r.box);
        for (const auto& p : s->pieces) {
            PieceReport pr;
            pr.type = p->type;
            const int pb[6] = {p->box.minX, p->box.minY, p->box.minZ, p->box.maxX, p->box.maxY, p->box.maxZ};
            std::copy(pb, pb + 6, pr.box);
            if (const auto* pp = dynamic_cast<const PoolPiece*>(p.get())) pr.detail = pp->element->describe() + " " + rotName(pp->rot);
            r.pieces.push_back(std::move(pr));
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<std::string> StructureSystem::referencesOf(int cx, int cz) const {
    std::vector<std::string> out;
    for (const auto& s : impl_->startsFor(cx, cz, nullptr, false))
        out.push_back(s->structure->id + "@" + std::to_string(s->chunkX) + "," + std::to_string(s->chunkZ));
    return out;
}

}  // namespace wg
