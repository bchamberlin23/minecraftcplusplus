#pragma once
// Exact outline (selection) and collision shapes of every block state, taken once from the game's own code
// (they are defined in code, not data) and cached per version. Shapes of blocks with a random per-position
// offset (flowers, grass...) are stored without the offset.
#include "assets/assets.h"

#include <unordered_map>

class BlockShapes {
public:
    bool load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir);

    struct Entry {
        u32 outline, collision;  // shape indices
    };
    const Entry* find(std::string_view fullStateKey) const;
    const std::vector<Box>& shape(u32 index) const { return shapes_[index]; }
    size_t stateCount() const { return states_.size(); }

private:
    std::vector<std::vector<Box>> shapes_;
    std::unordered_map<std::string, Entry> states_;
};
