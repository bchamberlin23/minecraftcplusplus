#pragma once
// A chunk column while it is being generated, plus the block/biome id lookups worldgen uses.
#include "core/common.h"
#include "world/nbt_dom.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wg {

// Block state key ("minecraft:stone", "minecraft:grass_block[snowy=false]") -> registry id.
using BlockLookup = std::function<u16(std::string_view stateKey)>;

struct GenChunk;
// SerializableChunkData's view of what generation left besides the blocks: block_entities, entities, block_ticks,
// fluid_ticks and PostProcessing, for saving the chunk as a proto chunk past its features.
NbtValue saveExtras(const GenChunk& c, const std::function<std::string(u16 state)>& stateName);

struct GenChunk {
    static constexpr u16 kAir = 0;
    int cx = 0, cz = 0;         // chunk coordinates
    int minY = -64, height = 384;
    std::vector<u16> blocks;    // ((y - minY) * 16 + z) * 16 + x
    std::vector<u16> biomes;    // ((qy * 4) + qz) * 4 + qx, qy = (y - minY) >> 2; ids from BiomeTable
    // Heightmaps during generation (the game's *_WG types): one past the highest matching block,
    // or minY when the column has none. Index z * 16 + x. Kept up to date by the generator.
    int worldSurfaceWG[256] = {};  // any non-air block
    int oceanFloorWG[256] = {};    // blocks that block motion (non-air, non-fluid, solid)
    // The final heightmaps (WORLD_SURFACE, MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES, OCEAN_FLOOR), same
    // convention. Primed by decoration from the blocks (the game primes them when terrain finishes) and then
    // kept up to date by every decoration write (wg::GenRegion).
    int finalHeightmaps[4][256] = {};
    bool finalHeightmapsPrimed = false;
    // What the game saves with a generated chunk besides its blocks: the block entity tags features and structures
    // configured ({"id", "x", "y", "z", ...}: loot tables with their seeds, spawner entities, template NBT), by
    // index(); entity tags they added ({"id", "Pos", ...}); positions marked for post-processing
    // (ChunkAccess.markPosForPostProcessing: fluids to schedule, shapes to update), by index().
    std::unordered_map<u32, NbtValue> blockEntities;
    std::vector<NbtValue> entities;
    std::vector<u32> postProcess;
    // Ticks scheduled during generation (block_ticks / fluid_ticks): the state's block or fluid, the delay.
    struct Tick {
        u32 index;
        u16 state;
        int delay;
        bool fluid;
    };
    std::vector<Tick> ticks;

    void init(int chunkX, int chunkZ, int bottomY, int totalHeight) {
        cx = chunkX, cz = chunkZ, minY = bottomY, height = totalHeight;
        blocks.assign(size_t(16) * 16 * height, kAir);
        biomes.assign(size_t(4) * 4 * (height / 4), 0);
        std::fill(std::begin(worldSurfaceWG), std::end(worldSurfaceWG), minY);
        std::fill(std::begin(oceanFloorWG), std::end(oceanFloorWG), minY);
        finalHeightmapsPrimed = false;
        blockEntities.clear();
        entities.clear();
        postProcess.clear();
        ticks.clear();
    }
    u32 index(int x, int y, int z) const { return u32((size_t(y - minY) * 16 + z) * 16 + x); }  // local x / z, world y
    void markPostProcess(int x, int y, int z) {
        if (inside(y)) postProcess.push_back(index(x, y, z));
    }
    bool inside(int y) const { return y >= minY && y < minY + height; }
    // Local x/z (0..15), world y.
    u16 get(int x, int y, int z) const { return inside(y) ? blocks[(size_t(y - minY) * 16 + z) * 16 + x] : kAir; }
    void set(int x, int y, int z, u16 id) {
        if (inside(y)) blocks[(size_t(y - minY) * 16 + z) * 16 + x] = id;
    }
    u16 biome(int qx, int qy, int qz) const {  // quart coordinates relative to the chunk / minY
        qy = std::clamp(qy, 0, height / 4 - 1);
        return biomes[(size_t(qy) * 4 + (qz & 3)) * 4 + (qx & 3)];
    }
    int minBlockX() const { return cx * 16; }
    int minBlockZ() const { return cz * 16; }
};

}  // namespace wg
