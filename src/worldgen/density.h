#pragma once
// Density functions (the game's data-driven terrain math) and the per-world "RandomState".
// Values must equal the game's DensitySampler.sampleValue results exactly (float math).
#include "worldgen/datapack.h"
#include "worldgen/noise.h"

#include <memory>

namespace wg {

class DensityFunction;  // opaque node; implemented in density.cpp
using DensityRef = std::shared_ptr<const DensityFunction>;

// Box of sample points, like the game's DensityVolume. Linear index: y + (x + z * sizeX) * sizeY.
struct Volume {
    int sizeX = 1, sizeY = 1, sizeZ = 1;
    int minX = 0, minY = 0, minZ = 0;
    int stepX = 1, stepY = 1, stepZ = 1;
    int blockX(int i) const { return minX + i * stepX; }
    int blockY(int i) const { return minY + i * stepY; }
    int blockZ(int i) const { return minZ + i * stepZ; }
    int index(int x, int y, int z) const { return y + (x + z * sizeX) * sizeY; }
    int size() const { return sizeX * sizeY * sizeZ; }
};

// Evaluation state for one thread (the game's SamplerContext with caches enabled):
// memoises "cache" nodes and interpolation cells. Not thread-safe; one per worker.
class DensityContext {
public:
    DensityContext();
    ~DensityContext();
    DensityContext(const DensityContext&) = delete;
    DensityContext& operator=(const DensityContext&) = delete;
    void clear();  // forget cached values (call between chunks)

    struct Impl;
    std::unique_ptr<Impl> impl;
};

// The game's RandomState for one world seed: seeded noises, random factories and compiled
// density functions. Construction and lookups are thread-safe; sampling is thread-safe as
// long as each thread uses its own DensityContext.
class NoiseState {
public:
    NoiseState(const DataPack& data, i64 seed, bool legacyRandomSource);
    ~NoiseState();

    // An inline JSON function, a numeric constant, or a "minecraft:..." registry reference.
    DensityRef parse(const nlohmann::json& j) const;
    DensityRef function(std::string_view id) const;  // density_function registry entry
    // Seeded noise for a "noise" registry key, created like RandomState.getOrCreateNoise.
    std::shared_ptr<const Noise> noise(std::string_view id) const;
    const PositionalRandomFactory& random() const;  // RandomState.random
    // RandomState.getOrCreateRandomFactory(name): random.fromHashOf(name).forkPositional()
    std::unique_ptr<PositionalRandomFactory> randomFactory(std::string_view name) const;
    i64 seed() const;
    const DataPack& data() const;

    float sample(const DensityRef& f, DensityContext& ctx, int x, int y, int z) const;
    // out[volume.index(x, y, z)] for every point; equal to sample() at each point.
    void sampleVolume(const DensityRef& f, DensityContext& ctx, const Volume& volume, float* out) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wg
