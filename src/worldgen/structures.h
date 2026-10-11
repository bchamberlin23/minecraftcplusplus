#pragma once
// Structures for one dimension, like the game's structure chunk statuses and decoration step:
//   starts      - ChunkGenerator.createStructures: structure sets (random spread with frequency reduction and
//                 exclusion zones, concentric rings for strongholds), biome and terrain height checks, pieces
//   references  - ChunkGenerator.createReferences: the starts within 8 chunks whose box reaches a chunk
//   terrain     - the Beardifier term of the final density (terrain adaptation around pieces)
//   decoration  - StructureStart.placeInChunk for each structure in its GenerationStep, before the step's features
// Starts are computed lazily per chunk and cached; everything here is thread-safe.
#include "worldgen/generator.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

class BlockRegistry;

namespace wg {

class FeatureSystem;
class GenRegion;
class WorldgenRandom;

namespace st {
class StructureEnv;
class Pools;
}  // namespace st

class StructureSystem {
public:
    // features: for feature pool elements, rule tests and providers (structures need it to place anything).
    StructureSystem(const ChunkGenerator& generator, const FeatureSystem* features, BlockRegistry& blocks, i64 seed);
    ~StructureSystem();
    bool ok() const;

    // The terrain adaptation density for a chunk (null when no structure piece is near it).
    std::unique_ptr<DensityAddition> beardifier(int cx, int cz) const;
    // ChunkGenerator.applyBiomeDecoration's structure part of one GenerationStep for the region's centre chunk.
    void placeStep(int step, GenRegion& level, int cx, int cz, WorldgenRandom& random, i64 decorationSeed) const;
    // LevelChunk.postProcessGeneration for the blocks structure pieces marked in chunks[4] (fences, torches, ladders,
    // bars): their shapes are updated from their final neighbours. Call when the chunk's 3x3 is decorated.
    void postProcess(const std::array<GenChunk*, 9>& chunks) const;
    // Drops cached starts, heights and marks farther than radius chunks from (cx, cz); they are deterministic and
    // recomputed when needed again. Without it the caches grew with every chunk generated while travelling.
    void trim(int cx, int cz, int radius) const;

    // Reports for verification: the valid starts originating in a chunk.
    struct PieceReport {
        std::string type;  // piece type id
        int box[6];        // minX, minY, minZ, maxX, maxY, maxZ
        std::string detail;
    };
    struct StartReport {
        std::string id;
        int chunkX = 0, chunkZ = 0;
        int box[6] = {};  // the start's bounding box (adjusted for terrain adaptation)
        std::vector<PieceReport> pieces;
    };
    std::vector<StartReport> startsIn(int cx, int cz) const;
    // Structure ids whose starts reach the chunk (its references), "id@sourceX,sourceZ".
    std::vector<std::string> referencesOf(int cx, int cz) const;
    double startMs() const;  // total time spent computing starts (profiling)

    st::StructureEnv& env() const;
    const st::Pools& pools() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wg
