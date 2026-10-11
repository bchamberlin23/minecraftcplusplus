#pragma once
#include "core/common.h"

#include <windows.h>

#include <functional>

#include "world/nbt_dom.h"

class World;
class BlockRegistry;

struct SaveStats {
    size_t chunks = 0, regions = 0;
    double ms = 0;
    bool ok = true;
};

struct SavedPlayer {
    double x, y, z;
    float yawDeg, pitchDeg;
};

// Writes edits back into the save in the game's own format. Only changed chunks are
// re-encoded; everything else in them (biomes, entities, block entities...) is preserved.
// Each region file is backed up once before it is first overwritten.
class WorldSaver {
public:
    WorldSaver(World& world, const BlockRegistry& registry, const fs::path& saveDir);
    ~WorldSaver();

    // Takes the world's session.lock like the game does, so both can never write at once.
    bool lock();
    bool locked() const { return lock_ != INVALID_HANDLE_VALUE; }
    // Caller must not be editing concurrently (main thread).
    SaveStats save(const SavedPlayer* player);

    // Enables saving chunks that were generated in this session (they have no stored copy yet):
    // biome name at quart coordinates, and the game's data version for new chunks.
    using BiomeNameFn = std::function<std::string(int quartX, int quartY, int quartZ)>;
    void enableNewChunks(BiomeNameFn biomes, int dataVersion) {
        biomeName_ = std::move(biomes);
        dataVersion_ = dataVersion;
    }

private:
    bool saveRegion(int rx, int rz, const std::vector<int>& chunks);
    bool savePlayer(const SavedPlayer& p);

    World& world_;
    const BlockRegistry& registry_;
    fs::path dir_, regionDir_, backupDir_;
    HANDLE lock_ = INVALID_HANDLE_VALUE;
    BiomeNameFn biomeName_;
    int dataVersion_ = 0;
    NbtValue newChunk(int cx, int cz);
};
