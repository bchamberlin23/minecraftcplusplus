#pragma once
#include "core/common.h"

class BlockRegistry;

// Compares our generator with chunks the real game generated (a region folder from a server
// run with a known seed): biomes for chunks at "minecraft:biomes" or later, and every block for
// chunks saved at "minecraft:terrain". Returns 0 when everything matches.
int runGenTest(BlockRegistry& registry, const fs::path& jar, const fs::path& regionDir, i64 seed, int maxChunks);
