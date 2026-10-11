#pragma once
// Terrain generation for one chunk column, equivalent to the game's chunk statuses up to
// "minecraft:terrain": biomes, noise fill with aquifers, surface materials and carvers.
#include "worldgen/aquifer.h"
#include "worldgen/carvers.h"
#include "worldgen/climate.h"
#include "worldgen/density.h"
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
    ChunkGenerator(const DataPack& data, BlockRegistry& blocks, i64 seed, std::string_view settings = "overworld");
    ~ChunkGenerator();
    bool ok() const { return ok_; }

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
    int minY() const { return minY_; }
    int height() const { return height_; }
    int seaLevel() const { return seaLevel_; }
    const BiomeZoom& zoom() const { return *zoom_; }
    u16 noiseBiome(DensityContext& ctx, int qx, int qy, int qz) const { return biomeSource_->noiseBiome(ctx, qx, qy, qz); }
    std::vector<u16> possibleBiomes() const { return biomeSource_->possibleBiomes(); }

private:
    void fill(GenChunk& chunk, DensityContext& ctx, AquiferSampler& aquifer, const DensityAddition* beardifier) const;
    void applyCarvingMask(GenChunk& chunk, DensityContext& ctx, const CarvingMask& mask, AquiferSampler& aquifer,
                          const NoiseBiomeFn& noiseBiome) const;
    void updateHeightmaps(GenChunk& chunk) const;
    bool uncarvable(u16 id) const;

    BlockRegistry& blocks_;
    BiomeTable biomes_;
    std::unique_ptr<NoiseState> noise_;
    std::unique_ptr<OverworldBiomeSource> biomeSource_;
    std::unique_ptr<MaterialSystem> materials_;
    std::unique_ptr<Carvers> carvers_;
    std::unique_ptr<BiomeZoom> zoom_;
    DensityRef finalDensity_;
    nlohmann::json aquifers_;
    bool hasAquifers_ = false, ok_ = false;
    int minY_ = -64, height_ = 384, seaLevel_ = 63;
    u16 defaultBlock_ = 0, water_ = 0, lava_ = 0, grass_ = 0, mycelium_ = 0, dirt_ = 0;
    std::vector<std::string> uncarvableNames_;
    mutable std::unique_ptr<std::atomic<i8>[]> uncarvableCache_;  // per block id: -1 unknown, 0 no, 1 yes
};

}  // namespace wg
