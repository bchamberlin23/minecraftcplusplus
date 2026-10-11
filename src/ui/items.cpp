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
            out.push_back(std::move(info));
        }
    };
    load("minecraft:enchantment", enchantments_);
    load("minecraft:painting_variant", paintings_);
    load("minecraft:instrument", instruments_);
    bannerPatterns_.clear();
    for (const net::Client::RegistryEntry& e : client.registryEntries("minecraft:banner_pattern")) bannerPatterns_.push_back(e.id);
    registriesSet_ = true;
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
    if (s.maxDamage > 0 || s.damage > 0) return 1;
    return defaultMaxStack(shortName(name(s.item)));
}

int Items::maxDamage(const ItemStack& s) const {
    return s.maxDamage >= 0 ? s.maxDamage : toolDurability(shortName(name(s.item)));
}

Text Items::displayName(const ItemStack& s) const {
    if (s.hasCustomName) return s.customName.withStyle([] {
        Style st;
        st.italic = 1;
        return st;
    }());
    if (s.hasItemName) return s.itemName;
    const std::string n = shortName(name(s.item));
    for (const char* prefix : {"item.minecraft.", "block.minecraft."})
        if (auto it = lang_->find(prefix + n); it != lang_->end()) {
            if (s.potion >= 0 && n.find("potion") != std::string::npos) break;
            return Text(it->second);
        }
    // Potions and tipped arrows: item.minecraft.<item>.effect.<potion>.
    if (s.potion >= 0) {
        if (s.potion < int(potions_.size()))
            if (auto it = lang_->find("item.minecraft." + n + ".effect." + shortName(potions_[size_t(s.potion)]));
                it != lang_->end())
                return Text(it->second);
    }
    if (auto it = lang_->find("item.minecraft." + n); it != lang_->end()) return Text(it->second);
    return Text(n);
}

Text Items::hoverName(const ItemStack& s) const {
    // Rarity colour (common white, uncommon yellow, rare aqua, epic light purple), raised one step when enchanted.
    int rarity = s.rarity >= 0 ? s.rarity : 0;
    if (s.enchantments > 0) rarity = rarity <= 1 ? 2 : 3;
    static const u32 kRarity[4] = {0xFFFFFF, 0xFFFF55, 0x55FFFF, 0xFF55FF};
    return displayName(s).withStyle(colorStyle(kRarity[std::clamp(rarity, 0, 3)]));
}

std::vector<Text> Items::tooltip(const ItemStack& s) const {
    std::vector<Text> lines{hoverName(s)};
    auto shown = [&](const char* component) {
        const int t = componentType(component);
        return std::find(s.hiddenTooltip.begin(), s.hiddenTooltip.end(), t) == s.hiddenTooltip.end();
    };
    auto tr = [&](const std::string& key, const std::string& fallback) {
        auto it = lang_->find(key);
        return it == lang_->end() ? fallback : it->second;
    };
    const Style gray = colorStyle(0xAAAAAA);
    // InstrumentComponent.addToTooltip: the description in gray.
    if (s.instrument >= 0 && size_t(s.instrument) < instruments_.size() && shown("instrument"))
        lines.push_back(instruments_[size_t(s.instrument)].description.withStyle(gray));
    // SuspiciousStewEffects (creative tooltips): PotionContents.addPotionTooltip without the attribute lines.
    if (!s.stewEffects.empty() && shown("suspicious_stew_effects")) {
        static const char* const kHarmful[] = {"slowness", "mining_fatigue", "instant_damage", "nausea", "blindness", "hunger",
                                               "weakness", "poison", "wither", "levitation", "unluck", "darkness", "infested",
                                               "oozing", "weaving", "wind_charged"};
        for (auto [effect, ticks] : s.stewEffects) {
            if (effect < 0 || size_t(effect) >= mobEffects_.size()) continue;
            const std::string n = mobEffects_[size_t(effect)].substr(mobEffects_[size_t(effect)].find(':') + 1);
            bool harmful = false;
            for (const char* h : kHarmful) harmful |= n == h;
            std::string line = tr("effect.minecraft." + n, n);
            if (ticks > 20) {
                const int secs = ticks / 20;
                char dur[16];
                std::snprintf(dur, sizeof(dur), "%d:%02d", secs / 60, secs % 60);
                line = formatJava(tr("potion.withDuration", "%s (%s)"), {line, dur});
            }
            lines.push_back(Text(line, colorStyle(harmful ? 0xFF5555 : 0x5555FF)));
        }
    }
    // ItemEnchantments.addToTooltip (stored first, as on books): Enchantment.getFullname.
    for (const auto* list : {&s.storedEnchants, &s.enchantList}) {
        if (!shown(list == &s.storedEnchants ? "stored_enchantments" : "enchantments")) continue;
        for (auto [id, level] : *list) {
            if (id < 0 || size_t(id) >= enchantments_.size()) continue;
            const RegistryInfo& e = enchantments_[size_t(id)];
            const bool curse = e.id.find("curse") != std::string::npos;  // EnchantmentTags.CURSE (vanilla members)
            const Style st = colorStyle(curse ? 0xFF5555 : 0xAAAAAA);
            Text t = e.description.withStyle(st);
            if (level != 1 || e.maxLevel != 1) t.append(" " + tr("enchantment.level." + std::to_string(level), std::to_string(level)), st);
            lines.push_back(t);
        }
    }
    // HangingEntityItem.appendHoverText: a painting variant's title, author and size.
    if (s.painting >= 0 && size_t(s.painting) < paintings_.size() && shown("painting/variant")) {
        const RegistryInfo& p = paintings_[size_t(s.painting)];
        if (p.hasTitle) lines.push_back(p.title);
        if (p.hasAuthor) lines.push_back(p.author);
        lines.push_back(Text(formatJava(tr("painting.dimensions", "%sx%s"), {std::to_string(p.width), std::to_string(p.height)})));
    }
    for (const Text& l : s.lore) {
        Style st = colorStyle(0xAA00AA);
        st.italic = 1;
        lines.push_back(l.withStyle(st));
    }
    const int md = maxDamage(s);
    if (s.damage > 0 && md > 0) {
        lines.push_back(Text(formatJava(lang_->count("item.durability") ? lang_->at("item.durability") : "Durability: %s / %s",
                                        {std::to_string(md - s.damage), std::to_string(md)})));
    }
    return lines;
}

// --- protocol ---------------------------------------------------------------------------------------

bool Items::readTemplate(net::Reader& r, int depth) const {
    r.varInt();  // item
    r.varInt();  // count
    ItemStack tmp;
    return readPatch(r, tmp, depth + 1);
}

bool Items::skipValue(net::Reader& r, int type, int depth) const {
    if (depth > kMaxDepth || type < 0 || type >= int(componentNames_.size())) return false;
    const std::string n = shortName(componentNames_[size_t(type)]);
    auto list = [&](auto&& each) {
        const i32 cnt = r.varInt();
        for (i32 i = 0; i < cnt && r.ok; ++i)
            if (!each()) return false;
        return r.ok;
    };
    auto optional = [&](auto&& each) { return r.boolean() ? each() : true; };
    auto holder = [&] { return r.varInt() != 0; };  // registry reference; 0 = inline value (unsupported)
    auto effectDetails = [&](auto&& self) -> bool {
        r.varInt(), r.varInt();
        r.boolean(), r.boolean(), r.boolean();
        return r.boolean() ? self(self) : true;
    };

    static const std::unordered_set<std::string> nbt = {
        "custom_data", "custom_name", "item_name", "intangible_projectile", "map_decorations", "debug_stick_state",
        "bucket_entity_data", "recipes", "lock", "container_loot"};
    static const std::unordered_set<std::string> varint = {
        "max_stack_size", "max_damage", "damage", "repair_cost", "additional_trade_cost", "map_id",
        "map_post_processing", "rarity", "ominous_bottle_amplifier", "base_color", "dye", "enchantable"};
    static const std::unordered_set<std::string> unit = {"unbreakable", "creative_slot_lock", "glider", "waxed"};
    if (nbt.count(n)) return r.nbt(), r.ok;
    if (varint.count(n) || n.find('/') != std::string::npos) return r.varInt(), r.ok;
    if (unit.count(n)) return true;
    if (n == "minimum_attack_charge" || n == "potion_duration_scale") return r.f32(), r.ok;
    if (n == "enchantment_glint_override") return r.boolean(), r.ok;
    if (n == "item_model" || n == "tooltip_style" || n == "note_block_sound") return r.str(), r.ok;
    if (n == "use_effects") return r.boolean(), r.boolean(), r.f32(), r.ok;
    if (n == "lore") return list([&] { return r.nbt(), r.ok; });
    if (n == "enchantments" || n == "stored_enchantments") return list([&] { return r.varInt(), r.varInt(), r.ok; });
    if (n == "custom_model_data")
        return list([&] { return r.f32(), true; }) && list([&] { return r.boolean(), true; }) &&
               list([&] { return r.str(), true; }) && list([&] { return r.u32v(), true; });
    if (n == "tooltip_display") return r.boolean(), list([&] { return r.varInt(), true; });
    if (n == "dyed_color") return r.u32v(), r.ok;
    if (n == "potion_contents")
        return optional([&] { return r.varInt(), true; }) && optional([&] { return r.u32v(), true; }) &&
               list([&] { return holder() && effectDetails(effectDetails); }) && optional([&] { return r.str(), true; });
    if (n == "suspicious_stew_effects") return list([&] { return r.varInt(), r.varInt(), true; });
    if (n == "writable_book_content") return list([&] { return r.str(), optional([&] { return r.str(), true; }); });
    if (n == "written_book_content")
        return r.str(), optional([&] { return r.str(), true; }) && (r.str(), r.varInt(), true) &&
                            list([&] { return r.nbt(), optional([&] { return r.nbt(), true; }); }) && (r.boolean(), r.ok);
    if (n == "charged_projectiles" || n == "bundle_contents") return list([&] { return readTemplate(r, depth); });
    if (n == "container") return list([&] { return optional([&] { return readTemplate(r, depth); }); });
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
    if (n == "trim") return holder() && holder();
    if (n == "instrument") return holder();
    // HolderSet<Block> (ByteBufCodecs.holderSet): 0 + tag id, else count + 1 then registry ids.
    auto holderSet = [&] {
        const i32 c = r.varInt();
        if (c == 0) return r.str(), r.ok;
        for (i32 i = 0; i < c - 1 && r.ok; ++i) r.varInt();
        return r.ok;
    };
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
            if (r.boolean()) r.nbt();                      // NbtPredicate
            return r.varInt() == 0 && r.varInt() == 0 && r.ok;  // DataComponentMatchers: exact / partial (not decoded)
        });
    if (n == "painting/variant") return holder();
    if (n == "banner_patterns") return list([&] { return holder() && (r.varInt(), r.ok); });
    if (n == "pot_decorations") return list([&] { return r.varInt(), r.ok; });
    if (n == "provides_banner_patterns") return r.str(), r.ok;
    return false;  // profile, jukebox_playable, food, tool, equippable, ... (not decoded yet)
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
            v.boolean();
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
