// Station screens: anvil (AnvilScreen), grindstone (GrindstoneScreen), smithing table (SmithingScreen) and
// enchanting table (EnchantmentScreen with its book and the galactic enchantment names), registered as StationDefs
// (modules.h) on the generic container screen.
#include "entity/entities.h"
#include "ui/entity_preview.h"
#include "ui/item_ui.h"

#include <cmath>

namespace ui {

namespace {
constexpr float kPi = 3.14159265f;

using TextField = Ui::State::TextField;

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

// ItemCombinerMenu.quickMoveStack: result -> inventory (backwards), inputs -> inventory, inventory -> inputs when
// canMoveIntoInputSlots (and nothing else if they are full), else main inventory <-> hotbar.
int combinerQuickMove(int resultSlot, int index, const std::function<bool(int, int, bool)>& moveTo, bool intoInputs = true) {
    const int inv = resultSlot + 1, invEnd = inv + 27, end = invEnd + 9;
    if (index == resultSlot) return moveTo(inv, end, true) ? 1 : 0;
    if (index < resultSlot) return moveTo(inv, end, false) ? 1 : 0;
    if (intoInputs && index >= inv && index < end) return moveTo(0, resultSlot, false) ? 1 : 0;
    if (index >= inv && index < invEnd) return moveTo(invEnd, end, false) ? 1 : 0;
    if (index >= invEnd && index < end) return moveTo(inv, invEnd, false) ? 1 : 0;
    return 1;
}
bool hasComponent(UiState& s, const ItemStack& st, const char* name) { return s.items.component(st, name).present(); }

// ---- the item logic of the combining stations, run on the client like the game's menus (slotsChanged ->
// createResult); the server's result replaces it when it arrives ----

using Enchants = std::vector<std::pair<int, int>>;  // (enchantment network id, level)

bool isEnchantedBook(UiState& s, const ItemStack& st) { return !st.empty() && s.items.name(st.item) == "minecraft:enchanted_book"; }
// ItemStack.isDamageableItem
bool damageable(UiState& s, const ItemStack& st) {
    return hasComponent(s, st, "max_damage") && !hasComponent(s, st, "unbreakable") && hasComponent(s, st, "damage");
}
int maxDamageOf(UiState& s, const ItemStack& st) { return hasComponent(s, st, "max_damage") ? s.items.maxDamage(st) : 0; }
int intComponent(UiState& s, const ItemStack& st, const char* name) {  // a VarInt component, else its default, else 0
    const Items::ComponentValue v = s.items.component(st, name);
    if (v.patched) {
        net::Reader r(reinterpret_cast<const u8*>(v.patched->data()), v.patched->size());
        return r.varInt();
    }
    return v.fallback && v.fallback->is_number_integer() ? v.fallback->get<int>() : 0;
}
// Sets a VarInt component (a value equal to the item's default leaves no patch entry, like ItemStack.set).
void setInt(UiState& s, ItemStack& st, const char* name, int value) {
    const nlohmann::json* d = s.items.component(ItemStack{st.item, 1}, name).fallback;
    if (d && d->is_number_integer() && d->get<int>() == value) return s.items.setComponent(st, name, nullptr);
    net::Writer w;
    w.varInt(value);
    const std::string data(w.buf.begin(), w.buf.end());
    s.items.setComponent(st, name, &data);
}
// EnchantmentHelper.getEnchantmentsForCrafting / setEnchantments: stored_enchantments on enchanted books.
const Enchants& enchantsOf(UiState& s, const ItemStack& st) { return isEnchantedBook(s, st) ? st.storedEnchants : st.enchantList; }
void setEnchants(UiState& s, ItemStack& st, const Enchants& e) {
    const char* name = isEnchantedBook(s, st) ? "stored_enchantments" : "enchantments";
    if (e.empty() && s.items.component(ItemStack{st.item, 1}, name).present()) return s.items.setComponent(st, name, nullptr);
    net::Writer w;  // ItemEnchantments.STREAM_CODEC: a map of holder ids to VarInt levels
    w.varInt(i32(e.size()));
    for (const auto& [id, level] : e) w.varInt(id), w.varInt(level);
    const std::string data(w.buf.begin(), w.buf.end());
    s.items.setComponent(st, name, &data);
}
int levelOf(const Enchants& e, int id) {
    for (const auto& [k, v] : e)
        if (k == id) return v;
    return 0;
}
void setLevel(Enchants& e, int id, int level) {  // ItemEnchantments.Mutable.set (0 removes)
    std::erase_if(e, [&](const auto& p) { return p.first == id; });
    if (level > 0) e.emplace_back(id, level);
}
int increasedRepairCost(int cost) { return int(std::min<i64>(i64(cost) * 2 + 1, INT32_MAX)); }  // calculateIncreasedRepairCost

// --- anvil -----------------------------------------------------------------------------------------------------

struct AnvilState {
    TextField name;
    int lastItem = -2;
    std::string lastName, sent;
    bool sentValid = false;
    std::string predicted;           // the inputs and name the result was last computed for
    int repairItemCountCost = 0;     // AnvilMenu.repairItemCountCost / onlyRenaming of that result
    bool onlyRenaming = false;
} anvil;

// What a combining station's result depends on: its inputs (and the anvil's name).
std::string inputsKey(const Menu& m, int inputs, const std::string& extra = {}) {
    std::string k = extra;
    for (int i = 0; i < inputs; ++i) {
        const ItemStack& st = m.slots[size_t(i)];
        k += '|' + std::to_string(st.empty() ? -1 : st.item) + ':' + std::to_string(st.count);
        for (const auto& c : st.added) k += ',' + std::to_string(c.type) + '=' + c.data;
        for (int t : st.removed) k += ",-" + std::to_string(t);
    }
    return k;
}

std::string hoverNameOf(UiState& s, const ItemStack& st) { return st.empty() ? std::string() : s.items.displayName(st).plain(); }

void anvilLayout(const Menu&, Layout& l) {
    l.background = "gui/container/anvil";
    l.slots = {{27, 47}, {76, 47}, {134, 47}};
    playerSlots(l, 3);
    l.titleX = 60;
}

void anvilOpened(UiState&, Menu&) {
    anvil = AnvilState{};
    anvil.name.maxLength = 50;
}

void anvilSendName(UiState& s, Menu& m) {  // AnvilScreen.onNameChanged
    const ItemStack& in = m.slots[0];
    if (in.empty()) return;
    std::string newName = anvil.name.value;
    if (!in.hasCustomName && newName == hoverNameOf(s, in)) newName.clear();
    // AnvilMenu.setItemName / validateName: StringUtil.filterText (no section signs, control characters or DEL), at
    // most 50 UTF-16 units, sent only when that differs from the menu's name (the packet carries the text as typed)
    std::string filtered;
    int units = 0;
    for (size_t i = 0; i < newName.size();) {
        const u8 c = u8(newName[i]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        const bool section = len == 2 && i + 1 < newName.size() && c == 0xC2 && u8(newName[i + 1]) == 0xA7;
        if (!section && !(len == 1 && (c < 0x20 || c == 0x7F))) filtered.append(newName, i, len), units += len == 4 ? 2 : 1;
        i += len;
    }
    if (units > 50) return;
    if (anvil.sentValid && filtered == anvil.sent) return;
    anvil.sent = filtered, anvil.sentValid = true;
    if (!s.online()) return;
    net::Writer w;
    w.str(newName);
    s.client->sendPlay("rename_item", w);
}

void anvilSync(UiState& s, Menu& m) {  // slotChanged(0): the field shows the new item's name
    const ItemStack& in = m.slots[0];
    const std::string hn = hoverNameOf(s, in);
    const int item = in.empty() ? -1 : in.item;
    if (item == anvil.lastItem && hn == anvil.lastName) return;
    anvil.lastItem = item, anvil.lastName = hn;
    // EditBox.setValue: cut to the 50 (UTF-16) character limit
    size_t end = 0;
    for (int units = 0; end < hn.size();) {
        const u8 c = u8(hn[end]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if ((units += len == 4 ? 2 : 1) > anvil.name.maxLength) break;
        end += len;
    }
    anvil.name.value = hn.substr(0, end);
    anvil.name.cursor = anvil.name.selection = int(anvil.name.value.size());
    anvil.name.scroll = 0;
    anvil.name.blinkStart = s.clock;
    anvilSendName(s, m);
}

// AnvilMenu.createResult: repairs with material, combines with a copy or an enchanted book, renames; the level cost in
// data slot 0.
void anvilPredict(UiState& s, Menu& m) {
    const std::string key = inputsKey(m, 2, anvil.sentValid ? anvil.sent : std::string("\x01"));
    if (key == anvil.predicted) return;
    anvil.predicted = key;
    const ItemStack& input = m.slots[0];
    const ItemStack& addition = m.slots[1];
    anvil.onlyRenaming = false;
    int cost = 1, price = 0, namingCost = 0;
    auto fail = [&] { m.slots[2] = ItemStack{}, m.data[0] = 0; };
    if (input.empty() || !hasComponent(s, input, isEnchantedBook(s, input) ? "stored_enchantments" : "enchantments")) return fail();
    ItemStack result = input;
    Enchants enchantments = enchantsOf(s, result);
    const i64 tax = i64(intComponent(s, input, "repair_cost")) + intComponent(s, addition, "repair_cost");
    anvil.repairItemCountCost = 0;
    if (!addition.empty()) {
        const bool usingBook = hasComponent(s, addition, "stored_enchantments");
        if (damageable(s, result) && s.items.repairableWith(input, addition.item)) {
            const int maxDamage = maxDamageOf(s, result);
            int damage = result.damage, repairAmount = std::min(damage, maxDamage / 4);
            if (repairAmount <= 0) return fail();
            int count = 0;
            for (; repairAmount > 0 && count < addition.count; ++count) {
                damage -= repairAmount;
                ++price;
                repairAmount = std::min(damage, maxDamage / 4);
            }
            setInt(s, result, "damage", damage);
            anvil.repairItemCountCost = count;
        } else {
            if (!usingBook && (result.item != addition.item || !damageable(s, result))) return fail();
            if (damageable(s, result) && !usingBook) {
                const int remaining1 = maxDamageOf(s, input) - input.damage, remaining2 = maxDamageOf(s, addition) - addition.damage;
                const int remaining = remaining1 + remaining2 + maxDamageOf(s, result) * 12 / 100;
                const int resultDamage = std::max(0, maxDamageOf(s, result) - remaining);
                if (resultDamage < result.damage) {
                    setInt(s, result, "damage", resultDamage);
                    price += 2;
                }
            }
            bool anyCompatible = false, anyNotCompatible = false;
            for (const auto& [enchantment, addLevel] : enchantsOf(s, addition)) {
                const int current = levelOf(enchantments, enchantment);
                int level = current == addLevel ? addLevel + 1 : std::max(addLevel, current);
                bool compatible = s.items.enchantmentSupports(enchantment, input.item);
                if (s.creative() || isEnchantedBook(s, input)) compatible = true;
                for (const auto& [other, otherLevel] : enchantments)
                    if (other != enchantment && !s.items.enchantmentsCompatible(enchantment, other)) compatible = false, ++price;
                if (!compatible) {
                    anyNotCompatible = true;
                    continue;
                }
                anyCompatible = true;
                const auto& defs = s.items.enchantments();
                const bool known = enchantment >= 0 && size_t(enchantment) < defs.size();
                level = std::min(level, known ? defs[size_t(enchantment)].maxLevel : level);
                setLevel(enchantments, enchantment, level);
                int fee = known ? defs[size_t(enchantment)].anvilCost : 1;
                if (usingBook) fee = std::max(1, fee / 2);
                price += fee * level;
                if (input.count > 1) price = 40;
            }
            if (anyNotCompatible && !anyCompatible) return fail();
        }
    }
    const std::string& itemName = anvil.sent;  // AnvilMenu.itemName (null before the screen set it)
    const bool blank = std::all_of(itemName.begin(), itemName.end(), [](char ch) { return ch == ' ' || (ch >= 9 && ch <= 13); });
    if (anvil.sentValid && !blank) {
        if (itemName != hoverNameOf(s, input)) {
            namingCost = 1, price += namingCost;
            net::Writer w;  // Component.literal: a plain text component is a network NBT string
            w.buf.push_back(8);
            w.u16v(u16(itemName.size()));
            w.bytes(itemName.data(), itemName.size());
            const std::string data(w.buf.begin(), w.buf.end());
            s.items.setComponent(result, "custom_name", &data);
        }
    } else if (input.hasCustomName) {
        namingCost = 1, price += namingCost;
        s.items.setComponent(result, "custom_name", nullptr);
    }
    cost = price <= 0 ? 0 : int(std::clamp<i64>(tax + price, 0, INT32_MAX));
    if (price <= 0) result = ItemStack{};
    if (namingCost == price && namingCost > 0) {
        if (cost >= 40) cost = 39;
        anvil.onlyRenaming = true;
    }
    if (cost >= 40 && !s.creative()) result = ItemStack{};
    if (!result.empty()) {
        int baseCost = std::max(intComponent(s, result, "repair_cost"), intComponent(s, addition, "repair_cost"));
        if (namingCost != price || namingCost == 0) baseCost = increasedRepairCost(baseCost);
        setInt(s, result, "repair_cost", baseCost);
        setEnchants(s, result, enchantments);
    }
    m.slots[2] = result;
    m.data[0] = cost;
}

void anvilDrawBg(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    anvilSync(s, m);
    anvilPredict(s, m);
    s.xpDisplayStart = s.guiTicks;  // AnvilScreen.containerTick: the experience bar stays up while the anvil is open
    const bool has0 = !m.slots[0].empty();
    blitSprite(c, sp(s, has0 ? "container/anvil/text_field" : "container/anvil/text_field_disabled"), left + 59, top + 20, 110, 16);
    if ((has0 || !m.slots[1].empty()) && m.slots[2].empty())
        blitSprite(c, sp(s, "container/anvil/error"), left + 99, top + 45, 28, 21);
    // EditBox (no border, white text with shadow) at (62, 24), 103 wide.
    s.drawTextField(c, anvil.name, left + 62, top + 24, 103, 0xFFFFFFFF, has0, true);
}

void anvilDrawFg(UiState& s, Canvas& c, Menu& m, const Layout& l, float left, float top) {  // extractLabels
    const int cost = m.data[0];
    if (cost <= 0) return;
    u32 color = 0xFF80FF20;
    Text line;
    bool show = true;
    if (cost >= 40 && !s.creative()) {
        line = Text(s.res.translate("container.repair.expensive"));
        color = 0xFFFF6060;
    } else if (m.slots[2].empty()) {
        show = false;
    } else {
        line = translatable("container.repair.cost", {Text(std::to_string(cost))}, s.lang());
        if (!(s.creative() || s.xpLevel >= cost)) color = 0xFFFF6060;  // the result's mayPickup
    }
    if (!show) return;
    const int tx = l.width - 8 - s.font.width(line) - 2;
    c.fill(left + float(tx - 2), top + 67, left + float(l.width - 8), top + 79, 0x4F000000);
    s.drawText(c, line, left + float(tx), top + 69, color, true);
}

bool anvilInput(UiState& s, const InputEvent& e, Menu& m, const Layout&, float, float) {
    if (m.slots[0].empty()) return false;  // not editable: keys go to the screen (E closes)
    if (e.type == InputEvent::Button && e.down && e.code == 0 && s.textFieldHit(anvil.name)) {  // EditBox.onClick
        s.textFieldMouseDown(anvil.name);
        return true;
    }
    if (e.type == InputEvent::Key && e.down && e.code == VK_ESCAPE) return false;
    if (e.type == InputEvent::Char || (e.type == InputEvent::Key && e.down)) {
        const std::string before = anvil.name.value;
        s.textFieldKey(anvil.name, e);
        if (anvil.name.value != before) anvilSendName(s, m);
        return e.type == InputEvent::Char || e.code != VK_ESCAPE;  // canConsumeInput: every key
    }
    return false;
}

int anvilQuickMove(UiState&, const Menu&, int index, const ItemStack&, const std::function<bool(int, int, bool)>& moveTo) {
    return combinerQuickMove(2, index, moveTo);
}
bool anvilMayPickup(UiState& s, const Menu& m, int slot) {  // AnvilMenu.mayPickup: the level cost (any in creative)
    return slot != 2 || ((s.creative() || s.xpLevel >= m.data[0]) && m.data[0] > 0);
}
void anvilOnTake(UiState& s, Menu& m) {  // AnvilMenu.onTake: the item and the addition (the repair count of it) are used
    if (!s.creative()) s.xpLevel = std::max(0, s.xpLevel - m.data[0]);  // giveExperienceLevels(-cost)
    m.slots[0] = ItemStack{};
    if (anvil.repairItemCountCost > 0) {
        ItemStack& addition = m.slots[1];
        if (!addition.empty() && addition.count > anvil.repairItemCountCost) addition.count -= anvil.repairItemCountCost;
        else addition = ItemStack{};
    } else if (!anvil.onlyRenaming) {
        m.slots[1] = ItemStack{};
    }
    m.data[0] = 0;
}

const StationRegistrar kAnvil({"anvil"}, [] {
    StationDef d;
    d.layout = anvilLayout, d.opened = anvilOpened, d.drawBg = anvilDrawBg, d.drawFg = anvilDrawFg, d.input = anvilInput;
    d.resultSlot = 2, d.quickMove = anvilQuickMove, d.mayPickup = anvilMayPickup, d.onTake = anvilOnTake;
    return d;
}());

// --- grindstone ------------------------------------------------------------------------------------------------

void grindLayout(const Menu&, Layout& l) {
    l.background = "gui/container/grindstone";
    l.slots = {{49, 19}, {49, 40}, {129, 34}};
    playerSlots(l, 3);
}
// GrindstoneMenu.computeResult: one enchanted item loses its non-curse enchantments; two of the same item combine their
// durability (+5%) and enchantments, then lose the non-curses.
ItemStack grindRemoveNonCurses(UiState& s, ItemStack item) {
    Enchants kept;
    for (const auto& e : enchantsOf(s, item))
        if (s.items.enchantmentIsCurse(e.first)) kept.push_back(e);
    setEnchants(s, item, kept);
    if (isEnchantedBook(s, item) && kept.empty()) {  // transmuteCopy(BOOK)
        item.item = s.items.id("minecraft:book");
        s.items.setComponent(item, "stored_enchantments", nullptr);
    }
    int repairCost = 0;
    for (size_t i = 0; i < kept.size(); ++i) repairCost = increasedRepairCost(repairCost);
    setInt(s, item, "repair_cost", repairCost);
    return item;
}
ItemStack grindResult(UiState& s, const ItemStack& input, const ItemStack& additional) {
    if (input.empty() && additional.empty()) return {};
    if ((!input.empty() && input.count > 1) || (!additional.empty() && additional.count > 1)) return {};
    if (input.empty() || additional.empty()) {
        const ItemStack& item = input.empty() ? additional : input;
        return item.enchantList.empty() && item.storedEnchants.empty() ? ItemStack{} : grindRemoveNonCurses(s, item);
    }
    if (input.item != additional.item) return {};  // mergeItems
    const int durability = std::max(maxDamageOf(s, input), maxDamageOf(s, additional));
    const int remaining = (maxDamageOf(s, input) - input.damage) + (maxDamageOf(s, additional) - additional.damage) + durability * 5 / 100;
    int count = 1;
    if (!damageable(s, input)) {
        if (s.items.maxStack(input) < 2 || !(input.count == additional.count && input.sameItemSameComponents(additional))) return {};
        count = 2;
    }
    ItemStack item = input.withCount(count);
    if (damageable(s, item)) {
        setInt(s, item, "max_damage", durability);
        setInt(s, item, "damage", std::max(durability - remaining, 0));
    }
    Enchants merged = enchantsOf(s, item);  // mergeEnchantsFrom: upgrade (curses only where absent)
    for (const auto& [e, level] : enchantsOf(s, additional))
        if (!s.items.enchantmentIsCurse(e) || levelOf(merged, e) == 0) setLevel(merged, e, std::max(levelOf(merged, e), level));
    setEnchants(s, item, merged);
    return grindRemoveNonCurses(s, item);
}

void grindDrawBg(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    static std::string predicted;
    if (const std::string key = inputsKey(m, 2); key != predicted) predicted = key, m.slots[2] = grindResult(s, m.slots[0], m.slots[1]);
    if ((!m.slots[0].empty() || !m.slots[1].empty()) && m.slots[2].empty())
        blitSprite(c, sp(s, "container/grindstone/error"), left + 92, top + 31, 28, 21);
}
// GrindstoneMenu: damageable or enchanted items in; quick moves go into the inputs only while one is empty.
bool grindMayPlace(UiState& s, const Menu&, int slot, const ItemStack& st) {
    if (slot > 1) return true;
    const bool damageable = hasComponent(s, st, "max_damage") && !hasComponent(s, st, "unbreakable") && hasComponent(s, st, "damage");
    return damageable || !st.enchantList.empty() || !st.storedEnchants.empty();
}
int grindQuickMove(UiState&, const Menu& m, int index, const ItemStack&, const std::function<bool(int, int, bool)>& moveTo) {
    if (index == 2) return moveTo(3, 39, true) ? 1 : 0;
    if (index == 0 || index == 1) return moveTo(3, 39, false) ? 1 : 0;
    if (!m.slots[0].empty() && !m.slots[1].empty()) {
        if (index >= 3 && index < 30) return moveTo(30, 39, false) ? 1 : 0;
        if (index >= 30 && index < 39) return moveTo(3, 30, false) ? 1 : 0;
        return 1;
    }
    return moveTo(0, 2, false) ? 1 : 0;
}
const StationRegistrar kGrindstone({"grindstone"}, [] {
    StationDef d;
    d.layout = grindLayout, d.drawBg = grindDrawBg, d.resultSlot = 2;
    d.mayPlace = grindMayPlace, d.quickMove = grindQuickMove;
    d.onTake = [](UiState&, Menu& m) { m.slots[0] = m.slots[1] = ItemStack{}; };
    return d;
}());

// --- smithing table --------------------------------------------------------------------------------------------

struct SmithingState {
    struct Cycle {
        std::vector<std::string> icons;
        int tick = 0, index = 0;
    } cycles[3];
    int lastTick = 0;
} smithing;

bool isTemplate(const std::string& n) { return n.ends_with("_smithing_template"); }

void smithingLayout(const Menu&, Layout& l) {
    l.background = "gui/container/smithing";
    l.slots = {{8, 48}, {26, 48}, {44, 48}, {98, 48}};
    playerSlots(l, 4);
    l.titleX = 44, l.titleY = 15;
}

void smithingOpened(UiState& s, Menu&) {
    smithing = SmithingState{};
    smithing.lastTick = s.guiTicks;
}

void smithingTick(UiState& s, Menu& m) {  // SmithingScreen.containerTick: CyclingSlotBackground.tick
    const std::string t = itemName(s, m.slots[0]);
    std::vector<std::string> base, add;
    if (t == "netherite_upgrade_smithing_template") {
        base = {"helmet", "sword", "chestplate", "pickaxe", "leggings", "axe", "boots", "hoe", "shovel", "nautilus_armor", "spear"};
        add = {"ingot"};
    } else if (isTemplate(t)) {
        base = {"helmet", "chestplate", "leggings", "boots"};
        add = {"ingot", "redstone_dust", "lapis_lazuli", "quartz", "diamond", "emerald", "amethyst_shard"};
    }
    const std::vector<std::string> lists[3] = {{"smithing_template_armor_trim", "smithing_template_netherite_upgrade"}, base, add};
    for (int i = 0; i < 3; ++i) {
        auto& cy = smithing.cycles[i];
        if (cy.icons != lists[i]) cy.icons = lists[i], cy.index = 0;
        if (!cy.icons.empty() && ++cy.tick % 30 == 0) cy.index = (cy.index + 1) % int(cy.icons.size());
    }
}

void smithingDrawBg(UiState& s, Canvas& c, Menu& m, const Layout& l, float left, float top) {
    for (int guard = 0; smithing.lastTick < s.guiTicks && guard < 40; ++guard) ++smithing.lastTick, smithingTick(s, m);
    smithing.lastTick = s.guiTicks;
    {  // the armour stand preview of the result, 25 px per block in (121, 20) - (161, 80)
        PreviewEntity pe;
        if (m.slots.size() > 3 && previewSmithingArmorStand(s, m.slots[3], pe))
            drawEntityFollowsMouse(s, c, pe, int(left) + 121, int(top) + 20, int(left) + 161, int(top) + 80, 25.0f, 0, s.mouseX, s.mouseY);
    }
    if (m.data[0] != 0) blitSprite(c, sp(s, "container/smithing/error"), left + 65, top + 46, 28, 21);
    for (int i = 0; i < 3; ++i) {  // CyclingSlotBackground.extractRenderState
        const auto& cy = smithing.cycles[i];
        if (cy.icons.empty() || !m.slots[size_t(i)].empty()) continue;
        const float x = left + float(l.slots[size_t(i)].x), y = top + float(l.slots[size_t(i)].y);
        const bool transition = cy.icons.size() > 1 && cy.tick >= 30;
        const float alpha = transition ? std::min(float(cy.tick % 30) + s.partialTick, 4.0f) / 4.0f : 1.0f;
        if (alpha < 1.0f) {
            const int prev = (cy.index - 1 + int(cy.icons.size())) % int(cy.icons.size());
            blitSprite(c, sp(s, "container/slot/" + cy.icons[size_t(prev)]), x, y, 16, 16, withAlpha(0xFFFFFF, 1.0f - alpha));
        }
        blitSprite(c, sp(s, "container/slot/" + cy.icons[size_t(cy.index)]), x, y, 16, 16, withAlpha(0xFFFFFF, alpha));
    }
}

void smithingDrawTop(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {  // onboarding tooltips
    std::string key;
    if (m.data[0] != 0 && inRect(s.mouseX, s.mouseY, left + 65, top + 46, 28, 21)) key = "container.upgrade.error_tooltip";
    const int hs = s.hoveredSlot;
    if (hs >= 0 && hs < 4) {
        const std::string t = itemName(s, m.slots[0]);
        if (t.empty()) {
            if (hs == 0) key = "container.upgrade.missing_template_tooltip";
        } else if (isTemplate(t) && m.slots[size_t(hs)].empty()) {
            const std::string kind = t == "netherite_upgrade_smithing_template" ? "netherite_upgrade" : "armor_trim";
            if (hs == 1) key = "item.minecraft.smithing_template." + kind + ".base_slot_description";
            if (hs == 2) key = "item.minecraft.smithing_template." + kind + ".additions_slot_description";
        }
    }
    if (!key.empty()) s.drawTooltip(c, s.font.split(Text(s.res.translate(key)), 115), s.mouseX, s.mouseY);
}

// SmithingMenu: the server's recipe property sets for the template, base and addition slots.
const char* const kSmithingSets[3] = {"minecraft:smithing_template", "minecraft:smithing_base", "minecraft:smithing_addition"};
bool smithingMayPlace(UiState&, const Menu&, int slot, const ItemStack& st) {
    return slot > 2 || inPropertySet(kSmithingSets[slot], st.item);
}

const StationRegistrar kSmithing({"smithing"}, [] {
    StationDef d;
    d.layout = smithingLayout, d.opened = smithingOpened, d.drawBg = smithingDrawBg, d.drawTop = smithingDrawTop;
    d.resultSlot = 3, d.mayPlace = smithingMayPlace, d.pickAll = 0;
    d.quickMove = [](UiState&, const Menu& m, int index, const ItemStack& st, const std::function<bool(int, int, bool)>& moveTo) {
        bool into = false;  // canMoveIntoInputSlots: a set the stack belongs to, with that slot empty
        for (int i = 0; i < 3; ++i) into |= inPropertySet(kSmithingSets[i], st.item) && m.slots[size_t(i)].empty();
        return combinerQuickMove(3, index, moveTo, into);
    };
    d.onTake = [](UiState&, Menu& m) {  // shrinkStackInSlot 0, 1, 2
        for (int i = 0; i < 3; ++i)
            if (!m.slots[size_t(i)].empty()) m.slots[size_t(i)] = m.slots[size_t(i)].withCount(m.slots[size_t(i)].count - 1);
    };
    return d;
}());

// --- enchanting table ------------------------------------------------------------------------------------------

// java.util.Random / LegacyRandomSource (EnchantmentNames seeds it with the menu's enchantment seed every frame).
struct JavaRandom {
    u64 seed = 0;
    void setSeed(i64 s) { seed = (u64(s) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1); }
    int next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return int(i64(seed) >> (48 - bits));
    }
    int nextInt(int bound) {
        if ((bound & (bound - 1)) == 0) return int((i64(bound) * i64(next(31))) >> 31);
        int sample, modulo;
        do {
            sample = next(31);
            modulo = sample % bound;
        } while (sample - modulo + (bound - 1) < 0);
        return modulo;
    }
};

const char* kWords[] = {"the",      "elder",   "scrolls",  "klaatu",   "berata",    "niktu",    "xyzzy",   "bless",   "curse",
                        "light",    "darkness", "fire",    "air",      "earth",     "water",    "hot",     "dry",     "cold",
                        "wet",      "ignite",  "snuff",    "embiggen", "twist",     "shorten",  "stretch", "fiddle",  "destroy",
                        "imbue",    "galvanize", "enchant", "free",    "limited",   "range",    "of",      "towards", "inside",
                        "sphere",   "cube",    "self",     "other",    "ball",      "mental",   "physical", "grow",   "shrink",
                        "demon",    "elemental", "spirit", "animal",   "creature",  "beast",    "humanoid", "undead", "fresh",
                        "stale",    "phnglui", "mglwnafh", "cthulhu",  "rlyeh",     "wgahnagl", "fhtagn",  "baguette"};

struct EnchantState {
    JavaRandom names;
    // Book (EnchantmentScreen.tickBook).
    JavaRandom random;
    float flip = 0, oFlip = 0, flipT = 0, flipA = 0, open = 0, oOpen = 0;
    int lastTick = 0;
    ItemStack last;  // the stack the book last saw (ItemStack.matches: item, count and components)
    bool seen = false;
    // Enchantment registry (network ids: the registry sorted by name).
    bool registryLoaded = false;
    struct Ench {
        std::string name;
        int maxLevel = 1;
        bool curse = false;
    };
    std::vector<Ench> registry;
} ench;

void loadEnchantments(UiState& s) {
    if (ench.registryLoaded) return;
    ench.registryLoaded = true;
    std::vector<std::string> names;
    for (const std::string& p : s.res.list("data/minecraft/enchantment/"))
        if (p.ends_with(".json")) names.push_back(p.substr(p.rfind('/') + 1, p.size() - p.rfind('/') - 6));
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::vector<std::string> curses;
    const nlohmann::json tag = s.res.json("data/minecraft/tags/enchantment/curse.json");
    if (tag.is_object() && tag.contains("values"))
        for (const auto& v : tag["values"])
            if (v.is_string()) curses.push_back(v.get<std::string>().substr(v.get<std::string>().find(':') + 1));
    for (const std::string& n : names) {
        EnchantState::Ench e;
        e.name = n;
        const nlohmann::json j = s.res.json("data/minecraft/enchantment/" + n + ".json");
        if (j.is_object()) e.maxLevel = j.value("max_level", 1);
        e.curse = std::find(curses.begin(), curses.end(), n) != curses.end();
        ench.registry.push_back(e);
    }
}

// Enchantment.getFullname: the name (gray, curses red) and the level in roman numerals unless the max level is 1.
Text enchantFullName(UiState& s, int id, int level) {
    loadEnchantments(s);
    if (id < 0 || size_t(id) >= ench.registry.size()) return Text("?");
    const auto& e = ench.registry[size_t(id)];
    std::string name = s.res.translate("enchantment.minecraft." + e.name);
    if (level != 1 || e.maxLevel != 1) {
        const std::string key = "enchantment.level." + std::to_string(level);
        const std::string lv = s.res.translate(key);
        name += " " + (lv == key ? std::to_string(level) : lv);
    }
    return Text(name, colorStyle(e.curse ? 0xFF5555 : 0xAAAAAA));
}

void enchantLayout(const Menu&, Layout& l) {
    l.background = "gui/container/enchanting_table";
    l.slots = {{15, 47}, {35, 47}};
    playerSlots(l, 2);
}

void enchantOpened(UiState& s, Menu& m) {
    const bool regLoaded = ench.registryLoaded;
    auto reg = std::move(ench.registry);
    ench.flip = ench.oFlip = ench.flipT = ench.flipA = ench.open = ench.oOpen = 0;
    ench.seen = true, ench.last = ItemStack{}, ench.lastTick = s.guiTicks;
    ench.random.setSeed(i64(std::llround(s.clock * 1000.0)) ^ 0x1234567);
    ench.registryLoaded = regLoaded, ench.registry = std::move(reg);
    for (int i = 4; i < 10; ++i) m.data[size_t(i)] = -1;  // enchantClue / levelClue default -1 until the server sends them
}

void tickBook(Menu& m) {
    const ItemStack& cur = m.slots[0];
    const bool same = ench.seen && (cur.empty() ? ench.last.empty()
                                                : !ench.last.empty() && cur.count == ench.last.count && cur.sameItemSameComponents(ench.last));
    if (!same) {
        ench.last = cur, ench.seen = true;
        do {  // nextInt(4) - nextInt(4), left to right
            const int first = ench.random.nextInt(4);
            ench.flipT += float(first - ench.random.nextInt(4));
        } while (ench.flip <= ench.flipT + 1.0f && ench.flip >= ench.flipT - 1.0f);
    }
    ench.oFlip = ench.flip, ench.oOpen = ench.open;
    const bool shouldBeOpen = m.data[0] != 0 || m.data[1] != 0 || m.data[2] != 0;
    ench.open = std::clamp(ench.open + (shouldBeOpen ? 0.2f : -0.2f), 0.0f, 1.0f);
    const float diff = std::clamp((ench.flipT - ench.flip) * 0.4f, -0.2f, 0.2f);
    ench.flipA += (diff - ench.flipA) * 0.9f;
    ench.flip += ench.flipA;
}

// The book (GuiBookModelRenderer): the game's book model, posed by BookModel.setupAnim, projected like the
// picture-in-picture renderer (40 px per block, origin 17 px below the box top).
struct M34 {
    float m[12];
};
M34 mul(const M34& a, const M34& b) {
    M34 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            float v = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j];
            if (j == 3) v += a.m[i * 4 + 3];
            r.m[i * 4 + j] = v;
        }
    return r;
}
M34 translate(float x, float y, float z) { return {{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z}}; }
M34 rotX(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{1, 0, 0, 0, 0, c, -s, 0, 0, s, c, 0}};
}
M34 rotY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0}};
}
M34 rotZ(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {{c, -s, 0, 0, s, c, 0, 0, 0, 0, 1, 0}};
}

void drawBook(UiState& s, Canvas& c, float left, float top) {
    if (!s.entities) return;
    const ent::EntityAssets& a = s.entities->assets();
    const int mi = a.model("book#main");
    const Sprite* tex = s.sprites.texture("entity/enchantment/enchanting_table_book");
    if (mi < 0 || !tex) return;
    const float pt = s.partialTick;
    const float open = ench.oOpen + (ench.open - ench.oOpen) * pt, flip = ench.oFlip + (ench.flip - ench.oFlip) * pt;
    auto frac = [](float v) { return v - std::floor(v); };
    const float pf1 = std::clamp(frac(flip + 0.25f) * 1.6f - 0.3f, 0.0f, 1.0f);
    const float pf2 = std::clamp(frac(flip + 0.75f) * 1.6f - 0.3f, 0.0f, 1.0f);
    const float openness = 1.25f * open;  // BookModel.State.forAnimation(0, ...)
    const float x0 = left + 14, y0 = top + 14, x1 = x0 + 38, y1 = y0 + 31;
    const float cx = (x0 + x1) / 2.0f, cy = y0 + 17.0f, size = 40.0f;
    const float deg = kPi / 180.0f;
    M34 root = mul(rotY(180 * deg), rotX(25 * deg));
    root = mul(root, translate((1 - open) * 0.2f, (1 - open) * 0.1f, (1 - open) * 0.25f));
    root = mul(root, rotY((-(1 - open) * 90.0f - 90.0f) * deg));
    root = mul(root, rotX(180 * deg));
    const ent::Model& model = a.modelAt(mi);
    struct Face {
        float xy[4][2], uv[4][2], depth;
        u32 color;
    };
    std::vector<Face> faces;
    std::vector<M34> mats(model.parts.size());
    for (size_t i = 0; i < model.parts.size(); ++i) {
        const ent::ModelPart& part = model.parts[i];
        ent::PartPose p = part.pose;
        const std::string& n = part.name;
        if (n == "left_lid") p.yRot = kPi + openness;
        else if (n == "right_lid") p.yRot = -openness;
        else if (n == "left_pages") p.yRot = openness, p.x = std::sin(openness);
        else if (n == "right_pages") p.yRot = -openness, p.x = std::sin(openness);
        else if (n == "flip_page1") p.yRot = openness - openness * 2.0f * pf1, p.x = std::sin(openness);
        else if (n == "flip_page2") p.yRot = openness - openness * 2.0f * pf2, p.x = std::sin(openness);
        M34 l = translate(p.x / 16.0f, p.y / 16.0f, p.z / 16.0f);
        if (p.zRot != 0) l = mul(l, rotZ(p.zRot));
        if (p.yRot != 0) l = mul(l, rotY(p.yRot));
        if (p.xRot != 0) l = mul(l, rotX(p.xRot));
        mats[i] = mul(part.parent >= 0 ? mats[size_t(part.parent)] : root, l);
        for (u32 q = part.quadFirst; q < part.quadFirst + part.quadCount; ++q) {
            const ent::GpuQuad& g = a.quads()[q];
            const M34& m = mats[i];
            float nrm[3];
            for (int k = 0; k < 3; ++k) nrm[k] = m.m[k * 4] * g.normal[0] + m.m[k * 4 + 1] * g.normal[1] + m.m[k * 4 + 2] * g.normal[2];
            const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            if (len > 0) nrm[0] /= len, nrm[1] /= len, nrm[2] /= len;
            if (nrm[2] >= -1e-4f) continue;
            Face f;
            float depth = 0;
            for (int k = 0; k < 4; ++k) {
                const float pp[3] = {g.pos[k][0] / 16.0f, g.pos[k][1] / 16.0f, g.pos[k][2] / 16.0f};
                float o[3];
                for (int r = 0; r < 3; ++r) o[r] = m.m[r * 4] * pp[0] + m.m[r * 4 + 1] * pp[1] + m.m[r * 4 + 2] * pp[2] + m.m[r * 4 + 3];
                f.xy[k][0] = cx + o[0] * size, f.xy[k][1] = cy + o[1] * size;
                depth += o[2];
                f.uv[k][0] = tex->u0 + g.uv[k][0] * (tex->u1 - tex->u0);
                f.uv[k][1] = tex->v0 + g.uv[k][1] * (tex->v1 - tex->v0);
            }
            f.depth = depth / 4;
            static const float L0[3] = {0.16f, -0.79f, -0.59f}, L1[3] = {-0.16f, -0.79f, -0.59f};
            const float d0 = std::max(0.0f, nrm[0] * L0[0] + nrm[1] * L0[1] + nrm[2] * L0[2]);
            const float d1 = std::max(0.0f, nrm[0] * L1[0] + nrm[1] * L1[1] + nrm[2] * L1[2]);
            f.color = scaleRgb(0xFFFFFFFF, std::min(1.0f, 0.4f + 0.6f * (d0 + d1)));
            faces.push_back(f);
        }
    }
    std::stable_sort(faces.begin(), faces.end(), [](const Face& l, const Face& r) { return l.depth > r.depth; });
    c.pushScissor(x0, y0, x1, y1);
    for (const Face& f : faces) c.quad4(f.xy, f.uv, f.color, ModeGui);
    c.popScissor();
}

// EnchantmentNames: the standard galactic alphabet (minecraft:alt font).
Style altStyle() {
    static const int font = fontIndex({FontDesc::Resource, "minecraft:alt"});
    Style st;
    st.font = font;
    return st;
}

std::string randomName(UiState& s, int maxWidth) {  // EnchantmentNames.getRandomName
    std::string r;
    const int words = ench.names.nextInt(2) + 3;
    for (int i = 0; i < words; ++i) {
        if (i) r += " ";
        r += kWords[ench.names.nextInt(int(std::size(kWords)))];
    }
    float w = 0;  // the widest prefix in the alt font (font.substrByWidth)
    size_t end = 0;
    for (size_t i = 0; i < r.size();) {
        size_t j = i;
        w += s.font.glyph(nextCodepoint(r, j), altStyle()).advance;
        if (w > float(maxWidth)) break;
        end = i = j;
    }
    return r.substr(0, end);
}

int lapisCount(const Menu& m) { return m.slots.size() > 1 && !m.slots[1].empty() ? m.slots[1].count : 0; }

void enchantDrawBg(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    s.xpDisplayStart = s.guiTicks;  // EnchantmentScreen.containerTick: the experience bar stays up
    for (int guard = 0; ench.lastTick < s.guiTicks && guard < 40; ++guard) ++ench.lastTick, tickBook(m);
    ench.lastTick = s.guiTicks;
    drawBook(s, c, left, top);
    ench.names.setSeed(i64(m.data[3]));
    const int gold = lapisCount(m);
    for (int i = 0; i < 3; ++i) {
        const float lx = left + 60, ly = top + 14 + float(19 * i), tx = lx + 20;
        const int cost = m.data[size_t(i)];
        if (cost == 0) {
            blitSprite(c, sp(s, "container/enchanting_table/enchantment_slot_disabled"), lx, ly, 108, 19);
            continue;
        }
        const std::string costText = std::to_string(cost);
        const int textWidth = 86 - s.font.width(costText);
        const std::string name = randomName(s, textWidth);
        Font& f = s.font;
        u32 col = 0xFF685E4A;
        if ((gold < i + 1 || s.xpLevel < cost) && !s.creative()) {
            blitSprite(c, sp(s, "container/enchanting_table/enchantment_slot_disabled"), lx, ly, 108, 19);
            blitSprite(c, sp(s, "container/enchanting_table/level_" + std::to_string(i + 1) + "_disabled"), lx + 1, ly + 1, 16, 16);
            f.draw(c, Text(name, altStyle()), tx, top + 16 + float(19 * i), 0xFF000000 | ((col & 0xFEFEFE) >> 1), false);
            col = 0xFF407F10;
        } else {
            if (inRect(s.mouseX, s.mouseY, lx, ly, 108, 19)) {
                blitSprite(c, sp(s, "container/enchanting_table/enchantment_slot_highlighted"), lx, ly, 108, 19);
                s.requestCursor(UiState::CursorPointingHand);
                col = 0xFFFFFF80;
            } else {
                blitSprite(c, sp(s, "container/enchanting_table/enchantment_slot"), lx, ly, 108, 19);
            }
            blitSprite(c, sp(s, "container/enchanting_table/level_" + std::to_string(i + 1)), lx + 1, ly + 1, 16, 16);
            f.draw(c, Text(name, altStyle()), tx, top + 16 + float(19 * i), col, false);
            col = 0xFF80FF20;
        }
        s.drawText(c, Text(costText), tx + 86 - float(s.font.width(costText)), top + 16 + float(19 * i) + 7, col, true);
    }
}

void enchantDrawTop(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {  // the clue tooltip
    const int gold = lapisCount(m);
    for (int i = 0; i < 3; ++i) {
        const int minLevel = m.data[size_t(i)], enchId = m.data[size_t(4 + i)], level = m.data[size_t(7 + i)];
        if (enchId < 0) continue;
        if (!inRect(s.mouseX, s.mouseY, left + 59, top + 13 + float(19 * i), 110, 19) || minLevel <= 0 || level < 0) continue;  // isHovering
        const int cost = i + 1;
        std::vector<Text> lines;
        lines.push_back(translatable("container.enchant.clue", {enchantFullName(s, enchId, level)}, s.lang(), colorStyle(0xFFFFFF)));
        if (!s.creative()) {
            lines.push_back(Text(""));
            if (s.xpLevel < minLevel) {
                lines.push_back(translatable("container.enchant.level.requirement", {Text(std::to_string(minLevel))}, s.lang(),
                                             colorStyle(0xFF5555)));
            } else {
                lines.push_back(cost == 1 ? Text(s.res.translate("container.enchant.lapis.one"), colorStyle(gold >= cost ? 0xAAAAAA : 0xFF5555))
                                          : translatable("container.enchant.lapis.many", {Text(std::to_string(cost))}, s.lang(),
                                                         colorStyle(gold >= cost ? 0xAAAAAA : 0xFF5555)));
                lines.push_back(cost == 1 ? Text(s.res.translate("container.enchant.level.one"), colorStyle(0xAAAAAA))
                                          : translatable("container.enchant.level.many", {Text(std::to_string(cost))}, s.lang(),
                                                         colorStyle(0xAAAAAA)));
            }
        }
        s.drawTooltip(c, lines, s.mouseX, s.mouseY);
        break;
    }
}

bool enchantInput(UiState& s, const InputEvent& e, Menu& m, const Layout&, float left, float top) {
    if (e.type != InputEvent::Button || !e.down) return false;
    for (int i = 0; i < 3; ++i) {
        if (!inRect(s.mouseX, s.mouseY, left + 60, top + 14 + float(19 * i), 108, 19)) continue;
        // EnchantmentMenu.clickMenuButton (client side): an offer, enough lapis and levels.
        const int cost = m.data[size_t(i)];
        const bool ok = cost > 0 && !m.slots[0].empty() &&
                        (s.creative() || (lapisCount(m) >= i + 1 && s.xpLevel >= cost));
        if (!ok) return false;
        sendContainerButton(s, i);
        return true;
    }
    return false;
}

bool enchantMayPlace(UiState& s, const Menu&, int slot, const ItemStack& st) {
    if (slot == 1) return itemName(s, st) == "lapis_lazuli";
    return true;
}

int enchantQuickMove(UiState& s, const Menu&, int index, const ItemStack& st, const std::function<bool(int, int, bool)>& moveTo) {
    if (index < 2) return moveTo(2, 38, true) ? 1 : 0;
    if (itemName(s, st) == "lapis_lazuli") return moveTo(1, 2, true) ? 1 : 0;
    return moveTo(0, 1, false) ? 1 : 0;  // one item into the empty item slot (it holds 1)
}

const StationRegistrar kEnchanting({"enchantment"}, [] {
    StationDef d;
    d.layout = enchantLayout, d.opened = enchantOpened, d.drawBg = enchantDrawBg, d.drawTop = enchantDrawTop;
    d.input = enchantInput, d.mayPlace = enchantMayPlace, d.quickMove = enchantQuickMove;
    d.slotMax = [](UiState&, const Menu&, int slot, const ItemStack&) { return slot == 0 ? 1 : 0; };
    return d;
}());

}  // namespace
}  // namespace ui
