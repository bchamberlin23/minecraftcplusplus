#pragma once
// Items: the item registry (network ids from the game's reports), item stacks as the protocol carries them
// (count, item, data component patch kept as raw bytes so it can be sent back) and display names.
#include "core/common.h"
#include "net/protocol.h"
#include "ui/text.h"

#include <unordered_map>

class GameReports;
class BlockRegistry;

namespace ui {

class Resources;

struct ItemStack {
    int item = -1;  // network id; < 0: empty
    int count = 0;
    struct Component {
        int type;
        std::string data;  // the component's encoded value
    };
    std::vector<Component> added;
    std::vector<int> removed;
    bool componentsKnown = true;  // false: a component could not be decoded (dropped)

    // Decoded for display.
    bool hasCustomName = false, hasItemName = false;
    Text customName, itemName;
    std::vector<Text> lore;
    int damage = 0, maxDamage = -1, maxStack = -1, rarity = -1, glint = -1;
    int enchantments = 0;  // count
    i64 dyedColor = -1, potionColor = -1;
    int potion = -1;

    bool empty() const { return item < 0 || count <= 0; }
    bool sameItemSameComponents(const ItemStack& o) const;
    ItemStack withCount(int n) const {
        ItemStack s = *this;
        s.count = n;
        if (n <= 0) s = ItemStack{};
        return s;
    }
};

class Items {
public:
    bool init(const GameReports& reports, const Resources& res);
    int count() const { return int(names_.size()); }
    const std::string& name(int id) const;  // "minecraft:stone"
    int id(std::string_view name) const;    // -1 if unknown
    int componentType(std::string_view name) const;
    const Lang& lang() const { return *lang_; }

    int maxStack(const ItemStack& s) const;
    int maxDamage(const ItemStack& s) const;
    bool isBlockItem(int id) const { return id >= 0 && id < int(blockItem_.size()) && blockItem_[size_t(id)]; }
    Text displayName(const ItemStack& s) const;   // custom name (italic) > item_name > translated name
    Text hoverName(const ItemStack& s) const;     // displayName in the rarity colour
    std::vector<Text> tooltip(const ItemStack& s) const;
    bool foil(const ItemStack& s) const { return s.glint >= 0 ? s.glint == 1 : s.enchantments > 0; }

    // Protocol. Item stacks: ItemStack.OPTIONAL_STREAM_CODEC / OPTIONAL_UNTRUSTED (delimited components).
    bool read(net::Reader& r, ItemStack& out) const;
    void write(net::Writer& w, const ItemStack& s, bool delimited) const;
    void writeHashed(net::Writer& w, const ItemStack& s) const;  // HashedStack (component hashes unknown: 0)
    bool readTemplate(net::Reader& r, ItemStack& out) const;     // ItemStackTemplate (item, count, patch)

private:
    bool readPatch(net::Reader& r, ItemStack& out, int depth) const;
    bool skipValue(net::Reader& r, int type, int depth) const;
    bool readTemplate(net::Reader& r, int depth) const;  // ItemStackTemplate

    std::vector<std::string> names_;
    std::unordered_map<std::string, int> ids_;
    std::vector<std::string> componentNames_;
    std::unordered_map<std::string, int> componentIds_;
    std::vector<std::string> potions_;
    std::vector<bool> blockItem_;
    const Lang* lang_ = nullptr;
};

}  // namespace ui
