// Screens: player inventory and server containers (AbstractContainerScreen + AbstractContainerMenu click
// logic), the creative item list, pause and options menus, chat input.
#include "audio/audio.h"
#include "render/renderer.h"
#include "ui/entity_preview.h"
#include "ui/frontend.h"
#include "ui/mp_hud.h"
#include "ui/ui_state.h"
#include "world/terrain.h"

#include <cmath>

namespace ui {

namespace {
constexpr u32 kLabel = 0xFF404040;

std::string shortName(const std::string& n) { return n.substr(n.find(':') + 1); }

void playerSlots(Layout& l, int start, int mainY, int hotbarY) {
    l.playerStart = start;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 9; ++c) l.slots.push_back({8 + c * 18, mainY + r * 18});
    for (int c = 0; c < 9; ++c) l.slots.push_back({8 + c * 18, hotbarY});
}

int equipmentSlot(const std::string& n) {  // inventory menu armor slot for an item (5 head .. 8 feet), -1 none
    if (n.ends_with("_helmet") || n == "turtle_helmet" || n == "carved_pumpkin" || n.ends_with("_head") || n.ends_with("_skull"))
        return 5;
    if (n.ends_with("_chestplate") || n == "elytra") return 6;
    if (n.ends_with("_leggings")) return 7;
    if (n.ends_with("_boots")) return 8;
    return -1;
}

bool isFuel(const std::string& n) {
    static const char* fuels[] = {"coal", "charcoal", "coal_block", "lava_bucket", "blaze_rod", "stick", "dried_kelp_block",
                                  "bamboo", "scaffolding", "bowl"};
    for (const char* f : fuels)
        if (n == f) return true;
    return n.ends_with("_log") || n.ends_with("_planks") || n.ends_with("_wood") || n.ends_with("_sapling") ||
           n.ends_with("_stem") || n.ends_with("_hyphae") || n.ends_with("_slab") && n.find("wood") != std::string::npos ||
           n.ends_with("_boat") || n.ends_with("_fence") || n.ends_with("_wool") || n.ends_with("_carpet");
}
}  // namespace

Layout layoutFor(const Menu& m) {
    Layout l;
    const std::string& t = m.typeName;
    if (m.type >= 0)  // a station screen (modules.h)
        if (const StationDef* d = stationFor(t); d && d->layout) {
            d->layout(m, l);
            return l;
        }
    if (m.type < 0) {
        l.background = "gui/container/inventory";
        l.slots.push_back({154, 28});
        for (int i = 0; i < 4; ++i) l.slots.push_back({98 + (i % 2) * 18, 18 + (i / 2) * 18});
        for (int i = 0; i < 4; ++i) l.slots.push_back({8, 8 + i * 18});
        playerSlots(l, 9, 84, 142);
        l.slots.push_back({77, 62});
        l.titleX = 97;
        l.invLabelY = -1000;  // InventoryScreen draws no inventory label
    } else if (t.starts_with("generic_9x") || t == "shulker_box") {
        const int rows = t == "shulker_box" ? 3 : t.back() - '0';
        l.background = t == "shulker_box" ? "gui/container/shulker_box" : "gui/container/generic_54";
        l.chestRows = t != "shulker_box";
        l.rows = rows;
        l.height = 114 + rows * 18;
        for (int i = 0; i < rows * 9; ++i) l.slots.push_back({8 + (i % 9) * 18, 18 + (i / 9) * 18});
        const int yo = (rows - 4) * 18;
        playerSlots(l, rows * 9, 103 + yo, 161 + yo);
        if (t == "shulker_box") l.height = 166, l.slots.resize(27), playerSlots(l, 27, 84, 142);
        l.invLabelY = l.height - 94;
    } else if (t == "crafting") {
        l.background = "gui/container/crafting_table";
        l.slots.push_back({124, 35});
        for (int i = 0; i < 9; ++i) l.slots.push_back({30 + (i % 3) * 18, 17 + (i / 3) * 18});
        playerSlots(l, 10, 84, 142);
        l.titleX = 29;
    } else if (t == "furnace" || t == "smoker" || t == "blast_furnace") {
        l.background = "gui/container/" + t;
        l.slots = {{56, 17}, {56, 53}, {116, 35}};
        playerSlots(l, 3, 84, 142);
        l.titleX = -1;  // centred
    } else if (t == "generic_3x3" || t == "crafter_3x3") {
        l.background = "gui/container/dispenser";
        for (int i = 0; i < 9; ++i) l.slots.push_back({62 + (i % 3) * 18, 17 + (i / 3) * 18});
        playerSlots(l, 9, 84, 142);
        l.titleX = -1;
    } else if (t == "hopper") {
        l.background = "gui/container/hopper";
        l.height = 133;
        for (int i = 0; i < 5; ++i) l.slots.push_back({44 + i * 18, 20});
        playerSlots(l, 5, 51, 109);
        l.invLabelY = l.height - 94;
    } else {
        // Other menus: their own slots as a plain grid (slot count from the server), the player inventory below.
        const int n = std::max(0, int(m.slots.size()) - 36);
        const int rows = std::clamp((n + 8) / 9, 1, 6);
        l.background = "gui/container/generic_54";
        l.chestRows = true;
        l.rows = rows;
        l.height = 114 + rows * 18;
        for (int i = 0; i < n; ++i) l.slots.push_back({8 + (i % 9) * 18, 18 + std::min(i / 9, 5) * 18});
        const int yo = (rows - 4) * 18;
        playerSlots(l, n, 103 + yo, 161 + yo);
        l.invLabelY = l.height - 94;
    }
    return l;
}

// --- click simulation (AbstractContainerMenu.doClick) -------------------------------------------------------

namespace {
struct Sim {
    Ui::State& s;
    Menu& m;
    ItemStack& carried;
    bool creative;
    const StationDef* station = nullptr;  // modules.h
    int resultSlot = -1;          // crafting / furnace output
    std::vector<int> craftGrid;   // consumed when the result is taken

    int maxStack(const ItemStack& st) const { return s.items.maxStack(st); }
    std::string name(const ItemStack& st) const { return shortName(s.items.name(st.item)); }
    bool mayPlace(int slot, const ItemStack& st) const {
        if (slot == resultSlot) return false;
        if (station && station->mayPlace && slot < int(m.slots.size()) - 36 && !station->mayPlace(s, m, slot, st)) return false;
        if (m.type < 0 && slot >= 5 && slot <= 8) return equipmentSlot(name(st)) == slot;
        return true;
    }
    int slotMax(int slot, const ItemStack& st) const {
        if (m.type < 0 && slot >= 5 && slot <= 8) return 1;
        return std::min(64, maxStack(st));
    }
    ItemStack& at(int i) { return m.slots[size_t(i)]; }
    void takeResult(int slot) {
        if (slot != resultSlot || craftGrid.empty()) return;
        for (int g : craftGrid)
            if (!at(g).empty()) at(g) = at(g).withCount(at(g).count - 1);
    }
    // Slot.safeInsert
    ItemStack safeInsert(int slot, ItemStack st, int amount) {
        if (st.empty() || !mayPlace(slot, st)) return st;
        ItemStack& cur = at(slot);
        const int n = std::min(std::min(amount, st.count), slotMax(slot, st) - (cur.empty() ? 0 : cur.count));
        if (n <= 0) return st;
        if (cur.empty()) cur = st.withCount(n);
        else if (cur.sameItemSameComponents(st)) cur.count += n;
        else return st;
        return st.withCount(st.count - n);
    }
    // Slot.tryRemove (results only all-or-nothing)
    ItemStack tryRemove(int slot, int amount, int room) {
        ItemStack& cur = at(slot);
        if (cur.empty()) return {};
        if (slot == resultSlot && cur.count > room) return {};
        const int n = std::min(std::min(amount, room), cur.count);
        ItemStack taken = cur.withCount(n);
        cur = cur.withCount(cur.count - n);
        if (slot == resultSlot) takeResult(slot), cur = ItemStack{};
        return taken;
    }
    bool moveTo(ItemStack& st, int start, int end, bool backwards) {
        bool changed = false;
        if (maxStack(st) > 1) {
            for (int i = backwards ? end - 1 : start; !st.empty() && (backwards ? i >= start : i < end); i += backwards ? -1 : 1) {
                ItemStack& t = at(i);
                if (t.empty() || !t.sameItemSameComponents(st)) continue;
                const int total = t.count + st.count, mx = slotMax(i, t);
                if (total <= mx) {
                    st.count = 0, t.count = total, changed = true;
                } else if (t.count < mx) {
                    st.count -= mx - t.count, t.count = mx, changed = true;
                }
            }
        }
        if (st.count > 0)
            for (int i = backwards ? end - 1 : start; backwards ? i >= start : i < end; i += backwards ? -1 : 1) {
                if (!at(i).empty() || !mayPlace(i, st)) continue;
                const int n = std::min(st.count, slotMax(i, st));
                at(i) = st.withCount(n);
                st.count -= n;
                changed = true;
                break;
            }
        if (st.count <= 0) st = ItemStack{};
        return changed;
    }
    ItemStack quickMove(int index) {
        if (at(index).empty()) return {};
        ItemStack st = at(index);
        const ItemStack clicked = st;
        const int n = int(m.slots.size());
        const std::string& t = m.typeName;
        bool ok = true;
        if (m.type < 0) {
            const int eq = equipmentSlot(name(st));
            if (index == 0) ok = moveTo(st, 9, 45, true);
            else if (index >= 1 && index < 9) ok = moveTo(st, 9, 45, false);
            else if (eq >= 0 && at(eq).empty()) ok = moveTo(st, eq, eq + 1, false);
            else if (name(st) == "shield" && at(45).empty()) ok = moveTo(st, 45, 46, false);
            else if (index >= 9 && index < 36) ok = moveTo(st, 36, 45, false);
            else if (index >= 36 && index < 45) ok = moveTo(st, 9, 36, false);
            else ok = moveTo(st, 9, 45, false);
        } else if (t == "crafting") {
            if (index == 0) ok = moveTo(st, 10, 46, true);
            else if (index >= 10 && index < 46) {
                if (!moveTo(st, 1, 10, false)) ok = index < 37 ? moveTo(st, 37, 46, false) : moveTo(st, 10, 37, false);
            } else ok = moveTo(st, 10, 46, false);
        } else if (t == "furnace" || t == "smoker" || t == "blast_furnace") {
            if (index == 2) ok = moveTo(st, 3, 39, true);
            else if (index != 0 && index != 1) {
                if (isFuel(name(st))) ok = moveTo(st, 1, 2, false);
                else ok = moveTo(st, 0, 1, false);
                if (!ok) ok = index < 30 ? moveTo(st, 30, 39, false) : moveTo(st, 3, 30, false);
            } else ok = moveTo(st, 3, 39, false);
        } else {
            int r = -1;
            if (station && station->quickMove)
                r = station->quickMove(s, m, index, clicked, [&](int a, int b, bool back) { return moveTo(st, a, b, back); });
            if (r >= 0) {
                ok = r == 1;
            } else {
                const int own = n - 36;
                ok = index < own ? moveTo(st, own, n, true) : moveTo(st, 0, own, false);
            }
        }
        if (!ok) return {};
        at(index) = st;
        if (index == resultSlot) takeResult(index), at(index) = ItemStack{};
        if (st.count == clicked.count) return {};
        return clicked;
    }
    bool canQuickReplace(int slot, const ItemStack& st) {
        const ItemStack& cur = at(slot);
        if (cur.empty()) return true;
        return cur.sameItemSameComponents(st) && cur.count <= maxStack(st);
    }
};
}  // namespace

static int qcStatus = 0, qcType = 0;
static int deathScreenTick = 0;  // DeathScreen.delayTicker start (guiTicks)
void openAdvancementsFromPause(UiState& s);  // advancements.cpp
void openStatsFromPause(UiState& s);          // stats.cpp
static std::vector<int> qcSlots;

void Ui::State::containerClick(int slot, int button, int input) {
    Menu& m = activeMenu();
    const std::vector<ItemStack> before = m.slots;
    const ItemStack carriedBefore = carried;
    Sim sim{*this, m, carried, creative()};
    if (m.type < 0) sim.resultSlot = 0, sim.craftGrid = {1, 2, 3, 4};
    else if (m.typeName == "crafting") sim.resultSlot = 0, sim.craftGrid = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    else if (m.typeName == "furnace" || m.typeName == "smoker" || m.typeName == "blast_furnace") sim.resultSlot = 2;
    if (m.type >= 0)
        if (const StationDef* d = stationFor(m.typeName)) sim.station = d, sim.resultSlot = d->resultSlot;
    const int n = int(m.slots.size());
    auto valid = [&](int i) { return i >= 0 && i < n; };

    enum { Pickup, QuickMove, Swap, Clone, Throw, QuickCraft, PickupAll };
    if (input == QuickCraft) {
        const int expected = qcStatus;
        qcStatus = button & 3;
        if ((expected != 1 || qcStatus != 2) && expected != qcStatus) {
            qcStatus = 0, qcSlots.clear();
        } else if (carried.empty()) {
            qcStatus = 0, qcSlots.clear();
        } else if (qcStatus == 0) {
            qcType = (button >> 2) & 3;
            if (qcType == 0 || qcType == 1 || (qcType == 2 && creative())) qcStatus = 1, qcSlots.clear();
            else qcStatus = 0;
        } else if (qcStatus == 1) {
            if (valid(slot) && sim.canQuickReplace(slot, carried) && sim.mayPlace(slot, carried) &&
                (qcType == 2 || carried.count > int(qcSlots.size())) &&
                std::find(qcSlots.begin(), qcSlots.end(), slot) == qcSlots.end())
                qcSlots.push_back(slot);
        } else if (qcStatus == 2) {
            if (qcSlots.size() == 1) {
                const int only = qcSlots[0];
                qcStatus = 0, qcSlots.clear();
                // doClick(slot, type, PICKUP): handled by the generic path below
                slot = only, button = qcType, input = Pickup;
            } else if (!qcSlots.empty()) {
                const ItemStack source = carried;
                int remaining = carried.count;
                for (int q : qcSlots) {
                    if (!sim.canQuickReplace(q, carried) || !sim.mayPlace(q, carried)) continue;
                    if (!(qcType == 2 || carried.count >= int(qcSlots.size()))) continue;
                    const int carry = m.slots[size_t(q)].empty() ? 0 : m.slots[size_t(q)].count;
                    const int maxSize = std::min(sim.maxStack(source), sim.slotMax(q, source));
                    const int per = qcType == 0 ? source.count / int(qcSlots.size()) : qcType == 1 ? 1 : sim.maxStack(source);
                    const int newCount = std::min(per + carry, maxSize);
                    remaining -= newCount - carry;
                    m.slots[size_t(q)] = source.withCount(newCount);
                }
                carried = source.withCount(remaining);
                qcStatus = 0, qcSlots.clear();
            } else {
                qcStatus = 0;
            }
        } else {
            qcStatus = 0, qcSlots.clear();
        }
    }
    if (input != QuickCraft && qcStatus != 0) {
        qcStatus = 0, qcSlots.clear();
    } else if ((input == Pickup || input == QuickMove) && (button == 0 || button == 1)) {
        if (slot == -999) {
            if (!carried.empty()) carried = button == 0 ? ItemStack{} : carried.withCount(carried.count - 1);
        } else if (input == QuickMove) {
            if (valid(slot)) {
                ItemStack moved = sim.quickMove(slot);
                for (int guard = 0; !moved.empty() && guard < 64 && m.slots[size_t(slot)].item == moved.item; ++guard)
                    moved = sim.quickMove(slot);
            }
        } else if (valid(slot)) {
            ItemStack clicked = m.slots[size_t(slot)];
            if (clicked.empty()) {
                if (!carried.empty()) carried = sim.safeInsert(slot, carried, button == 0 ? carried.count : 1);
            } else if (carried.empty()) {
                const int amount = button == 0 ? clicked.count : (clicked.count + 1) / 2;
                carried = sim.tryRemove(slot, amount, 1 << 30);
            } else if (sim.mayPlace(slot, carried)) {
                if (clicked.sameItemSameComponents(carried)) {
                    carried = sim.safeInsert(slot, carried, button == 0 ? carried.count : 1);
                } else if (carried.count <= sim.slotMax(slot, carried)) {
                    std::swap(carried, m.slots[size_t(slot)]);
                }
            } else if (clicked.sameItemSameComponents(carried)) {
                ItemStack taken = sim.tryRemove(slot, clicked.count, sim.maxStack(carried) - carried.count);
                if (!taken.empty()) carried.count += taken.count;
            }
        }
    } else if (input == Swap && valid(slot) && ((button >= 0 && button < 9) || button == 40)) {
        const int src = menuSlotForInventory(m, button);
        if (valid(src)) {
            ItemStack& a = m.slots[size_t(src)];
            ItemStack& b = m.slots[size_t(slot)];
            if (a.empty() && !b.empty()) {
                a = b, b = ItemStack{};
                if (slot == sim.resultSlot) sim.takeResult(slot);
            } else if (!a.empty() && b.empty()) {
                if (sim.mayPlace(slot, a)) {
                    const int mx = sim.slotMax(slot, a);
                    if (a.count > mx) b = a.withCount(mx), a = a.withCount(a.count - mx);
                    else b = a, a = ItemStack{};
                }
            } else if (!a.empty() && !b.empty() && sim.mayPlace(slot, a) && a.count <= sim.slotMax(slot, a)) {
                std::swap(a, b);
            }
        }
    } else if (input == Clone && creative() && carried.empty() && valid(slot)) {
        const ItemStack& st = m.slots[size_t(slot)];
        if (!st.empty()) carried = st.withCount(sim.maxStack(st));
    } else if (input == Throw && carried.empty() && valid(slot)) {
        ItemStack& st = m.slots[size_t(slot)];
        if (!st.empty()) {
            if (slot == sim.resultSlot) sim.takeResult(slot), st = ItemStack{};
            else st = button == 0 ? st.withCount(st.count - 1) : ItemStack{};
        }
    } else if (input == PickupAll && valid(slot) && !carried.empty()) {
        const int start = button == 0 ? 0 : n - 1, step = button == 0 ? 1 : -1;
        for (int pass = 0; pass < 2; ++pass)
            for (int i = start; i >= 0 && i < n && carried.count < sim.maxStack(carried); i += step) {
                ItemStack& t = m.slots[size_t(i)];
                if (t.empty() || i == sim.resultSlot || !t.sameItemSameComponents(carried)) continue;
                if (pass == 0 && t.count == sim.maxStack(t)) continue;
                const int take = std::min(t.count, sim.maxStack(carried) - carried.count);
                carried.count += take;
                t = t.withCount(t.count - take);
            }
    }

    if (!online()) return;
    // ServerboundContainerClickPacket with the slots our prediction changed.
    net::Writer w;
    w.varInt(m.containerId);
    w.varInt(m.stateId);
    w.u16v(u16(i16(slot)));
    w.u8v(u8(i8(button)));
    w.varInt(input);
    std::vector<int> changed;
    for (int i = 0; i < n && changed.size() < 128; ++i) {
        const ItemStack& a = before[size_t(i)];
        const ItemStack& b = m.slots[size_t(i)];
        if (a.empty() != b.empty() || (!a.empty() && (a.count != b.count || !a.sameItemSameComponents(b)))) changed.push_back(i);
    }
    w.varInt(i32(changed.size()));
    for (int i : changed) {
        w.u16v(u16(i));
        items.writeHashed(w, m.slots[size_t(i)]);
    }
    items.writeHashed(w, carried);
    client->sendPlay("container_click", w);
    (void)carriedBefore;
}

void Ui::State::creativeSetSlot(int menuSlot, const ItemStack& st) {
    if (menuSlot >= 0 && menuSlot < int(inventory.slots.size())) inventory.slots[size_t(menuSlot)] = st;
    if (!online()) return;
    net::Writer w;
    w.u16v(u16(i16(menuSlot)));
    items.write(w, st, true);
    client->sendPlay("set_creative_mode_slot", w);
}

// --- buttons ------------------------------------------------------------------------------------------------

void Ui::State::buildButtons() {
    buttons.clear();
    const int W = canvas ? guiW() : int(window->width()) / std::max(1, guiScale());
    const int H = canvas ? guiH() : int(window->height()) / std::max(1, guiScale());
    auto tr = [&](const char* key) { return Text(res.translate(key)); };
    if (screen == Screen::Mods) return buildModsButtons();  // mods_screen.cpp
    if (screen == Screen::PackOptions) return buildPackButtons();  // render/packs/pack_screen.cpp
    if (frontEndScreen()) return buildFrontEndButtons();     // title.cpp
    if (const ScreenDef* d = screenDef(screen); d && d->build) return d->build(*this);  // modules.h
    if (screen == Screen::Pause) {
        const int gridW = 212, gridH = 50 + 20 + 4 * 24;
        const int left = (W - gridW) / 2, top = int(float(H - gridH) * 0.25f);
        auto full = [&](int y, Text label, std::function<void()> fn, bool active = true) {
            Button b;
            b.x = float(left + 4), b.y = float(y), b.w = 204, b.label = std::move(label), b.onPress = std::move(fn), b.active = active;
            buttons.push_back(std::move(b));
        };
        auto half = [&](int col, int y, Text label, std::function<void()> fn, bool active = true) {
            Button b;
            b.x = float(left + 4 + col * 106), b.y = float(y), b.w = 98, b.label = std::move(label), b.onPress = std::move(fn);
            b.active = active;
            buttons.push_back(std::move(b));
        };
        int y = top + 50;
        full(y, tr("menu.returnToGame"), [this] { closeScreen(); });
        y += 24;
        half(0, y, tr("gui.advancements"), [this] { openAdvancementsFromPause(*this); });  // advancements.cpp
        half(1, y, tr("gui.stats"), [this] { openStatsFromPause(*this); });  // stats.cpp
        y += 24;
        full(y, Text("Mods & Packs"), [this] { openScreen(Screen::Mods); });  // mods_screen.cpp
        y += 24;
        half(0, y, tr("menu.options"), [this] { openScreen(Screen::Options); });
        if (fe->cfg.singleplayer)  // title.cpp
            half(1, y, tr("menu.shareToLan"), [this] { openToLanScreen(*this); }, fe->lanOpenPort == 0);
        else
            half(1, y, tr("options.worldOptions.button"), nullptr, false);
        y += 24;
        full(y, tr(online() && !fe->cfg.singleplayer ? "menu.disconnect" : "menu.returnToMenu"), [this] {
            fe->titleMenu = online() && !fe->cfg.singleplayer ? "servers" : "";
            fe->titleRequested = true;
            quit = true;
        });
    } else if (screen == Screen::Death) {  // DeathScreen: the buttons wake up after 20 ticks (init resets the delay)
        deathScreenTick = guiTicks;
        Button respawn;
        respawn.x = float(W / 2 - 100), respawn.y = float(H / 4 + 72);
        respawn.label = tr(hardcore ? "deathScreen.spectate" : "deathScreen.respawn");
        respawn.active = false;
        respawn.onPress = [this] {  // the screen closes when the server's respawn arrives (ui.cpp)
            if (online()) {
                net::Writer w;
                w.varInt(0);  // ServerboundClientCommandPacket.Action.PERFORM_RESPAWN
                client->sendPlay("client_command", w);
                deathScreenTick = INT32_MAX;  // pressed: stays inactive
                buttons[0].active = false;
            } else {
                deathMessage = Text();
                closeScreen();
            }
        };
        buttons.push_back(std::move(respawn));
        Button title;
        title.x = float(W / 2 - 100), title.y = float(H / 4 + 96), title.label = tr("deathScreen.titleScreen");
        title.active = false;
        title.onPress = [this] {
            fe->titleRequested = true;
            quit = true;
        };
        buttons.push_back(std::move(title));
    }
}

void Ui::State::drawButtons(Canvas& c) {
    for (size_t i = 0; i < buttons.size(); ++i) drawButton(c, i);
}

void Ui::State::drawButton(Canvas& c, size_t i, bool hoverable) {
    Button& b = buttons[i];
    const bool hover = hoverable && mouseX >= b.x && mouseY >= b.y && mouseX < b.x + b.w && mouseY < b.y + b.h;
    if (b.slider) {  // AbstractSliderButton: the handle lights up under the mouse (an inactive one never)
        blitSprite(c, sprites.sprite("widget/slider"), b.x, b.y, b.w, b.h);
        const float hx = b.x + float(int(b.value * (b.w - 8)));
        const bool lit = b.active && (hover || draggingSlider == int(i));
        blitSprite(c, sprites.sprite(lit ? "widget/slider_handle_highlighted" : "widget/slider_handle"), hx, b.y, 8, 20);
    } else {
        const char* sp = !b.active ? "widget/button_disabled" : hover ? "widget/button_highlighted" : "widget/button";
        blitSprite(c, sprites.sprite(sp), b.x, b.y, b.w, b.h);
    }
    const Text label = b.labelFn ? b.labelFn() : b.label;
    const u32 color = b.active ? 0xFFFFFFFF : 0xFFA0A0A0;
    drawCentered(c, label, b.x + b.w / 2, b.y + (b.h - 8) / 2, color);
}

bool Ui::State::buttonsInput(const Window::InputEvent& e, size_t first, size_t last) {
    if (e.type != Window::InputEvent::Button || e.code != 0) return false;
    if (!e.down) {
        if (draggingSlider >= 0) settings.save();
        draggingSlider = -1;
        return false;
    }
    for (size_t i = first; i < buttons.size() && i < last; ++i) {
        Button& b = buttons[i];
        if (!b.active || mouseX < b.x || mouseY < b.y || mouseX >= b.x + b.w || mouseY >= b.y + b.h) continue;
        if (b.slider) {
            draggingSlider = int(i);
            b.value = std::clamp((mouseX - (b.x + 4)) / (b.w - 8), 0.f, 1.f);
            if (b.onSlide) b.onSlide(b.value);
        } else if (b.onPress) {
            audio::playUiClick();  // ui.button.click, like the game's buttons
            auto fn = b.onPress;  // may rebuild the button list
            fn();
        }
        return true;
    }
    return false;
}

// --- screen drawing ------------------------------------------------------------------------------------------

void Ui::State::drawScreen(Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    // Slider dragging.
    if (draggingSlider >= 0 && size_t(draggingSlider) < buttons.size()) {
        Button& b = buttons[size_t(draggingSlider)];
        b.value = std::clamp((mouseX - (b.x + 4)) / (b.w - 8), 0.f, 1.f);
        if (b.onSlide) b.onSlide(b.value);
    }
    if (frontEndScreen()) return drawFrontEnd(c);  // title.cpp
    if (const ScreenDef* d = screenDef(screen); d && d->draw) return d->draw(*this, c);  // modules.h
    switch (screen) {
        case Screen::Chat: {
            drawChat(c, true);
            c.fill(2, float(H - 14), float(W - 2), float(H - 2), withAlpha(0, 0.5f));
            drawTextField(c, chatInput, 4, float(H - 12 + 2), float(W - 8), 0xFFE0E0E0, true, true);
            chatExtDraw(*this, c);  // mp_hud.h: command suggestions, hovered chat components
            return;
        }
        case Screen::Mods: drawModsScreen(c); return;  // mods_screen.cpp
        case Screen::PackOptions: drawPackScreen(c); return;  // render/packs/pack_screen.cpp
        case Screen::Death: {
            c.gradient(0, 0, float(W), float(H), 0x60500000, 0xA0803030);
            if (guiTicks - deathScreenTick >= 20 && deathScreenTick != INT32_MAX)
                for (Button& b : buttons) b.active = true;
            else if (deathScreenTick == INT32_MAX && buttons.size() > 1)
                buttons[1].active = true;
            c.push();
            c.scaleBy(2);
            drawCentered(c, Text(res.translate(hardcore ? "deathScreen.title.hardcore" : "deathScreen.title")), float(W / 2 / 2), 30, 0xFFFFFFFF);
            c.pop();
            if (!deathMessage.plain().empty()) drawCentered(c, deathMessage, float(W / 2), 85, 0xFFFFFFFF);
            drawCentered(c, translatable("deathScreen.score.value", {Text(std::to_string(xpTotal), colorStyle(0xFFFF55))}, lang()),
                         float(W / 2), 100, 0xFFFFFFFF);
            drawButtons(c);
            return;
        }
        case Screen::Pause: {
            c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);
            drawCentered(c, Text(res.translate("menu.game")), float(W / 2), 40.f, 0xFFFFFFFF);
            drawButtons(c);
            return;
        }
        default: break;
    }
    c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);

    // The creative inventory is creative.cpp (a registered screen).

    // Inventory / container.
    Menu& m = activeMenu();
    const Layout l = layoutFor(m);
    if (m.slots.size() < l.slots.size()) m.slots.resize(l.slots.size());
    const float left = float((W - l.width) / 2), top = float((H - l.height) / 2);
    const Sprite* bg = l.background.empty() ? nullptr : sprites.texture(l.background);
    if (!bg) {
    } else if (l.chestRows) {
        blitTexture(c, bg, left, top, 0, 0, float(l.width), float(l.rows * 18 + 17), 256, 256);
        blitTexture(c, bg, left, top + float(l.rows * 18 + 17), 0, 126, float(l.width), 96, 256, 256);
    } else {
        blitTexture(c, bg, left, top, 0, 0, float(l.width), float(l.height), l.texW, l.texH);
    }
    const StationDef* station = m.type >= 0 ? stationFor(m.typeName) : nullptr;  // modules.h
    if (m.typeName == "furnace" || m.typeName == "smoker" || m.typeName == "blast_furnace") {
        const std::string dir = "container/" + m.typeName + "/";
        const int litTime = m.data[0], litDuration = m.data[1], progress = m.data[2], total = m.data[3];
        if (litTime > 0) {
            const float lit = std::clamp(float(litTime) / float(litDuration ? litDuration : 200), 0.f, 1.f);
            const int h = int(std::ceil(lit * 13.0f)) + 1;
            blitSpriteRegion(c, sprites.sprite(dir + "lit_progress"), 14, 14, 0, 14 - h, left + 56, top + 36 + float(14 - h), 14, float(h));
        }
        const float burn = total != 0 && progress != 0 ? std::clamp(float(progress) / float(total), 0.f, 1.f) : 0.f;
        const int w = int(std::ceil(burn * 24.0f));
        blitSpriteRegion(c, sprites.sprite(dir + "burn_progress"), 24, 16, 0, 0, left + 79, top + 34, float(w), 16);
    }
    if (m.type < 0) {  // InventoryScreen: the player follows the mouse
        PreviewEntity pe;
        if (previewLocalPlayer(*this, pe))
            drawEntityFollowsMouse(*this, c, pe, int(left) + 26, int(top) + 8, int(left) + 75, int(top) + 78, 30, 0.0625f, mouseX, mouseY);
    }
    if (station && station->drawBg) station->drawBg(*this, c, m, l, left, top);
    // Slots.
    hoveredSlot = -1;
    for (size_t i = 0; i < l.slots.size(); ++i) {
        const float sx = left + float(l.slots[i].x), sy = top + float(l.slots[i].y);
        if (mouseX >= sx - 1 && mouseX < sx + 17 && mouseY >= sy - 1 && mouseY < sy + 17) hoveredSlot = int(i);
    }
    // Dragging with a carried stack (mouseDragged): collect slots for quick crafting.
    if (isQuickCrafting && hoveredSlot >= 0 && !carried.empty() && size_t(hoveredSlot) < m.slots.size() &&
        std::find(quickCraftSlots.begin(), quickCraftSlots.end(), hoveredSlot) == quickCraftSlots.end() &&
        (carried.count > int(quickCraftSlots.size()) || quickCraftType == 2)) {
        const ItemStack& cur = m.slots[size_t(hoveredSlot)];
        const bool resultSlot = (m.type < 0 || m.typeName == "crafting") ? hoveredSlot == 0
                                : (m.typeName == "furnace" || m.typeName == "smoker" || m.typeName == "blast_furnace") && hoveredSlot == 2;
        if (!resultSlot && (cur.empty() || (cur.sameItemSameComponents(carried) && cur.count <= items.maxStack(carried))))
            quickCraftSlots.push_back(hoveredSlot);
    }
    if (hoveredSlot >= 0)
        blitSprite(c, sprites.sprite("container/slot_highlight_back"), left + float(l.slots[size_t(hoveredSlot)].x) - 4,
                   top + float(l.slots[size_t(hoveredSlot)].y) - 4, 24, 24);
    // Armor slot placeholders and the offhand shield (InventoryMenu empty slot icons).
    static const char* kEmpty[4] = {"container/slot/helmet", "container/slot/chestplate", "container/slot/leggings", "container/slot/boots"};
    for (size_t i = 0; i < l.slots.size() && i < m.slots.size(); ++i) {
        const float sx = left + float(l.slots[i].x), sy = top + float(l.slots[i].y);
        if (m.type < 0 && m.slots[i].empty()) {
            if (i >= 5 && i <= 8) blitSprite(c, sprites.sprite(kEmpty[i - 5]), sx, sy, 16, 16);
            if (i == 45) blitSprite(c, sprites.sprite("container/slot/shield"), sx, sy, 16, 16);
        }
        ItemStack shown = m.slots[i];
        const char* countOverride = nullptr;
        std::string cnt;
        // Quick-craft (drag) preview: what each dragged slot will hold.
        if (isQuickCrafting && !carried.empty() && std::find(quickCraftSlots.begin(), quickCraftSlots.end(), int(i)) != quickCraftSlots.end() &&
            quickCraftSlots.size() > 1) {
            const int carry = shown.empty() ? 0 : shown.count;
            const int per = quickCraftType == 0 ? carried.count / int(quickCraftSlots.size()) : quickCraftType == 1 ? 1 : items.maxStack(carried);
            const int mx = items.maxStack(carried);
            const int nc = std::min(per + carry, mx);
            shown = carried.withCount(nc);
            if (nc == mx) cnt = "\xC2\xA7" "e" + std::to_string(nc), countOverride = cnt.c_str();
        }
        drawItem(c, shown, sx, sy, true, countOverride);
    }
    if (hoveredSlot >= 0)
        blitSprite(c, sprites.sprite("container/slot_highlight_front"), left + float(l.slots[size_t(hoveredSlot)].x) - 4,
                   top + float(l.slots[size_t(hoveredSlot)].y) - 4, 24, 24);
    // Labels.
    const Text title = m.type < 0 ? Text(res.translate("container.crafting")) : m.title;
    const float tx = l.titleX >= 0 ? float(l.titleX) : float((l.width - font.width(title)) / 2);
    if (l.titleY > -1000) drawText(c, title, left + tx, top + float(l.titleY), kLabel, false);
    if (l.invLabelY > -1000) drawText(c, Text(res.translate("container.inventory")), left + float(l.invLabelX), top + float(l.invLabelY), kLabel, false);
    if (station && station->drawFg) station->drawFg(*this, c, m, l, left, top);
    // Carried item, then the tooltip.
    if (!carried.empty()) {
        std::string remaining;
        if (isQuickCrafting && quickCraftSlots.size() > 1) {
            int left2 = carried.count;
            for (int q : quickCraftSlots) {
                const int carry = m.slots[size_t(q)].empty() ? 0 : m.slots[size_t(q)].count;
                const int per = quickCraftType == 0 ? carried.count / int(quickCraftSlots.size()) : quickCraftType == 1 ? 1 : items.maxStack(carried);
                left2 -= std::min(per + carry, items.maxStack(carried)) - carry;
            }
            remaining = left2 <= 0 ? "\xC2\xA7" "e0" : std::to_string(left2);
        }
        drawItem(c, carried, mouseX - 8, mouseY - 8, true, remaining.empty() ? nullptr : remaining.c_str());
    } else if (hoveredSlot >= 0 && size_t(hoveredSlot) < m.slots.size() && !m.slots[size_t(hoveredSlot)].empty()) {
        drawTooltip(c, items.tooltip(m.slots[size_t(hoveredSlot)], settings.advancedItemTooltips), mouseX, mouseY);
    }
    if (station && station->drawTop) station->drawTop(*this, c, m, l, left, top);
}

// --- screen input -------------------------------------------------------------------------------------------

void Ui::State::screenInput(const Window::InputEvent& e) {
    using E = Window::InputEvent;
    const bool shift = window->keyDown(VK_SHIFT);
    if (const ScreenDef* d = screenDef(screen); d && d->input) return d->input(*this, e);  // modules.h
    if (screen == Screen::Chat) {
        if (chatExtInput(*this, e)) return;  // mp_hud.h: suggestions list, chat clicks
        if (e.type == E::Key && e.down) {
            if (e.code == VK_ESCAPE) return closeScreen();
            if (e.code == VK_RETURN) {
                const std::string text = chatInput.value;
                closeScreen();
                sendChat(text);
                return;
            }
            if (e.code == VK_UP || e.code == VK_DOWN) {  // ChatScreen.moveInHistory
                if (sentHistory.empty()) return;
                if (historyPos < 0) historyDraft = chatInput.value, historyPos = int(sentHistory.size());
                historyPos = std::clamp(historyPos + (e.code == VK_UP ? -1 : 1), 0, int(sentHistory.size()));
                chatInput.value = historyPos == int(sentHistory.size()) ? historyDraft : sentHistory[size_t(historyPos)];
                chatInput.cursor = chatInput.selection = int(chatInput.value.size());
                return;
            }
            if (e.code == VK_PRIOR || e.code == VK_NEXT) {
                chatScroll = std::clamp(chatScroll + (e.code == VK_PRIOR ? 19 : -19), 0, std::max(0, int(chatLines.size()) - 20));
                return;
            }
        }
        if (e.type == E::Wheel) {
            chatScroll = std::clamp(chatScroll + (e.code > 0 ? 1 : -1) * (shift ? 1 : 7), 0, std::max(0, int(chatLines.size()) - 20));
            return;
        }
        textFieldKey(chatInput, e);
        return;
    }
    if (screen == Screen::Death) {
        buttonsInput(e);
        return;
    }
    if (screen == Screen::Mods) return modsInput(e);  // mods_screen.cpp
    if (screen == Screen::PackOptions) return packInput(e);  // render/packs/pack_screen.cpp
    if (frontEndScreen()) return frontEndInput(e);     // title.cpp
    if (screen == Screen::Pause) {
        if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return closeScreen();
        buttonsInput(e);
        return;
    }

    // Inventory / containers (AbstractContainerScreen).
    Menu& m = activeMenu();
    const Layout l = layoutFor(m);
    const float left = float((guiW() - l.width) / 2), top = float((guiH() - l.height) / 2);
    if (m.type >= 0)  // station widgets first (text fields take E)
        if (const StationDef* d = stationFor(m.typeName); d && d->input && d->input(*this, e, m, l, left, top)) return;
    auto slotAt = [&](float x, float y) {
        for (size_t i = 0; i < l.slots.size(); ++i) {
            const float sx = left + float(l.slots[i].x), sy = top + float(l.slots[i].y);
            if (x >= sx - 1 && x < sx + 17 && y >= sy - 1 && y < sy + 17) return int(i);
        }
        return -1;
    };
    const int slot = slotAt(mouseX, mouseY);
    const bool outside = mouseX < left || mouseY < top || mouseX >= left + l.width || mouseY >= top + l.height;
    enum { Pickup, QuickMove, Swap, Clone, Throw, QuickCraft, PickupAll };
    if (e.type == E::Key && e.down) {
        if (e.code == VK_ESCAPE || isKey(Key::Inventory, e)) return closeScreen();
        if (carried.empty() && slot >= 0) {  // checkHotbarKeyPressed
            for (int i = 0; i < 9; ++i)
                if (isKey(Key(int(Key::Hotbar1) + i), e)) return containerClick(slot, i, Swap);
            if (isKey(Key::SwapOffhand, e)) return containerClick(slot, 40, Swap);
        }
        if (slot >= 0 && size_t(slot) < m.slots.size() && !m.slots[size_t(slot)].empty() && isKey(Key::Drop, e))
            return containerClick(slot, window->keyDown(VK_CONTROL) ? 1 : 0, Throw);
        return;
    }
    if (e.type == E::Button && e.down) {
        const bool cloning = e.code == 2 && creative();
        const int slotId = outside ? -999 : slot;
        const double now = clock;
        doubleClick = slot == lastClickSlot && e.code == lastClickButton && now - lastClickTime < 0.25;
        lastClickTime = now, lastClickButton = e.code;
        skipNextRelease = false;
        if (e.code != 0 && e.code != 1 && !cloning) {
            lastClickSlot = slot;
            return;
        }
        if (slotId != -1 && !isQuickCrafting) {
            if (carried.empty()) {
                if (cloning) {
                    containerClick(slotId, e.code, Clone);
                } else {
                    const bool quick = slotId != -999 && shift;
                    int input = Pickup;
                    if (quick) {
                        lastQuickMoved = slot >= 0 ? m.slots[size_t(slot)] : ItemStack{};
                        input = QuickMove;
                    } else if (slotId == -999) {
                        input = Throw;
                    }
                    containerClick(slotId, e.code, input);
                }
                skipNextRelease = true;
            } else {
                isQuickCrafting = true;
                quickCraftButton = e.code;
                quickCraftSlots.clear();
                quickCraftType = e.code == 0 ? 0 : e.code == 1 ? 1 : 2;
            }
        }
        lastClickSlot = slot;
        return;
    }
    if (e.type == E::Button && !e.down) {
        const int slotId = outside ? -999 : slot;
        if (doubleClick && slot >= 0 && e.code == 0) {
            if (shift) {
                if (!lastQuickMoved.empty())
                    for (size_t i = 0; i < m.slots.size(); ++i)
                        if (!m.slots[i].empty() && m.slots[i].sameItemSameComponents(lastQuickMoved) &&
                            ((int(i) < l.playerStart) == (slot < l.playerStart)))
                            containerClick(int(i), 0, QuickMove);
            } else {
                containerClick(slot, 0, PickupAll);
            }
            doubleClick = false;
        } else {
            if (isQuickCrafting && quickCraftButton != e.code) {
                isQuickCrafting = false;
                quickCraftSlots.clear();
                skipNextRelease = true;
                return;
            }
            if (skipNextRelease) {
                skipNextRelease = false;
                return;
            }
            if (isQuickCrafting && !quickCraftSlots.empty()) {
                const int type = quickCraftType;
                containerClick(-999, (0 & 3) | ((type & 3) << 2), QuickCraft);
                for (int q : quickCraftSlots) containerClick(q, (1 & 3) | ((type & 3) << 2), QuickCraft);
                containerClick(-999, (2 & 3) | ((type & 3) << 2), QuickCraft);
            } else if (!carried.empty()) {
                if (e.code == 2 && creative()) {
                    containerClick(slotId, e.code, Clone);
                } else {
                    const bool quick = slotId != -999 && shift;
                    if (quick) lastQuickMoved = slot >= 0 ? m.slots[size_t(slot)] : ItemStack{};
                    containerClick(slotId, e.code, quick ? QuickMove : Pickup);
                }
            }
        }
        isQuickCrafting = false;
        quickCraftSlots.clear();
        return;
    }
}

}  // namespace ui
