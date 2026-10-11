#pragma once
// Underground water/lava placement (the game's Aquifer / NoiseBasedAquifer).
#include "worldgen/density.h"

namespace wg {

class AquiferSampler {
public:
    static constexpr u16 kDefaultBlock = 0xFFFF;  // "null" in the game: place the default block (stone)

    // config: noise_settings "aquifers" object, or null for a disabled aquifer (global fluids only).
    // volume: the chunk being generated (16 x height x 16 at the chunk's min corner).
    AquiferSampler(const NoiseState& noise, DensityContext& ctx, const nlohmann::json* config, const Volume& volume,
                   int seaLevel, u16 water, u16 lava, u16 air);
    ~AquiferSampler();

    // Aquifer.computeSubstance(x, y, z, density): fluid / air id, or kDefaultBlock.
    u16 computeSubstance(int x, int y, int z, float density);
    bool shouldScheduleFluidUpdate() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wg
