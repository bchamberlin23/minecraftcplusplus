#pragma once
// Caves and canyons (the game's carvers): which blocks of a chunk get carved out.
// The generator then applies the mask (fluids via the aquifer, grass re-surfacing).
#include "worldgen/climate.h"
#include "worldgen/density.h"

#include <functional>

namespace wg {

// One bit per block of a chunk over the carvable y range (the game's CarvingMask).
class CarvingMask {
public:
    void init(int minY, int maxY);  // carvable range [minY, maxY]
    void set(int x, int y, int z);  // local x/z, world y
    bool get(int x, int y, int z) const;
    bool empty() const { return count_ == 0; }
    int minY() const { return minY_; }
    int maxY() const { return maxY_; }
    // fn(x, z, bottomY, topY) for each column run of carved blocks, in the game's visit order.
    void visit(const std::function<void(int x, int z, int bottomY, int topY)>& fn) const;

private:
    int minY_ = 0, maxY_ = -1;
    size_t count_ = 0;
    std::vector<u64> bits_;
};

class Carvers {
public:
    Carvers(const NoiseState& noise, const BiomeTable& biomes);
    ~Carvers();
    // The game's generateCarvers loop for chunk (chunkX, chunkZ): every source chunk within range,
    // the carvers of the biome at that source chunk, seeded like the game, writing into mask.
    // genMinY/genDepth: the dimension's generation height (overworld -64 / 384).
    void carve(int chunkX, int chunkZ, const NoiseBiomeFn& noiseBiome, CarvingMask& mask, int genMinY,
               int genDepth) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wg
