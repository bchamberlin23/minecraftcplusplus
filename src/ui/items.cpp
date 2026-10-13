#include "ui/items.h"

#include "assets/game_reports.h"
#include "net/client.h"
#include "ui/resources.h"

#include <unordered_set>

namespace ui {

namespace {
constexpr int kMaxDepth = 8;

std::string shortName(const std::string& n) { return n.substr(n.find(':') + 1); }

bool endsWithAny(std::string_view s, std::initializer_list<std::string_view> suffixes) {
    for (auto x : suffixes)
        if (s.ends_with(x)) return true;
    return false;
}

int toolDurability(std::string_view n) {
    static const std::pair<std::string_view, int> mats[] = {{"wooden_", 59},  {"stone_", 131},   {"copper_", 190},
                                                            {"iron_", 250},   {"golden_", 32},   {"diamond_", 1561},
                                                            {"netherite_", 2031}};
    if (endsWithAny(n, {"_sword", "_pickaxe", "_axe", "_shovel", "_hoe", "_spear"}))
        for (auto [m, d] : mats)
            if (n.starts_with(m)) return d;
    static const std::pair<std::string_view, int> armor[] = {{"leather_", 5},  {"chainmail_", 15}, {"copper_", 11},
                                                             {"iron_", 15},    {"golden_", 7},     {"diamond_", 33},
                                                             {"netherite_", 37}, {"turtle_", 25}};
    static const std::pair<std::string_view, int> pieces[] = {{"_helmet", 11}, {"_chestplate", 16}, {"_leggings", 15},
                                                              {"_boots", 13}};
    for (auto [p, mult] : pieces)
        if (n.ends_with(p))
            for (auto [m, base] : armor)
                if (n.starts_with(m)) return base * mult;
    static const std::pair<std::string_view, int> single[] = {
        {"bow", 384},     {"crossbow", 465},         {"trident", 250},      {"shield", 336},  {"elytra", 432},
        {"fishing_rod", 64}, {"flint_and_steel", 64}, {"shears", 238},      {"carrot_on_a_stick", 25},
        {"warped_fungus_on_a_stick", 100}, {"brush", 64}, {"mace", 500}, {"wolf_armor", 64}};
    for (auto [s, d] : single)
        if (n == s) return d;
    return 0;
}

int defaultMaxStack(std::string_view n) {
    if (toolDurability(n) > 0) return 1;
    if (n == "bucket" || n == "ender_pearl" || n == "snowball" || n == "egg" || n == "blue_egg" || n == "brown_egg" ||
        n == "honey_bottle" || n == "armor_stand" || n.ends_with("_sign") || (n.ends_with("_banner") && !n.ends_with("_pattern")))
        return 16;
    if (n.ends_with("_bucket") || n.starts_with("music_disc_") || n.ends_with("_bed") || n.ends_with("shulker_box") ||
        n.ends_with("potion") || n.ends_with("_boat") || n.ends_with("_raft") || n.ends_with("minecart") ||
        n.ends_with("_horse_armor") || n.ends_with("bundle") || n.ends_with("_harness") || n.ends_with("_banner_pattern") ||
        n.ends_with("_stew") || n.ends_with("_soup"))
        return 1;
    static const std::unordered_set<std::string_view> ones = {
        "enchanted_book", "saddle",      "totem_of_undying", "cake",          "written_book", "writable_book",
        "spyglass",       "goat_horn",   "debug_stick",      "knowledge_book", "nether_star_disabled", "elytra",
        "shield",         "trident",     "mace",             "milk_bucket"};
    return ones.count(n) ? 1 : 64;
}
}  // namespace

namespace {
// Network NBT: skips one payload of `type`; false on malformed data.
bool skipNbt(net::Reader& r, u8 type, int depth = 0) {
    if (depth > 64) return false;
    switch (type) {
        case 1: r.skip(1); break;
        case 2: r.skip(2); break;
        case 3: case 5: r.skip(4); break;
        case 4: case 6: r.skip(8); break;
        case 7: r.skip(size_t(std::max(0, r.i32v()))); break;
        case 8: r.skip(r.u16v()); break;
        case 9: {
            const u8 t = r.u8v();
            const i32 n = r.i32v();
            for (i32 i = 0; i < n && r.ok; ++i)
                if (!skipNbt(r, t, depth + 1)) return false;
            break;
        }
        case 10:
            for (u8 t = r.u8v(); t != 0 && r.ok; t = r.u8v()) {
                r.skip(r.u16v());
                if (!skipNbt(r, t, depth + 1)) return false;
            }
            break;
        case 11: r.skip(size_t(std::max(0, r.i32v())) * 4); break;
        case 12: r.skip(size_t(std::max(0, r.i32v())) * 8); break;
        default: return false;
    }
    return r.ok;
}

// The fields of a network NBT compound: name -> standalone network NBT of the value (type byte + payload).
std::unordered_map<std::string, std::string> nbtFields(std::string_view nbt) {
    std::unordered_map<std::string, std::string> out;
    net::Reader r(reinterpret_cast<const u8*>(nbt.data()), nbt.size());
    if (r.u8v() != 10) return out;
    for (u8 t = r.u8v(); t != 0 && r.ok; t = r.u8v()) {
        const std::string name(r.bytes(r.u16v()));
        const u8* start = r.p;
        if (!skipNbt(r, t)) break;
        out[name] = std::string(1, char(t)) + std::string(reinterpret_cast<const char*>(start), size_t(r.p - start));
    }
    return out;
}

int nbtInt(const std::string& v, int fallback) {
    if (v.size() < 2) return fallback;
    net::Reader r(reinterpret_cast<const u8*>(v.data()) + 1, v.size() - 1);
    switch (u8(v[0])) {
        case 1: return i8(r.u8v());
        case 2: return r.i16v();
        case 3: return r.i32v();
        default: return fallback;
    }
}
}  // namespace

void Items::setRegistries(const net::Client& client) {
    auto load = [&](const char* registry, std::vector<RegistryInfo>& out) {
        out.clear();
        for (const net::Client::RegistryEntry& e : client.registryEntries(registry)) {
            RegistryInfo info;
            info.id = e.id;
            const auto f = nbtFields(e.nbt);
            auto text = [&](const char* key, Text& t) {
                auto it = f.find(key);
                if (it == f.end()) return false;
                t = parseComponent(it->second, lang_);
                return true;
            };
            text("description", info.description);
            if (auto it = f.find("max_level"); it != f.end()) info.maxLevel = nbtInt(it->second, 1);
            if (auto it = f.find("width"); it != f.end()) info.width = nbtInt(it->second, 0);
            if (auto it = f.find("height"); it != f.end()) info.height = nbtInt(it->second, 0);
            info.hasTitle = text("title", info.title);
            info.hasAuthor = text("author", info.author);
            if (auto it = f.find("translation_key"); it != f.end() && it->second.size() > 3)  // TAG_String: type, length
                info.translationKey = it->second.substr(3);
            if (auto it = f.find("asset_id"); it != f.end() && it->second.size() > 3) info.assetId = it->second.substr(3);
            out.push_back(std::move(info));
        }
    };
    load("minecraft:enchantment", enchantments_);
    load("minecraft:painting_variant", paintings_);
    load("minecraft:instrument", instruments_);
    for (const char* r : {"minecraft:jukebox_song", "minecraft:trim_material", "minecraft:trim_pattern", "minecraft:banner_pattern"})
        load(r, otherRegistries_[r]);
    enchantmentNbt_.clear();
    for (const net::Client::RegistryEntry& e : client.registryEntries("minecraft:enchantment")) enchantmentNbt_[e.id] = e.nbt;
    bannerPatterns_.clear();
    for (const net::Client::RegistryEntry& e : client.registryEntries("minecraft:banner_pattern")) bannerPatterns_.push_back(e.id);
    trimMaterials_.clear();
    for (const net::Client::RegistryEntry& e : client.registryEntries("minecraft:trim_material")) trimMaterials_.push_back(e.id);
    setTags(client.tags());
    enabledFeatures_ = client.enabledFeatures();
    registriesSet_ = true;
}

const std::vector<Items::RegistryInfo>& Items::registry(const std::string& name) const {
    static const std::vector<RegistryInfo> kNone;
    auto it = otherRegistries_.find(name);
    return it == otherRegistries_.end() ? kNone : it->second;
}

const std::string& Items::mobEffectName(int id) const {
    static const std::string kNone;
    return id >= 0 && size_t(id) < mobEffects_.size() ? mobEffects_[size_t(id)] : kNone;
}

const std::string& Items::potionName(int id) const {
    static const std::string kNone;
    return id >= 0 && size_t(id) < potions_.size() ? potions_[size_t(id)] : kNone;
}

int Items::mobEffectId(std::string_view name) const {
    auto it = mobEffectIds_.find(std::string(name.find(':') == std::string_view::npos ? "minecraft:" + std::string(name) : name));
    return it == mobEffectIds_.end() ? -1 : it->second;
}

bool ItemStack::sameItemSameComponents(const ItemStack& o) const {
    if (item != o.item || added.size() != o.added.size() || removed != o.removed) return false;
    for (size_t i = 0; i < added.size(); ++i)
        if (added[i].type != o.added[i].type || added[i].data != o.added[i].data) return false;
    return true;
}

bool Items::init(const GameReports& reports, const Resources& res) {
    lang_ = &res.lang();
    reports_ = &reports;
    const nlohmann::json& reg = reports.registries();
    auto load = [&](const char* key, std::vector<std::string>& names, std::unordered_map<std::string, int>& ids) {
        if (!reg.is_object() || !reg.contains(key)) return false;
        for (const auto& e : reg[key]["entries"].items()) {
            const int id = e.value().value("protocol_id", -1);
            if (id < 0) continue;
            if (size_t(id) >= names.size()) names.resize(size_t(id) + 1);
            names[size_t(id)] = e.key();
            ids[e.key()] = id;
        }
        return !names.empty();
    };
    if (!load("minecraft:item", names_, ids_) || !load("minecraft:data_component_type", componentNames_, componentIds_)) {
        LOG("ui: item registry unavailable (registries.json)");
        return false;
    }
    std::unordered_map<std::string, int> potionIds;
    load("minecraft:potion", potions_, potionIds);
    load("minecraft:mob_effect", mobEffects_, mobEffectIds_);
    std::unordered_map<std::string, int> consumeEffectIds;
    load("minecraft:consume_effect_type", consumeEffects_, consumeEffectIds);
    blockItem_.assign(names_.size(), false);
    for (size_t i = 0; i < names_.size(); ++i) blockItem_[i] = reports.hasBlock(names_[i]);
    return true;
}

const std::string& Items::name(int id) const {
    static const std::string kAir = "minecraft:air";
    return id >= 0 && id < int(names_.size()) ? names_[size_t(id)] : kAir;
}

int Items::id(std::string_view name) const {
    auto it = ids_.find(std::string(name.find(':') == std::string_view::npos ? "minecraft:" + std::string(name) : name));
    return it == ids_.end() ? -1 : it->second;
}

int Items::componentType(std::string_view name) const {
    auto it = componentIds_.find("minecraft:" + std::string(name));
    return it == componentIds_.end() ? -1 : it->second;
}

std::string Items::componentName(int type) const {
    return type >= 0 && size_t(type) < componentNames_.size() ? shortName(componentNames_[size_t(type)]) : std::string();
}

std::string Items::cooldownGroup(const ItemStack& s) const {
    const int type = componentType("use_cooldown");
    for (const ItemStack::Component& c : s.added) {
        if (c.type != type) continue;
        net::Reader r(reinterpret_cast<const u8*>(c.data.data()), c.data.size());  // UseCooldown.STREAM_CODEC
        r.f32();                                                                    // seconds
        if (r.boolean()) {
            std::string group(r.str());
            if (r.ok) return group;
        }
        break;
    }
    return name(s.item);
}

int Items::maxStack(const ItemStack& s) const {
    if (s.maxStack > 0) return s.maxStack;
    if (auto it = defaults(s.item).find("minecraft:max_stack_size"); it != defaults(s.item).end() && it->is_number_integer())
        return it->get<int>();
    if (s.maxDamage > 0 || s.damage > 0) return 1;
    return defaultMaxStack(shortName(name(s.item)));
}

int Items::maxDamage(const ItemStack& s) const {
    if (s.maxDamage >= 0) return s.maxDamage;
    if (auto it = defaults(s.item).find("minecraft:max_damage"); it != defaults(s.item).end() && it->is_number_integer())
        return it->get<int>();
    return toolDurability(shortName(name(s.item)));
}

// --- protocol ---------------------------------------------------------------------------------------

bool Items::readTemplate(net::Reader& r, int depth) const {
    r.varInt();  // item
    r.varInt();  // count
    ItemStack tmp;
    return readPatch(r, tmp, depth + 1);
}

// One component value per its network codec (DataComponents: networkSynchronized, else the persistent codec as NBT).
bool Items::skipValue(net::Reader& r, int type, int depth) const {
    if (depth > kMaxDepth || type < 0 || type >= int(componentNames_.size())) return false;
    const std::string n = shortName(componentNames_[size_t(type)]);
    auto list = [&](auto&& each) {
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i)
            if (!each()) return false;
        return r.ok;
    };
    auto optional = [&](auto&& each) { return r.boolean() ? each() : r.ok; };
    auto str = [&] { return r.str(), r.ok; };
    auto text = [&] { return r.nbt(), r.ok; };  // ComponentSerialization.STREAM_CODEC
    // ByteBufCodecs.holder: registry id + 1, or 0 followed by the inline value.
    auto holder = [&](auto&& direct) { return r.varInt() != 0 ? r.ok : direct(); };
    // ByteBufCodecs.holderSet: 0 + tag id, else count + 1 then registry ids.
    auto holderSet = [&] {
        const i32 c = r.varInt();
        if (c == 0) return str();
        for (i32 i = 0; i < c - 1 && r.ok; ++i) r.varInt();
        return r.ok;
    };
    auto sound = [&] { return holder([&] { return str() && optional([&] { return r.f32(), r.ok; }); }); };  // SoundEvent
    auto effectDetails = [&](auto&& self) -> bool {  // MobEffectInstance.Details (the hidden effect recurses)
        r.varInt(), r.varInt();
        r.boolean(), r.boolean(), r.boolean();
        return r.boolean() ? self(self) : r.ok;
    };
    auto effect = [&] { return r.varInt(), effectDetails(effectDetails); };  // MobEffectInstance (effect registry id)
    auto consumeEffect = [&] {  // ConsumeEffect: type registry id, then that type's codec
        const i32 t = r.varInt();
        const std::string e = t >= 0 && t < int(consumeEffects_.size()) ? shortName(consumeEffects_[size_t(t)]) : "";
        if (e == "apply_effects") return list(effect) && (r.f32(), r.ok);
        if (e == "remove_effects") return holderSet();
        if (e == "clear_all_effects") return r.ok;
        if (e == "teleport_randomly") return r.f32(), r.boolean(), r.ok;
        if (e == "play_sound") return sound();
        return false;
    };
    auto resolvableInt = [&] { return r.boolean() ? (r.i32v(), r.ok) : str(); };  // constant, else provider key
    auto resolvableFloat = [&] { return r.boolean() ? (r.f32(), r.ok) : str(); };
    auto signTexts = [&] {  // SignText messages: a fixed list of 4
        for (int i = 0; i < 4 && r.ok; ++i) r.nbt();
        return r.ok;
    };

    static const std::unordered_set<std::string> nbt = {
        "custom_data", "custom_name", "item_name", "intangible_projectile", "map_decorations", "debug_stick_state",
        "bucket_entity_data", "recipes", "lock", "container_loot"};
    static const std::unordered_set<std::string> varint = {
        "max_stack_size", "max_damage", "damage", "repair_cost", "additional_trade_cost", "map_id",
        "map_post_processing", "rarity", "ominous_bottle_amplifier", "base_color", "dye", "enchantable",
        "damage_type", "block_transformer", "villager_food", "provides_pottery_pattern"};
    static const std::unordered_set<std::string> unit = {"unbreakable", "creative_slot_lock", "glider", "waxed"};
    if (nbt.count(n)) return text();
    // PaintingVariant is a holder (inline: size, asset, optional title / author); the other entity variant
    // components are a registry or enum id.
    if (n == "painting/variant")
        return holder([&] { return (r.varInt(), r.varInt(), str()) && optional(text) && optional(text); });
    if (varint.count(n) || n.find('/') != std::string::npos) return r.varInt(), r.ok;
    if (unit.count(n)) return true;
    if (n == "minimum_attack_charge" || n == "potion_duration_scale") return r.f32(), r.ok;
    if (n == "enchantment_glint_override") return r.boolean(), r.ok;
    if (n == "item_model" || n == "tooltip_style" || n == "note_block_sound") return str();
    if (n == "use_effects") return r.boolean(), r.boolean(), r.f32(), r.ok;
    if (n == "lore") return list(text);
    if (n == "enchantments" || n == "stored_enchantments") return list([&] { return r.varInt(), r.varInt(), r.ok; });
    if (n == "custom_model_data")
        return list([&] { return r.f32(), true; }) && list([&] { return r.boolean(), true; }) &&
               list([&] { return r.str(), true; }) && list([&] { return r.u32v(), true; });
    if (n == "tooltip_display") return r.boolean(), list([&] { return r.varInt(), true; });
    if (n == "dyed_color") return r.u32v(), r.ok;
    if (n == "potion_contents")
        return optional([&] { return r.varInt(), true; }) && optional([&] { return r.u32v(), true; }) && list(effect) &&
               optional(str);
    if (n == "suspicious_stew_effects") return list([&] { return r.varInt(), r.varInt(), true; });
    if (n == "writable_book_content") return list([&] { return r.str(), optional([&] { return r.str(), true; }); });
    if (n == "written_book_content")
        return r.str(), optional([&] { return r.str(), true; }) && (r.str(), r.varInt(), true) &&
                            list([&] { return r.nbt(), optional([&] { return r.nbt(), true; }); }) && (r.boolean(), r.ok);
    if (n == "charged_projectiles" || n == "bundle_contents") return list([&] { return readTemplate(r, depth); });
    if (n == "container") return list([&] { return optional([&] { return readTemplate(r, depth); }); });
    if (n == "use_remainder" || n == "sulfur_cube_content") return readTemplate(r, depth);
    if (n == "pot_decorations") {  // back, left, right, front: optional item stack templates
        for (int i = 0; i < 4 && r.ok; ++i)
            if (!optional([&] { return readTemplate(r, depth); })) return false;
        return r.ok;
    }
    if (n == "entity_data" || n == "block_entity_data") return r.varInt(), r.nbt(), r.ok;
    if (n == "lodestone_tracker") return optional([&] { return r.str(), r.u64v(), true; }) && (r.boolean(), r.ok);
    auto explosion = [&] {
        r.varInt();
        return list([&] { return r.u32v(), true; }) && list([&] { return r.u32v(), true; }) &&
               (r.boolean(), r.boolean(), r.ok);
    };
    if (n == "firework_explosion") return explosion();
    if (n == "fireworks") return r.varInt(), list(explosion);
    if (n == "block_state") return list([&] { return r.str(), r.str(), r.ok; });
    auto trimMaterial = [&] { return holder([&] { return str() && text(); }); };  // inline: palette, description
    if (n == "trim") return trimMaterial() && holder([&] { return str() && text() && (r.boolean(), r.ok); });
    if (n == "provides_trim_material") return trimMaterial();
    if (n == "instrument") return holder([&] { return sound() && (r.f32(), r.f32(), r.varInt(), text()); });
    if (n == "jukebox_playable") return holder([&] { return sound() && text() && (r.f32(), r.varInt(), r.ok); });
    if (n == "tool")  // rules (blocks, optional speed, optional correct_for_drops), speed, damage per block, creative
        return list([&] {
                   return holderSet() && optional([&] { return r.f32(), r.ok; }) && optional([&] { return r.boolean(), r.ok; });
               }) &&
               (r.f32(), r.varInt(), r.boolean(), r.ok);
    if (n == "can_break" || n == "can_place_on")  // AdventureModePredicate: BlockPredicate list
        return list([&] {
            if (!optional(holderSet)) return false;
            const bool props = optional([&] {
                return list([&] {
                    r.str();
                    if (r.boolean()) r.str();                                 // ExactMatcher
                    else optional([&] { return r.str(), true; }), optional([&] { return r.str(), true; });  // RangedMatcher
                    return r.ok;
                });
            });
            if (!props) return false;
            if (r.boolean()) r.nbt();  // NbtPredicate
            // DataComponentMatchers: exact components (type + value), then partial predicates (a predicate type or a
            // component type, then the predicate as NBT).
            return list([&] { return skipValue(r, r.varInt(), depth + 1); }) &&
                   list([&] { return r.boolean(), r.varInt(), text(); });
        });
    if (n == "banner_patterns")  // layers: pattern (inline: asset, translation key), dye colour
        return list([&] { return holder([&] { return str() && str(); }) && (r.varInt(), r.ok); });
    if (n == "provides_banner_patterns") return holderSet();
    if (n == "profile") {  // ResolvableProfile: a full GameProfile or a partial one, then the skin patch
        auto properties = [&] { return list([&] { return str() && str() && optional(str); }); };
        const bool profile = r.boolean() ? (r.skip(16), str() && properties())
                                         : optional(str) && optional([&] { return r.skip(16), r.ok; }) && properties();
        return profile && optional(str) && optional(str) && optional(str) && optional([&] { return r.boolean(), r.ok; });
    }
    if (n == "bees") return list([&] { return r.varInt(), r.nbt(), r.varInt(), r.varInt(), r.ok; });
    if (n == "attribute_modifiers")  // attribute, modifier id, amount, operation, slot group, display (override: text)
        return list([&] {
            r.varInt(), r.str(), r.f64(), r.varInt(), r.varInt();
            return r.varInt() == 2 ? text() : r.ok;
        });
    if (n == "food") return r.varInt(), r.f32(), r.boolean(), r.ok;
    if (n == "consumable") return (r.f32(), r.varInt(), sound()) && (r.boolean(), list(consumeEffect));
    if (n == "death_protection") return list(consumeEffect);
    if (n == "use_cooldown") return r.f32(), optional(str);
    if (n == "damage_resistant" || n == "repairable") return holderSet();
    if (n == "weapon") return r.varInt(), r.f32(), r.ok;
    if (n == "attack_range") {  // min / max reach, creative min / max reach, hitbox margin, mob factor
        for (int i = 0; i < 6; ++i) r.f32();
        return r.ok;
    }
    if (n == "equippable")  // slot, equip sound, asset, camera overlay, allowed entities, 5 flags, shearing sound
        return (r.varInt(), sound()) && optional(str) && optional(str) && optional(holderSet) &&
               (r.boolean(), r.boolean(), r.boolean(), r.boolean(), r.boolean(), sound());
    if (n == "blocks_attacks")  // delay, disable scale, damage reductions, item damage, bypassed by, block / disable sounds
        return (r.f32(), r.f32(), list([&] { return r.f32(), optional(holderSet) && (r.f32(), r.f32(), r.ok); })) &&
               (r.f32(), r.f32(), r.f32(), optional(holderSet)) && optional(sound) && optional(sound);
    if (n == "piercing_weapon") return (r.boolean(), r.boolean(), optional(sound)) && optional(sound);
    if (n == "kinetic_weapon") {  // cooldown, delay, dismount / knockback / damage conditions, movement, multiplier, sounds
        auto condition = [&] { return r.varInt(), r.f32(), r.f32(), r.ok; };
        return (r.varInt(), r.varInt(), optional(condition)) && optional(condition) && optional(condition) &&
               (r.f32(), r.f32(), optional(sound)) && optional(sound);
    }
    if (n == "attack_animation" || n == "interact_animation") return r.varInt(), r.varInt(), r.ok;  // type, duration
    if (n == "break_sound") return sound();
    if (n == "compostable") return resolvableInt();
    if (n == "cooking_fuel" || n == "brewing_fuel") return resolvableInt() && resolvableFloat();
    if (n == "mob_visibility") return holderSet() && (r.f32(), r.ok);
    if (n == "sign_text_front" || n == "sign_text_back")  // messages, optional filtered messages, colour, glowing
        return signTexts() && optional(signTexts) && (r.varInt(), r.boolean(), r.ok);
    return false;  // not a component of this protocol version
}

bool Items::readPatch(net::Reader& r, ItemStack& out, int depth) const {
    const i32 added = r.varInt(), removed = r.varInt();
    for (i32 i = 0; i < added && r.ok; ++i) {
        const int type = r.varInt();
        const u8* start = r.p;
        if (!skipValue(r, type, depth) || !r.ok) {
            if (depth == 0)
                LOG("ui: cannot decode item component %s", type >= 0 && type < int(componentNames_.size())
                                                               ? componentNames_[size_t(type)].c_str()
                                                               : "?");
            return false;
        }
        if (depth > 0) continue;
        ItemStack::Component c{type, std::string(reinterpret_cast<const char*>(start), size_t(r.p - start))};
        // Decode the parts shown in the UI.
        net::Reader v(reinterpret_cast<const u8*>(c.data.data()), c.data.size());
        const std::string n = shortName(componentNames_[size_t(type)]);
        if (n == "custom_name") out.hasCustomName = true, out.customName = parseComponent(v.nbt(), lang_);
        else if (n == "item_name") out.hasItemName = true, out.itemName = parseComponent(v.nbt(), lang_);
        else if (n == "damage") out.damage = v.varInt();
        else if (n == "max_damage") out.maxDamage = v.varInt();
        else if (n == "max_stack_size") out.maxStack = v.varInt();
        else if (n == "rarity") out.rarity = v.varInt();
        else if (n == "enchantment_glint_override") out.glint = v.boolean();
        else if (n == "enchantments" || n == "stored_enchantments") {
            auto& list = n == "enchantments" ? out.enchantList : out.storedEnchants;
            const i32 cnt = v.varInt();
            for (i32 k = 0; k < cnt && v.ok; ++k) {
                const int id = v.varInt(), level = v.varInt();
                list.emplace_back(id, level);
            }
            if (n == "enchantments") out.enchantments = cnt;
        } else if (n == "suspicious_stew_effects") {
            const i32 cnt = v.varInt();
            for (i32 k = 0; k < cnt && v.ok; ++k) {
                const int id = v.varInt(), ticks = v.varInt();
                out.stewEffects.emplace_back(id, ticks);
            }
        } else if (n == "instrument") out.instrument = v.varInt() - 1;
        else if (n == "painting/variant") out.painting = v.varInt() - 1;
        else if (n == "tooltip_display") {
            out.hideTooltip = v.boolean();
            const i32 cnt = v.varInt();
            for (i32 k = 0; k < cnt && v.ok; ++k) out.hiddenTooltip.push_back(v.varInt());
        }
        else if (n == "dyed_color") out.dyedColor = v.u32v() & 0xFFFFFF;
        else if (n == "lore") {
            const i32 cnt = v.varInt();
            for (i32 k = 0; k < cnt && v.ok; ++k) out.lore.push_back(parseComponent(v.nbt(), lang_));
        } else if (n == "potion_contents") {
            if (v.boolean()) out.potion = v.varInt();
            if (v.boolean()) out.potionColor = v.u32v() & 0xFFFFFF;
        } else if (n == "food") {
            out.foodNutrition = v.varInt();
            out.foodSaturation = v.f32();
            out.foodAlwaysEdible = v.boolean();
        } else if (n == "consumable") {
            out.consumeSeconds = v.f32();
            out.consumeAnimation = v.varInt();
        } else if (n == "use_cooldown") {
            out.cooldownSeconds = v.f32();
            if (v.boolean()) out.cooldownGroup = v.str();
        } else if (n == "equippable") {
            out.equipSlot = v.varInt();
            if (v.varInt() == 0) {  // inline equip sound
                v.str();
                if (v.boolean()) v.f32();
            }
            if (v.boolean()) out.equipAsset = v.str();
            if (v.boolean()) out.equipCameraOverlay = v.str();
        } else if (n == "attribute_modifiers") {
            out.hasAttributeModifiers = true;
            const i32 cnt = v.varInt();
            for (i32 k = 0; k < cnt && v.ok; ++k) {
                ItemStack::AttributeModifier m;
                m.attribute = v.varInt();
                m.id = v.str();
                m.amount = v.f64();
                m.operation = v.varInt();
                m.slot = v.varInt();
                if (v.varInt() == 2) v.nbt();  // display override text
                out.attributeModifiers.push_back(std::move(m));
            }
        }
        out.added.push_back(std::move(c));
    }
    for (i32 i = 0; i < removed && r.ok; ++i) {
        const int type = r.varInt();
        if (depth == 0) out.removed.push_back(type);
    }
    return r.ok;
}

bool Items::readTemplate(net::Reader& r, ItemStack& out) const {
    out = ItemStack{};
    out.item = r.varInt();
    out.count = r.varInt();
    if (!readPatch(r, out, 0)) {
        out.componentsKnown = false;
        return false;
    }
    return r.ok;
}

bool Items::read(net::Reader& r, ItemStack& out) const {
    out = ItemStack{};
    const i32 count = r.varInt();
    if (count <= 0) return r.ok;
    out.count = count;
    out.item = r.varInt();
    if (!readPatch(r, out, 0)) {
        out.componentsKnown = false;
        return false;
    }
    return r.ok;
}

void Items::write(net::Writer& w, const ItemStack& s, bool delimited) const {
    if (s.empty()) {
        w.varInt(0);
        return;
    }
    w.varInt(s.count);
    w.varInt(s.item);
    w.varInt(i32(s.added.size()));
    w.varInt(i32(s.removed.size()));
    for (const auto& c : s.added) {
        w.varInt(c.type);
        if (delimited) w.varInt(i32(c.data.size()));
        w.bytes(c.data.data(), c.data.size());
    }
    for (int t : s.removed) w.varInt(t);
}

void Items::writeHashed(net::Writer& w, const ItemStack& s) const {
    w.boolean(!s.empty());
    if (s.empty()) return;
    w.varInt(s.item);
    w.varInt(s.count);
    w.varInt(i32(s.added.size()));
    for (const auto& c : s.added) {
        w.varInt(c.type);
        w.u32v(0);  // the component's hash (HashOps): not computed; the server resyncs mismatches
    }
    w.varInt(i32(s.removed.size()));
    for (int t : s.removed) w.varInt(t);
}

}  // namespace ui
