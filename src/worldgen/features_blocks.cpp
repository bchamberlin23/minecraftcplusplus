// Block facts for features: which block a state belongs to, tags and holder sets, fluid states, the
// shape-derived facts (isSolid, isFaceSturdy, solid render, face occlusion) and BlockState.canSurvive
// for the blocks decoration places. Shapes come from the block models; the name lists below are the
// block properties the game sets in Blocks.java (noCollision, forceSolidOn/Off, noOcclusion).
#include "worldgen/features_internal.h"

#include "assets/assets.h"

#include <algorithm>
#include <unordered_set>

namespace wg {

namespace {

constexpr const char* kNoCollision[] = {
    "acacia_hanging_sign", "acacia_pressure_plate", "acacia_sapling", "acacia_sign", "acacia_wall_hanging_sign",
    "acacia_wall_sign", "activator_rail", "air", "allium", "attached_melon_stem", "attached_pumpkin_stem",
    "azure_bluet", "bamboo_hanging_sign", "bamboo_pressure_plate", "bamboo_sapling", "bamboo_sign",
    "bamboo_wall_hanging_sign", "bamboo_wall_sign", "beetroots", "big_dripleaf_stem", "birch_hanging_sign",
    "birch_pressure_plate", "birch_sapling", "birch_sign", "birch_wall_hanging_sign", "birch_wall_sign",
    "blue_orchid", "brain_coral", "brain_coral_fan", "brain_coral_wall_fan", "brown_mushroom", "bubble_column",
    "bubble_coral", "bubble_coral_fan", "bubble_coral_wall_fan", "bush", "cactus_flower", "carrots", "cave_air",
    "cave_vines", "cave_vines_plant", "cherry_hanging_sign", "cherry_pressure_plate", "cherry_sapling",
    "cherry_sign", "cherry_wall_hanging_sign", "cherry_wall_sign", "closed_eyeblossom", "cobweb", "copper_torch",
    "copper_wall_torch", "cornflower", "crimson_fungus", "crimson_hanging_sign", "crimson_pressure_plate",
    "crimson_roots", "crimson_sign", "crimson_wall_hanging_sign", "crimson_wall_sign", "dandelion",
    "dark_oak_hanging_sign", "dark_oak_pressure_plate", "dark_oak_sapling", "dark_oak_sign",
    "dark_oak_wall_hanging_sign", "dark_oak_wall_sign", "dead_brain_coral", "dead_brain_coral_fan",
    "dead_brain_coral_wall_fan", "dead_bubble_coral", "dead_bubble_coral_fan", "dead_bubble_coral_wall_fan",
    "dead_bush", "dead_fire_coral", "dead_fire_coral_fan", "dead_fire_coral_wall_fan", "dead_horn_coral",
    "dead_horn_coral_fan", "dead_horn_coral_wall_fan", "dead_tube_coral", "dead_tube_coral_fan",
    "dead_tube_coral_wall_fan", "detector_rail", "end_gateway", "end_portal", "fern", "fire", "fire_coral",
    "fire_coral_fan", "fire_coral_wall_fan", "firefly_bush", "frogspawn", "glow_lichen", "golden_dandelion",
    "hanging_roots", "heavy_weighted_pressure_plate", "horn_coral", "horn_coral_fan", "horn_coral_wall_fan",
    "jungle_hanging_sign", "jungle_pressure_plate", "jungle_sapling", "jungle_sign", "jungle_wall_hanging_sign",
    "jungle_wall_sign", "kelp", "kelp_plant", "large_fern", "lava", "leaf_litter", "lever",
    "light_weighted_pressure_plate", "lilac", "lily_of_the_valley", "mangrove_hanging_sign",
    "mangrove_pressure_plate", "mangrove_propagule", "mangrove_sign", "mangrove_wall_hanging_sign",
    "mangrove_wall_sign", "melon_stem", "nether_portal", "nether_sprouts", "nether_wart", "oak_hanging_sign",
    "oak_pressure_plate", "oak_sapling", "oak_sign", "oak_wall_hanging_sign", "oak_wall_sign", "open_eyeblossom",
    "orange_tulip", "oxeye_daisy", "pale_hanging_moss", "pale_oak_hanging_sign", "pale_oak_pressure_plate",
    "pale_oak_sapling", "pale_oak_sign", "pale_oak_wall_hanging_sign", "pale_oak_wall_sign", "peony", "pink_petals",
    "pink_tulip", "pitcher_crop", "pitcher_plant", "polished_blackstone_pressure_plate", "poplar_hanging_sign",
    "poplar_pressure_plate", "poplar_sapling", "poplar_sign", "poplar_wall_hanging_sign", "poplar_wall_sign",
    "poppy", "potatoes", "powered_rail", "pumpkin_stem", "rail", "red_mushroom", "red_shrub", "red_tulip",
    "redstone_torch", "redstone_wall_torch", "redstone_wire", "resin_clump", "rose_bush", "scaffolding",
    "sculk_vein", "seagrass", "short_dry_grass", "short_grass", "small_dripleaf", "soul_fire", "soul_torch",
    "soul_wall_torch", "spore_blossom", "spruce_hanging_sign", "spruce_pressure_plate", "spruce_sapling",
    "spruce_sign", "spruce_wall_hanging_sign", "spruce_wall_sign", "stone_pressure_plate", "structure_void",
    "sugar_cane", "sunflower", "sweet_berry_bush", "tall_dry_grass", "tall_grass", "tall_seagrass", "torch",
    "torchflower", "torchflower_crop", "tripwire", "tripwire_hook", "tube_coral", "tube_coral_fan",
    "tube_coral_wall_fan", "twisting_vines", "twisting_vines_plant", "vine", "void_air", "wall_torch",
    "warped_fungus", "warped_hanging_sign", "warped_pressure_plate", "warped_roots", "warped_sign",
    "warped_wall_hanging_sign", "warped_wall_sign", "water", "weeping_vines", "weeping_vines_plant", "wheat",
    "white_tulip", "wildflowers", "wither_rose",
};
constexpr const char* kForceSolidOn[] = {
    "acacia_fence_gate", "acacia_hanging_sign", "acacia_pressure_plate", "acacia_sign", "acacia_wall_hanging_sign",
    "acacia_wall_sign", "amethyst_cluster", "bamboo", "bamboo_fence_gate", "bamboo_hanging_sign",
    "bamboo_pressure_plate", "bamboo_sapling", "bamboo_sign", "bamboo_wall_hanging_sign", "bamboo_wall_sign",
    "bell", "birch_fence_gate", "birch_hanging_sign", "birch_pressure_plate", "birch_sign",
    "birch_wall_hanging_sign", "birch_wall_sign", "cake", "candle_cake", "cherry_fence_gate", "cherry_hanging_sign",
    "cherry_pressure_plate", "cherry_sign", "cherry_wall_hanging_sign", "cherry_wall_sign", "cobweb", "conduit",
    "crimson_fence_gate", "crimson_hanging_sign", "crimson_pressure_plate", "crimson_sign",
    "crimson_wall_hanging_sign", "crimson_wall_sign", "dark_oak_fence_gate", "dark_oak_hanging_sign",
    "dark_oak_pressure_plate", "dark_oak_sign", "dark_oak_wall_hanging_sign", "dark_oak_wall_sign",
    "dead_brain_coral", "dead_brain_coral_block", "dead_brain_coral_fan", "dead_brain_coral_wall_fan",
    "dead_bubble_coral", "dead_bubble_coral_block", "dead_bubble_coral_fan", "dead_bubble_coral_wall_fan",
    "dead_fire_coral", "dead_fire_coral_block", "dead_fire_coral_fan", "dead_fire_coral_wall_fan",
    "dead_horn_coral", "dead_horn_coral_block", "dead_horn_coral_fan", "dead_horn_coral_wall_fan",
    "dead_tube_coral", "dead_tube_coral_block", "dead_tube_coral_fan", "dead_tube_coral_wall_fan", "dried_ghast",
    "heavy_weighted_pressure_plate", "iron_chain", "jungle_fence_gate", "jungle_hanging_sign",
    "jungle_pressure_plate", "jungle_sign", "jungle_wall_hanging_sign", "jungle_wall_sign", "lantern",
    "large_amethyst_bud", "light_weighted_pressure_plate", "mangrove_fence_gate", "mangrove_hanging_sign",
    "mangrove_pressure_plate", "mangrove_sign", "mangrove_wall_hanging_sign", "mangrove_wall_sign",
    "medium_amethyst_bud", "moving_piston", "oak_fence", "oak_fence_gate", "oak_hanging_sign", "oak_pressure_plate",
    "oak_sign", "oak_wall_hanging_sign", "oak_wall_sign", "pale_oak_fence_gate", "pale_oak_hanging_sign",
    "pale_oak_pressure_plate", "pale_oak_sign", "pale_oak_wall_hanging_sign", "pale_oak_wall_sign",
    "pointed_dripstone", "polished_blackstone_pressure_plate", "poplar_fence_gate", "poplar_hanging_sign",
    "poplar_pressure_plate", "poplar_sign", "poplar_wall_hanging_sign", "poplar_wall_sign", "sculk_vein",
    "small_amethyst_bud", "soul_lantern", "spruce_fence_gate", "spruce_hanging_sign", "spruce_pressure_plate",
    "spruce_sign", "spruce_wall_hanging_sign", "spruce_wall_sign", "stone_pressure_plate", "sulfur_spike",
    "turtle_egg", "warped_fence_gate", "warped_hanging_sign", "warped_pressure_plate", "warped_sign",
    "warped_wall_hanging_sign", "warped_wall_sign",
};
constexpr const char* kForceSolidOff[] = {
    "azalea", "big_dripleaf", "chorus_flower", "chorus_plant", "end_rod", "flowering_azalea", "ladder", "snow",
};
constexpr const char* kNoOcclusion[] = {
    "acacia_door", "acacia_leaves", "acacia_trapdoor", "amethyst_cluster", "azalea", "azalea_leaves", "bamboo",
    "bamboo_door", "bamboo_trapdoor", "barrier", "beacon", "birch_door", "birch_leaves", "birch_trapdoor",
    "brewing_stand", "campfire", "cauldron", "cherry_door", "cherry_leaves", "cherry_trapdoor", "chorus_flower",
    "chorus_plant", "cocoa", "conduit", "creeper_head", "crimson_door", "crimson_trapdoor", "dark_oak_door",
    "dark_oak_leaves", "dark_oak_trapdoor", "decorated_pot", "dragon_egg", "dragon_head", "dried_ghast", "end_rod",
    "flowering_azalea", "flowering_azalea_leaves", "frogspawn", "frosted_ice", "glass", "glass_pane", "honey_block",
    "hopper", "ice", "iron_bars", "iron_chain", "iron_door", "iron_trapdoor", "jungle_door", "jungle_leaves",
    "jungle_trapdoor", "ladder", "lantern", "large_amethyst_bud", "lava_cauldron", "light", "lily_pad",
    "mangrove_door", "mangrove_leaves", "mangrove_roots", "mangrove_trapdoor", "medium_amethyst_bud",
    "moving_piston", "oak_door", "oak_leaves", "oak_trapdoor", "orange_poplar_leaves", "pale_moss_carpet",
    "pale_oak_door", "pale_oak_leaves", "pale_oak_trapdoor", "piglin_head", "player_head", "pointed_dripstone",
    "poplar_door", "poplar_trapdoor", "powder_snow", "powder_snow_cauldron", "red_poplar_leaves", "sea_pickle",
    "shelf_mushroom", "skeleton_skull", "slime_block", "small_amethyst_bud", "sniffer_egg", "soul_campfire",
    "soul_lantern", "spawner", "spruce_door", "spruce_leaves", "spruce_trapdoor", "straw_bed", "sulfur_spike",
    "test_instance_block", "tinted_glass", "trial_spawner", "turtle_egg", "vault", "warped_door", "warped_trapdoor",
    "water_cauldron", "wither_skeleton_skull", "yellow_poplar_leaves", "zombie_head",
};

std::string_view stripNs(std::string_view id) {
    if (id.starts_with("minecraft:")) id.remove_prefix(10);
    return id;
}
std::string withNs(std::string_view id) {
    if (!id.empty() && id[0] == '#') id.remove_prefix(1);
    return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
}

// canSurvive rules (by block class in the game).
enum Survive : u8 {
    SNone, STagBelow, SMushroom, SLilyPad, SSeagrass, STallSeagrass, SSeaPickle, SSmallDripleaf, SSporeBlossom,
    SPropagule, SCactus, SCactusFlower, SSugarCane, SCarpet, SMossyCarpet, SSturdyBelow, SCoralWall, SSnow,
    SSoulFire, SMultiface, SVine
};
struct SurviveRule {
    Survive kind = SNone;
    bool doublePlant = false;  // the upper half needs the lower half below
    const BlockBits* tag = nullptr;
};

// Face coverage of a set of boxes, sampled at 1/16 cell centres within [lo, hi] of the face.
bool faceCovered(const std::vector<Box>& boxes, int dir, float lo = 0.0f, float hi = 1.0f) {
    const int axis = dir < 2 ? 1 : dir < 4 ? 2 : 0;
    const bool positive = (dir & 1) != 0;
    const int u = axis == 0 ? 1 : 0, v = axis == 2 ? 1 : 2;
    for (int i = 0; i < 16; ++i) {
        const float cu = (i + 0.5f) / 16.0f;
        if (cu < lo || cu > hi) continue;
        for (int j = 0; j < 16; ++j) {
            const float cv = (j + 0.5f) / 16.0f;
            if (cv < lo || cv > hi) continue;
            bool hit = false;
            for (const Box& b : boxes) {
                const bool touches = positive ? b.max[axis] >= 0.9999f : b.min[axis] <= 0.0001f;
                if (touches && b.min[u] <= cu && cu <= b.max[u] && b.min[v] <= cv && cv <= b.max[v]) {
                    hit = true;
                    break;
                }
            }
            if (!hit) return false;
        }
    }
    return true;
}

bool fullCube(const std::vector<Box>& boxes) {
    for (int d = 0; d < 6; ++d)
        if (!faceCovered(boxes, d)) return false;
    return !boxes.empty();
}

const char* const kFaceNames[6] = {"down", "up", "north", "south", "west", "east"};
constexpr int kHorizontal[4] = {North, East, South, West};  // Direction.Plane.HORIZONTAL

}  // namespace

struct BlockFacts::Impl::Lists {
    std::unordered_set<std::string_view> noCollision, forceSolidOn, forceSolidOff, noOcclusion;
    std::unordered_map<std::string, SurviveRule> survive;  // short name -> rule
    std::vector<SurviveRule> rules;                         // by rule id (0 = none)
};

BlockFacts::Impl::Impl(BlockRegistry& b, const DataPack& d) : blocks(b), data(d) {
    stateBlock_ = std::make_unique<std::atomic<u16>[]>(0x10000);
    stateFacts_ = std::make_unique<std::atomic<u64>[]>(0x10000);
    survive_ = std::make_unique<std::atomic<i16>[]>(kMaxBlockKinds);
    for (int i = 0; i < 0x10000; ++i) stateBlock_[i] = 0, stateFacts_[i] = 0;
    for (int i = 0; i < kMaxBlockKinds; ++i) survive_[i] = 0;
    bAir = blockIndex("minecraft:air");
    bWater = blockIndex("minecraft:water");
    bLava = blockIndex("minecraft:lava");
    bBedrock = blockIndex("minecraft:bedrock");
    bSnow = blockIndex("minecraft:snow");
    bDirtPath = blockIndex("minecraft:dirt_path");
    bVine = blockIndex("minecraft:vine");
    bIce = blockIndex("minecraft:ice");
    bMagma = blockIndex("minecraft:magma_block");
    bSculk = blockIndex("minecraft:sculk");
    bSculkCatalyst = blockIndex("minecraft:sculk_catalyst");
    bMovingPiston = blockIndex("minecraft:moving_piston");
    bCactus = blockIndex("minecraft:cactus");
    bSugarCane = blockIndex("minecraft:sugar_cane");
    tagFire = tag("fire");
    tagReplaceable = tag("replaceable");

    auto lists = std::make_shared<Lists>();
    for (const char* n : kNoCollision) lists->noCollision.insert(n);
    for (const char* n : kForceSolidOn) lists->forceSolidOn.insert(n);
    for (const char* n : kForceSolidOff) lists->forceSolidOff.insert(n);
    for (const char* n : kNoOcclusion) lists->noOcclusion.insert(n);
    auto rule = [&](std::initializer_list<const char*> names, Survive kind, const char* tagName = nullptr,
                    bool dbl = false) {
        const SurviveRule r{kind, dbl, tagName ? tag(tagName) : nullptr};
        for (const char* n : names) lists->survive[n] = r;
    };
    const char* veg = "supports_vegetation";
    rule({"allium", "azure_bluet", "blue_orchid", "cornflower", "dandelion", "golden_dandelion", "lily_of_the_valley",
          "orange_tulip", "oxeye_daisy", "pink_tulip", "poppy", "red_tulip", "torchflower", "white_tulip", "fern",
          "short_grass", "bush", "red_shrub", "firefly_bush", "pink_petals", "wildflowers", "sweet_berry_bush",
          "closed_eyeblossom", "open_eyeblossom", "acacia_sapling", "birch_sapling", "cherry_sapling",
          "dark_oak_sapling", "jungle_sapling", "oak_sapling", "pale_oak_sapling", "poplar_sapling", "spruce_sapling"},
         STagBelow, veg);
    rule({"lilac", "peony", "rose_bush", "sunflower", "large_fern", "tall_grass", "pitcher_plant"}, STagBelow, veg, true);
    rule({"dead_bush", "short_dry_grass", "tall_dry_grass"}, STagBelow, "supports_dry_vegetation");
    rule({"azalea", "flowering_azalea"}, STagBelow, "supports_azalea");
    rule({"wither_rose"}, STagBelow, "supports_wither_rose");
    rule({"crimson_fungus"}, STagBelow, "supports_crimson_fungus");
    rule({"warped_fungus"}, STagBelow, "supports_warped_fungus");
    rule({"crimson_roots"}, STagBelow, "supports_crimson_roots");
    rule({"warped_roots"}, STagBelow, "supports_warped_roots");
    rule({"nether_sprouts"}, STagBelow, "supports_nether_sprouts");
    rule({"nether_wart"}, STagBelow, "supports_nether_wart");
    rule({"wheat", "carrots", "potatoes", "beetroots", "torchflower_crop"}, STagBelow, "supports_crops");
    rule({"pitcher_crop"}, STagBelow, "supports_crops", true);
    rule({"bamboo", "bamboo_sapling"}, STagBelow, "supports_bamboo");
    rule({"brown_mushroom", "red_mushroom"}, SMushroom, "overrides_mushroom_light_requirement");
    rule({"lily_pad"}, SLilyPad, "supports_lily_pad");
    rule({"seagrass"}, SSeagrass, "cannot_support_seagrass");
    rule({"tall_seagrass"}, STallSeagrass, "cannot_support_seagrass", true);
    rule({"sea_pickle"}, SSeaPickle);
    rule({"small_dripleaf"}, SSmallDripleaf, "supports_small_dripleaf", true);
    rule({"spore_blossom"}, SSporeBlossom, "unstable_bottom_center");
    rule({"mangrove_propagule"}, SPropagule, "supports_mangrove_propagule");
    rule({"cactus"}, SCactus, "supports_cactus");
    rule({"cactus_flower"}, SCactusFlower, "support_override_cactus_flower");
    rule({"sugar_cane"}, SSugarCane, "supports_sugar_cane");
    rule({"moss_carpet"}, SCarpet);
    rule({"pale_moss_carpet"}, SMossyCarpet);
    rule({"leaf_litter", "fire"}, SSturdyBelow);
    rule({"snow"}, SSnow, "cannot_support_snow_layer");
    rule({"soul_fire"}, SSoulFire, "soul_fire_base_blocks");
    rule({"glow_lichen", "sculk_vein", "resin_clump"}, SMultiface);
    rule({"vine"}, SVine);
    for (const char* c : {"tube", "brain", "bubble", "fire", "horn"})
        for (const char* dead : {"", "dead_"}) {
            const std::string coral = std::string(dead) + c + "_coral";
            lists->survive[coral] = {SSturdyBelow};
            lists->survive[coral + "_fan"] = {SSturdyBelow};
            lists->survive[coral + "_wall_fan"] = {SCoralWall};
        }
    lists->rules.push_back({});
    lists->rules.reserve(kMaxBlockKinds);  // never reallocates: readers index without the lock
    lists_ = lists;
}

namespace {
// Per-thread cache of name lookups: features ask for the same few names (mostly string literals) thousands of
// times per chunk from several threads, and the shared tables take a lock. Matches by content.
template <typename T>
struct NameCache {
    struct Entry {
        std::string name;
        T value;
    };
    std::vector<Entry> entries;
    const T* find(std::string_view n) const {
        for (const Entry& e : entries)
            if (e.name == n) return &e.value;
        return nullptr;
    }
    void add(std::string_view n, T v) {
        if (entries.size() < 256) entries.push_back({std::string(n), v});
    }
};
}  // namespace

u16 BlockFacts::Impl::blockIndex(std::string_view name) const {
    thread_local NameCache<u16> cache;
    thread_local const Impl* owner = nullptr;
    if (owner != this) cache.entries.clear(), owner = this;
    if (const u16* hit = cache.find(name)) return *hit;
    const u16 index = blockIndexLocked(name);
    cache.add(name, index);
    return index;
}

u16 BlockFacts::Impl::blockIndexLocked(std::string_view name) const {
    const std::string key = withNs(name);
    std::lock_guard lock(mutex_);
    if (auto it = blockIds_.find(key); it != blockIds_.end()) return it->second;
    if (blockNames_.capacity() == 0) blockNames_.reserve(kMaxBlockKinds);
    if (blockNames_.size() >= size_t(kMaxBlockKinds) - 1) {
        LOG("features: more than %d block kinds", kMaxBlockKinds);
        return u16(kMaxBlockKinds - 1);
    }
    const u16 id = u16(blockNames_.size());
    blockNames_.push_back(key);
    blockIds_.emplace(key, id);
    return id;
}

const std::string& BlockFacts::Impl::blockName(u16 index) const {
    std::lock_guard lock(mutex_);
    return blockNames_[index];
}

u16 BlockFacts::Impl::blockOf(u16 state) const {
    const u16 cached = stateBlock_[state].load(std::memory_order_relaxed);
    if (cached) return u16(cached - 1);
    const u16 index = state == 0 ? bAir : blockIndex(baseName(blocks.name(state)));
    stateBlock_[state].store(u16(index + 1), std::memory_order_relaxed);
    return index;
}

std::vector<std::string> BlockFacts::Impl::tagOrdered(std::string_view kind, std::string_view name) const {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen, visiting;
    auto expand = [&](auto&& self, const std::string& id) -> void {
        if (!visiting.insert(id).second) return;
        if (const nlohmann::json* values = data.tagValues(kind, id))
            for (const auto& v : *values) {
                std::string s = v.is_string() ? v.get<std::string>() : v.value("id", std::string());
                if (s.empty()) continue;
                if (s[0] == '#') {
                    self(self, DataPack::normalize(s));
                } else {
                    s = withNs(s);
                    if (seen.insert(s).second) out.push_back(s);
                }
            }
    };
    expand(expand, DataPack::normalize(name));
    return out;
}

const BlockBits* BlockFacts::Impl::tag(std::string_view name) const {
    thread_local NameCache<const BlockBits*> cache;
    thread_local const Impl* owner = nullptr;
    if (owner != this) cache.entries.clear(), owner = this;
    if (const BlockBits* const* hit = cache.find(name)) return *hit;
    const BlockBits* bits = tagLocked(name);
    cache.add(name, bits);
    return bits;
}

const BlockBits* BlockFacts::Impl::tagLocked(std::string_view name) const {
    const std::string key = DataPack::normalize(name);
    {
        std::lock_guard lock(mutex_);
        if (auto it = tags_.find(key); it != tags_.end()) return it->second.get();
    }
    auto bits = std::make_unique<BlockBits>();
    for (const std::string& b : tagOrdered("block", key)) bits->set(blockIndex(b));
    std::lock_guard lock(mutex_);
    auto [it, inserted] = tags_.emplace(key, std::move(bits));
    return it->second.get();
}

std::vector<std::string> BlockFacts::Impl::holderList(const nlohmann::json& j) const {
    std::vector<std::string> out;
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (!s.empty() && s[0] == '#') return tagOrdered("block", s);
        out.push_back(withNs(s));
    } else if (j.is_array()) {
        for (const auto& e : j)
            if (e.is_string()) out.push_back(withNs(e.get<std::string>()));
    }
    return out;
}

BlockBits BlockFacts::Impl::holderSet(const nlohmann::json& j) const {
    BlockBits bits;
    for (const std::string& b : holderList(j)) bits.set(blockIndex(b));
    return bits;
}

bool BlockFacts::Impl::fluidInTag(u16 state, std::string_view fluidTag) const {
    static const char* const kNames[5] = {"minecraft:empty", "minecraft:water", "minecraft:flowing_water",
                                          "minecraft:lava", "minecraft:flowing_lava"};
    const FluidType t = fluidType(state);
    if (t == FluidNone) return false;
    thread_local std::unordered_map<std::string, u8> cache;  // tag -> mask of fluid types
    const std::string key(fluidTag);
    auto it = cache.find(key);
    if (it == cache.end()) {
        u8 mask = 0;
        for (const std::string& f : tagOrdered("fluid", key))
            for (int i = 1; i < 5; ++i)
                if (f == kNames[i]) mask |= u8(1 << i);
        it = cache.emplace(key, mask).first;
    }
    return (it->second >> t) & 1;
}

u64 BlockFacts::Impl::facts(u16 state) const {
    const u64 f = stateFacts_[state].load(std::memory_order_relaxed);
    if (f & fact::Done) return f;
    const u64 v = compute(state) | fact::Done;
    stateFacts_[state].store(v, std::memory_order_relaxed);
    return v;
}

u64 BlockFacts::Impl::compute(u16 state) const {
    const Lists& L = *lists_;
    const std::string key = blocks.name(state);
    const std::string_view base = baseName(key);
    const std::string_view name = stripNs(base);
    const auto props = stateProperties(key);
    auto prop = [&](std::string_view k) -> std::string_view {
        for (const auto& [pk, pv] : props)
            if (pk == k) return pv;
        return {};
    };
    const u16 block = blockOf(state);
    u64 f = 0;
    const bool air = state == 0 || name == "air" || name == "cave_air" || name == "void_air";
    if (air) f |= fact::Air;

    // FluidState: LiquidBlock levels 1-7 flow, 8+ fall; waterlogged and water plants hold a source.
    u64 fluidType = FluidNone, amount = 0, falling = 0;
    if (name == "water" || name == "lava") {
        f |= fact::Liquid;
        const int level = std::atoi(std::string(prop("level")).c_str());
        const bool water = name == "water";
        fluidType = level == 0 ? (water ? FluidWater : FluidLava) : (water ? FluidFlowingWater : FluidFlowingLava);
        amount = level == 0 || level >= 8 ? 8 : u64(8 - level);
        falling = level >= 8;
    } else if (prop("waterlogged") == "true" || name == "kelp" || name == "kelp_plant" || name == "seagrass" ||
               name == "tall_seagrass" || name == "bubble_column") {
        fluidType = FluidWater, amount = 8;
    }
    f |= (fluidType | (amount << 3) | (falling << 7)) << fact::FluidShift;

    // Shapes: collision, support (getBlockSupportShape) and outline (getShape).
    const BlockInfo& info = blocks.infos()[state];
    std::vector<Box> collision, support, outline;
    const Box full{{0, 0, 0}, {1, 1, 1}};
    auto boxes = [&](u32 first, u8 count) {
        std::vector<Box> v;
        for (u8 i = 0; i < count; ++i) v.push_back(blocks.box(first + i));
        return v;
    };
    if (name == "snow") {
        const int layers = std::max(1, std::atoi(std::string(prop("layers")).c_str()));
        if (layers > 1) collision.push_back({{0, 0, 0}, {1, float(layers - 1) / 8.0f, 1}});
        support.push_back({{0, 0, 0}, {1, float(layers) / 8.0f, 1}});
        outline = support;
    } else if (!air && fluidType == FluidNone && info.kind == KindCube) {
        collision = support = outline = {full};
    } else if (!air && !(f & fact::Liquid)) {
        outline = info.selectionCount ? boxes(info.selectionFirst, info.selectionCount)
                                      : boxes(info.collisionFirst, info.collisionCount);
        if (!L.noCollision.count(name))
            collision = info.collisionCount ? boxes(info.collisionFirst, info.collisionCount) : outline;
        support = collision;
    }
    if (L.noCollision.count(name)) collision.clear(), support.clear();
    if (tag("leaves")->test(block)) support.clear();  // LeavesBlock.getBlockSupportShape

    for (int d = 0; d < 6; ++d) {
        if (!support.empty() && faceCovered(support, d)) f |= 1ull << (fact::SturdyShift + d);
        if (!collision.empty() && faceCovered(collision, d)) f |= 1ull << (fact::CollisionFullShift + d);
    }
    if (!support.empty() && faceCovered(support, Down, 7.0f / 16.0f, 9.0f / 16.0f)) f |= fact::CenterDown;
    if (!support.empty() && faceCovered(support, Up, 7.0f / 16.0f, 9.0f / 16.0f)) f |= fact::CenterUp;
    for (const Box& b : collision)
        if (b.max[1] >= 0.9999f && b.max[0] > b.min[0] && b.max[2] > b.min[2]) f |= fact::CollisionUpAny;

    const bool canOcclude = !air && !(f & fact::Liquid) && !L.noOcclusion.count(name);
    if (canOcclude && !outline.empty()) {
        if (fullCube(outline)) f |= fact::SolidRender;
        for (int d = 0; d < 6; ++d)
            if (faceCovered(outline, d)) f |= 1ull << (fact::OcclusionFullShift + d);
    }

    // BlockStateBase.calculateSolid
    bool solid = false;
    if (L.forceSolidOn.count(name)) {
        solid = true;
    } else if (!L.forceSolidOff.count(name) && !collision.empty()) {
        float mn[3] = {1, 1, 1}, mx[3] = {0, 0, 0};
        for (const Box& b : collision)
            for (int a = 0; a < 3; ++a) mn[a] = std::min(mn[a], b.min[a]), mx[a] = std::max(mx[a], b.max[a]);
        const double size = (double(mx[0] - mn[0]) + double(mx[1] - mn[1]) + double(mx[2] - mn[2])) / 3.0;
        solid = size >= 0.7291666666666666 || mx[1] - mn[1] >= 1.0f;
    }
    if (solid) f |= fact::Solid;
    if (solid && name != "cobweb" && name != "bamboo_sapling") f |= fact::BlocksMotion;

    if (tagReplaceable->test(block)) f |= fact::Replaceable;
    if (tag("blocks_motion_in_heightmap")->test(block)) f |= fact::MotionTag;
    if (tag("blocks_motion_in_heightmap_no_leaves")->test(block)) f |= fact::MotionNoLeaves;
    if (tag("features_cannot_replace")->test(block)) f |= fact::CannotReplace;
    return f;
}

int BlockFacts::Impl::surviveRule(u16 block) const {
    const i16 cached = survive_[block].load(std::memory_order_acquire);
    if (cached) return cached - 1;
    Lists& L = const_cast<Lists&>(*lists_);
    int id = 0;
    const std::string name(stripNs(blockName(block)));
    {
        std::lock_guard lock(mutex_);
        auto it = L.survive.find(name);
        if (it == L.survive.end() && name.ends_with("_carpet") && name != "pale_moss_carpet")
            it = L.survive.find("moss_carpet");  // wool carpets are CarpetBlocks too
        if (it != L.survive.end() && L.rules.size() < L.rules.capacity()) {
            L.rules.push_back(it->second);
            id = int(L.rules.size() - 1);
        }
    }
    survive_[block].store(i16(id + 1), std::memory_order_release);
    return id;
}

bool BlockFacts::Impl::canSurvive(const GenRegion& level, u16 state, const BlockPos& p) const {
    const u16 block = blockOf(state);
    const int ruleId = surviveRule(block);
    if (ruleId == 0) return true;
    const SurviveRule rule = lists_->rules[size_t(ruleId)];
    const BlockPos belowPos = p.offset(0, -1, 0);
    const u16 below = level.block(belowPos);
    if (rule.doublePlant && blocks.property(state, "half") == "upper")
        return is(below, block) && blocks.property(below, "half") == "lower";
    auto inTag = [&](u16 s, const char* t) { return tag(t)->test(blockOf(s)); };
    switch (rule.kind) {
    case SNone: return true;
    case STagBelow: return rule.tag->test(blockOf(below));
    case SMushroom:
        // The light engine has no data for chunks being decorated: getRawBrightness reads 0 there.
        return rule.tag->test(blockOf(below)) || has(below, fact::SolidRender);
    case SLilyPad:
        return (fluidInTag(below, "supports_lily_pad") || rule.tag->test(blockOf(below))) &&
               fluidType(level.block(p)) == FluidNone;
    case SSeagrass: return sturdy(below, Up) && !rule.tag->test(blockOf(below));
    case STallSeagrass: {
        const u16 at = level.block(p);
        return sturdy(below, Up) && !rule.tag->test(blockOf(below)) && isWaterFluid(at) && fluidAmount(at) == 8;
    }
    case SSeaPickle: return has(below, fact::CollisionUpAny) || sturdy(below, Up);
    case SSmallDripleaf:
        return rule.tag->test(blockOf(below)) ||
               (fluidType(level.block(p)) == FluidWater && inTag(below, "supports_vegetation"));
    case SSporeBlossom: {
        const u16 above = level.block(p.offset(0, 1, 0));
        if (rule.tag->test(blockOf(above))) return false;
        return has(above, fact::CenterDown) && !isWaterFluid(level.block(p));
    }
    case SPropagule:
        if (blocks.property(state, "hanging") == "true")
            return inTag(level.block(p.offset(0, 1, 0)), "supports_hanging_mangrove_propagule");
        return rule.tag->test(blockOf(below));
    case SCactus: {
        for (int d : kHorizontal) {
            const u16 n = level.block(p.offset(kDirOffset[d][0], 0, kDirOffset[d][2]));
            const FluidType t = fluidType(n);
            if (has(n, fact::Solid) || t == FluidLava || t == FluidFlowingLava) return false;
        }
        return (is(below, bCactus) || rule.tag->test(blockOf(below))) &&
               !has(level.block(p.offset(0, 1, 0)), fact::Liquid);
    }
    case SCactusFlower: return rule.tag->test(blockOf(below)) || has(below, fact::CenterUp);
    case SSugarCane: {
        if (is(below, bSugarCane)) return true;
        if (!rule.tag->test(blockOf(below))) return false;
        for (int d : kHorizontal) {
            const u16 n = level.block(belowPos.offset(kDirOffset[d][0], 0, kDirOffset[d][2]));
            if (fluidInTag(n, "supports_sugar_cane_adjacently") || inTag(n, "supports_sugar_cane_adjacently"))
                return true;
        }
        return false;
    }
    case SCarpet: return !has(below, fact::Air);
    case SMossyCarpet:
        if (blocks.property(state, "bottom") == "true") return !has(below, fact::Air);
        return is(below, block) && blocks.property(below, "bottom") == "true";
    case SSturdyBelow: return sturdy(below, Up);
    case SCoralWall: {
        const std::string_view facing = blocks.property(state, "facing");
        for (int d : kHorizontal)
            if (facing == kFaceNames[d]) {
                const int o = d ^ 1;
                return sturdy(level.block(p.offset(kDirOffset[o][0], 0, kDirOffset[o][2])), d);
            }
        return false;
    }
    case SSnow:
        if (rule.tag->test(blockOf(below))) return false;
        if (inTag(below, "support_override_snow_layer")) return true;
        return collisionFull(below, Up) || (is(below, bSnow) && blocks.property(below, "layers") == "8");
    case SSoulFire: return rule.tag->test(blockOf(below));
    case SMultiface: {
        bool any = false;
        for (int d = 0; d < 6; ++d)
            if (blocks.property(state, kFaceNames[d]) == "true") {
                const u16 n = level.block(p.offset(kDirOffset[d][0], kDirOffset[d][1], kDirOffset[d][2]));
                if (!canAttachTo(n, d)) return false;
                any = true;
            }
        return any;
    }
    case SVine: {
        // VineBlock.canSurvive: hasFaces(getUpdatedState(state)).
        const u16 above = level.block(p.offset(0, 1, 0));
        if (blocks.property(state, "up") == "true" && canAttachTo(above, Up)) return true;
        for (int d = North; d <= East; ++d) {
            if (blocks.property(state, kFaceNames[d]) != "true") continue;
            if (canAttachTo(level.block(p.offset(kDirOffset[d][0], 0, kDirOffset[d][2])), d)) return true;
            if (is(above, bVine) && blocks.property(above, kFaceNames[d]) == "true") return true;
        }
        return false;
    }
    }
    return true;
}

// ---- helpers ------------------------------------------------------------------------------

std::string_view baseName(std::string_view stateKey) { return stateKey.substr(0, stateKey.find('[')); }

std::vector<std::pair<std::string_view, std::string_view>> stateProperties(std::string_view key) {
    std::vector<std::pair<std::string_view, std::string_view>> out;
    const size_t b = key.find('[');
    if (b == std::string_view::npos) return out;
    std::string_view rest = key.substr(b + 1);
    if (!rest.empty() && rest.back() == ']') rest.remove_suffix(1);
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string_view kv = rest.substr(0, comma);
        const size_t eq = kv.find('=');
        if (eq != std::string_view::npos) out.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }
    return out;
}

std::string stateKeyOf(const nlohmann::json& j) {
    if (j.is_string()) return withNs(j.get<std::string>());
    if (!j.is_object()) return "minecraft:air";
    std::string name = j.contains("Name") ? j["Name"].get<std::string>() : j.value("id", std::string("minecraft:air"));
    name = withNs(name);
    const nlohmann::json* props = nullptr;
    if (auto it = j.find("Properties"); it != j.end()) props = &*it;
    if (auto it = j.find("properties"); it != j.end()) props = &*it;
    std::vector<std::pair<std::string, std::string>> kv;
    if (props && props->is_object())
        for (const auto& item : props->items())
            kv.push_back({item.key(), item.value().is_string() ? item.value().get<std::string>() : item.value().dump()});
    std::sort(kv.begin(), kv.end());
    if (kv.empty()) return name;
    name += '[';
    for (size_t i = 0; i < kv.size(); ++i) name += (i ? "," : "") + kv[i].first + "=" + kv[i].second;
    return name + "]";
}

// ---- BlockFacts ---------------------------------------------------------------------------

BlockFacts::BlockFacts(BlockRegistry& blocks, const DataPack& data)
    : impl_(std::make_shared<Impl>(blocks, data)), blocks_(blocks) {}

bool BlockFacts::inTag(u16 id, std::string_view blockTag) const { return impl_->in(id, *impl_->tag(blockTag)); }
bool BlockFacts::isAir(u16 id) const { return impl_->has(id, fact::Air); }
bool BlockFacts::isFluid(u16 id) const { return impl_->fluidType(id) != FluidNone; }
bool BlockFacts::isWater(u16 id) const { return impl_->isWaterFluid(id); }
bool BlockFacts::isSourceWater(u16 id) const { return impl_->fluidType(id) == FluidWater; }
bool BlockFacts::blocksMotion(u16 id) const { return impl_->has(id, fact::BlocksMotion); }
bool BlockFacts::isSolid(u16 id) const { return impl_->has(id, fact::Solid); }
bool BlockFacts::canBeReplacedByFeature(u16 id) const { return !impl_->has(id, fact::CannotReplace); }
bool BlockFacts::is(u16 id, std::string_view blockName) const { return impl_->is(id, impl_->blockIndex(blockName)); }
bool BlockFacts::canBeReplaced(u16 id) const { return impl_->has(id, fact::Replaceable); }
bool BlockFacts::isSolidRender(u16 id) const { return impl_->has(id, fact::SolidRender); }
bool BlockFacts::isFaceSturdy(u16 id, Direction face) const { return impl_->sturdy(id, face); }
bool BlockFacts::isCollisionFaceFull(u16 id, Direction face) const { return impl_->collisionFull(id, face); }
bool BlockFacts::canSurvive(const GenRegion& level, u16 id, const BlockPos& p) const {
    return impl_->canSurvive(level, id, p);
}

}  // namespace wg
