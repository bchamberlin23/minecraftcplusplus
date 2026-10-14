// Station screens: stonecutter (StonecutterScreen, recipes from ClientboundUpdateRecipesPacket), loom (LoomScreen,
// selectable patterns per LoomMenu from the banner_pattern registry and tags, banner preview) and cartography table
// (CartographyTableScreen with the map image from ClientboundMapItemDataPacket), registered as StationDefs.
#include "audio/audio.h"
#include "ui/item_props.h"
#include "ui/item_ui.h"

#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace ui {

namespace {

void playerSlots(Layout& l, int start) {
    l.playerStart = start;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 9; ++c) l.slots.push_back({8 + c * 18, 84 + r * 18});
    for (int c = 0; c < 9; ++c) l.slots.push_back({8 + c * 18, 142});
}
bool inRect(float mx, float my, float x, float y, float w, float h) { return mx >= x && my >= y && mx < x + w && my < y + h; }
std::string itemName(UiState& s, const ItemStack& st) {
    if (st.empty()) return {};
    const std::string& n = s.items.name(st.item);
    return n.substr(n.find(':') + 1);
}
const Sprite* sp(UiState& s, std::string_view name) { return s.sprites.sprite(name); }
void uiSound(const char* event) {
    if (audio::AudioSystem* a = audio::instance()) a->playUi(event);
}
// The raw value of an added data component ("banner_patterns"), nullptr when absent.
const std::string* component(UiState& s, const ItemStack& st, const char* name) {
    const int type = s.items.componentType(name);
    for (const auto& c : st.added)
        if (c.type == type) return &c.data;
    return nullptr;
}

// --- packets: stonecutter recipes, map data -------------------------------------------------------------------

struct StoneRecipe {
    std::vector<int> inputs;  // Ingredient: item ids, or
    std::string tag;          // an item tag ("minecraft:planks")
    ItemStack result;
};
std::vector<StoneRecipe> g_stoneRecipes;

struct MapImage {
    int scale = 0;
    bool locked = false;
    std::vector<u8> colors = std::vector<u8>(128 * 128, 0);
    int ax = -1, ay = -1;  // GUI atlas region
    bool dirty = true;
    u32 version = 0;  // updates (mapColors: maps in item frames)
    std::vector<MapDecor> decorations;
};
std::unordered_map<int, MapImage> g_maps;

// SlotDisplay.STREAM_CODEC (the first stack it resolves to, for the stonecutter's option display).
bool readSlotDisplay(UiState& s, net::Reader& r, ItemStack& out, int depth) {
    if (depth > 8) return false;
    const int type = r.varInt();
    ItemStack tmp;
    auto holderSet = [&](std::vector<int>* ids) {
        const i32 n = r.varInt() - 1;
        if (n < 0) return r.str(), r.ok;
        for (i32 i = 0; i < n && r.ok; ++i) {
            const int id = r.varInt();
            if (ids) ids->push_back(id);
        }
        return r.ok;
    };
    switch (type) {
        case 0: case 1: return r.ok;                               // empty, any_fuel
        case 2: return readSlotDisplay(s, r, out, depth + 1);      // with_any_potion
        case 3: return readSlotDisplay(s, r, out, depth + 1) && (r.varInt(), r.ok);  // only_with_component
        case 4: out = ItemStack{r.varInt(), 1}; return r.ok;       // item
        case 5: return s.items.readTemplate(r, out);               // item_stack
        case 6: {                                                  // tag
            std::vector<int> ids;
            if (!holderSet(&ids)) return false;
            if (!ids.empty()) out = ItemStack{ids[0], 1};
            return true;
        }
        case 7: case 9:                                            // dyed, with_remainder
            return readSlotDisplay(s, r, out, depth + 1) && readSlotDisplay(s, r, tmp, depth + 1);
        case 10: {                                                 // composite
            const i32 n = r.varInt();
            for (i32 i = 0; i < n && r.ok; ++i)
                if (!readSlotDisplay(s, r, i == 0 ? out : tmp, depth + 1)) return false;
            return r.ok;
        }
        default: return false;  // smithing_trim (not used by stonecutting)
    }
}

void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("update_recipes", [sp](net::Reader& r) {
        UiState& s = *sp;
        std::unordered_map<std::string, std::vector<int>> propertySets;  // RecipePropertySet key -> item ids
        const i32 sets = r.varInt();
        for (i32 i = 0; i < sets && r.ok; ++i) {
            std::vector<int>& ids = propertySets[std::string(r.str())];
            const i32 n = r.varInt();
            for (i32 k = 0; k < n && r.ok; ++k) ids.push_back(r.varInt());
        }
        std::vector<StoneRecipe> recipes;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            StoneRecipe e;
            const i32 k = r.varInt() - 1;  // Ingredient: holder set (0: a tag)
            if (k < 0) e.tag = std::string(r.str());
            for (i32 j = 0; j < k && r.ok; ++j) e.inputs.push_back(r.varInt());
            if (!readSlotDisplay(s, r, e.result, 0)) break;
            recipes.push_back(std::move(e));
        }
        if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: update_recipes: %zu stonecutter recipes", recipes.size());
        s.post([recipes = std::move(recipes), propertySets = std::move(propertySets)]() mutable {
            g_stoneRecipes = std::move(recipes);
            setPropertySets(std::move(propertySets));  // item_ui.h: station input predicates
        });
    });
    client.onPlay("map_item_data", [sp](net::Reader& r) {
        const int id = r.varInt();
        const int scale = r.u8v();
        const bool locked = r.boolean();
        const bool hasDecorations = r.boolean();
        std::vector<MapDecor> decorations;
        if (hasDecorations) {  // MapDecoration: type, x, y (signed bytes), rotation (& 15), optional name
            const i32 n = r.varInt();
            for (i32 i = 0; i < n && r.ok; ++i) {
                MapDecor d;
                d.type = r.varInt();
                d.x = i8(r.u8v()), d.y = i8(r.u8v()), d.rot = r.u8v() & 15;
                if (r.boolean()) d.name = std::string(r.nbt());
                decorations.push_back(std::move(d));
            }
        }
        int w = r.u8v(), h = 0, x = 0, y = 0;
        std::string colors;
        if (w) {
            h = r.u8v(), x = r.u8v(), y = r.u8v();
            colors = std::string(r.byteArray());
        }
        if (!r.ok) return;
        sp->post([id, scale, locked, w, h, x, y, colors = std::move(colors), hasDecorations, decorations = std::move(decorations)] {
            MapImage& m = g_maps[id];
            m.scale = scale, m.locked = locked;
            if (hasDecorations) m.decorations = decorations;  // ClientboundMapItemDataPacket.applyToMap
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                    if (size_t(i + j * w) < colors.size() && x + i < 128 && y + j < 128)
                        m.colors[size_t(x + i + (y + j) * 128)] = u8(colors[size_t(i + j * w)]);
            if (w) m.dirty = true, ++m.version;
        });
    });
}

void attached(UiState&) { g_maps.clear(); }

const UiModuleRegistrar kModule([] {
    UiModule m;
    m.name = "stations_a2";
    m.registerHandlers = registerHandlers;
    m.attached = attached;
    return m;
}());

// --- stonecutter ----------------------------------------------------------------------------------------------

struct StonecutterState {
    float scrollOffs = 0;
    int startIndex = 0;
    bool scrolling = false;
    int lastInput = -2;
} stone;

bool accepts(UiState& s, const StoneRecipe& r, int item) {  // Ingredient.test
    if (!r.tag.empty()) return s.items.itemHasTag(item, r.tag);
    return std::find(r.inputs.begin(), r.inputs.end(), item) != r.inputs.end();
}

std::vector<int> visibleRecipes(UiState& s, const Menu& m) {
    std::vector<int> v;
    const ItemStack& in = m.slots[0];
    if (in.empty()) return v;
    for (size_t i = 0; i < g_stoneRecipes.size(); ++i)
        if (accepts(s, g_stoneRecipes[i], in.item)) v.push_back(int(i));
    return v;
}

void stoneLayout(const Menu&, Layout& l) {
    l.background = "gui/container/stonecutter";
    l.slots = {{20, 33}, {143, 33}};
    playerSlots(l, 2);
    l.titleY = 5;
}
void stoneOpened(UiState&, Menu&) { stone = StonecutterState{}; }

void stoneSync(const Menu& m) {  // slotUpdateListener: every change of the input slot (the count too)
    const int in = m.slots[0].empty() ? -1 : m.slots[0].item * 1024 + std::min(m.slots[0].count, 1023);
    if (in == stone.lastInput) return;
    stone.lastInput = in;
    stone.scrollOffs = 0, stone.startIndex = 0;
}

void stoneDrawBg(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    stoneSync(m);
    const std::vector<int> vis = visibleRecipes(s, m);
    const bool display = !m.slots[0].empty();
    const bool active = display && vis.size() > 12;
    const int offscreen = (int(vis.size()) + 3) / 4 - 3;
    if (stone.scrolling && !s.leftDown) stone.scrolling = false;
    if (stone.scrolling && active) {  // mouseDragged
        const float y0 = top + 14, y1 = y0 + 54;
        stone.scrollOffs = std::clamp((s.mouseY - y0 - 7.5f) / (y1 - y0 - 15.0f), 0.0f, 1.0f);
        stone.startIndex = int(stone.scrollOffs * float(offscreen) + 0.5f) * 4;
    }
    blitSprite(c, sp(s, active ? "container/stonecutter/scroller" : "container/stonecutter/scroller_disabled"), left + 119,
               top + 15 + float(int(41.0f * stone.scrollOffs)), 12, 15);
    if (inRect(s.mouseX, s.mouseY, left + 119, top + 15, 12, 54))
        s.requestCursor(!active ? UiState::CursorNotAllowed : stone.scrolling ? UiState::CursorResizeNS : UiState::CursorPointingHand);
    if (!display) return;
    const float x = left + 52, y = top + 14;
    const int selected = m.data[0];
    for (int index = stone.startIndex; index < stone.startIndex + 12 && index < int(vis.size()); ++index) {
        const int pos = index - stone.startIndex;
        const float px = x + float(pos % 4 * 16), py = y + float(pos / 4 * 18 + 2);
        const bool hover = inRect(s.mouseX, s.mouseY, px, py, 16, 18);
        if (inRect(s.mouseX, s.mouseY, px, py - 1, 16, 18)) s.requestCursor(UiState::CursorPointingHand);  // the sprite
        const char* sprite = index == selected ? "container/stonecutter/recipe_selected"
                             : hover ? "container/stonecutter/recipe_highlighted"
                                     : "container/stonecutter/recipe";
        blitSprite(c, sp(s, sprite), px, py - 1, 16, 18);
    }
    for (int index = stone.startIndex; index < stone.startIndex + 12 && index < int(vis.size()); ++index) {
        const int pos = index - stone.startIndex;
        s.drawItem(c, g_stoneRecipes[size_t(vis[size_t(index)])].result, x + float(pos % 4 * 16), y + float(pos / 4 * 18 + 2));
    }
}

void stoneDrawTop(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {  // extractTooltip
    if (m.slots[0].empty()) return;
    const std::vector<int> vis = visibleRecipes(s, m);
    for (int index = stone.startIndex; index < stone.startIndex + 12 && index < int(vis.size()); ++index) {
        const int pos = index - stone.startIndex;
        const float px = left + 52 + float(pos % 4 * 16), py = top + 14 + float(pos / 4 * 18 + 2);
        if (inRect(s.mouseX, s.mouseY, px, py, 16, 18))
        {
            const ItemStack& result = g_stoneRecipes[size_t(vis[size_t(index)])].result;
            s.drawTooltip(c, s.items.tooltip(result, s.settings.advancedItemTooltips), s.mouseX, s.mouseY, nullptr, true,
                          tooltipStyle(s.items, result));
        }
    }
}

bool stoneInput(UiState& s, const InputEvent& e, Menu& m, const Layout&, float left, float top) {
    const std::vector<int> vis = visibleRecipes(s, m);
    const bool display = !m.slots[0].empty();
    const int offscreen = (int(vis.size()) + 3) / 4 - 3;
    if (e.type == InputEvent::Wheel) {
        if (display && vis.size() > 12) {
            stone.scrollOffs = std::clamp(stone.scrollOffs - float(e.code) / float(offscreen), 0.0f, 1.0f);
            stone.startIndex = int(stone.scrollOffs * float(offscreen) + 0.5f) * 4;
        }
        return true;
    }
    if (e.type == InputEvent::Button && !e.down) stone.scrolling = false;  // mouseReleased: any button
    if (e.type == InputEvent::Button && e.down && display)  // mouseClicked: any button
        for (int index = stone.startIndex; index < stone.startIndex + 12; ++index) {
            const int pos = index - stone.startIndex;
            if (!inRect(s.mouseX, s.mouseY, left + 52 + float(pos % 4 * 16), top + 14 + float(pos / 4 * 18), 16, 18)) continue;
            // StonecutterMenu.clickMenuButton: false only for the selected recipe again (an empty cell still clicks)
            if (index == m.data[0]) return true;
            // setupResultSlot: the client's entries carry no recipe, so the result empties until the server's
            if (index < int(vis.size())) m.data[0] = index, m.slots[1] = ItemStack{};
            uiSound("minecraft:ui.stonecutter.select_recipe");
            sendContainerButton(s, index);
            return true;
        }
    if (e.type != InputEvent::Button || !e.down || !display) return false;  // the scroller: any button
    if (inRect(s.mouseX, s.mouseY, left + 119, top + 9, 12, 54)) {
        stone.scrolling = true;
        return true;
    }
    return false;
}

int stoneQuickMove(UiState& s, const Menu& m, int index, const ItemStack& st, const std::function<bool(int, int, bool)>& moveTo) {
    if (index == 1) return moveTo(2, 38, true) ? 1 : 0;
    if (index == 0) return moveTo(2, 38, false) ? 1 : 0;
    for (const StoneRecipe& r : g_stoneRecipes)  // stonecutterRecipes().acceptsInput
        if (accepts(s, r, st.item)) return moveTo(0, 1, false) ? 1 : 0;
    (void)s, (void)m;
    if (index < 29) return moveTo(29, 38, false) ? 1 : 0;
    return moveTo(2, 29, false) ? 1 : 0;
}

const StationRegistrar kStonecutter({"stonecutter"}, [] {
    StationDef d;
    d.layout = stoneLayout, d.opened = stoneOpened, d.drawBg = stoneDrawBg, d.drawTop = stoneDrawTop, d.input = stoneInput;
    d.resultSlot = 1, d.quickMove = stoneQuickMove, d.pickAll = 0;
    d.onTake = [](UiState&, Menu& m) { m.slots[0] = m.slots[0].withCount(m.slots[0].count - 1); };  // inputSlot.remove(1)
    return d;
}());

// --- loom -----------------------------------------------------------------------------------------------------

const char* kDyes[16] = {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                         "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"};
const u32 kDyeColors[16] = {0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
                            0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21};
int dyeIndex(const std::string& prefixed) {  // "red_dye" / "red_banner" -> 14
    for (int i = 15; i >= 0; --i)  // light_gray before gray, light_blue before blue
        if (prefixed.starts_with(std::string(kDyes[i]) + "_")) {
            if ((i == 7 || i == 11) && (prefixed.starts_with("light_"))) continue;
            return i;
        }
    return -1;
}

struct Patterns {
    bool loaded = false;
    struct Pattern {
        std::string name, asset, translation;
    };
    std::vector<Pattern> all;  // network ids: the registry sorted by name
    std::unordered_map<std::string, int> byName;
    std::unordered_map<std::string, std::vector<int>> tags;
} patterns;

void loadTag(UiState& s, const std::string& tag, std::vector<int>& out, int depth) {
    if (depth > 4) return;
    const nlohmann::json j = s.res.json("data/minecraft/tags/banner_pattern/" + tag + ".json");
    if (!j.is_object() || !j.contains("values")) return;
    for (const auto& v : j["values"]) {
        std::string n = v.is_string() ? v.get<std::string>() : v.is_object() ? v.value("id", "") : "";
        if (n.empty()) continue;
        if (n[0] == '#') {
            n = n.substr(1);
            loadTag(s, n.substr(n.find(':') + 1), out, depth + 1);
            continue;
        }
        auto it = patterns.byName.find(n.substr(n.find(':') + 1));
        if (it != patterns.byName.end()) out.push_back(it->second);
    }
}

void loadPatterns(UiState& s) {
    if (patterns.loaded) return;
    patterns.loaded = true;
    std::vector<std::string> names;
    for (const std::string& p : s.res.list("data/minecraft/banner_pattern/"))
        if (p.ends_with(".json")) names.push_back(p.substr(p.rfind('/') + 1, p.size() - p.rfind('/') - 6));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string& n : names) {
        const nlohmann::json j = s.res.json("data/minecraft/banner_pattern/" + n + ".json");
        Patterns::Pattern p;
        p.name = n;
        std::string asset = j.is_object() ? j.value("asset_id", "minecraft:" + n) : "minecraft:" + n;
        p.asset = asset.substr(asset.find(':') + 1);
        p.translation = j.is_object() ? j.value("translation_key", "block.minecraft.banner." + n) : "block.minecraft.banner." + n;
        patterns.byName[n] = int(patterns.all.size());
        patterns.all.push_back(p);
    }
}

const std::vector<int>& patternTag(UiState& s, const std::string& tag) {
    auto it = patterns.tags.find(tag);
    if (it != patterns.tags.end()) return it->second;
    std::vector<int>& v = patterns.tags[tag];
    loadTag(s, tag, v, 0);
    return v;
}

// LoomMenu.getSelectablePatterns: no pattern item: #no_item_required; else the item's provides_banner_patterns tag
// (the vanilla pattern items: <name>_banner_pattern provides #pattern_item/<name>).
// With the server's registry and tags (registry data, update_tags), pattern ids are its network ids; without them the
// jar's data (names sorted) stands in.
std::vector<int> selectablePatterns(UiState& s, const Menu& m) {
    loadPatterns(s);
    if (!s.items.registry("minecraft:banner_pattern").empty()) {
        if (m.slots[2].empty()) {
            const std::vector<int>* t = s.items.tag("minecraft:banner_pattern", "minecraft:no_item_required");
            return t ? *t : std::vector<int>{};
        }
        const std::string* data = component(s, m.slots[2], "provides_banner_patterns");  // a holder set
        if (!data) return {};
        net::Reader r(reinterpret_cast<const u8*>(data->data()), data->size());
        const i32 c = r.varInt();
        if (c == 0) {
            const std::vector<int>* t = s.items.tag("minecraft:banner_pattern", r.str());
            return t ? *t : std::vector<int>{};
        }
        std::vector<int> ids;
        for (i32 i = 0; i < c - 1 && r.ok; ++i) ids.push_back(r.varInt());
        return ids;
    }
    const std::string p = itemName(s, m.slots[2]);
    if (p.empty()) return patternTag(s, "no_item_required");
    if (p.ends_with("_banner_pattern")) return patternTag(s, "pattern_item/" + p.substr(0, p.size() - 15));
    return {};
}
// A pattern's texture name and translation key by id.
std::string patternAsset(UiState& s, int id) {
    const auto& reg = s.items.registry("minecraft:banner_pattern");
    if (!reg.empty()) {
        if (id < 0 || size_t(id) >= reg.size()) return {};
        const std::string& a = reg[size_t(id)].assetId;
        return a.substr(a.find(':') + 1);
    }
    return id >= 0 && size_t(id) < patterns.all.size() ? patterns.all[size_t(id)].asset : std::string();
}
std::string patternKey(UiState& s, int id) {
    const auto& reg = s.items.registry("minecraft:banner_pattern");
    if (!reg.empty()) return id >= 0 && size_t(id) < reg.size() ? reg[size_t(id)].translationKey : std::string();
    return id >= 0 && size_t(id) < patterns.all.size() ? patterns.all[size_t(id)].translation : std::string();
}

// BannerPatternLayers of a stack (banner_patterns component): (pattern id, dye) pairs.
std::vector<std::pair<int, int>> bannerLayers(UiState& s, const ItemStack& st) {
    std::vector<std::pair<int, int>> out;
    const std::string* data = component(s, st, "banner_patterns");
    if (!data) return out;
    net::Reader r(reinterpret_cast<const u8*>(data->data()), data->size());
    const i32 n = r.varInt();
    for (i32 i = 0; i < n && r.ok; ++i) {
        const int holder = r.varInt();  // registry id + 1 (0: inline, not sent by vanilla)
        const int dye = r.varInt();
        if (r.ok) out.push_back({holder - 1, dye});
    }
    return out;
}

struct LoomState {
    float scrollOffs = 0;
    int startRow = 0;
    bool scrolling = false;
    bool displayPatterns = false;
    std::string lastKey;
} loom;

void loomLayout(const Menu&, Layout& l) {
    l.background = "gui/container/loom";
    l.slots = {{13, 26}, {33, 26}, {23, 45}, {143, 57}};
    playerSlots(l, 4);
    l.titleY = 4;
}
void loomOpened(UiState&, Menu&) { loom = LoomState{}; }

bool loomHasMax(UiState& s, const Menu& m) { return bannerLayers(s, m.slots[0]).size() >= 6; }

void loomSync(UiState& s, const Menu& m) {  // containerChanged
    std::string key;
    for (int i = 0; i < 3; ++i) key += std::to_string(m.slots[size_t(i)].empty() ? -1 : m.slots[size_t(i)].item) + ",";
    key += std::to_string(bannerLayers(s, m.slots[0]).size());
    const std::vector<int> sel = selectablePatterns(s, m);
    const int rows = (int(sel.size()) + 3) / 4;
    if (key != loom.lastKey) {
        loom.lastKey = key;
        loom.displayPatterns = !m.slots[0].empty() && !m.slots[1].empty() && !loomHasMax(s, m) && !sel.empty();
    }
    if (loom.startRow >= rows) loom.startRow = 0, loom.scrollOffs = 0;
}

// The banner's flag face (the 20 x 40 front of the flag cube at (1, 1) of the 64 x 64 banner textures).
void drawBannerFace(UiState& s, Canvas& c, float x, float y, float w, float h, int baseColor, const std::vector<std::pair<int, int>>& layers) {
    auto layer = [&](const std::string& tex, int dye) {
        const Sprite* t = s.sprites.texture("entity/banner/" + tex);
        if (!t) return;
        const float du = t->u1 - t->u0, dv = t->v1 - t->v0;
        c.quad(x, y, x + w, y + h, t->u0 + du * 1.0f / 64.0f, t->v0 + dv * 1.0f / 64.0f, t->u0 + du * 21.0f / 64.0f,
               t->v0 + dv * 41.0f / 64.0f, 0xFF000000 | kDyeColors[std::clamp(dye, 0, 15)], ModeGui);
    };
    layer("base", baseColor);
    for (const auto& [id, dye] : layers)
        if (const std::string a = patternAsset(s, id); !a.empty()) layer(a, dye);
}

void loomDrawBg(UiState& s, Canvas& c, Menu& m, const Layout& l, float left, float top) {
    loomSync(s, m);
    const std::vector<int> sel = selectablePatterns(s, m);
    static const char* kEmpty[3] = {"container/slot/banner", "container/slot/dye", "container/slot/banner_pattern"};
    for (int i = 0; i < 3; ++i)
        if (m.slots[size_t(i)].empty())
            blitSprite(c, sp(s, kEmpty[i]), left + float(l.slots[size_t(i)].x), top + float(l.slots[size_t(i)].y), 16, 16);
    const int offscreen = (int(sel.size()) + 3) / 4 - 4;
    const bool active = loom.displayPatterns && sel.size() > 16;
    if (loom.scrolling && !s.leftDown) loom.scrolling = false;
    if (loom.scrolling && loom.displayPatterns && offscreen > 0) {
        const float y0 = top + 13, y1 = y0 + 56;
        loom.scrollOffs = std::clamp((s.mouseY - y0 - 7.5f) / (y1 - y0 - 15.0f), 0.0f, 1.0f);
        loom.startRow = std::max(int(loom.scrollOffs * float(offscreen) + 0.5f), 0);
    }
    blitSprite(c, sp(s, active ? "container/loom/scroller" : "container/loom/scroller_disabled"), left + 119,
               top + 13 + float(int(41.0f * loom.scrollOffs)), 12, 15);
    if (inRect(s.mouseX, s.mouseY, left + 119, top + 13, 12, 56))
        s.requestCursor(!active ? UiState::CursorNotAllowed : loom.scrolling ? UiState::CursorResizeNS : UiState::CursorPointingHand);
    const ItemStack& result = m.slots[3];
    const bool hasMax = loomHasMax(s, m);
    if (!result.empty() && !hasMax) {
        const int base = dyeIndex(itemName(s, result));
        drawBannerFace(s, c, left + 141, top + 8, 20, 40, base < 0 ? 0 : base, bannerLayers(s, result));
    } else if (hasMax) {
        blitSprite(c, sp(s, "container/loom/error"), left + float(l.slots[3].x) - 5, top + float(l.slots[3].y) - 5, 26, 26);
    }
    if (!loom.displayPatterns) return;
    const int selected = m.data[0];
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            const int index = (row + loom.startRow) * 4 + col;
            if (index >= int(sel.size())) return;
            const float px = left + 60 + float(col * 14), py = top + 13 + float(row * 14);
            const bool hl = inRect(s.mouseX, s.mouseY, px, py, 14, 14);
            if (hl && index != selected) s.requestCursor(UiState::CursorPointingHand);
            blitSprite(c, sp(s, index == selected ? "container/loom/pattern_selected" : hl ? "container/loom/pattern_highlighted" : "container/loom/pattern"),
                       px, py, 14, 14);
            // extractBannerOnButton: gray 5 x 10 with the pattern mask (21 x 40 of 64 at v 1) over it.
            c.fill(px + 4, py + 2, px + 9, py + 12, 0xFF000000 | kDyeColors[7]);
            if (const Sprite* t = s.sprites.texture("entity/banner/" + patternAsset(s, sel[size_t(index)]))) {
                const float du = t->u1 - t->u0, dv = t->v1 - t->v0;
                c.quad(px + 4, py + 2, px + 9, py + 12, t->u0, t->v0 + dv / 64.0f, t->u0 + du * 21.0f / 64.0f,
                       t->v0 + dv / 64.0f + dv * 40.0f / 64.0f, 0xFFFFFFFF, ModeGui);
            }
        }
}

void loomDrawTop(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    if (!loom.displayPatterns) return;
    const std::vector<int> sel = selectablePatterns(s, m);
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            const int index = (row + loom.startRow) * 4 + col;
            if (index >= int(sel.size())) return;
            if (index == m.data[0] || !inRect(s.mouseX, s.mouseY, left + 60 + float(col * 14), top + 13 + float(row * 14), 14, 14))
                continue;
            const int dye = dyeIndex(itemName(s, m.slots[1]));
            const std::string key = patternKey(s, sel[size_t(index)]) + "." + kDyes[dye < 0 ? 0 : dye];
            s.drawTooltip(c, {Text(s.res.translate(key))}, s.mouseX, s.mouseY);
        }
}

bool loomInput(UiState& s, const InputEvent& e, Menu& m, const Layout&, float left, float top) {
    const std::vector<int> sel = selectablePatterns(s, m);
    const int offscreen = (int(sel.size()) + 3) / 4 - 4;
    if (e.type == InputEvent::Wheel) {
        if (loom.displayPatterns && offscreen > 0) {
            loom.scrollOffs = std::clamp(loom.scrollOffs - float(e.code) / float(offscreen), 0.0f, 1.0f);
            loom.startRow = std::max(int(loom.scrollOffs * float(offscreen) + 0.5f), 0);
        }
        return true;
    }
    if (e.type == InputEvent::Button && !e.down) loom.scrolling = false;  // mouseReleased: any button
    if (e.type != InputEvent::Button || !e.down || !loom.displayPatterns) return false;  // any button picks a pattern
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            if (!inRect(s.mouseX, s.mouseY, left + 60 + float(col * 14), top + 13 + float(row * 14), 14, 14)) continue;
            const int index = (row + loom.startRow) * 4 + col;
            if (index >= int(sel.size())) return false;  // LoomMenu.clickMenuButton: a valid pattern index
            m.data[0] = index;
            // LoomMenu.clickMenuButton runs here too: setupResultSlot, the banner (one) with the pattern added in
            // the dye's colour (registry ids known; layers of inline patterns are left to the server)
            std::vector<std::pair<int, int>> layers = bannerLayers(s, m.slots[0]);
            const int dye = dyeIndex(itemName(s, m.slots[1]));
            const bool inline_ = std::any_of(layers.begin(), layers.end(), [](const auto& l) { return l.first < 0; });
            if (!m.slots[0].empty() && dye >= 0 && !inline_ && !s.items.registry("minecraft:banner_pattern").empty()) {
                layers.push_back({sel[size_t(index)], dye});
                net::Writer w;  // BannerPatternLayers.STREAM_CODEC: (holder id + 1, dye) per layer
                w.varInt(i32(layers.size()));
                for (const auto& [id, d] : layers) w.varInt(id + 1), w.varInt(d);
                const std::string data(w.buf.begin(), w.buf.end());
                ItemStack result = m.slots[0];
                result.count = 1;
                s.items.setComponent(result, "banner_patterns", &data);
                m.slots[3] = result;
            }
            uiSound("minecraft:ui.loom.select_pattern");
            sendContainerButton(s, index);
            return true;
        }
    if (inRect(s.mouseX, s.mouseY, left + 119, top + 9, 12, 56)) {  // the scroller: any button
        loom.scrolling = true;
        return true;
    }
    return false;
}

// LoomMenu: banners (BannerItem), #loom_dyes with a dye component, #loom_patterns providing banner patterns.
bool loomBanner(UiState& s, const ItemStack& st) { return itemName(s, st).ends_with("_banner"); }
bool loomDye(UiState& s, const ItemStack& st) {
    return s.items.itemHasTag(st.item, "minecraft:loom_dyes") && s.items.component(st, "dye").present();
}
bool loomPattern(UiState& s, const ItemStack& st) {
    return s.items.itemHasTag(st.item, "minecraft:loom_patterns") && s.items.component(st, "provides_banner_patterns").present();
}
bool loomMayPlace(UiState& s, const Menu&, int slot, const ItemStack& st) {
    if (slot == 0) return loomBanner(s, st);
    if (slot == 1) return loomDye(s, st);
    if (slot == 2) return loomPattern(s, st);
    return true;
}

int loomQuickMove(UiState& s, const Menu&, int index, const ItemStack& st, const std::function<bool(int, int, bool)>& moveTo) {
    if (index == 3) return moveTo(4, 40, true) ? 1 : 0;
    if (index < 3) return moveTo(4, 40, false) ? 1 : 0;
    if (loomBanner(s, st)) return moveTo(0, 1, false) ? 1 : 0;
    if (loomDye(s, st)) return moveTo(1, 2, false) ? 1 : 0;
    if (loomPattern(s, st)) return moveTo(2, 3, false) ? 1 : 0;
    if (index < 31) return moveTo(31, 40, false) ? 1 : 0;
    return moveTo(4, 31, false) ? 1 : 0;
}

const StationRegistrar kLoom({"loom"}, [] {
    StationDef d;
    d.layout = loomLayout, d.opened = loomOpened, d.drawBg = loomDrawBg, d.drawTop = loomDrawTop, d.input = loomInput;
    d.resultSlot = 3, d.mayPlace = loomMayPlace, d.quickMove = loomQuickMove;
    d.partialResult = true;  // a SimpleContainer output
    d.onTake = [](UiState&, Menu& m) {  // the banner and the dye are used; without either no pattern stays selected
        for (int i = 0; i < 2; ++i) m.slots[size_t(i)] = m.slots[size_t(i)].withCount(m.slots[size_t(i)].count - 1);
        if (m.slots[0].empty() || m.slots[1].empty()) m.data[0] = -1;
    };
    return d;
}());

// --- cartography table ----------------------------------------------------------------------------------------

// MapColor ids 0..61 (MapColor.java) and the brightness modifiers LOW, NORMAL, HIGH, LOWEST.
const u32 kMapColors[] = {0,        8368696,  16247203, 13092807, 16711680, 10526975, 10987431, 31744,    16777215, 10791096,
                          9923917,  7368816,  4210943,  9402184,  16776437, 14188339, 11685080, 6724056,  15066419, 8375321,
                          15892389, 5000268,  10066329, 5013401,  8339378,  3361970,  6704179,  6717235,  10040115, 1644825,
                          16445005, 6085589,  4882687,  55610,    8476209,  7340544,  13742497, 10441252, 9787244,  7367818,
                          12223780, 6780213,  10505550, 3746083,  8874850,  5725276,  8014168,  4996700,  4993571,  5001770,
                          9321518,  2430480,  12398641, 9715553,  6035741,  1474182,  3837580,  5647422,  1356933,  6579300,
                          14200723, 8365974};
u32 mapColor(u8 packed) {  // MapColor.getColorFromPackedId -> RGBA bytes (little endian ABGR)
    const int id = packed >> 2;
    if (id == 0 || id >= int(std::size(kMapColors))) return 0;
    static const int kMod[4] = {180, 220, 255, 135};
    const u32 c = kMapColors[id];
    const int m = kMod[packed & 3];
    const u32 r = ((c >> 16) & 255) * u32(m) / 255, g = ((c >> 8) & 255) * u32(m) / 255, b = (c & 255) * u32(m) / 255;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

int mapIdOf(UiState& s, const ItemStack& st) {
    const std::string* d = component(s, st, "map_id");
    if (!d) return -1;
    net::Reader r(reinterpret_cast<const u8*>(d->data()), d->size());
    const int id = r.varInt();
    return r.ok ? id : -1;
}

// Map decoration sprites in the GUI atlas, by sprite path.
std::unordered_map<std::string, std::array<int, 4>> g_decorSprites;  // x, y, w, h (w < 0: missing)

// GuiGraphicsExtractor.map: the map's 128 x 128 colours scaled into place, then the decorations item frames show
// (sprites turned by their rotation, v flipped like the game's blit) and their names on a dark background.
void drawMap(UiState& s, Canvas& c, int id, float x, float y, float scale) {
    auto it = g_maps.find(id);
    if (it == g_maps.end()) return;
    MapImage& m = it->second;
    if (m.ax < 0 && !s.guiAtlas.alloc(128, 128, m.ax, m.ay)) return;
    if (m.dirty) {
        std::vector<u8> px(128 * 128 * 4);
        for (int i = 0; i < 128 * 128; ++i) {
            const u32 v = mapColor(m.colors[size_t(i)]);
            std::memcpy(&px[size_t(i) * 4], &v, 4);
        }
        s.guiAtlas.blit(m.ax, m.ay, 128, 128, px.data(), 128 * 4);
        m.dirty = false;
    }
    const float W = float(s.guiAtlas.width()), H = float(s.guiAtlas.height());
    c.quad(x, y, x + 128 * scale, y + 128 * scale, float(m.ax) / W, float(m.ay) / H, float(m.ax + 128) / W, float(m.ay + 128) / H,
           0xFFFFFFFF, ModeGui);
    c.push();
    c.translate(x, y);
    c.scaleBy(scale);
    for (const MapDecor& d : m.decorations) {
        bool onFrame = false;
        const char* sprite = mapDecorationSprite(d.type, &onFrame);
        if (!sprite || !onFrame) continue;
        auto [it, fresh] = g_decorSprites.try_emplace(sprite, std::array<int, 4>{0, 0, -1, 0});
        if (fresh) {
            Image img;
            int ax, ay;
            if (s.res.image(std::string("assets/minecraft/textures/") + sprite + ".png", img) && img.w > 0 &&
                s.guiAtlas.alloc(img.w, img.h, ax, ay)) {
                s.guiAtlas.blit(ax, ay, img.w, img.h, img.rgba.data(), img.w * 4);
                it->second = {ax, ay, img.w, img.h};
            }
        }
        const auto& a = it->second;
        c.push();
        c.translate(float(d.x) / 2.0f + 64.0f, float(d.y) / 2.0f + 64.0f);
        c.rotate(float(d.rot) * 22.5f * 3.14159265f / 180.0f);
        c.scaleBy(4.0f);
        c.translate(-0.125f, 0.125f);
        if (a[2] > 0)
            c.quad(-1, -1, 1, 1, float(a[0]) / W, float(a[1] + a[3]) / H, float(a[0] + a[2]) / W, float(a[1]) / H, 0xFFFFFFFF, ModeGui);
        c.pop();
        if (!d.name.empty()) {
            const Text name = parseComponent(d.name, s.lang());
            const float width = s.font.widthF(name);
            const float sc = std::clamp(25.0f / width, 0.0f, 6.0f / 9.0f);
            c.push();
            c.translate(float(d.x) / 2.0f + 64.0f - width * sc / 2.0f, float(d.y) / 2.0f + 64.0f + 4.0f);
            c.scaleBy(sc);
            c.fill(-1, -1, width, 9, 0x80000000);  // the text's background (Integer.MIN_VALUE)
            s.drawText(c, name, 0, 0, 0xFFFFFFFF, false);
            c.pop();
        }
    }
    c.pop();
}

void cartoLayout(const Menu&, Layout& l) {
    l.background = "gui/container/cartography_table";
    l.slots = {{15, 15}, {15, 52}, {145, 39}};
    playerSlots(l, 3);
    l.titleY = 4;
}

void cartoDrawBg(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    const std::string add = itemName(s, m.slots[1]);
    const bool dup = add == "map", scaling = add == "paper", locking = add == "glass_pane";
    const ItemStack& map = m.slots[0];
    const int id = mapIdOf(s, map);
    bool locked = false;
    const MapImage* data = nullptr;
    if (id >= 0) {
        auto it = g_maps.find(id);
        if (it != g_maps.end()) {
            data = &it->second;
            if (data->locked) {
                locked = true;
                if (scaling || locking) blitSprite(c, sp(s, "container/cartography_table/error"), left + 35, top + 31, 28, 21);
            }
            if (scaling && (itemName(s, map) != "filled_map" || data->scale >= 4)) {
                locked = true;
                blitSprite(c, sp(s, "container/cartography_table/error"), left + 35, top + 31, 28, 21);
            }
        }
    }
    auto mapAt = [&](float x, float y, float sc) {
        if (data) drawMap(s, c, id, x, y, sc);
    };
    if (scaling && !locked) {
        blitSprite(c, sp(s, "container/cartography_table/scaled_map"), left + 67, top + 13, 66, 66);
        mapAt(left + 85, top + 31, 0.226f);
    } else if (dup) {
        blitSprite(c, sp(s, "container/cartography_table/duplicated_map"), left + 67 + 16, top + 13, 50, 66);
        mapAt(left + 86, top + 16, 0.34f);
        blitSprite(c, sp(s, "container/cartography_table/duplicated_map"), left + 67, top + 13 + 16, 50, 66);
        mapAt(left + 70, top + 32, 0.34f);
    } else if (locking) {
        blitSprite(c, sp(s, "container/cartography_table/map"), left + 67, top + 13, 66, 66);
        mapAt(left + 71, top + 17, 0.45f);
        blitSprite(c, sp(s, "container/cartography_table/locked"), left + 118, top + 60, 10, 14);
    } else {
        blitSprite(c, sp(s, "container/cartography_table/map"), left + 67, top + 13, 66, 66);
        mapAt(left + 71, top + 17, 0.45f);
    }
}

bool cartoMayPlace(UiState& s, const Menu&, int slot, const ItemStack& st) {
    const std::string n = itemName(s, st);
    if (slot == 0) return s.items.component(st, "map_id").present();
    if (slot == 1) return n == "paper" || n == "map" || n == "glass_pane";
    return true;
}

int cartoQuickMove(UiState& s, const Menu&, int index, const ItemStack& st, const std::function<bool(int, int, bool)>& moveTo) {
    if (index == 2) return moveTo(3, 39, true) ? 1 : 0;
    if (index < 2) return moveTo(3, 39, false) ? 1 : 0;
    const std::string n = itemName(s, st);
    if (s.items.component(st, "map_id").present()) return moveTo(0, 1, false) ? 1 : 0;
    if (n == "paper" || n == "map" || n == "glass_pane") return moveTo(1, 2, false) ? 1 : 0;
    if (index < 30) return moveTo(30, 39, false) ? 1 : 0;
    return moveTo(3, 30, false) ? 1 : 0;
}

const StationRegistrar kCartography({"cartography_table"}, [] {
    StationDef d;
    d.layout = cartoLayout, d.drawBg = cartoDrawBg, d.resultSlot = 2, d.mayPlace = cartoMayPlace, d.quickMove = cartoQuickMove;
    d.pickAll = 0;
    d.onTake = [](UiState&, Menu& m) {  // slots 0 and 1 remove(1)
        for (int i = 0; i < 2; ++i) m.slots[size_t(i)] = m.slots[size_t(i)].withCount(m.slots[size_t(i)].count - 1);
    };
    return d;
}());

}  // namespace

bool mapInfo(int id, int& scale, bool& locked) {
    auto it = g_maps.find(id);
    if (it == g_maps.end()) return false;
    scale = it->second.scale, locked = it->second.locked;
    return true;
}

const std::vector<u8>* mapColors(int id, u32& version) {
    auto it = g_maps.find(id);
    if (it == g_maps.end()) return nullptr;
    version = it->second.version;
    return &it->second.colors;
}

u32 mapColorRgba(u8 packed) { return mapColor(packed); }

const std::vector<MapDecor>* mapDecorations(int id) {
    auto it = g_maps.find(id);
    return it == g_maps.end() ? nullptr : &it->second.decorations;
}

const char* mapDecorationSprite(int type, bool* onFrame) {
    // MapDecorationTypes in registration (network id) order: asset name, showOnItemFrame.
    static const std::pair<const char*, bool> kTypes[] = {
        {"player", false}, {"frame", true}, {"red_marker", false}, {"blue_marker", false}, {"target_x", true},
        {"target_point", true}, {"player_off_map", false}, {"player_off_limits", false}, {"woodland_mansion", true},
        {"ocean_monument", true}, {"white_banner", true}, {"orange_banner", true}, {"magenta_banner", true},
        {"light_blue_banner", true}, {"yellow_banner", true}, {"lime_banner", true}, {"pink_banner", true},
        {"gray_banner", true}, {"light_gray_banner", true}, {"cyan_banner", true}, {"purple_banner", true},
        {"blue_banner", true}, {"brown_banner", true}, {"green_banner", true}, {"red_banner", true}, {"black_banner", true},
        {"red_x", true}, {"desert_village", true}, {"plains_village", true}, {"savanna_village", true},
        {"snowy_village", true}, {"taiga_village", true}, {"jungle_temple", true}, {"swamp_hut", true},
        {"trial_chambers", true}, {"abandoned_camp", true}, {"ancient_city", true}, {"desert_pyramid", true},
        {"mineshaft", true}, {"warm_ocean_ruins", true}};
    static const auto kPaths = [] {
        std::vector<std::string> v;
        for (auto [name, frame] : kTypes) v.push_back(std::string("map/decorations/") + name);
        return v;
    }();
    if (type < 0 || size_t(type) >= std::size(kTypes)) return nullptr;
    if (onFrame) *onFrame = kTypes[size_t(type)].second;
    return kPaths[size_t(type)].c_str();
}

}  // namespace ui
