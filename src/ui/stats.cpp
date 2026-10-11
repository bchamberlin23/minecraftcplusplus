// Statistics screen (StatsScreen): asks the server for the player's statistics (client_command REQUEST_STATS),
// shows General (custom stats sorted by name, formatted like StatFormatter), Items (mined / broken / crafted /
// used / picked up / dropped columns, sortable) and Mobs (killed / killed by) in a tab bar (MenuTabBar) with
// scrolling lists (AbstractSelectionList).
#include "assets/game_reports.h"
#include "audio/audio.h"
#include "ui/ui_state.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace ui {

namespace {
constexpr int kTabBarH = 24, kFooterH = 33, kRowW = 280;

struct Stats {
    std::vector<std::string> types;                     // stat_type registry (protocol order)
    std::unordered_map<std::string, std::vector<std::string>> registries;  // value registries by stat type
    std::map<std::pair<int, int>, int> values;           // (stat type, value id) -> value
    bool received = false;
    int tab = 0;
    double scroll[3] = {};
    bool draggingBar = false;
    int sortColumn = -1, sortOrder = 0;  // items: column 0..5, -1 / 1
    bool returnToPause = true;
};
Stats g;

std::vector<std::string> registryNames(const UiState& s, const char* key) {
    std::vector<std::string> v;
    const nlohmann::json& reg = s.reports->registries();
    if (!reg.contains(key)) return v;
    for (const auto& e : reg[key]["entries"].items()) {
        const int id = e.value().value("protocol_id", -1);
        if (id < 0) continue;
        if (size_t(id) >= v.size()) v.resize(size_t(id) + 1);
        v[size_t(id)] = e.key().substr(e.key().find(':') + 1);
    }
    return v;
}

void loadRegistries(UiState& s) {
    if (!g.types.empty()) return;
    g.types = registryNames(s, "minecraft:stat_type");
    g.registries["mined"] = registryNames(s, "minecraft:block");
    for (const char* t : {"crafted", "used", "broken", "picked_up", "dropped"}) g.registries[t] = registryNames(s, "minecraft:item");
    g.registries["killed"] = g.registries["killed_by"] = registryNames(s, "minecraft:entity_type");
    g.registries["custom"] = registryNames(s, "minecraft:custom_stat");
}
int typeId(const char* name) {
    for (size_t i = 0; i < g.types.size(); ++i)
        if (g.types[i] == name) return int(i);
    return -1;
}
int valueId(const char* type, const std::string& name) {
    const auto& v = g.registries[type];
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == name) return int(i);
    return -1;
}
int value(const char* type, const std::string& name) {
    const int t = typeId(type), id = valueId(type, name);
    auto it = g.values.find({t, id});
    return t < 0 || id < 0 || it == g.values.end() ? 0 : it->second;
}

// StatFormatter.
std::string fmt2(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.2f", v);
    return b;
}
std::string formatDefault(int v) {  // NumberFormat.getIntegerInstance(Locale.US): grouping commas
    std::string d = std::to_string(std::abs(v)), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return (v < 0 ? "-" : "") + out;
}
std::string formatStat(const std::string& custom, int v) {
    if (custom.ends_with("_one_cm")) {
        const double m = v / 100.0, km = m / 1000.0;
        return km > 0.5 ? fmt2(km) + " km" : m > 0.5 ? fmt2(m) + " m" : std::to_string(v) + " cm";
    }
    if (custom == "play_time" || custom == "total_world_time" || custom == "time_since_death" || custom == "time_since_rest" ||
        custom == "sneak_time") {
        const double sec = v / 20.0, min = sec / 60.0, h = min / 60.0, d = h / 24.0, y = d / 365.0;
        if (y > 0.5) return fmt2(y) + " y";
        if (d > 0.5) return fmt2(d) + " d";
        if (h > 0.5) return fmt2(h) + " h";
        if (min > 0.5) return fmt2(min) + " min";
        std::string s = std::to_string(sec);  // Java double toString of ticks / 20
        while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
        return s + " s";
    }
    if (custom.starts_with("damage_")) return fmt2(v * 0.1);
    return formatDefault(v);
}

// --- lists ----------------------------------------------------------------------------------------------------

struct Row {
    Text left, right;  // general: name, value; mobs: name (with two lines below)
    Text kills, killedBy;
    bool hasKills = false, wasKilledBy = false;
    int item = -1;     // items tab
    std::string itemName;
    int cols[6] = {};
    bool hasBlock = false;
};
std::vector<Row> rows[3];

const char* const kItemCols[6] = {"mined", "broken", "crafted", "used", "picked_up", "dropped"};
const char* const kColSprites[6] = {"statistics/block_mined", "statistics/item_broken", "statistics/item_crafted",
                                    "statistics/item_used", "statistics/item_picked_up", "statistics/item_dropped"};

void buildRows(UiState& s) {
    for (auto& r : rows) r.clear();
    const auto& custom = g.registries["custom"];
    for (const std::string& c : custom) {
        if (c.empty()) continue;
        Row r;
        std::string key = "stat.minecraft." + c;
        r.left = Text(s.res.translate(key));
        r.right = Text(formatStat(c, value("custom", c)));
        r.itemName = c;
        rows[0].push_back(r);
    }
    std::stable_sort(rows[0].begin(), rows[0].end(), [](const Row& a, const Row& b) { return a.left.plain() < b.left.plain(); });
    for (int id = 0; id < s.items.count(); ++id) {
        const std::string n = s.items.name(id).substr(10);
        if (n == "air") continue;
        Row r;
        r.item = id, r.itemName = n;
        r.hasBlock = s.items.isBlockItem(id);
        bool any = false;
        for (int c = 0; c < 6; ++c) {
            r.cols[c] = c == 0 ? (r.hasBlock ? value("mined", n) : 0) : value(kItemCols[c], n);
            any |= r.cols[c] > 0;
        }
        if (any) rows[1].push_back(r);
    }
    for (const std::string& e : g.registries["killed"]) {
        if (e.empty()) continue;
        const int k = value("killed", e), kb = value("killed_by", e);
        if (k <= 0 && kb <= 0) continue;
        Row r;
        r.left = Text(s.res.translate("entity.minecraft." + e));
        r.hasKills = k > 0, r.wasKilledBy = kb > 0;
        r.kills = k > 0 ? translatable("stat_type.minecraft.killed", {Text(std::to_string(k)), r.left}, s.lang())
                        : translatable("stat_type.minecraft.killed.none", {r.left}, s.lang());
        r.killedBy = kb > 0 ? translatable("stat_type.minecraft.killed_by", {r.left, Text(std::to_string(kb))}, s.lang())
                            : translatable("stat_type.minecraft.killed_by.none", {r.left}, s.lang());
        rows[2].push_back(r);
    }
}

void sortItems() {  // ItemRowComparator
    std::stable_sort(rows[1].begin(), rows[1].end(), [](const Row& a, const Row& b) {
        if (g.sortColumn < 0) return a.item < b.item;
        const int ka = g.sortColumn == 0 && !a.hasBlock ? -1 : a.cols[g.sortColumn];
        const int kb = g.sortColumn == 0 && !b.hasBlock ? -1 : b.cols[g.sortColumn];
        if (ka == kb) return g.sortOrder * (a.item - b.item) < 0;
        return g.sortOrder * (ka - kb) < 0;
    });
}

int rowHeight(int tab) { return tab == 0 ? 14 : tab == 1 ? 22 : 36; }
int listTop() { return kTabBarH; }
int listBottom(const UiState& s) { return s.guiH() - kFooterH; }
int contentHeight(int tab) { return (int(rows[tab].size()) + (tab == 1 && !rows[1].empty() ? 1 : 0)) * rowHeight(tab) + 4; }
int maxScroll(const UiState& s, int tab) { return std::max(0, contentHeight(tab) - (listBottom(s) - listTop())); }

bool tabActive(int t) { return t == 0 ? g.received : g.received && !rows[t].empty(); }

// MenuTabBar geometry.
void tabGeometry(int W, int& x0, int& tabW) {
    const int tabsW = std::min(400, W) - 28;
    tabW = (tabsW / 3) / 2 * 2;
    x0 = ((W - tabsW) / 2) / 2 * 2;
}

void tile(Canvas& c, const Sprite* s, float x, float y, float w, float h, float tw, float th, u32 color = 0xFFFFFFFF) {
    if (!s || w <= 0 || h <= 0) return;
    c.pushScissor(x, y, x + w, y + h);
    for (float ty = y; ty < y + h; ty += th)
        for (float tx = x; tx < x + w; tx += tw) blitTexture(c, s, tx, ty, 0, 0, tw, th, int(tw), int(th), color);
    c.popScissor();
}

// --- packets ----------------------------------------------------------------------------------------------------

void handlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("award_stats", [sp](net::Reader& r) {
        std::vector<std::tuple<int, int, int>> v;
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            const int type = r.varInt(), id = r.varInt(), val = r.varInt();
            v.emplace_back(type, id, val);
        }
        if (!r.ok) return;
        sp->post([sp, v] {
            for (auto [t, id, val] : v) g.values[{t, id}] = val;
            g.received = true;
            if (sp->screen == Screen::Stats) {
                loadRegistries(*sp);
                buildRows(*sp);
                sortItems();
            }
        });
    });
}

void attached(UiState&) {
    g.values.clear();
    g.received = false;
}

const UiModuleRegistrar kModule({"stats", handlers, attached, nullptr, nullptr, nullptr, nullptr});

// --- screen -------------------------------------------------------------------------------------------------------

void close(UiState& s) {
    if (g.returnToPause) s.openScreen(Screen::Pause);
    else s.closeScreen(false);
}

void build(UiState& s) {
    loadRegistries(s);
    s.buttons.clear();
    Button done;
    done.w = 200, done.x = float(s.guiW() / 2 - 100), done.y = float(s.guiH() - kFooterH + 6);
    done.label = Text(s.res.translate("gui.done"));
    UiState* sp = &s;
    done.onPress = [sp] { close(*sp); };
    s.buttons.push_back(std::move(done));
}

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    const float mx = s.mouseX, my = s.mouseY;
    // Backgrounds: the tab header, the in-world menu background, the footer separator (the game blurs the world
    // under them; without blur the darkening gradient of the other in-game screens keeps the text readable).
    c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);
    tile(c, s.sprites.texture("gui/tab_header_background"), 0, 0, float(W), float(kTabBarH), 16, 16);
    tile(c, s.sprites.texture("gui/inworld_menu_background"), 0, float(kTabBarH), float(W), float(H - kTabBarH), 16, 16);
    tile(c, s.sprites.texture("gui/inworld_footer_separator"), 0, float(H - kFooterH), float(W), 2, 32, 2);
    // Tab bar.
    int x0, tabW;
    tabGeometry(W, x0, tabW);
    tile(c, s.sprites.texture("gui/header_separator"), 0, float(kTabBarH - 2), float(x0), 2, 32, 2);
    tile(c, s.sprites.texture("gui/header_separator"), float(x0 + 3 * tabW), float(kTabBarH - 2), float(W - x0 - 3 * tabW), 2, 32, 2);
    static const char* kTabs[3] = {"stat.generalButton", "stat.itemsButton", "stat.mobsButton"};
    int hoveredTab = -1;
    for (int t = 0; t < 3; ++t) {
        const float x = float(x0 + t * tabW);
        const bool sel = t == g.tab, hover = mx >= x && mx < x + float(tabW) && my >= 0 && my < float(kTabBarH);
        if (hover) hoveredTab = t;
        const char* sp = sel ? (hover ? "widget/tab_selected_highlighted" : "widget/tab_selected") : (hover ? "widget/tab_highlighted" : "widget/tab");
        blitSprite(c, s.sprites.sprite(sp), x, 0, float(tabW), float(kTabBarH));
        const Text label(s.res.translate(kTabs[t]));
        const bool active = tabActive(t);
        if (sel) {
            tile(c, s.sprites.texture("gui/menu_background"), x + 2, 2, float(tabW - 4), float(kTabBarH - 2), 16, 16);
            const int w = std::min(s.font.width(label), tabW - 4);
            c.fill(x + float((tabW - w) / 2), float(kTabBarH - 2), x + float((tabW - w) / 2 + w), float(kTabBarH - 1),
                   active ? 0xFFFFFFFF : 0xFFA0A0A0);
        }
        const int top = sel ? 0 : 3;
        s.drawCentered(c, label, x + float(tabW) / 2, float((top + kTabBarH - 9) / 2 + 1), active ? 0xFFFFFFFF : 0xFFA0A0A0);
    }
    // List.
    const int top = listTop(), bottom = listBottom(s);
    const int rowLeft = W / 2 - kRowW / 2;
    const int t = g.tab;
    g.scroll[t] = std::clamp(g.scroll[t], 0.0, double(maxScroll(s, t)));
    if (g.draggingBar && s.leftDown) {
        const int barH = std::clamp(int(float((bottom - top) * (bottom - top)) / float(contentHeight(t))), 32, bottom - top - 8);
        g.scroll[t] = std::clamp((my - float(top) - float(barH) / 2) / float(std::max(1, bottom - top - barH)) * float(maxScroll(s, t)), 0.f,
                                 float(maxScroll(s, t)));
    }
    if (!s.leftDown) g.draggingBar = false;
    Text tooltip;
    bool hasTooltip = false;
    c.pushScissor(0, float(top), float(W), float(bottom));
    if (!g.received) {
        s.drawCentered(c, Text(s.res.translate("multiplayer.downloadingStats")), float(W / 2), float((top + bottom) / 2 - 9), 0xFFFFFFFF);
        static const char* kDots[4] = {"O o o", "o O o", "o o O", "o O o"};
        s.drawCentered(c, Text(kDots[int(s.clock * 3) % 4]), float(W / 2), float((top + bottom) / 2 + 3), 0xFF808080);
    } else {
        int y = top + 2 - int(g.scroll[t]);
        const int rh = rowHeight(t);
        if (t == 1 && !rows[1].empty()) {  // header: sort buttons
            for (int col = 0; col < 6; ++col) {
                const float bx = float(rowLeft + 2 + 75 + 40 * col - 18), by = float(y + 2 + 1);
                const bool hover = mx >= bx && mx < bx + 18 && my >= by && my < by + 18 && my >= top && my < bottom;
                blitSprite(c, s.sprites.sprite("statistics/header"), bx, by, 18, 18);
                blitSprite(c, s.sprites.sprite(kColSprites[col]), bx, by, 18, 18);
                if (hover) tooltip = Text(s.res.translate(std::string("stat_type.minecraft.") + kItemCols[col])), hasTooltip = true;
            }
            if (g.sortColumn >= 0)
                blitSprite(c, s.sprites.sprite(g.sortOrder == 1 ? "statistics/sort_up" : "statistics/sort_down"),
                           float(rowLeft + 2 + 75 + 40 * g.sortColumn - 36), float(y + 2 + 1), 18, 18);
            y += rh;
        }
        for (size_t i = 0; i < rows[t].size(); ++i, y += rh) {
            if (y + rh < top || y > bottom) continue;
            const Row& r = rows[t][i];
            const int cx = rowLeft + 2, cy = y + 2, ch = rh - 4;
            if (t == 0) {
                const u32 color = i % 2 == 0 ? 0xFFFFFFFF : 0xFFBABABA;
                const int ty = cy + ch / 2 - 9 / 2;
                s.drawText(c, r.left, float(cx + 2), float(ty), color, true);
                s.drawText(c, r.right, float(cx + kRowW - 4 - s.font.width(r.right) - 4), float(ty), color, true);
            } else if (t == 1) {
                const int index = int(i) + 1;  // the header is entry 0
                blitSprite(c, s.sprites.sprite("container/slot"), float(cx), float(cy), 18, 18);
                ItemStack st;
                st.item = r.item, st.count = 1;
                s.drawItem(c, st, float(cx + 1), float(cy + 1), false);
                if (mx >= cx && mx < cx + 18 && my >= cy && my < cy + 18 && my >= top && my < bottom)
                    tooltip = s.items.hoverName(st), hasTooltip = true;
                for (int col = 0; col < 6; ++col) {
                    const Text v = col == 0 && !r.hasBlock ? Text(s.res.translate("stats.none")) : Text(formatDefault(r.cols[col]));
                    s.drawText(c, v, float(cx + 75 + 40 * col - s.font.width(v)), float(cy + ch / 2 - 9 / 2),
                               index % 2 == 0 ? 0xFFFFFFFF : 0xFFBABABA, true);
                }
            } else {
                s.drawText(c, r.left, float(cx + 2), float(cy + 1), 0xFFFFFFFF, true);
                s.drawText(c, r.kills, float(cx + 12), float(cy + 1 + 9), r.hasKills ? 0xFFBABABA : 0xFF808080, true);
                s.drawText(c, r.killedBy, float(cx + 12), float(cy + 1 + 18), r.wasKilledBy ? 0xFFBABABA : 0xFF808080, true);
            }
        }
    }
    c.popScissor();
    // Scroll bar (AbstractScrollArea).
    if (g.received && maxScroll(s, t) > 0) {
        const int barX = rowLeft + kRowW + 6 + 2, barH = std::clamp(int(float((bottom - top) * (bottom - top)) / float(contentHeight(t))), 32, bottom - top - 8);
        const int barY = std::max(top, int(g.scroll[t]) * (bottom - top - barH) / maxScroll(s, t) + top);
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), float(barX), float(top), 6, float(bottom - top));
        blitSprite(c, s.sprites.sprite("widget/scroller"), float(barX), float(barY), 6, float(barH));
    }
    s.drawButtons(c);
    if (hoveredTab >= 0 && g.received && !tabActive(hoveredTab)) tooltip = Text(s.res.translate("gui.stats.none_found")), hasTooltip = true;
    if (hasTooltip) s.drawTooltip(c, {tooltip}, mx, my);
}

void input(UiState& s, const InputEvent& e) {
    if (e.type == InputEvent::Key && e.down && e.code == VK_ESCAPE) return close(s);
    if (s.buttonsInput(e)) return;
    const int W = s.guiW(), top = listTop(), bottom = listBottom(s);
    if (e.type == InputEvent::Wheel) {
        g.scroll[g.tab] = std::clamp(g.scroll[g.tab] - e.code * rowHeight(g.tab) / 2.0, 0.0, double(maxScroll(s, g.tab)));
        return;
    }
    if (e.type != InputEvent::Button || e.code != 0 || !e.down) return;
    const float mx = s.mouseX, my = s.mouseY;
    int x0, tabW;
    tabGeometry(W, x0, tabW);
    if (my >= 0 && my < kTabBarH)
        for (int t = 0; t < 3; ++t)
            if (mx >= float(x0 + t * tabW) && mx < float(x0 + (t + 1) * tabW) && tabActive(t) && t != g.tab) {
                audio::playUiClick();
                g.tab = t;
                return;
            }
    const int rowLeft = W / 2 - kRowW / 2, barX = rowLeft + kRowW + 8;
    if (mx >= barX && mx <= barX + 6 && my >= top && my < bottom && maxScroll(s, g.tab) > 0) {
        g.draggingBar = true;
        return;
    }
    if (g.tab == 1 && !rows[1].empty()) {  // sort buttons in the header row
        const int y = top + 2 - int(g.scroll[1]) + 3;
        for (int col = 0; col < 6; ++col) {
            const float bx = float(rowLeft + 2 + 75 + 40 * col - 18);
            if (mx >= bx && mx < bx + 18 && my >= float(y) && my < float(y + 18) && my >= top && my < bottom) {
                audio::playUiClick();
                if (col != g.sortColumn) g.sortColumn = col, g.sortOrder = -1;
                else if (g.sortOrder == -1) g.sortOrder = 1;
                else g.sortColumn = -1, g.sortOrder = 0;
                sortItems();
                return;
            }
        }
    }
}

void closed(UiState&) { g.draggingBar = false; }

const ScreenRegistrar kScreen({Screen::Stats, build, draw, input, closed});
}  // namespace

// The pause menu's Statistics button (screens.cpp): requests the statistics and opens the screen.
void openStatsFromPause(UiState& s) {
    loadRegistries(s);
    g.tab = 0;
    g.received = false;
    g.scroll[0] = g.scroll[1] = g.scroll[2] = 0;
    g.returnToPause = true;
    s.openScreen(Screen::Stats);
    if (s.online()) {
        net::Writer w;
        w.varInt(1);  // ServerboundClientCommandPacket.Action.REQUEST_STATS
        s.client->sendPlay("client_command", w);
    }
}

}  // namespace ui
