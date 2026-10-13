// The recipe book (ClientRecipeBook, RecipeBookComponent and its parts): the server's known recipes (recipe_book_add /
// _remove / _settings), the book beside the inventory, crafting table and furnaces with its tabs, search, craftable
// filter, pages, the alternatives overlay, ghost recipes (place_ghost_recipe), placing recipes (place_recipe) and the
// settings / seen-recipe packets; the "New Recipes Unlocked!" toast (RecipeToast).
#include "audio/audio.h"
#include "ui/item_props.h"
#include "ui/item_ui.h"
#include "ui/toasts.h"

#include <cmath>
#include <map>
#include <set>

namespace ui {

namespace {

constexpr float kPi = 3.14159265f;

// --- recipe displays -------------------------------------------------------------------------------------------

struct SlotDisplay {  // SlotDisplay (registry ids: slot_display)
    enum Type { Empty, AnyFuel, WithAnyPotion, OnlyWithComponent, Item, ItemStackT, Tag, Dyed, SmithingTrim, WithRemainder, Composite };
    int type = Empty;
    ItemStack stack;            // item / item_stack
    std::string tag;            // tag: a tag, or
    std::vector<int> ids;       //      item ids
    int component = -1;         // only_with_component
    std::vector<SlotDisplay> children;
};
struct RecipeDisplay {  // RecipeDisplay (registry ids: recipe_display)
    enum Type { Shapeless, Shaped, Furnace, Stonecutter, Smithing };
    int type = -1;
    int width = 0, height = 0;
    std::vector<SlotDisplay> ingredients;  // crafting grid; furnace: ingredient, fuel
    SlotDisplay result, station;
};
struct Ingredient {  // Ingredient: a holder set of items
    std::string tag;
    std::vector<int> ids;
};
struct Entry {  // RecipeDisplayEntry
    int id = 0;
    RecipeDisplay display;
    int group = -1, category = 0;
    bool hasRequirements = false;
    std::vector<Ingredient> requirements;
};

bool readHolderSet(net::Reader& r, std::string& tag, std::vector<int>& ids) {
    const i32 c = r.varInt();
    if (c == 0) tag = std::string(r.str());
    for (i32 i = 0; i < c - 1 && r.ok; ++i) ids.push_back(r.varInt());
    return r.ok;
}

bool readSlot(const Items& items, net::Reader& r, SlotDisplay& out, int depth = 0) {
    if (depth > 16) return false;
    out.type = r.varInt();
    auto child = [&] {
        out.children.emplace_back();
        return readSlot(items, r, out.children.back(), depth + 1);
    };
    switch (out.type) {
        case SlotDisplay::Empty: case SlotDisplay::AnyFuel: return r.ok;
        case SlotDisplay::WithAnyPotion: return child();
        case SlotDisplay::OnlyWithComponent: return child() && (out.component = r.varInt(), r.ok);
        case SlotDisplay::Item: out.stack.item = r.varInt(), out.stack.count = 1; return r.ok;
        case SlotDisplay::ItemStackT: return items.readTemplate(r, out.stack);
        case SlotDisplay::Tag: return readHolderSet(r, out.tag, out.ids);
        case SlotDisplay::Dyed: case SlotDisplay::WithRemainder: return child() && child();
        case SlotDisplay::SmithingTrim:  // base, material, pattern (holder: id + 1, or inline asset, description, decal)
            if (!child() || !child()) return false;
            if (r.varInt() == 0) r.str(), r.nbt(), r.boolean();
            return r.ok;
        case SlotDisplay::Composite: {
            const i32 n = r.varInt();
            for (i32 i = 0; i < n && r.ok; ++i)
                if (!child()) return false;
            return r.ok;
        }
        default: return false;
    }
}

bool readDisplay(const Items& items, net::Reader& r, RecipeDisplay& d) {
    d.type = r.varInt();
    auto slot = [&](SlotDisplay& s) { return readSlot(items, r, s); };
    auto list = [&](std::vector<SlotDisplay>& v) {
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            v.emplace_back();
            if (!slot(v.back())) return false;
        }
        return r.ok;
    };
    switch (d.type) {
        case RecipeDisplay::Shapeless: return list(d.ingredients) && slot(d.result) && slot(d.station);
        case RecipeDisplay::Shaped:
            d.width = r.varInt(), d.height = r.varInt();
            return list(d.ingredients) && slot(d.result) && slot(d.station);
        case RecipeDisplay::Furnace:
            d.ingredients.resize(2);
            if (!slot(d.ingredients[0]) || !slot(d.ingredients[1]) || !slot(d.result) || !slot(d.station)) return false;
            r.varInt(), r.f32();  // duration, experience
            return r.ok;
        case RecipeDisplay::Stonecutter:
            d.ingredients.resize(1);
            return slot(d.ingredients[0]) && slot(d.result) && slot(d.station);
        case RecipeDisplay::Smithing:
            d.ingredients.resize(3);
            return slot(d.ingredients[0]) && slot(d.ingredients[1]) && slot(d.ingredients[2]) && slot(d.result) && slot(d.station);
        default: return false;
    }
}

bool readEntry(const Items& items, net::Reader& r, Entry& e) {
    e.id = r.varInt();
    if (!readDisplay(items, r, e.display)) return false;
    e.group = r.varInt() - 1;  // OPTIONAL_VAR_INT
    e.category = r.varInt();
    e.hasRequirements = r.boolean();
    if (e.hasRequirements) {
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            Ingredient in;
            if (!readHolderSet(r, in.tag, in.ids)) return false;
            e.requirements.push_back(std::move(in));
        }
    }
    return r.ok;
}

// SlotDisplay.resolveForStacks for the display (the recipe book's slot display context).
std::vector<int> g_fuels;  // items with the cooking_fuel component, in registry order
bool g_fuelsKnown = false;
std::vector<ItemStack> resolve(UiState& s, const SlotDisplay& d) {
    std::vector<ItemStack> out;
    auto stackOf = [](int item) {
        ItemStack st;
        st.item = item, st.count = 1;
        return st;
    };
    switch (d.type) {
        case SlotDisplay::AnyFuel:
            if (!g_fuelsKnown) {
                g_fuelsKnown = true;
                for (int i = 0; i < s.items.count(); ++i)
                    if (s.items.component(stackOf(i), "cooking_fuel").present()) g_fuels.push_back(i);
            }
            for (int i : g_fuels) out.push_back(stackOf(i));
            break;
        case SlotDisplay::WithAnyPotion: {  // the stacks with every potion
            const std::vector<ItemStack> base = d.children.empty() ? out : resolve(s, d.children[0]);
            const int type = s.items.componentType("potion_contents");
            for (int p = 0; !s.items.potionName(p).empty(); ++p)
                for (const ItemStack& b : base) {
                    ItemStack st = b;
                    net::Writer w;
                    w.boolean(true), w.varInt(p), w.boolean(false), w.varInt(0), w.boolean(false);
                    std::erase_if(st.added, [&](const ItemStack::Component& c) { return c.type == type; });
                    st.added.push_back({type, std::string(w.buf.begin(), w.buf.end())});
                    st.potion = p;
                    out.push_back(st);
                }
            break;
        }
        case SlotDisplay::OnlyWithComponent:
            if (!d.children.empty())
                for (const ItemStack& st : resolve(s, d.children[0]))
                    if (s.items.component(st, s.items.componentName(d.component)).present()) out.push_back(st);
            break;
        case SlotDisplay::Item: case SlotDisplay::ItemStackT:
            if (!d.stack.empty()) out.push_back(d.stack);
            break;
        case SlotDisplay::Tag:
            if (!d.tag.empty()) {
                if (const std::vector<int>* t = s.items.tag("minecraft:item", d.tag))
                    for (int i : *t) out.push_back(stackOf(i));
            } else {
                for (int i : d.ids) out.push_back(stackOf(i));
            }
            break;
        case SlotDisplay::Dyed:  // the targets (the dye colours are the server's to apply)
            if (d.children.size() == 2) out = resolve(s, d.children[1]);
            if (out.size() > 16) out.resize(16);
            break;
        case SlotDisplay::SmithingTrim: case SlotDisplay::WithRemainder:
            if (!d.children.empty()) out = resolve(s, d.children[0]);
            break;
        case SlotDisplay::Composite:
            for (const SlotDisplay& c : d.children) {
                std::vector<ItemStack> v = resolve(s, c);
                out.insert(out.end(), v.begin(), v.end());
            }
            break;
        default: break;
    }
    return out;
}

// --- the book --------------------------------------------------------------------------------------------------

// RecipeBookCategories (network ids) and RecipeBookType.
enum Category { CraftingBuildingBlocks, CraftingRedstone, CraftingEquipment, CraftingMisc, FurnaceFood, FurnaceBlocks, FurnaceMisc,
                BlastFurnaceBlocks, BlastFurnaceMisc, SmokerFood };
enum BookType { Crafting, FurnaceBook, BlastFurnaceBook, SmokerBook };

struct Collection {  // RecipeCollection
    std::vector<int> entries;  // recipe ids
    std::set<int> craftable, selected;
    bool hasCraftable() const { return !craftable.empty(); }
    bool hasAnySelected() const { return !selected.empty(); }
};

struct Book {  // ClientRecipeBook
    std::map<int, Entry> known;  // by id (the game's HashMap iterates small ids in order)
    std::set<int> highlight;
    bool open[4] = {}, filtering[4] = {};
    std::vector<Collection> collections;
    std::map<int, std::vector<int>> byCategory;  // category -> collection indices
    int version = 0;                             // bumped with every rebuild
} g_book;

void rebuildCollections() {
    g_book.collections.clear();
    g_book.byCategory.clear();
    std::map<std::pair<int, int>, int> groups;  // (category, group) -> collection
    for (const auto& [id, e] : g_book.known) {
        if (e.group >= 0) {
            auto it = groups.find({e.category, e.group});
            if (it != groups.end()) {
                g_book.collections[size_t(it->second)].entries.push_back(id);
                continue;
            }
            groups[{e.category, e.group}] = int(g_book.collections.size());
        }
        g_book.byCategory[e.category].push_back(int(g_book.collections.size()));
        g_book.collections.push_back(Collection{{id}, {}, {}});
    }
    ++g_book.version;
}

// The toast (RecipeToast): the stations and results of the recipes unlocked while it shows.
std::vector<std::pair<ItemStack, ItemStack>> g_toastItems;

void recipeToast(UiState& s, const RecipeDisplay& d) {
    std::vector<ItemStack> station = resolve(s, d.station), result = resolve(s, d.result);
    g_toastItems.push_back({station.empty() ? ItemStack{} : station[0], result.empty() ? ItemStack{} : result[0]});
    UiState* sp = &s;
    showCustomToast("recipe", 160, [sp](Canvas& c, float x, float y, double shownFor) {
        UiState& s = *sp;
        blitSprite(c, s.sprites.sprite("toast/recipe"), x, y, 160, 32);
        s.drawText(c, translatable("recipe.toast.title", {}, s.lang()), x + 30, y + 7, 0xFF500050, false);
        s.drawText(c, translatable("recipe.toast.description", {}, s.lang()), x + 30, y + 18, 0xFF000000, false);
        if (g_toastItems.empty()) return;
        const double per = std::max(1.0, 5.0 / double(g_toastItems.size()));
        const auto& [station, result] = g_toastItems[size_t(shownFor / per) % g_toastItems.size()];
        c.push();
        c.translate(x, y);
        c.scaleBy(0.6f);
        s.drawItem(c, station, 3, 3, false);
        c.pop();
        s.drawItem(c, result, x + 8, y + 8, false);
    });
}

void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("recipe_book_add", [sp](net::Reader& r) {
        std::vector<std::pair<Entry, int>> entries;  // entry, flags (1 notification, 2 highlight)
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            Entry e;
            if (!readEntry(sp->items, r, e)) break;
            const int flags = r.u8v();
            entries.push_back({std::move(e), flags});
        }
        const bool replace = r.boolean();
        if (!r.ok) LOG("ui: recipe_book_add: malformed after %zu entries", entries.size());
        sp->post([sp, entries = std::move(entries), replace]() mutable {
            if (replace) g_book.known.clear(), g_book.highlight.clear();
            g_toastItems.clear();
            for (auto& [e, flags] : entries) {
                if (flags & 2) g_book.highlight.insert(e.id);
                if (flags & 1) recipeToast(*sp, e.display);
                const int id = e.id;
                g_book.known[id] = std::move(e);
            }
            rebuildCollections();
        });
    });
    client.onPlay("recipe_book_remove", [sp](net::Reader& r) {
        std::vector<int> ids;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) ids.push_back(r.varInt());
        sp->post([ids] {
            for (int id : ids) g_book.known.erase(id), g_book.highlight.erase(id);
            rebuildCollections();
        });
    });
    client.onPlay("recipe_book_settings", [sp](net::Reader& r) {  // crafting, furnace, blast furnace, smoker
        bool open[4], filtering[4];
        for (int i = 0; i < 4; ++i) open[i] = r.boolean(), filtering[i] = r.boolean();
        if (!r.ok) return;
        sp->post([open0 = std::vector<bool>(open, open + 4), filter0 = std::vector<bool>(filtering, filtering + 4)] {
            for (int i = 0; i < 4; ++i) g_book.open[i] = open0[size_t(i)], g_book.filtering[i] = filter0[size_t(i)];
            ++g_book.version;
        });
    });
}

void attached(UiState&) {
    g_book = Book{};
    g_toastItems.clear();
}

const UiModuleRegistrar kModule([] {
    UiModule m;
    m.name = "recipe_book";
    m.registerHandlers = registerHandlers;
    m.attached = attached;
    return m;
}());

// --- the component ---------------------------------------------------------------------------------------------

struct TabInfo {
    const char* icon;
    const char* icon2;  // nullptr: one icon
    int category;       // -1: the search tab
};
const std::vector<TabInfo>& tabsFor(int type) {
    static const std::vector<TabInfo> kTabs[4] = {
        {{"compass", nullptr, -1}, {"iron_axe", "golden_sword", CraftingEquipment}, {"bricks", nullptr, CraftingBuildingBlocks},
         {"lava_bucket", "apple", CraftingMisc}, {"redstone", nullptr, CraftingRedstone}},
        {{"compass", nullptr, -1}, {"porkchop", nullptr, FurnaceFood}, {"stone", nullptr, FurnaceBlocks}, {"lava_bucket", "emerald", FurnaceMisc}},
        {{"compass", nullptr, -1}, {"redstone_ore", nullptr, BlastFurnaceBlocks}, {"iron_shovel", "golden_leggings", BlastFurnaceMisc}},
        {{"compass", nullptr, -1}, {"porkchop", nullptr, SmokerFood}}};
    return kTabs[std::clamp(type, 0, 3)];
}
// SearchRecipeBookCategory: the categories a type's search tab shows, in order.
std::vector<int> searchCategories(int type) {
    switch (type) {
        case Crafting: return {CraftingEquipment, CraftingBuildingBlocks, CraftingMisc, CraftingRedstone};
        case FurnaceBook: return {FurnaceFood, FurnaceBlocks, FurnaceMisc};
        case BlastFurnaceBook: return {BlastFurnaceBlocks, BlastFurnaceMisc};
        default: return {SmokerFood};
    }
}
const char* filterName(int type) {
    static const char* kNames[4] = {"gui.recipebook.toggleRecipes.craftable", "gui.recipebook.toggleRecipes.smeltable",
                                    "gui.recipebook.toggleRecipes.blastable", "gui.recipebook.toggleRecipes.smokable"};
    return kNames[std::clamp(type, 0, 3)];
}

struct Ghost {  // GhostSlots
    std::vector<ItemStack> items;
    bool result = false;
};

struct Overlay {  // OverlayRecipeComponent
    bool visible = false;
    int collection = -1;
    float x = 0, y = 0;
    struct Button {
        int recipe;
        bool craftable;
        float x, y;
        struct Pos {
            int x, y;
            std::vector<ItemStack> items;
        };
        std::vector<Pos> slots;
    };
    std::vector<Button> buttons;
};

struct Component {  // RecipeBookComponent
    int containerId = -2;  // the menu it was set up for
    std::string menuType;
    int bookType = 0;
    bool visible = false, narrow = false;
    int xOffset = 86;
    double time = 0;  // ticks (SlotSelectTime: floor(time / 30))
    int selectedTab = 0;
    UiState::TextField search;
    bool searchFocused = false, ignoreText = false;
    std::string lastSearch;
    std::vector<int> page;  // the shown collections (indices into g_book.collections)
    int currentPage = 0, totalPages = 0;
    std::vector<float> animation = std::vector<float>(20, 0.0f);  // RecipeButton highlight squeeze
    std::vector<float> tabAnimation = std::vector<float>(5, 0.0f);
    Overlay overlay;
    std::map<int, Ghost> ghosts;  // menu slot -> ghost
    int lastPlaced = -1, lastRecipe = -1, lastCollection = -1;
    std::vector<std::pair<int, int>> contentsKey;  // what the craftable state was computed from
    int bookVersion = -1;
} g;

int slotSelectTime() { return int(std::floor(g.time / 30.0)); }

// The menus with a recipe book and their RecipeBookType; -1 none.
int bookTypeOf(const Menu& m) {
    if (m.type < 0 || m.typeName == "crafting") return Crafting;
    if (m.typeName == "furnace") return FurnaceBook;
    if (m.typeName == "blast_furnace") return BlastFurnaceBook;
    if (m.typeName == "smoker") return SmokerBook;
    return -1;
}
bool furnaceMenu() { return g.bookType != Crafting; }
int gridWidth(const Menu& m) { return m.type < 0 ? 2 : 3; }
std::vector<int> gridSlots(const Menu& m) {  // the input grid (menu slots, row by row)
    if (m.type < 0) return {1, 2, 3, 4};
    if (m.typeName == "crafting") return {1, 2, 3, 4, 5, 6, 7, 8, 9};
    return {0};
}
int resultSlotOf(const Menu& m) { return m.type < 0 || m.typeName == "crafting" ? 0 : 2; }
bool isCraftingSlot(const Menu& m, int slot) {
    if (furnaceMenu()) return slot >= 0 && slot <= 2;
    const std::vector<int> grid = gridSlots(m);
    return slot == resultSlotOf(m) || std::find(grid.begin(), grid.end(), slot) != grid.end();
}

void sendSettings(UiState& s) {  // ServerboundRecipeBookChangeSettingsPacket
    if (!s.online()) return;
    net::Writer w;
    w.varInt(g.bookType);
    w.boolean(g_book.open[g.bookType]);
    w.boolean(g_book.filtering[g.bookType]);
    s.client->sendPlay("recipe_book_change_settings", w);
}

// --- crafting from the inventory (StackedItemContents) -------------------------------------------------------

struct Contents {
    std::map<int, int> counts;  // item -> amount
};
bool usableForCrafting(UiState& s, const ItemStack& st) {  // Inventory.isUsableForCrafting
    return !(st.damage > 0 && s.items.maxDamage(st) > 0) && st.enchantList.empty() && !st.hasCustomName;
}
Contents stackedContents(UiState& s, const Menu& m) {
    Contents c;
    const int start = layoutFor(m).playerStart;
    auto account = [&](const ItemStack& st, bool simple) {
        if (st.empty() || (simple && !usableForCrafting(s, st))) return;
        c.counts[st.item] += std::min(st.count, s.items.maxStack(st));
    };
    for (int i = 0; i < 36 && start >= 0 && size_t(start + i) < m.slots.size(); ++i) account(m.slots[size_t(start + i)], true);
    if (furnaceMenu()) {
        for (int i = 0; i < 3 && size_t(i) < m.slots.size(); ++i) account(m.slots[size_t(i)], false);
    } else {
        for (int slot : gridSlots(m))
            if (size_t(slot) < m.slots.size()) account(m.slots[size_t(slot)], true);
    }
    return c;
}
// StackedContents.tryPick for one craft: each requirement takes one item it accepts (augmenting paths).
bool canCraft(UiState& s, const Contents& c, const Entry& e) {
    if (!e.hasRequirements) return false;
    std::vector<std::vector<int>> options;
    for (const Ingredient& in : e.requirements) {
        std::vector<int> items;
        if (!in.tag.empty()) {
            if (const std::vector<int>* t = s.items.tag("minecraft:item", in.tag)) items = *t;
        } else {
            items = in.ids;
        }
        std::erase_if(items, [&](int i) { return !c.counts.count(i); });
        if (items.empty()) return false;
        options.push_back(std::move(items));
    }
    std::map<int, std::vector<int>> holders;  // item -> the requirements using one of it
    std::function<bool(int, std::set<int>&)> augment = [&](int req, std::set<int>& seen) -> bool {
        for (int item : options[size_t(req)]) {
            if (!seen.insert(item).second) continue;
            std::vector<int>& h = holders[item];
            if (int(h.size()) < c.counts.at(item)) {
                h.push_back(req);
                return true;
            }
            for (size_t k = 0; k < h.size(); ++k)
                if (augment(h[k], seen)) {  // that requirement moved to another item: this one is free
                    holders[item][k] = req;
                    return true;
                }
        }
        return false;
    };
    for (size_t i = 0; i < options.size(); ++i) {
        std::set<int> seen;
        if (!augment(int(i), seen)) return false;
    }
    return true;
}

// RecipeCollection.selectRecipes for the open menu: what fits (CraftingRecipeBookComponent.canDisplay, furnace
// displays) and what the contents can craft.
void selectRecipes(UiState& s, const Menu& m) {
    const Contents c = stackedContents(s, m);
    const int w = gridWidth(m);
    for (Collection& col : g_book.collections) {
        col.selected.clear(), col.craftable.clear();
        for (int id : col.entries) {
            const Entry& e = g_book.known[id];
            const RecipeDisplay& d = e.display;
            bool fits = false;
            if (furnaceMenu()) fits = d.type == RecipeDisplay::Furnace;
            else if (d.type == RecipeDisplay::Shaped) fits = w >= d.width && w >= d.height;
            else if (d.type == RecipeDisplay::Shapeless) fits = w * w >= int(d.ingredients.size());
            if (!fits) continue;
            col.selected.insert(id);
            if (canCraft(s, c, e)) col.craftable.insert(id);
        }
    }
}

std::vector<int> tabCollections(int tab) {
    const TabInfo& t = tabsFor(g.bookType)[size_t(tab)];
    std::vector<int> out;
    for (int cat : t.category < 0 ? searchCategories(g.bookType) : std::vector<int>{t.category})
        if (auto it = g_book.byCategory.find(cat); it != g_book.byCategory.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    return out;
}

std::vector<int> selectedEntries(const Collection& col, bool craftableOnly) {  // getSelectedRecipes(CRAFTABLE / ANY)
    std::vector<int> out;
    for (int id : col.entries)
        if (craftableOnly ? col.craftable.count(id) : col.selected.count(id)) out.push_back(id);
    return out;
}

// The tooltip lines of a stack, lower case without formatting (SessionSearchTrees.getTooltipLines).
std::string searchText(UiState& s, const ItemStack& st) {
    std::string text;
    for (const Text& l : s.items.tooltip(st)) text += l.plain() + "\n";
    text += s.items.name(st.item);
    for (char& ch : text) ch = char(std::tolower(u8(ch)));
    return text;
}

void updateCollections(UiState& s, bool resetPage) {  // RecipeBookComponent.updateCollections
    const bool filtering = g_book.filtering[g.bookType];
    std::vector<int> list;
    std::string query;
    for (char ch : g.search.value) query += char(std::tolower(u8(ch)));
    for (int ci : tabCollections(g.selectedTab)) {
        const Collection& col = g_book.collections[size_t(ci)];
        if (!col.hasAnySelected()) continue;
        if (filtering && !col.hasCraftable()) continue;
        if (!query.empty()) {
            bool match = false;
            for (int id : col.entries) {
                for (const ItemStack& st : resolve(s, g_book.known[id].display.result))
                    if (searchText(s, st).find(query) != std::string::npos) match = true;
                if (match) break;
            }
            if (!match) continue;
        }
        list.push_back(ci);
    }
    g.page = std::move(list);
    g.totalPages = int(std::ceil(double(g.page.size()) / 20.0));
    if (g.totalPages <= g.currentPage || resetPage) g.currentPage = 0;
    // RecipeButton.init: newly shown highlighted recipes are seen (ServerboundRecipeBookSeenRecipePacket).
    for (int i = 0; i < 20; ++i) {
        const size_t at = size_t(g.currentPage * 20 + i);
        if (at >= g.page.size()) break;
        bool shown = false;
        for (int id : selectedEntries(g_book.collections[size_t(g.page[at])], filtering))
            if (g_book.highlight.erase(id)) {
                shown = true;
                if (s.online()) {
                    net::Writer w;
                    w.varInt(id);
                    s.client->sendPlay("recipe_book_seen_recipe", w);
                }
            }
        if (shown) g.animation[size_t(i)] = 15.0f;
    }
}

bool tabVisible(int tab) {  // RecipeBookTabButton.updateVisibility (the search tab always)
    if (tabsFor(g.bookType)[size_t(tab)].category < 0) return true;
    for (int ci : tabCollections(tab))
        if (g_book.collections[size_t(ci)].hasAnySelected()) return true;
    return false;
}

std::vector<std::pair<int, int>> contentsKey(const Menu& m) {
    std::vector<std::pair<int, int>> key;
    for (const ItemStack& st : m.slots) key.push_back({st.item, st.empty() ? 0 : st.count});
    return key;
}

// Keeps the component in step with the menu, the book data and the contents (init, tick, recipesUpdated).
void refresh(UiState& s, const Menu& m) {
    const int type = bookTypeOf(m);
    const bool newMenu = g.containerId != m.containerId || g.menuType != m.typeName;
    if (newMenu) {  // init
        g.containerId = m.containerId, g.menuType = m.typeName;
        g.bookType = type;
        g.ghosts.clear();
        g.overlay.visible = false;
        g.lastPlaced = g.lastRecipe = g.lastCollection = -1;
        g.searchFocused = false;
        g.contentsKey.clear();
        g.bookVersion = -1;
        if (size_t(g.selectedTab) >= tabsFor(type).size()) g.selectedTab = 0;
    }
    g.narrow = s.guiW() < 379;
    g.xOffset = g.narrow ? 0 : 86;
    const bool shouldBeVisible = g_book.open[g.bookType];
    if (shouldBeVisible != g.visible) {
        g.visible = shouldBeVisible;
        if (!g.visible) g.overlay.visible = false;
        g.bookVersion = -1;
    }
    if (!g.visible) return;
    const auto key = contentsKey(m);
    if (key != g.contentsKey || g.bookVersion != g_book.version) {
        const bool rebuilt = g.bookVersion != g_book.version;
        g.contentsKey = key, g.bookVersion = g_book.version;
        selectRecipes(s, m);
        if (!tabVisible(g.selectedTab)) g.selectedTab = 0;
        if (rebuilt)  // startAnimation: a tab with a highlighted recipe
            for (size_t t = 0; t < tabsFor(g.bookType).size(); ++t)
                for (int ci : tabCollections(int(t)))
                    for (int id : selectedEntries(g_book.collections[size_t(ci)], g_book.filtering[g.bookType]))
                        if (g_book.highlight.count(id)) g.tabAnimation[t] = 15.0f;
        updateCollections(s, false);
    }
}

float originX(UiState& s) { return float((s.guiW() - 147) / 2 - g.xOffset); }
float originY(UiState& s) { return float((s.guiH() - 166) / 2); }

struct ButtonPos {
    float x, y;
};
ButtonPos toggleButton(UiState& s, const Menu& m, float left) {  // getRecipeBookButtonPosition
    const float H = float(s.guiH());
    if (m.type < 0) return {left + 104, float(int(H) / 2 - 22)};
    if (m.typeName == "crafting") return {left + 5, float(int(H) / 2 - 49)};
    return {left + 20, float(int(H) / 2 - 49)};
}
bool inRect(float mx, float my, float x, float y, float w, float h) { return mx >= x && my >= y && mx < x + w && my < y + h; }

void setVisible(UiState& s, bool visible) {
    g.visible = visible;
    g_book.open[g.bookType] = visible;
    if (!visible) g.overlay.visible = false;
    g.bookVersion = -1;
    sendSettings(s);
}

void openOverlay(UiState& s, int collection, float bx, float by) {  // OverlayRecipeComponent.init
    Overlay& o = g.overlay;
    o = Overlay{};
    o.collection = collection;
    const Collection& col = g_book.collections[size_t(collection)];
    const bool filtering = g_book.filtering[g.bookType];
    std::vector<int> craftable = selectedEntries(col, true), uncraftable;
    if (!filtering)
        for (int id : col.entries)
            if (col.selected.count(id) && !col.craftable.count(id)) uncraftable.push_back(id);
    const int total = int(craftable.size() + uncraftable.size());
    const int maxRow = total <= 16 ? 4 : 5;
    const int rows = (total + maxRow - 1) / maxRow;
    const float xo = originX(s), yo = originY(s);
    const float centerX = xo + 147 / 2, centerY = yo + 13 + 166 / 2;
    o.x = bx, o.y = by;
    const float right = o.x + float(std::min(total, maxRow) * 25), maxLeft = centerX + 50;
    if (right > maxLeft) o.x -= 25.0f * float(int((right - maxLeft) / 25.0f));
    const float bottom = o.y + float(rows * 25), maxBottom = centerY + 50;
    if (bottom > maxBottom) o.y -= 25.0f * std::ceil((bottom - maxBottom) / 25.0f);
    const float maxTop = centerY - 100;
    if (o.y < maxTop) o.y -= 25.0f * std::ceil((o.y - maxTop) / 25.0f);
    o.visible = true;
    for (int i = 0; i < total; ++i) {
        const bool can = i < int(craftable.size());
        const int id = can ? craftable[size_t(i)] : uncraftable[size_t(i) - craftable.size()];
        Overlay::Button b{id, can, o.x + 4 + float(25 * (i % maxRow)), o.y + 5 + float(25 * (i / maxRow)), {}};
        const RecipeDisplay& d = g_book.known[id].display;
        auto pos = [&](int gx, int gy, const SlotDisplay& sd) {
            std::vector<ItemStack> items = resolve(s, sd);
            if (!items.empty()) b.slots.push_back({3 + gx * 7, 3 + gy * 7, std::move(items)});
        };
        if (furnaceMenu()) {
            if (d.type == RecipeDisplay::Furnace && !d.ingredients.empty()) pos(1, 1, d.ingredients[0]);
        } else if (d.type == RecipeDisplay::Shaped) {  // PlaceRecipeHelper in a 3 x 3 grid
            for (int y = 0; y < d.height; ++y)
                for (int x = 0; x < d.width; ++x)
                    if (size_t(x + y * d.width) < d.ingredients.size()) pos(x, y, d.ingredients[size_t(x + y * d.width)]);
        } else if (d.type == RecipeDisplay::Shapeless) {
            for (size_t k = 0; k < d.ingredients.size(); ++k) pos(int(k % 3), int(k / 3), d.ingredients[k]);
        }
        o.buttons.push_back(std::move(b));
    }
}

// GhostSlots from a recipe display (Crafting/FurnaceRecipeBookComponent.fillGhostRecipe).
void fillGhost(UiState& s, const Menu& m, const RecipeDisplay& d) {
    g.ghosts.clear();
    auto set = [&](int slot, const SlotDisplay& sd, bool result) {
        std::vector<ItemStack> items = resolve(s, sd);
        if (!items.empty()) g.ghosts[slot] = Ghost{std::move(items), result};
    };
    set(resultSlotOf(m), d.result, true);
    if (furnaceMenu()) {
        if (d.type != RecipeDisplay::Furnace || d.ingredients.size() < 2) return;
        set(0, d.ingredients[0], false);
        if (m.slots.size() > 1 && m.slots[1].empty()) set(1, d.ingredients[1], false);
        return;
    }
    const std::vector<int> grid = gridSlots(m);
    const int w = gridWidth(m);
    if (d.type == RecipeDisplay::Shaped) {  // PlaceRecipeHelper.placeRecipe: the recipe at the grid's top left
        for (int y = 0; y < d.height && y < w; ++y)
            for (int x = 0; x < d.width && x < w; ++x)
                if (size_t(x + y * d.width) < d.ingredients.size()) set(grid[size_t(x + y * w)], d.ingredients[size_t(x + y * d.width)], false);
    } else if (d.type == RecipeDisplay::Shapeless) {
        for (size_t i = 0; i < d.ingredients.size() && i < grid.size(); ++i) set(grid[i], d.ingredients[i], false);
    }
}

bool tryPlace(UiState& s, const Menu& m, int collection, int recipe, bool useMax) {  // tryPlaceRecipe
    const Collection& col = g_book.collections[size_t(collection)];
    if (!col.craftable.count(recipe) && recipe == g.lastPlaced) return false;
    g.lastPlaced = recipe;
    g.ghosts.clear();
    if (s.online()) {  // ServerboundPlaceRecipePacket
        net::Writer w;
        w.varInt(m.containerId);
        w.varInt(recipe);
        w.boolean(useMax);
        s.client->sendPlay("place_recipe", w);
    }
    return true;
}


// RecipeButton.getDisplayStack / getCurrentRecipe / allRecipesHaveSameResultDisplay
struct ButtonView {
    std::vector<int> ids;
    std::vector<std::vector<ItemStack>> results;
    ItemStack display;
    int current = -1;
    bool sameResult = true;
};
ButtonView viewOf(UiState& s, int collection) {
    ButtonView v;
    v.ids = selectedEntries(g_book.collections[size_t(collection)], g_book.filtering[g.bookType]);
    for (int id : v.ids) v.results.push_back(resolve(s, g_book.known[id].display.result));
    if (v.ids.empty()) return v;
    const int index = slotSelectTime(), n = int(v.ids.size());
    const int offset = index / n, entry = index - n * offset;
    v.current = v.ids[size_t(index % n)];
    const std::vector<ItemStack>& r = v.results[size_t(entry)];
    if (!r.empty()) v.display = r[size_t(offset % int(r.size()))];
    const ItemStack* firstItem = nullptr;
    for (const auto& list : v.results)
        for (const ItemStack& st : list) {
            if (!firstItem) firstItem = &st;
            else if (!firstItem->sameItemSameComponents(st)) v.sameResult = false;
        }
    return v;
}

}  // namespace

// --- the container screen's hooks (item_ui.h) -----------------------------------------------------------------

int recipeBookLeft(UiState& s, const Menu& m, int imageWidth) {  // RecipeBookComponent.updateScreenPosition
    const int W = s.guiW();
    if (bookTypeOf(m) < 0 || !s.survivalLike() && m.type < 0) return (W - imageWidth) / 2;
    refresh(s, m);
    if (g.visible && !g.narrow) return 177 + (W - imageWidth - 200) / 2;
    return (W - imageWidth) / 2;
}

bool recipeBookCovers(const Menu& m, float x, float y, float left, float top, int imageHeight) {  // hasClickedOutside
    if (bookTypeOf(m) < 0 || !g.visible) return false;
    return left - 147 < x && x < left && top < y && y < top + float(imageHeight);
}

void recipeBookDrawBackground(UiState& s, Canvas& c, Menu& m, float left) {  // the toggle button (a screen widget)
    if (bookTypeOf(m) < 0) return;
    const ButtonPos b = toggleButton(s, m, left);
    const bool hover = inRect(s.mouseX, s.mouseY, b.x, b.y, 20, 18);
    blitSprite(c, s.sprites.sprite(hover ? "recipe_book/button_highlighted" : "recipe_book/button"), b.x, b.y, 20, 18);
}

void recipeBookDrawGhosts(UiState& s, Canvas& c, Menu& m, const Layout& l, float left, float top) {
    if (bookTypeOf(m) < 0 || !g.visible) return;
    const bool big = m.type >= 0;  // isBiggerResultSlot (not the inventory)
    for (const auto& [slot, ghost] : g.ghosts) {
        if (size_t(slot) >= l.slots.size()) continue;
        const float x = left + float(l.slots[size_t(slot)].x), y = top + float(l.slots[size_t(slot)].y);
        if (ghost.result && big) c.fill(x - 4, y - 4, x + 20, y + 20, 0x30FF0000);
        else c.fill(x, y, x + 16, y + 16, 0x30FF0000);
        const ItemStack& st = ghost.items[size_t(slotSelectTime()) % ghost.items.size()];
        s.drawItem(c, st, x, y, false);
        c.fill(x, y, x + 16, y + 16, 0x30FFFFFF);
        if (ghost.result) s.drawItem(c, st, x, y, true, st.count == 1 ? nullptr : std::to_string(st.count).c_str());
    }
}

void recipeBookDraw(UiState& s, Canvas& c, Menu& m) {
    if (bookTypeOf(m) < 0 || !g.visible) return;
    if (!s.window->keyDown(VK_CONTROL)) g.time += s.frameMs / 50.0;  // ticks; held Ctrl freezes the cycling
    const float xo = originX(s), yo = originY(s);
    const float mx = s.mouseX, my = s.mouseY;
    if (const Sprite* bg = s.sprites.texture("gui/recipe_book")) blitTexture(c, bg, xo, yo, 1, 1, 147, 166, 256, 256);
    // The search box (EditBox 81 x 14, bordered) with its hint.
    const float sx = xo + 25, sy = yo + 13;
    blitSprite(c, s.sprites.sprite(g.searchFocused ? "widget/text_field_highlighted" : "widget/text_field"), sx, sy, 81, 14);
    if (g.search.value.empty() && !g.searchFocused) {
        Style hint = colorStyle(0xAAAAAA);
        hint.italic = 1;
        s.drawText(c, translatable("gui.recipebook.search_hint", {}, s.lang(), hint), sx + 4, sy + 3, 0xFFFFFFFF, true);
    } else {
        s.drawTextField(c, g.search, sx + 4, sy + 3, 73, 0xFFFFFFFF, g.searchFocused, true);
    }
    // Tabs (RecipeBookTabButton) down the left edge.
    const std::vector<TabInfo>& tabs = tabsFor(g.bookType);
    int index = 0;
    for (size_t t = 0; t < tabs.size(); ++t) {
        if (!tabVisible(int(t))) continue;
        const bool sel = int(t) == g.selectedTab;
        const float tx = xo - 30, ty = yo + 3 + float(27 * index++);
        c.push();
        if (g.tabAnimation[t] > 0) {
            const float squeeze = 1.0f + 0.1f * std::sin(g.tabAnimation[t] / 15.0f * kPi);
            c.translate(tx + 8, ty + 12);
            c.scaleBy(1.0f, squeeze);
            c.translate(-(tx + 8), -(ty + 12));
            g.tabAnimation[t] -= float(s.frameMs / 50.0);
        }
        blitSprite(c, s.sprites.sprite(sel ? "recipe_book/tab_selected" : "recipe_book/tab"), sel ? tx - 2 : tx, ty, 35, 27);
        const float move = sel ? -2.0f : 0.0f;
        ItemStack a, b;
        a.item = s.items.id(tabs[t].icon), a.count = 1;
        if (tabs[t].icon2) {
            b.item = s.items.id(tabs[t].icon2), b.count = 1;
            s.drawItem(c, a, tx + 3 + move, ty + 5, false);
            s.drawItem(c, b, tx + 14 + move, ty + 5, false);
        } else {
            s.drawItem(c, a, tx + 9 + move, ty + 5, false);
        }
        c.pop();
    }
    // The craftable filter (CycleButton with the filter sprites).
    const bool filtering = g_book.filtering[g.bookType];
    const bool filterHover = inRect(mx, my, xo + 110, yo + 12, 26, 16);
    const std::string prefix = furnaceMenu() ? "recipe_book/furnace_filter_" : "recipe_book/filter_";
    blitSprite(c, s.sprites.sprite(prefix + (filtering ? "enabled" : "disabled") + (filterHover ? "_highlighted" : "")), xo + 110, yo + 12, 26, 16);
    // The page (RecipeBookPage): 5 x 4 recipe buttons, the page number and arrows.
    if (g.totalPages > 1) {
        const Text pn = translatable("gui.recipebook.page", {Text(std::to_string(g.currentPage + 1)), Text(std::to_string(g.totalPages))}, s.lang());
        s.drawText(c, pn, xo - float(s.font.width(pn) / 2) + 73, yo + 141, 0xFFFFFFFF, true);
    }
    for (int i = 0; i < 20; ++i) {
        const size_t at = size_t(g.currentPage * 20 + i);
        if (at >= g.page.size()) break;
        const int ci = g.page[at];
        const ButtonView v = viewOf(s, ci);
        if (v.ids.empty()) continue;
        const float bx = xo + 11 + float(25 * (i % 5)), by = yo + 31 + float(25 * (i / 5));
        const bool many = v.ids.size() > 1;
        const char* sprite = g_book.collections[size_t(ci)].hasCraftable() ? (many ? "recipe_book/slot_many_craftable" : "recipe_book/slot_craftable")
                                                                          : (many ? "recipe_book/slot_many_uncraftable" : "recipe_book/slot_uncraftable");
        c.push();
        if (g.animation[size_t(i)] > 0) {
            const float squeeze = 1.0f + 0.1f * std::sin(g.animation[size_t(i)] / 15.0f * kPi);
            c.translate(bx + 8, by + 12);
            c.scaleBy(squeeze, squeeze);
            c.translate(-(bx + 8), -(by + 12));
            g.animation[size_t(i)] -= float(s.frameMs / 50.0);
        }
        blitSprite(c, s.sprites.sprite(sprite), bx, by, 25, 25);
        float offset = 4;
        if (many && v.sameResult) s.drawItem(c, v.display, bx + offset + 1, by + offset + 1, false), offset -= 1;
        s.drawItem(c, v.display, bx + offset, by + offset, false);
        c.pop();
    }
    const bool forward = g.totalPages > 1 && g.currentPage < g.totalPages - 1, back = g.totalPages > 1 && g.currentPage > 0;
    if (forward)
        blitSprite(c, s.sprites.sprite(inRect(mx, my, xo + 93, yo + 137, 12, 17) ? "recipe_book/page_forward_highlighted" : "recipe_book/page_forward"),
                   xo + 93, yo + 137, 12, 17);
    if (back)
        blitSprite(c, s.sprites.sprite(inRect(mx, my, xo + 38, yo + 137, 12, 17) ? "recipe_book/page_backward_highlighted" : "recipe_book/page_backward"),
                   xo + 38, yo + 137, 12, 17);
    // The alternatives overlay.
    const Overlay& o = g.overlay;
    if (o.visible) {
        const int maxRow = o.buttons.size() <= 16 ? 4 : 5;
        const int w = std::min(int(o.buttons.size()), maxRow), h = (int(o.buttons.size()) + maxRow - 1) / maxRow;
        blitSprite(c, s.sprites.sprite("recipe_book/overlay_recipe"), o.x, o.y, float(w * 25 + 8), float(h * 25 + 8));
        for (const Overlay::Button& b : o.buttons) {
            const bool hover = inRect(mx, my, b.x, b.y, 24, 24);
            const std::string base = furnaceMenu() ? "recipe_book/furnace_overlay" : "recipe_book/crafting_overlay";
            blitSprite(c, s.sprites.sprite(base + (b.craftable ? "" : "_disabled") + (hover ? "_highlighted" : "")), b.x, b.y, 24, 24);
            for (const auto& p : b.slots) {  // the ingredients at 0.375 scale
                c.push();
                c.translate(b.x + 2 + float(p.x), b.y + 2 + float(p.y));
                c.scaleBy(0.375f, 0.375f);
                c.translate(-8, -8);
                s.drawItem(c, p.items[size_t(slotSelectTime()) % p.items.size()], 0, 0, true);
                c.pop();
            }
        }
    }
}

void recipeBookTooltip(UiState& s, Canvas& c, Menu& m, int hoveredSlot) {
    if (bookTypeOf(m) < 0 || !g.visible) return;
    const float xo = originX(s), yo = originY(s);
    const float mx = s.mouseX, my = s.mouseY;
    if (!g.overlay.visible)  // RecipeBookPage.extractTooltip
        for (int i = 0; i < 20; ++i) {
            const size_t at = size_t(g.currentPage * 20 + i);
            if (at >= g.page.size()) break;
            if (!inRect(mx, my, xo + 11 + float(25 * (i % 5)), yo + 31 + float(25 * (i / 5)), 25, 25)) continue;
            const ButtonView v = viewOf(s, g.page[at]);
            if (v.display.empty()) break;
            std::vector<Text> lines = s.items.tooltip(v.display, s.settings.advancedItemTooltips);
            if (v.ids.size() > 1) lines.push_back(translatable("gui.recipebook.moreRecipes", {}, s.lang()));
            s.drawTooltip(c, lines, mx, my);  // a component tooltip: no image
            return;
        }
    if (hoveredSlot >= 0)  // GhostSlots.extractTooltip
        if (auto it = g.ghosts.find(hoveredSlot); it != g.ghosts.end() && size_t(hoveredSlot) < m.slots.size() && m.slots[size_t(hoveredSlot)].empty()) {
            const ItemStack& st = it->second.items[size_t(slotSelectTime()) % it->second.items.size()];
            s.drawTooltip(c, s.items.tooltip(st, s.settings.advancedItemTooltips), mx, my, nullptr, true, tooltipStyle(s.items, st));
            return;
        }
    // The filter's and the page arrows' tooltips.
    if (inRect(mx, my, xo + 110, yo + 12, 26, 16)) {
        const char* key = g_book.filtering[g.bookType] ? filterName(g.bookType) : "gui.recipebook.toggleRecipes.all";
        s.drawTooltip(c, {translatable(key, {}, s.lang())}, mx, my);
    } else if (g.totalPages > 1 && g.currentPage < g.totalPages - 1 && inRect(mx, my, xo + 93, yo + 137, 12, 17)) {
        s.drawTooltip(c, {translatable("gui.recipebook.next_page", {}, s.lang())}, mx, my);
    } else if (g.totalPages > 1 && g.currentPage > 0 && inRect(mx, my, xo + 38, yo + 137, 12, 17)) {
        s.drawTooltip(c, {translatable("gui.recipebook.previous_page", {}, s.lang())}, mx, my);
    }
}

bool recipeBookInput(UiState& s, const InputEvent& e, Menu& m, float left) {
    if (bookTypeOf(m) < 0) return false;
    refresh(s, m);
    const float mx = s.mouseX, my = s.mouseY;
    const float xo = originX(s), yo = originY(s);
    if (e.type == InputEvent::Button && e.down && e.code == 0) {  // the toggle button
        const ButtonPos b = toggleButton(s, m, left);
        if (inRect(mx, my, b.x, b.y, 20, 18)) {
            audio::playUiClick();
            setVisible(s, !g.visible);
            return true;
        }
    }
    if (!g.visible || s.spectator()) return false;
    if (e.type == InputEvent::Char) {
        if (g.ignoreText) {
            g.ignoreText = false;
            return true;
        }
        if (!g.searchFocused) return false;
        s.textFieldKey(g.search, e);
    } else if (e.type == InputEvent::Key && e.down) {
        g.ignoreText = false;
        if (e.code == VK_ESCAPE && !g.searchFocused && g.narrow) {
            setVisible(s, false);
            return true;
        }
        if (g.searchFocused) {
            if (e.code == VK_ESCAPE) return false;
            s.textFieldKey(g.search, e);  // captures every other key
        } else if (s.isKey(Key::Chat, e)) {
            g.ignoreText = true;
            g.searchFocused = true;
            return true;
        } else if ((e.code == VK_RETURN || e.code == VK_SPACE) && g.lastCollection >= 0 && g.lastRecipe >= 0 &&
                   size_t(g.lastCollection) < g_book.collections.size()) {  // isSelection: place the last recipe again
            audio::playUiClick();
            tryPlace(s, m, g.lastCollection, g.lastRecipe, s.window->keyDown(VK_SHIFT));
            return true;
        } else {
            return false;
        }
    } else if (e.type == InputEvent::Button && e.down) {
        const bool left = e.code == 0, right = e.code == 1;
        if (!left && !right) return false;
        // RecipeBookPage.mouseClicked
        Overlay& o = g.overlay;
        if (o.visible) {
            int clicked = -1;
            if (left)
                for (const Overlay::Button& b : o.buttons)
                    if (inRect(mx, my, b.x, b.y, 24, 24)) clicked = b.recipe;
            if (clicked >= 0) {
                audio::playUiClick();
                const int col = o.collection;
                if (tryPlace(s, m, col, clicked, s.window->keyDown(VK_SHIFT))) {
                    g.lastCollection = col, g.lastRecipe = clicked;
                    if (g.narrow) setVisible(s, false);
                }
            }
            o.visible = false;
            return true;
        }
        if (left && g.totalPages > 1 && g.currentPage < g.totalPages - 1 && inRect(mx, my, xo + 93, yo + 137, 12, 17)) {
            audio::playUiClick();
            ++g.currentPage;
            updateCollections(s, false);
            return true;
        }
        if (left && g.totalPages > 1 && g.currentPage > 0 && inRect(mx, my, xo + 38, yo + 137, 12, 17)) {
            audio::playUiClick();
            --g.currentPage;
            updateCollections(s, false);
            return true;
        }
        for (int i = 0; i < 20; ++i) {
            const size_t at = size_t(g.currentPage * 20 + i);
            if (at >= g.page.size()) break;
            const float bx = xo + 11 + float(25 * (i % 5)), by = yo + 31 + float(25 * (i / 5));
            if (!inRect(mx, my, bx, by, 25, 25)) continue;
            const int ci = g.page[at];
            const ButtonView v = viewOf(s, ci);
            audio::playUiClick();
            if (left && v.current >= 0) {
                if (tryPlace(s, m, ci, v.current, s.window->keyDown(VK_SHIFT))) {
                    g.lastCollection = ci, g.lastRecipe = v.current;
                    if (g.narrow) setVisible(s, false);
                }
            } else if (right && v.ids.size() > 1) {
                openOverlay(s, ci, bx, by);
            }
            return true;
        }
        if (!left) return false;
        // The search box (and the magnifier left of it).
        if (inRect(mx, my, xo + 8, yo + 13, 98, 14)) {
            g.searchFocused = true;
            if (s.textFieldHit(g.search)) s.textFieldMouseDown(g.search);
            return true;
        }
        g.searchFocused = false;
        if (inRect(mx, my, xo + 110, yo + 12, 26, 16)) {  // the filter: toggleFiltering, sendUpdateSettings
            audio::playUiClick();
            g_book.filtering[g.bookType] = !g_book.filtering[g.bookType];
            sendSettings(s);
            updateCollections(s, false);
            return true;
        }
        const std::vector<TabInfo>& tabs = tabsFor(g.bookType);
        int index = 0;
        for (size_t t = 0; t < tabs.size(); ++t) {
            if (!tabVisible(int(t))) continue;
            const float tx = xo - 30, ty = yo + 3 + float(27 * index++);
            if (!inRect(mx, my, tx, ty, 35, 27)) continue;
            if (int(t) != g.selectedTab) {
                audio::playUiClick();
                g.selectedTab = int(t);
                updateCollections(s, true);
            }
            return true;
        }
        return false;
    } else {
        return false;
    }
    // A key or character reached the search box: checkSearchStringUpdate.
    std::string q;
    for (char ch : g.search.value) q += char(std::tolower(u8(ch)));
    if (q != g.lastSearch) {
        g.lastSearch = q;
        updateCollections(s, false);
    }
    return true;
}

void recipeBookSlotClicked(UiState& s, Menu& m, int slot) {  // slotClicked: a crafting slot clears the ghost recipe
    if (bookTypeOf(m) < 0 || !isCraftingSlot(m, slot)) return;
    g.lastPlaced = -1;
    g.ghosts.clear();
    (void)s;
}

// ClientboundPlaceGhostRecipePacket: the server could not place the recipe (missing items).
void recipeBookGhost(UiState& s, int containerId, const std::string& display) {
    Menu& m = s.activeMenu();
    if (m.containerId != containerId || bookTypeOf(m) < 0) return;
    net::Reader r(reinterpret_cast<const u8*>(display.data()), display.size());
    RecipeDisplay d;
    if (readDisplay(s.items, r, d)) fillGhost(s, m, d);
}

namespace {
void ghostHandler(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("place_ghost_recipe", [sp](net::Reader& r) {
        const int id = r.varInt();
        std::string rest(reinterpret_cast<const char*>(r.p), r.left());
        sp->post([sp, id, rest] { recipeBookGhost(*sp, id, rest); });
    });
}
const UiModuleRegistrar kGhostModule([] {
    UiModule m;
    m.name = "recipe_book_ghost";
    m.registerHandlers = ghostHandler;
    return m;
}());
}  // namespace

}  // namespace ui
