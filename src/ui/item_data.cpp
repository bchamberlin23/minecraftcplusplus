// Item data beyond the registries: facts only the game's code has (extracted once from the installed jar with a small
// Java program, like the gameplay rules), the item component reports (default components per item), and the tags /
// feature flags the server sends.
#include "assets/game_reports.h"
#include "ui/items.h"

#include <fstream>
#include <unordered_set>

namespace ui {

namespace {
constexpr int kFormat = 2;

// Runs with the game's classes (runGameProgram): potions' effects and base names, mob effect categories and
// attribute modifiers, attribute names and tooltip colours, the player's base attack values, item feature flags and
// the texts Item.appendHoverText adds without components (smithing templates, disc fragments), dye firework colours,
// entity / block entity types the tooltips name (peaceful, op-only data), tropical fish patterns, crafting remainders.
constexpr const char* kDumpSource = R"JAVA(
import com.google.gson.*;
import com.mojang.serialization.JsonOps;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.chat.Component;
import net.minecraft.network.chat.ComponentSerialization;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.effect.MobEffect;
import net.minecraft.world.effect.MobEffectInstance;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.ai.attributes.Attribute;
import net.minecraft.world.entity.ai.attributes.AttributeSupplier;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.animal.fish.TropicalFish;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.item.DyeColor;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.TooltipFlag;
import net.minecraft.world.item.alchemy.Potion;
import net.minecraft.world.item.component.TooltipDisplay;
import net.minecraft.world.level.block.entity.BlockEntityType;

public class ItemUiDump {
    static JsonElement text(Component c) { return ComponentSerialization.CODEC.encodeStart(JsonOps.INSTANCE, c).getOrThrow(); }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        JsonObject root = new JsonObject();
        root.addProperty("format", Integer.parseInt(args[1]));
        JsonObject potions = new JsonObject();
        for (Potion p : BuiltInRegistries.POTION) {
            JsonObject o = new JsonObject();
            o.addProperty("name", p.name());
            JsonArray effects = new JsonArray();
            for (MobEffectInstance e : p.getEffects()) {
                JsonArray a = new JsonArray();
                a.add(e.getEffect().unwrapKey().get().identifier().toString());
                a.add(e.getDuration());
                a.add(e.getAmplifier());
                effects.add(a);
            }
            o.add("effects", effects);
            potions.add(BuiltInRegistries.POTION.getKey(p).toString(), o);
        }
        root.add("potions", potions);
        JsonObject effects = new JsonObject();
        for (MobEffect e : BuiltInRegistries.MOB_EFFECT) {
            JsonObject o = new JsonObject();
            o.addProperty("category", e.getCategory().name().toLowerCase(Locale.ROOT));
            JsonArray mods = new JsonArray();
            e.createModifiers(0, (attribute, modifier) -> {
                JsonArray a = new JsonArray();
                a.add(attribute.unwrapKey().get().identifier().toString());
                a.add(modifier.amount());
                a.add(modifier.operation().id());
                mods.add(a);
            });
            o.add("modifiers", mods);
            effects.add(BuiltInRegistries.MOB_EFFECT.getKey(e).toString(), o);
        }
        root.add("effects", effects);
        JsonObject attributes = new JsonObject();
        for (Attribute a : BuiltInRegistries.ATTRIBUTE) {
            JsonObject o = new JsonObject();
            o.addProperty("key", a.getDescriptionId());
            o.addProperty("increase", a.getStyle(true).name().toLowerCase(Locale.ROOT));
            o.addProperty("decrease", a.getStyle(false).name().toLowerCase(Locale.ROOT));
            attributes.add(BuiltInRegistries.ATTRIBUTE.getKey(a).toString(), o);
        }
        root.add("attributes", attributes);
        AttributeSupplier player = Player.createAttributes().build();
        JsonObject base = new JsonObject();
        base.addProperty("attack_damage", player.getBaseValue(Attributes.ATTACK_DAMAGE));
        base.addProperty("attack_speed", player.getBaseValue(Attributes.ATTACK_SPEED));
        root.add("player", base);
        JsonObject items = new JsonObject();
        String vanilla = FeatureFlags.REGISTRY.toNames(FeatureFlags.VANILLA_SET).iterator().next().toString();
        for (Item item : BuiltInRegistries.ITEM) {
            JsonObject o = new JsonObject();
            JsonArray f = new JsonArray();
            for (Object n : FeatureFlags.REGISTRY.toNames(item.requiredFeatures()))
                if (!n.toString().equals(vanilla)) f.add(n.toString());
            if (f.size() > 0) o.add("features", f);
            if (item.getCraftingRemainder() != null)
                o.addProperty("remainder", BuiltInRegistries.ITEM.getKey(item.getCraftingRemainder().item().value()).toString());
            List<Component> lines = new ArrayList<>();
            try {
                item.appendHoverText(ItemStack.EMPTY, Item.TooltipContext.EMPTY, TooltipDisplay.DEFAULT, lines::add, TooltipFlag.NORMAL);
            } catch (Exception ex) {
            }
            if (!lines.isEmpty()) {
                JsonArray h = new JsonArray();
                for (Component c : lines) h.add(text(c));
                o.add("hover", h);
            }
            if (o.size() > 0) items.add(BuiltInRegistries.ITEM.getKey(item).toString(), o);
        }
        root.add("items", items);
        JsonArray dyes = new JsonArray();
        for (DyeColor d : DyeColor.values()) {
            JsonArray a = new JsonArray();
            a.add(d.getName());
            a.add(d.getFireworkColor());
            dyes.add(a);
        }
        root.add("dyes", dyes);
        JsonArray notPeaceful = new JsonArray(), opEntities = new JsonArray(), opBlockEntities = new JsonArray();
        for (EntityType<?> t : BuiltInRegistries.ENTITY_TYPE) {
            String n = BuiltInRegistries.ENTITY_TYPE.getKey(t).toString();
            if (!t.isAllowedInPeaceful()) notPeaceful.add(n);
            if (t.onlyOpCanSetNbt()) opEntities.add(n);
        }
        for (BlockEntityType<?> t : BuiltInRegistries.BLOCK_ENTITY_TYPE)
            if (t.onlyOpCanSetNbt()) opBlockEntities.add(BuiltInRegistries.BLOCK_ENTITY_TYPE.getKey(t).toString());
        root.add("not_peaceful", notPeaceful);
        root.add("op_entities", opEntities);
        root.add("op_block_entities", opBlockEntities);
        JsonObject fish = new JsonObject();
        JsonObject patterns = new JsonObject();
        for (TropicalFish.Pattern p : TropicalFish.Pattern.values()) patterns.addProperty(Integer.toString(p.getPackedId()), p.getSerializedName());
        fish.add("patterns", patterns);
        JsonArray common = new JsonArray();
        for (TropicalFish.Variant v : TropicalFish.COMMON_VARIANTS) {
            JsonArray a = new JsonArray();
            a.add(v.pattern().getPackedId());
            a.add(v.baseColor().getId());
            a.add(v.patternColor().getId());
            common.add(a);
        }
        fish.add("common", common);
        root.add("tropical_fish", fish);
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[0]), StandardCharsets.UTF_8)) {
            new Gson().toJson(root, w);
        }
    }
}
)JAVA";

std::string shortName(const std::string& n) { return n.substr(n.find(':') + 1); }

nlohmann::json readJsonFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return nlohmann::json::parse(f, nullptr, false);
}
}  // namespace

void Items::loadGameData(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir,
                         const fs::path& componentReports) {
    componentReports_ = componentReports;
    defaults_.clear();
    defaults_.resize(names_.size());
    const fs::path dir = cacheDir / version, file = dir / "item_ui.json";
    nlohmann::json j = readJsonFile(file);
    if (!j.is_object() || j.value("format", 0) != kFormat) {
        LOG("extracting item tooltip data with the game's code (one time, ~5 s)...");
        const fs::path tmp = dir / "item_ui.tmp";
        if (!runGameProgram(minecraftDir, version, "ItemUiDump", kDumpSource, {tmp.wstring(), std::to_wstring(kFormat)}, dir)) {
            LOG("warning: item tooltip data unavailable (potion effects, attribute lines)");
            return;
        }
        std::error_code ec;
        fs::rename(tmp, file, ec);
        j = readJsonFile(file);
        if (!j.is_object()) return;
    }
    gameData_ = std::move(j);
}

const nlohmann::json& Items::defaults(int item) const {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    if (item < 0 || size_t(item) >= defaults_.size() || componentReports_.empty()) return kEmpty;
    std::unique_ptr<nlohmann::json>& d = defaults_[size_t(item)];
    if (!d) {
        nlohmann::json j = readJsonFile(componentReports_ / (shortName(name(item)) + ".json"));
        d = std::make_unique<nlohmann::json>(j.is_object() && j.contains("components") && j["components"].is_object()
                                                 ? std::move(j["components"])
                                                 : nlohmann::json::object());
    }
    return *d;
}

Items::ComponentValue Items::component(const ItemStack& s, std::string_view name) const {
    ComponentValue v;
    const int type = componentType(name);
    if (type < 0 || s.item < 0) return v;
    for (const ItemStack::Component& c : s.added)
        if (c.type == type) {
            v.patched = &c.data;
            return v;
        }
    if (std::find(s.removed.begin(), s.removed.end(), type) != s.removed.end()) return v;
    const nlohmann::json& d = defaults(s.item);
    if (auto it = d.find("minecraft:" + std::string(name)); it != d.end()) v.fallback = &*it;
    return v;
}

int Items::componentCount(const ItemStack& s) const {
    std::unordered_set<int> types;
    for (const auto& [key, value] : defaults(s.item).items()) types.insert(componentType(std::string_view(key).substr(key.find(':') + 1)));
    for (const ItemStack::Component& c : s.added) types.insert(c.type);
    for (int t : s.removed) types.erase(t);
    types.erase(-1);
    return int(types.size());
}

void Items::setTags(const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<int>>>& tags) {
    for (const auto& [registry, map] : tags) tags_[registry] = map;
}

const std::vector<int>* Items::tag(std::string_view registry, std::string_view tag) const {
    auto r = tags_.find(std::string(registry));
    if (r == tags_.end()) return nullptr;
    auto t = r->second.find(tag.find(':') == std::string_view::npos ? "minecraft:" + std::string(tag) : std::string(tag));
    return t == r->second.end() ? nullptr : &t->second;
}

bool Items::itemHasTag(int item, std::string_view t) const {
    const std::vector<int>* ids = tag("minecraft:item", t);
    return ids && std::find(ids->begin(), ids->end(), item) != ids->end();
}

bool Items::itemEnabled(int item) const {
    auto items = gameData_.find("items");
    if (items == gameData_.end()) return true;
    auto it = items->find(name(item));
    if (it == items->end() || !it->contains("features")) return true;
    for (const auto& f : (*it)["features"])
        if (std::find(enabledFeatures_.begin(), enabledFeatures_.end(), f.get<std::string>()) == enabledFeatures_.end()) return false;
    return true;
}

int Items::craftingRemainder(int item) const {
    auto items = gameData_.find("items");
    if (items == gameData_.end()) return -1;
    auto it = items->find(name(item));
    return it == items->end() || !it->contains("remainder") ? -1 : id((*it)["remainder"].get<std::string>());
}

namespace {
int equipmentSlotId(std::string_view n) {
    static const char* const kNames[] = {"mainhand", "feet", "legs", "chest", "head", "offhand", "body", "saddle"};
    for (int i = 0; i < 8; ++i)
        if (n == kNames[i]) return i;
    return -1;
}
// Equippable: slot, then (after the sound, asset and overlay) the allowed entities as a tag or ids.
bool readEquippable(const Items& items, const ItemStack& s, int& slot, std::string& tag, std::vector<std::string>& ids, bool& any) {
    Items::ComponentValue v = items.component(s, "equippable");
    any = true;
    if (v.patched) {
        net::Reader r(reinterpret_cast<const u8*>(v.patched->data()), v.patched->size());
        slot = r.varInt();
        if (r.varInt() == 0) {  // inline sound event
            r.str();
            if (r.boolean()) r.f32();
        }
        if (r.boolean()) r.str();
        if (r.boolean()) r.str();
        if (r.boolean()) {
            any = false;
            const i32 c = r.varInt();
            if (c == 0) tag = std::string(r.str());
            for (i32 i = 0; i < c - 1 && r.ok; ++i) ids.push_back(std::to_string(r.varInt()));
        }
        return r.ok;
    }
    if (!v.fallback || !v.fallback->is_object()) return false;
    slot = equipmentSlotId(v.fallback->value("slot", ""));
    if (auto a = v.fallback->find("allowed_entities"); a != v.fallback->end()) {
        any = false;
        if (a->is_string()) {
            const std::string e = a->get<std::string>();
            if (!e.empty() && e[0] == '#') tag = e.substr(1);
            else ids.push_back(e);
        } else if (a->is_array()) {
            for (const auto& e : *a) ids.push_back(e.get<std::string>());
        }
    }
    return slot >= 0;
}
}  // namespace

int Items::equipmentSlot(const ItemStack& s) const {
    int slot = -1;
    std::string tag;
    std::vector<std::string> ids;
    bool any = true;
    return readEquippable(*this, s, slot, tag, ids, any) ? slot : -1;
}

bool Items::canBeEquippedBy(const ItemStack& s, std::string_view entityType) const {
    int slot = -1;
    std::string tag;
    std::vector<std::string> ids;
    bool any = true;
    if (!readEquippable(*this, s, slot, tag, ids, any)) return false;
    if (any) return true;
    const std::string type = entityType.find(':') == std::string_view::npos ? "minecraft:" + std::string(entityType) : std::string(entityType);
    const nlohmann::json& reg = reports_ ? reports_->registries() : nlohmann::json();
    int typeId = -1;
    if (reg.contains("minecraft:entity_type") && reg["minecraft:entity_type"]["entries"].contains(type))
        typeId = reg["minecraft:entity_type"]["entries"][type].value("protocol_id", -1);
    if (!tag.empty()) {
        const std::vector<int>* t = this->tag("minecraft:entity_type", tag);
        return t && std::find(t->begin(), t->end(), typeId) != t->end();
    }
    for (const std::string& id : ids)
        if (id == type || id == std::to_string(typeId)) return true;
    return false;
}

bool Items::hasEnchantmentEffect(const ItemStack& s, std::string_view effect) const {
    ComponentValue v = component(s, "enchantments");
    if (!v.patched) return false;
    net::Reader r(reinterpret_cast<const u8*>(v.patched->data()), v.patched->size());
    for (i32 k = 0, c = r.varInt(); k < c && r.ok; ++k) {
        const int e = r.varInt();
        r.varInt();
        if (e < 0 || size_t(e) >= enchantments_.size()) continue;
        auto it = enchantmentNbt_.find(enchantments_[size_t(e)].id);
        if (it != enchantmentNbt_.end() && it->second.find(effect) != std::string::npos) return true;  // a key of "effects"
    }
    return false;
}

}  // namespace ui
