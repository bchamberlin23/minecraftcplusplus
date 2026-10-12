#pragma once
#include "core/common.h"
#include "world/nbt_dom.h"

#include <array>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

class BlockRegistry;

struct PlayerStart {
    double x = 0.5, y = 100, z = 0.5;
    float yaw = 0, pitch = 0;
    bool found = false;
};

struct LoadStats {
    size_t regions = 0, chunks = 0, sections = 0, unsupportedChunks = 0;
    u64 compressedBytes = 0;
};

// Block storage in the game's own palette format (registry ids + packed indices),
// so a section costs ~bits*512 bytes instead of 8 KB.
//
// Threading: the main thread is the only writer. Background readers take mutex() shared;
// the main thread takes it exclusively around edits.
class World {
public:
    // Light layer reference: absent, uniform (kLightUniform | level) or index into Region::lightArrays.
    static constexpr u32 kLightAbsent = 0xFFFFFFFF;
    static constexpr u32 kLightUniform = 0x80000000;
    using LightArray = std::array<u8, 2048>;  // nibbles, index = y << 8 | z << 4 | x

    struct Section {
        u32 paletteOffset = 0;
        u32 dataOffset = 0;
        u32 sky = kLightAbsent, block = kLightAbsent;
        u16 paletteSize = 0;  // 0 = no blocks (may still carry light)
        u8 bits = 0;          // 0 = uniform (palette[0])
    };
    struct Column {
        u32 first = 0;  // index into Region::sections
        i8 minY = 0;    // section Y of sections[first]
        u8 count = 0;
    };
    struct Region {
        Region() { std::fill(std::begin(biomes), std::end(biomes), kUnknownBiome); }
        int rx = 0, rz = 0;
        Column columns[1024];  // [z * 32 + x]
        u8 biomes[1024 * 16];  // surface biome per 4x4 column: [column * 16 + qz * 4 + qx]
        std::vector<Section> sections;
        std::vector<u16> palettes;
        std::vector<u64> words;
        std::deque<LightArray> lightArrays;  // deque: arrays never move
    };

    // Resolves the game's stored light for one section, including its rule for sections
    // without sky data: reuse the bottom layer of the nearest lit section above (or 15).
    struct LightView {
        const Region* skyRegion = nullptr;
        const Region* blockRegion = nullptr;
        u32 sky = kLightUniform | 15, block = kLightAbsent;
        bool skyBottomRow = false;
        static u8 nibble(const Region* r, u32 ref, int index) {
            if (ref == kLightAbsent) return 0;
            if (ref & kLightUniform) return u8(ref & 15);
            u8 b = r->lightArrays[ref][index >> 1];
            return (index & 1) ? (b >> 4) : (b & 15);
        }
        u8 sky4(int x, int y, int z) const { return nibble(skyRegion, sky, ((skyBottomRow ? 0 : y) << 8) | (z << 4) | x); }
        u8 block4(int x, int y, int z) const { return nibble(blockRegion, block, (y << 8) | (z << 4) | x); }
        u8 at(int x, int y, int z) const { return u8((sky4(x, y, z) << 4) | block4(x, y, z)); }  // sky << 4 | block
    };
    LightView light(int cx, int sy, int cz) const;

    // Biome names <-> small ids shared by everything that stores biomes (thread-safe).
    static constexpr u8 kUnknownBiome = 0xFF;
    static u8 biomeId(std::string_view name);  // "minecraft:plains"; kUnknownBiome when the table is full
    static std::string biomeName(u8 id);
    static std::string_view biomeNameView(u8 id);  // stays valid for the program's lifetime ("" if unknown)
    u8 biomeAt(int x, int z) const;  // surface biome of the 4x4 column holding block (x, z), or unknown
    void setChunkBiomes(int cx, int cz, const u8* quarts);  // 16 ids, [qz * 4 + qx]; the region must exist

    static PlayerStart readPlayer(const fs::path& saveDir);
    // World seed: data/minecraft/world_gen_settings.dat (26.x) or level.dat WorldGenSettings (older).
    static bool readSeed(const fs::path& saveDir, i64& seed);

    // Loads every fully generated chunk within `radius` chunks of the centre (radius <= 0: all).
    bool load(const fs::path& saveDir, BlockRegistry& registry, int centerCx, int centerCz, int radius,
              LoadStats& stats);

    std::shared_mutex& mutex() const { return mutex_; }

    const Section* section(int cx, int sy, int cz, const Region** outRegion) const;  // with blocks only
    const Section* anySection(int cx, int sy, int cz, const Region** outRegion) const;
    const Column* column(int cx, int cz, const Region** outRegion) const;
    const std::vector<std::unique_ptr<Region>>& regions() const { return regions_; }
    int minSectionY() const { return minSy_; }  // vertical build range in sections
    int maxSectionY() const { return maxSy_; }
    bool loaded(int x, int z) const { return column(x >> 4, z >> 4, nullptr) != nullptr; }

    // Block access by world coordinates (air outside the loaded world).
    u16 block(int x, int y, int z) const;
    // Edits (main thread, holding mutex() exclusively). Returns false outside the loaded world.
    bool setBlock(int x, int y, int z, u16 id);
    // Adds a freshly generated chunk column (ids for y in [minY, minY + height), index
    // ((y - minY) * 16 + z) * 16 + x). Creates its region if needed; never replaces a chunk that
    // already has data. Main thread, holding mutex() exclusively. Returns false if skipped.
    bool insertChunk(int cx, int cz, int minY, int height, const u16* blocks);
    bool hasChunk(int cx, int cz) const {
        const Column* c = column(cx, cz, nullptr);
        return c && c->count;
    }
    // Writable light layer of a section, materialised from the resolved values if needed.
    LightArray* lightArray(int cx, int sy, int cz, bool sky);

    // Chunk columns received from a server (main thread, holding mutex() exclusively; never saved).
    struct PackedSection {
        std::vector<u16> palette;  // block ids; empty = no blocks
        u8 bits = 0;               // 0 = uniform palette[0], else index bits per entry in words
        std::vector<u64> words;
        u32 sky = kLightAbsent, block = kLightAbsent;  // absent or kLightUniform | level when no data is given
        std::vector<u8> skyData, blockData;            // 2048 bytes of nibbles, or empty
        bool hasBiomes = false;
        u8 biomes[16] = {};  // lowest quart layer, [qz * 4 + qx], World biome ids
    };
    // Empty world with a fixed vertical range, filled at runtime (before Terrain is created).
    void initEmpty(int minSectionY, int maxSectionY) { minSy_ = minSectionY, maxSy_ = maxSectionY; }
    // Replaces a column with sections[i] at section y = firstSy + i (light may extend one section past
    // the build range).
    void replaceChunk(int cx, int cz, int firstSy, const std::vector<PackedSection>& sections);
    void removeChunk(int cx, int cz);
    // Whole dimension unload, holding mutex() exclusively. Region objects/indices stay stable
    // for background terrain jobs; all chunk data and block entities are discarded at once.
    void clearChunks();
    // Sets one light layer of a section of a loaded column (data: 2048 bytes, or null for `ref`).
    // Returns true if the stored values changed.
    bool setLight(int cx, int sy, int cz, bool sky, u32 ref, const u8* data);

    // Chunks changed since the last save, as (cx << 32 | cz) keys.
    const std::unordered_set<u64>& dirtyChunks() const { return dirtyChunks_; }
    void clearDirtyChunks() { dirtyChunks_.clear(); }
    void clearDirtyChunk(int cx, int cz) { dirtyChunks_.erase(chunkKey(cx, cz)); }
    void markChunkDirty(int cx, int cz) { dirtyChunks_.insert(chunkKey(cx, cz)); }  // saved like an edited chunk
    // What a chunk generated this session saves besides its blocks (a compound with block_entities, entities,
    // block_ticks, fluid_ticks and PostProcessing, in the game's chunk format); WorldSaver::newChunk writes it.
    void setGenExtras(int cx, int cz, NbtValue extras) { genExtras_[chunkKey(cx, cz)] = std::move(extras); }
    const NbtValue* genExtras(int cx, int cz) const {
        const auto it = genExtras_.find(chunkKey(cx, cz));
        return it == genExtras_.end() ? nullptr : &it->second;
    }
    // Generated chunks regenerate identically from the seed, so lighting them is not a change.
    void setTrackChanges(bool track) { trackChanges_ = track; }
    static u64 chunkKey(int cx, int cz) { return (u64(u32(cx)) << 32) | u32(cz); }

    // Block entity data (sign text, banner patterns, skull owners, pot sherds, ...): the game's NBT of each
    // block entity as a network tag (type byte + payload), by chunk. From saves and the server; which blocks
    // have block entities follows from the block states, so this is only the extra data. Main thread only.
    struct BlockEntityNbt {
        i32 x = 0, y = 0, z = 0;
        std::string nbt;
    };
    const std::vector<BlockEntityNbt>* blockEntities(int cx, int cz) const {
        auto it = blockEntities_.find(chunkKey(cx, cz));
        return it == blockEntities_.end() ? nullptr : &it->second;
    }
    void setChunkBlockEntities(int cx, int cz, std::vector<BlockEntityNbt> list);  // replaces the chunk's list
    void setBlockEntity(BlockEntityNbt be);  // adds or replaces the one at its position
    // Opt-in log of chunks whose blocks or block entities changed (setBlock, chunk insert / replace / remove),
    // as chunkKey values, for consumers that mirror world content (block entity rendering).
    void trackBlockChanges(bool on) { trackBlockChanges_ = on; }
    void takeBlockChanges(std::vector<u64>& out) {
        out.swap(blockChanges_);
        blockChanges_.clear();
    }

    static u16 blockAt(const Region& r, const Section& s, u32 index) {  // index = y*256 + z*16 + x
        if (s.bits == 0) return r.palettes[s.paletteOffset];
        u32 perWord = 64 / s.bits;
        u64 w = r.words[s.dataOffset + index / perWord];
        u32 v = u32(w >> ((index % perWord) * s.bits)) & ((1u << s.bits) - 1);
        return v < s.paletteSize ? r.palettes[s.paletteOffset + v] : 0;
    }
    static void decode(const Region& r, const Section& s, u16* out);  // 4096 ids, YZX order

private:
    Region* regionFor(int cx, int cz);
    Region& regionOrCreate(int cx, int cz);
    Section& ensureSection(Region& r, Column& c, int sy);
    u32 storeLight(Region& r, u32 ref, const u8* data);
    void compact(Region& r);

    std::vector<std::unique_ptr<Region>> regions_;
    std::unordered_map<u64, Region*> lookup_;
    int minSy_ = 0, maxSy_ = 0;
    mutable std::shared_mutex mutex_;
    std::unordered_set<u64> dirtyChunks_;
    std::unordered_map<u64, NbtValue> genExtras_;
    bool trackChanges_ = true;
    std::unordered_map<u64, std::vector<BlockEntityNbt>> blockEntities_;
    std::vector<u64> blockChanges_;
    bool trackBlockChanges_ = false;
    void noteBlockChange(int cx, int cz) {
        const u64 k = chunkKey(cx, cz);
        if (trackBlockChanges_ && (blockChanges_.empty() || blockChanges_.back() != k)) blockChanges_.push_back(k);
    }
};
