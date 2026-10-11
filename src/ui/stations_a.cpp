// Station screens: anvil (AnvilScreen), grindstone (GrindstoneScreen), smithing table (SmithingScreen) and
// enchanting table (EnchantmentScreen with its book and the galactic enchantment names), registered as StationDefs
// (modules.h) on the generic container screen.
#include "entity/entities.h"
#include "ui/ui_state.h"

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

// ItemCombinerMenu.quickMoveStack: result -> inventory (backwards), inputs -> inventory, inventory -> inputs.
int combinerQuickMove(int resultSlot, int index, const std::function<bool(int, int, bool)>& moveTo) {
    const int inv = resultSlot + 1, end = inv + 36;
    if (index == resultSlot) return moveTo(inv, end, true) ? 1 : 0;
    if (index < resultSlot) return moveTo(inv, end, false) ? 1 : 0;
    if (moveTo(0, resultSlot, false)) return 1;
    if (index < inv + 27) return moveTo(inv + 27, end, false) ? 1 : 0;
    return moveTo(inv, inv + 27, false) ? 1 : 0;
}

// --- anvil -----------------------------------------------------------------------------------------------------

struct AnvilState {
    TextField name;
    int lastItem = -2;
    std::string lastName, sent;
    bool sentValid = false;
} anvil;

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
    if (anvil.sentValid && newName == anvil.sent) return;
    anvil.sent = newName, anvil.sentValid = true;
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
    anvil.name.value = hn;
    anvil.name.cursor = anvil.name.selection = int(hn.size());
    anvil.name.scroll = 0;
    anvil.name.blinkStart = s.clock;
    anvilSendName(s, m);
}

void anvilDrawBg(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    anvilSync(s, m);
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

const StationRegistrar kAnvil({"anvil"}, [] {
    StationDef d;
    d.layout = anvilLayout, d.opened = anvilOpened, d.drawBg = anvilDrawBg, d.drawFg = anvilDrawFg, d.input = anvilInput;
    d.resultSlot = 2, d.quickMove = anvilQuickMove;
    return d;
}());

// --- grindstone ------------------------------------------------------------------------------------------------

void grindLayout(const Menu&, Layout& l) {
    l.background = "gui/container/grindstone";
    l.slots = {{49, 19}, {49, 40}, {129, 34}};
    playerSlots(l, 3);
}
void grindDrawBg(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    if ((!m.slots[0].empty() || !m.slots[1].empty()) && m.slots[2].empty())
        blitSprite(c, sp(s, "container/grindstone/error"), left + 92, top + 31, 28, 21);
}
const StationRegistrar kGrindstone({"grindstone"}, [] {
    StationDef d;
    d.layout = grindLayout, d.drawBg = grindDrawBg, d.resultSlot = 2;
    d.quickMove = [](UiState&, const Menu&, int index, const ItemStack&, const std::function<bool(int, int, bool)>& moveTo) {
        return combinerQuickMove(2, index, moveTo);
    };
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
    if (!s.carried.empty()) return;
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

bool smithingMayPlace(UiState& s, const Menu&, int slot, const ItemStack& st) {
    const std::string n = itemName(s, st);
    if (slot == 0) return isTemplate(n);
    return true;
}

const StationRegistrar kSmithing({"smithing"}, [] {
    StationDef d;
    d.layout = smithingLayout, d.opened = smithingOpened, d.drawBg = smithingDrawBg, d.drawTop = smithingDrawTop;
    d.resultSlot = 3, d.mayPlace = smithingMayPlace;
    d.quickMove = [](UiState&, const Menu&, int index, const ItemStack&, const std::function<bool(int, int, bool)>& moveTo) {
        return combinerQuickMove(3, index, moveTo);
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
    Font alt;
    bool altTried = false, altOk = false;
    JavaRandom names;
    // Book (EnchantmentScreen.tickBook).
    JavaRandom random;
    float flip = 0, oFlip = 0, flipT = 0, flipA = 0, open = 0, oOpen = 0;
    int lastItem = -2, lastCount = 0, lastTick = 0;
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
    const bool altOk = ench.altOk;
    const bool altTried = ench.altTried;
    const bool regLoaded = ench.registryLoaded;
    auto reg = std::move(ench.registry);
    ench.flip = ench.oFlip = ench.flipT = ench.flipA = ench.open = ench.oOpen = 0;
    ench.lastItem = -2, ench.lastCount = 0, ench.lastTick = s.guiTicks;
    ench.random.setSeed(i64(std::llround(s.clock * 1000.0)) ^ 0x1234567);
    ench.altOk = altOk, ench.altTried = altTried, ench.registryLoaded = regLoaded, ench.registry = std::move(reg);
    for (int i = 4; i < 10; ++i) m.data[size_t(i)] = -1;  // enchantClue / levelClue default -1 until the server sends them
}

void tickBook(Menu& m) {
    const ItemStack& cur = m.slots[0];
    const int item = cur.empty() ? -1 : cur.item, count = cur.empty() ? 0 : cur.count;
    if (item != ench.lastItem || count != ench.lastCount) {
        ench.lastItem = item, ench.lastCount = count;
        do {
            ench.flipT += float(ench.random.nextInt(4) - ench.random.nextInt(4));
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

std::string randomName(UiState& s, int maxWidth) {  // EnchantmentNames.getRandomName
    std::string r;
    const int words = ench.names.nextInt(2) + 3;
    for (int i = 0; i < words; ++i) {
        if (i) r += " ";
        r += kWords[ench.names.nextInt(int(std::size(kWords)))];
    }
    Font& f = ench.altOk ? ench.alt : s.font;
    return f.headByWidth(r, maxWidth);
}

int lapisCount(const Menu& m) { return m.slots.size() > 1 && !m.slots[1].empty() ? m.slots[1].count : 0; }

void enchantDrawBg(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    if (!ench.altTried) {
        ench.altTried = true;
        ench.altOk = ench.alt.load(s.res, s.guiAtlas, "minecraft:alt");
    }
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
        Font& f = ench.altOk ? ench.alt : s.font;
        u32 col = 0xFF685E4A;
        if ((gold < i + 1 || s.xpLevel < cost) && !s.creative()) {
            blitSprite(c, sp(s, "container/enchanting_table/enchantment_slot_disabled"), lx, ly, 108, 19);
            blitSprite(c, sp(s, "container/enchanting_table/level_" + std::to_string(i + 1) + "_disabled"), lx + 1, ly + 1, 16, 16);
            f.draw(c, name, tx, top + 16 + float(19 * i), 0xFF000000 | ((col & 0xFEFEFE) >> 1), false);
            col = 0xFF407F10;
        } else {
            if (inRect(s.mouseX, s.mouseY, lx, ly, 108, 19)) {
                blitSprite(c, sp(s, "container/enchanting_table/enchantment_slot_highlighted"), lx, ly, 108, 19);
                col = 0xFFFFFF80;
            } else {
                blitSprite(c, sp(s, "container/enchanting_table/enchantment_slot"), lx, ly, 108, 19);
            }
            blitSprite(c, sp(s, "container/enchanting_table/level_" + std::to_string(i + 1)), lx + 1, ly + 1, 16, 16);
            f.draw(c, name, tx, top + 16 + float(19 * i), col, false);
            col = 0xFF80FF20;
        }
        s.drawText(c, Text(costText), tx + 86 - float(s.font.width(costText)), top + 16 + float(19 * i) + 7, col, true);
    }
}

void enchantDrawTop(UiState& s, Canvas& c, Menu& m, const Layout&, float left, float top) {  // the clue tooltip
    if (!s.carried.empty()) return;
    const int gold = lapisCount(m);
    for (int i = 0; i < 3; ++i) {
        const int minLevel = m.data[size_t(i)], enchId = m.data[size_t(4 + i)], level = m.data[size_t(7 + i)];
        if (enchId < 0) continue;
        if (!inRect(s.mouseX, s.mouseY, left + 60, top + 14 + float(19 * i), 108, 17) || minLevel <= 0 || level < 0) continue;
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
    return moveTo(0, 1, false) ? 1 : 0;
}

const StationRegistrar kEnchanting({"enchantment"}, [] {
    StationDef d;
    d.layout = enchantLayout, d.opened = enchantOpened, d.drawBg = enchantDrawBg, d.drawTop = enchantDrawTop;
    d.input = enchantInput, d.mayPlace = enchantMayPlace, d.quickMove = enchantQuickMove;
    return d;
}());

}  // namespace
}  // namespace ui
