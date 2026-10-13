// Item names and tooltips (ItemStack.getStyledHoverName / getTooltipLines / addDetailsToTooltip): every component's
// TooltipProvider in the game's order, read from the stack's patch or the item's default components (the item
// component reports), with the data the game's code holds extracted once (item_data.cpp).
#include "assets/game_reports.h"
#include "ui/items.h"
#include "world/nbt_dom.h"

#include <cmath>
#include <unordered_set>

namespace ui {

namespace {
std::string shortName(std::string_view n) { return std::string(n.substr(n.find(':') + 1)); }
net::Reader reader(const std::string& d) { return net::Reader(reinterpret_cast<const u8*>(d.data()), d.size()); }
Style fmt(const char* chatFormatting) { return colorStyle(u32(std::max(0, colorByName(chatFormatting)))); }
Style fmt(const char* chatFormatting, bool italic) {
    Style st = fmt(chatFormatting);
    st.italic = italic ? 1 : -1;
    return st;
}

// Network NBT (type byte + nameless payload) as a document.
bool netNbt(std::string_view nbt, NbtValue& out) {
    if (nbt.size() < 2 || u8(nbt[0]) != TagCompound) return false;
    std::string named;
    named.reserve(nbt.size() + 2);
    named += char(TagCompound);
    named += std::string(2, '\0');
    named.append(nbt.substr(1));
    return nbtRead(reinterpret_cast<const u8*>(named.data()), named.size(), out);
}
double nbtNumber(const NbtValue* v, double fallback) {
    if (!v) return fallback;
    if (v->type == TagFloat || v->type == TagDouble) return v->d;
    if (v->type >= TagByte && v->type <= TagLong) return double(v->i);
    return fallback;
}

// DecimalFormat("#.##") (HALF_EVEN, no trailing zeros).
std::string decimal(double v) {
    const double scaled = v * 100.0;
    double r = std::round(scaled);
    if (std::fabs(scaled - std::trunc(scaled)) == 0.5) r = 2.0 * std::round(scaled / 2.0);  // ties to even
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", r / 100.0);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

}  // namespace

// StringUtil.formatTickDuration at 20 ticks per second.
std::string tickDuration(int ticks) {
    int seconds = ticks / 20;
    int minutes = seconds / 60;
    seconds %= 60;
    const int hours = minutes / 60;
    minutes %= 60;
    char buf[32];
    if (hours > 0) std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, minutes, seconds);
    else std::snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
    return buf;
}

namespace {

const char* const kSlotGroups[] = {"any", "mainhand", "offhand", "hand", "feet", "legs", "chest", "head", "armor", "body", "saddle"};
int slotGroupId(std::string_view name) {
    for (int i = 0; i < int(std::size(kSlotGroups)); ++i)
        if (name == kSlotGroups[i]) return i;
    return -1;
}
int operationId(std::string_view name) { return name == "add_multiplied_base" ? 1 : name == "add_multiplied_total" ? 2 : 0; }

// MobEffectInstance as the tooltip needs it.
struct EffectLine {
    std::string effect;  // "minecraft:speed"
    int duration = 0, amplifier = 0;
};
// MobEffectInstance.Details after the effect id: amplifier, duration, ambient, visible, show icon, hidden effect.
bool readEffectDetails(net::Reader& r, EffectLine& e, int depth = 0) {
    if (depth > 8) return false;
    e.amplifier = r.varInt(), e.duration = r.varInt();
    r.boolean(), r.boolean(), r.boolean();
    if (r.boolean()) {
        EffectLine hidden;
        return readEffectDetails(r, hidden, depth + 1);
    }
    return r.ok;
}

struct PotionContents {
    int potion = -1;
    std::vector<EffectLine> custom;
    std::string customName;
    bool hasCustomName = false;
};
PotionContents readPotionContents(const Items& items, const Items::ComponentValue& v) {
    PotionContents p;
    if (!v.patched) return p;  // the default ({}) is empty
    net::Reader r = reader(*v.patched);
    if (r.boolean()) p.potion = r.varInt();
    if (r.boolean()) r.u32v();
    const i32 n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) {
        EffectLine e;
        e.effect = items.mobEffectName(r.varInt());
        if (!readEffectDetails(r, e)) break;
        p.custom.push_back(e);
    }
    if (r.ok && r.boolean()) p.hasCustomName = true, p.customName = std::string(r.str());
    return p;
}
}  // namespace

const std::string& Items::registryName(const char* registry, int id) const {
    static const std::string kNone;
    auto it = registryNames_.find(registry);
    if (it == registryNames_.end()) {
        std::vector<std::string>& names = registryNames_[registry];
        const nlohmann::json& reg = reports_ ? reports_->registries() : nlohmann::json();
        if (reg.is_object() && reg.contains(registry))
            for (const auto& e : reg[registry]["entries"].items()) {
                const int pid = e.value().value("protocol_id", -1);
                if (pid < 0) continue;
                if (size_t(pid) >= names.size()) names.resize(size_t(pid) + 1);
                names[size_t(pid)] = e.key();
            }
        it = registryNames_.find(registry);
    }
    return id >= 0 && size_t(id) < it->second.size() ? it->second[size_t(id)] : kNone;
}

Text Items::displayName(const ItemStack& s) const {
    auto tr = [&](const std::string& key, std::vector<Text> args = {}) { return translatable(key, args, lang_); };
    if (s.hasCustomName) return s.customName.withStyle([] {
        Style st;
        st.italic = 1;
        return st;
    }());
    // ItemStack.getCustomName: a written book's title.
    if (ComponentValue v = component(s, "written_book_content"); v.patched) {
        net::Reader r = reader(*v.patched);
        std::string title(r.str());
        if (r.ok && title.find_first_not_of(" \t\r\n") != std::string::npos) return Text(title);
    }
    const std::string n = shortName(name(s.item));
    // Item.getName overrides.
    if (n == "potion" || n == "splash_potion" || n == "lingering_potion" || n == "tipped_arrow") {
        if (ComponentValue v = component(s, "potion_contents"); v.present()) {  // PotionContents.getName
            const PotionContents p = readPotionContents(*this, v);
            std::string suffix = "empty";
            if (p.hasCustomName) {
                suffix = p.customName;
            } else if (p.potion >= 0) {
                const std::string& key = potionName(p.potion);
                suffix = shortName(key);
                if (auto pt = gameData_.find("potions"); pt != gameData_.end())
                    if (auto e = pt->find(key); e != pt->end()) suffix = e->value("name", suffix);
            }
            return tr("item.minecraft." + n + ".effect." + suffix);
        }
    }
    if (n == "compass" && component(s, "lodestone_tracker").present()) return tr("item.minecraft.lodestone_compass");
    if (n == "player_head")
        if (ComponentValue v = component(s, "profile"); v.patched) {  // ResolvableProfile: a full or partial profile
            net::Reader r = reader(*v.patched);
            std::string profileName;
            if (r.boolean()) r.skip(16), profileName = std::string(r.str());
            else if (r.boolean()) profileName = std::string(r.str());
            if (r.ok && !profileName.empty()) return tr("block.minecraft.player_head.named", {Text(profileName)});
        }
    if (n == "shield")
        if (ComponentValue v = component(s, "base_color"); v.patched) {
            net::Reader r = reader(*v.patched);
            const int dye = r.varInt();
            const auto dyes = gameData_.find("dyes");
            if (r.ok && dyes != gameData_.end() && dye >= 0 && size_t(dye) < dyes->size())
                return tr("item.minecraft.shield." + (*dyes)[size_t(dye)][0].get<std::string>());
        }
    if (s.hasItemName) return s.itemName;
    if (auto it = defaults(s.item).find("minecraft:item_name"); it != defaults(s.item).end()) return parseJsonComponent(*it, lang_);
    for (const char* prefix : {"item.minecraft.", "block.minecraft."})
        if (auto it = lang_->find(prefix + n); it != lang_->end()) return Text(it->second);
    return Text(n);
}

Text Items::hoverName(const ItemStack& s) const { return displayName(s).withStyle(colorStyle(rarityColor(s))); }

u32 Items::rarityColor(const ItemStack& s) const {
    // ItemStack.getRarity: common white, uncommon yellow, rare aqua, epic light purple; raised a step when enchanted.
    int rarity = 0;
    if (ComponentValue v = component(s, "rarity"); v.patched) {
        net::Reader r = reader(*v.patched);
        rarity = r.varInt();
    } else if (v.fallback && v.fallback->is_string()) {
        const std::string n = v.fallback->get<std::string>();
        rarity = n == "uncommon" ? 1 : n == "rare" ? 2 : n == "epic" ? 3 : 0;
    }
    bool enchanted = false;
    if (ComponentValue v = component(s, "enchantments"); v.patched) {
        net::Reader r = reader(*v.patched);
        enchanted = r.varInt() > 0;
    } else if (v.fallback) {
        enchanted = !v.fallback->empty();
    }
    if (enchanted) rarity = rarity <= 1 ? 2 : 3;
    static const u32 kRarity[4] = {0xFFFFFF, 0xFFFF55, 0x55FFFF, 0xFF55FF};
    return kRarity[std::clamp(rarity, 0, 3)];
}

std::vector<Text> Items::tooltip(const ItemStack& s, bool advanced, bool creative) const {
    // ItemStack.getTooltipLines; Item.shouldPrintOpWarning (block / spawn egg items): data only operators may set.
    auto opWarning = [&] {
        if (tooltipEnv.permissionLevel < 2) return false;
        const char* comp = isBlockItem(s.item) ? "block_entity_data" : name(s.item).ends_with("_spawn_egg") ? "entity_data" : nullptr;
        if (!comp) return false;
        ComponentValue v = component(s, comp);
        if (!v.patched) return false;
        net::Reader r = reader(*v.patched);
        const bool blockEntity = comp[0] == 'b';
        const std::string& type = registryName(blockEntity ? "minecraft:block_entity_type" : "minecraft:entity_type", r.varInt());
        const auto list = gameData_.find(blockEntity ? "op_block_entities" : "op_entities");
        if (list == gameData_.end()) return false;
        for (const auto& t : *list)
            if (t.get<std::string>() == type) return true;
        return false;
    };
    auto addOpWarning = [&](std::vector<Text>& out) {
        Style bold = fmt("red");
        bold.bold = 1;
        out.push_back(translatable("item.op_warning.line1", {}, lang_, bold));
        out.push_back(translatable("item.op_warning.line2", {}, lang_, fmt("red")));
        out.push_back(translatable("item.op_warning.line3", {}, lang_, fmt("red")));
    };
    std::vector<Text> lines;
    if (!creative && s.hideTooltip) {
        if (opWarning()) addOpWarning(lines);
        return lines;
    }
    lines.push_back(hoverName(s));
    addDetails(s, advanced, creative, tooltipEnv.player, lines);
    if (opWarning()) addOpWarning(lines);
    return lines;
}

void Items::addDetails(const ItemStack& s, bool advanced, bool creative, bool player, std::vector<Text>& out) const {
    const std::string itemId = name(s.item), n = shortName(itemId);
    auto shown = [&](const char* component) {  // TooltipDisplay.shows
        const int t = componentType(component);
        return std::find(s.hiddenTooltip.begin(), s.hiddenTooltip.end(), t) == s.hiddenTooltip.end();
    };
    auto get = [&](const char* comp) {  // present and shown
        ComponentValue v = component(s, comp);
        if (!shown(comp)) v = {};
        return v;
    };
    auto tr = [&](const std::string& key, std::vector<Text> args = {}, Style st = {}) { return translatable(key, args, lang_, st); };
    const Style gray = fmt("gray");
    const nlohmann::json* dyes = gameData_.contains("dyes") ? &gameData_["dyes"] : nullptr;
    auto dyeName = [&](int id) {
        return dyes && id >= 0 && size_t(id) < dyes->size() ? (*dyes)[size_t(id)][0].get<std::string>() : std::string("white");
    };
    auto indexOf = [](const std::vector<RegistryInfo>& v, const std::string& id) {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].id == id) return int(i);
        return -1;
    };
    // A registry holder: the registry entry (id + 1), or inline (0) whose description `inlineDesc` reads.
    auto holderDescription = [&](net::Reader& r, const std::vector<RegistryInfo>& reg, const std::function<Text(net::Reader&)>& inlineDesc) {
        const int id = r.varInt();
        if (id > 0) return id - 1 < int(reg.size()) ? reg[size_t(id - 1)].description : Text();
        return inlineDesc(r);
    };
    auto skipSound = [&](net::Reader& r) {  // Holder<SoundEvent>
        if (r.varInt() == 0) {
            r.str();
            if (r.boolean()) r.f32();
        }
    };
    // PotionContents.addPotionTooltip
    auto potionTooltip = [&](const std::vector<EffectLine>& effects, float durationScale) {
        struct Mod {
            std::string attribute;
            double amount;
            int op;
        };
        std::vector<Mod> mods;
        const nlohmann::json* effectData = gameData_.contains("effects") ? &gameData_["effects"] : nullptr;
        for (const EffectLine& e : effects) {
            const nlohmann::json* d = effectData && effectData->contains(e.effect) ? &(*effectData)[e.effect] : nullptr;
            if (d)
                for (const auto& m : d->value("modifiers", nlohmann::json::array()))
                    mods.push_back({m[0].get<std::string>(), m[1].get<double>() * (e.amplifier + 1), m[2].get<int>()});
            Text line = tr("effect." + shortName(e.effect.substr(0, e.effect.find(':'))) + "." + shortName(e.effect));
            if (e.amplifier > 0) line = tr("potion.withAmplifier", {line, tr("potion.potency." + std::to_string(e.amplifier))});
            if (e.duration == -1 || e.duration > 20) {
                const Text dur = e.duration == -1 ? tr("effect.duration.infinite")
                                                   : Text(tickDuration(int(std::floor(float(e.duration) * durationScale))));
                line = tr("potion.withDuration", {line, dur});
            }
            const std::string category = d ? d->value("category", "beneficial") : "beneficial";
            out.push_back(line.withStyle(fmt(category == "harmful" ? "red" : "blue")));
        }
        if (effects.empty()) out.push_back(tr("effect.none", {}, gray));
        if (mods.empty()) return;
        out.push_back(Text());
        out.push_back(tr("potion.whenDrank", {}, fmt("dark_purple")));
        const nlohmann::json* attrs = gameData_.contains("attributes") ? &gameData_["attributes"] : nullptr;
        for (const Mod& m : mods) {
            const std::string key = attrs && attrs->contains(m.attribute) ? (*attrs)[m.attribute].value("key", "") : "";
            const double display = m.op == 0 ? m.amount : m.amount * 100.0;
            if (m.amount > 0)
                out.push_back(tr("attribute.modifier.plus." + std::to_string(m.op), {Text(decimal(display)), tr(key)}, fmt("blue")));
            else if (m.amount < 0)
                out.push_back(tr("attribute.modifier.take." + std::to_string(m.op), {Text(decimal(-display)), tr(key)}, fmt("red")));
        }
    };
    auto hoverOf = [&](net::Reader& r, ItemStack& st) { return readTemplate(r, st); };

    // Item.appendHoverText: smithing templates, disc fragments (extracted), paintings (HangingEntityItem).
    if (auto items = gameData_.find("items"); items != gameData_.end())
        if (auto it = items->find(itemId); it != items->end() && it->contains("hover"))
            for (const auto& line : (*it)["hover"]) out.push_back(parseJsonComponent(line, lang_));
    if (n == "painting" && shown("painting/variant")) {
        if (s.painting >= 0 && size_t(s.painting) < paintings_.size()) {
            const RegistryInfo& p = paintings_[size_t(s.painting)];
            if (p.hasTitle) out.push_back(p.title);
            if (p.hasAuthor) out.push_back(p.author);
            out.push_back(tr("painting.dimensions", {Text(std::to_string(p.width)), Text(std::to_string(p.height))}));
        } else if (creative) {
            out.push_back(tr("painting.random", {}, gray));
        }
    }
    // TropicalFish.Pattern
    if (ComponentValue v = get("tropical_fish/pattern"); v.patched) {
        net::Reader r = reader(*v.patched);
        const int pattern = r.varInt();
        auto colorOf = [&](const char* comp) {
            ComponentValue c = component(s, comp);
            if (!c.patched) return 0;  // TropicalFish.DEFAULT_VARIANT: white
            net::Reader cr = reader(*c.patched);
            return cr.varInt();
        };
        const int base = colorOf("tropical_fish/base_color"), patternColor = colorOf("tropical_fish/pattern_color");
        const Style st = fmt("gray", true);
        int common = -1;
        if (auto fish = gameData_.find("tropical_fish"); fish != gameData_.end()) {
            const auto& list = (*fish)["common"];
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i][0] == pattern && list[i][1] == base && list[i][2] == patternColor) common = int(i);
            if (common >= 0) {
                out.push_back(tr("entity.minecraft.tropical_fish.predefined." + std::to_string(common), {}, st));
            } else {
                const std::string pname = (*fish)["patterns"].value(std::to_string(pattern), "kob");
                out.push_back(tr("entity.minecraft.tropical_fish.type." + pname, {}, st));
                Text colors = tr("color.minecraft." + dyeName(base));
                if (base != patternColor) colors.append(Text(", ")).append(tr("color.minecraft." + dyeName(patternColor)));
                out.push_back(colors.withStyle(st));
            }
        }
    }
    // InstrumentComponent
    if (ComponentValue v = get("instrument"); v.present()) {
        Text desc;
        if (v.patched) {
            net::Reader r = reader(*v.patched);
            desc = holderDescription(r, instruments_, [&](net::Reader& ir) {
                skipSound(ir);
                ir.f32(), ir.f32(), ir.varInt();
                return parseComponent(ir.nbt(), lang_);
            });
        } else if (v.fallback->is_string()) {
            const int id = indexOf(instruments_, v.fallback->get<std::string>());
            if (id >= 0) desc = instruments_[size_t(id)].description;
        }
        out.push_back(desc.withStyle(gray));
    }
    // MapId
    if (ComponentValue v = get("map_id"); v.patched) {
        net::Reader r = reader(*v.patched);
        const int id = r.varInt();
        int scale = 0;
        bool locked = false;
        if (!tooltipEnv.mapData || !tooltipEnv.mapData(id, scale, locked)) {
            out.push_back(tr("filled_map.unknown", {}, gray));
        } else {
            int post = -1;  // MapPostProcessing: 0 lock, 1 scale
            if (ComponentValue p = component(s, "map_post_processing"); p.patched) {
                net::Reader pr = reader(*p.patched);
                post = pr.varInt();
            }
            if (!s.hasCustomName && post < 0) out.push_back(tr("filled_map.id", {Text(std::to_string(id))}, gray));
            if (locked || post == 0) out.push_back(tr("filled_map.locked", {}, gray));
            if (advanced) {
                const int sc = std::min(scale + (post == 1 ? 1 : 0), 4);
                out.push_back(tr("filled_map.scale", {Text(std::to_string(1 << sc))}, gray));
                out.push_back(tr("filled_map.level", {Text(std::to_string(sc)), Text("4")}, gray));
            }
        }
    }
    // Bees
    if (ComponentValue v = get("bees"); v.present()) {
        int count = 0;
        if (v.patched) {
            net::Reader r = reader(*v.patched);
            count = r.varInt();
        } else if (v.fallback->is_array()) {
            count = int(v.fallback->size());
        }
        out.push_back(tr("container.beehive.bees", {Text(std::to_string(count)), Text("3")}, gray));
    }
    // SeededContainerLoot
    if (get("container_loot").present()) out.push_back(tr("item.container.loot_table.unknown"));
    // ItemContainerContents
    if (ComponentValue v = get("container"); v.patched) {
        net::Reader r = reader(*v.patched);
        const i32 n2 = r.varInt();
        int lineCount = 0, itemCount = 0;
        for (i32 i = 0; i < n2 && r.ok; ++i) {
            if (!r.boolean()) continue;
            ItemStack st;
            if (!hoverOf(r, st)) break;
            if (st.empty()) continue;
            ++itemCount;
            if (lineCount <= 4) {
                ++lineCount;
                out.push_back(tr("item.container.item_count", {Text(std::to_string(st.count)), displayName(st)}));
            }
        }
        Style italic;
        italic.italic = 1;
        if (itemCount - lineCount > 0) out.push_back(tr("item.container.more_items", {Text(std::to_string(itemCount - lineCount))}, italic));
    }
    // BannerPatternLayers: the first six layers' "<pattern>.<colour>"
    if (ComponentValue v = get("banner_patterns"); v.patched) {
        net::Reader r = reader(*v.patched);
        const i32 layers = r.varInt();
        const std::vector<RegistryInfo>& reg = registry("minecraft:banner_pattern");
        for (i32 i = 0; i < layers && r.ok; ++i) {
            std::string key;
            const int id = r.varInt();
            if (id > 0) {
                if (size_t(id - 1) < reg.size()) key = reg[size_t(id - 1)].translationKey;
            } else {
                r.str();
                key = std::string(r.str());
            }
            const int dye = r.varInt();
            if (i < 6) out.push_back(tr(key + "." + dyeName(dye), {}, gray));
        }
    }
    // PotDecorations: shown unless all four sides are empty (an all-brick pot lists its bricks)
    if (ComponentValue v = get("pot_decorations"); v.present()) {
        std::vector<Text> sides(4);  // back, left, right, front
        std::vector<bool> has(4, false);
        if (v.patched) {
            net::Reader r = reader(*v.patched);
            for (int i = 0; i < 4 && r.ok; ++i)
                if (r.boolean()) {
                    ItemStack st;
                    if (!hoverOf(r, st)) break;
                    has[size_t(i)] = true, sides[size_t(i)] = Text(displayName(st).plain(), gray);
                }
        } else if (v.fallback->is_object()) {
            const char* keys[4] = {"back", "left", "right", "front"};
            for (int i = 0; i < 4; ++i)
                if (v.fallback->contains(keys[i])) {
                    ItemStack st;
                    st.item = id((*v.fallback)[keys[i]].value("id", "")), st.count = 1;
                    has[size_t(i)] = st.item >= 0, sides[size_t(i)] = Text(displayName(st).plain(), gray);
                }
        }
        if (has[0] || has[1] || has[2] || has[3]) {
            out.push_back(Text());
            for (int i : {3, 1, 2, 0})
                if (has[size_t(i)]) out.push_back(sides[size_t(i)]);
        }
    }
    // WrittenBookContent
    if (ComponentValue v = get("written_book_content"); v.patched) {
        net::Reader r = reader(*v.patched);
        r.str();
        if (r.boolean()) r.str();  // filtered title
        const std::string author(r.str());
        const int generation = r.varInt();
        if (r.ok) {
            if (author.find_first_not_of(" \t\r\n") != std::string::npos) out.push_back(tr("book.byAuthor", {Text(author)}, gray));
            out.push_back(tr("book.generation." + std::to_string(generation), {}, gray));
        }
    }
    // ChargedProjectiles: runs of equal projectiles, then each projectile's own details
    if (ComponentValue v = get("charged_projectiles"); v.patched) {
        net::Reader r = reader(*v.patched);
        const i32 n2 = r.varInt();
        std::vector<ItemStack> list;
        for (i32 i = 0; i < n2 && r.ok; ++i) {
            ItemStack st;
            if (!hoverOf(r, st)) break;
            list.push_back(std::move(st));
        }
        for (size_t i = 0; i < list.size();) {
            size_t j = i + 1;
            while (j < list.size() && list[j].count == list[i].count && list[j].sameItemSameComponents(list[i])) ++j;
            const ItemStack& p = list[i];
            const int count = int(j - i);
            if (count == 1) {  // ItemStack.getDisplayName: [name] in the rarity colour
                const Text name = Text("[").append(displayName(p)).append(Text("]")).withStyle(colorStyle(rarityColor(p)));
                out.push_back(tr("item.minecraft.crossbow.projectile.single", {name}));
            } else {
                out.push_back(tr("item.minecraft.crossbow.projectile.multiple", {Text(std::to_string(count)), displayName(p)}));
            }
            std::vector<Text> sub;
            addDetails(p, false, false, false, sub);
            for (const Text& l : sub) out.push_back(Text("  ").append(l).withStyle(gray));
            i = j;
        }
    }
    // FireworkExplosion lines (shape, colours, fade, trail, twinkle)
    auto colorName = [&](i32 rgb) {
        if (dyes)
            for (const auto& d : *dyes)
                if (d[1].get<i32>() == rgb) return tr("item.minecraft.firework_star." + d[0].get<std::string>());
        return tr("item.minecraft.firework_star.custom_color");
    };
    struct Explosion {
        int shape = 0;
        std::vector<i32> colors, fades;
        bool trail = false, twinkle = false;
        bool operator==(const Explosion&) const = default;
    };
    auto readExplosion = [&](net::Reader& r) {
        Explosion e;
        e.shape = r.varInt();
        for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) e.colors.push_back(r.i32v());
        for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) e.fades.push_back(r.i32v());
        e.trail = r.boolean(), e.twinkle = r.boolean();
        return e;
    };
    auto additional = [&](const Explosion& e, std::vector<Text>& to) {
        if (!e.colors.empty()) {
            Text t;
            for (size_t k = 0; k < e.colors.size(); ++k) {
                if (k) t.append(Text(", "));
                t.append(colorName(e.colors[k]));
            }
            to.push_back(t.withStyle(gray));
        }
        if (!e.fades.empty()) {
            Text t = tr("item.minecraft.firework_star.fade_to").append(Text(" "));
            for (size_t k = 0; k < e.fades.size(); ++k) {
                if (k) t.append(Text(", "));
                t.append(colorName(e.fades[k]));
            }
            to.push_back(t.withStyle(gray));
        }
        if (e.trail) to.push_back(tr("item.minecraft.firework_star.trail", {}, gray));
        if (e.twinkle) to.push_back(tr("item.minecraft.firework_star.flicker", {}, gray));
    };
    static const char* const kShapes[] = {"small_ball", "large_ball", "star", "creeper", "burst"};
    auto shapeName = [&](int shape) { return tr(std::string("item.minecraft.firework_star.shape.") + kShapes[std::clamp(shape, 0, 4)]); };
    if (ComponentValue v = get("fireworks"); v.present()) {
        int flight = 0;
        std::vector<Explosion> explosions;
        if (v.patched) {
            net::Reader r = reader(*v.patched);
            flight = r.varInt();
            for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) explosions.push_back(readExplosion(r));
        } else if (v.fallback->is_object()) {
            flight = v.fallback->value("flight_duration", 0);
        }
        if (flight > 0) out.push_back(tr("item.minecraft.firework_rocket.flight_duration", {Text(std::to_string(flight))}, gray));
        for (size_t i = 0; i < explosions.size();) {  // Fireworks.addExplosionTooltip
            size_t j = i + 1;
            while (j < explosions.size() && explosions[j] == explosions[i]) ++j;
            const int count = int(j - i);
            Text shape = shapeName(explosions[i].shape);
            out.push_back(count == 1 ? tr("item.minecraft.firework_rocket.single_star", {shape}, gray)
                                     : tr("item.minecraft.firework_rocket.multiple_stars", {Text(std::to_string(count)), shape}, gray));
            std::vector<Text> sub;
            additional(explosions[i], sub);
            for (const Text& l : sub) out.push_back(Text("  ").append(l));
            i = j;
        }
    }
    if (ComponentValue v = get("firework_explosion"); v.patched) {
        net::Reader r = reader(*v.patched);
        const Explosion e = readExplosion(r);
        out.push_back(shapeName(e.shape).withStyle(gray));
        additional(e, out);
    }
    // PotionContents
    if (ComponentValue v = get("potion_contents"); v.present()) {
        const PotionContents p = readPotionContents(*this, v);
        std::vector<EffectLine> effects;
        if (p.potion >= 0)
            if (auto pt = gameData_.find("potions"); pt != gameData_.end())
                if (auto e = pt->find(potionName(p.potion)); e != pt->end())
                    for (const auto& x : e->value("effects", nlohmann::json::array()))
                        effects.push_back({x[0].get<std::string>(), x[1].get<int>(), x[2].get<int>()});
        effects.insert(effects.end(), p.custom.begin(), p.custom.end());
        float scale = 1;
        if (ComponentValue d = component(s, "potion_duration_scale"); d.patched) {
            net::Reader r = reader(*d.patched);
            scale = r.f32();
        } else if (d.fallback && d.fallback->is_number()) {
            scale = d.fallback->get<float>();
        }
        potionTooltip(effects, scale);
    }
    // JukeboxPlayable
    if (ComponentValue v = get("jukebox_playable"); v.present()) {
        const std::vector<RegistryInfo>& songs = registry("minecraft:jukebox_song");
        Text desc;
        if (v.patched) {
            net::Reader r = reader(*v.patched);
            desc = holderDescription(r, songs, [&](net::Reader& ir) {
                skipSound(ir);
                return parseComponent(ir.nbt(), lang_);
            });
        } else if (v.fallback->is_string()) {
            const int i = indexOf(songs, v.fallback->get<std::string>());
            if (i >= 0) desc = songs[size_t(i)].description;
        }
        out.push_back(desc.withStyle(gray));
    }
    // ArmorTrim: the upgrade title, the pattern in the material's style, the material
    if (ComponentValue v = get("trim"); v.patched) {
        net::Reader r = reader(*v.patched);
        const Text material = holderDescription(r, registry("minecraft:trim_material"), [&](net::Reader& ir) {
            ir.str();
            return parseComponent(ir.nbt(), lang_);
        });
        Text pattern = holderDescription(r, registry("minecraft:trim_pattern"), [&](net::Reader& ir) {
            ir.str();
            Text d = parseComponent(ir.nbt(), lang_);
            ir.boolean();
            return d;
        });
        if (r.ok) {
            const Style materialStyle = material.spans.empty() ? Style{} : material.spans[0].style;
            out.push_back(tr("item.minecraft.smithing_template.upgrade", {}, gray));
            Style ps = materialStyle;
            ps.events = nullptr;
            Text styled;
            for (const Span& sp : pattern.spans) styled.spans.push_back({sp.text, ps.over(sp.style)});
            out.push_back(Text(" ").append(styled));
            out.push_back(Text(" ").append(material));
        }
    }
    // ItemEnchantments (stored, then the item's): the #tooltip_order enchantments first, then the rest
    auto enchantmentLines = [&](const char* comp) {
        ComponentValue v = get(comp);
        if (!v.patched) return;
        std::vector<std::pair<int, int>> list;
        net::Reader r = reader(*v.patched);
        for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) {
            const int e = r.varInt(), level = r.varInt();
            list.emplace_back(e, level);
        }
        const std::vector<int>* order = tag("minecraft:enchantment", "minecraft:tooltip_order");
        const std::vector<int>* curses = tag("minecraft:enchantment", "minecraft:curse");
        auto line = [&](int e, int level) {
            if (e < 0 || size_t(e) >= enchantments_.size()) return;
            const RegistryInfo& info = enchantments_[size_t(e)];
            const bool curse = curses ? std::find(curses->begin(), curses->end(), e) != curses->end() : info.id.find("curse") != std::string::npos;
            const Style st = fmt(curse ? "red" : "gray");
            Text t = info.description.withStyle(st);
            if (level != 1 || info.maxLevel != 1) t.append(Text(" ")).append(tr("enchantment.level." + std::to_string(level)).withStyle(st));
            out.push_back(t);
        };
        if (order)
            for (int e : *order)
                for (auto [id2, level] : list)
                    if (id2 == e && level > 0) line(e, level);
        for (auto [id2, level] : list)
            if (!order || std::find(order->begin(), order->end(), id2) == order->end()) line(id2, level);
    };
    enchantmentLines("stored_enchantments");
    enchantmentLines("enchantments");
    // DyedItemColor
    if (ComponentValue v = get("dyed_color"); v.patched) {
        net::Reader r = reader(*v.patched);
        const u32 rgb = r.u32v() & 0xFFFFFF;
        char hex[16];
        std::snprintf(hex, sizeof(hex), "#%06X", rgb);
        out.push_back(advanced ? tr("item.color", {Text(hex)}, gray) : tr("item.dyed", {}, fmt("gray", true)));
    }
    // ResolvableProfile.Dynamic: a partial profile with only a name or only an id (and no properties)
    if (ComponentValue v = get("profile"); v.patched) {
        net::Reader r = reader(*v.patched);
        if (!r.boolean()) {
            const bool hasName = r.boolean();
            if (hasName) r.str();
            const bool hasId = r.boolean();
            if (hasId) r.skip(16);
            if (r.ok && r.varInt() == 0 && hasName != hasId) out.push_back(tr("component.profile.dynamic", {}, gray));
        }
    }
    // ItemLore
    if (shown("lore"))
        for (const Text& l : s.lore) out.push_back(l.withStyle(fmt("dark_purple", true)));
    // SulfurCubeContent
    if (ComponentValue v = get("sulfur_cube_content"); v.patched) {
        net::Reader r = reader(*v.patched);
        ItemStack st;
        if (hoverOf(r, st)) out.push_back(tr("entity.minecraft.sulfur_cube.content", {displayName(st)}, fmt("gray", true)));
    }
    // Attribute modifiers per slot group: the item's (patched or default), then its enchantments' attribute effects
    if (shown("attribute_modifiers")) {
        struct Mod {
            std::string attribute;  // "minecraft:attack_damage"
            std::string id;         // modifier id
            double amount = 0;
            int op = 0, slot = 0, display = 0;  // display: 0 default, 1 hidden, 2 override text
            Text override;
        };
        std::vector<Mod> mods;
        if (ComponentValue v = component(s, "attribute_modifiers"); v.patched) {
            net::Reader r = reader(*v.patched);
            for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) {
                Mod m;
                m.attribute = registryName("minecraft:attribute", r.varInt());
                m.id = std::string(r.str());
                m.amount = r.f64(), m.op = r.varInt(), m.slot = r.varInt(), m.display = r.varInt();
                if (m.display == 2) m.override = parseComponent(r.nbt(), lang_);
                if (r.ok) mods.push_back(std::move(m));
            }
        } else if (v.fallback && v.fallback->is_array()) {
            for (const auto& j : *v.fallback) {
                Mod m;
                m.attribute = j.value("type", "");
                m.id = j.value("id", "");
                m.amount = j.value("amount", 0.0);
                m.op = operationId(j.value("operation", "add_value"));
                m.slot = std::max(0, slotGroupId(j.value("slot", "any")));
                if (j.contains("display")) {
                    const std::string type = j["display"].value("type", "default");
                    m.display = type == "hidden" ? 1 : type == "override" ? 2 : 0;
                    if (m.display == 2 && j["display"].contains("value")) m.override = parseJsonComponent(j["display"]["value"], lang_);
                }
                mods.push_back(std::move(m));
            }
        }
        // EnchantmentHelper.forEachModifier: minecraft:attributes effects of the item's enchantments in their slots.
        std::vector<Mod> enchantMods;
        if (ComponentValue v = component(s, "enchantments"); v.patched) {
            net::Reader r = reader(*v.patched);
            for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) {
                const int e = r.varInt(), level = r.varInt();
                if (e < 0 || size_t(e) >= enchantments_.size()) continue;
                auto it = enchantmentNbt_.find(enchantments_[size_t(e)].id);
                NbtValue root;
                if (it == enchantmentNbt_.end() || !netNbt(it->second, root)) continue;
                std::vector<int> slots;
                if (NbtValue* sl = root.find("slots"))
                    for (const NbtValue& x : sl->items) slots.push_back(slotGroupId(x.s));
                NbtValue* effects = root.find("effects");
                NbtValue* attrs = effects ? effects->find("minecraft:attributes") : nullptr;
                if (!attrs) continue;
                for (NbtValue& a : attrs->items) {
                    NbtValue* amount = a.find("amount");
                    double value = 0;  // LevelBasedValue.calculate
                    std::function<double(NbtValue*)> calc = [&](NbtValue* lv) -> double {
                        if (!lv) return 0;
                        if (lv->type != TagCompound) return nbtNumber(lv, 0);
                        const NbtValue* t = lv->find("type");
                        const std::string type = t ? shortName(t->s) : "";
                        if (type == "linear")
                            return nbtNumber(lv->find("base"), 0) + nbtNumber(lv->find("per_level_above_first"), 0) * (level - 1);
                        if (type == "levels_squared") return double(level * level) + nbtNumber(lv->find("added"), 0);
                        if (type == "clamped")
                            return std::clamp(calc(lv->find("value")), nbtNumber(lv->find("min"), 0), nbtNumber(lv->find("max"), 0));
                        if (type == "fraction") {
                            const double d = calc(lv->find("denominator"));
                            return d == 0 ? 0 : calc(lv->find("numerator")) / d;
                        }
                        if (type == "lookup") {
                            NbtValue* values = lv->find("values");
                            if (values && level >= 1 && size_t(level) <= values->items.size()) return nbtNumber(&values->items[size_t(level - 1)], 0);
                            return calc(lv->find("fallback"));
                        }
                        return 0;
                    };
                    value = calc(amount);
                    const NbtValue* attribute = a.find("attribute");
                    const NbtValue* op = a.find("operation");
                    for (int sl : slots) {
                        Mod m;
                        m.attribute = attribute ? attribute->s : "";
                        m.amount = value, m.op = op ? operationId(op->s) : 0, m.slot = std::max(0, sl);
                        enchantMods.push_back(std::move(m));
                    }
                }
            }
        }
        const nlohmann::json* attrs = gameData_.contains("attributes") ? &gameData_["attributes"] : nullptr;
        const nlohmann::json* base = gameData_.contains("player") ? &gameData_["player"] : nullptr;
        auto apply = [&](const Mod& m) {  // ItemAttributeModifiers.Display
            if (m.display == 1) return;
            if (m.display == 2) {
                out.push_back(m.override);
                return;
            }
            const nlohmann::json* a = attrs && attrs->contains(m.attribute) ? &(*attrs)[m.attribute] : nullptr;
            const Text attrName = tr(a ? a->value("key", "") : "attribute.name." + shortName(m.attribute));
            double amount = m.amount;
            bool withBase = false;
            if (player && base) {
                if (m.id == "minecraft:base_attack_damage") amount += base->value("attack_damage", 1.0), withBase = true;
                else if (m.id == "minecraft:base_attack_speed") amount += base->value("attack_speed", 4.0), withBase = true;
            }
            const double display = m.op != 0 ? amount * 100.0 : m.attribute == "minecraft:knockback_resistance" ? amount * 10.0 : amount;
            if (withBase) {
                out.push_back(Text(" ").append(tr("attribute.modifier.equals." + std::to_string(m.op), {Text(decimal(display)), attrName})).withStyle(fmt("dark_green")));
            } else if (amount > 0) {
                out.push_back(tr("attribute.modifier.plus." + std::to_string(m.op), {Text(decimal(display)), attrName},
                                 fmt(a ? a->value("increase", "blue").c_str() : "blue")));
            } else if (amount < 0) {
                out.push_back(tr("attribute.modifier.take." + std::to_string(m.op), {Text(decimal(-display)), attrName},
                                 fmt(a ? a->value("decrease", "red").c_str() : "red")));
            }
        };
        for (int slot = 0; slot < int(std::size(kSlotGroups)); ++slot) {
            bool first = true;
            for (const std::vector<Mod>* list : {&mods, &enchantMods})
                for (const Mod& m : *list) {
                    if (m.slot != slot || m.display == 1) continue;
                    if (first) {
                        out.push_back(Text());
                        out.push_back(tr(std::string("item.modifiers.") + kSlotGroups[slot], {}, gray));
                        first = false;
                    }
                    apply(m);
                }
        }
    }
    if (get("intangible_projectile").present()) out.push_back(tr("item.intangible", {}, gray));
    if (get("unbreakable").present()) out.push_back(tr("item.unbreakable", {}, fmt("blue")));
    // SignText (front / back): nothing for an empty sign
    for (const char* side : {"sign_text_front", "sign_text_back"}) {
        ComponentValue v = get(side);
        if (!v.patched) continue;  // the default is the empty sign
        net::Reader r = reader(*v.patched);
        std::vector<Text> messages;
        for (int i = 0; i < 4 && r.ok; ++i) messages.push_back(parseComponent(r.nbt(), lang_));
        bool filteredDiffer = false;
        if (r.boolean())
            for (int i = 0; i < 4 && r.ok; ++i) filteredDiffer |= !parseComponent(r.nbt(), lang_).plain().empty();
        const int color = r.varInt();
        const bool glowing = r.boolean();
        bool empty = color == 15 && !glowing && !filteredDiffer;  // SignText.EMPTY: black, no glow, empty lines
        for (const Text& m : messages) empty &= m.plain().empty();
        if (!r.ok || empty) continue;
        out.push_back(tr(side[10] == 'f' ? "sign.front_text" : "sign.back_text", {}, gray));
        for (const Text& m : messages) out.push_back(Text("  ").append(m));
    }
    // OminousBottleAmplifier: Bad Omen for 120000 ticks
    if (ComponentValue v = get("ominous_bottle_amplifier"); v.present()) {
        int amplifier = 0;
        if (v.patched) {
            net::Reader r = reader(*v.patched);
            amplifier = r.varInt();
        } else if (v.fallback->is_number_integer()) {
            amplifier = v.fallback->get<int>();
        }
        potionTooltip({{"minecraft:bad_omen", 120000, amplifier}}, 1.0f);
    }
    // SuspiciousStewEffects (creative tooltips only)
    if (ComponentValue v = get("suspicious_stew_effects"); v.patched && creative) {
        std::vector<EffectLine> effects;
        net::Reader r = reader(*v.patched);
        for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) {
            EffectLine e;
            e.effect = mobEffectName(r.varInt());
            e.duration = r.varInt();
            effects.push_back(e);
        }
        potionTooltip(effects, 1.0f);
    }
    // BlockItemStateProperties: a beehive's honey level
    if (ComponentValue v = get("block_state"); v.present()) {
        std::string honey;
        if (v.patched) {
            net::Reader r = reader(*v.patched);
            for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) {
                const std::string key(r.str()), value(r.str());
                if (key == "honey_level") honey = value;
            }
        } else if (v.fallback->is_object()) {
            honey = v.fallback->value("honey_level", "");
        }
        if (!honey.empty() && honey.find_first_not_of("0123456789") == std::string::npos)
            out.push_back(tr("container.beehive.honey", {Text(honey), Text("5")}, gray));
    }
    // TypedEntityData (spawn eggs): not allowed in peaceful
    if (ComponentValue v = get("entity_data"); v.present() && tooltipEnv.peaceful) {
        std::string type;
        if (v.patched) {
            net::Reader r = reader(*v.patched);
            type = registryName("minecraft:entity_type", r.varInt());
        } else if (v.fallback->is_object()) {
            type = v.fallback->value("id", "");
        }
        if (auto np = gameData_.find("not_peaceful"); np != gameData_.end())
            for (const auto& t : *np)
                if (t.get<std::string>() == type) out.push_back(tr("item.spawn_egg.peaceful", {}, fmt("red")));
    }
    // Spawner.appendHoverText (spawner, trial spawner)
    if ((n == "spawner" || n == "trial_spawner") && shown("block_entity_data")) {
        Text spawns;
        bool has = false;
        if (ComponentValue v = component(s, "block_entity_data"); v.patched) {
            net::Reader r = reader(*v.patched);
            r.varInt();
            NbtValue root;
            if (netNbt(r.nbt(), root))
                if (NbtValue* sd = root.find("SpawnData"))
                    if (NbtValue* e = sd->find("entity"))
                        if (NbtValue* idv = e->find("id"); idv && idv->type == TagString) {
                            const std::string type = idv->s.find(':') == std::string::npos ? "minecraft:" + idv->s : idv->s;
                            spawns = tr("entity." + type.substr(0, type.find(':')) + "." + shortName(type), {}, gray);
                            has = true;
                        }
        }
        if (has) {
            out.push_back(spawns);
        } else {
            out.push_back(Text());
            out.push_back(tr("block.minecraft.spawner.desc1", {}, gray));
            out.push_back(Text(" ").append(tr("block.minecraft.spawner.desc2", {}, fmt("blue"))));
        }
    }
    // AdventureModePredicate (can_break / can_place_on): the predicates' blocks, or "unknown" when one has none
    for (const char* comp : {"can_break", "can_place_on"}) {
        ComponentValue v = get(comp);
        if (!v.patched) continue;
        net::Reader r = reader(*v.patched);
        std::vector<Text> names;
        std::unordered_set<int> seen;
        bool unknown = false;
        for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) {
            if (r.boolean()) {  // blocks: a tag or ids
                std::vector<int> blocks;
                const i32 hs = r.varInt();
                if (hs == 0) {
                    if (const std::vector<int>* t = tag("minecraft:block", r.str())) blocks = *t;
                } else {
                    for (i32 j = 0; j < hs - 1 && r.ok; ++j) blocks.push_back(r.varInt());
                }
                for (int b : blocks)
                    if (seen.insert(b).second)
                        names.push_back(tr("block." + shortName(registryName("minecraft:block", b).substr(0, registryName("minecraft:block", b).find(':'))) + "." +
                                               shortName(registryName("minecraft:block", b)),
                                           {}, fmt("dark_gray")));
            } else {
                unknown = true;
            }
            if (r.boolean())  // properties
                for (i32 j = 0, pc = r.varInt(); j < pc && r.ok; ++j) {
                    r.str();
                    if (r.boolean()) r.str();
                    else {
                        if (r.boolean()) r.str();
                        if (r.boolean()) r.str();
                    }
                }
            if (r.boolean()) r.nbt();
            for (i32 j = 0, ec = r.varInt(); j < ec && r.ok; ++j) skipValue(r, r.varInt(), 1);
            for (i32 j = 0, pc = r.varInt(); j < pc && r.ok; ++j) r.boolean(), r.varInt(), r.nbt();
        }
        if (!r.ok) continue;
        out.push_back(Text());
        out.push_back(tr(comp[4] == 'b' ? "item.canBreak" : "item.canPlace", {}, gray));
        if (unknown) out.push_back(tr("item.canUse.unknown", {}, gray));
        else out.insert(out.end(), names.begin(), names.end());
    }
    if (advanced) {
        const int md = maxDamage(s);
        if (s.damage > 0 && md > 0 && shown("damage"))
            out.push_back(tr("item.durability", {Text(std::to_string(md - s.damage)), Text(std::to_string(md))}));
        out.push_back(Text(itemId, fmt("dark_gray")));
        if (const int count = componentCount(s); count > 0) out.push_back(tr("item.components", {Text(std::to_string(count))}, fmt("dark_gray")));
    }
    if (player && !itemEnabled(s.item)) out.push_back(tr("item.disabled", {}, fmt("red")));
}

}  // namespace ui
