// Create New World (CreateWorldScreen: the Game / World / More tabs) and the screens it opens, as pages of the same
// front-end screen: Superflat customization and its presets (CreateFlatWorldScreen / PresetFlatWorldScreen), Single
// Biome (CreateBuffetWorldScreen), Experiments, Game Rules (WorldCreationGameRulesScreen) and Data Packs. Re-Create
// (WorldSelectionList.recreateWorld) opens it filled in from an existing world.
//
// The world itself is made by the official server (net/local_server.h Config::Create, --create-* flags): world preset
// and Superflat layers through server.properties level-type / generator-settings, Single Biome through a preset
// override pack, --bonusChest, experiments as initial-enabled-packs, game rules from its console before anyone joins.
#include "ui/frontend.h"

#include "assets/asset_source.h"
#include "audio/audio.h"
#include "assets/game_reports.h"
#include "mods/mod_manager.h"
#include "world/nbt_dom.h"

#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <future>
#include <map>

namespace ui {

namespace {

constexpr int kFooter = 33, kTabBar = 24, kRuleFormat = 1;
const char* kModes[] = {"survival", "hardcore", "creative"};
const char* kDifficulties[] = {"peaceful", "easy", "normal", "hard"};

struct FlatLayer {
    std::string block;
    int height = 1;
};
struct FlatSettings {
    std::vector<FlatLayer> layers;  // bottom first
    std::string biome = "minecraft:plains";
    bool features = false, lakes = false;
    std::vector<std::string> structures;  // structure_overrides (structure sets)
};
struct FlatPreset {
    std::string id, icon;
    FlatSettings settings;
};
struct RuleDef {
    std::string id, category, def;
    bool boolean = true;
    i64 min = INT_MIN, max = INT_MAX;
    std::vector<std::string> features;  // required feature flags ("minecraft:minecart_improvements")
};
struct PackChoice {
    std::string id, name, description;
    fs::path path;
    bool on = false, locked = false;  // locked: enabled for every world in Mods & Packs
};
struct Experiment {
    std::string id;  // feature pack ("trade_rebalance")
    bool on = false;
};

enum class Page { Tabs, Flat, FlatPresets, Buffet, Experiments, GameRules, DataPacks, ConfirmExperimental, ExperimentalDetails };

struct Create {
    Page page = Page::Tabs;
    int tab = 0;
    // World tab
    std::vector<std::string> presets, altPresets;  // world_preset tags normal / extended (Shift)
    std::string preset = "minecraft:normal";
    bool bonusChest = false;
    FlatSettings flat;
    std::string biome = "minecraft:plains";
    // More tab
    std::vector<Experiment> experiments;
    std::map<std::string, std::string> rules;  // changed game rules (id -> value)
    std::vector<PackChoice> packs;
    // Sub-pages: a working copy, kept on Done
    FlatSettings flatEdit;
    std::vector<FlatPreset> flatPresets;
    std::vector<std::string> biomes;  // sorted by name
    std::string biomeEdit;
    std::vector<Experiment> experimentsEdit;
    std::map<std::string, std::string> rulesEdit;
    std::vector<PackChoice> packsEdit;
    int sel = -1;
    float scroll = 0;
    Field search, presetText;
    std::vector<int> view;  // filtered rows (biomes, rules)
    std::vector<Field> ruleFields;  // integer rules of the rows on screen
    std::vector<std::string> ruleFieldIds;
    // Game rule definitions from the game's code (extracted once, cached)
    std::vector<RuleDef> ruleDefs;
    std::future<bool> rulesTask;
    bool rulesLoaded = false, rulesFailed = false;
    bool loaded = false;
};
Create g;  // main thread only

std::string pathOf(const std::string& id) {
    const size_t c = id.find(':');
    return c == std::string::npos ? id : id.substr(c + 1);
}
std::string withNs(const std::string& id) { return id.find(':') == std::string::npos ? "minecraft:" + id : id; }
bool has(State& s, const std::string& key) { return s.res.lang().count(key) != 0; }
std::string lang(State& s, const std::string& key, const std::string& fallback) { return has(s, key) ? s.res.translate(key) : fallback; }
// Identifier.toLanguageKey(prefix): "prefix.namespace.path".
std::string langKey(const char* prefix, const std::string& id) {
    const std::string full = withNs(id);
    return std::string(prefix) + "." + full.substr(0, full.find(':')) + "." + pathOf(full);
}
std::string onOff(State& s, bool on) { return s.res.translate(on ? "options.on" : "options.off"); }
Text cycleLabel(State& s, const char* key, const std::string& value) { return trf(s, "options.generic_value", {tr(s, key), Text(value)}); }

nlohmann::json dataJson(const std::string& path) {
    std::vector<u8> bytes;
    if (!AssetSource::data().read(path, bytes)) return nullptr;
    return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
}

// A tag's values with nested tags expanded ("#minecraft:normal").
void tagValues(const std::string& dir, const std::string& tag, std::vector<std::string>& out, int depth = 0) {
    const nlohmann::json j = dataJson("data/" + withNs(tag).substr(0, withNs(tag).find(':')) + "/tags/" + dir + "/" + pathOf(tag) + ".json");
    if (!j.is_object() || !j.contains("values") || depth > 4) return;
    for (const auto& v : j["values"]) {
        const std::string id = v.is_object() ? v.value("id", "") : v.is_string() ? v.get<std::string>() : "";
        if (id.starts_with("#")) tagValues(dir, id.substr(1), out, depth + 1);
        else if (!id.empty() && std::find(out.begin(), out.end(), withNs(id)) == out.end()) out.push_back(withNs(id));
    }
}

FlatSettings parseFlat(const nlohmann::json& j) {
    FlatSettings f;
    if (!j.is_object()) return f;
    for (const auto& l : j.value("layers", nlohmann::json::array()))
        f.layers.push_back({withNs(l.value("block", "minecraft:air")), std::max(0, l.value("height", 1))});
    f.biome = withNs(j.value("biome", "minecraft:plains"));
    f.features = j.value("features", false);
    f.lakes = j.value("lakes", false);
    for (const auto& s : j.value("structure_overrides", nlohmann::json::array()))
        if (s.is_string()) f.structures.push_back(s.get<std::string>());
    return f;
}

nlohmann::json flatJson(const FlatSettings& f) {
    nlohmann::json layers = nlohmann::json::array();
    for (const FlatLayer& l : f.layers) layers.push_back({{"block", l.block}, {"height", l.height}});
    nlohmann::json j = {{"layers", layers}, {"biome", f.biome}, {"features", f.features}, {"lakes", f.lakes}};
    if (!f.structures.empty()) j["structure_overrides"] = f.structures;
    return j;
}

// PresetFlatWorldScreen.save / fromString: "minecraft:bedrock,2*minecraft:dirt,minecraft:grass_block;minecraft:plains".
std::string flatString(const FlatSettings& f) {
    std::string s;
    for (const FlatLayer& l : f.layers) {
        if (!s.empty()) s += ",";
        if (l.height > 1) s += std::to_string(l.height) + "*";
        s += l.block;
    }
    return s + ";" + f.biome;
}
bool parseFlatString(const std::string& text, const FlatSettings& base, FlatSettings& out) {
    out = base;
    out.layers.clear();
    const size_t semi = text.find(';');
    const std::string layers = text.substr(0, semi);
    for (size_t i = 0; i <= layers.size();) {
        const size_t comma = std::min(layers.find(',', i), layers.size());
        std::string part = layers.substr(i, comma - i);
        i = comma + 1;
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (part.empty()) continue;
        int height = 1;
        if (const size_t star = part.find('*'); star != std::string::npos) {
            height = std::atoi(part.substr(0, star).c_str());
            part = part.substr(star + 1);
        }
        if (height <= 0 || part.empty()) return false;
        out.layers.push_back({withNs(part), height});
    }
    int total = 0;
    for (const FlatLayer& l : out.layers) total += l.height;
    if (out.layers.empty() || total > 4064) return false;  // DimensionType.Y_SIZE
    if (semi != std::string::npos && semi + 1 < text.size()) out.biome = withNs(text.substr(semi + 1));
    return true;
}

// Game rule definitions: the registry, types, defaults, categories and bounds only the game's code has.
constexpr const char* kRuleDump = R"JAVA(
import com.google.gson.*;
import com.mojang.brigadier.arguments.IntegerArgumentType;
import java.io.*;
import java.nio.charset.StandardCharsets;
import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.level.gamerules.GameRule;

public class GameRuleDump {
    static <T> String def(GameRule<T> r) { return r.serialize(r.defaultValue()); }

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        JsonObject root = new JsonObject();
        root.addProperty("format", Integer.parseInt(args[1]));
        JsonArray rules = new JsonArray();
        for (GameRule<?> r : BuiltInRegistries.GAME_RULE) {
            JsonObject o = new JsonObject();
            o.addProperty("id", r.getIdentifier().toString());
            o.addProperty("type", r.gameRuleType().getSerializedName());
            o.addProperty("default", def(r));
            o.addProperty("category", r.category().id().toString());
            if (r.argument() instanceof IntegerArgumentType a) {
                o.addProperty("min", a.getMinimum());
                o.addProperty("max", a.getMaximum());
            }
            JsonArray features = new JsonArray();
            for (Identifier f : FeatureFlags.REGISTRY.toNames(r.requiredFeatures())) features.add(f.toString());
            o.add("features", features);
            rules.add(o);
        }
        root.add("rules", rules);
        try (Writer w = new OutputStreamWriter(new FileOutputStream(args[0]), StandardCharsets.UTF_8)) {
            new GsonBuilder().setPrettyPrinting().create().toJson(root, w);
        }
    }
}
)JAVA";

bool loadRuleDefs(const fs::path& minecraftDir, const std::string& version, std::vector<RuleDef>& out) {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    const fs::path dir = fs::path(local ? local : L".") / "minecraftCplusplus" / "reports" / version, file = dir / "game_rules.json";
    std::vector<u8> bytes;
    nlohmann::json j = readFile(file, bytes) ? nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false) : nullptr;
    if (!j.is_object() || j.value("format", 0) != kRuleFormat) {
        LOG("extracting the game rules with the game's code (one time)...");
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path tmp = dir / "game_rules.tmp";
        if (!runGameProgram(minecraftDir, version, "GameRuleDump", kRuleDump, {tmp.wstring(), std::to_wstring(kRuleFormat)}, dir))
            return false;
        fs::rename(tmp, file, ec);
        bytes.clear();
        j = readFile(file, bytes) ? nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false) : nullptr;
        if (!j.is_object()) return false;
    }
    for (const auto& r : j.value("rules", nlohmann::json::array())) {
        RuleDef d;
        d.id = r.value("id", ""), d.category = r.value("category", "minecraft:misc"), d.def = r.value("default", "");
        d.boolean = r.value("type", "boolean") == "boolean";
        d.min = r.value("min", i64(INT_MIN)), d.max = r.value("max", i64(INT_MAX));
        for (const auto& f : r.value("features", nlohmann::json::array())) d.features.push_back(f.get<std::string>());
        out.push_back(std::move(d));
    }
    return !out.empty();
}

// The game rule's lang key: gamerule.<namespace>.<path>, else the key from before the rules were renamed (the
// installed language file still has those for most of them).
std::string ruleKey(State& s, const std::string& id) {
    static const std::map<std::string, std::string> kOld = {
        {"advance_time", "doDaylightCycle"}, {"advance_weather", "doWeatherCycle"}, {"block_drops", "doTileDrops"},
        {"command_blocks_work", "commandBlocksEnabled"}, {"entity_drops", "doEntityDrops"}, {"immediate_respawn", "doImmediateRespawn"},
        {"limited_crafting", "doLimitedCrafting"}, {"max_block_modifications", "commandModificationBlockLimit"},
        {"max_command_forks", "maxCommandForkCount"}, {"max_command_sequence_length", "maxCommandChainLength"},
        {"max_minecart_speed", "minecartMaxSpeed"}, {"max_snow_accumulation_height", "snowAccumulationHeight"},
        {"mob_drops", "doMobLoot"}, {"natural_health_regeneration", "naturalRegeneration"},
        {"show_advancement_messages", "announceAdvancements"}, {"spawner_blocks_work", "spawnerBlocksEnabled"},
        {"spawn_mobs", "doMobSpawning"}, {"spawn_patrols", "doPatrolSpawning"}, {"spawn_phantoms", "doInsomnia"},
        {"spawn_wandering_traders", "doTraderSpawning"}, {"spawn_wardens", "doWardenSpawning"}, {"spread_vines", "doVinesSpread"},
        {"respawn_radius", "spawnRadius"}};
    if (has(s, langKey("gamerule", id))) return langKey("gamerule", id);
    if (auto it = kOld.find(pathOf(id)); it != kOld.end() && withNs(id).starts_with("minecraft:")) return "gamerule." + it->second;
    std::string camel;
    bool up = false;
    for (char ch : pathOf(id)) {
        if (ch == '_') up = true;
        else camel += up ? char(std::toupper(u8(ch))) : ch, up = false;
    }
    return "gamerule." + camel;
}
std::string ruleName(State& s, const std::string& id) { return lang(s, ruleKey(s, id), pathOf(id)); }
std::string ruleDescription(State& s, const std::string& id) { return lang(s, ruleKey(s, id) + ".description", ""); }
void ensureRules(State& s) {
    if (g.rulesLoaded || g.rulesFailed || g.rulesTask.valid()) return;
    const fs::path mc = s.fe->cfg.minecraftDir;
    const std::string version = s.fe->cfg.version;
    g.rulesTask = std::async(std::launch::async, [mc, version] {
        std::vector<RuleDef> defs;
        if (!loadRuleDefs(mc, version, defs)) return false;
        std::sort(defs.begin(), defs.end(), [](const RuleDef& a, const RuleDef& b) { return a.id < b.id; });
        g.ruleDefs = std::move(defs);  // read only after the task is collected (main thread)
        return true;
    });
}

void pollRules(State& s) {
    if (!g.rulesTask.valid() || g.rulesTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const bool ok = g.rulesTask.get();
    g.rulesLoaded = ok, g.rulesFailed = !ok;
    if (!ok) LOG("ui: game rules unavailable (the game's code could not be run)");
    if (g.page == Page::GameRules) s.buildButtons();
}

const RuleDef* ruleDef(const std::string& id) {
    for (const RuleDef& d : g.ruleDefs)
        if (d.id == id) return &d;
    return nullptr;
}

bool experimentOn(const std::string& feature) {  // a feature flag requested by an enabled experiment pack
    for (const Experiment& e : g.experiments)
        if (e.on && withNs(e.id) == feature) return true;
    return false;
}

// What the creation screens list: world presets, Superflat presets, biomes and experiments from the game's data (with
// the global data packs), data packs from the Mods & Packs folder.
void loadData(State& s) {
    if (g.loaded) return;
    g.loaded = true;
    tagValues("worldgen/world_preset", "minecraft:normal", g.presets);
    tagValues("worldgen/world_preset", "minecraft:extended", g.altPresets);
    if (g.presets.empty()) g.presets = {"minecraft:normal"};
    if (g.altPresets.empty()) g.altPresets = g.presets;
    std::vector<std::string> flatIds;
    tagValues("worldgen/flat_level_generator_preset", "minecraft:visible", flatIds);
    for (const std::string& id : flatIds) {
        const nlohmann::json j = dataJson("data/" + id.substr(0, id.find(':')) + "/worldgen/flat_level_generator_preset/" + pathOf(id) + ".json");
        if (!j.is_object()) continue;
        g.flatPresets.push_back({id, withNs(j.value("display", "minecraft:stone")), parseFlat(j.value("settings", nlohmann::json::object()))});
    }
    for (const std::string& file : AssetSource::data().list("data/", ".json")) {
        const size_t at = file.find("/worldgen/biome/");
        if (at == std::string::npos || file.find('/', at + 16) != std::string::npos) continue;
        g.biomes.push_back(file.substr(5, at - 5) + ":" + file.substr(at + 16, file.size() - at - 16 - 5));
    }
    std::sort(g.biomes.begin(), g.biomes.end(), [&](const std::string& a, const std::string& b) {
        return lang(s, langKey("biome", a), a) < lang(s, langKey("biome", b), b);
    });
    for (const std::string& file : AssetSource::data().list("data/minecraft/datapacks/", "/pack.mcmeta")) {
        const std::string id = file.substr(25, file.size() - 25 - 12);
        if (id.find('/') == std::string::npos) g.experiments.push_back({id, false});
    }
    // Superflat starts as the game's default (FlatLevelGeneratorSettings.getDefault = the classic flat preset).
    for (const FlatPreset& p : g.flatPresets)
        if (p.id == "minecraft:classic_flat") g.flat = p.settings;
    if (g.flat.layers.empty())
        g.flat.layers = {{"minecraft:bedrock", 1}, {"minecraft:dirt", 2}, {"minecraft:grass_block", 1}};
}

void resetPacks() {
    g.packs.clear();
    const auto& entries = mods::Manager::get().entries();
    for (const mods::Entry& e : entries) {
        if (!e.isDataPack() || e.source != "mods folder") continue;
        g.packs.push_back({e.id, e.name, e.description, e.path, e.enabled, e.enabled});
    }
}

bool isDebug() { return g.preset == "minecraft:debug_all_block_states"; }
bool isHardcore(State& s) { return s.fe->createMode == 1; }
bool hasEditor() { return g.preset == "minecraft:flat" || g.preset == "minecraft:single_biome_surface"; }

int roundUp2(int v) { return (v + 1) / 2 * 2; }  // Mth.roundToward(v, 2)

void tile(Canvas& c, const Sprite* t, float x0, float y0, float x1, float y1, int size) {  // a texture repeated from (x0, y0)
    if (!t) return;
    for (float y = y0; y < y1; y += float(size))
        for (float x = x0; x < x1; x += float(size))
            blitTexture(c, t, x, y, 0, 0, std::min(float(size), x1 - x), std::min(float(size), y1 - y), size, size);
}

void openPage(State& s, Page p) {
    g.page = p;
    g.sel = -1;
    g.scroll = 0;
    g.search.text = TextField{};
    s.fe->focus = -1;
    s.buildButtons();
}

// --- shared list drawing ------------------------------------------------------------------------------------------

struct ListArea {
    float top, bottom, left, width;
    int rowH, rows;
    float y(int i) const { return top + 4 + float(i * rowH) - g.scroll; }
    float maxScroll() const { return std::max(0.0f, float(rows * rowH + 8) - (bottom - top)); }
    int at(float mx, float my) const {
        if (my < top || my >= bottom || mx < left || mx >= left + width) return -1;
        const int i = int(std::floor((my - top - 4 + g.scroll) / float(rowH)));
        return i >= 0 && i < rows ? i : -1;
    }
    bool visible(int i) const { return y(i) >= top && y(i) + float(rowH) <= bottom + 1; }
};

void drawListFrame(State& s, Canvas& c, const ListArea& l) {
    listBackground(s, c, l.top, l.bottom);
    if (const float ms = l.maxScroll(); ms > 0) {
        const float x = l.left + l.width + 4, h = l.bottom - l.top;
        const float bar = std::max(32.0f, h * h / (h + ms)), by = l.top + (h - bar) * std::clamp(g.scroll, 0.0f, ms) / ms;
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), x, l.top, 6, h);
        blitSprite(c, s.sprites.sprite("widget/scroller"), x, by, 6, bar);
        s.scrollbarCursor(x, l.top, h);
    }
}
void drawSelection(Canvas& c, const ListArea& l, int i) {  // AbstractSelectionList.renderSelection
    const float y = l.y(i);
    c.fill(l.left - 2, y - 2, l.left + l.width - 2, y + float(l.rowH) - 2, 0xFFFFFFFF);
    c.fill(l.left - 1, y - 1, l.left + l.width - 3, y + float(l.rowH) - 3, 0xFF000000);
}
void drawSlotItem(State& s, Canvas& c, const std::string& item, float x, float y) {  // the presets' and layers' icons
    blitSprite(c, s.sprites.sprite("container/slot"), x + 1, y + 1, 18, 18);
    ItemStack st;
    st.item = s.items.id(item);
    st.count = 1;
    if (st.item >= 0) s.drawItem(c, st, x + 2, y + 2, false);
}
std::string itemForBlock(State& s, const std::string& block) {  // CreateFlatWorldScreen.getDisplayItem
    if (block == "minecraft:water") return "minecraft:water_bucket";
    if (block == "minecraft:lava") return "minecraft:lava_bucket";
    return s.items.id(block) >= 0 ? block : std::string();
}
std::string blockName(State& s, const std::string& block) {
    return lang(s, langKey("block", block), lang(s, langKey("item", block), block));
}

void hoverTooltips(State& s, Canvas& c) {  // Tooltip of the button under the mouse
    for (const Button& b : s.buttons)
        if (b.tooltip && s.mouseX >= b.x && s.mouseY >= b.y && s.mouseX < b.x + b.w && s.mouseY < b.y + b.h) {
            const Text t = b.tooltip();
            if (!t.empty()) s.drawTooltip(c, s.font.split(t, 170), s.mouseX, s.mouseY);
            return;
        }
}

// --- Game / World / More tabs -------------------------------------------------------------------------------------

int tabTop(State& s, int contentH) {
    const int area = s.guiH() - kFooter - kTabBar;
    return kTabBar + int(float(area - contentH) / 6.0f);  // GridLayoutTab: aligned at 1/6 of the tab area
}

void cycleWorldType(State& s) {
    const bool alt = s.window->keyDown(VK_SHIFT);  // CycleButton.DEFAULT_ALT_LIST_SELECTOR
    const std::vector<std::string>& list = alt ? g.altPresets : g.presets;
    auto it = std::find(list.begin(), list.end(), g.preset);
    g.preset = it == list.end() || it + 1 == list.end() ? list.front() : *(it + 1);
    if (isDebug()) s.fe->createCheats = true;
    s.buildButtons();
}

Text presetName(State& s, const std::string& id) { return Text(lang(s, langKey("generator", id), pathOf(id))); }

void createWorldNow(State& s);

void buildTabs(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    if (g.tab == 0) {
        const int y0 = tabTop(s, 117);
        const float x = float(W / 2 - 105);
        f.createName.x = float(W / 2 - 104), f.createName.y = float(y0 + 13), f.createName.w = 208;
        setFields(s, {&f.createName}, 0);
        Button& mode = addButton(s, x, float(y0 + 41), 210, Text(), [p] {
            FrontEndState& fe = *p->fe;
            fe.createMode = (fe.createMode + 1) % 3;
            fe.createCheats = fe.createMode == 2;  // WorldCreationUiState.setGameMode: creative turns commands on
            p->buildButtons();
        }, !isDebug());
        mode.labelFn = [p] {
            if (isDebug()) return cycleLabel(*p, "selectWorld.gameMode", p->res.translate("selectWorld.gameMode.spectator"));
            return cycleLabel(*p, "selectWorld.gameMode", p->res.translate(std::string("selectWorld.gameMode.") + kModes[p->fe->createMode]));
        };
        mode.tooltip = [p] { return Text(p->res.translate(std::string("selectWorld.gameMode.") + kModes[p->fe->createMode] + ".info")); };
        Button& diff = addButton(s, x, float(y0 + 69), 210, Text(), [p] {
            p->fe->createDifficulty = (p->fe->createDifficulty + 1) % 4;
        }, !isHardcore(s));
        diff.labelFn = [p] {
            const int d = isHardcore(*p) ? 3 : p->fe->createDifficulty;
            return cycleLabel(*p, "options.difficulty", p->res.translate(std::string("options.difficulty.") + kDifficulties[d]));
        };
        diff.tooltip = [p] {
            const int d = isHardcore(*p) ? 3 : p->fe->createDifficulty;
            return Text(p->res.translate(std::string("options.difficulty.") + kDifficulties[d] + ".info"));
        };
        Button& cheats = addButton(s, x, float(y0 + 97), 210, Text(), [p] { p->fe->createCheats = !p->fe->createCheats; },
                                   !isHardcore(s) && !isDebug());
        cheats.labelFn = [p] {
            return cycleLabel(*p, "selectWorld.allowCommands", onOff(*p, p->fe->createCheats && !isHardcore(*p)));
        };
        cheats.tooltip = [p] { return tr(*p, "selectWorld.allowCommands.info"); };
    } else if (g.tab == 1) {
        const int y0 = tabTop(s, 113);
        const float x0 = float(W / 2 - 155);
        Button& type = addButton(s, x0, float(y0), 150, Text(), [p] { cycleWorldType(*p); },
                                 std::find(g.altPresets.begin(), g.altPresets.end(), g.preset) != g.altPresets.end());
        type.labelFn = [p] { return trf(*p, "options.generic_value", {tr(*p, "selectWorld.mapType"), presetName(*p, g.preset)}); };
        type.tooltip = [p] { return g.preset == "minecraft:amplified" ? tr(*p, "generator.minecraft.amplified.info") : Text(); };
        addButton(s, x0 + 160, float(y0), 150, tr(s, "selectWorld.customizeType"), [p] {
            if (g.preset == "minecraft:flat") {
                g.flatEdit = g.flat;
                openPage(*p, Page::Flat);
            } else {
                g.biomeEdit = g.biome;
                openPage(*p, Page::Buffet);
                g.view.clear();
                for (size_t i = 0; i < g.biomes.size(); ++i) g.view.push_back(int(i));
                p->buildButtons();
            }
        }, !isDebug() && hasEditor());
        f.createSeed.x = x0 + 1, f.createSeed.y = float(y0 + 41), f.createSeed.w = 308;
        setFields(s, {&f.createSeed}, -1);
        Button& structures = addButton(s, x0 + 266, float(y0 + 61), 44, Text(), [p] { p->fe->createStructures = !p->fe->createStructures; },
                                       !isDebug());
        structures.labelFn = [p] { return Text(onOff(*p, p->fe->createStructures)); };
        structures.tooltip = [p] { return tr(*p, "selectWorld.mapFeatures.info"); };
        Button& bonus = addButton(s, x0 + 266, float(y0 + 85), 44, Text(), [] { g.bonusChest = !g.bonusChest; },
                                  !isHardcore(s) && !isDebug());
        bonus.labelFn = [p] { return Text(onOff(*p, g.bonusChest && !isHardcore(*p))); };
    } else {
        const int y0 = tabTop(s, 76);
        const float x = float(W / 2 - 105);
        addButton(s, x, float(y0), 210, tr(s, "selectWorld.gameRules"), [p] {
            g.rulesEdit = g.rules;
            ensureRules(*p);
            openPage(*p, Page::GameRules);
        });
        addButton(s, x, float(y0 + 28), 210, tr(s, "selectWorld.experiments"), [p] {
            g.experimentsEdit = g.experiments;
            openPage(*p, Page::Experiments);
        });
        addButton(s, x, float(y0 + 56), 210, tr(s, "selectWorld.dataPacks"), [p] {
            g.packsEdit = g.packs;
            openPage(*p, Page::DataPacks);
        });
    }
    addButton(s, float(W / 2 - 154), float(H - 27), 150, tr(s, "selectWorld.create"), [p] { createWorldNow(*p); });
    addButton(s, float(W / 2 + 4), float(H - 27), 150, tr(s, "gui.cancel"), [p] { openWorlds(*p); });
    f.onBack = [p] { openWorlds(*p); };
    f.onEnter = [p] { createWorldNow(*p); };
}

struct TabGeom {
    int x, w;
};
TabGeom tabGeom(State& s) {
    const int tabsW = std::min(400, s.guiW()) - 28;
    return {roundUp2((s.guiW() - tabsW) / 2), roundUp2(tabsW / 3)};
}

void drawTabs(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth(), H = c.guiHeight();
    menuBackground(s, c);
    // CreateWorldScreen.extractMenuBackground: the tab header background over the bar.
    tile(c, s.sprites.texture("gui/tab_header_background"), 0, 0, float(W), float(kTabBar), 16);
    const TabGeom tg = tabGeom(s);
    const char* titles[] = {"createWorld.tab.game.title", "createWorld.tab.world.title", "createWorld.tab.more.title"};
    const Sprite* sep = s.sprites.texture(f.cfg.menu ? "gui/header_separator" : "gui/inworld_header_separator");
    for (int x = 0; x < tg.x; x += 32) blitTexture(c, sep, float(x), float(kTabBar - 2), 0, 0, float(std::min(32, tg.x - x)), 2, 32, 2);
    for (int x = tg.x + 3 * tg.w; x < W; x += 32) blitTexture(c, sep, float(x), float(kTabBar - 2), 0, 0, float(std::min(32, W - x)), 2, 32, 2);
    for (int i = 0; i < 3; ++i) {  // MenuTabBar.MenuTabButton
        const float x = float(tg.x + i * tg.w);
        const bool selected = g.tab == i;
        const bool hover = s.mouseX >= x && s.mouseX < x + float(tg.w) && s.mouseY >= 0 && s.mouseY < float(kTabBar);
        blitSprite(c, s.sprites.sprite(selected ? (hover ? "widget/tab_selected_highlighted" : "widget/tab_selected")
                                                : (hover ? "widget/tab_highlighted" : "widget/tab")),
                   x, 0, float(tg.w), float(kTabBar));
        const Text label = tr(s, titles[i]);
        if (selected) {
            tile(c, s.sprites.texture(f.cfg.menu ? "gui/menu_background" : "gui/inworld_menu_background"), x + 2, 2,
                 x + float(tg.w - 2), float(kTabBar), 32);
            const int lw = std::min(s.font.width(label), tg.w - 4);
            c.fill(x + float((tg.w - lw) / 2), float(kTabBar - 2), x + float((tg.w - lw) / 2 + lw), float(kTabBar - 1), 0xFFFFFFFF);
        }
        s.drawCentered(c, label, x + float(tg.w / 2), float(selected ? 8 : 10), 0xFFFFFFFF);
    }
    const Sprite* foot = s.sprites.texture(f.cfg.menu ? "gui/footer_separator" : "gui/inworld_footer_separator");
    for (int x = 0; x < W; x += 32) blitTexture(c, foot, float(x), float(H - kFooter - 2), 0, 0, float(std::min(32, W - x)), 2, 32, 2);
    std::vector<Text> tooltip;
    if (g.tab == 0) {
        const int y0 = tabTop(s, 117);
        s.drawText(c, tr(s, "selectWorld.enterName"), float(W / 2 - 104), float(y0), 0xFFFFFFFF, true);
        const Field& n = f.createName;
        if (s.mouseX >= n.x && s.mouseY >= n.y && s.mouseX < n.x + n.w && s.mouseY < n.y + n.h) {
            std::string name = n.text.value;
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) name = s.res.translate("selectWorld.newWorld");
            const std::string fmt = s.res.translate("selectWorld.targetFolder");
            Text t(fmt.substr(0, fmt.find("%s")));
            Style it;
            it.italic = 1;
            t.append(worldFolderFor(f.cfg.savesDir, name), it);
            tooltip = s.font.split(t, 170);
        }
    } else if (g.tab == 1) {
        const int y0 = tabTop(s, 113);
        const float x0 = float(W / 2 - 155);
        s.drawText(c, tr(s, "selectWorld.enterSeed"), x0 + 1, float(y0 + 28), 0xFFFFFFFF, true);
        s.drawText(c, tr(s, "selectWorld.mapFeatures"), x0, float(y0 + 67), isDebug() ? 0xFFA0A0A0 : 0xFFFFFFFF, true);
        s.drawText(c, tr(s, "selectWorld.bonusItems"), x0, float(y0 + 91), isHardcore(s) || isDebug() ? 0xFFA0A0A0 : 0xFFFFFFFF, true);
    }
    drawFields(s, c);
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
    else hoverTooltips(s, c);
}

// --- Superflat customization and presets --------------------------------------------------------------------------

ListArea flatList(State& s) {
    return {33, float(s.guiH() - 64), float(s.guiW() / 2 - 110), 220, 24, int(g.flatEdit.layers.size()) + 1};
}

void buildFlat(State& s) {
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    addButton(s, float(W / 2 - 154), float(H - 54), 150, tr(s, "createWorld.customize.flat.removeLayer"), [p] {
        if (g.sel < 0 || g.sel >= int(g.flatEdit.layers.size())) return;
        g.flatEdit.layers.erase(g.flatEdit.layers.begin() + g.sel);
        g.sel = std::min(g.sel, int(g.flatEdit.layers.size()) - 1);
        p->buildButtons();
    }, g.sel >= 0);
    addButton(s, float(W / 2 + 4), float(H - 54), 150, tr(s, "createWorld.customize.presets"), [p] {
        openPage(*p, Page::FlatPresets);
        g.presetText.text.value = flatString(g.flatEdit);
        g.presetText.text.maxLength = 1230;
        g.presetText.text.cursor = g.presetText.text.selection = 0;
        p->buildButtons();
    });
    auto done = [p] {
        if (!g.flatEdit.layers.empty()) g.flat = g.flatEdit;
        openPage(*p, Page::Tabs);
    };
    addButton(s, float(W / 2 - 154), float(H - 30), 150, tr(s, "gui.done"), done);
    addButton(s, float(W / 2 + 4), float(H - 30), 150, tr(s, "gui.cancel"), [p] { openPage(*p, Page::Tabs); });
    s.fe->onBack = [p] { openPage(*p, Page::Tabs); };
    s.fe->onEnter = done;
    s.onFilesDrop = [p](const std::vector<fs::path>& files) {  // the new packs join the list (off)
        dropIntoModsFolder(*p, files, Screen::CreateWorld, [] {
            for (const mods::Entry& e : mods::Manager::get().entries()) {
                if (!e.isDataPack() || e.source != "mods folder") continue;
                if (std::none_of(g.packsEdit.begin(), g.packsEdit.end(), [&](const PackChoice& c) { return c.id == e.id; }))
                    g.packsEdit.push_back({e.id, e.name, e.description, e.path, false, e.enabled});
            }
        });
    };
}

void drawFlat(State& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "createWorld.customize.flat.title"), float(W / 2), 12, 0xFFFFFFFF);
    const ListArea l = flatList(s);
    g.scroll = std::clamp(g.scroll, 0.0f, l.maxScroll());
    drawListFrame(s, c, l);
    c.pushScissor(0, l.top, float(W), l.bottom);
    Style ul;
    ul.underlined = 1;
    const auto& layers = g.flatEdit.layers;
    for (int i = 0; i < l.rows; ++i) {
        const float y = l.y(i), x = l.left + 2, right = l.left + l.width - 4;
        if (y + float(l.rowH) < l.top || y > l.bottom) continue;
        if (i == 0) {  // the column titles
            s.drawText(c, Text(s.res.translate("createWorld.customize.flat.tile"), ul), x, y, 0xFFFFFFFF, true);
            const Text h(s.res.translate("createWorld.customize.flat.height"), ul);
            s.drawText(c, h, right - float(s.font.width(h)), y, 0xFFFFFFFF, true);
            continue;
        }
        const int li = int(layers.size()) - i;  // top layer first
        if (li == g.sel) drawSelection(c, l, i);
        const FlatLayer& layer = layers[size_t(li)];
        drawSlotItem(s, c, itemForBlock(s, layer.block), x, y);
        s.drawText(c, Text(blockName(s, layer.block)), x + 23, y + 6, 0xFFFFFFFF, true);
        const std::string n = std::to_string(layer.height);
        const Text h = i == 1 ? trf(s, "createWorld.customize.flat.layer.top", {Text(n)})
                       : li == 0 ? trf(s, "createWorld.customize.flat.layer.bottom", {Text(n)})
                                 : trf(s, "createWorld.customize.flat.layer", {Text(n)});
        s.drawText(c, h, right - float(s.font.width(h)), y + 6, 0xFFFFFFFF, true);
    }
    c.popScissor();
    s.drawButtons(c);
}

ListArea presetList(State& s) {
    return {80, float(s.guiH() - 37), float(s.guiW() / 2 - 110), 220, 24, int(g.flatPresets.size())};
}

void usePreset(State& s) {
    FlatSettings out;
    const bool exact = g.sel >= 0 && g.sel < int(g.flatPresets.size()) && g.presetText.text.value == flatString(g.flatPresets[size_t(g.sel)].settings);
    if (exact) out = g.flatPresets[size_t(g.sel)].settings;
    else if (!parseFlatString(g.presetText.text.value, g.flatEdit, out)) return;
    g.flatEdit = out;
    openPage(s, Page::Flat);
}

void buildFlatPresets(State& s) {
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    g.presetText.x = 50, g.presetText.y = 40, g.presetText.w = float(W - 100);
    setFields(s, {&g.presetText}, -1);
    FlatSettings probe;
    addButton(s, float(W / 2 - 155), float(H - 28), 150, tr(s, "createWorld.customize.presets.select"), [p] { usePreset(*p); },
              parseFlatString(g.presetText.text.value, g.flatEdit, probe));
    addButton(s, float(W / 2 + 5), float(H - 28), 150, tr(s, "gui.cancel"), [p] { openPage(*p, Page::Flat); });
    s.fe->onBack = [p] { openPage(*p, Page::Flat); };
    s.fe->onEnter = [p] { usePreset(*p); };
}

void drawFlatPresets(State& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "createWorld.customize.presets.title"), float(W / 2), 8, 0xFFFFFFFF);
    s.drawText(c, tr(s, "createWorld.customize.presets.share"), 51, 30, 0xFFA0A0A0, true);
    s.drawText(c, tr(s, "createWorld.customize.presets.list"), 51, 68, 0xFFA0A0A0, true);
    const ListArea l = presetList(s);
    g.scroll = std::clamp(g.scroll, 0.0f, l.maxScroll());
    drawListFrame(s, c, l);
    c.pushScissor(0, l.top, float(W), l.bottom);
    for (int i = 0; i < l.rows; ++i) {
        const float y = l.y(i), x = l.left + 2;
        if (y + float(l.rowH) < l.top || y > l.bottom) continue;
        if (i == g.sel) drawSelection(c, l, i);
        const FlatPreset& pr = g.flatPresets[size_t(i)];
        drawSlotItem(s, c, pr.icon, x, y);
        s.drawText(c, Text(lang(s, langKey("flat_world_preset", pr.id), s.res.translate("flat_world_preset.unknown"))), x + 23, y + 6,
                   0xFFFFFFFF, true);
    }
    c.popScissor();
    drawFields(s, c);
    s.drawButtons(c);
}

// --- Single Biome ---------------------------------------------------------------------------------------------------

ListArea biomeList(State& s) { return {40, float(s.guiH() - kFooter), float(s.guiW() / 2 - 110), 220, 15, int(g.view.size())}; }

void filterBiomes(State& s) {
    std::string q = g.search.text.value;
    for (char& ch : q) ch = char(std::tolower(u8(ch)));
    g.view.clear();
    for (size_t i = 0; i < g.biomes.size(); ++i) {
        std::string n = lang(s, langKey("biome", g.biomes[i]), g.biomes[i]);
        for (char& ch : n) ch = char(std::tolower(u8(ch)));
        if (q.empty() || n.find(q) != std::string::npos || g.biomes[i].find(q) != std::string::npos) g.view.push_back(int(i));
    }
    g.scroll = 0;
}

void buildBuffet(State& s) {
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    g.search.x = float(W / 2 - 100), g.search.y = 18, g.search.w = 200, g.search.h = 15;
    g.search.hint = s.res.translate("createWorld.customize.buffet.search");
    setFields(s, {&g.search}, 0);
    auto done = [p] {
        if (g.biomeEdit.empty()) return;
        g.biome = g.biomeEdit;
        openPage(*p, Page::Tabs);
    };
    addButton(s, float(W / 2 - 154), float(H - 27), 150, tr(s, "gui.done"), done, !g.biomeEdit.empty());
    addButton(s, float(W / 2 + 4), float(H - 27), 150, tr(s, "gui.cancel"), [p] { openPage(*p, Page::Tabs); });
    s.fe->onBack = [p] { openPage(*p, Page::Tabs); };
    s.fe->onEnter = done;
}

void drawBuffet(State& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "createWorld.customize.buffet.title"), float(W / 2), 6, 0xFFFFFFFF);
    const ListArea l = biomeList(s);
    g.scroll = std::clamp(g.scroll, 0.0f, l.maxScroll());
    drawListFrame(s, c, l);
    c.pushScissor(0, l.top, float(W), l.bottom);
    for (int i = 0; i < l.rows; ++i) {
        const float y = l.y(i);
        if (y + float(l.rowH) < l.top || y > l.bottom) continue;
        const std::string& id = g.biomes[size_t(g.view[size_t(i)])];
        if (id == g.biomeEdit) drawSelection(c, l, i);
        s.drawText(c, Text(lang(s, langKey("biome", id), id)), l.left + 5, y + 2, 0xFFFFFFFF, true);
    }
    c.popScissor();
    drawFields(s, c);
    s.drawButtons(c);
}

// --- Experiments --------------------------------------------------------------------------------------------------

struct ExperimentsLayout {
    std::vector<Text> info;
    int top, gridTop;
};
ExperimentsLayout experimentsLayout(State& s) {
    ExperimentsLayout el;
    el.info = s.font.split(tr(s, "selectWorld.experiments.info"), 310);
    const int grid = std::min(130, int(g.experimentsEdit.size()) * 42 - 4);
    const int content = int(el.info.size()) * 9 + 15 + grid;
    el.top = 33 + (s.guiH() - 66 - content) / 2;
    el.gridTop = el.top + int(el.info.size()) * 9 + 15;
    return el;
}

void buildExperiments(State& s) {
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    const ExperimentsLayout el = experimentsLayout(s);
    for (size_t i = 0; i < g.experimentsEdit.size(); ++i) {
        Button& b = addButton(s, float(W / 2 - 150 + 299 - 44), float(el.gridTop + int(i) * 42), 44, Text(), [i] {
            g.experimentsEdit[i].on = !g.experimentsEdit[i].on;
        });
        b.labelFn = [p, i] { return Text(onOff(*p, g.experimentsEdit[i].on)); };
    }
    auto done = [p] {
        g.experiments = g.experimentsEdit;
        openPage(*p, Page::Tabs);
    };
    addButton(s, float(W / 2 - 154), float(H - 27), 150, tr(s, "gui.done"), done);
    addButton(s, float(W / 2 + 4), float(H - 27), 150, tr(s, "gui.cancel"), [p] { openPage(*p, Page::Tabs); });
    s.fe->onBack = [p] { openPage(*p, Page::Tabs); };
    s.fe->onEnter = done;
}

void drawExperiments(State& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "selectWorld.experiments"), float(W / 2), 12, 0xFFFFFFFF);
    const ExperimentsLayout el = experimentsLayout(s);
    for (size_t i = 0; i < el.info.size(); ++i)
        s.drawText(c, el.info[i], float(W / 2 - 155), float(el.top + int(i) * 9), 0xFFFF5555, true);
    const float x = float(W / 2 - 150);
    for (size_t i = 0; i < g.experimentsEdit.size(); ++i) {
        const std::string& id = g.experimentsEdit[i].id;
        const float y = float(el.gridTop + int(i) * 42);
        s.drawText(c, Text(lang(s, "dataPack." + id + ".name", id)), x, y + 6, 0xFFFFFFFF, true);
        const std::vector<Text> d = s.font.split(Text(lang(s, "dataPack." + id + ".description", "")), 299 - 44);
        for (size_t k = 0; k < d.size() && k < 2; ++k) s.drawText(c, d[k], x, y + 20 + float(k) * 9, 0xFFAAAAAA, true);
    }
    s.drawButtons(c);
}

// --- Game rules -----------------------------------------------------------------------------------------------------

struct RuleRow {
    std::string category;  // a category header when rule is null
    const RuleDef* rule = nullptr;
};
std::vector<RuleRow> g_ruleRows;

std::string ruleValue(const RuleDef& d) {
    auto it = g.rulesEdit.find(d.id);
    return it == g.rulesEdit.end() ? d.def : it->second;
}
bool ruleValid(const RuleDef& d, const std::string& v) {
    if (d.boolean) return v == "true" || v == "false";
    if (v.empty() || v.size() > 11) return false;
    char* end = nullptr;
    const long long n = std::strtoll(v.c_str(), &end, 10);
    return *end == 0 && n >= d.min && n <= d.max;
}

void filterRules(State& s) {
    std::string q = g.search.text.value;
    for (char& ch : q) ch = char(std::tolower(u8(ch)));
    auto lower = [](std::string v) {
        for (char& ch : v) ch = char(std::tolower(u8(ch)));
        return v;
    };
    std::map<std::string, std::vector<const RuleDef*>> byCategory;
    for (const RuleDef& d : g.ruleDefs) {
        bool enabled = true;
        for (const std::string& feat : d.features) enabled = enabled && experimentOn(feat);
        if (!enabled) continue;
        const std::string cat = lang(s, "gamerule.category." + pathOf(d.category), pathOf(d.category));
        if (!q.empty() && lower(d.id).find(q) == std::string::npos && lower(ruleName(s, d.id)).find(q) == std::string::npos &&
            lower(cat).find(q) == std::string::npos && lower(ruleDescription(s, d.id)).find(q) == std::string::npos)
            continue;
        byCategory[d.category].push_back(&d);
    }
    g_ruleRows.clear();
    for (auto& [cat, rules] : byCategory) {
        g_ruleRows.push_back({cat, nullptr});
        for (const RuleDef* d : rules) g_ruleRows.push_back({cat, d});
    }
}

ListArea ruleList(State& s) { return {36, float(s.guiH() - kFooter), float(s.guiW() / 2 - 155), 310, 24, int(g_ruleRows.size())}; }

void syncRuleFields() {  // the integer fields on screen -> the edited values
    for (size_t i = 0; i < g.ruleFields.size() && i < g.ruleFieldIds.size(); ++i) {
        const RuleDef* d = ruleDef(g.ruleFieldIds[i]);
        if (!d) continue;
        const std::string& v = g.ruleFields[i].text.value;
        if (v == d->def) g.rulesEdit.erase(d->id);
        else g.rulesEdit[d->id] = v;
    }
}

bool rulesValid() {
    for (const auto& [id, v] : g.rulesEdit)
        if (const RuleDef* d = ruleDef(id); d && !ruleValid(*d, v)) return false;
    return true;
}

void buildGameRules(State& s) {
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    syncRuleFields();
    pollRules(s);
    if (g.rulesLoaded) filterRules(s);
    else g_ruleRows.clear();
    g.search.x = float(W / 2 - 100), g.search.y = 17, g.search.w = 200, g.search.h = 15;
    g.search.hint = s.res.translate("gui.game_rule.search");
    const ListArea l = ruleList(s);
    g.scroll = std::clamp(g.scroll, 0.0f, l.maxScroll());
    g.ruleFields.clear(), g.ruleFieldIds.clear();
    g.ruleFields.reserve(g_ruleRows.size());
    std::vector<Field*> fields{&g.search};
    for (int i = 0; i < l.rows; ++i) {
        const RuleRow& r = g_ruleRows[size_t(i)];
        if (!r.rule || !l.visible(i)) continue;
        const float x = l.left + l.width - 4 - 45, y = l.y(i);
        const RuleDef& d = *r.rule;
        if (d.boolean) {
            Button& b = addButton(s, x, y, 44, Text(), [id = d.id] {
                const RuleDef* rd = ruleDef(id);
                const bool now = ruleValue(*rd) == "true";
                const std::string v = now ? "false" : "true";
                if (v == rd->def) g.rulesEdit.erase(id);
                else g.rulesEdit[id] = v;
            });
            b.labelFn = [p, id = d.id] { return Text(onOff(*p, ruleValue(*ruleDef(id)) == "true")); };
        } else {
            Field fl;
            fl.x = x, fl.y = y, fl.w = 44, fl.h = 20;
            fl.text.value = ruleValue(d);
            fl.text.maxLength = 11;
            fl.text.cursor = fl.text.selection = int(fl.text.value.size());
            g.ruleFields.push_back(std::move(fl));
            g.ruleFieldIds.push_back(d.id);
        }
    }
    for (Field& fl : g.ruleFields) fields.push_back(&fl);
    setFields(s, fields, 0);
    auto done = [p] {
        syncRuleFields();
        if (!rulesValid()) return;
        g.rules = g.rulesEdit;
        openPage(*p, Page::Tabs);
    };
    addButton(s, float(W / 2 - 154), float(H - 27), 150, tr(s, "gui.done"), done, g.rulesLoaded && rulesValid());
    addButton(s, float(W / 2 + 4), float(H - 27), 150, tr(s, "gui.cancel"), [p] { openPage(*p, Page::Tabs); });
    s.fe->onBack = [p] { openPage(*p, Page::Tabs); };
    s.fe->onEnter = done;
}

void drawGameRules(State& s, Canvas& c) {
    const int W = c.guiWidth();
    if (g.rulesTask.valid()) {
        pollRules(s);
        if (g.rulesLoaded) s.buildButtons();
    }
    syncRuleFields();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "editGamerule.title"), float(W / 2), 4, 0xFFFFFFFF);
    const ListArea l = ruleList(s);
    drawListFrame(s, c, l);
    std::vector<Text> tooltip;
    if (!g.rulesLoaded)
        s.drawCentered(c, Text(g.rulesFailed ? "The game rules could not be read from the game." : "Reading the game rules from the game..."),
                       float(W / 2), l.top + 20, 0xFFA0A0A0);
    c.pushScissor(0, l.top, float(W), l.bottom);
    for (int i = 0; i < l.rows; ++i) {
        const float y = l.y(i);
        if (y + float(l.rowH) < l.top || y > l.bottom) continue;
        const RuleRow& r = g_ruleRows[size_t(i)];
        if (!r.rule) {  // CategoryRuleEntry
            s.drawCentered(c, Text(lang(s, "gamerule.category." + pathOf(r.category), pathOf(r.category))), float(W / 2), y + 6, 0xFFFFFFFF);
            continue;
        }
        const std::vector<Text> name = s.font.split(Text(ruleName(s, r.rule->id)), 170);
        const float x = l.left + 4;
        if (name.size() == 1) s.drawText(c, name[0], x, y + 5, 0xFFFFFFFF, true);
        else
            for (size_t k = 0; k < name.size() && k < 2; ++k) s.drawText(c, name[k], x, y + float(k) * 10, 0xFFFFFFFF, true);
        if (!r.rule->boolean && !ruleValid(*r.rule, ruleValue(*r.rule)))  // an invalid number: red frame
            c.fill(l.left + l.width - 50, y - 1, l.left + l.width - 3, y + 21, 0xFFFF0000);
        if (s.mouseX >= x && s.mouseX < x + 170 && s.mouseY >= y && s.mouseY < y + 20 && s.mouseY >= l.top && s.mouseY < l.bottom) {
            tooltip.push_back(Text(r.rule->id, colorStyle(0xFFFF55)));
            for (const Text& t : s.font.split(Text(ruleDescription(s, r.rule->id)), 150))
                if (!t.empty()) tooltip.push_back(t);
            tooltip.push_back(trf(s, "editGamerule.default", {Text(r.rule->def)}).withStyle(colorStyle(0xAAAAAA)));
        }
    }
    c.popScissor();
    drawFields(s, c);
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
}

// --- Data packs -----------------------------------------------------------------------------------------------------

ListArea packList(State& s) { return {33, float(s.guiH() - kFooter), float(s.guiW() / 2 - 155), 310, 36, int(g.packsEdit.size()) + 1}; }

// The selected packs asking for features beyond the vanilla set (FeatureFlags.printMissingFlags): (title, flags).
std::vector<std::pair<std::string, std::string>> experimentalPacks() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const PackChoice& c : g.packsEdit) {
        if (!c.on) continue;
        for (const mods::Entry& e : mods::Manager::get().entries()) {
            if (e.id != c.id) continue;
            std::string flags;
            for (const std::string& f : e.features)
                if (f != "minecraft:vanilla") flags += (flags.empty() ? "" : ", ") + f;
            if (!flags.empty()) out.push_back({c.name, flags});
        }
    }
    return out;
}

// ConfirmExperimentalFeaturesScreen: the title, the message (310 wide), Details, then Proceed / Back; rows 20 apart.
int confirmTop(State& s, std::vector<Text>* lines) {
    std::vector<Text> l = s.font.split(tr(s, "selectWorld.experimental.message"), 310);
    const int h = 9 + 20 + int(l.size()) * 9 + 20 + 20 + 20 + 20;
    if (lines) *lines = std::move(l);
    return (s.guiH() - h) / 2;
}

void buildConfirmExperimental(State& s) {
    State* p = &s;
    std::vector<Text> lines;
    const int top = confirmTop(s, &lines), W = s.guiW();
    const int detailsY = top + 9 + 20 + int(lines.size()) * 9 + 20, buttonsY = detailsY + 20 + 20;
    addButton(s, float(W / 2 - 50), float(detailsY), 100, tr(s, "selectWorld.experimental.details"),
              [p] { openPage(*p, Page::ExperimentalDetails); });
    addButton(s, float(W / 2 - 155), float(buttonsY), 150, tr(s, "gui.proceed"), [p] {
        g.packs = g.packsEdit;
        openPage(*p, Page::Tabs);
    });
    auto back = [p] {  // onAbort: the data packs again with the applied selection
        g.packsEdit = g.packs;
        openPage(*p, Page::DataPacks);
    };
    addButton(s, float(W / 2 + 5), float(buttonsY), 150, tr(s, "gui.back"), back);
    s.fe->onBack = back;
    s.fe->onEnter = nullptr;
}

void drawConfirmExperimental(State& s, Canvas& c) {
    menuBackground(s, c);
    std::vector<Text> lines;
    const int top = confirmTop(s, &lines);
    const float cx = float(c.guiWidth() / 2);
    s.drawCentered(c, tr(s, "selectWorld.experimental.title"), cx, float(top), 0xFFFFFFFF);
    for (size_t i = 0; i < lines.size(); ++i) s.drawCentered(c, lines[i], cx, float(top + 29 + int(i) * 9), 0xFFFFFFFF);
    s.drawButtons(c);
}

// ConfirmExperimentalFeaturesScreen.DetailsScreen: each pack (bold) and selectWorld.experimental.details.entry.
void buildExperimentalDetails(State& s) {
    State* p = &s;
    addButton(s, float(s.guiW() / 2 - 75), float(s.guiH() - 27), 150, tr(s, "gui.back"), [p] { openPage(*p, Page::ConfirmExperimental); });
    s.fe->onBack = [p] { openPage(*p, Page::ConfirmExperimental); };
    s.fe->onEnter = nullptr;
}

void drawExperimentalDetails(State& s, Canvas& c) {
    menuBackground(s, c);
    const int W = c.guiWidth(), rowW = W * 3 / 4, x = (W - rowW) / 2;
    s.drawCentered(c, tr(s, "selectWorld.experimental.details.title"), float(W / 2), 12, 0xFFFFFFFF);
    c.pushScissor(0, 33, float(W), float(c.guiHeight() - 33));
    int y = 33 + 4;
    Style bold;
    bold.bold = 1;
    for (const auto& [title, flags] : experimentalPacks()) {
        s.drawText(c, Text(title, bold), float(x), float(y), 0xFFFFFFFF, true);
        const std::vector<Text> lines = s.font.split(translatable("selectWorld.experimental.details.entry", {Text(flags)}, s.lang()), rowW);
        for (size_t i = 0; i < lines.size() && i < 2; ++i) s.drawText(c, lines[i], float(x), float(y + 12 + int(i) * 9), 0xFFFFFFFF, true);
        y += 33;
    }
    c.popScissor();
    s.drawButtons(c);
}

void buildDataPacks(State& s) {
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    const ListArea l = packList(s);
    g.scroll = std::clamp(g.scroll, 0.0f, l.maxScroll());
    for (int i = 1; i < l.rows; ++i) {
        if (!l.visible(i)) continue;
        const size_t k = size_t(i - 1);
        Button& b = addButton(s, l.left + l.width - 4 - 45, l.y(i) + 6, 44, Text(), [k] { g.packsEdit[k].on = !g.packsEdit[k].on; },
                              !g.packsEdit[k].locked);
        b.labelFn = [p, k] { return Text(onOff(*p, g.packsEdit[k].on)); };
        if (g.packsEdit[k].locked) b.tooltip = [] { return Text("On for every world (Mods & Packs)"); };
    }
    addButton(s, float(W / 2 - 154), float(H - 27), 150, tr(s, "pack.openFolder"), [] { mods::Manager::get().openModsFolder(); });
    auto done = [p] {  // CreateWorldScreen.tryApplyNewDataPacks: a changed selection asking for experimental features
        bool changed = g.packs.size() != g.packsEdit.size();
        for (size_t i = 0; i < g.packs.size() && !changed; ++i) changed = g.packs[i].on != g.packsEdit[i].on;
        if (changed && !experimentalPacks().empty()) return openPage(*p, Page::ConfirmExperimental);
        g.packs = g.packsEdit;
        openPage(*p, Page::Tabs);
    };
    addButton(s, float(W / 2 + 4), float(H - 27), 150, tr(s, "gui.done"), done);
    s.fe->onBack = [p] { openPage(*p, Page::Tabs); };
    s.fe->onEnter = done;
}

void drawDataPacks(State& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "dataPack.title"), float(W / 2), 12, 0xFFFFFFFF);
    const ListArea l = packList(s);
    drawListFrame(s, c, l);
    c.pushScissor(0, l.top, float(W), l.bottom);
    for (int i = 0; i < l.rows; ++i) {
        const float y = l.y(i);
        if (y + float(l.rowH) < l.top || y > l.bottom) continue;
        const std::string name = i == 0 ? s.res.translate("dataPack.vanilla.name") : g.packsEdit[size_t(i - 1)].name;
        const std::string desc = i == 0 ? s.res.translate("dataPack.vanilla.description") : g.packsEdit[size_t(i - 1)].description;
        s.drawText(c, Text(fitText(s.font, name, 250)), l.left + 4, y + 2, 0xFFFFFFFF, true);
        const std::vector<Text> d = s.font.split(Text(desc), 250);
        for (size_t k = 0; k < d.size() && k < 2; ++k) s.drawText(c, d[k], l.left + 4, y + 13 + float(k) * 9, 0xFF808080, true);
    }
    if (l.rows == 1)
        s.drawCentered(c, Text("Drop data packs into the Mods & Packs folder to add them"), float(W / 2), l.y(1) + 10, 0xFFA0A0A0);
    c.popScissor();
    s.drawButtons(c);
    hoverTooltips(s, c);
}

// --- creating ---------------------------------------------------------------------------------------------------------

bool copyPack(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::is_directory(from, ec)) fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    else fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

void createWorldNow(State& s) {
    FrontEndState& f = *s.fe;
    std::string name = f.createName.text.value;
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) name = s.res.translate("selectWorld.newWorld");
    const std::string folder = worldFolderFor(f.cfg.savesDir, name);
    const fs::path dir = f.cfg.savesDir / widen(folder);
    const bool debug = isDebug(), hc = f.createMode == 1;
    const char* mode = debug ? "spectator" : kModes[f.createMode];
    std::vector<std::wstring> a = launch::stickyArguments();
    a.insert(a.end(), {L"--play", L"--world", dir.wstring(), L"--create-name", widen(name), L"--create-seed",
                       widen(f.createSeed.text.value), L"--create-mode", widen(mode), L"--create-difficulty",
                       widen(kDifficulties[debug ? 0 : hc ? 3 : f.createDifficulty]), L"--create-cheats",
                       (f.createCheats && !hc) || debug ? L"1" : L"0", L"--create-structures", f.createStructures ? L"1" : L"0"});
    if (g.preset != "minecraft:normal") a.insert(a.end(), {L"--create-type", widen(g.preset)});
    if (g.preset == "minecraft:flat") a.insert(a.end(), {L"--create-generator", widen(flatJson(g.flat).dump())});
    if (g.preset == "minecraft:single_biome_surface") a.insert(a.end(), {L"--create-biome", widen(g.biome)});
    if (g.bonusChest && !hc && !debug) a.insert(a.end(), {L"--create-bonus", L"1"});
    for (const auto& [id, v] : g.rules) {
        const RuleDef* d = ruleDef(id);
        if (!d || v != d->def) a.insert(a.end(), {L"--create-gamerule", widen(id + "=" + v)});
    }
    std::string packs = "vanilla", disabled;
    for (const Experiment& e : g.experiments) {
        std::string& list = e.on ? packs : disabled;
        list += (list.empty() ? "" : ",") + e.id;
    }
    if (packs != "vanilla") a.insert(a.end(), {L"--create-packs", widen(packs), L"--create-disabled-packs", widen(disabled)});
    // The world's own data packs (Data Packs page, a re-created world's): copied in before the server makes it.
    std::error_code ec;
    for (const PackChoice& p : g.packs) {
        if (!p.on || p.locked) continue;
        fs::create_directories(dir / "datapacks", ec);
        if (!copyPack(p.path, dir / "datapacks" / p.path.filename())) LOG("ui: could not copy the data pack %s", utf8(p.path).c_str());
    }
    LOG("ui: creating world \"%s\" in %s (%s)", name.c_str(), folder.c_str(), g.preset.c_str());
    f.failTitle = Text("Could not create the world");
    startChild(s, a, Text("Creating \"" + name + "\""), Screen::Worlds, false);
}

}  // namespace

void openCreateWorld(State& s) {
    FrontEndState& f = *s.fe;
    loadData(s);
    f.createName.text = TextField{};
    f.createName.text.value = s.res.translate("selectWorld.newWorld");
    f.createName.text.cursor = f.createName.text.selection = int(f.createName.text.value.size());
    f.createName.text.maxLength = 32;
    f.createSeed.text = TextField{};
    f.createSeed.text.maxLength = 32;
    f.createSeed.hint = s.res.translate("selectWorld.seedInfo");
    f.createMode = 0, f.createDifficulty = 2, f.createCheats = false, f.createStructures = true;
    g.page = Page::Tabs, g.tab = 0;
    g.preset = "minecraft:normal", g.bonusChest = false, g.biome = "minecraft:plains";
    for (const FlatPreset& p : g.flatPresets)
        if (p.id == "minecraft:classic_flat") g.flat = p.settings;
    for (Experiment& e : g.experiments) e.on = false;
    g.rules.clear();
    resetPacks();
    ensureRules(s);  // the game rules page needs them (a one-time extraction on first use)
    s.openScreen(Screen::CreateWorld);
}

void buildCreateWorld(State& s) {
    switch (g.page) {
        case Page::Tabs: return buildTabs(s);
        case Page::Flat: return buildFlat(s);
        case Page::FlatPresets: return buildFlatPresets(s);
        case Page::Buffet: return buildBuffet(s);
        case Page::Experiments: return buildExperiments(s);
        case Page::GameRules: return buildGameRules(s);
        case Page::DataPacks: return buildDataPacks(s);
        case Page::ConfirmExperimental: return buildConfirmExperimental(s);
        case Page::ExperimentalDetails: return buildExperimentalDetails(s);
    }
}

void drawCreateWorld(State& s, Canvas& c) {
    switch (g.page) {
        case Page::Tabs: return drawTabs(s, c);
        case Page::Flat: return drawFlat(s, c);
        case Page::FlatPresets: return drawFlatPresets(s, c);
        case Page::Buffet: return drawBuffet(s, c);
        case Page::Experiments: return drawExperiments(s, c);
        case Page::GameRules: return drawGameRules(s, c);
        case Page::DataPacks: return drawDataPacks(s, c);
        case Page::ConfirmExperimental: return drawConfirmExperimental(s, c);
        case Page::ExperimentalDetails: return drawExperimentalDetails(s, c);
    }
}

void createWorldInput(State& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    FrontEndState& f = *s.fe;
    const bool click = e.type == E::Button && e.down && e.code == 0;
    if (g.page == Page::Tabs) {
        const TabGeom tg = tabGeom(s);
        if (click && s.mouseY >= 0 && s.mouseY < float(kTabBar) && s.mouseX >= float(tg.x) && s.mouseX < float(tg.x + 3 * tg.w)) {
            const int t = int((s.mouseX - float(tg.x)) / float(tg.w));
            if (t != g.tab) {
                audio::playUiClick();
                g.tab = t;
                s.buildButtons();
            }
            return;
        }
        if (e.type == E::Key && e.down && e.code == VK_TAB && s.window->keyDown(VK_CONTROL)) {  // Ctrl+Tab: next tab
            g.tab = (g.tab + (s.window->keyDown(VK_SHIFT) ? 2 : 1)) % 3;
            s.buildButtons();
            return;
        }
    } else {
        ListArea l{0, 0, 0, 0, 1, 0};
        switch (g.page) {
            case Page::Flat: l = flatList(s); break;
            case Page::FlatPresets: l = presetList(s); break;
            case Page::Buffet: l = biomeList(s); break;
            case Page::GameRules: l = ruleList(s); break;
            case Page::DataPacks: l = packList(s); break;
            default: break;
        }
        if (e.type == E::Wheel && l.rows) {
            g.scroll = std::clamp(g.scroll - float(e.code) * float(l.rowH) / 2, 0.0f, l.maxScroll());
            if (g.page == Page::GameRules || g.page == Page::DataPacks) s.buildButtons();  // widgets in the rows
            return;
        }
        if (click && g.page != Page::GameRules && g.page != Page::DataPacks) {
            const int i = l.at(s.mouseX, s.mouseY);
            if (i >= 0) {
                if (g.page == Page::Flat && i > 0) g.sel = int(g.flatEdit.layers.size()) - i;
                if (g.page == Page::FlatPresets) {
                    g.sel = i;
                    g.presetText.text.value = flatString(g.flatPresets[size_t(i)].settings);
                    g.presetText.text.cursor = g.presetText.text.selection = 0;
                    if (doubleClicked(s, i)) return usePreset(s);
                }
                if (g.page == Page::Buffet) {
                    g.biomeEdit = g.biomes[size_t(g.view[size_t(i)])];
                    if (doubleClicked(s, i)) {
                        g.biome = g.biomeEdit;
                        return openPage(s, Page::Tabs);
                    }
                }
                f.focus = -1;
                s.buildButtons();
                return;
            }
        }
    }
    const std::string before = g.search.text.value, presetBefore = g.presetText.text.value;
    if (fieldsInput(s, e)) {
        if (g.page == Page::Buffet && g.search.text.value != before) filterBiomes(s), s.buildButtons();
        if (g.page == Page::GameRules && g.search.text.value != before) g.scroll = 0, s.buildButtons();
        if (g.page == Page::FlatPresets && g.presetText.text.value != presetBefore) {
            const int focus = f.focus;
            s.buildButtons();
            f.focus = focus;
        }
        if (g.page == Page::GameRules) {
            syncRuleFields();
            for (Button& b : s.buttons)  // Done follows the numbers' validity (the buttons keep their places)
                if (b.label.plain() == s.res.translate("gui.done")) b.active = g.rulesLoaded && rulesValid();
        }
        return;
    }
    s.buttonsInput(e);
}

// Re-Create (WorldSelectionList.recreateWorld -> WorldOpenFlows.recreateWorldData): Create New World filled in with an
// existing world's settings, generation and data packs; the copy gets its own folder.
bool recreateWorld(State& s, const fs::path& dir) {
    NbtValue level, gen, rules;
    if (!readNbtGz(dir / "level.dat", level)) return false;
    NbtValue* data = level.find("Data");
    if (!data) return false;
    openCreateWorld(s);
    FrontEndState& f = *s.fe;
    auto num = [](NbtValue* c, const char* key, i64 fallback) {
        NbtValue* v = c ? c->find(key) : nullptr;
        return v ? v->i : fallback;
    };
    if (NbtValue* n = data->find("LevelName"); n && !n->s.empty()) {
        f.createName.text.value = n->s.substr(0, 32);
        f.createName.text.cursor = f.createName.text.selection = int(f.createName.text.value.size());
    }
    const int type = int(num(data, "GameType", 0));
    std::string difficulty;
    bool hardcore = num(data, "hardcore", 0) != 0;
    if (NbtValue* ds = data->find("difficulty_settings")) {
        if (NbtValue* d = ds->find("difficulty")) difficulty = d->s;
        hardcore = hardcore || num(ds, "hardcore", 0) != 0;
    } else {
        difficulty = kDifficulties[std::clamp(int(num(data, "Difficulty", 2)), 0, 3)];
    }
    for (int i = 0; i < 4; ++i)
        if (difficulty == kDifficulties[i]) f.createDifficulty = i;
    f.createMode = hardcore ? 1 : type == 1 ? 2 : 0;
    f.createCheats = num(data, "allowCommands", 0) != 0;
    if (NbtValue* packs = data->find("DataPacks"))
        if (NbtValue* enabled = packs->find("Enabled"))
            for (const NbtValue& v : enabled->items)
                for (Experiment& e : g.experiments)
                    if (e.id == v.s) e.on = true;
    // 26.x: data/minecraft/world_gen_settings.dat; older: level.dat WorldGenSettings.
    NbtValue* settings = nullptr;
    if (readNbtGz(dir / "data" / "minecraft" / "world_gen_settings.dat", gen)) settings = gen.find("data");
    if (!settings) settings = data->find("WorldGenSettings");
    if (settings) {
        if (NbtValue* sd = settings->find("seed")) f.createSeed.text.value = std::to_string(sd->i);
        f.createStructures = num(settings, "generate_structures", num(settings, "generate_features", 1)) != 0;
        g.bonusChest = num(settings, "bonus_chest", 0) != 0;
        NbtValue* dims = settings->find("dimensions");
        NbtValue* ow = dims ? dims->find("minecraft:overworld") : nullptr;
        NbtValue* gener = ow ? ow->find("generator") : nullptr;
        if (gener) {
            const NbtValue* t = gener->find("type");
            const std::string gt = t ? t->s : "";
            if (gt == "minecraft:flat") {
                g.preset = "minecraft:flat";
                if (NbtValue* fs = gener->find("settings")) {
                    FlatSettings fl;
                    if (NbtValue* layers = fs->find("layers"))
                        for (NbtValue& l : layers->items) {
                            NbtValue* b = l.find("block");
                            fl.layers.push_back({b ? b->s : "minecraft:air", int(num(&l, "height", 1))});
                        }
                    if (NbtValue* b = fs->find("biome")) fl.biome = b->s;
                    fl.features = num(fs, "features", 0) != 0, fl.lakes = num(fs, "lakes", 0) != 0;
                    if (NbtValue* so = fs->find("structure_overrides"))
                        for (NbtValue& v : so->items) fl.structures.push_back(v.s);
                    if (!fl.layers.empty()) g.flat = fl;
                }
            } else if (gt == "minecraft:debug") {
                g.preset = "minecraft:debug_all_block_states";
            } else {
                NbtValue* st = gener->find("settings");
                NbtValue* src = gener->find("biome_source");
                NbtValue* srcType = src ? src->find("type") : nullptr;
                if (srcType && srcType->s == "minecraft:fixed") {
                    g.preset = "minecraft:single_biome_surface";
                    if (NbtValue* b = src->find("biome")) g.biome = b->s;
                } else if (st && st->s == "minecraft:large_biomes") {
                    g.preset = "minecraft:large_biomes";
                } else if (st && st->s == "minecraft:amplified") {
                    g.preset = "minecraft:amplified";
                }
            }
        }
    }
    if (readNbtGz(dir / "data" / "minecraft" / "game_rules.dat", rules))  // compared with the defaults when created
        if (NbtValue* rd = rules.find("data"))
            for (size_t i = 0; i < rd->names.size(); ++i)
                g.rules[rd->names[i]] = rd->items[i].type == TagByte ? (rd->items[i].i ? "true" : "false") : std::to_string(rd->items[i].i);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir / "datapacks", ec))  // the world's own packs come along
        g.packs.push_back({"world/" + utf8(e.path().filename()), utf8(e.path().filename()), "From the original world", e.path(), true, false});
    LOG("ui: re-creating world %s (%s)", utf8(dir.filename()).c_str(), g.preset.c_str());
    s.buildButtons();
    return true;
}

const std::vector<GameRuleInfo>* gameRuleInfos(State& s) {
    ensureRules(s);
    if (g.rulesTask.valid() && g.rulesTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool ok = g.rulesTask.get();
        g.rulesLoaded = ok, g.rulesFailed = !ok;
    }
    if (!g.rulesLoaded) return nullptr;
    static std::vector<GameRuleInfo> infos;
    if (infos.size() != g.ruleDefs.size()) {
        infos.clear();
        for (const RuleDef& d : g.ruleDefs) infos.push_back({d.id, d.category, d.def, d.boolean, d.min, d.max});
    }
    return &infos;
}
bool gameRulesFailed() { return g.rulesFailed; }
std::string gameRuleName(State& s, const std::string& id) { return ruleName(s, id); }
std::string gameRuleDescription(State& s, const std::string& id) { return ruleDescription(s, id); }
std::string gameRuleCategoryName(State& s, const std::string& category) {
    return lang(s, "gamerule.category." + pathOf(category), pathOf(category));
}

}  // namespace ui
