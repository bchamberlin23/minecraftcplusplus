// Port of the game's Aquifer: NoiseBasedAquifer (overworld-like settings) and createDisabled (no "aquifers").
// The global fluid picker is NoiseBasedChunkGenerator.createFluidPicker.
#include "worldgen/aquifer.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace wg {

namespace {

constexpr int kWayBelowMinY = -2032 * 16;  // DimensionType.WAY_BELOW_MIN_Y (MIN_Y << 4)

// Aquifer cells are 16 x 12 x 16 blocks; the sample point of each cell is randomised within 10 x 9 x 10.
inline int gridXZ(int block) { return block >> 4; }
inline int gridY(int block) { return floorDiv(block, 12); }
inline int fromGridXZ(int grid, int offset) { return grid * 16 + offset; }
inline int fromGridY(int grid, int offset) { return grid * 12 + offset; }

inline double similarity(int distanceSqr1, int distanceSqr2) { return 1.0 - double(distanceSqr2 - distanceSqr1) / 25.0; }
const double kFlowingUpdateSimilarity = similarity(10 * 10, 12 * 12);

// SURFACE_SAMPLING_OFFSETS_IN_CHUNKS; the first entry is the cell's own column.
constexpr int kSurfaceOffsets[13][2] = {{0, 0},  {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {-3, 0}, {-2, 0},
                                        {-1, 0}, {1, 0},   {-2, 1},  {-1, 1}, {0, 1},  {1, 1}};

struct FluidStatus {
    int level = 0;
    u16 type = 0;
    bool operator==(const FluidStatus&) const = default;
};

}  // namespace

struct AquiferSampler::Impl {
    struct Cell {
        int x = 0, y = 0, z = 0;  // randomised sample position (aquiferLocationCache)
        bool located = false;
        bool hasStatus = false;
        FluidStatus status;  // aquiferCache
    };

    const NoiseState& noise;
    DensityContext& ctx;
    int seaLevel;
    u16 fluid, lava, air;  // fluid: the settings' default fluid
    bool fluidIsWater;
    bool enabled = false;
    bool scheduleFluidUpdate = false;

    DensityRef barrier, floodedness, spread, lavaNoise, exclusion, surface;
    std::unique_ptr<PositionalRandomFactory> random;
    int minGridX = 0, minGridY = 0, minGridZ = 0;
    int gridSizeX = 0, gridSizeY = 0, gridSizeZ = 0;
    int skipSamplingAboveY = 0;
    std::vector<Cell> cells;
    std::unordered_map<i64, int> surfaceCache;  // quart-aligned (x, z) -> floor(surface_level)

    Impl(const NoiseState& n, DensityContext& c, int sea, u16 f, u16 l, u16 a)
        : noise(n), ctx(c), seaLevel(sea), fluid(f), lava(l), air(a), fluidIsWater(f != l && f != a) {}

    bool isWater(u16 id) const { return fluidIsWater && id == fluid; }
    FluidStatus globalFluid(int y) const {
        return y < std::min(-54, seaLevel) ? FluidStatus{-54, lava} : FluidStatus{seaLevel, fluid};
    }
    u16 at(const FluidStatus& s, int y) const { return y < s.level ? s.type : air; }

    float sample(const DensityRef& f, int x, int y, int z) const { return noise.sample(f, ctx, x, y, z); }

    static i64 columnKey(int x, int z) { return i64(u64(u32(x)) | (u64(u32(z)) << 32)); }

    int surfaceLevel(int blockX, int blockZ) {
        const int qx = (blockX >> 2) * 4, qz = (blockZ >> 2) * 4;
        auto [it, inserted] = surfaceCache.try_emplace(columnKey(qx, qz), 0);
        if (inserted) it->second = jfloor(sample(surface, qx, 0, qz));
        return it->second;
    }

    // Samples the surface over the whole area at quart resolution, filling the cache; returns the maximum.
    int maxSurfaceLevel(int minBlockX, int minBlockZ, int maxBlockX, int maxBlockZ) {
        const int minQX = minBlockX >> 2, maxQX = maxBlockX >> 2, minQZ = minBlockZ >> 2, maxQZ = maxBlockZ >> 2;
        Volume v;
        v.sizeX = maxQX - minQX + 1, v.sizeY = 1, v.sizeZ = maxQZ - minQZ + 1;
        v.minX = minQX * 4, v.minY = 0, v.minZ = minQZ * 4;
        v.stepX = 4, v.stepY = 1, v.stepZ = 4;
        std::vector<float> buf(size_t(v.size()));
        noise.sampleVolume(surface, ctx, v, buf.data());
        int maxY = INT_MIN;
        for (int z = 0; z < v.sizeZ; ++z) {
            for (int x = 0; x < v.sizeX; ++x) {
                const int level = jfloor(buf[size_t(v.index(x, 0, z))]);
                surfaceCache[columnKey(v.blockX(x), v.blockZ(z))] = level;
                maxY = std::max(maxY, level);
            }
        }
        return maxY;
    }

    int cellIndex(int gx, int gy, int gz) const {
        const int x = gx - minGridX, y = gy - minGridY, z = gz - minGridZ;
        assert(x >= 0 && x < gridSizeX && y >= 0 && y < gridSizeY && z >= 0 && z < gridSizeZ);
        return (y * gridSizeZ + z) * gridSizeX + x;
    }

    Cell& locate(int index, int gx, int gy, int gz) {
        Cell& c = cells[size_t(index)];
        if (!c.located) {
            auto r = random->at(gx, gy, gz);
            const int ox = r->nextInt(10);
            const int oy = r->nextInt(9);
            const int oz = r->nextInt(10);
            c.x = fromGridXZ(gx, ox), c.y = fromGridY(gy, oy), c.z = fromGridXZ(gz, oz);
            c.located = true;
        }
        return c;
    }

    const FluidStatus& status(int index) {
        Cell& c = cells[size_t(index)];
        if (!c.hasStatus) {
            c.status = computeFluid(c.x, c.y, c.z);
            c.hasStatus = true;
        }
        return c.status;
    }

    FluidStatus computeFluid(int x, int y, int z) {
        const FluidStatus global = globalFluid(y);
        int lowestSurface = INT_MAX;
        const int top = y + 12, bottom = y - 12;
        bool centerUnderGlobalFluid = false;
        for (const auto& off : kSurfaceOffsets) {
            const int sx = x + off[0] * 16, sz = z + off[1] * 16;
            const int level = surfaceLevel(sx, sz);
            const int adjusted = level + 8;
            const bool start = off[0] == 0 && off[1] == 0;
            if (start && bottom > adjusted) return global;
            const bool pokesAbove = top > adjusted;
            if (pokesAbove || start) {
                const FluidStatus atSurface = globalFluid(adjusted);
                if (at(atSurface, adjusted) != air) {
                    if (start) centerUnderGlobalFluid = true;
                    if (pokesAbove) return atSurface;
                }
            }
            lowestSurface = std::min(lowestSurface, level);
        }
        const int fluidLevel = computeSurfaceLevel(x, y, z, global, lowestSurface, centerUnderGlobalFluid);
        return {fluidLevel, computeFluidType(x, y, z, global, fluidLevel)};
    }

    int computeSurfaceLevel(int x, int y, int z, const FluidStatus& global, int lowestSurface, bool centerUnder) {
        double partially, fully;
        if (double(sample(exclusion, x, y, z)) > 0.0) {
            partially = -1.0, fully = -1.0;
        } else {
            const int distanceBelowSurface = lowestSurface + 8 - y;
            const double factor = centerUnder ? clampedMap(double(distanceBelowSurface), 0.0, 64.0, 1.0, 0.0) : 0.0;
            const double value = clampd(double(sample(floodedness, x, y, z)), -1.0, 1.0);
            const double fullyThreshold = map(factor, 1.0, 0.0, -0.3, 0.8);
            const double partiallyThreshold = map(factor, 1.0, 0.0, -0.8, 0.4);
            partially = value - partiallyThreshold;
            fully = value - fullyThreshold;
        }
        if (fully > 0.0) return global.level;
        if (partially > 0.0) return randomizedSurfaceLevel(x, y, z, lowestSurface);
        return kWayBelowMinY;
    }

    int randomizedSurfaceLevel(int x, int y, int z, int lowestSurface) {
        const int cx = floorDiv(x, 16), cy = floorDiv(y, 40), cz = floorDiv(z, 16);
        const int middleY = cy * 40 + 20;
        const float spreadValue = sample(spread, cx, cy, cz) * 10.0f;  // float multiply, then widened
        const int quantized = jfloor(double(spreadValue) / 3.0) * 3;   // Mth.quantize(value, 3)
        return std::min(lowestSurface, middleY + quantized);
    }

    u16 computeFluidType(int x, int y, int z, const FluidStatus& global, int fluidLevel) {
        if (fluidLevel <= -10 && fluidLevel != kWayBelowMinY && global.type != lava) {
            const double v = sample(lavaNoise, floorDiv(x, 64), floorDiv(y, 40), floorDiv(z, 64));
            if (std::abs(v) > 0.3) return lava;
        }
        return global.type;
    }

    // barrierValue memoises the barrier noise for one computeSubstance call (NaN = not sampled yet).
    double pressure(int x, int y, int z, double& barrierValue, const FluidStatus& s1, const FluidStatus& s2) {
        const u16 t1 = at(s1, y), t2 = at(s2, y);
        if ((t1 == lava && isWater(t2)) || (isWater(t1) && t2 == lava)) return 2.0;
        const int fluidYDiff = std::abs(s1.level - s2.level);
        if (fluidYDiff == 0) return 0.0;
        const double averageFluidY = 0.5 * double(s1.level + s2.level);
        const double aboveAverage = double(y) + 0.5 - averageFluidY;
        const double baseValue = double(fluidYDiff) / 2.0;
        const double towardsMiddle = baseValue - std::abs(aboveAverage);
        double gradient;
        if (aboveAverage > 0.0) {
            const double center = 0.0 + towardsMiddle;
            gradient = center > 0.0 ? center / 1.5 : center / 2.5;
        } else {
            const double center = 3.0 + towardsMiddle;
            gradient = center > 0.0 ? center / 3.0 : center / 10.0;
        }
        double noiseValue = 0.0;
        if (!(gradient < -2.0) && !(gradient > 2.0)) {
            if (std::isnan(barrierValue)) barrierValue = double(sample(barrier, x, y, z));
            noiseValue = barrierValue;
        }
        return 2.0 * (noiseValue + gradient);
    }

    u16 computeSubstance(int x, int y, int z, double density) {
        if (density > 0.0) {
            scheduleFluidUpdate = false;
            return kDefaultBlock;
        }
        const FluidStatus global = globalFluid(y);
        if (!enabled || y > skipSamplingAboveY) {
            scheduleFluidUpdate = false;
            return at(global, y);
        }
        if (at(global, y) == lava) {
            scheduleFluidUpdate = false;
            return lava;
        }

        // The four nearest cell sample points among the 2 x 3 x 2 surrounding cells (ties favour later cells).
        const int xAnchor = gridXZ(x - 5), yAnchor = gridY(y + 1), zAnchor = gridXZ(z - 5);
        int d1 = INT_MAX, d2 = INT_MAX, d3 = INT_MAX, d4 = INT_MAX;
        int i1 = 0, i2 = 0, i3 = 0, i4 = 0;
        for (int x1 = 0; x1 <= 1; ++x1) {
            for (int y1 = -1; y1 <= 1; ++y1) {
                for (int z1 = 0; z1 <= 1; ++z1) {
                    const int gx = xAnchor + x1, gy = yAnchor + y1, gz = zAnchor + z1;
                    const int index = cellIndex(gx, gy, gz);
                    const Cell& c = locate(index, gx, gy, gz);
                    const int dx = c.x - x, dy = c.y - y, dz = c.z - z;
                    const int d = dx * dx + dy * dy + dz * dz;
                    if (d1 >= d) {
                        i4 = i3, i3 = i2, i2 = i1, i1 = index;
                        d4 = d3, d3 = d2, d2 = d1, d1 = d;
                    } else if (d2 >= d) {
                        i4 = i3, i3 = i2, i2 = index;
                        d4 = d3, d3 = d2, d2 = d;
                    } else if (d3 >= d) {
                        i4 = i3, i3 = index;
                        d4 = d3, d3 = d;
                    } else if (d4 >= d) {
                        i4 = index;
                        d4 = d;
                    }
                }
            }
        }

        const FluidStatus s1 = status(i1);
        const double similarity12 = similarity(d1, d2);
        const u16 fluidState = at(s1, y);
        if (similarity12 <= 0.0) {
            scheduleFluidUpdate = similarity12 >= kFlowingUpdateSimilarity && !(s1 == status(i2));
            return fluidState;
        }
        if (isWater(fluidState) && at(globalFluid(y - 1), y - 1) == lava) {
            scheduleFluidUpdate = true;
            return fluidState;
        }

        double barrierValue = std::nan("");
        const FluidStatus s2 = status(i2);
        const double barrier12 = similarity12 * pressure(x, y, z, barrierValue, s1, s2);
        if (density + barrier12 > 0.0) {
            scheduleFluidUpdate = false;
            return kDefaultBlock;
        }
        const FluidStatus s3 = status(i3);
        const double similarity13 = similarity(d1, d3);
        if (similarity13 > 0.0) {
            const double barrier13 = similarity12 * similarity13 * pressure(x, y, z, barrierValue, s1, s3);
            if (density + barrier13 > 0.0) {
                scheduleFluidUpdate = false;
                return kDefaultBlock;
            }
        }
        const double similarity23 = similarity(d2, d3);
        if (similarity23 > 0.0) {
            const double barrier23 = similarity12 * similarity23 * pressure(x, y, z, barrierValue, s2, s3);
            if (density + barrier23 > 0.0) {
                scheduleFluidUpdate = false;
                return kDefaultBlock;
            }
        }

        const bool mayFlow12 = !(s1 == s2);
        const bool mayFlow23 = similarity23 >= kFlowingUpdateSimilarity && !(s2 == s3);
        const bool mayFlow13 = similarity13 >= kFlowingUpdateSimilarity && !(s1 == s3);
        if (mayFlow12 || mayFlow23 || mayFlow13) {
            scheduleFluidUpdate = true;
        } else {
            scheduleFluidUpdate = similarity13 >= kFlowingUpdateSimilarity &&
                                  similarity(d1, d4) >= kFlowingUpdateSimilarity && !(s1 == status(i4));
        }
        return fluidState;
    }
};

// water: the noise settings' default_fluid state (water in the overworld, lava in the nether, air in the end).
AquiferSampler::AquiferSampler(const NoiseState& noise, DensityContext& ctx, const nlohmann::json* config,
                               const Volume& volume, int seaLevel, u16 water, u16 lava, u16 air)
    : impl_(std::make_unique<Impl>(noise, ctx, seaLevel, water, lava, air)) {
    if (!config || config->is_null()) return;
    Impl& m = *impl_;
    m.enabled = true;
    m.barrier = noise.parse(config->at("barrier"));
    m.floodedness = noise.parse(config->at("fluid_level_floodedness"));
    m.spread = noise.parse(config->at("fluid_level_spread"));
    m.lavaNoise = noise.parse(config->at("lava"));
    m.exclusion = noise.parse(config->at("exclusion"));
    m.surface = noise.parse(config->at("surface_level"));
    m.random = noise.randomFactory("minecraft:aquifer");

    const int maxBlockX = volume.minX + volume.sizeX * volume.stepX - 1;
    const int maxBlockY = volume.minY + volume.sizeY * volume.stepY - 1;
    const int maxBlockZ = volume.minZ + volume.sizeZ * volume.stepZ - 1;
    m.minGridX = gridXZ(volume.minX - 5);
    const int maxGridX = gridXZ(maxBlockX - 5) + 1;
    m.gridSizeX = maxGridX - m.minGridX + 1;
    m.minGridY = gridY(volume.minY + 1) - 1;
    const int maxGridY = gridY(maxBlockY + 1) + 1;
    m.gridSizeY = maxGridY - m.minGridY + 1;
    m.minGridZ = gridXZ(volume.minZ - 5);
    const int maxGridZ = gridXZ(maxBlockZ - 5) + 1;
    m.gridSizeZ = maxGridZ - m.minGridZ + 1;
    m.cells.resize(size_t(m.gridSizeX) * m.gridSizeY * m.gridSizeZ);

    const int maxAdjustedSurface = m.maxSurfaceLevel(fromGridXZ(m.minGridX, 0), fromGridXZ(m.minGridZ, 0),
                                                     fromGridXZ(maxGridX, 9), fromGridXZ(maxGridZ, 9)) + 8;
    const int skipSamplingAboveGridY = gridY(maxAdjustedSurface + 12) + 1;
    m.skipSamplingAboveY = fromGridY(skipSamplingAboveGridY, 11) - 1;
}

AquiferSampler::~AquiferSampler() = default;

u16 AquiferSampler::computeSubstance(int x, int y, int z, float density) {
    return impl_->computeSubstance(x, y, z, double(density));
}

bool AquiferSampler::shouldScheduleFluidUpdate() const { return impl_->scheduleFluidUpdate; }

}  // namespace wg
