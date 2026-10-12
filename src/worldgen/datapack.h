#pragma once
// The game's built-in data (worldgen JSON and tags), read from the player's own client jar, with the world's
// data packs and the enabled global ones layered over it (assets/asset_source.h, mods/mod_manager.h).
#include "core/common.h"

#include <json/json.hpp>

#include <unordered_map>

namespace wg {

class DataPack {
public:
    bool open(const fs::path& jar);

    // registry: "density_function", "noise", "noise_settings", "biome", "material_rule",
    // "material_condition", "configured_carver", "placed_feature", ... (folders under
    // data/minecraft/worldgen/). id may be "minecraft:x/y" or "x/y". Null if missing.
    const nlohmann::json* worldgen(std::string_view registry, std::string_view id) const;
    // Every entry of a worldgen registry ("minecraft:x/y"), in the game's registration order (sorted by path, then
    // namespace, like RegistryDataLoader).
    std::vector<std::string> ids(std::string_view registry) const;
    // Top-level data folders other than worldgen: "dimension_type", "dimension" (null if missing), and their ids.
    const nlohmann::json* file(std::string_view folder, std::string_view id) const;
    std::vector<std::string> fileIds(std::string_view folder) const;
    // Tags: kind "block", "worldgen/biome", ... ; id without '#'. Expands nested tags.
    std::vector<std::string> tag(std::string_view kind, std::string_view id) const;
    // A tag file's own "values" (nested tags unexpanded, in file order), or null.
    const nlohmann::json* tagValues(std::string_view kind, std::string_view id) const;

    static std::string normalize(std::string_view id);  // strips "minecraft:"
    const fs::path& jar() const { return jar_; }        // the jar open() read
    // Any data file ("data/minecraft/structure/x.nbt") from the game's data with the enabled data packs.
    bool readFile(std::string_view path, std::vector<u8>& out) const;

private:
    fs::path jar_;
    std::unordered_map<std::string, nlohmann::json> files_;  // "worldgen/<registry>/<id>" and "tags/<kind>/<id>"
};

// "minecraft:grass_block" + {"snowy": "false"} -> "minecraft:grass_block[snowy=false]" (sorted keys),
// the state key format used by BlockRegistry::idFor.
std::string blockStateKey(const nlohmann::json& state);

}  // namespace wg
