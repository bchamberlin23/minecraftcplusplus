#pragma once
// Gameplay rules that live in the game's code rather than its data files, taken once per version from the game
// itself (a small Java program against the client jar, like assets/block_shapes) and cached:
//  - every block's destroy time (BlockBehaviour destroySpeed, -1 = unbreakable) and requiresCorrectToolForDrops;
//  - every block's piston push reaction and whether it has a block entity (PistonBaseBlock.isPushable);
//  - every block tag, resolved to block names.
// Items come from the data generator's item component reports (reports/minecraft/components/item/<item>.json):
//  - the default Tool component (rules, default mining speed, damage per block, can_destroy_blocks_in_creative);
//  - the use animation (ItemUseAnimation) and use duration in ticks (Item.getUseAnimation / getUseDuration).
#include "core/common.h"

#include <json/json.hpp>

#include <array>
#include <unordered_map>

class ItemRules {
public:
    bool load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir,
              const fs::path& componentReports);
    bool loaded() const { return !blocks_.empty(); }

    struct Hardness {
        float destroyTime = 0;      // seconds-ish units of BlockBehaviour.destroySpeed; -1: unbreakable
        bool requiresTool = false;  // requiresCorrectToolForDrops
    };
    // By full state key ("minecraft:stone", "minecraft:oak_log[axis=y]").
    Hardness hardness(std::string_view stateKey) const;

    enum class PushReaction : u8 { PushPull, Push, Popped, Immoveable, IgnoreEntity };
    struct Push {
        u8 reaction = 0;  // PushReaction
        bool blockEntity = false;
    };
    Push push(std::string_view blockName) const;  // "minecraft:obsidian" (no properties)

    struct Rule {
        float speed = -1;          // < 0: none
        i8 correct = -1;           // -1 none, 0 / 1
        std::vector<u32> blocks;   // block indices, sorted
    };
    struct Tool {
        float defaultSpeed = 1;
        int damagePerBlock = 1;
        bool canDestroyInCreative = true;
        std::vector<Rule> rules;
    };
    const Tool* tool(std::string_view itemName);  // "minecraft:wooden_pickaxe"; nullptr: no tool component
    // Block indices of block names and "#tag" references (the indices Tool rules use).
    std::vector<u32> blocks(const std::vector<std::string>& namesOrTags) const;
    bool blockIn(const std::vector<u32>& set, std::string_view blockName) const;
    // Tool.getMiningSpeed / isCorrectForDrops for a block (name without properties).
    float miningSpeed(const Tool& t, std::string_view blockName) const;
    bool correctForDrops(const Tool& t, std::string_view blockName) const;

    enum class Anim : u8 { None, Eat, Drink, Block, Bow, Trident, Crossbow, Spyglass, TootHorn, Brush, Bundle, Spear };
    struct Use {
        Anim anim = Anim::None;
        int duration = 0;  // ticks (72000 for bows, shields...)
        // use_effects: movement while using.
        float speed = 0.2f;
        bool canSprint = false;
        // Consumables: the eat / drink sound and particles, food (Player.canEat unless can_always_eat).
        bool consumable = false, particles = true, food = false, alwaysEat = false;
        std::string sound;  // "minecraft:entity.generic.eat"
    };
    Use use(std::string_view itemName);
    bool glider(std::string_view itemName);  // the minecraft:glider component (elytra)
    // Other default components the client's use / equipment rules read (stack patches override them).
    struct Components {
        bool consumable = false, blocksAttacks = false, kinetic = false;
        bool classUse = false;  // the item class decides the use (bows, crossbows, tridents, spyglasses, horns...)
        int equipSlot = -1;     // equippable: EquipmentSlot id (1 feet .. 4 head, 5 off hand, 6 body, 7 saddle), -1 none
        bool swappable = true, equipForPlayer = true;  // equippable swappable / allowed_entities includes players
        float cooldownSeconds = -1;  // use_cooldown, -1 none
        std::string cooldownGroup;
        // Weapons: piercing_weapon (attacks stab), minimum_attack_charge, attack_range (min / max reach, creative
        // min / max, hitbox margin, mob factor), attack_animation duration.
        bool piercing = false, hasAttackRange = false;
        float minimumAttackCharge = 0;
        float attackRange[6] = {0, 3, 0, 5, 0.3f, 1};
        int attackSwingTicks = 6;
    };
    const Components& components(std::string_view itemName);
    bool inTag(std::string_view tag, std::string_view blockName) const;  // "minecraft:climbable"

    // EntityType dimensions and attachment points (EntityAttachments at yRot 0).
    struct EntityDims {
        float width = 0.6f, height = 1.8f, eye = 1.62f;
        std::vector<std::array<double, 3>> seats;  // PASSENGER points by passenger index
        std::array<double, 3> vehicle{};           // VEHICLE point: where this entity sits on what it rides
    };
    const EntityDims* entity(std::string_view typeName) const;  // "minecraft:oak_boat"

private:
    bool loadImpl(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir,
                  const fs::path& componentReports);
    u32 blockIndex(std::string_view name) const;  // UINT32_MAX if unknown
    struct ItemInfo {
        bool hasTool = false, glider = false;
        Tool tool;
        Use use;
        Components components;
    };
    const ItemInfo& item(std::string_view itemName);  // read from the component report on first use
    std::vector<u32> blockSet(const nlohmann::json& holderSet) const;

    fs::path componentReports_;
    std::unordered_map<std::string, std::vector<u32>> tags_;  // sorted block indices
    std::unordered_map<std::string, ItemInfo> items_;

    std::unordered_map<std::string, EntityDims> entities_;
    std::unordered_map<std::string, Hardness> blocks_, states_;
    std::unordered_map<std::string, Push> push_;
    std::unordered_map<std::string, u32> blockIds_;
};
