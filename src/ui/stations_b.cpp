// Station screens, part 2 (modules.h StationDef): villager / wandering trader trading (MerchantScreen), beacon
// (BeaconScreen), brewing stand (BrewingStandScreen), crafter (CrafterScreen) and mount inventories
// (HorseInventoryScreen / NautilusInventoryScreen, opened by mount_screen_open, with the mount following the mouse).
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "entity/entities.h"
#include "ui/item_props.h"
#include "ui/entity_preview.h"
#include "ui/item_ui.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr u32 kLabel = 0xFF404040;
using State = Ui::State;

bool inRect(const State& s, float x, float y, float w, float h) {
    return s.mouseX >= x && s.mouseY >= y && s.mouseX < x + w && s.mouseY < y + h;
}
// AbstractContainerMenu.addStandardInventorySlots(inventory, x, y).
void standardSlots(Layout& l, int x, int y) {
    l.playerStart = int(l.slots.size());
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 9; ++c) l.slots.push_back({x + c * 18, y + r * 18});
    for (int c = 0; c < 9; ++c) l.slots.push_back({x + c * 18, y + 58});
}
std::string shortName(const std::string& n) { return n.substr(n.find(':') + 1); }
std::string itemName(const State& s, const ItemStack& st) { return st.empty() ? std::string() : shortName(s.items.name(st.item)); }
// GuiGraphics.itemDecorations count only (text: forced label or the count when != 1).
void drawCount(State& s, Canvas& c, const ItemStack& st, float x, float y, const char* forced = nullptr) {
    if (st.empty() || (st.count == 1 && !forced)) return;
    const std::string n = forced ? forced : std::to_string(st.count);
    s.font.draw(c, n, x + 19 - 2 - float(s.font.width(n)), y + 6 + 3, 0xFFFFFFFF, true);
}
void emptyIcon(State& s, Canvas& c, const Menu& m, const Layout& l, int slot, float left, float top, const char* sprite) {
    if (size_t(slot) >= m.slots.size() || !m.slots[size_t(slot)].empty()) return;
    blitSprite(c, s.sprites.sprite(sprite), left + float(l.slots[size_t(slot)].x), top + float(l.slots[size_t(slot)].y), 16, 16);
}
bool leftPress(const InputEvent& e) { return e.type == InputEvent::Button && e.down && e.code == 0; }
bool anyPress(const InputEvent& e) { return e.type == InputEvent::Button && e.down && (e.code == 0 || e.code == 1); }

// --- merchant (MerchantScreen / MerchantMenu) ------------------------------------------------------------------

struct Offer {
    ItemStack baseCostA, costA, costB, result;
    bool outOfStock = false;
    int xp = 0;
};
struct MerchantState {
    int containerId = -1;
    std::vector<Offer> offers;
    int level = 0, xp = 0;
    bool showProgress = false, canRestock = false;
    int shopItem = 0, scrollOff = 0;
    bool dragging = false;
} g_merchant;

// ItemCost.STREAM_CODEC: item, count, DataComponentExactPredicate (a list of typed components). Decoded through
// Items::read by framing it as an item stack whose patch adds those components.
bool readCost(const Items& items, net::Reader& r, ItemStack& out) {
    const int item = r.varInt(), count = r.varInt(), n = r.varInt();
    if (!r.ok) return false;
    net::Writer w;
    w.varInt(std::max(1, count)), w.varInt(item), w.varInt(n), w.varInt(0);
    const size_t head = w.buf.size();
    w.bytes(r.p, r.left());
    net::Reader t(w.buf.data(), w.buf.size());
    const bool ok = items.read(t, out);
    r.skip(size_t(t.p - w.buf.data()) - head);
    out.count = count;
    return ok && r.ok;
}

void merchantLayout(const Menu&, Layout& l) {
    l.background = "gui/container/villager";
    l.width = 276, l.height = 166, l.texW = 512, l.texH = 256;
    l.slots = {{136, 37}, {162, 37}, {220, 37}};
    standardSlots(l, 108, 84);
    l.titleY = -1000;
    l.invLabelX = 107, l.invLabelY = 72;
}
void merchantOpened(State&, Menu& m) {
    if (g_merchant.containerId != m.containerId) g_merchant = MerchantState{};
    g_merchant.containerId = m.containerId;
    g_merchant.shopItem = 0, g_merchant.scrollOff = 0, g_merchant.dragging = false;
}
bool merchantCanScroll() { return g_merchant.offers.size() > 7; }
int activeOffer(const Menu& m);

void merchantDrawBg(State& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    MerchantState& g = g_merchant;
    if (g.offers.empty()) return;
    const int n = int(g.offers.size());
    if (g.shopItem >= 0 && g.shopItem < n && g.offers[size_t(g.shopItem)].outOfStock)
        blitSprite(c, s.sprites.sprite("container/villager/out_of_stock"), left + 83 + 99, top + 35, 28, 21);
    // Scroll bar dragging (mouseDragged).
    if (g.dragging && s.leftDown && merchantCanScroll()) {
        const float topY = top + 18, bottomY = topY + 139;
        const int maxScrollOff = n - 7;
        float scrolling = (s.mouseY - topY - 13.5f) / (bottomY - topY - 27.0f);
        scrolling = scrolling * float(maxScrollOff) + 0.5f;
        g.scrollOff = std::clamp(int(scrolling), 0, maxScrollOff);
    } else if (!s.leftDown) {
        g.dragging = false;
    }
    // Offer buttons (Button.Plain).
    for (int i = 0; i < 7 && i < n; ++i) {
        const float bx = left + 5, by = top + 18 + float(i * 20);
        const bool hover = inRect(s, bx, by, 88, 20);
        if (hover) s.requestCursor(State::CursorPointingHand);  // TradeOfferButton (AbstractWidget.handleCursor)
        blitSprite(c, s.sprites.sprite(hover ? "widget/button_highlighted" : "widget/button"), bx, by, 88, 20);
    }
    // Scroller.
    const int steps = n + 1 - 7;
    if (steps > 1) {
        const int leftOver = 139 - (27 + (steps - 1) * 139 / steps);
        const int stepHeight = 1 + leftOver / steps + 139 / steps;
        int yOff = std::min(113, g.scrollOff * stepHeight);
        if (g.scrollOff == steps - 1) yOff = 113;
        blitSprite(c, s.sprites.sprite("container/villager/scroller"), left + 94, top + 18 + float(yOff), 6, 27);
        const float sy = top + 18 + float(yOff);
        if (s.mouseX >= left + 94 && s.mouseX < left + 100 && s.mouseY >= sy && s.mouseY <= sy + 27)
            s.requestCursor(g.dragging ? State::CursorResizeNS : State::CursorPointingHand);
    } else {
        blitSprite(c, s.sprites.sprite("container/villager/scroller_disabled"), left + 94, top + 18, 6, 27);
    }
    // Offers.
    float offerY = top + 16 + 1;
    for (int i = 0; i < n; ++i) {
        if (merchantCanScroll() && (i < g.scrollOff || i >= 7 + g.scrollOff)) continue;
        const Offer& o = g.offers[size_t(i)];
        const float y = offerY + 2, x1 = left + 5 + 5;
        s.drawItem(c, o.costA, x1, y, false);
        if (o.baseCostA.count == o.costA.count) {
            drawCount(s, c, o.costA, x1, y);
        } else {
            drawCount(s, c, o.baseCostA, x1, y, o.baseCostA.count == 1 ? "1" : nullptr);
            drawCount(s, c, o.costA, x1 + 14, y, o.costA.count == 1 ? "1" : nullptr);
            blitSprite(c, s.sprites.sprite("container/villager/discount_strikethrough"), x1 + 7, y + 12, 9, 2);
        }
        if (!o.costB.empty()) s.drawItem(c, o.costB, left + 5 + 35, y);
        blitSprite(c, s.sprites.sprite(o.outOfStock ? "container/villager/trade_arrow_out_of_stock" : "container/villager/trade_arrow"),
                   left + 5 + 35 + 20, y + 3, 10, 9);
        s.drawItem(c, o.result, left + 5 + 68, y);
        offerY += 20;
    }
    // Experience bar (extractProgressBar), with the selected trade's experience while its result is shown.
    if (g.showProgress && g.level < 5 && g.shopItem >= 0 && g.shopItem < n) {
        blitSprite(c, s.sprites.sprite("container/villager/experience_bar_background"), left + 136, top + 16, 102, 5);
        static const int kThresholds[5] = {0, 10, 70, 150, 250};
        const bool canLevelUp = g.level >= 1 && g.level < 5;
        const int minXp = canLevelUp ? kThresholds[g.level - 1] : 0, maxXp = canLevelUp ? kThresholds[g.level] : 0;
        if (g.xp >= minXp && canLevelUp) {
            const float mult = 102.0f / float(maxXp - minXp);
            const int w = std::min(int(std::floor(mult * float(g.xp - minXp))), 102);
            blitSpriteRegion(c, s.sprites.sprite("container/villager/experience_bar_current"), 102, 5, 0, 0, left + 136, top + 16, float(w), 5);
            const int active = activeOffer(m);  // getFutureTraderXp: the active offer's experience
            const int future = active >= 0 ? g.offers[size_t(active)].xp : 0;
            if (future > 0) {
                const int fw = std::min(int(std::floor(float(future) * mult)), 102 - w);
                blitSpriteRegion(c, s.sprites.sprite("container/villager/experience_bar_result"), 102, 5, w, 0, left + 136 + float(w),
                                 top + 16, float(fw), 5);
            }
        }
    }
}
// MerchantContainer.updateSellItem: the offer the payment satisfies (MerchantOffers.getRecipeFor with the selected
// trade as the hint when it is not the first; then the payment swapped when none or out of stock); -1 none.
int activeOffer(const Menu& m) {
    const MerchantState& g = g_merchant;
    if (m.slots.size() < 2) return -1;
    ItemStack a = m.slots[0], b = m.slots[1];
    if (a.empty()) a = b, b = ItemStack{};
    if (a.empty()) return -1;
    const int n = int(g.offers.size());
    auto find = [&](const ItemStack& x, const ItemStack& y) {  // MerchantOffer.satisfiedBy
        auto sat = [&](const Offer& o) {
            if (x.empty() || x.item != o.costA.item || x.count < o.costA.count) return false;
            return o.costB.empty() ? y.empty() : !y.empty() && y.item == o.costB.item && y.count >= o.costB.count;
        };
        if (g.shopItem > 0 && g.shopItem < n) return sat(g.offers[size_t(g.shopItem)]) ? g.shopItem : -1;
        for (int i = 0; i < n; ++i)
            if (sat(g.offers[size_t(i)])) return i;
        return -1;
    };
    int i = find(a, b);
    if (i < 0 || g.offers[size_t(i)].outOfStock) i = find(b, a);
    return i >= 0 && !g.offers[size_t(i)].outOfStock ? i : -1;
}

void merchantDrawFg(State& s, Canvas& c, Menu& m, const Layout& l, float left, float top) {
    const MerchantState& g = g_merchant;
    Text title = m.title;
    if (g.level > 0 && g.level <= 5 && g.showProgress)
        title = translatable("merchant.title", {m.title, Text(s.res.translate("merchant.level." + std::to_string(g.level)))}, s.lang());
    s.drawText(c, title, left + float(49 + l.width / 2 - s.font.width(title) / 2), top + 6, kLabel, false);
    const Text trades(s.res.translate("merchant.trades"));
    s.drawText(c, trades, left + float(5 - s.font.width(trades) / 2 + 48), top + 6, kLabel, false);
}
void merchantDrawTop(State& s, Canvas& c, Menu&, const Layout&, float left, float top) {
    const MerchantState& g = g_merchant;
    if (s.hoveredSlot >= 0 || g.offers.empty()) return;
    const int n = int(g.offers.size());
    for (int i = 0; i < 7 && i < n; ++i) {
        const float bx = left + 5, by = top + 18 + float(i * 20);
        if (!inRect(s, bx, by, 88, 20) || i + g.scrollOff >= n) continue;
        const Offer& o = g.offers[size_t(i + g.scrollOff)];
        const ItemStack* st = nullptr;
        if (s.mouseX < bx + 20) st = &o.costA;
        else if (s.mouseX < bx + 50 && s.mouseX > bx + 30) st = o.costB.empty() ? nullptr : &o.costB;
        else if (s.mouseX > bx + 65) st = &o.result;
        if (st) s.drawTooltip(c, s.items.tooltip(*st, s.settings.advancedItemTooltips), s.mouseX, s.mouseY, nullptr, true, tooltipStyle(s.items, *st));
        return;
    }
    if (g.shopItem >= 0 && g.shopItem < n && g.offers[size_t(g.shopItem)].outOfStock && inRect(s, left + 186, top + 35, 22, 21) &&
        g.canRestock)
        s.drawTooltip(c, {Text(s.res.translate("merchant.deprecated"))}, s.mouseX, s.mouseY);  // one line, unwrapped
}
bool merchantInput(State& s, const InputEvent& e, Menu& m, const Layout&, float left, float top) {
    MerchantState& g = g_merchant;
    const int n = int(g.offers.size());
    if (e.type == InputEvent::Wheel) {
        if (merchantCanScroll()) g.scrollOff = std::clamp(g.scrollOff - e.code, 0, n - 7);
        return true;
    }
    if (e.type == InputEvent::Button && !e.down && e.code == 0) g.dragging = false;
    if (!leftPress(e)) return false;
    if (merchantCanScroll() && s.mouseX > left + 94 && s.mouseX < left + 94 + 6 && s.mouseY > top + 18 && s.mouseY <= top + 18 + 139 + 1) {
        g.dragging = true;
        return true;
    }
    for (int i = 0; i < 7 && i < n; ++i) {
        if (!inRect(s, left + 5, top + 18 + float(i * 20), 88, 20)) continue;
        audio::playUiClick();
        g.shopItem = i + g.scrollOff;
        // postButtonClick: the server moves the payment back and fills in the trade's cost (tryMoveItems) and
        // syncs the slots.
        if (s.online()) {
            net::Writer w;
            w.varInt(g.shopItem);
            s.client->sendPlay("select_trade", w);
        }
        (void)m;
        return true;
    }
    return false;
}
int merchantQuickMove(State&, const Menu&, int index, const ItemStack&, const std::function<bool(int, int, bool)>& moveTo) {
    if (index == 2) return moveTo(3, 39, true) ? 1 : 0;
    if (index == 0 || index == 1) return moveTo(3, 39, false) ? 1 : 0;
    if (index >= 3 && index < 30) return moveTo(30, 39, false) ? 1 : 0;
    if (index >= 30 && index < 39) return moveTo(3, 30, false) ? 1 : 0;
    return 0;
}

// MerchantResultSlot.onTake: the active offer (the one the payment satisfies) takes its costs.
void merchantOnTake(State& s, Menu& m) {
    auto satisfies = [&](const ItemStack& have, const ItemStack& cost) {
        if (cost.empty()) return true;
        return !have.empty() && have.item == cost.item && have.count >= cost.count;
    };
    const int active = activeOffer(m);  // slots.getActiveOffer
    if (active < 0) return;
    {
        const Offer& o = g_merchant.offers[size_t(active)];
        g_merchant.xp += o.xp;  // merchant.overrideXp(getVillagerXp() + offer.getXp())
        for (int swap = 0; swap < 2; ++swap) {
            ItemStack& a = m.slots[size_t(swap)];
            ItemStack& b = m.slots[size_t(1 - swap)];
            if (!satisfies(a, o.costA) || !satisfies(b, o.costB)) continue;
            a = a.withCount(a.count - o.costA.count);
            if (!o.costB.empty()) b = b.withCount(b.count - o.costB.count);
            (void)s;
            return;
        }
    }
}
const StationRegistrar kMerchant({"merchant"}, [] {
    StationDef d{merchantLayout, merchantOpened, merchantDrawBg, merchantDrawFg, merchantDrawTop, merchantInput, 2, merchantQuickMove, nullptr};
    d.onTake = merchantOnTake;
    d.pickAll = -1;  // MerchantMenu.canTakeItemForPickAll: never
    return d;
}());

// --- beacon (BeaconScreen / BeaconMenu) -------------------------------------------------------------------------

struct BeaconState {
    int primary = -1, secondary = -1;  // mob_effect registry ids, -1 none
    int seen1 = -2, seen2 = -2;        // data values last applied (dataChanged)
    std::vector<int> ids;              // speed, haste, resistance, jump_boost, strength, regeneration
    std::vector<std::string> names;
} g_beacon;

int effectId(State& s, const std::string& name) {
    const nlohmann::json& reg = s.reports->registries();
    if (!reg.contains("minecraft:mob_effect")) return -1;
    const auto& e = reg["minecraft:mob_effect"]["entries"];
    const std::string key = "minecraft:" + name;
    return e.contains(key) ? e[key].value("protocol_id", -1) : -1;
}
const char* kBeaconEffects[4][2] = {{"speed", "haste"}, {"resistance", "jump_boost"}, {"strength", nullptr}, {"regeneration", nullptr}};

struct BeaconButton {
    float x, y;
    std::string effect;  // "" for confirm / cancel
    int kind;            // 0 primary power, 1 secondary power, 2 upgrade, 3 confirm, 4 cancel
    int tier;
};
std::vector<BeaconButton> beaconButtons(float left, float top) {
    std::vector<BeaconButton> b;
    for (int tier = 0; tier <= 2; ++tier) {
        const int count = kBeaconEffects[tier][1] ? 2 : 1, totalWidth = count * 22 + (count - 1) * 2;
        for (int c = 0; c < count; ++c)
            b.push_back({left + float(76 + c * 24 - totalWidth / 2), top + float(22 + tier * 25), kBeaconEffects[tier][c], 0, tier});
    }
    const int count = 2, totalWidth = count * 22 + (count - 1) * 2;
    b.push_back({left + float(167 - totalWidth / 2), top + 47, "regeneration", 1, 3});
    b.push_back({left + float(167 + 24 - totalWidth / 2), top + 47, "", 2, 3});
    b.push_back({left + 164, top + 107, "", 3, 0});
    b.push_back({left + 190, top + 107, "", 4, 0});
    return b;
}
int effectIdByName(State& s, const std::string& n) { return n.empty() ? -1 : effectId(s, n); }
std::string effectNameById(State& s, int id) {
    for (int t = 0; t < 4; ++t)
        for (int c = 0; c < 2; ++c)
            if (kBeaconEffects[t][c] && effectId(s, kBeaconEffects[t][c]) == id) return kBeaconEffects[t][c];
    return {};
}
void beaconSync(State& s, const Menu& m) {  // the listener's dataChanged: selection follows the beacon's data
    if (m.data[1] != g_beacon.seen1 || m.data[2] != g_beacon.seen2) {
        g_beacon.seen1 = m.data[1], g_beacon.seen2 = m.data[2];
        g_beacon.primary = m.data[1] - 1, g_beacon.secondary = m.data[2] - 1;
    }
    (void)s;
}
// active, selected, visible
void beaconStatus(State& s, const Menu& m, const BeaconButton& b, bool& active, bool& selected, bool& visible) {
    const int levels = m.data[0];
    const bool hasPayment = !m.slots.empty() && !m.slots[0].empty();
    active = true, selected = false, visible = true;
    if (b.kind == 3) active = hasPayment && g_beacon.primary >= 0;
    else if (b.kind == 4) active = true;
    else if (b.kind == 2) {
        visible = g_beacon.primary >= 0;
        active = 3 < levels;
        selected = g_beacon.primary >= 0 && g_beacon.secondary == g_beacon.primary;
    } else {
        active = b.tier < levels;
        selected = effectIdByName(s, b.effect) == (b.kind == 0 ? g_beacon.primary : g_beacon.secondary) && effectIdByName(s, b.effect) >= 0;
    }
}
void beaconLayout(const Menu&, Layout& l) {
    l.background = "gui/container/beacon";
    l.width = 230, l.height = 219;
    l.slots = {{136, 110}};
    standardSlots(l, 36, 137);
    l.titleY = -1000, l.invLabelY = -1000;
}
void beaconOpened(State&, Menu&) {
    g_beacon.primary = g_beacon.secondary = -1;
    g_beacon.seen1 = g_beacon.seen2 = -2;
}
void beaconDrawBg(State& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    beaconSync(s, m);
    const char* pay[5] = {"netherite_ingot", "emerald", "diamond", "gold_ingot", "iron_ingot"};
    const float px[5] = {20, 41, 41 + 22, 42 + 44, 42 + 66};
    for (int i = 0; i < 5; ++i) s.drawItem(c, ItemStack{s.items.id(pay[i]), 1}, left + px[i], top + 109);
    for (const BeaconButton& b : beaconButtons(left, top)) {
        bool active, selected, visible;
        beaconStatus(s, m, b, active, selected, visible);
        if (!visible) continue;
        const char* sp = !active ? "container/beacon/button_disabled" : selected ? "container/beacon/button_selected"
                         : inRect(s, b.x, b.y, 22, 22) ? "container/beacon/button_highlighted" : "container/beacon/button";
        blitSprite(c, s.sprites.sprite(sp), b.x, b.y, 22, 22);
        if (b.kind == 3) blitSprite(c, s.sprites.sprite("container/beacon/confirm"), b.x + 2, b.y + 2, 18, 18);
        else if (b.kind == 4) blitSprite(c, s.sprites.sprite("container/beacon/cancel"), b.x + 2, b.y + 2, 18, 18);
        else {
            const std::string eff = b.kind == 2 ? effectNameById(s, g_beacon.primary) : b.effect;
            if (const Sprite* icon = s.sprites.texture("mob_effect/" + eff)) blitTexture(c, icon, b.x + 2, b.y + 2, 0, 0, 18, 18, 18, 18);
        }
    }
}
void beaconDrawFg(State& s, Canvas& c, Menu&, const Layout&, float left, float top) {
    s.drawCentered(c, Text(s.res.translate("block.minecraft.beacon.primary")), left + 62, top + 10, 0xFFE0E0E0);
    s.drawCentered(c, Text(s.res.translate("block.minecraft.beacon.secondary")), left + 169, top + 10, 0xFFE0E0E0);
}
void beaconDrawTop(State& s, Canvas& c, Menu& m, const Layout&, float left, float top) {
    if (!s.carried.empty() || s.hoveredSlot >= 0) return;
    for (const BeaconButton& b : beaconButtons(left, top)) {
        bool active, selected, visible;
        beaconStatus(s, m, b, active, selected, visible);
        if (!visible || !inRect(s, b.x, b.y, 22, 22)) continue;
        Text tip;
        if (b.kind == 3) tip = Text(s.res.translate("gui.done"));
        else if (b.kind == 4) tip = Text(s.res.translate("gui.cancel"));
        else {
            const std::string eff = b.kind == 2 ? effectNameById(s, g_beacon.primary) : b.effect;
            tip = Text(s.res.translate("effect.minecraft." + eff) + (b.kind == 2 ? " II" : ""));
        }
        s.drawTooltip(c, {tip}, s.mouseX, s.mouseY);
        return;
    }
}
bool beaconInput(State& s, const InputEvent& e, Menu& m, const Layout&, float left, float top) {
    if (!leftPress(e)) return false;
    beaconSync(s, m);
    for (const BeaconButton& b : beaconButtons(left, top)) {
        bool active, selected, visible;
        beaconStatus(s, m, b, active, selected, visible);
        if (!visible || !inRect(s, b.x, b.y, 22, 22)) continue;
        if (!active) return true;
        audio::playUiClick();
        if (b.kind == 4) {
            s.closeScreen();
        } else if (b.kind == 3) {
            if (s.online()) {
                net::Writer w;
                w.boolean(g_beacon.primary >= 0);
                if (g_beacon.primary >= 0) w.varInt(g_beacon.primary);
                w.boolean(g_beacon.secondary >= 0);
                if (g_beacon.secondary >= 0) w.varInt(g_beacon.secondary);
                s.client->sendPlay("set_beacon", w);
            }
            s.closeScreen();
        } else if (!selected) {
            const int id = b.kind == 2 ? g_beacon.primary : effectIdByName(s, b.effect);
            if (b.kind == 0) {
                g_beacon.primary = id;
                if (g_beacon.secondary != id) g_beacon.secondary = -1;
            } else {
                g_beacon.secondary = id;
            }
        }
        return true;
    }
    return false;
}
bool beaconMayPlace(State& s, const Menu&, int slot, const ItemStack& st) {  // PaymentSlot: #beacon_payment_items
    return slot != 0 || s.items.itemHasTag(st.item, "minecraft:beacon_payment_items");
}
int beaconQuickMove(State& s, const Menu& m, int index, const ItemStack& st, const std::function<bool(int, int, bool)>& moveTo) {
    if (index == 0) return moveTo(1, 37, true) ? 1 : 0;
    if (m.slots[0].empty() && beaconMayPlace(s, m, 0, st) && st.count == 1) return moveTo(0, 1, false) ? 1 : 0;
    if (index >= 1 && index < 28) return moveTo(28, 37, false) ? 1 : 0;
    if (index >= 28 && index < 37) return moveTo(1, 28, false) ? 1 : 0;
    return moveTo(1, 37, false) ? 1 : 0;
}

const StationRegistrar kBeacon({"beacon"}, [] {
    StationDef d{beaconLayout, beaconOpened, beaconDrawBg, beaconDrawFg, beaconDrawTop, beaconInput, -1, beaconQuickMove, beaconMayPlace};
    d.slotMax = [](State&, const Menu&, int slot, const ItemStack&) { return slot == 0 ? 1 : 0; };
    return d;
}());

// --- brewing stand (BrewingStandScreen / BrewingStandMenu) ----------------------------------------------------

void brewingLayout(const Menu&, Layout& l) {
    l.background = "gui/container/brewing_stand";
    l.slots = {{56, 51}, {79, 58}, {102, 51}, {79, 17}, {17, 17}};
    standardSlots(l, 8, 84);
    l.titleX = -1;
}
void brewingDrawBg(State& s, Canvas& c, Menu& m, const Layout& l, float left, float top) {
    const int brewTicks = m.data[0], fuel = m.data[1], totalBrew = m.data[2], totalFuel = m.data[3];
    const int fuelLength = totalFuel > 0 ? std::clamp((18 * fuel + totalFuel - 1) / totalFuel, 0, 18) : 0;
    if (fuelLength > 0)
        blitSpriteRegion(c, s.sprites.sprite("container/brewing_stand/fuel_length"), 18, 4, 0, 0, left + 60, top + 44, float(fuelLength), 4);
    if (brewTicks > 0 && totalBrew > 0) {
        int length = int(28.0f * (1.0f - float(brewTicks) / float(totalBrew)));
        if (length > 0)
            blitSpriteRegion(c, s.sprites.sprite("container/brewing_stand/brew_progress"), 9, 28, 0, 0, left + 97, top + 16, 9, float(length));
        static const int kBubbles[7] = {29, 24, 20, 16, 11, 6, 0};
        length = kBubbles[brewTicks / 2 % 7];
        if (length > 0)
            blitSpriteRegion(c, s.sprites.sprite("container/brewing_stand/bubbles"), 12, 29, 0, 29 - length, left + 63,
                             top + 14 + float(29 - length), 12, float(length));
    }
    for (int i = 0; i < 3; ++i) emptyIcon(s, c, m, l, i, left, top, "container/slot/potion");
    emptyIcon(s, c, m, l, 4, left, top, "container/slot/brewing_fuel");
}
// BrewingStandMenu: PotionIngredient.isPotionInput (the brewing_input set or #brewing_potion_inputs), the
// brewing_reagent set, the brewing_fuel component.
bool isPotionInput(State& s, const ItemStack& st) {
    return inPropertySet("minecraft:brewing_input", st.item) || s.items.itemHasTag(st.item, "minecraft:brewing_potion_inputs");
}
bool isBrewingFuel(State& s, const ItemStack& st) { return s.items.component(st, "brewing_fuel").present(); }
bool brewingMayPlace(State& s, const Menu&, int slot, const ItemStack& st) {
    if (slot <= 2) return isPotionInput(s, st);
    if (slot == 3) return inPropertySet("minecraft:brewing_reagent", st.item);
    if (slot == 4) return isBrewingFuel(s, st);
    return true;
}
int brewingQuickMove(State& s, const Menu& m, int index, const ItemStack& st, const std::function<bool(int, int, bool)>& moveTo) {
    if (index >= 0 && index <= 4) return moveTo(5, 41, true) ? 1 : 0;
    if (isBrewingFuel(s, st)) {  // into the fuel slot ends the move (the game returns EMPTY there)
        if (moveTo(4, 5, false)) return 3;
        if (brewingMayPlace(s, m, 3, st) && !moveTo(3, 4, false)) return 0;
        return 1;
    }
    if (brewingMayPlace(s, m, 3, st)) return moveTo(3, 4, false) ? 1 : 0;
    if (isPotionInput(s, st)) return moveTo(0, 3, false) ? 1 : 0;
    if (index >= 5 && index < 32) return moveTo(32, 41, false) ? 1 : 0;
    if (index >= 32 && index < 41) return moveTo(5, 32, false) ? 1 : 0;
    return moveTo(5, 41, false) ? 1 : 0;
}

const StationRegistrar kBrewing({"brewing_stand"}, [] {
    StationDef d{brewingLayout, nullptr, brewingDrawBg, nullptr, nullptr, nullptr, -1, brewingQuickMove, brewingMayPlace};
    d.slotMax = [](State&, const Menu&, int slot, const ItemStack&) { return slot <= 2 ? 1 : 0; };  // PotionSlot
    return d;
}());

// --- crafter (CrafterScreen / CrafterMenu) -------------------------------------------------------------------

void crafterLayout(const Menu&, Layout& l) {
    l.background = "gui/container/crafter";
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x) l.slots.push_back({26 + x * 18, 17 + y * 18});
    standardSlots(l, 8, 84);
    l.slots.push_back({134, 35, false});  // the result (NonInteractiveResultSlot, not highlighted), after the player inventory
    l.titleX = -1;
}
bool crafterDisabled(const Menu& m, int slot) { return slot >= 0 && slot < 9 && m.data[size_t(slot)] == 1; }
void crafterSetSlot(State& s, Menu& m, int slot, bool enabled) {
    m.data[size_t(slot)] = enabled ? 0 : 1;
    if (s.online()) {
        net::Writer w;
        w.varInt(slot);
        w.varInt(m.containerId);
        w.boolean(enabled);
        s.client->sendPlay("container_slot_state_changed", w);
    }
    // CrafterScreen.setSlotState: player.playSound(UI_BUTTON_CLICK, 0.4, enabled ? 1 : 0.75)
    if (audio::AudioSystem* a = audio::instance(); a && s.game) {
        const Player& p = s.game->player();
        a->play("minecraft:ui.button.click", audio::Category::Player, p.pos[0], p.pos[1], p.pos[2], 0.4f, enabled ? 1.0f : 0.75f);
    }
}
void crafterDrawBg(State& s, Canvas& c, Menu& m, const Layout& l, float left, float top) {
    for (int i = 0; i < 9; ++i) {
        if (crafterDisabled(m, i))
            blitSprite(c, s.sprites.sprite("container/crafter/disabled_slot"), left + float(l.slots[size_t(i)].x - 1),
                       top + float(l.slots[size_t(i)].y - 1), 18, 18);
        const float x0 = left + float(l.slots[size_t(i)].x - 2), y0 = top + float(l.slots[size_t(i)].y - 2);  // CrafterScreen.extractSlot
        if (s.mouseX > x0 && s.mouseY > y0 && s.mouseX < x0 + 19 && s.mouseY < y0 + 19) s.requestCursor(State::CursorPointingHand);
    }
}
void crafterDrawTop(State& s, Canvas& c, Menu& m, const Layout&, float, float) {
    blitSprite(c, s.sprites.sprite(m.data[9] == 1 ? "container/crafter/powered_redstone" : "container/crafter/unpowered_redstone"),
               float(c.guiWidth() / 2 + 9), float(c.guiHeight() / 2 - 48), 16, 16);
    const int h = s.hoveredSlot;
    if (h >= 0 && h < 9 && !crafterDisabled(m, h) && s.carried.empty() && m.slots[size_t(h)].empty() && !s.spectator())
        s.drawTooltip(c, {Text(s.res.translate("gui.togglable_slot"))}, s.mouseX, s.mouseY);
}
int crafterSlotAt(State& s, const Layout& l, float left, float top) {
    for (size_t i = 0; i < l.slots.size(); ++i) {
        const float sx = left + float(l.slots[i].x), sy = top + float(l.slots[i].y);
        if (s.mouseX >= sx - 1 && s.mouseX < sx + 17 && s.mouseY >= sy - 1 && s.mouseY < sy + 17) return int(i);
    }
    return -1;
}
bool crafterInput(State& s, const InputEvent& e, Menu& m, const Layout& l, float left, float top) {
    const int slot = crafterSlotAt(s, l, left, top);
    if (slot < 0 || slot >= 9 || s.spectator() || !m.slots[size_t(slot)].empty()) return false;
    // slotClicked: PICKUP toggles an empty crafter slot, SWAP (number keys) enables it with an item to put in.
    if (anyPress(e) && !s.window->keyDown(VK_SHIFT)) {
        if (crafterDisabled(m, slot)) crafterSetSlot(s, m, slot, true);
        else if (s.carried.empty()) crafterSetSlot(s, m, slot, false);
    } else if (e.type == InputEvent::Key && e.down && crafterDisabled(m, slot)) {  // SWAP: no cursor check
        // SWAP: the hotbar keys (Inventory slots 0-8) and the offhand key (40) with an item there
        int from = -1;
        for (int i = 0; i < 9 && from < 0; ++i)
            if (s.isKey(Key(int(Key::Hotbar1) + i), e)) from = 36 + i;
        if (from < 0 && s.isKey(Key::SwapOffhand, e)) from = 45;
        if (from >= 0 && !s.inventory.slots[size_t(from)].empty()) crafterSetSlot(s, m, slot, true);
    }
    return false;  // the click goes on (AbstractContainerScreen.slotClicked)
}
bool crafterMayPlace(State&, const Menu& m, int slot, const ItemStack&) {  // the result: NonInteractiveResultSlot
    return slot != 45 && !crafterDisabled(m, slot);
}
int crafterQuickMove(State&, const Menu&, int index, const ItemStack&, const std::function<bool(int, int, bool)>& moveTo) {
    if (index < 9) return moveTo(9, 45, true) ? 1 : 0;
    if (index == 45) return 0;
    return moveTo(0, 9, false) ? 1 : 0;
}

const StationRegistrar kCrafter({"crafter_3x3"}, [] {
    StationDef d{crafterLayout, nullptr, crafterDrawBg, nullptr, crafterDrawTop, crafterInput, 45, crafterQuickMove, crafterMayPlace};
    d.mayPickup = [](State&, const Menu&, int slot) { return slot != 45; };  // NonInteractiveResultSlot
    return d;
}());

// --- mounts (mount_screen_open: HorseInventoryScreen / NautilusInventoryScreen) -------------------------------

struct MountState {
    int columns = 0, entity = -1;
    std::string type = "horse";  // entity type without namespace
    bool baby = false;
    bool tamed = false;  // AbstractHorse.isTamed (DATA_ID_FLAGS & 2) / TamableAnimal.isTame (DATA_FLAGS_ID & 4)
} g_mount;

bool mountNautilus() { return g_mount.type == "nautilus" || g_mount.type == "zombie_nautilus"; }
bool mountLlama() { return g_mount.type == "llama" || g_mount.type == "trader_llama"; }
// canUseSlot: Horse, Llama, SkeletonHorse and ZombieHorse every slot; other AbstractHorses the saddle when adult and
// tamed; AbstractNautilus the saddle and the body when adult and tame.
bool mountSaddle() {  // canUseSlot(SADDLE) && #can_equip_saddle
    const std::string& t = g_mount.type;
    if (t == "horse" || t == "skeleton_horse" || t == "zombie_horse") return true;
    if (t == "donkey" || t == "mule" || t == "camel" || t == "camel_husk" || mountNautilus()) return !g_mount.baby && g_mount.tamed;
    return false;
}
bool mountArmor() {  // canUseSlot(BODY) && (#can_wear_horse_armor || llama); nautili: canUseSlot(BODY)
    if (mountNautilus()) return !g_mount.baby && g_mount.tamed;
    return g_mount.type == "horse" || g_mount.type == "zombie_horse" || mountLlama();
}
void mountLayout(const Menu&, Layout& l) {
    l.background = mountNautilus() ? "gui/container/nautilus" : "gui/container/horse";
    l.slots.push_back(mountSaddle() ? SlotPos{8, 18} : SlotPos{-10000, -10000});  // inactive slots are not shown
    l.slots.push_back(mountArmor() ? SlotPos{8, 36} : SlotPos{-10000, -10000});
    if (!mountNautilus())
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < g_mount.columns; ++x) l.slots.push_back({80 + x * 18, 18 + y * 18});
    standardSlots(l, 8, 84);
}
void mountDrawBg(State& s, Canvas& c, Menu& m, const Layout& l, float left, float top) {
    if (g_mount.columns > 0 && !mountNautilus())
        blitSpriteRegion(c, s.sprites.sprite("container/horse/chest_slots"), 90, 54, 0, 0, left + 79, top + 17, float(g_mount.columns * 18), 54);
    if (mountSaddle()) {
        blitSprite(c, s.sprites.sprite("container/slot"), left + 7, top + 35 - 18, 18, 18);
        emptyIcon(s, c, m, l, 0, left, top, "container/slot/saddle");
    }
    if (mountArmor()) {
        blitSprite(c, s.sprites.sprite("container/slot"), left + 7, top + 35, 18, 18);
        emptyIcon(s, c, m, l, 1, left, top,
                  mountNautilus() ? "container/slot/nautilus_armor_inventory" : mountLlama() ? "container/slot/llama_armor" : "container/slot/horse_armor");
    }
    PreviewEntity pe;
    if (previewEntity(s, g_mount.entity, pe))
        drawEntityFollowsMouse(s, c, pe, int(left) + 26, int(top) + 18, int(left) + 78, int(top) + 70, 17, 0.25f, s.mouseX, s.mouseY);
}
// ArmorSlot (SADDLE / BODY) of the mount: LivingEntity.isEquippableInSlot (the equippable slot and allowed entities).
bool mountMayPlace(State& s, const Menu&, int slot, const ItemStack& st) {
    if (slot == 0) return mountSaddle() && s.items.equipmentSlot(st) == 7 && s.items.canBeEquippedBy(st, g_mount.type);
    if (slot == 1) return mountArmor() && s.items.equipmentSlot(st) == 6 && s.items.canBeEquippedBy(st, g_mount.type);
    return true;
}
// AbstractMountInventoryMenu.quickMoveStack (moves between the inventory and the hotbar end the shift-click: 3).
int mountQuickMove(State& s, const Menu& m, int index, const ItemStack& st, const std::function<bool(int, int, bool)>& moveTo) {
    const int start = 2 + (mountNautilus() ? 0 : g_mount.columns * 3), n = int(m.slots.size());
    if (index < start) return moveTo(start, n, true) ? 1 : 0;
    if (mountMayPlace(s, m, 1, st) && m.slots[1].empty()) return moveTo(1, 2, false) ? 1 : 0;
    if (mountMayPlace(s, m, 0, st) && m.slots[0].empty()) return moveTo(0, 1, false) ? 1 : 0;
    if (start > 2 && moveTo(2, start, false)) return 1;
    const int end = start + 27, hotbarEnd = end + 9;
    if (index >= end && index < hotbarEnd) return moveTo(start, end, false) ? 3 : 0;
    if (index >= start && index < end) return moveTo(end, hotbarEnd, false) ? 3 : 0;
    return 0;
}

const StationRegistrar kMount({"mount"}, [] {
    StationDef d{mountLayout, nullptr, mountDrawBg, nullptr, nullptr, nullptr, -1, mountQuickMove, mountMayPlace};
    d.slotMax = [](State&, const Menu&, int slot, const ItemStack&) { return slot <= 1 ? 1 : 0; };  // ArmorSlot
    d.mayPickup = [](State& s, const Menu& m, int slot) {  // ArmorSlot: curse of binding
        return slot > 1 || s.creative() || !s.items.hasEnchantmentEffect(m.slots[size_t(slot)], "minecraft:prevent_armor_change");
    };
    return d;
}());

// Minecraft.handleKeybinds: the inventory key while riding a horse-like mount (PlayerRideableJumping) asks the
// server for the mount's inventory (ServerboundPlayerCommandPacket OPEN_INVENTORY) instead of the player's own.
int g_vehicle = -1;  // the entity the local player rides (set_passengers), -1 none

bool ridingJumpable(State& s) {
    if (g_vehicle < 0 || !s.entities) return false;
    const auto& all = s.entities->entities();
    auto it = all.find(g_vehicle);
    if (it == all.end() || !it->second.info) return false;
    static const char* kTypes[] = {"horse", "donkey", "mule", "skeleton_horse", "zombie_horse", "llama", "trader_llama", "camel",
                                   "camel_husk", "nautilus", "zombie_nautilus"};
    const std::string t = shortName(it->second.info->name);
    for (const char* k : kTypes)
        if (t == k) return true;
    return false;
}
bool worldKey(State& s, const InputEvent& e) {
    if (e.repeat || !s.isKey(Key::Inventory, e) || !s.online() || !s.session || !ridingJumpable(s)) return false;
    net::Writer w;
    w.varInt(s.session->entityId());
    w.varInt(5);  // Action.OPEN_INVENTORY
    w.varInt(0);
    s.client->sendPlay("player_command", w);
    return true;
}
void attached(State&) { g_vehicle = -1; }

// --- packets --------------------------------------------------------------------------------------------------

void registerHandlers(State& st, net::Client& client) {
    State* s = &st;
    const Items* items = &st.items;
    client.onPlayPeek("set_passengers", [s](net::Reader& r) {
        const int vehicle = r.varInt();
        const i32 n = r.varInt();
        std::vector<int> riders;
        for (i32 i = 0; i < n && r.ok; ++i) riders.push_back(r.varInt());
        if (!r.ok) return;
        s->post([s, vehicle, riders] {
            if (!s->session) return;
            const bool me = std::find(riders.begin(), riders.end(), s->session->entityId()) != riders.end();
            if (me) g_vehicle = vehicle;
            else if (g_vehicle == vehicle) g_vehicle = -1;
        });
    });
    client.onPlay("merchant_offers", [s, items](net::Reader& r) {
        const int id = r.varInt();
        const i32 n = r.varInt();
        std::vector<Offer> offers;
        for (i32 i = 0; i < n && r.ok; ++i) {
            Offer o;
            if (!readCost(*items, r, o.baseCostA)) break;
            items->read(r, o.result);
            if (r.boolean() && !readCost(*items, r, o.costB)) break;
            const bool exhausted = r.boolean();
            const int uses = r.i32v(), maxUses = r.i32v();
            o.xp = r.i32v();
            const int special = r.i32v();
            const float mult = r.f32();
            const int demand = r.i32v();
            o.outOfStock = exhausted || uses >= maxUses;
            // MerchantOffer.getModifiedCostCount
            const int base = o.baseCostA.count;
            const int demandDiff = std::max(0, int(std::floor(float(base) * float(demand) * mult)));
            o.costA = o.baseCostA.withCount(std::clamp(base + demandDiff + special, 1, std::max(1, items->maxStack(o.baseCostA))));
            offers.push_back(std::move(o));
        }
        const int level = r.varInt(), xp = r.varInt();
        const bool progress = r.boolean(), restock = r.boolean();
        if (!r.ok) LOG("ui: merchant_offers: decoding stopped after %zu of %d offers", offers.size(), n);
        if (std::getenv("MCPP_UI_VERBOSE")) LOG("ui: merchant_offers %d: %zu offers, level %d xp %d", id, offers.size(), level, xp);
        s->post([s, id, offers = std::move(offers), level, xp, progress, restock] {
            if (!s->container || s->container->containerId != id || s->container->typeName != "merchant") return;
            MerchantState& g = g_merchant;
            g.containerId = id;
            g.offers = offers;
            g.level = level, g.xp = xp, g.showProgress = progress, g.canRestock = restock;
            if (g.shopItem >= int(offers.size())) g.shopItem = 0;
            g.scrollOff = std::clamp(g.scrollOff, 0, std::max(0, int(offers.size()) - 7));
        });
    });
    client.onPlay("mount_screen_open", [s](net::Reader& r) {
        const int id = r.varInt(), columns = r.varInt(), entity = r.i32v();
        if (!r.ok) return;
        s->post([s, id, columns, entity] {
            g_mount = MountState{};
            g_mount.columns = columns, g_mount.entity = entity;
            Text title("Horse");
            if (s->entities) {
                const auto& all = s->entities->entities();
                if (auto it = all.find(entity); it != all.end() && it->second.info) {
                    g_mount.type = shortName(it->second.info->name);
                    g_mount.baby = it->second.baby;
                    g_mount.tamed = mountNautilus() ? (ent::dataInt(it->second, "DATA_FLAGS_ID") & 4) != 0
                                                    : (ent::dataInt(it->second, "DATA_ID_FLAGS") & 2) != 0;
                    title = it->second.name.empty() ? Text(s->res.translate("entity.minecraft." + g_mount.type)) : Text(it->second.name);
                }
            }
            if (s->screen != Screen::None && s->screen != Screen::Container) s->closeScreen(false);
            auto m = std::make_unique<Menu>();
            m->containerId = id;
            m->type = 1 << 20;  // not a minecraft:menu type: the "mount" station
            m->typeName = "mount";
            m->title = title;
            s->container = std::move(m);
            const Layout l = layoutFor(*s->container);
            s->container->slots.assign(l.slots.size(), ItemStack{});
            s->openScreen(Screen::Container);
            if (std::getenv("MCPP_UI_VERBOSE"))
                LOG("ui: mount_screen_open %d: %s, %d columns, entity %d", id, g_mount.type.c_str(), columns, entity);
        });
    });
}

const UiModuleRegistrar kModule(UiModule{"stations_b", registerHandlers, attached, nullptr, nullptr, nullptr, worldKey});

}  // namespace
}  // namespace ui
