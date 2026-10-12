#pragma once
// The local player's server-driven gameplay state and the Game hooks built on it:
//  - mob effects and attribute values of our own entity (update_mob_effect / remove_mob_effect / update_attributes),
//    read by the mining speed (Player.getDestroySpeed) and movement;
//  - block cracks of other players (block_destruction -> Game::destroyBlockProgress);
//  - Game::destroyProgress (BlockBehaviour.getDestroyProgress with the held item's Tool component, efficiency,
//    haste / conduit power / mining fatigue, block_break_speed, submerged_mining_speed, the not-on-ground penalty),
//    Game::heldIdentity and Game::canDestroyInCreative;
//  - the local player's item use (Game::use): predicted on the use press like Item.use -> startUsingItem, kept in
//    step with the server's living-entity flags (LocalPlayer.onSyncedDataUpdated), stopped when the hand changes,
//    eating / drinking particles and sounds (Consumable.emitParticlesAndSounds);
//  - riding (game/riding.h): set_passengers, the server's vehicle corrections (move_vehicle), vehicle attributes.
// Packets are peeked on the network thread and applied on the main thread in frame().
#include "core/common.h"
#include "game/item_rules.h"

#include <memory>

#include <atomic>
#include <functional>
#include <map>
#include <random>
#include <mutex>
#include <unordered_map>

class Game;
class World;
class BlockRegistry;
class GameReports;
namespace net {
class Client;
}
namespace ui {
class Ui;
struct ItemStack;
}  // namespace ui
namespace ent {
class EntityWorld;
}
class Riding;

class Gameplay {
public:
    Gameplay(World& world, BlockRegistry& registry, const GameReports& reports);
    ~Gameplay();
    // Block hardness / tags (cached Java dump) and item components (the data generator's reports); any thread.
    bool load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir,
              const fs::path& componentReports);
    void attach(net::Client& client);       // before Client::start
    void setPlayer(Game& game, ui::Ui& ui);  // installs the Game hooks
    void setEntities(ent::EntityWorld* entities) { entities_ = entities; }  // before setPlayer (riding)
    void frame();                            // main thread, before Game::frame

    int entityId() const { return entityId_; }
    // Mob effect amplifier of the local player (0 = level I), -1 when it does not have it ("haste").
    int effect(std::string_view name) const;
    double attribute(std::string_view name, double fallback) const;  // the server's value, else fallback
    const ui::ItemStack* mainHand() const;
    const ui::ItemStack* offHand() const;
    // CrossbowItem.isCharged / the charge_type item model property: 0 none, 1 arrow, 2 firework rocket.
    int chargeType(const ui::ItemStack* s) const;
    int food() const { return food_; }
    // Attribute of any entity the server reported (vehicles: movement_speed, jump_strength, max_health...).
    double entityAttribute(int entity, std::string_view name, double fallback) const;
    Riding* riding() { return riding_.get(); }
    World& world() { return world_; }
    BlockRegistry& registry() { return registry_; }
    std::string itemName(int item) const;  // "minecraft:bow", "" if unknown
    ItemRules& rules() { return rules_; }
    // LivingEntity.onKineticHit for the local player (the server's KINETIC_HIT entity event): hit feedback for the
    // spear animations and the kinetic_weapon's hit sound, at most every 10 ticks.
    void kineticHit();
    // Entity.isEyeInFluid(WATER) for the local player (updateFluidOnEyes: eye y - 1/9 below the fluid surface).
    bool eyeInWater() const;

private:
    float destroyProgress(int x, int y, int z, u16 state);
    // The held stack's Tool: its own minecraft:tool component (added / removed), else the item's default.
    const ItemRules::Tool* heldTool(const ui::ItemStack* s);
    // Player.blockActionRestricted in adventure mode: the held stack's can_break (AdventureModePredicate) allows it.
    bool adventureMayBreak(int x, int y, int z);
    // Item use prediction (Game::startUseItem): ItemStack.use / useOn results per hand, buckets, riptide.
    int predictItemUse(int hand, const struct RayHit* target);
    int predictUseOn(int hand, const struct RayHit& hit);
    int predictBucket(int hand, const std::string& name);
    void releaseUse();
    ItemRules::Use stackUse(const ui::ItemStack& s);  // the item's use with the stack's component patch
    int equipSlot(const ui::ItemStack& s, bool& allowed, bool* swappable);  // getEquipmentSlotForItem
    float riptideStrength(const ui::ItemStack& s) const;
    bool rainingAt(int x, int y, int z) const;
    bool inWaterOrRain() const;
    int componentType(const char* name);  // data component type id by short name ("consumable")
    void heldWeapon();                    // Game's held weapon fields from the main hand stack
    ItemRules::Components handComponents(const ui::ItemStack* s);  // swing / kinetic components with the stack's patch
    std::string weaponSound(const ui::ItemStack& s, const char* type);  // kinetic_weapon / piercing_weapon sound
    bool onCooldown(const ui::ItemStack* s) const;
    bool startUsing(int hand);  // LivingEntity.startUsingItem
    void tickUse();
    bool hasProjectile() const;  // Player.getProjectile: arrows anywhere in the inventory (creative: always)
    float randf() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng_); }

    World& world_;
    BlockRegistry& registry_;
    const GameReports& reports_;
    ItemRules rules_;
    bool rulesOk_ = false;
    Game* game_ = nullptr;
    ui::Ui* ui_ = nullptr;
    std::vector<std::string> itemNames_, attributeNames_, effectNames_;  // by network id

    int entityId_ = INT32_MIN;
    std::atomic<int> netEntityId_{INT32_MIN};  // the same, for the network thread's peeks
    int food_ = 20;
    float health_ = 20;
    int lastMainItem_ = -2;
    bool verbose_ = false;
    int loggedState_ = -1;
    int chargedProjectilesType_ = -1;  // data component type id of minecraft:charged_projectiles
    int toolType_ = -1, canBreakType_ = -1;  // data component type ids of minecraft:tool / minecraft:can_break
    std::vector<std::string> blockNames_;     // block registry by network id
    std::vector<std::string> soundNames_;     // sound_event registry by network id
    std::map<std::string, int, std::less<>> componentTypes_;
    int playerEntityType_ = -1;
    float rainLevel_ = 0;  // ClientLevel rain level (game events)
    std::unordered_map<std::string, ItemRules::Tool> toolCache_;  // decoded tool components by their bytes
    std::mt19937 rng_{0x5EEDu};
    std::map<std::string, int, std::less<>> effects_;        // name -> amplifier
    std::map<std::string, double, std::less<>> attributes_;  // name -> value
    std::unordered_map<int, std::map<std::string, double, std::less<>>> entityAttributes_;  // other entities
    ent::EntityWorld* entities_ = nullptr;
    net::Client* client_ = nullptr;
    std::unique_ptr<Riding> riding_;

    std::mutex mutex_;
    std::vector<std::function<void()>> pending_;  // decoded on the network thread
    void post(std::function<void()> fn);
};
