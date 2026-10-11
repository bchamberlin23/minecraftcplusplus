#pragma once
// Surface materials (the game's MaterialSystem + material rules / conditions, formerly
// "surface rules"): grass, dirt, sand, snow, terracotta bands, bedrock, deepslate, ore veins...
#include "worldgen/climate.h"
#include "worldgen/density.h"
#include "worldgen/gen_chunk.h"

#include <optional>

class BlockRegistry;

namespace wg {

class MaterialSystem {
public:
    // noiseSettings: the full noise_settings JSON (material_rule, default_block, sea_level...).
    MaterialSystem(const NoiseState& noise, BlockRegistry& blocks, const BiomeTable& biomes,
                   const nlohmann::json& noiseSettings);
    ~MaterialSystem();

    // MaterialSystem.buildSurface over a chunk filled with terrain + fluids.
    void buildSurface(GenChunk& chunk, DensityContext& ctx, const BiomeZoom& zoom, const NoiseBiomeFn& noiseBiome) const;
    // MaterialSystem.topMaterial (used when carvers expose dirt under grass).
    std::optional<u16> topMaterial(GenChunk& chunk, DensityContext& ctx, const BiomeZoom& zoom,
                                   const NoiseBiomeFn& noiseBiome, int x, int y, int z, bool underFluid) const;

    // The rules at one position with an explicit context (far-terrain approximation): stoneAbove/stoneBelow and
    // waterHeight (one above the fluid surface over the block, or INT_MIN) as buildSurface computes them. Only the
    // chunk's position and worldSurfaceWG heightmap are read.
    std::optional<u16> materialAt(GenChunk& chunk, DensityContext& ctx, const BiomeZoom& zoom,
                                  const NoiseBiomeFn& noiseBiome, int x, int y, int z, int stoneAbove, int stoneBelow,
                                  int waterHeight) const;
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wg
