#pragma once
// Biomes: climate sampling, the overworld multi-noise biome source and the game's "zoomed"
// per-block biome lookup (BiomeManager). Ports Climate.java, OverworldBiomeBuilder.java,
// MultiNoiseBiomeSource and BiomeManager.
#include "worldgen/density.h"
#include "worldgen/gen_chunk.h"

#include <mutex>
#include <unordered_map>

namespace wg {

// Biome names ("minecraft:plains") <-> compact ids. Thread-safe.
class BiomeTable {
public:
    u16 id(std::string_view name);
    const std::string& name(u16 id) const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, u16> ids_;
};

// noise biome at quart coordinates (block >> 2), as the biome source returns it
using NoiseBiomeFn = std::function<u16(int quartX, int quartY, int quartZ)>;

class OverworldBiomeSource {
public:
    // router: the noise_settings "noise_router" object (temperature, vegetation, continents,
    // erosion, depth, ridges are used).
    OverworldBiomeSource(const NoiseState& noise, const nlohmann::json& router, BiomeTable& table);
    ~OverworldBiomeSource();

    // MultiNoiseBiomeSource.getNoiseBiome(quartX, quartY, quartZ, sampler).
    u16 noiseBiome(DensityContext& ctx, int quartX, int quartY, int quartZ) const;
    // ChunkAccess.fillBiomesFromNoise for the whole column.
    void fillBiomes(GenChunk& chunk, DensityContext& ctx) const;
    // MultiNoiseBiomeSource.possibleBiomes(): the parameter list's biomes in first-appearance order.
    std::vector<u16> possibleBiomes() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// BiomeManager: the per-block biome with the game's seeded "fuzzy" zoom between quart cells.
class BiomeZoom {
public:
    explicit BiomeZoom(i64 worldSeed);  // applies BiomeManager.obfuscateSeed
    u16 biome(int blockX, int blockY, int blockZ, const NoiseBiomeFn& noiseBiome) const;

private:
    i64 zoomSeed_;
};

}  // namespace wg
