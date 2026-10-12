#pragma once
// Item model properties, shared by the GUI icons (ui/icons.cpp) and the 3D item models (entity/item_models.cpp) so
// both pick the same models: ItemModelResolver's view of a stack (the item_model component), the condition / select /
// range_dispatch properties (ConditionalItemModelProperties, SelectItemModelProperties,
// RangeSelectItemModelProperties), the tint sources (ItemTintSources) and the data components special model
// renderers read (banner patterns, profiles, pot decorations, bundle contents ...), decoded from the stack's patch.
//
// The client level and the local player's state the properties read live in itemWorld(), filled by the game (sky
// clock, spawn / death positions from packets, keys); the holder of a stack is described per call (ItemOwner).
#include "ui/items.h"
#include "ui/resources.h"

#include <json/json.hpp>

#include <functional>

namespace net {
class Client;
}

namespace ui {

// ItemDisplayContext (ids and serialized names in that order).
enum class ItemDisplay : u8 { None, ThirdLeft, ThirdRight, FirstLeft, FirstRight, Head, Gui, Ground, Fixed, OnShelf };
const char* itemDisplayName(ItemDisplay d);

// ItemOwner: the entity holding / showing the stack, and what its state says about this very stack (the game compares
// stack identity: getUseItem() == stack, getSelectedItem() == stack, ...).
struct ItemOwner {
    bool present = false;
    bool living = false, player = false, local = false;  // LivingEntity, Player, the local player
    bool viewEntity = false;                              // the camera entity
    int id = 0;
    std::string type;    // entity type id ("minecraft:player")
    double pos[3] = {};  // position
    float yaw = 0;       // getVisualRotationYInDegrees
    int mainArm = 1;     // HumanoidArm: 0 left, 1 right
    bool usingThis = false;   // isUsingItem && getUseItem() == stack
    float useTicks = 0;       // UseDuration.useDuration (ticks used)
    float remaining = 0;      // getUseItemRemainingTicks
    float chargeDuration = 25;  // CrossbowItem.getChargeDuration
    bool fishing = false;     // FishingRodCast: a hook out and this stack in the rod's arm
    bool selected = false;    // getInventory().getSelectedItem() == stack
    bool carried = false;     // containerMenu.getCarried() == stack
    float cooldown = 0;       // getCooldowns().getCooldownPercent(stack)
    int teamColor = -1;       // RGB of the owner's team colour, -1 none
};

// ClientLevel and local client state.
struct ItemWorld {
    bool present = false;                    // a level is loaded
    std::string dimension = "minecraft:overworld";
    i64 gameTime = 0;                        // level game time (ticks)
    float sunAngle = 0;                      // EnvironmentAttributes.SUN_ANGLE (degrees)
    int moonPhase = 0;                       // MoonPhase index
    bool ticking = true;                     // tickRateManager().runsNormally()
    bool hasSpawn = false;                   // level.getRespawnData() (set_default_spawn_position)
    std::string spawnDimension;
    int spawn[3] = {};
    bool hasDeath = false;                   // the local player's last death location (login / respawn)
    std::string deathDimension;
    int death[3] = {};
    bool shiftDown = false;                  // Minecraft.hasShiftDown
    std::function<bool(std::string_view)> keyDown;  // KeyMapping.isDown by name ("key.use")
    int localId = -1;                        // the local player's entity id (login)
    // The local player as an item owner (filled by the entity visuals each frame; present once in a level), and the
    // inventory menu slots its per-stack state refers to: the selected hotbar slot, the stack in use and the hand
    // holding a cast fishing rod (36 + selected / 45, -1 none); ItemCooldowns by stack.
    ItemOwner player;
    int selectedSlot = -1, useSlot = -1, fishingSlot = -1;
    float useTicks = 0, useRemaining = 0, chargeDuration = 25;
    std::function<float(const ItemStack&)> cooldown;
};
ItemWorld& itemWorld();
// Dimension, spawn and death positions from the play packets (before Client::start); applied by updateItemWorld (main
// thread, once per frame).
void attachItemWorld(net::Client& client);
void updateItemWorld();
// The game's item component reports (default components per item: has_component, component selects).
void setItemComponentReports(const fs::path& dir);
// A texture of the item / block atlases by id ("minecraft:item/stick"): its png, else a sprite the atlases generate
// (atlases/*.json paletted_permutations: armour trim overlays recoloured with a material palette).
bool atlasTextureImage(const Resources& res, const std::string& id, Image& out);

struct ItemContext {
    ItemDisplay display = ItemDisplay::Gui;
    ItemOwner owner;
    int seed = 0;
};

// --- data components ----------------------------------------------------------------------------------------
// The patch's encoded value of a component ("map_id"), nullptr when the patch does not set it.
const std::string* findComponent(const Items& items, const ItemStack& s, std::string_view name);
// DataComponentHolder.has: patched, or a default component of the item (and not removed). ignoreDefault: hasNonDefault.
bool hasComponent(const Items& items, const ItemStack& s, std::string_view name, bool ignoreDefault = false);
// The item definition id (item_model component, else the item's default model id): "minecraft:compass".
std::string itemModelId(const Items& items, const ItemStack& s);

struct CustomModelData {
    std::vector<float> floats;
    std::vector<u8> flags;
    std::vector<std::string> strings;
    std::vector<i32> colors;
};
bool customModelData(const Items& items, const ItemStack& s, CustomModelData& out);
bool bundleContents(const Items& items, const ItemStack& s, std::vector<ItemStack>& out);
float bundleFullness(const Items& items, const ItemStack& s);  // BundleItem.getFullnessDisplay
// BundleItem.getSelectedItem (the selected index is client state: ItemStack::bundleSelected).
bool bundleSelectedItem(const Items& items, const ItemStack& s, ItemStack& out);
struct BannerLayer {
    std::string pattern;  // BannerPattern asset id ("minecraft:creeper")
    int dye = 0;          // DyeColor id
};
std::vector<BannerLayer> bannerPatterns(const Items& items, const ItemStack& s);
int baseColor(const Items& items, const ItemStack& s);  // base_color DyeColor id, -1 none
struct ProfileInfo {
    std::string name;
    u8 uuid[16] = {};
    bool hasId = false;
    std::string textures;  // the "textures" property (base64 JSON with the skin URL), "" none
    std::string skinTexture;  // skin patch body texture ("entity/player/wide/zuri"), "" none
    int model = -1;           // skin patch model: 0 wide, 1 slim, -1 none
};
bool profile(const Items& items, const ItemStack& s, ProfileInfo& out);
void potDecorations(const Items& items, const ItemStack& s, int out[4]);  // back, left, right, front item ids (-1)
u32 potionColor(const Items& items, const ItemStack& s, u32 fallback);    // PotionContents.getColorOr (RGB)
std::vector<u32> fireworkColors(const Items& items, const ItemStack& s);  // firework_explosion colours (RGB)
std::string trimMaterial(const Items& items, const ItemStack& s);        // trim material key, "" none / inline
int mapId(const Items& items, const ItemStack& s);                       // map_id, -1 none
// An item stack compound as block entities keep it ({id, count, components}: the NBT payload without its type byte):
// the item and the components the item models read, in their network encoding.
bool itemStackFromNbt(const Items& items, const std::string& compound, ItemStack& out);
// DyeColor: textureDiffuseColor (RGB) and serialized name by id.
u32 dyeDiffuse(int dye);
const char* dyeName(int dye);

// --- item model properties ----------------------------------------------------------------------------------
// condition / select / range_dispatch node: the branch to evaluate for this stack and context (nullptr: the missing
// model, nothing drawn). Stateful properties (compass / clock needle wobble) keep their state per node like the game's
// baked models.
const nlohmann::json* chooseItemModel(const Items& items, const nlohmann::json& node, const ItemStack& s,
                                      const ItemContext& c);
// One entry of a "model" node's tints: ARGB.
u32 itemTint(const Items& items, const nlohmann::json& tint, const ItemStack& s, const ItemContext& c,
             const std::function<u32(bool foliage, float temperature, float downfall)>& colormap);

}  // namespace ui
