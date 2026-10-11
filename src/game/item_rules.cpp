#include "game/item_rules.h"

#include "assets/game_reports.h"

#include <json/json.hpp>

#include <algorithm>

namespace {

constexpr int kFormat = 3;  // bump when the dump's layout changes

// Run with the game's code on the class path (`java GameplayDump.java <out> <format>`). Block tags in tool rules
// are resolved from the jar's data files (tags are not bound without a server).
constexpr const char* kDumpSource = R"JAVA(
import com.google.gson.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.jar.*;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.entity.EntityAttachment;
import net.minecraft.world.entity.EntityAttachments;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.material.PushReaction;

public class GameplayDump {
    static <T extends Comparable<T>> String value(BlockState s, Property<T> p) { return p.getName(s.getValue(p)); }

    static String key(String name, BlockState s) {
        List<String> props = new ArrayList<>();
        for (Property<?> p : s.getProperties()) props.add(p.getName() + "=" + value(s, p));
        if (props.isEmpty()) return name;
        Collections.sort(props);
        return name + "[" + String.join(",", props) + "]";
    }

    static JsonElement read(JarFile jar, String path) throws IOException {
        JarEntry e = jar.getJarEntry(path);
        if (e == null) return null;
        try (Reader r = new InputStreamReader(jar.getInputStream(e), StandardCharsets.UTF_8)) { return JsonParser.parseReader(r); }
    }

    static void resolveTag(JarFile jar, String tag, Set<String> out, Set<String> seen) throws IOException {
        if (!seen.add(tag)) return;
        String id = tag.startsWith("minecraft:") ? tag.substring(10) : tag;
        JsonElement j = read(jar, "data/minecraft/tags/block/" + id + ".json");
        if (j == null) return;
        for (JsonElement v : j.getAsJsonObject().getAsJsonArray("values")) {
            String s = v.isJsonObject() ? v.getAsJsonObject().get("id").getAsString() : v.getAsString();
            if (s.startsWith("#")) resolveTag(jar, s.substring(1), out, seen);
            else out.add(s.contains(":") ? s : "minecraft:" + s);
        }
    }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        JsonObject root = new JsonObject();
        root.addProperty("format", Integer.parseInt(args[1]));
        JsonObject blocks = new JsonObject(), states = new JsonObject(), push = new JsonObject();
        for (Block block : BuiltInRegistries.BLOCK) {
            String name = BuiltInRegistries.BLOCK.getKey(block).toString();
            BlockState d = block.defaultBlockState();
            if (d.getPistonPushReaction() != PushReaction.PUSH_PULL || d.hasBlockEntity()) {
                JsonArray p = new JsonArray();
                p.add(d.getPistonPushReaction().ordinal());
                p.add(d.hasBlockEntity() ? 1 : 0);
                push.add(name, p);
            }
            float h = d.getDestroySpeed(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
            boolean req = d.requiresCorrectToolForDrops();
            JsonArray a = new JsonArray();
            a.add(h);
            a.add(req ? 1 : 0);
            blocks.add(name, a);
            for (BlockState s : block.getStateDefinition().getPossibleStates()) {
                float hs = s.getDestroySpeed(EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
                if (hs == h && s.requiresCorrectToolForDrops() == req) continue;
                JsonArray b = new JsonArray();
                b.add(hs);
                b.add(s.requiresCorrectToolForDrops() ? 1 : 0);
                states.add(key(name, s), b);
            }
        }
        root.add("blocks", blocks);
        root.add("states", states);
        root.add("push", push);
        // Every block tag of the game, resolved to block names.
        File jarFile = new File(Block.class.getProtectionDomain().getCodeSource().getLocation().toURI());
        JsonObject tags = new JsonObject();
        try (JarFile jar = new JarFile(jarFile)) {
            final String prefix = "data/minecraft/tags/block/";
            for (Enumeration<JarEntry> en = jar.entries(); en.hasMoreElements();) {
                String n = en.nextElement().getName();
                if (!n.startsWith(prefix) || !n.endsWith(".json")) continue;
                String tag = "minecraft:" + n.substring(prefix.length(), n.length() - 5);
                Set<String> members = new TreeSet<>();
                resolveTag(jar, tag, members, new HashSet<>());
                JsonArray m = new JsonArray();
                for (String s : members) m.add(s);
                tags.add(tag, m);
            }
        }
        root.add("tags", tags);
        // Entity dimensions and attachment points (passenger seats, where a passenger sits on its vehicle).
        JsonObject entities = new JsonObject();
        for (EntityType<?> type : BuiltInRegistries.ENTITY_TYPE) {
            JsonObject o = new JsonObject();
            o.addProperty("w", type.getDimensions().width());
            o.addProperty("h", type.getDimensions().height());
            o.addProperty("eye", type.getDimensions().eyeHeight());
            EntityAttachments a = type.getDimensions().attachments();
            JsonArray seats = new JsonArray();
            for (int i = 0; i < 4; ++i) {
                Vec3 p = a.getNullable(EntityAttachment.PASSENGER, i, 0.0F);
                if (p == null) break;
                JsonArray v = new JsonArray();
                v.add(p.x); v.add(p.y); v.add(p.z);
                seats.add(v);
            }
            o.add("passengers", seats);
            Vec3 veh = a.getNullable(EntityAttachment.VEHICLE, 0, 0.0F);
            if (veh != null) {
                JsonArray v = new JsonArray();
                v.add(veh.x); v.add(veh.y); v.add(veh.z);
                o.add("vehicle", v);
            }
            entities.add(BuiltInRegistries.ENTITY_TYPE.getKey(type).toString(), o);
        }
        root.add("entities", entities);
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[0]), StandardCharsets.UTF_8)) {
            new GsonBuilder().disableHtmlEscaping().create().toJson(root, w);
        }
        System.exit(0);
    }
}
)JAVA";

nlohmann::json readJson(const fs::path& p) {
    std::vector<u8> bytes;
    if (!readFile(p, bytes)) return nullptr;
    return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
}

ItemRules::Anim animFromName(const std::string& n) {
    static const std::pair<const char*, ItemRules::Anim> kNames[] = {
        {"eat", ItemRules::Anim::Eat},         {"drink", ItemRules::Anim::Drink},       {"block", ItemRules::Anim::Block},
        {"bow", ItemRules::Anim::Bow},         {"trident", ItemRules::Anim::Trident},   {"crossbow", ItemRules::Anim::Crossbow},
        {"spyglass", ItemRules::Anim::Spyglass}, {"toot_horn", ItemRules::Anim::TootHorn}, {"brush", ItemRules::Anim::Brush},
        {"bundle", ItemRules::Anim::Bundle},   {"spear", ItemRules::Anim::Spear}};
    for (const auto& [k, v] : kNames)
        if (n == k) return v;
    return ItemRules::Anim::None;
}

}  // namespace

bool ItemRules::load(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir,
                     const fs::path& componentReports) {
    try {
        return loadImpl(minecraftDir, version, cacheDir, componentReports);
    } catch (const std::exception& ex) {
        LOG("warning: gameplay rules unreadable (%s)", ex.what());
        blocks_.clear();
        return false;
    }
}

bool ItemRules::loadImpl(const fs::path& minecraftDir, const std::string& version, const fs::path& cacheDir,
                         const fs::path& componentReports) {
    componentReports_ = componentReports;
    const fs::path dir = cacheDir / version, file = dir / "gameplay_rules.json";
    nlohmann::json j = readJson(file);
    if (!j.is_object() || j.value("format", 0) != kFormat) {
        LOG("extracting block hardness and tags with the game's code (one time, ~5 s)...");
        const fs::path tmp = dir / "gameplay_rules.tmp";
        if (!runGameProgram(minecraftDir, version, "GameplayDump", kDumpSource, {tmp.wstring(), std::to_wstring(kFormat)},
                            dir)) {
            LOG("warning: block hardness unavailable; survival mining breaks blocks instantly");
            return false;
        }
        std::error_code ec;
        fs::rename(tmp, file, ec);
        j = readJson(file);
        if (!j.is_object()) return false;
    }
    for (const auto& [k, v] : j["blocks"].items()) {
        blockIds_.emplace(k, u32(blockIds_.size()));
        blocks_[k] = {v[0].get<float>(), v[1].get<int>() != 0};
    }
    for (const auto& [k, v] : j["states"].items()) states_[k] = {v[0].get<float>(), v[1].get<int>() != 0};
    for (const auto& [k, v] : j["push"].items()) push_[k] = {u8(v[0].get<int>()), v[1].get<int>() != 0};
    for (const auto& [k, v] : j["tags"].items()) {
        std::vector<u32>& set = tags_[k];
        for (const auto& b : v)
            if (const u32 i = blockIndex(b.get<std::string>()); i != UINT32_MAX) set.push_back(i);
        std::sort(set.begin(), set.end());
    }
    const nlohmann::json entities = j.value("entities", nlohmann::json::object());  // (items() needs a live object)
    for (const auto& [k, v] : entities.items()) {
        EntityDims d;
        d.width = v.value("w", 0.6f), d.height = v.value("h", 1.8f), d.eye = v.value("eye", 1.62f);
        for (const auto& s : v.value("passengers", nlohmann::json::array())) d.seats.push_back({s[0].get<double>(), s[1].get<double>(), s[2].get<double>()});
        if (v.contains("vehicle")) d.vehicle = {v["vehicle"][0].get<double>(), v["vehicle"][1].get<double>(), v["vehicle"][2].get<double>()};
        entities_.emplace(k, std::move(d));
    }
    return !blocks_.empty();
}

const ItemRules::EntityDims* ItemRules::entity(std::string_view typeName) const {
    auto it = entities_.find(std::string(typeName));
    return it == entities_.end() ? nullptr : &it->second;
}

ItemRules::Push ItemRules::push(std::string_view blockName) const {
    auto it = push_.find(std::string(blockName));
    return it == push_.end() ? Push{} : it->second;
}

ItemRules::Hardness ItemRules::hardness(std::string_view stateKey) const {
    if (!states_.empty())
        if (auto it = states_.find(std::string(stateKey)); it != states_.end()) return it->second;
    auto it = blocks_.find(std::string(stateKey.substr(0, stateKey.find('['))));
    return it == blocks_.end() ? Hardness{} : it->second;
}

u32 ItemRules::blockIndex(std::string_view name) const {
    auto it = blockIds_.find(std::string(name));
    return it == blockIds_.end() ? UINT32_MAX : it->second;
}

bool ItemRules::inTag(std::string_view tag, std::string_view blockName) const {
    auto it = tags_.find(std::string(tag));
    return it != tags_.end() && std::binary_search(it->second.begin(), it->second.end(), blockIndex(blockName));
}

// HolderSet<Block> as the codec writes it: "#tag", "block" or a list of blocks.
std::vector<u32> ItemRules::blockSet(const nlohmann::json& h) const {
    std::vector<u32> out;
    auto add = [&](const std::string& n) {
        if (n.starts_with("#")) {
            auto it = tags_.find(n.substr(1));
            if (it != tags_.end()) out.insert(out.end(), it->second.begin(), it->second.end());
        } else if (const u32 i = blockIndex(n.find(':') == std::string::npos ? "minecraft:" + n : n); i != UINT32_MAX) {
            out.push_back(i);
        }
    };
    if (h.is_string()) {
        add(h.get<std::string>());
    } else if (h.is_array()) {
        for (const auto& e : h)
            if (e.is_string()) add(e.get<std::string>());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const ItemRules::ItemInfo& ItemRules::item(std::string_view itemName) {
    auto it = items_.find(std::string(itemName));
    if (it != items_.end()) return it->second;
    ItemInfo info;
    const std::string shortName(itemName.substr(itemName.find(':') + 1));
    const nlohmann::json j = componentReports_.empty() ? nlohmann::json() : readJson(componentReports_ / (shortName + ".json"));
    const nlohmann::json c = j.is_object() ? j.value("components", nlohmann::json::object()) : nlohmann::json::object();
    if (c.contains("minecraft:tool") && c["minecraft:tool"].is_object()) {
        const nlohmann::json& t = c["minecraft:tool"];
        info.hasTool = true;
        info.tool.defaultSpeed = t.value("default_mining_speed", 1.0f);
        info.tool.damagePerBlock = t.value("damage_per_block", 1);
        info.tool.canDestroyInCreative = t.value("can_destroy_blocks_in_creative", true);
        for (const auto& r : t.value("rules", nlohmann::json::array())) {
            Rule rule;
            if (r.contains("speed")) rule.speed = r["speed"].get<float>();
            if (r.contains("correct_for_drops")) rule.correct = r["correct_for_drops"].get<bool>() ? 1 : 0;
            rule.blocks = blockSet(r.value("blocks", nlohmann::json()));
            info.tool.rules.push_back(std::move(rule));
        }
    }
    info.glider = c.contains("minecraft:glider");
    // Item.getUseAnimation / getUseDuration: data components, then the item classes that override them.
    if (c.contains("minecraft:consumable")) {
        const nlohmann::json& k = c["minecraft:consumable"];
        info.use.anim = k.is_object() ? animFromName(k.value("animation", "eat")) : Anim::Eat;
        info.use.duration = int((k.is_object() ? k.value("consume_seconds", 1.6f) : 1.6f) * 20.0f);
        info.use.consumable = true;
        info.use.particles = k.is_object() ? k.value("has_consume_particles", true) : true;
        info.use.sound = k.is_object() && k.contains("sound") && k["sound"].is_string() ? k["sound"].get<std::string>()
                                                                                      : "minecraft:entity.generic.eat";
        if (c.contains("minecraft:food")) {
            info.use.food = true;
            info.use.alwaysEat = c["minecraft:food"].is_object() && c["minecraft:food"].value("can_always_eat", false);
        }
    } else if (c.contains("minecraft:blocks_attacks")) {
        info.use = {Anim::Block, 72000};
    } else if (c.contains("minecraft:kinetic_weapon")) {
        info.use = {Anim::Spear, 72000};
    }
    if (shortName == "bow") info.use = {Anim::Bow, 72000};
    else if (shortName == "crossbow") info.use = {Anim::Crossbow, 28};  // CrossbowItem: charge (1.25 s) + 3
    else if (shortName == "trident") info.use = {Anim::Trident, 72000};
    else if (shortName == "spyglass") info.use = {Anim::Spyglass, 1200};
    else if (shortName == "goat_horn") info.use = {Anim::TootHorn, 140};  // the instrument's use duration (7 s)
    else if (shortName == "brush") info.use = {Anim::Brush, 200};
    else if (shortName == "bundle" || shortName.ends_with("_bundle")) info.use = {Anim::Bundle, 200};
    if (c.contains("minecraft:use_effects") && c["minecraft:use_effects"].is_object()) {
        info.use.speed = c["minecraft:use_effects"].value("speed_multiplier", 0.2f);
        info.use.canSprint = c["minecraft:use_effects"].value("can_sprint", false);
    }
    return items_.emplace(std::string(itemName), std::move(info)).first->second;
}

std::vector<u32> ItemRules::blocks(const std::vector<std::string>& namesOrTags) const {
    return blockSet(nlohmann::json(namesOrTags));
}

bool ItemRules::blockIn(const std::vector<u32>& set, std::string_view blockName) const {
    return std::binary_search(set.begin(), set.end(), blockIndex(blockName));
}

const ItemRules::Tool* ItemRules::tool(std::string_view itemName) {
    const ItemInfo& i = item(itemName);
    return i.hasTool ? &i.tool : nullptr;
}

float ItemRules::miningSpeed(const Tool& t, std::string_view blockName) const {
    const u32 b = blockIndex(blockName);
    for (const Rule& r : t.rules)
        if (r.speed >= 0 && std::binary_search(r.blocks.begin(), r.blocks.end(), b)) return r.speed;
    return t.defaultSpeed;
}

bool ItemRules::correctForDrops(const Tool& t, std::string_view blockName) const {
    const u32 b = blockIndex(blockName);
    for (const Rule& r : t.rules)
        if (r.correct >= 0 && std::binary_search(r.blocks.begin(), r.blocks.end(), b)) return r.correct == 1;
    return false;
}

ItemRules::Use ItemRules::use(std::string_view itemName) { return item(itemName).use; }

bool ItemRules::glider(std::string_view itemName) { return item(itemName).glider; }
