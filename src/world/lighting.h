#pragma once
#include "core/common.h"
#include "world/world.h"

#include <unordered_map>
#include <unordered_set>

class BlockRegistry;

// Incremental light engine: after a block changes, removes and re-propagates block light and
// sky light with the game's rules (each step costs max(1, opacity); sky light at 15 travels
// straight down through fully transparent blocks without fading). Works directly on the
// world's light arrays, materialising them on first touch.
class Lighting {
public:
    Lighting(World& world, const BlockRegistry& registry) : world_(world), registry_(registry) {}

    // Caller holds world.mutex() exclusively. Adds every section whose light changed, plus
    // neighbours whose faces sample it, to `dirty` as packed section keys (see sectionKey).
    void blockChanged(int x, int y, int z, std::unordered_set<u64>& dirty);
    // Lights a freshly inserted chunk: sky light straight down from the top, then flood fill from
    // its cells, its emitters and its neighbours' borders (light also spills into neighbours).
    void initChunk(int cx, int cz, std::unordered_set<u64>& dirty);

    static u64 sectionKey(int cx, int sy, int cz) {
        return (u64(u32(cx + (1 << 23)) & 0xFFFFFF) << 40) | (u64(u32(sy + 2048) & 0xFFF) << 28) |
               u64(u32(cz + (1 << 23)) & 0xFFFFFF);
    }
    static void unpackSectionKey(u64 k, int& cx, int& sy, int& cz) {
        cx = int((k >> 40) & 0xFFFFFF) - (1 << 23);
        sy = int((k >> 28) & 0xFFF) - 2048;
        cz = int(k & 0xFFFFFF) - (1 << 23);
    }

private:
    struct Channel;
    void relight(bool sky, int x, int y, int z, std::unordered_set<u64>& dirty);

    World& world_;
    const BlockRegistry& registry_;
};
