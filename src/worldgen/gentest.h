#pragma once
#include "core/common.h"
#include "worldgen/dimensions.h"

class BlockRegistry;

// What a gentest run compares: a region folder (the default overworld generator with the given seed), or a world
// folder (its saved WorldGenSettings; GENTEST_DIM names the dimension, default minecraft:overworld).
struct GenTestTarget {
    fs::path regionDir;
    wg::DimensionSpec dim;
    i64 seed = 0;
    bool structures = true;  // the world's generate_structures
};
GenTestTarget genTestTarget(const wg::DataPack& data, const fs::path& dir, i64 seed);

// Compares our generator with chunks the real game generated (a region folder from a server
// run with a known seed): biomes for chunks at "minecraft:biomes" or later, and every block for
// chunks saved at "minecraft:terrain". Returns 0 when everything matches.
int runGenTest(BlockRegistry& registry, const fs::path& jar, const fs::path& regionDir, i64 seed, int maxChunks);
// Structures (gentest_structures.cpp; GENTEST_STRUCTURES=starts|all selects it in runGenTest): starts, references
// and blocks against a world generated with structures.
int runStructureTest(BlockRegistry& registry, const fs::path& jar, const fs::path& regionDir, i64 seed, const std::string& mode);
