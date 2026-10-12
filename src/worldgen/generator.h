#pragma once
// Terrain generation for one chunk column, equivalent to the game's chunk statuses up to
// "minecraft:terrain": biomes, noise fill with aquifers, surface materials and carvers. One generator per
// dimension, of the dimension's own kind: noise (any noise settings and biome source), flat or debug.
#include "worldgen/aquifer.h"
#include "worldgen/carvers.h"
#include "worldgen/climate.h"
#include "worldgen/density.h"
#include "worldgen/dimensions.h"
#include "worldgen/gen_chunk.h"
#include "worldgen/materials.h"
#include "world/far_columns.h"

#include <atomic>

class BlockRegistry;

namespace wg {

struct GenTimings {
    double biomes = 0, fill = 0, surface = 0, carve = 0;
};

// Extra final density per block, added after the noise router's final density (the game's Beardifier: terrain
// adaptation around structures). density[v.index(x, y, z)] += value for the points it affects.
class DensityAddition {
public:
    virtual ~DensityAddition() = default;
    virtual void add(const Volume& v, float* density) const = 0;
};

class ChunkGenerator {
public:
    // A dimension's generator (DimensionSpec::generator). Unknown or broken settings leave ok() false.
    ChunkGenerator(const DataPack& data, BlockRegistry& blocks, i64 seed, const DimensionSpec& dimension);
    // The default overworld generator with these noise settings ("overworld", "large_biomes", "amplified"...).
    ChunkGenerator(const DataPack& data, BlockRegistry& blocks, i64 seed, std::string_view settings = "overworld");
    ~ChunkGenerator();
    bool ok() const { return ok_; }

    enum class Kind { Noise, Flat, Debug };
    Kind kind() const { return kind_; }
    bool hasCeiling() const { return hasCeiling_; }  // the dimension type's (the nether's bedrock roof)
    // Flat worlds: FlatLevelGeneratorSettings ("lakes", "features", "structure_overrides"...), else null.
    const nlohmann::json* flatSettings() const { return kind_ == Kind::Flat ? &flat_ : nullptr; }
    // The debug world's block states, in the game's state id order (DebugLevelSource.ALL_BLOCKS). Set once before
    // generating a debug world; without it the debug world is empty.
    static void setAllStates(std::vector<std::string> stateKeys);
    // DebugLevelSource.getBlockStateFor(x, z) (air outside the grid).
    u16 debugState(int x, int z) const;

    // Thread-safe as long as each thread passes its own DensityContext.
    void generate(int cx, int cz, GenChunk& out, DensityContext& ctx, GenTimings* timings = nullptr,
                  const DensityAddition* beardifier = nullptr) const;
    // NoiseBasedChunkGenerator.iterateNoiseColumn: the base terrain column at block column (x, z) without surface,
    // carvers or structures (default block, fluids from the aquifer, air), out[y - minY]. Thread-safe with a
    // per-thread context.
    void baseColumn(int x, int z, std::vector<u16>& out, DensityContext& ctx) const;

    // Far terrain of one 512x512 region at LOD `level` (columns of 2^level blocks with exact heights; see
    // generator.cpp). coarser: this region at level + 1, if any; then only the surface band is sampled.
    void generateCoarse(int rx, int rz, int level, FarColumns& out, DensityContext& ctx,
                        const FarColumns* coarser = nullptr,
                        double* phaseMs = nullptr,  // phaseMs[0] += density, [1] += materials
                        std::vector<u16>* columnBiomes = nullptr) const;  // n * n surface biomes (biomes() ids)
    BiomeTable& biomes() { return biomes_; }
    const NoiseState& noise() const { return *noise_; }
    // The dimension's build range (chunks are this tall); terrain fills the part of it the noise settings cover.
    int minY() const { return minY_; }
    int height() const { return height_; }
    int genMinY() const { return genMinY_; }  // ChunkGenerator.getMinY / getGenDepth
    int genDepth() const { return genHeight_; }
    int seaLevel() const { return seaLevel_; }
    const BiomeZoom& zoom() const { return *zoom_; }
    u16 noiseBiome(DensityContext& ctx, int qx, int qy, int qz) const { return biomeSource_->noiseBiome(ctx, qx, qy, qz); }
    std::vector<u16> possibleBiomes() const { return biomeSource_->possibleBiomes(); }
    int fixedBiome() const { return biomeSource_->fixedBiome(); }  // a fixed biome source's biome, else -1

private:
    void fill(GenChunk& chunk, DensityContext& ctx, AquiferSampler& aquifer, const DensityAddition* beardifier) const;
    void applyCarvingMask(GenChunk& chunk, DensityContext& ctx, const CarvingMask& mask, AquiferSampler& aquifer,
                          const NoiseBiomeFn& noiseBiome) const;
    void updateHeightmaps(GenChunk& chunk) const;
    bool uncarvable(u16 id) const;
    void init(const DataPack& data, i64 seed, const DimensionSpec& dimension);
    // generateCoarse for flat and debug worlds: their columns do not depend on noise.
    void generateCoarseFixed(int rx, int rz, int level, FarColumns& out, std::vector<u16>* columnBiomes) const;

    BlockRegistry& blocks_;
    BiomeTable biomes_;
    std::unique_ptr<NoiseState> noise_;
    std::unique_ptr<BiomeSource> biomeSource_;
    std::unique_ptr<MaterialSystem> materials_;
    std::unique_ptr<Carvers> carvers_;
    std::unique_ptr<BiomeZoom> zoom_;
    DensityRef finalDensity_;
    nlohmann::json aquifers_;
    bool hasAquifers_ = false, ok_ = false;
    Kind kind_ = Kind::Noise;
    bool hasCeiling_ = false;
    nlohmann::json flat_;
    std::vector<u16> flatLayers_;  // flat worlds: block per y from minY_ (air above the layers)
    int minY_ = -64, height_ = 384, genMinY_ = -64, genHeight_ = 384, seaLevel_ = 63;
    u16 defaultBlock_ = 0, water_ = 0, lava_ = 0, grass_ = 0, mycelium_ = 0, dirt_ = 0;
    std::vector<std::string> uncarvableNames_;
    mutable std::unique_ptr<std::atomic<i8>[]> uncarvableCache_;  // per block id: -1 unknown, 0 no, 1 yes
};

}  // namespace wg
