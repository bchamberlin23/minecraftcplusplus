#pragma once
// Block/registry definitions produced by the game's own data generator ("--reports"), run once
// per game version with the player's installed Java runtime and cached. Gives every block's
// properties, default state and network state ids, which the game's data files do not contain.
#include "core/common.h"

#include <json/json.hpp>

#include <unordered_map>

// java.exe of the runtime the launcher installed for this game version (empty if missing).
fs::path findGameJava(const fs::path& minecraftDir, const std::string& version);
// Runs a single-file Java program (its source text; launched as `java <File>.java`, no compiler needed) with the
// game's client jar and libraries on the class path, to extract data only the game's code has. Blocks until it
// exits; true on exit code 0.
bool runGameProgram(const fs::path& minecraftDir, const std::string& version, const std::string& className,
                    const std::string& source, const std::vector<std::wstring>& args, const fs::path& workDir,
                    unsigned timeoutMs = 180000);

class GameReports {
public:
    // Loads the cached reports for this version, generating them first if needed.
    bool load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir);

    // "minecraft:oak_leaves" -> "minecraft:oak_leaves[distance=7,persistent=false,waterlogged=false]".
    // Properties already given are kept; missing ones get the block's defaults. Unknown blocks are
    // returned unchanged.
    std::string complete(std::string_view stateKey) const;
    int stateId(std::string_view fullStateKey) const;  // network id, -1 if unknown
    const std::unordered_map<std::string, int>& stateIds() const { return stateIds_; }  // full state key -> id
    bool hasBlock(std::string_view name) const { return blocks_.count(std::string(name)) != 0; }
    const nlohmann::json& registries() const { return registries_; }
    const nlohmann::json& packets() const { return packets_; }

private:
    struct Block {
        std::vector<std::pair<std::string, std::string>> defaults;  // sorted by key
    };
    bool generate(const fs::path& minecraftDir, const std::string& version, const fs::path& out);

    std::unordered_map<std::string, Block> blocks_;
    std::unordered_map<std::string, int> stateIds_;
    nlohmann::json registries_, packets_;
};
