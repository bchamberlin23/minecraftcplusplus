#include "worldgen/generator.h"

#include "assets/assets.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <unordered_map>

namespace wg {

namespace {
// Fluids and their canonical state keys (the game's default fluid states).
std::string fluidKey(std::string key) {
    if (key == "minecraft:water" || key == "minecraft:lava") key += "[level=0]";
    return key;
}
}  // namespace

ChunkGenerator::ChunkGenerator(const DataPack& data, BlockRegistry& blocks, i64 seed, std::string_view settings)
    : blocks_(blocks) {
    const nlohmann::json* s = data.worldgen("noise_settings", settings);
    if (!s || !s->contains("noise_router") || !s->contains("noise")) {
        LOG("error: noise settings '%.*s' missing from the game data", int(settings.size()), settings.data());
        return;
    }
    const nlohmann::json& ns = *s;
    minY_ = ns["noise"].value("min_y", -64);
    height_ = ns["noise"].value("height", 384);
    seaLevel_ = ns.value("sea_level", 63);
    noise_ = std::make_unique<NoiseState>(data, seed, ns.value("legacy_random_source", false));
    finalDensity_ = noise_->parse(ns["noise_router"]["final_density"]);
    biomeSource_ = std::make_unique<OverworldBiomeSource>(*noise_, ns["noise_router"], biomes_);
    materials_ = std::make_unique<MaterialSystem>(*noise_, blocks, biomes_, ns);
    carvers_ = std::make_unique<Carvers>(*noise_, biomes_);
    zoom_ = std::make_unique<BiomeZoom>(seed);
    if (ns.contains("aquifers") && !ns["aquifers"].is_null()) {
        aquifers_ = ns["aquifers"];
        hasAquifers_ = true;
    }
    defaultBlock_ = blocks.idFor(fluidKey(blockStateKey(ns.value("default_block", nlohmann::json("minecraft:stone")))));
    // The aquifer's global fluid is the dimension's default fluid (water here, lava in the nether).
    water_ = blocks.idFor(fluidKey(blockStateKey(ns.value("default_fluid", nlohmann::json("minecraft:water")))));
    lava_ = blocks.idFor("minecraft:lava[level=0]");
    grass_ = blocks.idFor("minecraft:grass_block[snowy=false]");
    mycelium_ = blocks.idFor("minecraft:mycelium[snowy=false]");
    dirt_ = blocks.idFor("minecraft:dirt");
    uncarvableNames_ = data.tag("block", "uncarvable");
    uncarvableCache_ = std::make_unique<std::atomic<i8>[]>(0x10000);
    for (int i = 0; i < 0x10000; ++i) uncarvableCache_[i] = -1;
    ok_ = finalDensity_ != nullptr;
}

ChunkGenerator::~ChunkGenerator() = default;

bool ChunkGenerator::uncarvable(u16 id) const {
    i8 v = uncarvableCache_[id].load(std::memory_order_relaxed);
    if (v < 0) {
        const std::string& key = blocks_.name(id);
        const std::string base = key.substr(0, key.find('['));
        v = std::find(uncarvableNames_.begin(), uncarvableNames_.end(), base) != uncarvableNames_.end();
        uncarvableCache_[id].store(v, std::memory_order_relaxed);
    }
    return v == 1;
}

// NoiseBasedChunkGenerator.doFill: final density over the column, top to bottom, through the aquifer.
void ChunkGenerator::fill(GenChunk& chunk, DensityContext& ctx, AquiferSampler& aquifer, const DensityAddition* beardifier) const {
    const Volume v{16, height_, 16, chunk.minBlockX(), minY_, chunk.minBlockZ()};
    thread_local std::vector<float> density;
    density.resize(size_t(v.size()));
    noise_->sampleVolume(finalDensity_, ctx, v, density.data());
    if (beardifier) beardifier->add(v, density.data());  // final_density's "minecraft:beardifier" term
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            for (int y = height_ - 1; y >= 0; --y) {
                const int by = minY_ + y;
                u16 state = aquifer.computeSubstance(v.blockX(x), by, v.blockZ(z), density[size_t(v.index(x, y, z))]);
                if (state == AquiferSampler::kDefaultBlock) state = defaultBlock_;
                if (state != GenChunk::kAir) chunk.set(x, by, z, state);
            }
}

// NoiseBasedChunkGenerator.applyCarvingMask.
void ChunkGenerator::applyCarvingMask(GenChunk& chunk, DensityContext& ctx, const CarvingMask& mask,
                                      AquiferSampler& aquifer, const NoiseBiomeFn& noiseBiome) const {
    mask.visit([&](int x, int z, int bottomY, int topY) {
        bool hasGrass = false;
        const int worldX = chunk.minBlockX() + x, worldZ = chunk.minBlockZ() + z;
        for (int y = topY; y >= bottomY; --y) {
            const u16 current = chunk.get(x, y, z);
            if (uncarvable(current)) continue;
            if (current == grass_ || current == mycelium_ || blocks_.name(current).starts_with("minecraft:grass_block") ||
                blocks_.name(current).starts_with("minecraft:mycelium"))
                hasGrass = true;
            const u16 state = aquifer.computeSubstance(worldX, y, worldZ, 0.0f);
            if (state == AquiferSampler::kDefaultBlock) continue;
            chunk.set(x, y, z, state);
            if (hasGrass && chunk.get(x, y - 1, z) == dirt_) {
                const bool fluid = blocks_.infos()[state].fluid;
                if (auto top = materials_->topMaterial(chunk, ctx, *zoom_, noiseBiome, x, y - 1, z, fluid))
                    chunk.set(x, y - 1, z, *top);
            }
        }
    });
}

void ChunkGenerator::updateHeightmaps(GenChunk& chunk) const {
    const BlockInfo* infos = blocks_.infos();
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            int surface = chunk.minY, floor = chunk.minY;
            for (int y = chunk.minY + chunk.height - 1; y >= chunk.minY; --y) {
                const u16 id = chunk.get(x, y, z);
                if (id == GenChunk::kAir) continue;
                if (surface == chunk.minY) surface = y + 1;
                const BlockInfo& b = infos[id];
                if (!b.fluid && b.collisionCount > 0) {
                    floor = y + 1;
                    break;
                }
            }
            chunk.worldSurfaceWG[z * 16 + x] = surface;
            chunk.oceanFloorWG[z * 16 + x] = floor;
        }
}

// Far terrain for one region at LOD `level`: columns 2^level blocks wide (sampled at their centre where that is
// a corner of the density's 4x8x4 interpolation) with exact block heights: the final density is sampled at the
// 8-block interpolation corners and interpolated linearly in between, sea-level water stands in for aquifers,
// carvers and features are left out, and the real material rules colour the top of every solid run. Cavities
// deep below a column's surface are filled (never seen from afar). With `coarser` (this region at level + 1)
// only a band around the neighbouring surfaces is sampled; above and below it the coarser columns carry over.
namespace {

// Far terrain has no features: forests get a canopy instead, from blobs of value noise at about the size of a tree
// crown, with each biome's leaves, height and density. Real chunks (with their real trees) take over up close.
struct Canopy {
    float cover = 0;  // fraction of the ground under leaves
    int trunk = 4, thickness = 3;
    u16 leaves = 0;
};

Canopy canopyFor(std::string_view biome, BlockRegistry& blocks) {
    struct Rule {
        const char* match;
        float cover;
        int trunk, thickness;
        const char* leaves;
    };
    static constexpr Rule kRules[] = {
        {"pale_garden", 0.85f, 5, 4, "pale_oak_leaves"}, {"dark_forest", 0.9f, 5, 4, "dark_oak_leaves"},
        {"bamboo_jungle", 0.5f, 7, 4, "jungle_leaves"},  {"sparse_jungle", 0.3f, 6, 3, "jungle_leaves"},
        {"jungle", 0.85f, 9, 5, "jungle_leaves"},        {"mangrove", 0.7f, 6, 4, "mangrove_leaves"},
        {"cherry", 0.45f, 5, 4, "cherry_leaves"},        {"birch", 0.6f, 5, 3, "birch_leaves"},
        {"old_growth_pine", 0.6f, 12, 8, "spruce_leaves"}, {"old_growth_spruce", 0.65f, 12, 9, "spruce_leaves"},
        {"snowy_taiga", 0.5f, 5, 6, "spruce_leaves"},    {"taiga", 0.6f, 5, 6, "spruce_leaves"},
        {"grove", 0.35f, 5, 6, "spruce_leaves"},         {"windswept_forest", 0.4f, 5, 5, "spruce_leaves"},
        {"flower_forest", 0.4f, 4, 3, "oak_leaves"},     {"forest", 0.6f, 4, 3, "oak_leaves"},
        {"wooded_badlands", 0.25f, 4, 3, "oak_leaves"},  {"swamp", 0.35f, 4, 3, "oak_leaves"},
        {"savanna", 0.08f, 5, 2, "acacia_leaves"},       {"meadow", 0.03f, 4, 3, "birch_leaves"},
        {"plains", 0.02f, 4, 3, "oak_leaves"},           {"river", 0.0f, 4, 3, "oak_leaves"},
    };
    for (const Rule& r : kRules)
        if (biome.find(r.match) != std::string_view::npos)
            return {r.cover, r.trunk, r.thickness, blocks.idFor(std::string("minecraft:") + r.leaves)};
    return {};
}

float valueNoise(i64 seed, int x, int z, int scale) {
    auto hash = [&](int i, int j) {
        u64 h = u64(seed) ^ (u64(u32(i)) * 0x9E3779B97F4A7C15ull) ^ (u64(u32(j)) * 0xC2B2AE3D27D4EB4Full);
        h ^= h >> 31, h *= 0xBF58476D1CE4E5B9ull, h ^= h >> 29;
        return float(h >> 40) / float(1 << 24);
    };
    const int i = x >= 0 ? x / scale : (x + 1) / scale - 1, j = z >= 0 ? z / scale : (z + 1) / scale - 1;
    const float fx = float(x - i * scale) / float(scale), fz = float(z - j * scale) / float(scale);
    const float a = hash(i, j) + (hash(i + 1, j) - hash(i, j)) * fx;
    const float b = hash(i, j + 1) + (hash(i + 1, j + 1) - hash(i, j + 1)) * fx;
    return a + (b - a) * fz;
}

}  // namespace

void ChunkGenerator::generateCoarse(int rx, int rz, int level, FarColumns& out, DensityContext& ctx,
                                    const FarColumns* coarser, double* phaseMs, std::vector<u16>* columnBiomes) const {
    Timer phase;
    const int s = 1 << level, n = 512 >> level;
    const int off = s >= 8 ? s / 2 : 0;  // a multiple of 4: an interpolation corner horizontally
    const int x0 = rx * 512 + off, z0 = rz * 512 + off;
    const int corners = height_ / 8 + 1;
    enum Kind : u8 { Air, Water, Solid };
    struct KindRun {
        i16 y;  // relative to minY_
        u16 count;
        u8 kind;
    };
    std::vector<KindRun> kinds;
    std::vector<u32> kindStart(size_t(n) * n + 1);
    std::vector<int> top(size_t(n) * n, 0);      // one above the highest solid block (rows from minY_)
    std::vector<int> surface(size_t(n) * n, 0);  // the same including water (the game's WORLD_SURFACE_WG)
    std::vector<u8> rows(static_cast<size_t>(height_));
    std::vector<float> density(static_cast<size_t>(corners));

    const bool band = coarser && coarser->level == level + 1 && coarser->n == n / 2;
    std::vector<int> coarseTop;
    if (band) {
        coarseTop.assign(size_t(coarser->n) * coarser->n, 0);
        for (int i = 0; i < coarser->n * coarser->n; ++i)
            for (u32 r = coarser->start[size_t(i)]; r < coarser->start[size_t(i) + 1]; ++r) {
                const FarColumns::Run& run = coarser->runs[r];
                if (run.block != water_) coarseTop[size_t(i)] = std::max(coarseTop[size_t(i)], run.y + run.count - minY_);
            }
    }
    int sampledColumns = 0;
    auto bandAround = [&](int cx, int cz, int& lo, int& hi) {  // rows worth sampling near coarser column (cx, cz)
        const int cn = coarser->n;
        int mn = INT_MAX, mx = 0;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const int t = coarseTop[size_t(std::clamp(cz + dz, 0, cn - 1)) * cn + std::clamp(cx + dx, 0, cn - 1)];
                mn = std::min(mn, t), mx = std::max(mx, t);
            }
        lo = std::clamp(mn - 16, 0, height_), hi = std::clamp(std::max(mx, seaLevel_ - minY_) + 16, 0, height_);
    };
    // Level 1 (2-block columns): the game blends density bilinearly between 4-block interpolation corners, so
    // sample those corner columns once and blend them (odd columns are not corners and would be slow to sample).
    const bool blend = level == 1;
    const int gn = blend ? n / 2 + 1 : 0;
    std::vector<float> cornerDensity;
    std::vector<int> cornerLo, cornerHi;  // rows each corner column can interpolate
    if (blend) {
        cornerDensity.assign(size_t(gn) * gn * corners, 0.0f);
        cornerLo.assign(size_t(gn) * gn, 0), cornerHi.assign(size_t(gn) * gn, 0);
        for (int cz = 0; cz < gn; ++cz)
            for (int cx = 0; cx < gn; ++cx) {
                int lo = 0, hi = height_;
                if (band) bandAround(std::min(cx, coarser->n - 1), std::min(cz, coarser->n - 1), lo, hi);
                if (hi <= lo) continue;
                const int c0 = lo / 8, c1 = std::min(corners - 1, (hi - 1) / 8 + 1);
                const Volume v{1, c1 - c0 + 1, 1, rx * 512 + cx * 4, minY_ + c0 * 8, rz * 512 + cz * 4, 1, 8, 1};
                if ((++sampledColumns & 255) == 0) ctx.clear();
                const size_t c = size_t(cz) * gn + cx;
                noise_->sampleVolume(finalDensity_, ctx, v, &cornerDensity[c * corners + size_t(c0)]);
                cornerLo[c] = c0 * 8, cornerHi[c] = std::min(height_, c1 * 8 + 1);
            }
    }
    auto cornerAt = [&](size_t c, int y) {
        const float* d = &cornerDensity[c * corners];
        const int k = y / 8, k1 = std::min(k + 1, corners - 1);
        return d[k] + (d[k1] - d[k]) * float(y & 7) / 8.0f;
    };
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const size_t col = size_t(z) * n + x;
            const int bx = x0 + x * s, bz = z0 + z * s;
            int lo = 0, hi = height_;
            if (band) {
                const int cx = x >> 1, cz = z >> 1, cn = coarser->n;
                int mn = INT_MAX, mx = 0;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int t = coarseTop[size_t(std::clamp(cz + dz, 0, cn - 1)) * cn + std::clamp(cx + dx, 0, cn - 1)];
                        mn = std::min(mn, t), mx = std::max(mx, t);
                    }
                lo = std::clamp(mn - 16, 0, height_), hi = std::clamp(std::max(mx, seaLevel_ - minY_) + 16, 0, height_);
                if (blend) {  // only where all four corner columns were sampled
                    const size_t c00 = size_t(z >> 1) * gn + (x >> 1);
                    for (size_t c : {c00, c00 + (x & 1), c00 + size_t(z & 1) * gn, c00 + size_t(z & 1) * gn + (x & 1)})
                        lo = std::max(lo, cornerLo[c]), hi = std::min(hi, cornerHi[c]);
                }
                std::fill(rows.begin(), rows.end(), u8(Air));
                auto [b, e] = coarser->column(cx, cz);
                for (const FarColumns::Run* r = b; r != e; ++r)
                    for (int y = r->y - minY_; y < r->y - minY_ + int(r->count); ++y)
                        if (y >= 0 && y < height_ && (y < lo || y >= hi)) rows[size_t(y)] = r->block == water_ ? Water : Solid;
            }
            if (blend) {
                const size_t c00 = size_t(z >> 1) * gn + (x >> 1), c10 = c00 + (x & 1), c01 = c00 + size_t(z & 1) * gn,
                             c11 = c01 + (x & 1);
                for (size_t c : {c00, c10, c01, c11}) lo = std::max(lo, cornerLo[c]), hi = std::min(hi, cornerHi[c]);
                const float fx = (x & 1) * 0.5f, fz = (z & 1) * 0.5f;
                for (int y = lo; y < hi; ++y) {
                    const float a = cornerAt(c00, y) + (cornerAt(c10, y) - cornerAt(c00, y)) * fx;
                    const float b = cornerAt(c01, y) + (cornerAt(c11, y) - cornerAt(c01, y)) * fx;
                    const float d = a + (b - a) * fz;
                    rows[size_t(y)] = d > 0.0f ? Solid : minY_ + y < seaLevel_ ? Water : Air;
                }
            } else if (hi > lo) {
                const int c0 = lo / 8, c1 = std::min(corners - 1, (hi - 1) / 8 + 1);
                const Volume v{1, c1 - c0 + 1, 1, bx, minY_ + c0 * 8, bz, 1, 8, 1};
                if ((++sampledColumns & 255) == 0) ctx.clear();
                noise_->sampleVolume(finalDensity_, ctx, v, density.data());
                for (int y = lo; y < hi; ++y) {
                    const int k = y / 8 - c0, k1 = std::min(k + 1, c1 - c0);
                    const float d = density[size_t(k)] + (density[size_t(k1)] - density[size_t(k)]) * float(y & 7) / 8.0f;
                    rows[size_t(y)] = d > 0.0f ? Solid : minY_ + y < seaLevel_ ? Water : Air;
                }
            }
            int t = 0, w = 0;
            for (int y = height_ - 1; y >= 0 && !t; --y) {
                if (!w && rows[size_t(y)] != Air) w = y + 1;
                if (rows[size_t(y)] == Solid) t = y + 1;
            }
            top[col] = t, surface[col] = std::max(t, w);
            for (int y = 0; y < t - 32; ++y) rows[size_t(y)] = Solid;  // deep cavities are never seen
            kindStart[col] = u32(kinds.size());
            for (int y = 0; y < height_;) {
                int e = y;
                while (e < height_ && rows[size_t(e)] == rows[size_t(y)]) ++e;
                if (rows[size_t(y)] != Air) kinds.push_back({i16(y), u16(e - y), rows[size_t(y)]});
                y = e;
            }
        }
    kindStart[size_t(n) * n] = u32(kinds.size());
    if (phaseMs) phaseMs[0] += phase.ms(), phase = Timer();

    // Biome lookups repeat across a column's blocks: one biome per quart column, at its surface.
    std::unordered_map<u64, u16> biomeCache;
    int surfaceQuart = 0;
    const NoiseBiomeFn noiseBiome = [&](int qx, int, int qz) {
        auto [it, inserted] = biomeCache.try_emplace((u64(u32(qx)) << 32) | u32(qz), u16(0));
        if (inserted) it->second = biomeSource_->noiseBiome(ctx, qx, surfaceQuart, qz);
        return it->second;
    };
    auto surfaceAt = [&](int x, int z) { return minY_ + surface[size_t(std::clamp(z, 0, n - 1)) * n + std::clamp(x, 0, n - 1)]; };
    out.level = level, out.n = n;
    out.start.assign(size_t(n) * n + 1, 0);
    out.runs.clear();
    if (columnBiomes) columnBiomes->assign(size_t(n) * n, 0);
    size_t colFirst = 0;  // runs merge only within a column
    auto push = [&](int y, int count, u16 block) {
        if (count <= 0) return;
        if (out.runs.size() > colFirst && out.runs.back().block == block && out.runs.back().y + out.runs.back().count == y)
            out.runs.back().count = u16(out.runs.back().count + count);
        else
            out.runs.push_back({i16(y), u16(count), block});
    };
    GenChunk column;  // only the position and the WG surface heightmap are read by the rules
    column.minY = minY_, column.height = height_;
    thread_local std::vector<Canopy> canopies;  // by generator biome id
    thread_local std::vector<u8> canopyKnown;
    const i64 canopySeed = noise_->seed() * 31 + 7;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const size_t col = size_t(z) * n + x;
            const int bx = x0 + x * s, bz = z0 + z * s;
            column.cx = bx >> 4, column.cz = bz >> 4;
            const float slopeX = float(surfaceAt(x + 1, z) - surfaceAt(x - 1, z)) / float(2 * s);
            const float slopeZ = float(surfaceAt(x, z + 1) - surfaceAt(x, z - 1)) / float(2 * s);
            const int h = surfaceAt(x, z), lx = bx & 15, lz = bz & 15;
            for (int j = 0; j < 16; ++j)
                for (int i = 0; i < 16; ++i)
                    column.worldSurfaceWG[j * 16 + i] = h + int(std::lround(slopeX * float(i - lx) + slopeZ * float(j - lz)));
            surfaceQuart = (std::max(h, seaLevel_) - 1) >> 2;
            if (columnBiomes) (*columnBiomes)[col] = noiseBiome(bx >> 2, surfaceQuart, bz >> 2);
            out.start[col] = u32(out.runs.size());
            colFirst = out.runs.size();
            for (u32 k = kindStart[col]; k < kindStart[col + 1]; ++k) {
                const KindRun& r = kinds[k];
                const int y = minY_ + r.y;
                if (r.kind == Water) {
                    push(y, r.count, water_);
                    continue;
                }
                // Solid: stone below, the material rules for the top blocks (grass, dirt, sand, snow, terracotta...).
                const int depth = std::min<int>(4, r.count);
                push(y, r.count - depth, defaultBlock_);
                const bool waterAbove = k + 1 < kindStart[col + 1] && kinds[k + 1].kind == Water && kinds[k + 1].y == r.y + r.count;
                for (int i = depth - 1; i >= 0; --i) {
                    const int by = y + r.count - 1 - i;
                    const auto m = materials_->materialAt(column, ctx, *zoom_, noiseBiome, bx, by, bz, i + 1, by - y + 1,
                                                          waterAbove ? seaLevel_ : INT_MIN);
                    push(by, 1, m && *m ? *m : defaultBlock_);
                }
            }
            // Forest canopy on dry ground (see Canopy).
            const u16 biome = noiseBiome(bx >> 2, surfaceQuart, bz >> 2);
            if (biome >= canopies.size()) canopies.resize(size_t(biome) + 1), canopyKnown.resize(size_t(biome) + 1, 0);
            if (!canopyKnown[biome]) canopies[biome] = canopyFor(biomes_.name(biome), blocks_), canopyKnown[biome] = 1;
            const Canopy& c = canopies[biome];
            if (c.cover > 0 && top[col] > 0 && top[col] == surface[col] && minY_ + top[col] > seaLevel_ &&
                valueNoise(canopySeed, bx, bz, 6) < c.cover) {
                const int jitter = int(valueNoise(canopySeed + 1, bx, bz, 3) * 3.0f) - 1;
                push(minY_ + top[col] + c.trunk + jitter, c.thickness, c.leaves);
            }
        }
    out.start[size_t(n) * n] = u32(out.runs.size());
    if (phaseMs) phaseMs[1] += phase.ms();
}

void ChunkGenerator::baseColumn(int x, int z, std::vector<u16>& out, DensityContext& ctx) const {
    const Volume v{1, height_, 1, x, minY_, z};
    thread_local std::vector<float> density;
    density.resize(size_t(height_));
    ctx.clear();
    noise_->sampleVolume(finalDensity_, ctx, v, density.data());
    AquiferSampler aquifer(*noise_, ctx, hasAquifers_ ? &aquifers_ : nullptr, v, seaLevel_, water_, lava_, GenChunk::kAir);
    out.assign(size_t(height_), GenChunk::kAir);
    for (int y = height_ - 1; y >= 0; --y) {
        u16 state = aquifer.computeSubstance(x, minY_ + y, z, density[size_t(y)]);
        if (state == AquiferSampler::kDefaultBlock) state = defaultBlock_;
        out[size_t(y)] = state;
    }
}

void ChunkGenerator::generate(int cx, int cz, GenChunk& chunk, DensityContext& ctx, GenTimings* timings,
                              const DensityAddition* beardifier) const {
    Timer t;
    chunk.init(cx, cz, minY_, height_);
    ctx.clear();
    biomeSource_->fillBiomes(chunk, ctx);
    if (timings) timings->biomes += t.ms();

    t = Timer();
    const Volume volume{16, height_, 16, chunk.minBlockX(), minY_, chunk.minBlockZ()};
    AquiferSampler aquifer(*noise_, ctx, hasAquifers_ ? &aquifers_ : nullptr, volume, seaLevel_, water_, lava_,
                           GenChunk::kAir);
    fill(chunk, ctx, aquifer, beardifier);
    updateHeightmaps(chunk);
    if (timings) timings->fill += t.ms();

    const NoiseBiomeFn noiseBiome = [&](int qx, int qy, int qz) { return biomeSource_->noiseBiome(ctx, qx, qy, qz); };
    t = Timer();
    materials_->buildSurface(chunk, ctx, *zoom_, noiseBiome);
    if (timings) timings->surface += t.ms();

    t = Timer();
    CarvingMask mask;
    mask.init(minY_ + 1, minY_ + height_ - 1 - 7);  // seven protected blocks at the top, like the game
    carvers_->carve(cx, cz, noiseBiome, mask, minY_, height_);
    if (!mask.empty()) applyCarvingMask(chunk, ctx, mask, aquifer, noiseBiome);
    updateHeightmaps(chunk);
    if (timings) timings->carve += t.ms();
}

}  // namespace wg
