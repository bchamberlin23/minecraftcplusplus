#pragma once
// Far terrain of one 512x512 region at LOD `level`: columns 2^level blocks wide with exact block heights,
// stored as runs of identical blocks from the bottom up (air is implicit between runs).
#include "core/common.h"

struct FarColumns {
    struct Run {
        i16 y;      // first block (world y)
        u16 count;  // blocks
        u16 block;  // block state id
    };
    int level = 0, n = 0;     // n = 512 >> level columns per side
    std::vector<u32> start;   // n * n + 1 offsets into runs; column index z * n + x
    std::vector<Run> runs;
    std::vector<u8> biomes;   // surface biome per column (World ids)

    std::pair<const Run*, const Run*> column(int x, int z) const {
        const size_t c = size_t(z) * size_t(n) + size_t(x);
        return {runs.data() + start[c], runs.data() + start[c + 1]};
    }
    // Block at world y in a column (0 = air).
    u16 at(int x, int z, int y) const {
        auto [b, e] = column(x, z);
        for (const Run* r = b; r != e; ++r)
            if (y >= r->y && y < r->y + int(r->count)) return r->block;
        return 0;
    }
};
