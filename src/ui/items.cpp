#include "ui/items.h"

#include "assets/game_reports.h"
#include "net/client.h"
#include "ui/resources.h"

#include <array>
#include <cstring>
#include <unordered_set>

#include "world/nbt_dom.h"

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

// A HolderSet in network NBT: "#tag", one id, or a list of ids.
void holderNames(const std::unordered_map<std::string, std::string>& f, const char* key, std::string& tag,
                 std::vector<std::string>& ids) {
    auto it = f.find(key);
    if (it == f.end() || it->second.size() < 2) return;
    net::Reader r(reinterpret_cast<const u8*>(it->second.data()) + 1, it->second.size() - 1);
    if (it->second[0] == 8) {
        std::string v(r.bytes(r.u16v()));
        if (v.starts_with('#')) tag = v.substr(1);
        else ids.push_back(std::move(v));
    } else if (it->second[0] == 9 && r.u8v() == 8) {
        for (i32 n = r.i32v(); n > 0 && r.ok; --n) ids.emplace_back(r.bytes(r.u16v()));
    }
}

std::string withNamespace(std::string_view id) {
    return id.find(':') == std::string_view::npos ? "minecraft:" + std::string(id) : std::string(id);
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
            if (auto it = f.find("anvil_cost"); it != f.end()) info.anvilCost = nbtInt(it->second, 1);
            holderNames(f, "supported_items", info.supportedTag, info.supportedList);
            holderNames(f, "exclusive_set", info.exclusiveTag, info.exclusiveList);
            if (auto it = f.find("width"); it != f.end()) info.width = nbtInt(it->second, 0);
            if (auto it = f.find("height"); it != f.end()) info.height = nbtInt(it->second, 0);
            info.hasTitle = text("title", info.title);
            info.hasAuthor = text("author", info.author);
            if (auto it = f.find("translation_key"); it != f.end() && it->second.size() > 3)  // TAG_String: type, length
                info.translationKey = it->second.substr(3);
            if (auto it = f.find("asset_id"); it != f.end() && it->second.size() > 3) info.assetId = it->second.substr(3);
            if (auto it = f.find("sound_event"); it != f.end() && it->second.size() > 3 && it->second[0] == 8)
                info.soundEvent = it->second.substr(3);
            auto number = [&](const char* key, float& out) {  // TAG_Float / TAG_Double
                auto it = f.find(key);
                if (it == f.end() || it->second.size() < 5) return;
                net::Reader r(reinterpret_cast<const u8*>(it->second.data()) + 1, it->second.size() - 1);
                if (it->second[0] == 5) out = r.f32();
                else if (it->second[0] == 6 && it->second.size() >= 9) out = float(r.f64());
            };
            number("range", info.range);
            number("use_duration", info.useDuration);
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
    trimMaterials_.clear(), trimMaterialNbt_.clear(), trimPatterns_.clear(), trimPatternNbt_.clear();
    for (const net::Client::RegistryEntry& e : client.registryEntries("minecraft:trim_material"))
        trimMaterials_.push_back(e.id), trimMaterialNbt_.push_back(e.nbt);
    for (const net::Client::RegistryEntry& e : client.registryEntries("minecraft:trim_pattern"))
        trimPatterns_.push_back(e.id), trimPatternNbt_.push_back(e.nbt);
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

bool Items::enchantmentSupports(int enchantment, int item) const {
    if (enchantment < 0 || size_t(enchantment) >= enchantments_.size()) return false;
    const RegistryInfo& e = enchantments_[size_t(enchantment)];
    if (!e.supportedTag.empty()) {
        const std::vector<int>* t = tag("minecraft:item", withNamespace(e.supportedTag));
        return t && std::find(t->begin(), t->end(), item) != t->end();
    }
    for (const std::string& n : e.supportedList)
        if (id(withNamespace(n)) == item) return true;
    return false;
}

bool Items::enchantmentsCompatible(int a, int b) const {
    auto excludes = [&](int x, int y) {  // y is in x's exclusive set
        if (x < 0 || size_t(x) >= enchantments_.size() || y < 0 || size_t(y) >= enchantments_.size()) return false;
        const RegistryInfo& e = enchantments_[size_t(x)];
        if (!e.exclusiveTag.empty()) {
            const std::vector<int>* t = tag("minecraft:enchantment", withNamespace(e.exclusiveTag));
            return t && std::find(t->begin(), t->end(), y) != t->end();
        }
        for (const std::string& n : e.exclusiveList)
            if (withNamespace(n) == enchantments_[size_t(y)].id) return true;
        return false;
    };
    return a != b && !excludes(a, b) && !excludes(b, a);
}

bool Items::enchantmentIsCurse(int enchantment) const {
    const std::vector<int>* t = tag("minecraft:enchantment", "minecraft:curse");
    return t && std::find(t->begin(), t->end(), enchantment) != t->end();
}

bool Items::repairableWith(const ItemStack& s, int item) const {
    const ComponentValue v = component(s, "repairable");
    if (v.patched) {  // Repairable: HolderSet stream codec (size + 1 ids, or 0 and a tag)
        net::Reader r(reinterpret_cast<const u8*>(v.patched->data()), v.patched->size());
        const i32 n = r.varInt();
        if (n == 0) {
            const std::vector<int>* t = tag("minecraft:item", withNamespace(r.str()));
            return t && std::find(t->begin(), t->end(), item) != t->end();
        }
        for (i32 i = 1; i < n && r.ok; ++i)
            if (r.varInt() == item) return true;
        return false;
    }
    if (!v.fallback || !v.fallback->is_object()) return false;
    const nlohmann::json& items = v.fallback->value("items", nlohmann::json());
    auto match = [&](const nlohmann::json& j) {
        if (!j.is_string()) return false;
        const std::string n = j.get<std::string>();
        if (!n.starts_with('#')) return id(withNamespace(n)) == item;
        const std::vector<int>* t = tag("minecraft:item", withNamespace(n.substr(1)));
        return t && std::find(t->begin(), t->end(), item) != t->end();
    };
    if (items.is_array()) return std::any_of(items.begin(), items.end(), match);
    return match(items);
}

void Items::setComponent(ItemStack& s, std::string_view name, const std::string* data) const {
    const int type = componentType(name);
    if (type < 0 || s.empty()) return;
    std::erase_if(s.added, [&](const ItemStack::Component& c) { return c.type == type; });
    std::erase(s.removed, type);
    if (data) s.added.push_back({type, *data});
    net::Writer w;  // decoded again from the patch, like a stack from the network
    write(w, s, false);
    net::Reader r(w.buf.data(), w.buf.size());
    ItemStack decoded;
    if (read(r, decoded)) decoded.bundleSelected = s.bundleSelected, s = std::move(decoded);
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

// --- HashOps (CRC32C over a tagged encoding of the component's codec value) ------------------------------------------
namespace {

u32 crc32c(const std::vector<u8>& data) {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0x82F63B78u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    u32 c = 0xFFFFFFFFu;
    for (u8 b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
void le(std::vector<u8>& b, u64 v, int bytes) {
    for (int i = 0; i < bytes; ++i) b.push_back(u8(v >> (8 * i)));
}
u32 hTagged(u8 tag, u64 v, int bytes) {
    std::vector<u8> b{tag};
    le(b, v, bytes);
    return crc32c(b);
}
u32 hInt(i32 v) { return hTagged(8, u32(v), 4); }
constexpr const char* kDyeNames[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                       "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
u32 hBool(bool v) { return crc32c({13, u8(v ? 1 : 0)}); }
u32 hString(const std::string& utf8) {  // [12] + the UTF-16 length + the UTF-16 units (putUnencodedChars)
    std::u16string u;
    for (size_t i = 0; i < utf8.size();) {
        const u32 cp = nextCodepoint(utf8, i);
        if (cp > 0xFFFF) u.push_back(char16_t(0xD800 + ((cp - 0x10000) >> 10))), u.push_back(char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF)));
        else u.push_back(char16_t(cp));
    }
    std::vector<u8> b{12};
    le(b, u.size(), 4);
    for (char16_t ch : u) le(b, ch, 2);
    return crc32c(b);
}
u32 hList(const std::vector<u32>& xs) {
    std::vector<u8> b{4};
    for (u32 x : xs) le(b, x, 4);
    b.push_back(5);
    return crc32c(b);
}
u32 hMap(std::vector<std::pair<u32, u32>> kv) {  // entries by key hash, then value hash (padToLong: unsigned)
    std::sort(kv.begin(), kv.end());
    std::vector<u8> b{2};
    for (auto [k, v] : kv) le(b, k, 4), le(b, v, 4);
    b.push_back(3);
    return crc32c(b);
}
// An NBT value as its codec wrote it through NbtOps: booleans became bytes (the text style flags), mixed lists wrap
// their elements in {"": ...} compounds.
u32 hNbt(const NbtValue& v, std::string_view key, bool text = true) {
    static const std::string_view kBools[] = {"bold", "italic", "underlined", "strikethrough", "obfuscated", "interpret"};
    switch (v.type) {
        case TagByte:
            if (text && std::find(std::begin(kBools), std::end(kBools), key) != std::end(kBools)) return hBool(v.i != 0);
            return crc32c({6, u8(v.i)});
        case TagShort: return hTagged(7, u16(v.i), 2);
        case TagInt: return hInt(i32(v.i));
        case TagLong: return hTagged(9, u64(v.i), 8);
        case TagFloat: {
            const float f = float(v.d);
            u32 bits;
            std::memcpy(&bits, &f, 4);
            return hTagged(10, bits, 4);
        }
        case TagDouble: {
            u64 bits;
            std::memcpy(&bits, &v.d, 8);
            return hTagged(11, bits, 8);
        }
        case TagString: return hString(v.s);
        case TagByteArray: {
            std::vector<u8> b{14};
            b.insert(b.end(), v.bytes.begin(), v.bytes.end());
            b.push_back(15);
            return crc32c(b);
        }
        case TagIntArray: {
            std::vector<u8> b{16};
            for (i32 x : v.ints) le(b, u32(x), 4);
            b.push_back(17);
            return crc32c(b);
        }
        case TagLongArray: {
            std::vector<u8> b{18};
            for (i64 x : v.longs) le(b, u64(x), 8);
            b.push_back(19);
            return crc32c(b);
        }
        case TagList: {
            std::vector<u32> xs;
            for (const NbtValue& e : v.items)
                xs.push_back(text && e.type == TagCompound && e.names.size() == 1 && e.names[0].empty() ? hNbt(e.items[0], "", text)
                                                                                                      : hNbt(e, "", text));
            return hList(xs);
        }
        case TagCompound: {
            std::vector<std::pair<u32, u32>> kv;
            for (size_t k = 0; k < v.items.size(); ++k) kv.emplace_back(hString(v.names[k]), hNbt(v.items[k], v.names[k], text));
            return hMap(std::move(kv));
        }
        default: return crc32c({1});
    }
}

}  // namespace

// ClientPacketListener.decoratedHashOpsGenerator: the component's value through its codec into HashOps; false for the
// components not reproduced here (their hash stays 0 and the server resyncs the slot).
bool Items::componentHash(const ItemStack::Component& c, u32& out) const {
    const std::string n = componentName(c.type);
    net::Reader r(reinterpret_cast<const u8*>(c.data.data()), c.data.size());
    auto textNbt = [&](NbtValue& v) {
        const std::string_view raw = r.nbt();
        return r.ok && nbtReadNetwork(reinterpret_cast<const u8*>(raw.data()), raw.size(), v);
    };
    if (n == "damage" || n == "max_damage" || n == "repair_cost" || n == "max_stack_size" || n == "map_id" ||
        n == "ominous_bottle_amplifier")
        out = hInt(r.varInt());
    else if (n == "dyed_color" || n == "map_color")
        out = hInt(r.i32v());
    else if (n == "enchantment_glint_override")
        out = hBool(r.boolean());
    else if (n == "unbreakable" || n == "creative_slot_lock" || n == "glider" || n == "intangible_projectile")
        out = crc32c({2, 3});  // Unit: an empty map
    else if (n == "rarity") {
        static const char* kRarity[] = {"common", "uncommon", "rare", "epic"};
        const i32 id = r.varInt();
        if (id < 0 || id > 3) return false;
        out = hString(kRarity[id]);
    } else if (n == "custom_name" || n == "item_name") {
        NbtValue v;
        if (!textNbt(v)) return false;
        out = hNbt(v, "");
    } else if (n == "custom_data" || n == "bucket_entity_data") {  // CustomData: the CompoundTag itself (NBT types as they are)
        NbtValue v;
        if (!textNbt(v)) return false;
        out = hNbt(v, "", false);
    } else if (n == "lore") {
        std::vector<u32> lines;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            NbtValue v;
            if (!textNbt(v)) return false;
            lines.push_back(hNbt(v, ""));
        }
        out = hList(lines);
    } else if (n == "potion_contents") {  // PotionContents.FULL_CODEC: {potion, custom_color, custom_effects, custom_name}
        std::vector<std::pair<u32, u32>> kv;
        if (r.boolean()) {
            const std::string& potion = potionName(r.varInt());
            if (potion.empty()) return false;
            kv.emplace_back(hString("potion"), hString(potion));
        }
        if (r.boolean()) kv.emplace_back(hString("custom_color"), hInt(r.i32v()));
        if (r.varInt() != 0) return false;  // custom effects (MobEffectInstance): not reproduced
        if (r.boolean()) kv.emplace_back(hString("custom_name"), hString(std::string(r.str())));
        out = hMap(std::move(kv));
    } else if (n == "writable_book_content") {  // {pages: [{raw, filtered?}...]}, no pages: {}
        std::vector<u32> pages;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {  // Filterable.codec: the full form
            std::vector<std::pair<u32, u32>> page{{hString("raw"), hString(std::string(r.str()))}};
            if (r.boolean()) page.emplace_back(hString("filtered"), hString(std::string(r.str())));
            pages.push_back(hMap(std::move(page)));
        }
        std::vector<std::pair<u32, u32>> kv;
        if (!pages.empty()) kv.emplace_back(hString("pages"), hList(pages));
        out = hMap(std::move(kv));
    } else if (n == "written_book_content") {  // {title: {raw}, author, generation?, pages?, resolved?}
        std::vector<std::pair<u32, u32>> title{{hString("raw"), hString(std::string(r.str()))}};
        if (r.boolean()) title.emplace_back(hString("filtered"), hString(std::string(r.str())));
        std::vector<std::pair<u32, u32>> kv{{hString("title"), hMap(std::move(title))}};
        kv.emplace_back(hString("author"), hString(std::string(r.str())));
        if (const i32 generation = r.varInt(); generation != 0) kv.emplace_back(hString("generation"), hInt(generation));
        std::vector<u32> pages;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            NbtValue raw;
            if (!textNbt(raw)) return false;
            std::vector<std::pair<u32, u32>> page{{hString("raw"), hNbt(raw, "")}};
            if (r.boolean()) {
                NbtValue filtered;
                if (!textNbt(filtered)) return false;
                page.emplace_back(hString("filtered"), hNbt(filtered, ""));
            }
            pages.push_back(hMap(std::move(page)));
        }
        if (!pages.empty()) kv.emplace_back(hString("pages"), hList(pages));
        if (r.boolean()) kv.emplace_back(hString("resolved"), hBool(true));
        out = hMap(std::move(kv));
    } else if (n == "note_block_sound") {  // an identifier
        out = hString(std::string(r.str()));
    } else if (n == "instrument") {  // a registry holder (id + 1; 0: inline, not reproduced): its key
        const i32 holder = r.varInt();
        if (holder <= 0 || size_t(holder - 1) >= instruments_.size()) return false;
        out = hString(instruments_[size_t(holder - 1)].id);
    } else if (n == "lodestone_tracker") {  // {target?: {dimension, pos: [x, y, z]}, tracked unless true}
        std::vector<std::pair<u32, u32>> kv;
        if (r.boolean()) {
            const std::string dimension(r.str());
            int x, y, z;
            r.blockPos(x, y, z);
            std::vector<u8> ints{16};  // BlockPos.CODEC: an int stream
            for (int v : {x, y, z}) le(ints, u32(v), 4);
            ints.push_back(17);
            kv.emplace_back(hString("target"), hMap({{hString("dimension"), hString(dimension)}, {hString("pos"), crc32c(ints)}}));
        }
        if (!r.boolean()) kv.emplace_back(hString("tracked"), hBool(false));
        out = hMap(std::move(kv));
    } else if (n == "custom_model_data") {  // {floats, flags, strings, colors}: each unless empty
        std::vector<std::pair<u32, u32>> kv;
        std::vector<u32> floats, flags, strings, colors;
        for (i32 i = 0, cnt = r.varInt(); i < cnt && r.ok; ++i) {
            const float f = r.f32();
            u32 bits;
            std::memcpy(&bits, &f, 4);
            floats.push_back(hTagged(10, bits, 4));
        }
        for (i32 i = 0, cnt = r.varInt(); i < cnt && r.ok; ++i) flags.push_back(hBool(r.boolean()));
        for (i32 i = 0, cnt = r.varInt(); i < cnt && r.ok; ++i) strings.push_back(hString(std::string(r.str())));
        for (i32 i = 0, cnt = r.varInt(); i < cnt && r.ok; ++i) colors.push_back(hInt(r.i32v()));
        if (!floats.empty()) kv.emplace_back(hString("floats"), hList(floats));
        if (!flags.empty()) kv.emplace_back(hString("flags"), hList(flags));
        if (!strings.empty()) kv.emplace_back(hString("strings"), hList(strings));
        if (!colors.empty()) kv.emplace_back(hString("colors"), hList(colors));
        out = hMap(std::move(kv));
    } else if (n == "tooltip_display") {  // {hide_tooltip if set, hidden_components unless empty}
        std::vector<std::pair<u32, u32>> kv;
        if (r.boolean()) kv.emplace_back(hString("hide_tooltip"), hBool(true));
        std::vector<u32> hidden;
        for (i32 i = 0, cnt = r.varInt(); i < cnt && r.ok; ++i) {
            const int type = r.varInt();
            if (type < 0 || size_t(type) >= componentNames_.size()) return false;
            hidden.push_back(hString(componentNames_[size_t(type)]));
        }
        if (!hidden.empty()) kv.emplace_back(hString("hidden_components"), hList(hidden));
        out = hMap(std::move(kv));
    } else if (n == "firework_explosion") {
        if (!explosionHash(r, out)) return false;
    } else if (n == "fireworks") {  // {flight_duration (an unsigned byte) unless 0, explosions unless empty}
        std::vector<std::pair<u32, u32>> kv;
        if (const i32 flight = r.varInt(); flight != 0) kv.emplace_back(hString("flight_duration"), crc32c({6, u8(flight)}));
        std::vector<u32> explosions;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            u32 h;
            if (!explosionHash(r, h)) return false;
            explosions.push_back(h);
        }
        if (!explosions.empty()) kv.emplace_back(hString("explosions"), hList(explosions));
        out = hMap(std::move(kv));
    } else if (n == "base_color") {
        const i32 dye = r.varInt();
        if (dye < 0 || dye > 15) return false;
        out = hString(kDyeNames[dye]);
    } else if (n == "banner_patterns") {  // [{pattern, color}...]
        std::vector<u32> layers;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            const i32 holder = r.varInt(), dye = r.varInt();  // a registry pattern (id + 1; 0: inline, not reproduced)
            if (holder <= 0 || size_t(holder - 1) >= bannerPatterns_.size() || dye < 0 || dye > 15) return false;
            layers.push_back(hMap({{hString("pattern"), hString(bannerPatterns_[size_t(holder - 1)])}, {hString("color"), hString(kDyeNames[dye])}}));
        }
        out = hList(layers);
    } else if (n == "trim") {  // {material, pattern}: registry holders (id + 1)
        const i32 material = r.varInt();
        if (material <= 0 || size_t(material - 1) >= trimMaterials_.size()) return false;
        const i32 pattern = r.varInt();
        if (pattern <= 0 || size_t(pattern - 1) >= trimPatterns_.size()) return false;
        out = hMap({{hString("material"), hString(trimMaterials_[size_t(material - 1)])},
                    {hString("pattern"), hString(trimPatterns_[size_t(pattern - 1)])}});
    } else if (n == "suspicious_stew_effects") {  // [{id, duration unless 160}...]
        std::vector<u32> effects;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            const std::string& effect = mobEffectName(r.varInt());
            const i32 duration = r.varInt();
            if (effect.empty()) return false;
            std::vector<std::pair<u32, u32>> kv{{hString("id"), hString(effect)}};
            if (duration != 160) kv.emplace_back(hString("duration"), hInt(duration));
            effects.push_back(hMap(std::move(kv)));
        }
        out = hList(effects);
    } else if (n == "pot_decorations") {  // {back?, left?, right?, front?}
        std::vector<std::pair<u32, u32>> kv;
        for (const char* side : {"back", "left", "right", "front"}) {
            if (!r.boolean()) continue;
            u32 h;
            if (!templateHash(r, h)) return false;
            kv.emplace_back(hString(side), h);
        }
        out = hMap(std::move(kv));
    } else if (n == "block_state") {  // {"property": "value"...}
        std::vector<std::pair<u32, u32>> kv;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            const std::string key(r.str());
            kv.emplace_back(hString(key), hString(std::string(r.str())));
        }
        out = hMap(std::move(kv));
    } else if (n == "bundle_contents" || n == "charged_projectiles") {  // a list of ItemStackTemplates
        std::vector<u32> items;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            u32 h;
            if (!templateHash(r, h)) return false;
            items.push_back(h);
        }
        out = hList(items);
    } else if (n == "container") {  // ItemContainerContents.CODEC: [{slot, item}...] of the filled slots in order
        std::vector<u32> slots;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            if (!r.boolean()) continue;
            u32 h;
            if (!templateHash(r, h)) return false;
            slots.push_back(hMap({{hString("slot"), hInt(i)}, {hString("item"), h}}));
        }
        out = hList(slots);
    } else if (n == "enchantments" || n == "stored_enchantments") {  // {"minecraft:sharpness": 3, ...}
        std::vector<std::pair<u32, u32>> kv;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) {
            const int id = r.varInt(), level = r.varInt();
            if (id < 0 || size_t(id) >= enchantments_.size()) return false;
            kv.emplace_back(hString(enchantments_[size_t(id)].id), hInt(level));
        }
        out = hMap(std::move(kv));
    } else {
        return false;
    }
    return r.ok && r.p == r.end;
}

// FireworkExplosion.CODEC: {shape, colors unless empty, fade_colors unless empty, has_trail / has_twinkle if set}.
bool Items::explosionHash(net::Reader& r, u32& out) const {
    static const char* const kShapes[5] = {"small_ball", "large_ball", "star", "creeper", "burst"};
    const i32 shape = r.varInt();
    if (shape < 0 || shape > 4) return false;
    std::vector<std::pair<u32, u32>> kv{{hString("shape"), hString(kShapes[shape])}};
    for (const char* key : {"colors", "fade_colors"}) {
        std::vector<u32> colors;
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i) colors.push_back(hInt(r.i32v()));
        if (!colors.empty()) kv.emplace_back(hString(key), hList(colors));
    }
    if (r.boolean()) kv.emplace_back(hString("has_trail"), hBool(true));
    if (r.boolean()) kv.emplace_back(hString("has_twinkle"), hBool(true));
    out = hMap(std::move(kv));
    return r.ok;
}

// ItemStackTemplate.CODEC (its map form): {id, count unless 1, components unless empty}; the patch as
// DataComponentPatch.CODEC: "minecraft:<type>" -> the value, "!minecraft:<type>" -> {} for a removed one.
bool Items::templateHash(net::Reader& r, u32& out) const {
    ItemStack t;
    t.item = r.varInt();
    t.count = r.varInt();
    if (!r.ok || !readPatch(r, t, 1)) return false;
    const std::string& id = name(t.item);
    if (id.empty()) return false;
    std::vector<std::pair<u32, u32>> kv{{hString("id"), hString(id)}};
    if (t.count != 1) kv.emplace_back(hString("count"), hInt(t.count));
    std::vector<std::pair<u32, u32>> patch;
    for (const ItemStack::Component& comp : t.added) {
        u32 h;
        if (!componentHash(comp, h)) return false;
        patch.emplace_back(hString("minecraft:" + componentName(comp.type)), h);
    }
    for (int type : t.removed) patch.emplace_back(hString("!minecraft:" + componentName(type)), crc32c({2, 3}));
    if (!patch.empty()) kv.emplace_back(hString("components"), hMap(std::move(patch)));
    out = hMap(std::move(kv));
    return true;
}

void Items::writeHashed(net::Writer& w, const ItemStack& s) const {
    w.boolean(!s.empty());
    if (s.empty()) return;
    w.varInt(s.item);
    w.varInt(s.count);
    w.varInt(i32(s.added.size()));
    for (const auto& c : s.added) {
        w.varInt(c.type);
        u32 h = 0;
        if (!componentHash(c, h)) h = 0;  // unknown: the server resyncs the slot
        w.u32v(h);
    }
    w.varInt(i32(s.removed.size()));
    for (int t : s.removed) w.varInt(t);
}

}  // namespace ui
