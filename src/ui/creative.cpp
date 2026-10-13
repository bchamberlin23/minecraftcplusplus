// Creative inventory (CreativeModeInventoryScreen): the vanilla category tabs (contents from CreativeModeTabs,
// generated into creative_tabs.inc), the hotbar tab (empty saved hotbars), search, and the survival inventory tab
// with armour / off-hand slots, the destroy-item slot and the player preview. Changes to the player's inventory go
// to the server slot by slot (set_creative_mode_slot), like the game's CreativeInventoryListener.
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "ui/entity_preview.h"
#include "ui/frontend.h"
#include "ui/item_ui.h"
#include "ui/ui_state.h"

#include "world/nbt_dom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace ui {

namespace {
enum class TabKind { Category, Hotbar, Search, Inventory };
struct TabDef {
    const char* title;  // translation key
    const char* icon;
    TabKind kind;
    int top;  // row: 1 top, 0 bottom
    int column;
    bool alignedRight, showTitle, scrollBar;
    const char* background;  // gui/container/creative_inventory/tab_<background>
    const char* const* items;
};
#include "ui/creative_tabs.inc"

constexpr int kTabCount = int(std::size(kTabDefs));
constexpr int kW = 195, kH = 136;
constexpr u32 kLabel = 0xFF404040;

std::string shortName(const std::string& n) { return n.substr(n.find(':') + 1); }

struct Creative {
    bool built = false;
    std::vector<ItemStack> tabItems[kTabCount];
    std::vector<ItemStack> searchItems[kTabCount];  // what each tab adds to search (TabVisibility.SEARCH_TAB_ONLY)
    std::unordered_map<int, std::vector<int>> tabsOfItem;  // item id -> category tabs that list it
    int selected = 0;           // like the game's static selectedTab: kept between openings
    std::vector<ItemStack> shown;  // ItemPickerMenu.items
    float scroll = 0;
    bool scrolling = false, opened = false, ignoreText = false;
    int hoveredGrid = -1, hoveredInv = -1;  // grid cell (0..44) / inventory menu slot
    bool hoveredDestroy = false;
};
Creative g;

// CreativeModeTab.shouldDisplay: the operator tab has items only with the Operator Items Tab option on and
// Player.canUseGameMasterBlocks (creative and permission level 2).
bool g_opTab = false;
bool tabVisible(int i) { return g_opTab || std::string(kTabDefs[i].title) != "itemGroup.op"; }

void buildTabs(UiState& s) {
    if (g.built) return;
    g.built = true;
    if (s.client && !s.items.registriesSet()) s.items.setRegistries(*s.client);
    auto stack = [&](const char* name) {
        ItemStack st;
        st.item = s.items.id(name), st.count = 1;
        return st;
    };
    auto component = [&](ItemStack& st, const char* type, const std::string& bytes) {
        const int t = s.items.componentType(type);
        if (t >= 0) st.added.push_back({t, bytes});
    };
    int potions = 0;
    if (const nlohmann::json& reg = s.reports->registries(); reg.contains("minecraft:potion"))
        potions = int(reg["minecraft:potion"]["entries"].size());
    auto bytes = [](const net::Writer& w) { return std::string(w.buf.begin(), w.buf.end()); };
    auto indexOf = [](const std::vector<Items::RegistryInfo>& v, const std::string& id) {
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].id == id) return int(i);
        return -1;
    };
    auto enchantedBook = [&](int id, int level) {  // EnchantmentHelper.createBook
        ItemStack st = stack("enchanted_book");
        net::Writer w;
        w.varInt(1), w.varInt(id), w.varInt(level);
        component(st, "stored_enchantments", bytes(w));
        st.storedEnchants.emplace_back(id, level);
        return st;
    };
    for (int t = 0; t < kTabCount; ++t) {
        std::vector<ItemStack>& out = g.tabItems[t];
        std::vector<ItemStack>& searchOut = g.searchItems[t];
        for (const char* const* p = kTabDefs[t].items; *p; ++p) {
            const std::string e = *p;
            const size_t before = out.size();
            if (e.starts_with("@paintings:")) {
                // generatePresetPaintings: the (non-)placeable variants by area, then width (stable over registry order).
                const auto& all = s.items.paintings();
                std::vector<int> ids;
                for (size_t i = 0; i < all.size(); ++i) {
                    bool placeable = false;
                    for (const char* const* q = kPlaceablePaintings; *q; ++q) placeable |= all[i].id == *q;
                    if (placeable == (e == "@paintings:placeable")) ids.push_back(int(i));
                }
                std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) {
                    const int aa = all[size_t(a)].width * all[size_t(a)].height, ab = all[size_t(b)].width * all[size_t(b)].height;
                    return aa != ab ? aa < ab : all[size_t(a)].width < all[size_t(b)].width;
                });
                for (int id : ids) {
                    ItemStack st = stack("painting");
                    net::Writer w;
                    w.varInt(id + 1);  // Holder: registry id + 1
                    component(st, "painting/variant", bytes(w));
                    st.painting = id;
                    out.push_back(st);
                }
            } else if (e == "@goat_horns") {  // generateInstrumentTypes(GOAT_HORN, InstrumentTags.GOAT_HORNS)
                for (const char* const* q = kGoatHorns; *q; ++q) {
                    const int id = indexOf(s.items.instruments(), *q);
                    if (id < 0) continue;
                    ItemStack st = stack("goat_horn");
                    net::Writer w;
                    w.varInt(id + 1);
                    component(st, "instrument", bytes(w));
                    st.instrument = id;
                    out.push_back(st);
                }
            } else if (e == "@suspicious_stews") {  // generateSuspiciousStews: one per distinct flower effect
                for (const StewEffect& se : kSuspiciousStews) {
                    const int effect = s.items.mobEffectId(se.effect);
                    if (effect < 0) continue;
                    ItemStack st = stack("suspicious_stew");
                    net::Writer w;
                    w.varInt(1), w.varInt(effect), w.varInt(se.ticks);
                    component(st, "suspicious_stew_effects", bytes(w));
                    st.stewEffects.emplace_back(effect, se.ticks);
                    out.push_back(st);
                }
            } else if (e == "@enchanted_books") {
                // OnlyMaxLevel in the tab (PARENT_TAB_ONLY), AllLevels in search (SEARCH_TAB_ONLY).
                const auto& all = s.items.enchantments();
                for (size_t i = 0; i < all.size(); ++i) out.push_back(enchantedBook(int(i), all[i].maxLevel));
                for (size_t i = 0; i < all.size(); ++i)
                    for (int level = 1; level <= all[i].maxLevel; ++level) searchOut.push_back(enchantedBook(int(i), level));
                continue;
            } else if (e == "@ominous_banner") {  // Raid.getOminousBannerInstance
                ItemStack st = stack("white_banner");
                static const std::pair<const char*, int> kLayers[] = {
                    {"minecraft:rhombus", 9}, {"minecraft:stripe_bottom", 8}, {"minecraft:stripe_center", 7},
                    {"minecraft:border", 8},  {"minecraft:stripe_middle", 15}, {"minecraft:half_horizontal", 8},
                    {"minecraft:circle", 8},  {"minecraft:border", 15}};  // DyeColor ids
                const auto& patterns = s.items.bannerPatterns();
                net::Writer w;
                int n = 0;
                for (auto [id, dye] : kLayers) n += std::find(patterns.begin(), patterns.end(), id) != patterns.end();
                w.varInt(n);
                for (auto [id, dye] : kLayers)
                    if (auto it = std::find(patterns.begin(), patterns.end(), id); it != patterns.end())
                        w.varInt(int(it - patterns.begin()) + 1), w.varInt(dye);
                component(st, "banner_patterns", bytes(w));
                const int patternsType = s.items.componentType("banner_patterns");
                net::Writer td;
                td.boolean(false), td.varInt(1), td.varInt(patternsType);
                component(st, "tooltip_display", bytes(td));
                st.hiddenTooltip.push_back(patternsType);
                // item_name: {translate: "block.minecraft.ominous_banner"} as network NBT
                const std::string key = "block.minecraft.ominous_banner";
                net::Writer nm;
                nm.u8v(10), nm.u8v(8), nm.u16v(9), nm.bytes("translate", 9), nm.u16v(u16(key.size())), nm.bytes(key.data(), key.size());
                nm.u8v(0);
                component(st, "item_name", bytes(nm));
                st.hasItemName = true, st.itemName = translatable(key, {}, s.lang());
                net::Writer rr;
                rr.varInt(1);  // Rarity.UNCOMMON
                component(st, "rarity", bytes(rr));
                st.rarity = 1;
                out.push_back(st);
            } else if (e.starts_with("@potion:")) {  // PotionContents(potion): optional potion, no colour, no effects, no name
                for (int id = 0; id < potions; ++id) {
                    ItemStack st = stack(e.c_str() + 8);
                    net::Writer w;
                    w.boolean(true), w.varInt(id), w.boolean(false), w.varInt(0), w.boolean(false);
                    component(st, "potion_contents", std::string(w.buf.begin(), w.buf.end()));
                    st.potion = id;
                    if (st.item >= 0) out.push_back(st);
                }
            } else if (e == "@fireworks") {  // FireworkRocketItem.CRAFTABLE_DURATIONS
                for (int d = 1; d <= 3; ++d) {
                    ItemStack st = stack("firework_rocket");
                    component(st, "fireworks", std::string{char(d), char(0)});
                    out.push_back(st);
                }
            } else if (e == "@test_blocks" || e == "@lights") {
                // TestBlock.setModeOnStack (TestBlockMode values) / LightBlock.setLightOnStack (15 down to 0): block_state
                auto blockState = [&](const char* item, const char* key, const std::string& value) {
                    ItemStack st = stack(item);
                    net::Writer w;
                    w.varInt(1), w.str(key), w.str(value);
                    component(st, "block_state", bytes(w));
                    if (st.item >= 0) out.push_back(st);
                };
                if (e == "@test_blocks")
                    for (const char* mode : {"start", "log", "fail", "accept"}) blockState("test_block", "mode", mode);
                else
                    for (int level = 15; level >= 0; --level) blockState("light", "level", std::to_string(level));
            } else if (e == "@ominous_bottles") {
                for (int a = 0; a <= 4; ++a) {
                    ItemStack st = stack("ominous_bottle");
                    component(st, "ominous_bottle_amplifier", std::string{char(a)});
                    out.push_back(st);
                }
            } else {
                ItemStack st = stack(e.c_str());
                if (st.item >= 0) out.push_back(st);
            }
            // CreativeModeTab.ItemDisplayBuilder.accept: items not enabled by the world's feature flags are left out
            out.erase(std::remove_if(out.begin() + ptrdiff_t(before), out.end(), [&](const ItemStack& st) { return !s.items.itemEnabled(st.item); }),
                      out.end());
            searchOut.insert(searchOut.end(), out.begin() + ptrdiff_t(before), out.end());
        }
        if (kTabDefs[t].kind == TabKind::Category && tabVisible(t))
            for (const ItemStack& st : out) {
                auto& v = g.tabsOfItem[st.item];
                if (v.empty() || v.back() != t) v.push_back(t);
            }
    }
    // Search: every category tab's items in tab order, without duplicates (type and components).
    for (int t = 0; t < kTabCount; ++t) {
        if (kTabDefs[t].kind != TabKind::Category || !tabVisible(t)) continue;
        for (const ItemStack& st : g.searchItems[t]) {
            bool dup = false;
            for (const ItemStack& o : g.tabItems[6])
                if (o.item == st.item && o.sameItemSameComponents(st)) dup = true;
            if (!dup) g.tabItems[6].push_back(st);
        }
    }
}

int searchTab() {
    for (int i = 0; i < kTabCount; ++i)
        if (kTabDefs[i].kind == TabKind::Search) return i;
    return 0;
}
const TabDef& tab() { return kTabDefs[g.selected]; }
int rowsBeyond() { return (int(g.shown.size()) + 8) / 9 - 5; }  // ItemPickerMenu.calculateRowCount
bool canScroll() { return tab().scrollBar && g.shown.size() > 45; }
int firstRow() { return std::max(int(g.scroll * float(rowsBeyond()) + 0.5f), 0); }

std::vector<std::string> g_visibleTags;  // the '#' search's matching item tags (search tab tooltips list them)

void refreshSearch(UiState& s) {
    g.shown.clear();
    g_visibleTags.clear();
    std::string q;
    for (char ch : s.search.value) q += char(std::tolower(u8(ch)));
    auto lower = [](std::string v) {
        for (char& ch : v) ch = char(std::tolower(u8(ch)));
        return v;
    };
    if (q.starts_with("#")) {  // CreativeModeInventoryScreen.updateVisibleTags: tags by namespace / path
        const std::string query = q.substr(1);
        const size_t colon = query.find(':');
        if (const auto* tags = s.items.tags("minecraft:item"))
            for (const auto& [id, members] : *tags) {
                const size_t c = id.find(':');
                const std::string ns = id.substr(0, c), path = id.substr(c + 1);
                const bool match = colon == std::string::npos ? path.find(query) != std::string::npos
                                                              : ns.find(query.substr(0, colon)) != std::string::npos &&
                                                                    path.find(query.substr(colon + 1)) != std::string::npos;
                if (match) g_visibleTags.push_back(id);
            }
        std::sort(g_visibleTags.begin(), g_visibleTags.end());
        for (const ItemStack& st : g.tabItems[g.selected])
            for (const std::string& t : g_visibleTags)
                if (s.items.itemHasTag(st.item, t)) {
                    g.shown.push_back(st);
                    break;
                }
        g.scroll = 0;
        return;
    }
    // SessionSearchTrees: the item's tooltip lines and its full id (lower case, built once per tab contents)
    static std::vector<std::string> text;
    static const std::vector<ItemStack>* textOf = nullptr;
    static size_t textCount = 0;
    const std::vector<ItemStack>& items = g.tabItems[g.selected];
    if (textOf != &items || textCount != items.size()) {
        text.clear();
        for (const ItemStack& st : items) {
            std::string t = lower(s.items.name(st.item));
            for (const Text& line : s.items.tooltip(st, false, true)) t += '|' + lower(line.plain());
            text.push_back(std::move(t));
        }
        textOf = &items, textCount = items.size();
    }
    for (size_t i = 0; i < items.size(); ++i)
        if (q.empty() || text[i].find(q) != std::string::npos) g.shown.push_back(items[i]);
    g.scroll = 0;
}

// HotbarManager: the nine saved hotbars in hotbar.nbt next to options.txt (the game's file in its own directory). Its
// layout: DataVersion, "0".."8" each a list of nine stacks, {} empty; a stack keeps its id and count and, instead of
// the game's codec form of its components, the network encoding (mcpp_stack) the engine holds stacks in.
struct Hotbars {
    bool loaded = false;
    std::array<std::array<ItemStack, 9>, 9> rows{};
} hotbars;

std::filesystem::path hotbarPath() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return std::filesystem::path(local ? local : L".") / L"minecraftCplusplus" / L"hotbar.nbt";
}

Hotbars& loadedHotbars(UiState& s) {
    if (hotbars.loaded) return hotbars;
    hotbars.loaded = true;
    std::ifstream in(hotbarPath(), std::ios::binary);
    const std::vector<u8> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    NbtValue root;
    if (data.empty() || !nbtRead(data.data(), data.size(), root) || root.type != TagCompound) return hotbars;
    for (int h = 0; h < 9; ++h) {
        const NbtValue* row = root.find(std::to_string(h));
        if (!row || row->type != TagList) continue;
        for (size_t i = 0; i < 9 && i < row->items.size(); ++i) {
            NbtValue item = row->items[i];
            const NbtValue* id = item.find("id");
            const NbtValue* count = item.find("count");
            if (!id || id->type != TagString || s.items.id(id->s) < 0) continue;
            ItemStack st;
            if (NbtValue* enc = item.find("mcpp_stack")) {
                net::Reader r(enc->bytes.data(), enc->bytes.size());
                if (!s.items.read(r, st) || !r.ok) st = ItemStack{};
            }
            if (st.empty()) st = ItemStack{s.items.id(id->s), 1};
            st.item = s.items.id(id->s);  // by name: the network ids may have moved
            if (count) st.count = std::max(1, int(count->i));
            hotbars.rows[size_t(h)][i] = st;
        }
    }
    return hotbars;
}

void saveHotbars(UiState& s) {
    NbtValue root = NbtValue::make(TagCompound);
    NbtValue version = NbtValue::make(TagInt);
    version.i = 4649;
    root.set("DataVersion", version);
    for (int h = 0; h < 9; ++h) {
        NbtValue row = NbtValue::make(TagList);
        row.listType = TagCompound;
        for (const ItemStack& st : hotbars.rows[size_t(h)]) {
            NbtValue item = NbtValue::make(TagCompound);
            if (!st.empty()) {
                NbtValue id = NbtValue::make(TagString), count = NbtValue::make(TagInt), enc = NbtValue::make(TagByteArray);
                id.s = s.items.name(st.item), count.i = st.count;
                net::Writer w;
                s.items.write(w, st, false);
                enc.bytes = w.buf;
                item.set("id", id), item.set("count", count), item.set("mcpp_stack", enc);
            }
            row.items.push_back(std::move(item));
        }
        root.set(std::to_string(h), std::move(row));
    }
    std::vector<u8> out;
    nbtWrite(root, out);
    std::error_code ec;
    std::filesystem::create_directories(hotbarPath().parent_path(), ec);
    std::ofstream(hotbarPath(), std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}

bool rowEmpty(const std::array<ItemStack, 9>& row) {
    return std::all_of(row.begin(), row.end(), [](const ItemStack& st) { return st.empty(); });
}

// Minecraft.handleKeybinds -> CreativeModeInventoryScreen.handleHotbarLoadOrSave: a hotbar key with the save / load
// activator held, in creative without a screen.
bool hotbarLoadOrSave(UiState& s, int index) {
    if (!s.creative() || s.screen != Screen::None || !s.window) return false;
    const int saveKey = s.settings.key(Key::SaveToolbarActivator), loadKey = s.settings.key(Key::LoadToolbarActivator);
    const bool load = loadKey && s.window->keyDown(loadKey), save = saveKey && s.window->keyDown(saveKey);
    if (!load && !save) return false;
    Hotbars& hb = loadedHotbars(s);
    if (load) {
        for (int i = 0; i < 9; ++i) s.creativeSetSlot(36 + i, hb.rows[size_t(index)][size_t(i)]);
    } else {
        for (int i = 0; i < 9; ++i) hb.rows[size_t(index)][size_t(i)] = s.inventory.slots[size_t(36 + i)];
        s.actionbar = translatable("inventory.hotbarSaved", {s.keyLabel(loadKey), s.keyLabel(s.settings.key(Key(int(Key::Hotbar1) + index)))}, s.lang());
        s.actionbarTime = 60, s.actionbarAnimated = false;
        saveHotbars(s);
    }
    return true;
}

void selectTab(UiState& s, int t) {
    const int old = g.selected;
    g.selected = t;
    g.shown.clear();
    if (tab().kind == TabKind::Hotbar) {
        // The saved hotbars, a row each; an empty one shows a paper on its own column telling how to save it.
        const Hotbars& hb = loadedHotbars(s);
        for (int h = 0; h < 9; ++h)
            for (int i = 0; i < 9; ++i) {
                if (!rowEmpty(hb.rows[size_t(h)])) {
                    g.shown.push_back(hb.rows[size_t(h)][size_t(i)]);
                    continue;
                }
                ItemStack st;
                if (i == h) {
                    st.item = s.items.id("paper"), st.count = 1;
                    st.hasItemName = true;
                    st.itemName = translatable("inventory.hotbarInfo",
                                               {s.keyLabel(s.settings.key(Key::SaveToolbarActivator)), s.keyLabel(s.settings.key(Key(int(Key::Hotbar1) + h)))}, s.lang());
                    st.maxStack = 0;  // creative_slot_lock: cannot be taken
                }
                g.shown.push_back(st);
            }
    } else if (tab().kind == TabKind::Category) {
        g.shown = g.tabItems[t];
    }
    if (tab().kind == TabKind::Search) {
        if (old != t) s.search = UiState::TextField{}, s.search.maxLength = 50;
        refreshSearch(s);
    } else {
        s.search = UiState::TextField{};
    }
    g.scroll = 0;
}

// Grid cell / inventory slot positions of the selected tab (relative to the panel).
struct VSlot {
    int grid = -1, inv = -1;  // creative cell 0..44 or inventory menu slot
    bool destroy = false;
    int x, y;
};
std::vector<VSlot> slotsOf() {
    std::vector<VSlot> v;
    if (tab().kind == TabKind::Inventory) {
        for (int ix = 5; ix < 46; ++ix) {
            VSlot sl;
            sl.inv = ix;
            if (ix < 9) {
                const int pos = ix - 5;
                sl.x = 54 + pos / 2 * 54, sl.y = 6 + pos % 2 * 27;
            } else if (ix == 45) {
                sl.x = 35, sl.y = 20;
            } else {
                const int pos = ix - 9;
                sl.x = 9 + pos % 9 * 18, sl.y = ix >= 36 ? 112 : 54 + pos / 9 * 18;
            }
            v.push_back(sl);
        }
        VSlot d;
        d.destroy = true, d.x = 173, d.y = 112;
        v.push_back(d);
        return v;
    }
    for (int i = 0; i < 45; ++i) {
        VSlot sl;
        sl.grid = i, sl.x = 9 + i % 9 * 18, sl.y = 18 + i / 9 * 18;
        v.push_back(sl);
    }
    for (int i = 0; i < 9; ++i) {
        VSlot sl;
        sl.inv = 36 + i, sl.x = 9 + i * 18, sl.y = 112;
        v.push_back(sl);
    }
    return v;
}

ItemStack gridItem(int cell) {
    const size_t idx = size_t(cell + firstRow() * 9);
    return idx < g.shown.size() ? g.shown[idx] : ItemStack{};
}

int tabX(int t) {
    const TabDef& d = kTabDefs[t];
    return d.alignedRight ? kW - 27 * (7 - d.column) + 1 : 27 * d.column;
}
int tabY(int t) { return kTabDefs[t].top ? -32 : kH; }
bool overTab(int t, float mx, float my) {  // checkTabClicked (relative to the panel)
    const int x = tabX(t), y = tabY(t);
    return mx >= float(x) && mx <= float(x + 26) && my >= float(y) && my <= float(y + 32);
}

void drawTab(UiState& s, Canvas& c, int t, float left, float top) {
    const TabDef& d = kTabDefs[t];
    const bool sel = t == g.selected;
    const float x = left + float(tabX(t)), y = top - (d.top ? 28.0f : -float(kH - 4));
    const std::string sp = std::string("container/creative_inventory/tab_") + (d.top ? "top_" : "bottom_") +
                           (sel ? "selected_" : "unselected_") + std::to_string(std::clamp(d.column, 0, 6) + 1);
    blitSprite(c, s.sprites.sprite(sp), x, y, 26, 32);
    if (!sel && s.mouseX > x && s.mouseY > y && s.mouseX < x + 26 && s.mouseY < y + 32) s.requestCursor(UiState::CursorPointingHand);
    ItemStack icon;
    icon.item = s.items.id(d.icon), icon.count = 1;
    s.drawItem(c, icon, x + 13 - 8, y + 16 - 8 + (d.top ? 1 : -1), false);
}

// Inventory menu clicks without the container_click packet: the server is told each changed slot instead.
void localInventoryClick(UiState& s, int slot, int button, int input) {
    const std::vector<ItemStack> before = s.inventory.slots;
    net::Client* client = s.client;
    s.client = nullptr;  // containerClick simulates only (it sends nothing while offline); restored below
    s.containerClick(slot, button, input);
    s.client = client;
    for (size_t i = 0; i < s.inventory.slots.size(); ++i) {
        const ItemStack& a = before[i];
        const ItemStack& b = s.inventory.slots[i];
        if (a.empty() != b.empty() || (!a.empty() && (a.count != b.count || !a.sameItemSameComponents(b))))
            s.creativeSetSlot(int(i), b);
    }
}

void dropStack(UiState& s, const ItemStack& st) {  // handleCreativeModeItemDrop
    if (!s.online() || st.empty()) return;
    net::Writer w;
    w.u16v(u16(i16(-1)));
    s.items.write(w, st, true);
    s.client->sendPlay("set_creative_mode_slot", w);
}

std::vector<Text> tooltipFor(UiState& s, const ItemStack& st, bool creativeSlot) {
    std::vector<Text> lines = s.items.tooltip(st, s.settings.advancedItemTooltips, creativeSlot);
    if (tab().kind == TabKind::Search && creativeSlot && !lines.empty()) {  // the '#' search's matching tags, dark purple
        int at = 1;
        for (const std::string& t : g_visibleTags)
            if (s.items.itemHasTag(st.item, t)) lines.insert(lines.begin() + at++, Text("#" + t, colorStyle(0xAA00AA)));
    }
    if (tab().kind == TabKind::Category && creativeSlot) return lines;
    auto it = g.tabsOfItem.find(st.item);
    if (it != g.tabsOfItem.end() && !lines.empty()) {
        // CreativeModeTab.contains: the tabs whose search contents hold this exact stack (item and components);
        // stacks we cannot match exactly (component order from elsewhere) fall back to the tabs listing the item.
        std::vector<int> tabs;
        for (int t : it->second)
            for (const ItemStack& o : g.searchItems[t])
                if (o.sameItemSameComponents(st)) {
                    tabs.push_back(t);
                    break;
                }
        if (tabs.empty()) tabs = it->second;
        int at = 1;
        for (int t : tabs) lines.insert(lines.begin() + at++, Text(s.res.translate(kTabDefs[t].title), colorStyle(0x5555FF)));
    }
    return lines;
}

// --- ScreenDef ------------------------------------------------------------------------------------------------

void build(UiState& s) {
    // CreativeModeTabs.tryRebuildTabContents: the contents follow hasPermissions (the operator tab).
    const bool opTab = s.settings.operatorItemsTab && s.creative() && permissionLevel() >= 2;
    static std::vector<std::string> features;  // CreativeModeTabs.tryRebuildTabContents: the enabled features too
    if (opTab != g_opTab || features != s.items.enabledFeatures()) {
        g_opTab = opTab;
        features = s.items.enabledFeatures();
        g.built = false;
        for (int t = 0; t < kTabCount; ++t) g.tabItems[t].clear(), g.searchItems[t].clear();
        g.tabsOfItem.clear();
        buildTabs(s);
        if (g.opened) selectTab(s, tabVisible(g.selected) ? g.selected : 0);  // refreshCurrentTabContents
    }
    buildTabs(s);
    if (!g.opened) {  // init(): re-select the remembered tab
        g.opened = true;
        g.scrolling = false;
        const int keep = g.selected;
        g.selected = 0;
        selectTab(s, tabVisible(keep) ? keep : 0);
    }
}

void closed(UiState&) { g.opened = false; }

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    const float mx = s.mouseX, my = s.mouseY;
    c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);
    const float left = float((W - kW) / 2), top = float((H - kH) / 2);
    if (g.scrolling && s.leftDown) {
        const float y0 = top + 18, y1 = y0 + 112;
        g.scroll = std::clamp((my - y0 - 7.5f) / (y1 - y0 - 15.0f), 0.f, 1.f);
    } else if (!s.leftDown) {
        g.scrolling = false;
    }
    for (int t = 0; t < kTabCount; ++t)
        if (t != g.selected && tabVisible(t)) drawTab(s, c, t, left, top);
    blitTexture(c, s.sprites.texture(std::string("gui/container/creative_inventory/tab_") + tab().background), left, top, 0, 0,
                float(kW), float(kH), 256, 256);
    if (tab().kind == TabKind::Search) s.drawTextField(c, s.search, left + 82, top + 6, 80, 0xFFFFFFFF, true, true);
    if (tab().scrollBar) {
        blitSprite(c, s.sprites.sprite(canScroll() ? "container/creative_inventory/scroller" : "container/creative_inventory/scroller_disabled"),
                   left + 175, top + 18 + float(int(float(112 - 17) * g.scroll)), 12, 15);
        if (mx >= left + 175 && my >= top + 18 && mx < left + 189 && my < top + 130)  // insideScrollbar
            s.requestCursor(!canScroll() ? UiState::CursorNotAllowed : g.scrolling ? UiState::CursorResizeNS : UiState::CursorPointingHand);
    }
    drawTab(s, c, g.selected, left, top);
    if (tab().kind == TabKind::Inventory) {
        PreviewEntity pe;
        if (previewLocalPlayer(s, pe))
            drawEntityFollowsMouse(s, c, pe, int(left) + 73, int(top) + 6, int(left) + 105, int(top) + 49, 20, 0.0625f, mx, my);
    }
    // Slots.
    g.hoveredGrid = g.hoveredInv = -1;
    g.hoveredDestroy = false;
    const std::vector<VSlot> slots = slotsOf();
    const VSlot* hovered = nullptr;
    for (const VSlot& sl : slots)
        if (mx >= left + float(sl.x) - 1 && mx < left + float(sl.x) + 17 && my >= top + float(sl.y) - 1 && my < top + float(sl.y) + 17)
            hovered = &sl;
    if (hovered) {
        g.hoveredGrid = hovered->grid, g.hoveredInv = hovered->inv, g.hoveredDestroy = hovered->destroy;
        blitSprite(c, s.sprites.sprite("container/slot_highlight_back"), left + float(hovered->x) - 4, top + float(hovered->y) - 4, 24, 24);
    }
    static const char* kEmpty[4] = {"container/slot/helmet", "container/slot/chestplate", "container/slot/leggings", "container/slot/boots"};
    for (const VSlot& sl : slots) {
        const float x = left + float(sl.x), y = top + float(sl.y);
        if (sl.grid >= 0) {
            s.drawItem(c, gridItem(sl.grid), x, y);
        } else if (sl.inv >= 0) {
            const ItemStack& st = s.inventory.slots[size_t(sl.inv)];
            if (st.empty() && sl.inv >= 5 && sl.inv <= 8) blitSprite(c, s.sprites.sprite(kEmpty[sl.inv - 5]), x, y, 16, 16);
            if (st.empty() && sl.inv == 45) blitSprite(c, s.sprites.sprite("container/slot/shield"), x, y, 16, 16);
            s.drawItem(c, st, x, y);
        }
    }
    if (hovered)
        blitSprite(c, s.sprites.sprite("container/slot_highlight_front"), left + float(hovered->x) - 4, top + float(hovered->y) - 4, 24, 24);
    if (tab().showTitle) s.drawText(c, Text(s.res.translate(tab().title)), left + 8, top + 6, kLabel, false);
    s.drawEffectsInInventory(c, Screen::Creative, int(left), kW, int(top));  // CreativeModeInventoryScreen
    // Carried item, tooltips.
    if (!s.carried.empty()) {
        s.drawItem(c, s.carried, mx - 8, my - 8);
        return;
    }
    if (g.hoveredGrid >= 0) {
        const ItemStack st = gridItem(g.hoveredGrid);
        if (!st.empty()) drawItemTooltip(s, c, st, tooltipFor(s, st, true), mx, my);
    } else if (g.hoveredInv >= 0 && !s.inventory.slots[size_t(g.hoveredInv)].empty()) {
        drawItemTooltip(s, c, s.inventory.slots[size_t(g.hoveredInv)], tooltipFor(s, s.inventory.slots[size_t(g.hoveredInv)], false), mx, my);
    }
    for (int t = 0; t < kTabCount; ++t) {  // checkTabHovering
        if (!tabVisible(t)) continue;
        const float x = left + float(tabX(t)) + 3, y = top + float(tabY(t)) + 3;
        if (mx >= x - 1 && mx < x + 21 + 1 && my >= y - 1 && my < y + 27 + 1) {
            s.drawTooltip(c, {Text(s.res.translate(kTabDefs[t].title))}, mx, my);
            break;
        }
    }
    if (g.hoveredDestroy) s.drawTooltip(c, {Text(s.res.translate("inventory.binSlot"))}, mx, my);
}

void gridClick(UiState& s, int cell, int button, int input) {  // slotClicked for the CONTAINER slots
    enum { Pickup, QuickMove, Swap, Clone, Throw };
    const ItemStack clicked = gridItem(cell);
    if (!clicked.empty() && clicked.maxStack == 0 && tab().kind == TabKind::Hotbar) return;  // locked placeholder
    ItemStack& carried = s.carried;
    const bool quick = input == QuickMove;
    if (input == Swap) {
        if (!clicked.empty()) s.creativeSetSlot(36 + button, clicked.withCount(s.items.maxStack(clicked)));
        return;
    }
    if (input == Clone) {
        if (carried.empty() && !clicked.empty()) carried = clicked.withCount(s.items.maxStack(clicked));
        return;
    }
    if (input == Throw) {
        if (!clicked.empty()) dropStack(s, clicked.withCount(button == 0 ? 1 : s.items.maxStack(clicked)));
        return;
    }
    if (!carried.empty() && !clicked.empty() && carried.sameItemSameComponents(clicked)) {
        const int mx = s.items.maxStack(carried);
        if (button == 0) {
            if (quick) carried.count = mx;
            else if (carried.count < mx) ++carried.count;
        } else {
            carried = carried.withCount(carried.count - 1);
        }
    } else if (!clicked.empty() && carried.empty()) {
        carried = clicked.withCount(quick ? s.items.maxStack(clicked) : clicked.count);
    } else if (button == 0) {
        carried = ItemStack{};
    } else if (!carried.empty()) {
        carried = carried.withCount(carried.count - 1);
    }
}

void input(UiState& s, const InputEvent& e) {
    using E = InputEvent;
    enum { Pickup, QuickMove, Swap, Clone, Throw };
    const float left = float((s.guiW() - kW) / 2), top = float((s.guiH() - kH) / 2);
    const float mx = s.mouseX, my = s.mouseY;
    const bool shift = s.window->keyDown(VK_SHIFT);
    const bool searching = tab().kind == TabKind::Search;
    if (e.type == E::Char) {
        if (g.ignoreText || !searching) return;
        const std::string before = s.search.value;
        s.textFieldKey(s.search, e);
        if (s.search.value != before) refreshSearch(s);
        return;
    }
    if (e.type == E::Key) {
        if (!e.down) {
            g.ignoreText = false;
            return;
        }
        g.ignoreText = false;
        if (e.code == VK_ESCAPE) return s.closeScreen();
        int hotbar = -1;
        for (int i = 0; i < 9; ++i)
            if (s.isKey(Key(int(Key::Hotbar1) + i), e)) hotbar = i;
        // Hotbar keys over a slot (checkHotbarKeyPressed).
        auto hotbarKey = [&] {
            if (hotbar < 0 || !s.carried.empty()) return false;
            if (g.hoveredGrid >= 0) {
                if (gridItem(g.hoveredGrid).empty() && searching) return false;
                gridClick(s, g.hoveredGrid, hotbar, Swap);
                return true;
            }
            if (g.hoveredInv >= 0) {
                localInventoryClick(s, g.hoveredInv, hotbar, Swap);
                return true;
            }
            return false;
        };
        if (!searching) {
            if (s.isKey(Key::Chat, e)) {  // keyChat: jump to the search tab
                g.ignoreText = true;
                selectTab(s, searchTab());
                return;
            }
            if (hotbarKey()) return;
            if (s.isKey(Key::Inventory, e)) return s.closeScreen();
            if (s.isKey(Key::Drop, e) && s.carried.empty()) {
                const bool all = s.window->keyDown(VK_CONTROL);
                if (g.hoveredGrid >= 0) gridClick(s, g.hoveredGrid, all ? 1 : 0, Throw);
                else if (g.hoveredInv >= 0 && !s.inventory.slots[size_t(g.hoveredInv)].empty()) {
                    ItemStack& st = s.inventory.slots[size_t(g.hoveredInv)];
                    const ItemStack drop = st.withCount(all ? st.count : 1);
                    dropStack(s, drop);
                    s.creativeSetSlot(g.hoveredInv, st.withCount(st.count - drop.count));
                }
            }
            return;
        }
        if (hotbarKey()) {
            g.ignoreText = true;
            return;
        }
        const std::string before = s.search.value;
        s.textFieldKey(s.search, e);
        if (s.search.value != before) refreshSearch(s);
        return;
    }
    if (e.type == E::Wheel) {
        if (canScroll()) g.scroll = std::clamp(g.scroll - float(e.code) / float(rowsBeyond()), 0.f, 1.f);
        return;
    }
    if (e.type != E::Button) return;
    const float rx = mx - left, ry = my - top;
    if (!e.down) {
        if (e.code == 0) {
            g.scrolling = false;
            for (int t = 0; t < kTabCount; ++t)
                if (tabVisible(t) && overTab(t, rx, ry)) {
                    if (t != g.selected) audio::playUiClick();
                    selectTab(s, t);
                    return;
                }
        }
        return;
    }
    const int button = e.code;
    if (button == 0 && tab().kind == TabKind::Search && s.textFieldHit(s.search)) return s.textFieldMouseDown(s.search);  // EditBox.onClick
    if (button == 0) {
        for (int t = 0; t < kTabCount; ++t)
            if (tabVisible(t) && overTab(t, rx, ry)) return;  // selected on release
        if (tab().kind != TabKind::Inventory && rx >= 175 && rx < 189 && ry >= 18 && ry < 130) {
            g.scrolling = canScroll();
            if (g.scrolling) g.scroll = std::clamp((ry - 18 - 7.5f) / (112 - 15.0f), 0.f, 1.f);
            return;
        }
    }
    // The slot under the cursor now (the drawn frame may be older).
    const VSlot* hit = nullptr;
    const std::vector<VSlot> slots = slotsOf();
    for (const VSlot& sl : slots)
        if (rx >= float(sl.x) - 1 && rx < float(sl.x) + 17 && ry >= float(sl.y) - 1 && ry < float(sl.y) + 17) hit = &sl;
    const bool inside = rx >= 0 && ry >= 0 && rx < kW && ry < kH;
    const int input = button == 2 ? Clone : shift ? QuickMove : Pickup;
    if (button == 2 && !s.creative()) return;
    if (!hit) {
        if (!inside && !s.carried.empty()) {  // clicked outside: drop
            if (button == 0) dropStack(s, s.carried), s.carried = ItemStack{};
            else if (button == 1) dropStack(s, s.carried.withCount(1)), s.carried = s.carried.withCount(s.carried.count - 1);
        }
        return;
    }
    if (hit->destroy) {
        if (shift) {
            for (int i = 0; i < int(s.inventory.slots.size()); ++i)
                if (!s.inventory.slots[size_t(i)].empty()) s.creativeSetSlot(i, ItemStack{});
        } else {
            s.carried = ItemStack{};
        }
        return;
    }
    if (hit->grid >= 0) return gridClick(s, hit->grid, button, input);
    if (tab().kind != TabKind::Inventory && input == QuickMove) {  // ItemPickerMenu.quickMoveStack: hotbar slot cleared
        s.creativeSetSlot(hit->inv, ItemStack{});
        return;
    }
    localInventoryClick(s, hit->inv, button, input);
}

const ScreenRegistrar kScreen({Screen::Creative, build, draw, input, closed});

void hotbarsAttached(UiState& s) {
    if (s.game) s.game->hotbarKey = [&s](int index) { return hotbarLoadOrSave(s, index); };
}
const UiModuleRegistrar kHotbars({.name = "saved_hotbars", .attached = hotbarsAttached});
}  // namespace

}  // namespace ui
