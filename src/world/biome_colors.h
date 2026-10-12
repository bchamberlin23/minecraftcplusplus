#pragma once
// Biome tints (grass, foliage, water) from the game's own biome definitions and colormaps, read from
// the player's jar, blended over the Biome Blend option's square of blocks (5x5 by default) like the game.
#include "core/common.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

class World;

class BiomeColors {
public:
    enum Kind { Grass, Foliage, Water, KindCount };

    BiomeColors();
    ~BiomeColors();
    bool init(const fs::path& jar);
    // 0xRRGGBB for a World biome id; unknown biomes use plains. Thread-safe.
    u32 color(u8 biome, Kind kind) const;

private:
    struct Def;
    u32 compute(u8 biome, Kind kind) const;

    std::vector<u32> grassMap_, foliageMap_;  // 256x256 colormaps, 0xRRGGBB
    std::unique_ptr<std::unordered_map<std::string, Def>> defs_;
    mutable std::array<std::atomic<u32>, 256 * KindCount> cache_{};  // bit 31 set = computed
};

// Indices into the global colour table (see mesher.h) for every column of a padded section (18x18,
// [(z + 1) * 18 + (x + 1)]) by tint kind.
struct ColumnColors {
    u16 index[BiomeColors::KindCount][18 * 18];
};

// Blended colours of the padded columns around section column (cx, cz). Caller holds a shared lock on
// world.mutex(). Columns of unknown biome get the game's default tints.
void gatherColumnColors(const World& world, const BiomeColors& colors, int cx, int cz, ColumnColors& out);
void setBiomeBlendRadius(int radius);  // the Biome Blend option (0..7, default 2: 5x5); re-mesh after a change
