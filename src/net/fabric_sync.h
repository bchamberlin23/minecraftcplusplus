#pragma once
// Fabric servers whose mods add blocks or items (mods/fabric_server.h):
//  - Fabric API's registry sync disconnects clients that cannot receive its registry ids. During configuration we
//    answer the server's "minecraft:register" with the channel "fabric:registry/sync", read the ids it sends and
//    confirm with "fabric:registry/sync/complete", like a Fabric client.
//  - Block state ids: a modded server appends its blocks' states after the game's. The local Fabric server runs a
//    small helper mod that writes every block's states (mcpp-registries.json); with the synced block ids this gives
//    the server's global state table, so its chunks map to state keys our BlockRegistry draws from the mods' models.
#include "core/common.h"

#include <unordered_map>

namespace net {

struct Reader;

class FabricSync {
public:
    // mcpp-registries.json written by the helper mod in the local server's directory.
    bool loadDump(const fs::path& file);
    // Body of a fabric:registry/sync payload (after the channel id).
    bool parseSync(Reader& r);
    // Server state id -> state key ("toms_storage:ts.inventory_connector[...]"); empty when unknown.
    std::vector<std::string> stateTable() const;
    size_t moddedBlocks() const;  // synced or dumped blocks outside the minecraft namespace

private:
    struct Block {
        std::string id;
        std::vector<std::string> states;  // state keys, in the block's state order
    };
    std::vector<Block> blocks_;      // dump, server raw id order
    std::vector<std::string> table_;  // dump of Block.BLOCK_STATE_REGISTRY
    std::unordered_map<std::string, std::unordered_map<std::string, int>> synced_;  // registry -> id -> raw id
};

}  // namespace net
