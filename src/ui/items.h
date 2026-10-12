#pragma once
// Items: the item registry (network ids from the game's reports), item stacks as the protocol carries them
// (count, item, data component patch kept as raw bytes so it can be sent back) and display names.
#include "core/common.h"
#include "net/protocol.h"
#include "ui/text.h"

#include <unordered_map>

class GameReports;
class BlockRegistry;
namespace net {
class Client;
}

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
    std::vector<std::pair<int, int>> enchantList, storedEnchants;  // (enchantment network id, level)
    std::vector<std::pair<int, int>> stewEffects;                  // (mob_effect id, duration ticks)
    int instrument = -1, painting = -1;                            // registry network ids
    std::vector<int> hiddenTooltip;                                // tooltip_display hidden component types
    bool hideTooltip = false;                                      // tooltip_display hide_tooltip
    // Gameplay components the patch sets (otherwise the item's defaults apply).
    int foodNutrition = -1;  // food: -1 not patched
    float foodSaturation = 0;
    bool foodAlwaysEdible = false;
    float consumeSeconds = -1;  // consumable: -1 not patched
    int consumeAnimation = 0;   // ItemUseAnimation id
    float cooldownSeconds = -1;  // use_cooldown: -1 not patched
    std::string cooldownGroup;   // "" = the item
    int equipSlot = -1;          // equippable: EquipmentSlot id (0 main hand, 1..4 feet..head, 5 off hand, 6 body, 7 saddle)
    std::string equipAsset;      // equippable asset id ("minecraft:diamond"), "" none
    struct AttributeModifier {
        int attribute = -1;  // attribute registry id
        std::string id;
        double amount = 0;
        int operation = 0, slot = 0;  // AttributeModifier.Operation, EquipmentSlotGroup ids
    };
    bool hasAttributeModifiers = false;  // attribute_modifiers patched (an empty list removes the defaults)
    std::vector<AttributeModifier> attributeModifiers;
    int bundleSelected = -1;  // BundleContents.selectedItem: client-side state (not part of the component), -1 none

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
    // advanced: TooltipFlag.ADVANCED (F3 + H): durability and the item id. creative: TooltipFlag.isCreative (creative
    // inventory item slots), which ignores tooltip_display's hide_tooltip.
    std::vector<Text> tooltip(const ItemStack& s, bool advanced = false, bool creative = false) const;
    bool foil(const ItemStack& s) const { return s.glint >= 0 ? s.glint == 1 : s.enchantments > 0; }
    // ItemCooldowns.getCooldownGroup: the use_cooldown component's group, else the item's name.
    std::string cooldownGroup(const ItemStack& s) const;

    // Data-driven registries the server synced (net::Client::registryEntries): names and tooltip data of
    // enchantments, painting variants, instruments; banner pattern ids. Set once the client is in play.
    struct RegistryInfo {
        std::string id;              // "minecraft:sharpness"
        Text description;            // enchantment / instrument description
        int maxLevel = 1;            // enchantment max_level
        int width = 0, height = 0;   // painting size (blocks)
        bool hasTitle = false, hasAuthor = false;
        Text title, author;          // painting
    };
    void setRegistries(const net::Client& client);
    bool registriesSet() const { return registriesSet_; }
    const std::vector<RegistryInfo>& enchantments() const { return enchantments_; }
    const std::vector<RegistryInfo>& paintings() const { return paintings_; }
    const std::vector<RegistryInfo>& instruments() const { return instruments_; }
    const std::vector<std::string>& bannerPatterns() const { return bannerPatterns_; }
    const std::vector<std::string>& trimMaterials() const { return trimMaterials_; }  // trim_material ids
    const std::string& potionName(int id) const { return entry(potions_, id); }       // "minecraft:healing", "" unknown
    const std::string& mobEffectName(int id) const { return entry(mobEffects_, id); }
    int potionId(std::string_view name) const {  // -1 if unknown
        const std::string n = name.find(':') == std::string_view::npos ? "minecraft:" + std::string(name) : std::string(name);
        for (size_t i = 0; i < potions_.size(); ++i)
            if (potions_[i] == n) return int(i);
        return -1;
    }
    int mobEffectId(std::string_view name) const;  // static registry protocol id, -1 if unknown

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
    std::vector<std::string> mobEffects_;
    std::vector<std::string> consumeEffects_;  // consume_effect_type names by network id
    std::unordered_map<std::string, int> mobEffectIds_;
    std::vector<RegistryInfo> enchantments_, paintings_, instruments_;
    std::vector<std::string> bannerPatterns_, trimMaterials_;
    static const std::string& entry(const std::vector<std::string>& v, int id) {
        static const std::string kNone;
        return id >= 0 && size_t(id) < v.size() ? v[size_t(id)] : kNone;
    }
    bool registriesSet_ = false;
    std::vector<bool> blockItem_;
    const Lang* lang_ = nullptr;
};

}  // namespace ui
