#pragma once
// Biomes: climate sampling, the biome sources (multi-noise with the overworld / nether presets or a data pack's
// list, the End's, fixed and checkerboard) and the game's "zoomed" per-block biome lookup (BiomeManager). Ports
// Climate.java, OverworldBiomeBuilder.java, MultiNoiseBiomeSource(ParameterList), TheEndBiomeSource,
// FixedBiomeSource, CheckerboardColumnBiomeSource and BiomeManager.
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

// BiomeSource: a dimension's noise biomes.
class BiomeSource {
public:
    virtual ~BiomeSource() = default;
    // BiomeSource.getNoiseBiome(quartX, quartY, quartZ, sampler).
    virtual u16 noiseBiome(DensityContext& ctx, int quartX, int quartY, int quartZ) const = 0;
    // ChunkAccess.fillBiomesFromNoise for the whole column (sections, then x / y / z within each).
    virtual void fillBiomes(GenChunk& chunk, DensityContext& ctx) const;
    // BiomeSource.possibleBiomes(): in the source's order, without duplicates (FeatureSorter input order).
    virtual std::vector<u16> possibleBiomes() const = 0;
    // FixedBiomeSource's biome (its own findBiomeHorizontal), else -1.
    virtual int fixedBiome() const { return -1; }
};

// The "biome_source" of a generator. router: the noise settings' "noise_router" (the climate functions).
// Throws std::runtime_error for a type or preset it does not know.
std::unique_ptr<BiomeSource> makeBiomeSource(const nlohmann::json& source, const NoiseState& noise,
                                             const nlohmann::json& router, BiomeTable& table);

class MultiNoiseBiomeSource final : public BiomeSource {
public:
    // source: {"preset": "minecraft:overworld" | "minecraft:nether" | a parameter list id} or {"biomes": [...]}.
    // router: the noise_settings "noise_router" object (temperature, vegetation, continents, erosion, depth, ridges
    // are used).
    MultiNoiseBiomeSource(const NoiseState& noise, const nlohmann::json& router, BiomeTable& table,
                          const nlohmann::json& source = {{"preset", "minecraft:overworld"}});
    ~MultiNoiseBiomeSource() override;

    // MultiNoiseBiomeSource.getNoiseBiome(quartX, quartY, quartZ, sampler).
    u16 noiseBiome(DensityContext& ctx, int quartX, int quartY, int quartZ) const override;
    void fillBiomes(GenChunk& chunk, DensityContext& ctx) const override;
    // The parameter list's biomes in first-appearance order.
    std::vector<u16> possibleBiomes() const override;

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
